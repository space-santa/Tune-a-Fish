// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "dsp/PitchCorrector.h"
#include "dsp/PitchDetector.h"
#include "dsp/Scale.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <random>
#include <string>
#include <vector>

//==============================================================================
// A tiny test runner.

namespace testing
{
struct TestCase
{
    const char* name;
    void (*run)();
};

inline std::vector<TestCase>& registry()
{
    static std::vector<TestCase> tests;
    return tests;
}

struct Registrar
{
    Registrar (const char* name, void (*run)()) { registry().push_back ({ name, run }); }
};

inline int failures = 0;
inline int checks = 0;
} // namespace testing

#define TEST(name)                                                         \
    static void name();                                                    \
    static const testing::Registrar registrar_##name (#name, name);        \
    static void name()

#define CHECK(condition)                                                                    \
    do                                                                                      \
    {                                                                                       \
        ++testing::checks;                                                                  \
        if (! (condition))                                                                  \
        {                                                                                   \
            ++testing::failures;                                                            \
            std::printf ("    FAILED %s:%d: %s\n", __FILE__, __LINE__, #condition);          \
        }                                                                                   \
    } while (false)

#define CHECK_NEAR(actual, expected, tolerance)                                                     \
    do                                                                                              \
    {                                                                                               \
        ++testing::checks;                                                                          \
        const double a_ = (actual), e_ = (expected), t_ = (tolerance);                              \
        if (! (std::abs (a_ - e_) <= t_))                                                           \
        {                                                                                           \
            ++testing::failures;                                                                    \
            std::printf ("    FAILED %s:%d: %s = %.4f, expected %.4f +/- %.4f\n", __FILE__, __LINE__, \
                         #actual, a_, e_, t_);                                                      \
        }                                                                                           \
    } while (false)

//==============================================================================
// Test signals.

namespace testing
{
/** A sung "ah": a band-limited pulse train shaped by three formants. The formants stay put
    while the pitch moves, like a real voice. f0 is a function of time in seconds. */
inline std::vector<float> vowel (double sampleRate, double seconds, const std::function<double (double)>& f0,
                                 double peak = 0.3)
{
    struct Formant
    {
        double hz, bandwidth, gain;
    };

    constexpr Formant formants[] { { 730.0, 80.0, 1.0 }, { 1090.0, 90.0, 0.5 }, { 2440.0, 120.0, 0.25 } };

    const auto envelope = [&] (double hz)
    {
        double e = 0.03;

        for (const auto& f : formants)
        {
            const double d = (hz - f.hz) / f.bandwidth;
            e += f.gain / (1.0 + d * d);
        }

        return e;
    };

    const auto numSamples = (size_t) (seconds * sampleRate);
    std::vector<float> out (numSamples);
    double phase = 0.0;
    double maxAbs = 0.0;
    std::vector<double> gains;

    for (size_t i = 0; i < numSamples; ++i)
    {
        const double hz = f0 ((double) i / sampleRate);
        const int harmonics = std::max (1, (int) (std::min (0.45 * sampleRate, 8000.0) / hz));

        if (i % 32 == 0 || gains.size() != (size_t) harmonics)
        {
            gains.resize ((size_t) harmonics);

            for (int k = 1; k <= harmonics; ++k)
                gains[(size_t) k - 1] = envelope (k * hz) / k;
        }

        // Sum of cos(k * theta) by the Chebyshev recurrence.
        const double theta = 2.0 * 3.14159265358979323846 * phase;
        const double c1 = std::cos (theta);
        double previous = 1.0, current = c1, sum = gains[0] * c1;

        for (int k = 2; k <= harmonics; ++k)
        {
            const double next = 2.0 * c1 * current - previous;
            previous = current;
            current = next;
            sum += gains[(size_t) k - 1] * current;
        }

        out[i] = (float) sum;
        maxAbs = std::max (maxAbs, std::abs (sum));
        phase += hz / sampleRate;
        phase -= std::floor (phase);
    }

    for (auto& s : out)
        s = (float) (s * peak / maxAbs);

    return out;
}

inline std::vector<float> steadyVowel (double sampleRate, double seconds, double hz)
{
    return vowel (sampleRate, seconds, [hz] (double) { return hz; });
}

inline std::vector<float> noise (double sampleRate, double seconds, double amplitude, unsigned seed = 1)
{
    std::mt19937 rng (seed);
    std::uniform_real_distribution<float> dist (-1.0f, 1.0f);
    std::vector<float> out ((size_t) (seconds * sampleRate));

    for (auto& s : out)
        s = (float) amplitude * dist (rng);

    return out;
}

//==============================================================================
// Running the corrector.

struct NoteEvent
{
    std::int64_t sample;
    int note;
    bool on;
};

struct Run
{
    std::vector<float> output; // aligned with the input (latency removed)
    std::int64_t shortfalls = 0;
    int latency = 0;
};

struct GrainCollector final : tuneafish::PitchCorrector::Listener
{
    std::vector<tuneafish::PitchCorrector::GrainInfo> grains;
    void grainProduced (const tuneafish::PitchCorrector::GrainInfo& g) override { grains.push_back (g); }
};

/** Runs a mono signal through the corrector, in blocks, with optional MIDI. Notes at the
    same sample are sent in the order given. */
inline Run correct (const std::vector<float>& input, double sampleRate, const tuneafish::CorrectorSettings& settings,
                    std::vector<NoteEvent> notes = {}, int blockSize = 256,
                    tuneafish::PitchCorrector::Listener* listener = nullptr)
{
    tuneafish::PitchCorrector corrector;
    corrector.prepare (sampleRate, 1);
    corrector.setSettings (settings);
    corrector.setListener (listener);

    Run run;
    run.latency = corrector.getLatencySamples();

    std::vector<float> padded (input);
    padded.resize (input.size() + (size_t) run.latency, 0.0f);
    std::vector<float> out (padded.size(), 0.0f);

    std::stable_sort (notes.begin(), notes.end(), [] (auto& a, auto& b) { return a.sample < b.sample; });
    size_t nextNote = 0;
    std::int64_t position = 0;
    const auto total = (std::int64_t) padded.size();

    while (position < total)
    {
        auto end = std::min (total, position + blockSize);

        while (nextNote < notes.size() && notes[nextNote].sample <= position)
        {
            if (notes[nextNote].on)
                corrector.noteOn (notes[nextNote].note);
            else
                corrector.noteOff (notes[nextNote].note);

            ++nextNote;
        }

        if (nextNote < notes.size())
            end = std::min (end, notes[nextNote].sample);

        const float* in[] { padded.data() + position };
        float* o[] { out.data() + position };
        corrector.process (in, o, 1, (int) (end - position));
        position = end;
    }

    run.output.assign (out.begin() + run.latency, out.end());
    run.shortfalls = corrector.getLookaheadShortfalls();
    return run;
}

//==============================================================================
// Measuring pitch.

struct PitchStats
{
    double median = 0.0;       // semitones
    double mean = 0.0;
    double stdevCents = 0.0;
    double voicedFraction = 0.0;
};

/** Detected pitch (in semitones, or -1 when unvoiced) every 4 ms, with the time it refers to. */
inline std::vector<std::pair<double, double>> pitchTrack (const std::vector<float>& signal, double sampleRate,
                                                          double minHz = 65.0, double maxHz = 1200.0)
{
    tuneafish::PitchDetector detector;
    detector.prepare (sampleRate, minHz);
    detector.setRange (minHz, maxHz);

    // The estimate describes the window that just ended; date it at the window's centre.
    const double delay = 1.0 / minHz;
    std::vector<std::pair<double, double>> track;

    for (size_t i = 0; i < signal.size(); ++i)
    {
        if (detector.push (signal[i]))
        {
            const auto& e = detector.estimate();
            const double t = (double) i / sampleRate - delay;
            track.push_back ({ t, e.voiced ? tuneafish::frequencyToPitch (sampleRate / e.period) : -1.0 });
        }
    }

    return track;
}

inline PitchStats pitchStats (const std::vector<float>& signal, double sampleRate, double fromSeconds, double toSeconds)
{
    const auto track = pitchTrack (signal, sampleRate);
    std::vector<double> pitches;
    int frames = 0;

    for (const auto& [t, p] : track)
    {
        if (t < fromSeconds || t > toSeconds)
            continue;

        ++frames;

        if (p > 0.0)
            pitches.push_back (p);
    }

    PitchStats stats;

    if (pitches.empty())
        return stats;

    stats.voicedFraction = (double) pitches.size() / frames;

    double sum = 0.0;

    for (auto p : pitches)
        sum += p;

    stats.mean = sum / (double) pitches.size();

    double squares = 0.0;

    for (auto p : pitches)
        squares += (p - stats.mean) * (p - stats.mean);

    stats.stdevCents = 100.0 * std::sqrt (squares / (double) pitches.size());

    std::sort (pitches.begin(), pitches.end());
    stats.median = pitches[pitches.size() / 2];
    return stats;
}

/** Signal-to-error ratio in dB between `reference` and `test` read `shift` samples later
    (a fractional shift is read with cubic interpolation). */
inline double snrAtShift (const std::vector<float>& reference, const std::vector<float>& test, size_t from, size_t to,
                          double shift)
{
    const auto whole = (std::int64_t) std::floor (shift);
    const double t = shift - (double) whole;
    const double w0 = t * (-0.5 + t * (1.0 - 0.5 * t));
    const double w1 = 1.0 + t * t * (-2.5 + 1.5 * t);
    const double w2 = t * (0.5 + t * (2.0 - 1.5 * t));
    const double w3 = t * t * (-0.5 + 0.5 * t);

    double signal = 0.0, error = 0.0;

    for (size_t i = from; i < to; ++i)
    {
        const auto j = (std::int64_t) i + whole;

        if (j < 1 || j + 2 >= (std::int64_t) test.size())
            continue;

        const double r = reference[i];
        const double value = w0 * test[(size_t) j - 1] + w1 * test[(size_t) j] + w2 * test[(size_t) j + 1]
                             + w3 * test[(size_t) j + 2];
        signal += r * r;
        error += (value - r) * (value - r);
    }

    return 10.0 * std::log10 (signal / std::max (error, 1.0e-30));
}

/** The best signal-to-error ratio in dB between two signals over a range, trying every
    alignment within +/- maxShift samples to a tenth of a sample. */
inline double alignedSnr (const std::vector<float>& reference, const std::vector<float>& test, size_t from, size_t to,
                          int maxShift, double* shiftOut = nullptr)
{
    double bestSnr = -1.0e9, bestShift = 0.0;

    for (int shift = -maxShift; shift <= maxShift; ++shift)
    {
        const double snr = snrAtShift (reference, test, from, to, shift);

        if (snr > bestSnr)
        {
            bestSnr = snr;
            bestShift = shift;
        }
    }

    const double whole = bestShift;

    for (double shift = whole - 1.0; shift <= whole + 1.0; shift += 0.1)
    {
        const double snr = snrAtShift (reference, test, from, to, shift);

        if (snr > bestSnr)
        {
            bestSnr = snr;
            bestShift = shift;
        }
    }

    if (shiftOut != nullptr)
        *shiftOut = bestShift;

    return bestSnr;
}
} // namespace testing
