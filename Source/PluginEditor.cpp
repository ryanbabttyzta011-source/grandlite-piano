#include "PluginEditor.h"

GrandLiteEditor::GrandLiteEditor (GrandLiteProcessor& p)
    : AudioProcessorEditor (&p), proc (p), progressBar (progress)
{
    addKnob (volume, "volume", "Volume");
    addKnob (tone, "tone", "Tone");
    addKnob (reverb, "reverbMix", "Reverb");
    addKnob (room, "reverbSize", "Room");
    addKnob (keyNoise, "releaseNoise", "Key Noise");
    addKnob (dynamics, "velSens", "Dynamics");
    addKnob (damper, "damper", "Damper");

    addAndMakeVisible (loadButton);
    loadButton.onClick = [this]
    {
        chooser = std::make_unique<juce::FileChooser> ("Select the folder containing your piano samples",
                                                       juce::File (proc.getSamplePath()));
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                              [this] (const juce::FileChooser& fc)
                              {
                                  const auto f = fc.getResult();
                                  if (f.isDirectory()) proc.loadSamplesFrom (f);
                              });
    };

    statusLabel.setJustificationType (juce::Justification::centredLeft);
    pathLabel.setJustificationType (juce::Justification::centredLeft);
    pathLabel.setColour (juce::Label::textColourId, juce::Colours::grey);
    addAndMakeVisible (statusLabel);
    addAndMakeVisible (pathLabel);
    addAndMakeVisible (progressBar);

    setSize (620, 330);
    startTimerHz (10);
    timerCallback();
}

GrandLiteEditor::~GrandLiteEditor() { stopTimer(); }

void GrandLiteEditor::addKnob (Knob& k, const juce::String& id, const juce::String& text)
{
    addAndMakeVisible (k.slider);
    k.label.setText (text, juce::dontSendNotification);
    k.label.setJustificationType (juce::Justification::centred);
    k.label.attachToComponent (&k.slider, false);
    addAndMakeVisible (k.label);
    k.attach = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (proc.apvts, id, k.slider);
}

void GrandLiteEditor::timerCallback()
{
    statusLabel.setText (proc.getStatus(), juce::dontSendNotification);
    pathLabel.setText (proc.getSamplePath(), juce::dontSendNotification);
    progress = (double) proc.getLoadProgress();
    progressBar.setVisible (progress < 1.0);
}

void GrandLiteEditor::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff15171a));
    g.setColour (juce::Colours::white);
    g.setFont (juce::FontOptions (22.0f, juce::Font::bold));
    g.drawText ("GrandLite Piano", 20, 10, 300, 30, juce::Justification::centredLeft);
}

void GrandLiteEditor::resized()
{
    auto area = getLocalBounds().reduced (20);
    area.removeFromTop (34);

    auto top = area.removeFromTop (36);
    loadButton.setBounds (top.removeFromLeft (200));
    top.removeFromLeft (10);
    statusLabel.setBounds (top);
    pathLabel.setBounds (area.removeFromTop (20));
    progressBar.setBounds (area.removeFromTop (14).reduced (0, 2));

    area.removeFromTop (28); // room for knob labels
    Knob* knobs[] = { &volume, &tone, &reverb, &room, &keyNoise, &dynamics, &damper };
    const int w = area.getWidth() / 7;
    for (auto* k : knobs)
        k->slider.setBounds (area.removeFromLeft (w).reduced (6, 0).removeFromTop (110));
}
