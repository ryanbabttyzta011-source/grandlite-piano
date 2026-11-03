#include "PianoEngine.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

namespace grandlite
{

// ---------------------------------------------------------------- bank loading

namespace
{
struct ParsedName
{
    bool valid = false, isRelease = false;
    int midi = 60, layer = 1, relIndex = 0;
};

ParsedName parseName (const juce::String& stem)
{
    ParsedName p;
    const auto s = stem.trim();

    if (s.startsWithIgnoreCase ("rel") && s.length() > 3 && s.substring (3).containsOnly ("0123456789"))
    {
        p.valid = p.isRelease = true;
        p.relIndex = s.substring (3).getIntValue();
        p.midi = 20 + p.relIndex; // rel1 = A0 (midi 21)
        return p;
    }

    int i = 0;
    const auto c = juce::CharacterFunctions::toUpperCase (s[i++]);
    static const int base[7] = { 9, 11, 0, 2, 4, 5, 7 }; // A B C D E F G
    if (c < 'A' || c > 'G') return p;
    int semi = base[c - 'A'];

    const auto a = s[i];
    if (a == '#' || a == 's' || a == 'S') { semi += 1; ++i; }
    else if (a == 'b') { semi -= 1; ++i; }

    const int octStart = i;
    if (s[i] == '-') ++i;
    while (juce::CharacterFunctions::isDigit (s[i])) ++i;
    if (i == octStart) return p;
    const int oct = s.substring (octStart, i).getIntValue();

    p.layer = 1;
    if (s[i] == 'v' || s[i] == 'V')
    {
        const auto rest = s.substring (i + 1);
        if (rest.isEmpty() || ! rest.containsOnly ("0123456789")) return p;
        p.layer = rest.getIntValue();
    }
    else if (i < s.length())
        return p; // some other kind of file (harmonics, pedal noise...) - ignore

    p.midi = (oct + 1) * 12 + semi;
    p.valid = p.midi >= 0 && p.midi < 128;
    return p;
}

std::unique_ptr<Sample> loadSample (juce::AudioFormatManager& fm, const juce::File& f, double maxSeconds)
{
    std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (f));
    if (r == nullptr || r->lengthInSamples <= 0) return nullptr;

    const int frames = (int) std::min<juce::int64> (r->lengthInSamples, (juce::int64) (maxSeconds * r->sampleRate));
    const int ch = r->numChannels >= 2 ? 2 : 1;

    juce::AudioBuffer<float> buf (ch, frames);
    if (! r->read (&buf, 0, frames, 0, true, ch == 2)) return nullptr;

    // Fade the tail so truncated samples never click.
    const int fadeLen = std::min (frames, (int) (0.05 * r->sampleRate));
    buf.applyGainRamp (0, frames - fadeLen, fadeLen, 1.0f, 0.0f);

    auto s = std::make_unique<Sample>();
    s->channels = ch;
    s->numFrames = frames;
    s->sampleRate = r->sampleRate;
    s->data.resize ((size_t) frames * (size_t) ch);
    for (int c = 0; c < ch; ++c)
    {
        const float* src = buf.getReadPointer (c);
        for (int i = 0; i < frames; ++i)
            s->data[(size_t) i * (size_t) ch + (size_t) c] = (int16_t) juce::jlimit (-32767, 32767, juce::roundToInt (src[i] * 32767.0f));
    }
    return s;
}

const Sample* nearestRoot (const std::map<int, const Sample*>& roots, int note, int& rootOut)
{
    if (roots.empty()) return nullptr;
    auto it = roots.lower_bound (note);
    if (it == roots.begin()) { rootOut = it->first; return it->second; }
    if (it == roots.end()) { --it; rootOut = it->first; return it->second; }
    auto hi = it;
    auto lo = std::prev (it);
    // tie goes to the lower root (pitching down sounds darker/more natural than pitching up)
    const auto& pick = (note - lo->first <= hi->first - note) ? *lo : *hi;
    rootOut = pick.first;
    return pick.second;
}
} // namespace

