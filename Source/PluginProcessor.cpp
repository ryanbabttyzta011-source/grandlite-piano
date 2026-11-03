#include "PluginProcessor.h"
#include "PluginEditor.h"

// Background thread that decodes the sample folder, then hands the finished bank to the engine.
class GrandLiteProcessor::Loader : public juce::Thread
{
public:
    Loader (GrandLiteProcessor& o, juce::File f) : juce::Thread ("GrandLite loader"), owner (o), folder (std::move (f)) {}
    ~Loader() override { cancel = true; stopThread (10000); }

    void run() override
    {
        juce::String summary;
        auto bank = grandlite::buildBankFromFolder (folder, done, total, cancel, summary);
        if (cancel.load() || threadShouldExit()) return;

        const bool ok = bank != nullptr;
        if (ok) owner.engine.publishBank (std::shared_ptr<grandlite::SampleBank> (std::move (bank)));

        juce::MessageManager::callAsync ([&o = owner, summary, ok]
        {
            const juce::ScopedLock sl (o.statusLock);
            o.status = ok ? "Loaded: " + summary : summary;
        });
        finished = true;
    }

    std::atomic<int> done { 0 }, total { 0 };
    std::atomic<bool> cancel { false }, finished { false };

private:
    GrandLiteProcessor& owner;
    juce::File folder;
};

juce::AudioProcessorValueTreeState::ParameterLayout GrandLiteProcessor::createLayout()
{
    using namespace juce;
    AudioProcessorValueTreeState::ParameterLayout l;
    l.add (std::make_unique<AudioParameterFloat> (ParameterID { "volume", 1 }, "Volume", NormalisableRange<float> (-24.0f, 6.0f, 0.1f), 0.0f));
    l.add (std::make_unique<AudioParameterFloat> (ParameterID { "tone", 1 }, "Tone", NormalisableRange<float> (0.0f, 1.0f, 0.001f), 0.8f));
    l.add (std::make_unique<AudioParameterFloat> (ParameterID { "reverbMix", 1 }, "Reverb", NormalisableRange<float> (0.0f, 1.0f, 0.001f), 0.15f));
    l.add (std::make_unique<AudioParameterFloat> (ParameterID { "reverbSize", 1 }, "Room", NormalisableRange<float> (0.0f, 1.0f, 0.001f), 0.5f));
    l.add (std::make_unique<AudioParameterFloat> (ParameterID { "releaseNoise", 1 }, "Key Noise", NormalisableRange<float> (0.0f, 1.0f, 0.001f), 0.5f));
    l.add (std::make_unique<AudioParameterFloat> (ParameterID { "velSens", 1 }, "Dynamics", NormalisableRange<float> (0.0f, 1.5f, 0.01f), 0.7f));
    l.add (std::make_unique<AudioParameterFloat> (ParameterID { "damper", 1 }, "Damper", NormalisableRange<float> (0.05f, 1.0f, 0.01f, 0.5f), 0.25f));
    return l;
}

GrandLiteProcessor::GrandLiteProcessor()
    : AudioProcessor (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "STATE", createLayout())
{
    pVolume = apvts.getRawParameterValue ("volume");
    pTone = apvts.getRawParameterValue ("tone");
    pReverbMix = apvts.getRawParameterValue ("reverbMix");
    pReverbSize = apvts.getRawParameterValue ("reverbSize");
    pReleaseNoise = apvts.getRawParameterValue ("releaseNoise");
    pVelSens = apvts.getRawParameterValue ("velSens");
    pDamper = apvts.getRawParameterValue ("damper");
    startTimer (250); // frees retired sample banks on the message thread
}

GrandLiteProcessor::~GrandLiteProcessor()
{
    stopTimer();
    loader.reset();
}

void GrandLiteProcessor::timerCallback()
{
    engine.collectGarbage();
}

bool GrandLiteProcessor::isBusesLayoutSupported (const BusesLayout& l) const
{
    return l.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
}

void GrandLiteProcessor::prepareToPlay (double sr, int)
{
    sampleRate = sr;
    engine.prepare (sr);
    reverb.setSampleRate (sr);
    reverb.reset();
    volume.reset (sr, 0.03);
    toneCoeff.reset (sr, 0.03);
    volume.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (pVolume->load()));
    lpL = lpR = 0.0f;
}

static float toneToCoeff (float tone, double sr)
{
    // 1.5 kHz .. 20 kHz, exponential. One-pole low-pass coefficient.
    const double fc = 1500.0 * std::pow (20000.0 / 1500.0, (double) tone);
    return (float) (1.0 - std::exp (-2.0 * juce::MathConstants<double>::pi * std::min (fc, sr * 0.45) / sr));
}

