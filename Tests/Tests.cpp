// SPDX-License-Identifier: AGPL-3.0-or-later

#include "TestHelpers.h"

#include <cstring>

using namespace tuneafish;
using namespace testing;

namespace
{
CorrectorSettings scaleMode (int key, ScaleType scale, double retuneMs = 0.0)
{
    CorrectorSettings s;
    s.key = key;
    s.scale = scale;
    s.retuneMs = retuneMs;
    return s;
}

CorrectorSettings midiMode (double retuneMs = 0.0)
{
    CorrectorSettings s;
    s.midiMode = true;
    s.retuneMs = retuneMs;
    return s;
}

constexpr double sr = 48000.0;
} // namespace

//==============================================================================
TEST (nearest_note_in_scale)
{
    // Chromatic just rounds.
    CHECK (nearestNoteInScale (60.4, 0, ScaleType::chromatic) == 60);
    CHECK (nearestNoteInScale (60.6, 0, ScaleType::chromatic) == 61);
    CHECK (nearestNoteInScale (-0.4, 0, ScaleType::chromatic) == 0);

    // C major skips the black keys.
    CHECK (nearestNoteInScale (61.3, 0, ScaleType::major) == 62); // C# + 30 cents -> D
    CHECK (nearestNoteInScale (60.9, 0, ScaleType::major) == 60); // C# - 10 cents -> C
    CHECK (nearestNoteInScale (58.2, 0, ScaleType::major) == 59); // A# + 20 cents -> B

    // A minor (key 9) has the same notes as C major.
    for (double p = 40.0; p < 90.0; p += 0.37)
        CHECK (nearestNoteInScale (p, 9, ScaleType::minor) == nearestNoteInScale (p, 0, ScaleType::major));

    // D harmonic minor has C#, not C.
    CHECK (nearestNoteInScale (60.2, 2, ScaleType::harmonicMinor) == 61);

    // C major pentatonic: C D E G A.
    CHECK (nearestNoteInScale (65.4, 0, ScaleType::majorPentatonic) == 64); // F + 40 cents -> E
    CHECK (nearestNoteInScale (65.6, 0, ScaleType::majorPentatonic) == 67); // F + 60 cents -> G

    // Every result is in the scale.
    for (int scale = 0; scale < (int) ScaleType::count; ++scale)
        for (int key = 0; key < 12; ++key)
            for (double p = 30.0; p < 100.0; p += 0.13)
                CHECK (isInScale (nearestNoteInScale (p, key, (ScaleType) scale), key, (ScaleType) scale));
}

TEST (frequency_conversions)
{
    CHECK_NEAR (frequencyToPitch (440.0), 69.0, 1.0e-9);
    CHECK_NEAR (frequencyToPitch (220.0), 57.0, 1.0e-9);
    CHECK_NEAR (pitchToFrequency (60.0), 261.6255653, 1.0e-6);
}

//==============================================================================
TEST (detector_finds_pitch_of_a_voice)
{
    for (double rate : { 44100.0, 48000.0, 96000.0 })
    {
        for (double hz : { 75.0, 110.0, 196.0, 330.0, 523.0, 880.0 })
        {
            const auto signal = steadyVowel (rate, 1.0, hz);
            const auto stats = pitchStats (signal, rate, 0.1, 0.9);
            CHECK (stats.voicedFraction > 0.95);
            CHECK_NEAR (stats.median, frequencyToPitch (hz), 0.03); // 3 cents
        }
    }
}

TEST (detector_ignores_noise_and_silence)
{
    const auto hiss = noise (sr, 1.0, 0.2);
    CHECK (pitchStats (hiss, sr, 0.1, 0.9).voicedFraction < 0.05);

    const std::vector<float> silence ((size_t) sr, 0.0f);
    CHECK (pitchStats (silence, sr, 0.1, 0.9).voicedFraction == 0.0);
}

