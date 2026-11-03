#pragma once

#include "PluginProcessor.h"

class GrandLiteEditor : public juce::AudioProcessorEditor,
                        private juce::Timer
{
public:
    explicit GrandLiteEditor (GrandLiteProcessor&);
    ~GrandLiteEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;

    struct Knob
    {
        juce::Slider slider { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow };
        juce::Label label;
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attach;
    };

    void addKnob (Knob& k, const juce::String& id, const juce::String& text);

    GrandLiteProcessor& proc;
    Knob volume, tone, reverb, room, keyNoise, dynamics, damper;
    juce::TextButton loadButton { "Choose sample folder..." };
    juce::Label statusLabel, pathLabel;
    juce::ProgressBar progressBar;
    double progress = 0.0;
    std::unique_ptr<juce::FileChooser> chooser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GrandLiteEditor)
};
