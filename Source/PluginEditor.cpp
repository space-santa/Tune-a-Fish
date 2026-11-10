// SPDX-License-Identifier: AGPL-3.0-or-later

#include "PluginEditor.h"

#include <cmath>

namespace
{
constexpr int editorWidth = 860;
constexpr int editorHeight = 520;
} // namespace

//==============================================================================
TuneAFishLookAndFeel::TuneAFishLookAndFeel()
{
    setColour (juce::PopupMenu::backgroundColourId, theme::panel);
    setColour (juce::PopupMenu::textColourId, theme::text);
    setColour (juce::PopupMenu::highlightedBackgroundColourId, theme::target.withAlpha (0.3f));
    setColour (juce::PopupMenu::highlightedTextColourId, theme::text);
    setColour (juce::ComboBox::backgroundColourId, theme::background);
    setColour (juce::ComboBox::outlineColourId, theme::outline);
    setColour (juce::ComboBox::textColourId, theme::text);
    setColour (juce::ComboBox::arrowColourId, theme::textDim);
    setColour (juce::ComboBox::focusedOutlineColourId, theme::target);
}

void TuneAFishLookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height, float sliderPos,
                                             float startAngle, float endAngle, juce::Slider& slider)
{
    constexpr float lineWidth = 3.0f;
    const auto bounds = juce::Rectangle<int> (x, y, width, height).toFloat().reduced (2.0f);
    const auto centre = bounds.getCentre();
    const auto radius = 0.5f * juce::jmin (bounds.getWidth(), bounds.getHeight()) - lineWidth;
    const auto valueAngle = startAngle + sliderPos * (endAngle - startAngle);
    const juce::PathStrokeType stroke (lineWidth, juce::PathStrokeType::curved, juce::PathStrokeType::rounded);

    juce::Path track;
    track.addCentredArc (centre.x, centre.y, radius, radius, 0.0f, startAngle, endAngle, true);
    g.setColour (slider.findColour (juce::Slider::rotarySliderOutlineColourId));
    g.strokePath (track, stroke);

    if (sliderPos > 0.0f)
    {
        juce::Path value;
        value.addCentredArc (centre.x, centre.y, radius, radius, 0.0f, startAngle, valueAngle, true);
        g.setColour (slider.findColour (juce::Slider::rotarySliderFillColourId));
        g.strokePath (value, stroke);
    }

    g.setColour (theme::text);
    g.drawLine ({ centre.getPointOnCircumference (radius * 0.2f, valueAngle),
                  centre.getPointOnCircumference (radius * 0.75f, valueAngle) },
                2.0f);
}

//==============================================================================
TuneAFishEditor::TuneAFishEditor (TuneAFishProcessor& p)
    : AudioProcessorEditor (p),
      pitchView (p)
{
    setLookAndFeel (&lookAndFeel);
    addAndMakeVisible (pitchView);

    const std::array<std::pair<const char*, const char*>, numChoices> choiceSpecs {
        { { "mode", "Mode" }, { "key", "Key" }, { "scale", "Scale" }, { "voice", "Voice" } }
    };

    for (size_t i = 0; i < choices.size(); ++i)
    {
        auto& choice = choices[i];
        const auto* paramID = choiceSpecs[i].first;
        choice.label = juce::String (choiceSpecs[i].second).toUpperCase();

        if (auto* param = dynamic_cast<juce::AudioParameterChoice*> (p.params.getParameter (paramID)))
            choice.box.addItemList (param->choices, 1);

        addAndMakeVisible (choice.box);
        choice.attachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (p.params, paramID, choice.box);
    }

    const std::array<std::pair<const char*, const char*>, numKnobs> knobSpecs {
        { { "retune", "Retune speed" }, { "amount", "Amount" } }
    };

    for (size_t i = 0; i < knobs.size(); ++i)
    {
        auto& knob = knobs[i];
        auto& slider = knob.slider;
        const auto* paramID = knobSpecs[i].first;
        knob.label = juce::String (knobSpecs[i].second).toUpperCase();

        slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
        slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 72, 18);
        slider.setColour (juce::Slider::rotarySliderFillColourId, theme::output);
        slider.setColour (juce::Slider::rotarySliderOutlineColourId, theme::outline);
        slider.setColour (juce::Slider::thumbColourId, theme::output);
        slider.setColour (juce::Slider::textBoxTextColourId, theme::text);
        slider.setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
        slider.setColour (juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
        slider.setColour (juce::Slider::textBoxHighlightColourId, theme::output.withAlpha (0.4f));
        addAndMakeVisible (slider);

        knob.attachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (p.params, paramID, slider);

        // Double-click a knob to reset it.
        if (auto* param = p.params.getParameter (paramID))
            slider.setDoubleClickReturnValue (true, param->convertFrom0to1 (param->getDefaultValue()));
    }

    setSize (editorWidth, editorHeight);
    startTimerHz (30);
}