//==============================================================================
TEST (latency_is_about_40ms)
{
    for (double rate : { 44100.0, 48000.0, 96000.0 })
    {
        PitchCorrector c;
        c.prepare (rate, 2);
        const double ms = 1000.0 * c.getLatencySamples() / rate;
        std::printf ("    %.0f Hz: latency %d samples (%.1f ms)\n", rate, c.getLatencySamples(), ms);
        CHECK (ms > 30.0 && ms < 45.0);
    }
}

TEST (unvoiced_audio_passes_through_unchanged)
{
    const auto hiss = noise (sr, 1.0, 0.2);
    const auto run = correct (hiss, sr, scaleMode (0, ScaleType::chromatic));

    double maxError = 0.0;

    for (size_t i = 0; i < hiss.size(); ++i)
        maxError = std::max (maxError, (double) std::abs (run.output[i] - hiss[i]));

    std::printf ("    max error %.2e\n", maxError);
    CHECK (maxError < 1.0e-5);
    CHECK (run.shortfalls == 0);
}

TEST (zero_amount_leaves_a_voice_alone)
{
    auto settings = scaleMode (0, ScaleType::chromatic);
    settings.amount = 0.0;

    const auto voice = steadyVowel (sr, 1.5, 225.0);
    const auto run = correct (voice, sr, settings);
    const auto in = pitchStats (voice, sr, 0.2, 1.3);
    const auto out = pitchStats (run.output, sr, 0.2, 1.3);

    double shift = 0.0;
    const double snr = alignedSnr (voice, run.output, (size_t) (0.2 * sr), (size_t) (1.3 * sr), 200, &shift);
    std::printf ("    pitch in %.3f, out %.3f; SNR %.1f dB at a shift of %.1f samples\n", in.median, out.median, snr, shift);

    CHECK_NEAR (out.median, in.median, 0.02);
    CHECK (snr > 30.0);
    CHECK (run.shortfalls == 0);
}

TEST (zero_amount_is_transparent_through_glides_and_gaps)
{
    // Speech-like: a voice sliding up and down by an octave, interrupted by hiss and silence.
    auto signal = vowel (sr, 1.2, [] (double t) { return 140.0 * std::exp2 (std::sin (2.0 * 3.14159265 * 0.8 * t)); });
    const auto hiss = noise (sr, 0.15, 0.05, 3);
    signal.insert (signal.end(), hiss.begin(), hiss.end());
    const auto second = vowel (sr, 0.8, [] (double t) { return 300.0 - 150.0 * t; });
    signal.insert (signal.end(), second.begin(), second.end());
    signal.resize (signal.size() + (size_t) (0.1 * sr), 0.0f);
    const auto third = steadyVowel (sr, 0.6, 180.0);
    signal.insert (signal.end(), third.begin(), third.end());

    auto settings = scaleMode (0, ScaleType::chromatic);
    settings.amount = 0.0;
    const auto run = correct (signal, sr, settings);

    double shift = 0.0;
    const double snr = alignedSnr (signal, run.output, (size_t) (0.1 * sr), signal.size() - (size_t) (0.1 * sr), 300, &shift);
    std::printf ("    SNR %.1f dB at a shift of %.1f samples\n", snr, shift);

    CHECK (snr > 30.0);
    CHECK (run.shortfalls == 0);
}

//==============================================================================
TEST (chromatic_snaps_to_nearest_semitone)
{
    // 225 Hz is A3 (220 Hz) + 39 cents.
    const auto voice = steadyVowel (sr, 1.5, 225.0);
    const auto run = correct (voice, sr, scaleMode (0, ScaleType::chromatic));
    const auto out = pitchStats (run.output, sr, 0.2, 1.3);
    std::printf ("    out %.4f (target 57), stdev %.2f cents\n", out.median, out.stdevCents);

    CHECK_NEAR (out.median, 57.0, 0.03);
    CHECK (out.stdevCents < 3.0);
    CHECK (out.voicedFraction > 0.95);
    CHECK (run.shortfalls == 0);
}

