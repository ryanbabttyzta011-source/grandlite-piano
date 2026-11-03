#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_events/juce_events.h>

#include "PianoEngine.h"

class GrandLiteProcessor : public juce::AudioProcessor,
                           private juce::Timer
{
public:
    GrandLiteProcessor();
    ~GrandLiteProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "GrandLite Piano"; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 4.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    // Sample loading (message thread)
    void loadSamplesFrom (const juce::File& folder);
    juce::String getStatus() const;
    float getLoadProgress() const;
    juce::String getSamplePath() const { return samplePath; }

    juce::AudioProcessorValueTreeState apvts;

private:
    class Loader;
    void timerCallback() override;
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();

    grandlite::PianoEngine engine;
    juce::Reverb reverb;
    juce::Reverb::Parameters reverbParams;

    juce::SmoothedValue<float> volume, toneCoeff;
    float lpL = 0.0f, lpR = 0.0f;
    double sampleRate = 44100.0;

    std::unique_ptr<Loader> loader;
    juce::String samplePath;
    mutable juce::CriticalSection statusLock;
    juce::String status { "No samples loaded - choose your piano sample folder" };

    std::atomic<float>* pVolume = nullptr;
    std::atomic<float>* pTone = nullptr;
    std::atomic<float>* pReverbMix = nullptr;
    std::atomic<float>* pReverbSize = nullptr;
    std::atomic<float>* pReleaseNoise = nullptr;
    std::atomic<float>* pVelSens = nullptr;
    std::atomic<float>* pDamper = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GrandLiteProcessor)
};
