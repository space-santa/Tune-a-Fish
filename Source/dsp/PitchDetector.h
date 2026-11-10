// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace tuneafish
{
/** A streaming YIN pitch detector for one voice.

    The input is filtered (50 Hz to 4 kHz) and decimated to roughly 20 kHz, which is plenty
    for finding a singing voice's fundamental, and keeps the cost low at high sample rates.
    Every 4 ms it runs YIN (de Cheveigné & Kawahara, 2002) over the most recent window,
    which spans two periods of the lowest pitch in the current range. */
class PitchDetector
{
public:
    struct Estimate
    {
        double period = 0.0;       // in input samples, 0 when unvoiced
        double aperiodicity = 1.0; // YIN's normalised difference at the chosen lag: 0 is perfectly periodic
        double level = 0.0;        // RMS of the filtered analysis window
        bool voiced = false;
    };

    /** lowestHz is the lowest pitch any range will ask for. It sets the buffer sizes. */
    void prepare (double sampleRate, double lowestHz);

    /** The pitch range to search. Narrower ranges make octave mistakes less likely. */
    void setRange (double minHz, double maxHz);

    void reset();

    /** Feed one input sample. Returns true when a new estimate is ready. */
    bool push (float sample);

    const Estimate& estimate() const { return current; }

private:
    struct Biquad
    {
        double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
        double z1 = 0.0, z2 = 0.0;

        static Biquad lowpass (double sampleRate, double hz, double q);
        static Biquad highpass (double sampleRate, double hz, double q);

        double process (double x)
        {
            const double y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            return y;
        }
    };

    void analyse();
    double lagMinimumNear (double lag, int& lagOut) const;

    double sampleRate = 44100.0;
    double detectRate = 22050.0;
    int decimation = 1;
    int decimationPhase = 0;
    int hop = 88;
    int hopCounter = 0;

    int maxLagEver = 0; // for the lowest pitch we were prepared for
    int minLag = 2;     // current range, in decimated samples
    int maxLag = 2;

    std::array<Biquad, 3> filters;
    std::vector<float> ring;
    std::int64_t ringMask = 0;
    std::int64_t written = 0;

    std::vector<float> frame, difference, normalised;
    Estimate current;
};
} // namespace tuneafish