TEST (corrects_in_both_directions_at_several_pitches)
{
    for (double rate : { 44100.0, 48000.0 })
    {
        for (double hz : { 92.0, 151.0, 300.0, 455.0, 690.0 })
        {
            auto settings = scaleMode (0, ScaleType::chromatic);
            settings.voice = hz < 100.0 ? VoiceType::low : (hz > 600.0 ? VoiceType::soprano : VoiceType::altoTenor);

            const double target = std::round (frequencyToPitch (hz));
            const auto run = correct (steadyVowel (rate, 1.2, hz), rate, settings);
            const auto out = pitchStats (run.output, rate, 0.2, 1.0);
            std::printf ("    %.0f Hz @ %.0f: in %.3f -> out %.3f (target %.0f)\n", hz, rate, frequencyToPitch (hz),
                         out.median, target);

            CHECK_NEAR (out.median, target, 0.03);
            CHECK (run.shortfalls == 0);
        }
    }
}

TEST (scale_mode_uses_only_scale_notes)
{
    // 236 Hz is A#3 + 21 cents. A# isn't in C major; B3 is closer than A3.
    const auto voice = steadyVowel (sr, 1.5, 236.0);
    const auto run = correct (voice, sr, scaleMode (0, ScaleType::major));
    const auto out = pitchStats (run.output, sr, 0.2, 1.3);
    std::printf ("    out %.4f (target 59)\n", out.median);
    CHECK_NEAR (out.median, 59.0, 0.03);

    // In F major (which has A#/Bb) it goes to A#.
    const auto runF = correct (voice, sr, scaleMode (5, ScaleType::major));
    CHECK_NEAR (pitchStats (runF.output, sr, 0.2, 1.3).median, 58.0, 0.03);
}

TEST (instant_retune_flattens_vibrato)
{
    // +/- 30 cents of vibrato at 5.5 Hz around A3 + 10 cents. It never gets closer to A#
    // than to A, so the target stays on A.
    const auto voice = vowel (sr, 2.0, [] (double t)
                              { return 220.0 * std::exp2 ((0.1 + 0.3 * std::sin (2.0 * 3.14159265 * 5.5 * t)) / 12.0); });

    const auto in = pitchStats (voice, sr, 0.3, 1.8);
    const auto run = correct (voice, sr, scaleMode (0, ScaleType::chromatic, 0.0));
    const auto out = pitchStats (run.output, sr, 0.3, 1.8);
    std::printf ("    vibrato in %.1f cents, out %.1f cents; out median %.3f\n", in.stdevCents, out.stdevCents, out.median);

    CHECK (in.stdevCents > 20.0);
    CHECK (out.stdevCents < 4.0);
    CHECK_NEAR (out.median, 57.0, 0.03);
    CHECK (run.shortfalls == 0);
}

TEST (slow_retune_keeps_vibrato)
{
    // +/- 25 cents around A3 + 20 cents: always nearer A than A#.
    const auto voice = vowel (sr, 3.0, [] (double t)
                              { return 220.0 * std::exp2 ((0.2 + 0.25 * std::sin (2.0 * 3.14159265 * 5.5 * t)) / 12.0); });

    const auto in = pitchStats (voice, sr, 1.0, 2.8);
    const auto run = correct (voice, sr, scaleMode (0, ScaleType::chromatic, 400.0));
    const auto out = pitchStats (run.output, sr, 1.0, 2.8);
    std::printf ("    vibrato in %.1f cents, out %.1f cents; mean in %.3f, out %.3f\n", in.stdevCents, out.stdevCents,
                 in.mean, out.mean);

    CHECK (out.stdevCents > 0.6 * in.stdevCents); // the wobble survives...
    CHECK_NEAR (out.mean, 57.0, 0.06);            // ...but its centre moves onto the note
}

