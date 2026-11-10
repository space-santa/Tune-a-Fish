// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "PluginProcessor.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <deque>

namespace theme
{
inline const juce::Colour background { 0xff0e1418 };
inline const juce::Colour panel      { 0xff161f26 };
inline const juce::Colour outline    { 0xff27343e };
inline const juce::Colour text       { 0xffe6edf0 };
inline const juce::Colour textDim    { 0xff7f8f99 };
inline const juce::Colour input      { 0xff4fa3c7 }; // the voice as sung: sea blue
inline const juce::Colour output     { 0xffff8a5c }; // the voice after correction: coral
inline const juce::Colour target     { 0xff3fd0b0 }; // the note being tuned to: teal
} // namespace theme

/** A scrolling graph of the last few seconds: the sung pitch, the corrected pitch, and the
    note each was pulled towards, over a background of the notes in the current scale. */
class PitchView final : public juce::Component
{
public:
    explicit PitchView (TuneAFishProcessor&);

    /** Pulls new pitch reports from the processor. Call from the message thread. */
    void update();

    void paint (juce::Graphics&) override;

    struct Readout
    {
        bool voiced = false;
        float inputPitch = 0.0f;
        int targetNote = -1;
    };

    /** The most recent pitch, for the header. */
    Readout getReadout() const;

private:
    struct Point
    {
        std::int64_t sample;
        float input, output;
        int target;
    };

    float yForPitch (float pitch, juce::Rectangle<float> plot) const;
    std::int64_t nowSample() const;

    TuneAFishProcessor& processor;
    std::deque<Point> points;
    float centrePitch = 60.0f; // the pitch in the middle of the view, follows the singer
    double sampleRate = 44100.0;

    std::atomic<float>* mode = nullptr;
    std::atomic<float>* key = nullptr;
    std::atomic<float>* scale = nullptr;

    static constexpr double secondsShown = 5.0;
    static constexpr float semitonesShown = 14.0f;
};
