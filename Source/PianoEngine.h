#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

namespace grandlite
{

// One decoded sample, stored as 16-bit PCM to halve memory use.
struct Sample
{
    std::vector<int16_t> data; // interleaved if channels == 2
    int channels = 2;
    int numFrames = 0;
    double sampleRate = 44100.0;
};

struct Zone
{
    const Sample* sample = nullptr;
    int root = 60; // midi note the sample was recorded at
};

// Immutable once published to the engine.
struct SampleBank
{
    std::vector<std::unique_ptr<Sample>> samples;
    int numLayers = 0;
    std::vector<Zone> zones;          // [128 * numLayers], index = note * numLayers + layer
    std::array<Zone, 128> release {}; // key-off noise per note
    bool hasRelease = false;

    const Zone& zone (int note, int layer) const { return zones[(size_t) (note * numLayers + layer)]; }
};

// Builds a bank from a folder. Naming: C4v8.wav / F#3v2.flac / Ds3v5.wav (sustain, vN = velocity layer),
// rel12.wav (key-off noise, number = key index counted from A0 = 1).
// Runs on a background thread; returns nullptr if cancelled or nothing usable was found.
std::unique_ptr<SampleBank> buildBankFromFolder (const juce::File& folder,
                                                 std::atomic<int>& filesDone,
                                                 std::atomic<int>& filesTotal,
                                                 const std::atomic<bool>& cancel,
                                                 juce::String& summaryOut);

struct EngineParams
{
    float damperSeconds = 0.25f;   // time for a released key to fade to silence (-60 dB)
    float releaseNoise = 0.5f;     // level of key-off samples
    float velocitySens = 0.7f;     // dynamics within a velocity layer
};

class PianoEngine
{
public:
    static constexpr int maxVoices = 64;

    void prepare (double hostSampleRate);
    void setParams (const EngineParams& p) { params = p; }

    // Message/loader thread: hand over a new bank. Audio thread picks it up at the next block.
    void publishBank (std::shared_ptr<SampleBank> newBank);
    // Message thread: frees the previous bank (never free memory on the audio thread).
    void collectGarbage();
    bool hasBank() const { return bankReady.load(); }

    // Audio thread
    void adoptPendingBank();
    void noteOn (int note, int velocity);
    void noteOff (int note);
    void setSustain (bool down);
    void allNotesOff();
    // Adds into outL/outR (caller zeroes them first).
    void render (float* outL, float* outR, int numSamples);

private:
    struct Voice
    {
        const Sample* s = nullptr;
        double pos = 0.0, ratio = 1.0;
        float gain = 1.0f, env = 1.0f, relCoeff = 1.0f, fade = 0.0f;
        int note = 0;
        uint64_t age = 0;
        bool active = false, keyDown = false, sustained = false, releasing = false, isReleaseSample = false;
        double heldSeconds = 0.0;
    };

    void startRelease (Voice& v);
    Voice& allocateVoice();
    void renderVoice (Voice& v, float* L, float* R, int n);

    std::array<Voice, maxVoices> voices {};
    std::array<float, 128> noteVelocity {};
    std::array<double, 128> noteOnTime {};
    uint64_t counter = 0;
    double sr = 44100.0, clock = 0.0;
    float fadeInc = 0.01f;
    bool pedalDown = false;
    EngineParams params;

    juce::SpinLock lock;
    std::shared_ptr<SampleBank> pending, retired, currentOwner;
    const SampleBank* current = nullptr;
    std::atomic<bool> hasPending { false }, bankReady { false }, needsGc { false };
};

} // namespace grandlite