TEST (retune_speed_sets_how_fast_a_note_is_pulled_in)
{
    // A note that starts after a gap, 40 cents sharp. With a 200 ms retune, the pitch is
    // still well off target early on, and on target later.
    std::vector<float> signal ((size_t) (0.3 * sr), 0.0f);
    const auto note = steadyVowel (sr, 1.5, 220.0 * std::exp2 (0.4 / 12.0));
    signal.insert (signal.end(), note.begin(), note.end());

    const auto run = correct (signal, sr, scaleMode (0, ScaleType::chromatic, 200.0));
    const auto early = pitchStats (run.output, sr, 0.36, 0.40);
    const auto late = pitchStats (run.output, sr, 1.0, 1.7);
    std::printf ("    early %.3f, late %.3f\n", early.median, late.median);

    CHECK (early.median > 57.15);
    CHECK_NEAR (late.median, 57.0, 0.03);
}

//==============================================================================
TEST (midi_mode_follows_the_held_note)
{
    // Sing A3 + 39 cents. Hold C4 for the first half, then G3.
    const auto voice = steadyVowel (sr, 3.0, 225.0);
    const auto half = (std::int64_t) (1.5 * sr);
    const auto run = correct (voice, sr, midiMode(), { { 0, 60, true }, { half, 60, false }, { half, 55, true } });

    const auto first = pitchStats (run.output, sr, 0.2, 1.4);
    const auto second = pitchStats (run.output, sr, 1.7, 2.8);
    std::printf ("    first %.3f (target 60), second %.3f (target 55)\n", first.median, second.median);

    CHECK_NEAR (first.median, 60.0, 0.03);
    CHECK_NEAR (second.median, 55.0, 0.03);
    CHECK (run.shortfalls == 0);
}

TEST (midi_mode_uses_the_last_note_of_several)
{
    const auto voice = steadyVowel (sr, 2.0, 225.0);
    const auto run = correct (voice, sr, midiMode(), { { 0, 60, true }, { 1000, 62, true } });
    CHECK_NEAR (pitchStats (run.output, sr, 0.2, 1.8).median, 62.0, 0.03);

    // Releasing the top note falls back to the one still held.
    const auto run2 = correct (voice, sr, midiMode(), { { 0, 60, true }, { 1000, 62, true }, { 2000, 62, false } });
    CHECK_NEAR (pitchStats (run2.output, sr, 0.2, 1.8).median, 60.0, 0.03);
}

TEST (midi_mode_without_notes_leaves_pitch_alone)
{
    const auto voice = steadyVowel (sr, 1.5, 225.0);
    const auto run = correct (voice, sr, midiMode());
    CHECK_NEAR (pitchStats (run.output, sr, 0.2, 1.3).median, frequencyToPitch (225.0), 0.02);
}

TEST (large_shifts_stay_accurate)
{
    const auto voice = steadyVowel (sr, 1.5, 220.0);

    for (int note : { 45, 50, 53, 62, 64, 69 }) // -12 .. +12 semitones from A3
    {
        const auto run = correct (voice, sr, midiMode(), { { 0, note, true } });
        const auto out = pitchStats (run.output, sr, 0.2, 1.3);
        std::printf ("    A3 -> %d: out %.3f\n", note, out.median);
        CHECK_NEAR (out.median, note, 0.05);
        CHECK (run.shortfalls == 0);
    }
}

namespace
{
/** The amplitude-weighted average frequency of a periodic signal's harmonics below 4 kHz.
    It follows the formants, so it's a rough measure of where the vowel's energy sits. */
double harmonicCentroid (const std::vector<float>& signal, double hz, size_t from, size_t length)
{
    double weighted = 0.0, total = 0.0;

    for (int k = 1; k * hz < 4000.0; ++k)
    {
        double re = 0.0, im = 0.0;
        const double w = 2.0 * 3.14159265358979 * k * hz / sr;

        for (size_t n = 0; n < length; ++n)
        {
            const double hann = 0.5 - 0.5 * std::cos (2.0 * 3.14159265358979 * (double) n / (double) length);
            re += signal[from + n] * hann * std::cos (w * (double) n);
            im += signal[from + n] * hann * std::sin (w * (double) n);
        }

        const double amplitude = std::sqrt (re * re + im * im);
        weighted += amplitude * k * hz;
        total += amplitude;
    }

    return weighted / total;
}
} // namespace