// Transparent below 0.8, smoothly saturates above so dense chords can't hard-clip the host.
static inline float softLimit (float x)
{
    const float a = std::abs (x);
    if (a <= 0.8f) return x;
    const float o = a - 0.8f;
    return std::copysign (0.8f + 0.2f * (o / (0.2f + o)), x);
}

void GrandLiteProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    const int n = buffer.getNumSamples();
    buffer.clear();

    engine.adoptPendingBank();

    grandlite::EngineParams ep;
    ep.damperSeconds = pDamper->load();
    ep.releaseNoise = pReleaseNoise->load();
    ep.velocitySens = pVelSens->load();
    engine.setParams (ep);

    float* L = buffer.getWritePointer (0);
    float* R = buffer.getWritePointer (1);

    // Render in segments so MIDI events land on the right sample.
    int pos = 0;
    for (const auto meta : midi)
    {
        const int at = juce::jlimit (pos, n, meta.samplePosition);
        if (at > pos) { engine.render (L + pos, R + pos, at - pos); pos = at; }

        const auto m = meta.getMessage();
        if (m.isNoteOn()) engine.noteOn (m.getNoteNumber(), m.getVelocity());
        else if (m.isNoteOff()) engine.noteOff (m.getNoteNumber());
        else if (m.isController() && m.getControllerNumber() == 64) engine.setSustain (m.getControllerValue() >= 64);
        else if (m.isAllNotesOff() || m.isAllSoundOff()) engine.allNotesOff();
    }
    if (pos < n) engine.render (L + pos, R + pos, n - pos);

    // Tone (one-pole low-pass) + volume.
    volume.setTargetValue (juce::Decibels::decibelsToGain (pVolume->load()));
    toneCoeff.setTargetValue (toneToCoeff (pTone->load(), sampleRate));
    for (int i = 0; i < n; ++i)
    {
        const float c = toneCoeff.getNextValue();
        const float g = volume.getNextValue();
        lpL += c * (L[i] - lpL);
        lpR += c * (R[i] - lpR);
        L[i] = softLimit (lpL * g);
        R[i] = softLimit (lpR * g);
    }

    // Reverb only costs CPU when it is audible.
    const float mix = pReverbMix->load();
    if (mix > 0.005f)
    {
        reverbParams.roomSize = 0.3f + 0.65f * pReverbSize->load();
        reverbParams.damping = 0.5f;
        reverbParams.wetLevel = mix * 0.4f;
        reverbParams.dryLevel = 1.0f - mix * 0.3f;
        reverbParams.width = 1.0f;
        reverb.setParameters (reverbParams);
        reverb.processStereo (L, R, n);
    }
}

void GrandLiteProcessor::loadSamplesFrom (const juce::File& folder)
{
    loader.reset(); // cancels any load in progress
    samplePath = folder.getFullPathName();
    {
        const juce::ScopedLock sl (statusLock);
        status = "Loading...";
    }
    loader = std::make_unique<Loader> (*this, folder);
    loader->startThread (juce::Thread::Priority::low);
}

juce::String GrandLiteProcessor::getStatus() const
{
    const juce::ScopedLock sl (statusLock);
    return status;
}

float GrandLiteProcessor::getLoadProgress() const
{
    if (loader == nullptr || loader->finished.load()) return 1.0f;
    const int t = loader->total.load();
    return t > 0 ? (float) loader->done.load() / (float) t : 0.0f;
}

void GrandLiteProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    auto state = apvts.copyState();
    state.setProperty ("samplePath", samplePath, nullptr);
    if (auto xml = state.createXml()) copyXmlToBinary (*xml, dest);
}

void GrandLiteProcessor::setStateInformation (const void* data, int size)
{
    if (auto xml = getXmlFromBinary (data, size))
    {
        auto state = juce::ValueTree::fromXml (*xml);
        if (! state.isValid()) return;
        const auto path = state.getProperty ("samplePath").toString();
        apvts.replaceState (state);
        if (path.isNotEmpty() && path != samplePath)
        {
            juce::File f (path);
            if (f.isDirectory())
                juce::MessageManager::callAsync ([this, f] { loadSamplesFrom (f); });
        }
    }
}

juce::AudioProcessorEditor* GrandLiteProcessor::createEditor() { return new GrandLiteEditor (*this); }

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new GrandLiteProcessor(); }
