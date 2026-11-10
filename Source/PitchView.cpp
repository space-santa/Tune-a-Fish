// SPDX-License-Identifier: AGPL-3.0-or-later

#include "PitchView.h"

#include <cmath>

namespace
{
constexpr double gapSeconds = 0.03; // break the lines where reports are further apart than this
constexpr float followRate = 0.05f; // how quickly the view re-centres on the singer, per frame
} // namespace

PitchView::PitchView (TuneAFishProcessor& p)
    : processor (p),
      mode (p.params.getRawParameterValue ("mode")),
      key (p.params.getRawParameterValue ("key")),
      scale (p.params.getRawParameterValue ("scale"))
{
    setOpaque (true);
}

std::int64_t PitchView::nowSample() const
{
    return processor.history.outputPosition.load();
}

void PitchView::update()
{
    sampleRate = processor.history.sampleRate.load();

    processor.history.popAll ([this] (const PitchHistory::Grain& grain)
                              {
                                  // The processor was reset (new sample rate, or the host restarted it).
                                  if (! points.empty() && grain.outputSample < points.back().sample - (std::int64_t) sampleRate)
                                      points.clear();

                                  points.push_back ({ grain.outputSample, grain.inputPitch, grain.outputPitch, grain.targetNote });
                              });

    const auto now = nowSample();
    const auto oldest = now - (std::int64_t) ((secondsShown + 0.2) * sampleRate);

    while (! points.empty() && points.front().sample < oldest)
        points.pop_front();

    // Drift the view towards the average pitch of the last second and a half.
    float sum = 0.0f;
    int count = 0;

    for (auto it = points.rbegin(); it != points.rend() && it->sample > now - (std::int64_t) (1.5 * sampleRate); ++it)
    {
        if (it->output > 0.0f)
        {
            sum += it->output;
            ++count;
        }
    }

    if (count > 0)
        centrePitch += (sum / (float) count - centrePitch) * followRate;

    repaint();
}

PitchView::Readout PitchView::getReadout() const
{
    const auto now = nowSample();

    for (auto it = points.rbegin(); it != points.rend(); ++it)
    {
        if (it->sample > now)
            continue; // not heard yet

        if (it->sample < now - (std::int64_t) (0.15 * sampleRate) || it->input <= 0.0f)
            break;

        return { true, it->input, it->target };
    }

    return {};
}

float PitchView::yForPitch (float pitch, juce::Rectangle<float> plot) const
{
    const float low = centrePitch - 0.5f * semitonesShown;
    return plot.getBottom() - (pitch - low) / semitonesShown * plot.getHeight();
}