TEST (formants_stay_put_when_shifting)
{
    // Simply speeding up a recording by 5 semitones would move the formants up by 33%
    // (the chipmunk effect). PSOLA keeps them where they were.
    const auto voice = steadyVowel (sr, 1.5, 220.0);
    const auto from = (size_t) (0.4 * sr), length = (size_t) (0.8 * sr);
    const double before = harmonicCentroid (voice, 220.0, from, length);

    for (int note : { 52, 62 }) // A3 -/+ 5 semitones
    {
        const auto run = correct (voice, sr, midiMode(), { { 0, note, true } });
        const double after = harmonicCentroid (run.output, pitchToFrequency (note), from, length);
        std::printf ("    A3 -> %d: centroid %.0f Hz -> %.0f Hz (ratio %.3f; resampling would give %.3f)\n", note, before,
                     after, after / before, std::exp2 ((note - 57) / 12.0));
        CHECK (after / before > 0.9 && after / before < 1.1);
    }
}

TEST (notes_ending_abruptly_give_no_wild_pitches)
{
    // Notes cut off dead, with silence and hiss after them. The detector takes a few
    // milliseconds to notice; no grain in between should report a made-up pitch.
    std::vector<float> signal;

    for (double hz : { 110.0, 196.0, 330.0 })
    {
        const auto note = steadyVowel (sr, 0.4, hz);
        signal.insert (signal.end(), note.begin(), note.end());
        signal.resize (signal.size() + (size_t) (0.15 * sr), 0.0f);
        const auto hiss = noise (sr, 0.1, 0.02, 5);
        signal.insert (signal.end(), hiss.begin(), hiss.end());
    }

    struct Collector : PitchCorrector::Listener
    {
        std::vector<PitchCorrector::GrainInfo> grains;
        void grainProduced (const PitchCorrector::GrainInfo& g) override { grains.push_back (g); }
    } collector;

    PitchCorrector corrector;
    corrector.prepare (sr, 1);
    corrector.setSettings (scaleMode (0, ScaleType::chromatic));
    corrector.setListener (&collector);

    std::vector<float> out (signal.size());
    const float* in[] { signal.data() };
    float* o[] { out.data() };
    corrector.process (in, o, 1, (int) signal.size());

    int wild = 0;
    const double notes[] { frequencyToPitch (110.0), frequencyToPitch (196.0), frequencyToPitch (330.0) };

    for (const auto& g : collector.grains)
    {
        if (g.inputPitch < 0.0f)
            continue;

        bool near = false;

        for (double n : notes)
            near = near || std::abs (g.inputPitch - n) < 0.5;

        wild += near ? 0 : 1;
    }

    std::printf ("    %d of %zu grains had a pitch that wasn't sung\n", wild, collector.grains.size());
    CHECK (wild == 0);
}

