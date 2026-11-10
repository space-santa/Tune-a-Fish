// SPDX-License-Identifier: AGPL-3.0-or-later

#include "PitchDetector.h"

#include <algorithm>
#include <cmath>

namespace tuneafish
{
namespace
{
constexpr double pi = 3.14159265358979323846;

constexpr double targetDetectRate = 20000.0; // decimate down to about this rate
constexpr double hopSeconds = 0.004;
constexpr double lowpassHz = 4000.0;
constexpr double highpassHz = 50.0;

// YIN takes the first dip below this as the period.
constexpr double threshold = 0.15;

// Voicing has hysteresis: it's harder to start a voiced stretch than to stay in one.
constexpr double startVoicedBelow = 0.2;
constexpr double stayVoicedBelow = 0.35;
constexpr double silenceRms = 0.0015; // about -56 dBFS

std::int64_t nextPowerOfTwo (std::int64_t n)
{
    std::int64_t p = 1;

    while (p < n)
        p <<= 1;

    return p;
}
} // namespace

PitchDetector::Biquad PitchDetector::Biquad::lowpass (double sampleRate, double hz, double q)
{
    const double w0 = 2.0 * pi * hz / sampleRate;
    const double alpha = std::sin (w0) / (2.0 * q);
    const double cosW0 = std::cos (w0);
    const double a0 = 1.0 + alpha;

    Biquad f;
    f.b0 = (1.0 - cosW0) * 0.5 / a0;
    f.b1 = (1.0 - cosW0) / a0;
    f.b2 = f.b0;
    f.a1 = -2.0 * cosW0 / a0;
    f.a2 = (1.0 - alpha) / a0;
    return f;
}

PitchDetector::Biquad PitchDetector::Biquad::highpass (double sampleRate, double hz, double q)
{
    const double w0 = 2.0 * pi * hz / sampleRate;
    const double alpha = std::sin (w0) / (2.0 * q);
    const double cosW0 = std::cos (w0);
    const double a0 = 1.0 + alpha;

    Biquad f;
    f.b0 = (1.0 + cosW0) * 0.5 / a0;
    f.b1 = -(1.0 + cosW0) / a0;
    f.b2 = f.b0;
    f.a1 = -2.0 * cosW0 / a0;
    f.a2 = (1.0 - alpha) / a0;
    return f;
}

void PitchDetector::prepare (double newSampleRate, double lowestHz)
{
    sampleRate = newSampleRate;
    decimation = std::max (1, (int) std::floor (sampleRate / targetDetectRate));
    detectRate = sampleRate / decimation;
    hop = std::max (1, (int) std::lround (detectRate * hopSeconds));

    const double cutoff = std::min (lowpassHz, 0.4 * detectRate);
    filters[0] = Biquad::highpass (sampleRate, highpassHz, 0.7071);
    filters[1] = Biquad::lowpass (sampleRate, cutoff, 0.5412); // two sections make a
    filters[2] = Biquad::lowpass (sampleRate, cutoff, 1.3066); // 4th-order Butterworth

    maxLagEver = (int) std::ceil (detectRate / lowestHz) + 2;

    const auto frameSize = (size_t) (2 * maxLagEver);
    ring.assign ((size_t) nextPowerOfTwo ((std::int64_t) frameSize + 1), 0.0f);
    ringMask = (std::int64_t) ring.size() - 1;
    frame.assign (frameSize, 0.0f);
    difference.assign ((size_t) maxLagEver + 2, 0.0f);
    normalised.assign ((size_t) maxLagEver + 2, 1.0f);

    setRange (lowestHz, 1000.0);
    reset();
}

void PitchDetector::setRange (double minHz, double maxHz)
{
    // There's nothing to fit a range into before prepare(), which sets its own.
    if (maxLagEver < 4)
        return;

    maxLag = std::clamp ((int) std::ceil (detectRate / minHz), 4, maxLagEver);
    minLag = std::clamp ((int) std::floor (detectRate / maxHz), 2, maxLag - 2);
}

void PitchDetector::reset()
{
    for (auto& f : filters)
        f.z1 = f.z2 = 0.0;

    std::fill (ring.begin(), ring.end(), 0.0f);
    written = 0;
    decimationPhase = 0;
    hopCounter = 0;
    current = {};
}

bool PitchDetector::push (float sample)
{
    double x = sample;

    for (auto& f : filters)
        x = f.process (x);

    if (++decimationPhase < decimation)
        return false;

    decimationPhase = 0;
    ring[(size_t) (written & ringMask)] = (float) x;
    ++written;

    if (++hopCounter < hop)
        return false;

    hopCounter = 0;
    analyse();
    return true;
}

double PitchDetector::lagMinimumNear (double lag, int& lagOut) const
{
    const int from = std::max (minLag, (int) std::floor (lag * 0.9));
    const int to = std::min (maxLag - 1, (int) std::ceil (lag * 1.1));
    double best = 1.0e9;
    lagOut = -1;

    for (int tau = from; tau <= to; ++tau)
    {
        if (normalised[(size_t) tau] < best)
        {
            best = normalised[(size_t) tau];
            lagOut = tau;
        }
    }

    return best;
}

void PitchDetector::analyse()
{
    const bool wasVoiced = current.voiced;
    const double previousLag = current.period / decimation;

    const int window = maxLag; // one period of the lowest pitch in range
    const int needed = window + maxLag;

    if (written < needed)
    {
        current = {};
        return;
    }

    const std::int64_t start = written - needed;
    double energy = 0.0;

    for (int i = 0; i < needed; ++i)
    {
        const float v = ring[(size_t) ((start + i) & ringMask)];
        frame[(size_t) i] = v;
        energy += (double) v * v;
    }

    const double level = std::sqrt (energy / needed);

    // Step 1: the difference function. How different is the signal from itself delayed by tau?
    const float* x = frame.data();

    for (int tau = 1; tau <= maxLag; ++tau)
    {
        float sum = 0.0f;

        for (int j = 0; j < window; ++j)
        {
            const float d = x[j] - x[j + tau];
            sum += d * d;
        }

        difference[(size_t) tau] = sum;
    }

    // Step 2: normalise by the running mean, so the dips are comparable across lags.
    normalised[0] = 1.0f;
    double running = 0.0;

    for (int tau = 1; tau <= maxLag; ++tau)
    {
        running += difference[(size_t) tau];
        normalised[(size_t) tau] = running > 0.0 ? (float) (difference[(size_t) tau] * tau / running) : 1.0f;
    }

    // Step 3: the first dip below the threshold, followed down to its bottom.
    int best = -1;

    for (int tau = minLag; tau < maxLag; ++tau)
    {
        if (normalised[(size_t) tau] < threshold)
        {
            while (tau + 1 < maxLag && normalised[(size_t) tau + 1] < normalised[(size_t) tau])
                ++tau;

            best = tau;
            break;
        }
    }

    if (best < 0)
        best = (int) (std::min_element (normalised.begin() + minLag, normalised.begin() + maxLag) - normalised.begin());

    // A jump down an octave is usually a missed dip at the real period, which sits just
    // above the threshold. Stay where we were if the signal is still nearly periodic there.
    if (wasVoiced && previousLag > 0.0 && best > 1.8 * previousLag && best < 2.25 * previousLag)
    {
        int near = -1;

        if (lagMinimumNear (previousLag, near) < 2.0 * threshold && near > 0)
            best = near;
    }

    // Step 4: parabolic interpolation for a lag between samples.
    double lag = best;

    if (best > 1 && best < maxLag)
    {
        const double a = normalised[(size_t) best - 1];
        const double b = normalised[(size_t) best];
        const double c = normalised[(size_t) best + 1];
        const double denominator = a - 2.0 * b + c;

        if (denominator > 0.0)
            lag += std::clamp (0.5 * (a - c) / denominator, -0.5, 0.5);
    }

    const double aperiodicity = normalised[(size_t) best];
    const bool periodic = aperiodicity < (wasVoiced ? stayVoicedBelow : startVoicedBelow);

    current.aperiodicity = aperiodicity;
    current.level = level;
    current.voiced = periodic && level > silenceRms;
    current.period = current.voiced ? lag * decimation : 0.0;
}
} // namespace tuneafish