void PitchView::paint (juce::Graphics& g)
{
    g.fillAll (theme::background);

    const auto bounds = getLocalBounds().toFloat();
    g.setColour (theme::panel);
    g.fillRoundedRectangle (bounds, 8.0f);

    auto plot = bounds.reduced (12.0f, 10.0f);
    const auto labels = plot.removeFromLeft (40.0f);

    const bool midiMode = mode->load() > 0.5f;
    const int keyIndex = juce::jlimit (0, 11, juce::roundToInt (key->load()));
    const auto scaleType = (tuneafish::ScaleType) juce::jlimit (0, (int) tuneafish::ScaleType::count - 1,
                                                                juce::roundToInt (scale->load()));

    // Note rows. In scale mode the notes of the scale are highlighted, since those are the
    // only places the voice can land. In MIDI mode any note can be a target.
    const float rowHeight = plot.getHeight() / semitonesShown;
    const int lowest = (int) std::floor (centrePitch - 0.5f * semitonesShown);
    const int highest = (int) std::ceil (centrePitch + 0.5f * semitonesShown);

    g.setFont (juce::FontOptions (11.0f));

    for (int note = lowest; note <= highest; ++note)
    {
        const float y = yForPitch ((float) note, plot);

        if (y < plot.getY() || y > plot.getBottom())
            continue;

        const bool highlighted = ! midiMode && tuneafish::isInScale (note, keyIndex, scaleType);

        if (highlighted)
        {
            g.setColour (theme::target.withAlpha (0.05f));
            g.fillRect (plot.getX(), y - 0.5f * rowHeight, plot.getWidth(), rowHeight);
        }

        g.setColour (theme::outline.withAlpha (highlighted || midiMode ? 0.9f : 0.35f));
        g.drawHorizontalLine (juce::roundToInt (y), plot.getX(), plot.getRight());

        if (highlighted || midiMode)
        {
            g.setColour (theme::textDim);
            g.drawText (TuneAFishProcessor::noteName (note), labels.withY (y - 7.0f).withHeight (14.0f),
                        juce::Justification::centredLeft);
        }
    }

    // Legend.
    {
        auto legend = plot.removeFromTop (18.0f).removeFromRight (250.0f).translated (0.0f, 2.0f);
        const auto item = [&] (juce::Colour colour, const juce::String& label)
        {
            auto area = legend.removeFromLeft (84.0f);
            g.setColour (colour);
            g.fillRoundedRectangle (area.removeFromLeft (14.0f).withSizeKeepingCentre (14.0f, 3.0f), 1.5f);
            g.setColour (theme::textDim);
            g.drawText (label, area.withTrimmedLeft (6.0f), juce::Justification::centredLeft);
        };

        item (theme::input, "sung");
        item (theme::output, "corrected");
        item (theme::target.withAlpha (0.5f), "target");
        plot = plot.withTop (bounds.getY() + 10.0f); // the legend sits over the plot
    }

    if (points.empty())
        return;

    juce::Graphics::ScopedSaveState state (g);
    g.reduceClipRegion (plot.toNearestInt());

    const auto now = nowSample();
    const double span = secondsShown * sampleRate;
    const auto gap = (std::int64_t) (gapSeconds * sampleRate);
    const auto xFor = [&] (std::int64_t sample)
    { return plot.getRight() - (float) ((double) (now - sample) / span) * plot.getWidth(); };

    // Target notes, as bars along their rows while the voice is being pulled to them.
    {
        std::int64_t runStart = 0, runEnd = 0;
        int runTarget = -1;

        const auto flush = [&]
        {
            if (runTarget < 0)
                return;

            const float y = yForPitch ((float) runTarget, plot);
            const float x0 = xFor (runStart), x1 = xFor (runEnd);
            g.setColour (theme::target.withAlpha (0.28f));
            g.fillRoundedRectangle (x0, y - 3.5f, juce::jmax (2.0f, x1 - x0), 7.0f, 3.5f);
        };

        for (const auto& p : points)
        {
            const int target = p.input > 0.0f ? p.target : -1;

            if (target != runTarget || p.sample - runEnd > gap)
            {
                flush();
                runTarget = target;
                runStart = p.sample;
            }

            runEnd = p.sample;
        }

        flush();
    }

    const auto strokePitch = [&] (auto valueOf, juce::Colour colour, float thickness)
    {
        juce::Path path;
        bool drawing = false;
        std::int64_t last = 0;

        for (const auto& p : points)
        {
            const float value = valueOf (p);

            if (value <= 0.0f)
            {
                drawing = false;
                continue;
            }

            const juce::Point<float> point { xFor (p.sample), yForPitch (value, plot) };

            if (drawing && p.sample - last <= gap)
                path.lineTo (point);
            else
                path.startNewSubPath (point);

            drawing = true;
            last = p.sample;
        }

        g.setColour (colour);
        g.strokePath (path, juce::PathStrokeType (thickness, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    };

    strokePitch ([] (const Point& p) { return p.input; }, theme::input.withAlpha (0.85f), 1.5f);
    strokePitch ([] (const Point& p) { return p.output; }, theme::output, 2.5f);
}