//==============================================================================
TEST (survives_anything_at_any_rate_and_block_size)
{
    for (double rate : { 22050.0, 44100.0, 48000.0, 88200.0, 96000.0, 192000.0 })
    {
        // A slide across the whole vocal range, noise, silence, a full-scale square wave
        // and a burst of DC.
        auto signal = vowel (rate, 2.0, [] (double t) { return 75.0 * std::pow (1000.0 / 75.0, t / 2.0); }, 0.9);
        const auto hiss = noise (rate, 0.3, 0.8, 7);
        signal.insert (signal.end(), hiss.begin(), hiss.end());
        signal.resize (signal.size() + (size_t) (0.2 * rate), 0.0f);

        for (int i = 0; i < (int) (0.3 * rate); ++i)
            signal.push_back ((i / (int) (rate / 200.0)) % 2 == 0 ? 1.0f : -1.0f);

        signal.resize (signal.size() + (size_t) (0.1 * rate), 0.5f);
        signal.resize (signal.size() + (size_t) (0.1 * rate), 0.0f);

        for (int block : { 1, 64, 441, 4096 })
        {
            for (int voice = 0; voice < (int) VoiceType::count; ++voice)
            {
                auto settings = midiMode (20.0);
                settings.voice = (VoiceType) voice;
                const auto run = correct (signal, rate, settings, { { 0, 60, true } }, block);

                bool finite = true;
                double peak = 0.0;

                for (auto s : run.output)
                {
                    finite = finite && std::isfinite (s);
                    peak = std::max (peak, (double) std::abs (s));
                }

                CHECK (finite);
                CHECK (peak < 3.0);
                CHECK (run.shortfalls == 0);

                if (run.shortfalls != 0 || ! finite || peak >= 3.0)
                    std::printf ("    at %.0f Hz, block %d, voice %d: peak %.2f, shortfalls %lld\n", rate, block, voice,
                                 peak, (long long) run.shortfalls);
            }
        }
    }
}

TEST (stereo_channels_get_the_same_correction)
{
    const auto voice = steadyVowel (sr, 1.0, 225.0);
    std::vector<float> left (voice), right (voice);

    for (auto& s : right)
        s *= 0.5f;

    PitchCorrector corrector;
    corrector.prepare (sr, 2);
    corrector.setSettings (scaleMode (0, ScaleType::chromatic));

    float* channels[] { left.data(), right.data() };
    corrector.process (channels, channels, 2, (int) left.size()); // in place

    double maxDifference = 0.0;

    for (size_t i = 0; i < left.size(); ++i)
        maxDifference = std::max (maxDifference, (double) std::abs (0.5f * left[i] - right[i]));

    CHECK (maxDifference < 1.0e-5);
}

TEST (reports_grains_for_the_display)
{
    struct Collector : PitchCorrector::Listener
    {
        std::vector<PitchCorrector::GrainInfo> grains;
        void grainProduced (const PitchCorrector::GrainInfo& g) override { grains.push_back (g); }
    } collector;

    PitchCorrector corrector;
    corrector.prepare (sr, 1);
    corrector.setSettings (scaleMode (0, ScaleType::chromatic));
    corrector.setListener (&collector);

    auto voice = steadyVowel (sr, 1.0, 225.0);
    const float* in[] { voice.data() };
    std::vector<float> out (voice.size());
    float* o[] { out.data() };
    corrector.process (in, o, 1, (int) voice.size());

    int voiced = 0;

    for (const auto& g : collector.grains)
    {
        if (g.inputPitch < 0.0f || g.outputSample < (std::int64_t) (0.5 * sr))
            continue;

        ++voiced;
        CHECK (g.targetNote == 57);
        CHECK_NEAR (g.inputPitch, frequencyToPitch (225.0), 0.02);
        CHECK_NEAR (g.outputPitch, 57.0, 0.001);
    }

    CHECK (voiced > 50);
}

//==============================================================================
int main (int argc, char** argv)
{
    const char* filter = argc > 1 ? argv[1] : nullptr;
    int run = 0;

    for (const auto& test : registry())
    {
        if (filter != nullptr && std::strstr (test.name, filter) == nullptr)
            continue;

        const int failuresBefore = failures;
        std::printf ("%s\n", test.name);
        test.run();
        std::printf ("  %s\n", failures == failuresBefore ? "ok" : "FAILED");
        ++run;
    }

    std::printf ("\n%d tests, %d checks, %d failures\n", run, checks, failures);
    return failures == 0 ? 0 : 1;
}