std::unique_ptr<SampleBank> buildBankFromFolder (const juce::File& folder,
                                                 std::atomic<int>& filesDone,
                                                 std::atomic<int>& filesTotal,
                                                 const std::atomic<bool>& cancel,
                                                 juce::String& summaryOut)
{
    constexpr double maxSeconds = 10.0; // memory cap per sample

    juce::AudioFormatManager fm;
    fm.registerBasicFormats();

    struct Item { juce::File file; ParsedName name; };
    std::vector<Item> items;
    std::set<int> layerIds;

    for (const auto& f : folder.findChildFiles (juce::File::findFiles, true))
    {
        if (! fm.findFormatForFileExtension (f.getFileExtension()))
            continue;
        const auto p = parseName (f.getFileNameWithoutExtension());
        if (! p.valid) continue;
        items.push_back ({ f, p });
        if (! p.isRelease) layerIds.insert (p.layer);
    }

    filesDone = 0;
    filesTotal = (int) items.size();
    if (layerIds.empty())
    {
        summaryOut = "No samples found. Expected names like C4v8.wav, F#3v2.flac, Ds3v5.wav";
        return nullptr;
    }

    auto bank = std::make_unique<SampleBank>();
    bank->numLayers = (int) layerIds.size();

    std::map<int, int> layerIndex;
    for (int id : layerIds) { const int idx = (int) layerIndex.size(); layerIndex[id] = idx; }

    std::vector<std::map<int, const Sample*>> roots ((size_t) bank->numLayers);
    std::map<int, const Sample*> releaseRoots;
    int sustainCount = 0;

    for (auto& it : items)
    {
        if (cancel.load()) return nullptr;
        auto s = loadSample (fm, it.file, it.name.isRelease ? 3.0 : maxSeconds);
        ++filesDone;
        if (s == nullptr) continue;
        const Sample* raw = s.get();
        bank->samples.push_back (std::move (s));
        if (it.name.isRelease) releaseRoots[it.name.midi] = raw;
        else { roots[(size_t) layerIndex[it.name.layer]][it.name.midi] = raw; ++sustainCount; }
    }

    // Layers that failed to load entirely borrow from their nearest populated neighbour.
    for (int l = 0; l < bank->numLayers; ++l)
    {
        if (! roots[(size_t) l].empty()) continue;
        for (int d = 1; d < bank->numLayers; ++d)
        {
            const int a = l - d, b = l + d;
            if (a >= 0 && ! roots[(size_t) a].empty()) { roots[(size_t) l] = roots[(size_t) a]; break; }
            if (b < bank->numLayers && ! roots[(size_t) b].empty()) { roots[(size_t) l] = roots[(size_t) b]; break; }
        }
    }

    if (sustainCount == 0)
    {
        summaryOut = "Sample files were found but none could be decoded";
        return nullptr;
    }

    bank->zones.resize ((size_t) (128 * bank->numLayers));
    for (int n = 0; n < 128; ++n)
        for (int l = 0; l < bank->numLayers; ++l)
        {
            int root = n;
            const Sample* s = nearestRoot (roots[(size_t) l], n, root);
            bank->zones[(size_t) (n * bank->numLayers + l)] = { s, root };
        }

    for (int n = 0; n < 128; ++n)
    {
        int root = n;
        const Sample* s = nearestRoot (releaseRoots, n, root);
        bank->release[(size_t) n] = { s, root };
    }
    bank->hasRelease = ! releaseRoots.empty();

    summaryOut = juce::String (sustainCount) + " samples, " + juce::String (bank->numLayers) + " velocity layers"
               + (bank->hasRelease ? ", " + juce::String ((int) releaseRoots.size()) + " release samples" : ", no release samples");
    return bank;
}

// ---------------------------------------------------------------- engine

void PianoEngine::prepare (double hostSampleRate)
{
    sr = hostSampleRate;
    fadeInc = (float) (1.0 / (0.003 * sr)); // 3 ms declick on note start
    allNotesOff();
}

void PianoEngine::publishBank (std::shared_ptr<SampleBank> newBank)
{
    const juce::SpinLock::ScopedLockType sl (lock);
    pending = std::move (newBank);
    hasPending = true;
}

void PianoEngine::collectGarbage()
{
    std::shared_ptr<SampleBank> dead;
    {
        const juce::SpinLock::ScopedLockType sl (lock);
        dead = std::move (retired);
        retired.reset();
    }
    // dead is destroyed here, on the message thread
}

void PianoEngine::adoptPendingBank()
{
    if (! hasPending.load()) return;
    const juce::SpinLock::ScopedTryLockType sl (lock);
    if (! sl.isLocked() || retired != nullptr) return; // wait until the message thread has freed the old bank

    for (auto& v : voices) v.active = false;
    retired = std::move (currentOwner);
    currentOwner = std::move (pending);
    pending.reset();
    current = currentOwner.get();
    hasPending = false;
    bankReady = current != nullptr;
}

void PianoEngine::allNotesOff()
{
    for (auto& v : voices) v.active = false;
    pedalDown = false;
}

PianoEngine::Voice& PianoEngine::allocateVoice()
{
    Voice* best = nullptr;
    for (auto& v : voices)
        if (! v.active) return v;

    // Prefer stealing the oldest releasing voice, then the oldest voice overall.
    for (auto& v : voices)
        if (v.releasing && (best == nullptr || v.age < best->age)) best = &v;
    if (best == nullptr)
        for (auto& v : voices)
            if (best == nullptr || v.age < best->age) best = &v;
    return *best;
}

