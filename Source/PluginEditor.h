// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "PitchView.h"
#include "PluginProcessor.h"

#include <array>
#include <memory>

/** Thin-arc knobs with a pointer, and dark pop-up menus. */
class TuneAFishLookAndFeel final : public juce::LookAndFeel_V4
{
public:
    TuneAFishLookAndFeel();

    void drawRotarySlider (juce::Graphics&, int x, int y, int width, int height, float sliderPos,
                           float startAngle, float endAngle, juce::Slider&) override;
};

class TuneAFishEditor final : public juce::AudioProcessorEditor,
                              private juce::Timer
{
public:
    explicit TuneAFishEditor (TuneAFishProcessor&);
    ~TuneAFishEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;
    void paintReadout (juce::Graphics&);

    struct Choice
    {
        juce::ComboBox box;
        std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> attachment;
        juce::String label;
    };

    struct Knob
    {
        juce::Slider slider;
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attachment;
        juce::String label;
    };

    enum ChoiceIndex { modeChoice, keyChoice, scaleChoice, voiceChoice, numChoices };
    enum KnobIndex { retuneKnob, amountKnob, numKnobs };

    TuneAFishLookAndFeel lookAndFeel; // declared first so it outlives the components using it
    PitchView pitchView;
    std::array<Choice, numChoices> choices;
    std::array<Knob, numKnobs> knobs;
    juce::Rectangle<int> titleArea, readoutArea, controlsArea;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TuneAFishEditor)
};