TuneAFishEditor::~TuneAFishEditor()
{
    stopTimer();
    setLookAndFeel (nullptr);
}

void TuneAFishEditor::timerCallback()
{
    // Key and scale don't apply in MIDI mode.
    const bool scaleMode = choices[modeChoice].box.getSelectedItemIndex() == 0;
    choices[keyChoice].box.setEnabled (scaleMode);
    choices[scaleChoice].box.setEnabled (scaleMode);

    pitchView.update();
    repaint (readoutArea);
}

void TuneAFishEditor::paint (juce::Graphics& g)
{
    g.fillAll (theme::background);

    auto title = titleArea.toFloat();

    g.setColour (theme::text);
    g.setFont (juce::Font (juce::FontOptions (26.0f, juce::Font::bold)).withExtraKerningFactor (0.2f));
    g.drawText ("TUNE-A-FISH", title.removeFromTop (title.getHeight() * 0.62f), juce::Justification::bottomLeft);

    g.setColour (theme::textDim);
    g.setFont (juce::FontOptions (12.0f));
    g.drawText ("Vocal pitch correction", title, juce::Justification::topLeft);

    paintReadout (g);

    // Controls panel, with a label over each control.
    g.setColour (theme::panel);
    g.fillRoundedRectangle (controlsArea.toFloat(), 8.0f);

    g.setFont (juce::FontOptions (10.5f));

    for (const auto& choice : choices)
    {
        g.setColour (choice.box.isEnabled() ? theme::textDim : theme::textDim.withAlpha (0.4f));
        g.drawText (choice.label, choice.box.getBounds().withHeight (14).translated (0, -18), juce::Justification::centredLeft);
    }

    g.setColour (theme::textDim);

    for (const auto& knob : knobs)
        g.drawText (knob.label, knob.slider.getBounds().withHeight (14).translated (0, -14), juce::Justification::centred);
}

void TuneAFishEditor::paintReadout (juce::Graphics& g)
{
    const auto readout = pitchView.getReadout();
    auto area = readoutArea.toFloat();
    const auto big = area.removeFromTop (area.getHeight() * 0.62f);

    juce::String note = "-", detail;
    auto noteColour = theme::textDim;

    if (readout.voiced)
    {
        if (readout.targetNote >= 0)
        {
            note = TuneAFishProcessor::noteName (readout.targetNote);
            noteColour = theme::target;

            const int cents = juce::roundToInt (100.0f * (readout.inputPitch - (float) readout.targetNote));

            if (std::abs (cents) < 3)
                detail = "sung in tune";
            else
                detail = "sung " + juce::String (std::abs (cents)) + juce::String::fromUTF8 ("\xc2\xa2 ")
                         + (cents > 0 ? "sharp" : "flat");
        }
        else
        {
            // MIDI mode with no note held: the voice passes through untouched.
            note = TuneAFishProcessor::noteName (juce::roundToInt (readout.inputPitch));
            detail = "no MIDI note held";
        }
    }

    g.setColour (noteColour);
    g.setFont (juce::FontOptions (28.0f, juce::Font::bold));
    g.drawText (note, big, juce::Justification::bottomRight);

    g.setColour (theme::textDim);
    g.setFont (juce::FontOptions (12.0f));
    g.drawText (detail, area, juce::Justification::topRight);
}

void TuneAFishEditor::resized()
{
    auto area = getLocalBounds().reduced (16, 12);

    auto header = area.removeFromTop (56);
    readoutArea = header.removeFromRight (220);
    titleArea = header;

    area.removeFromTop (8);
    controlsArea = area.removeFromBottom (112);
    area.removeFromBottom (12);
    pitchView.setBounds (area);

    auto controls = controlsArea.reduced (16, 12);
    auto knobArea = controls.removeFromRight (2 * 112);

    for (auto& knob : knobs)
        knob.slider.setBounds (knobArea.removeFromLeft (112).withTrimmedTop (14).reduced (8, 0));

    controls.removeFromRight (16);
    const int columnWidth = controls.getWidth() / numChoices;

    for (auto& choice : choices)
    {
        auto column = controls.removeFromLeft (columnWidth).reduced (6, 0);
        choice.box.setBounds (column.withSizeKeepingCentre (column.getWidth(), 30).translated (0, 6));
    }
}