void PianoEngine::noteOn (int note, int velocity)
{
    if (current == nullptr || note < 0 || note > 127 || velocity <= 0) return;

    // Re-striking a key damps the previous instance of that note.
    for (auto& v : voices)
        if (v.active && v.note == note && ! v.isReleaseSample && ! v.releasing)
            startRelease (v);

    const int layers = current->numLayers;
    const int layer = std::min (layers - 1, velocity * layers / 128);
    const Zone& z = current->zone (note, layer);
    if (z.sample == nullptr) return;

    const float vel = (float) velocity / 127.0f;
    const float centre = ((float) layer + 0.5f) / (float) layers;
    const float g = juce::jlimit (0.4f, 1.6f, std::pow (vel / centre, params.velocitySens));

    auto& v = allocateVoice();
    v = Voice();
    v.s = z.sample;
    v.pos = 0.0;
    v.ratio = std::pow (2.0, (double) (note - z.root) / 12.0) * z.sample->sampleRate / sr;
    v.gain = g * 0.7f;
    v.note = note;
    v.age = ++counter;
    v.active = v.keyDown = true;
    noteVelocity[(size_t) note] = vel;
    noteOnTime[(size_t) note] = clock;
}

void PianoEngine::startRelease (Voice& v)
{
    v.releasing = true;
    v.relCoeff = std::pow (0.001f, 1.0f / std::max (1.0f, (float) (params.damperSeconds * sr)));
}

void PianoEngine::noteOff (int note)
{
    if (current == nullptr || note < 0 || note > 127) return;

    bool wasPlaying = false;
    for (auto& v : voices)
    {
        if (! v.active || v.note != note || v.isReleaseSample || ! v.keyDown) continue;
        v.keyDown = false;
        wasPlaying = true;
        if (pedalDown) v.sustained = true;
        else startRelease (v);
    }

    // Key-off thump (damper/action noise) - only when the damper actually lands.
    if (wasPlaying && ! pedalDown && current->hasRelease && params.releaseNoise > 0.001f)
    {
        const Zone& z = current->release[(size_t) note];
        if (z.sample != nullptr)
        {
            const double held = clock - noteOnTime[(size_t) note];
            const float amount = (float) juce::jlimit (0.15, 1.0, held / 0.4);
            auto& v = allocateVoice();
            v = Voice();
            v.s = z.sample;
            v.ratio = std::pow (2.0, (double) (note - z.root) / 12.0) * z.sample->sampleRate / sr;
            v.gain = params.releaseNoise * 0.5f * amount * (0.4f + 0.6f * noteVelocity[(size_t) note]);
            v.note = note;
            v.age = ++counter;
            v.active = v.isReleaseSample = true;
            v.keyDown = false;
        }
    }
}

void PianoEngine::setSustain (bool down)
{
    if (down == pedalDown) return;
    pedalDown = down;
    if (down) return;
    for (auto& v : voices)
        if (v.active && v.sustained && ! v.keyDown && ! v.isReleaseSample)
        {
            v.sustained = false;
            startRelease (v);
        }
}

void PianoEngine::renderVoice (Voice& v, float* L, float* R, int n)
{
    const Sample& s = *v.s;
    const int16_t* d = s.data.data();
    const int last = s.numFrames - 2;
    const bool stereo = s.channels == 2;
    constexpr float k = 1.0f / 32768.0f;

    double pos = v.pos;
    float env = v.env, fade = v.fade;
    const float rc = v.releasing ? v.relCoeff : 1.0f;

    for (int i = 0; i < n; ++i)
    {
        const int idx = (int) pos;
        if (idx >= last || env < 0.0003f) { v.active = false; return; }
        const float fr = (float) (pos - idx);
        const float amp = v.gain * env * fade * k;

        float l, r;
        if (stereo)
        {
            const int16_t* p = d + (size_t) idx * 2;
            l = (float) p[0] + fr * (float) (p[2] - p[0]);
            r = (float) p[1] + fr * (float) (p[3] - p[1]);
        }
        else
        {
            l = r = (float) d[idx] + fr * (float) (d[idx + 1] - d[idx]);
        }
        L[i] += l * amp;
        R[i] += r * amp;

        pos += v.ratio;
        env *= rc;
        fade = std::min (1.0f, fade + fadeInc);
    }
    v.pos = pos; v.env = env; v.fade = fade;
}

void PianoEngine::render (float* L, float* R, int n)
{
    if (current == nullptr) return;
    for (auto& v : voices)
    {
        if (! v.active) continue;
        renderVoice (v, L, R, n);
        if (v.active) v.heldSeconds += (double) n / sr;
    }
    clock += (double) n / sr;
}

} // namespace grandlite
