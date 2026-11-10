// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "PitchDetector.h"
#include "Scale.h"

#include <array>
#include <cstdint>
#include <vector>

namespace tuneafish
{
enum class VoiceType
{
    soprano,   // 150 - 1100 Hz
    altoTenor, // 90 - 750 Hz
    low,       // 70 - 450 Hz
    count
};

inline constexpr std::array<const char*, (size_t) VoiceType::count> voiceNames { "Soprano", "Alto/Tenor", "Low" };

struct CorrectorSettings
{
    bool midiMode = false; // false: snap to the nearest note in key and scale; true: snap to the held MIDI note
    int key = 0;           // 0 = C ... 11 = B
    ScaleType scale = ScaleType::chromatic;
    double retuneMs = 50.0; // how long it takes to pull a note into tune, 0 = instantly
    double amount = 1.0;    // 0 = no correction, 1 = full correction
    VoiceType voice = VoiceType::altoTenor;
};

/** Pitch correction for a single voice.

    How it works:
    - A PitchDetector finds the pitch of the input, and whether it's voiced at all.
    - Pitch marks are placed one period apart through the voiced parts of the input, each
      lined up with the previous one by cross-correlation. Unvoiced parts (breaths,
      consonants, silence) get marks at a fixed spacing.
    - The output is built from two-period grains cut out around the marks (PSOLA:
      pitch-synchronous overlap-add). Spacing the grains closer together raises the pitch;
      spacing them further apart lowers it. Each grain keeps the shape of the original
      period, so the voice's formants (its character) stay where they were.

    The output is delayed by getLatencySamples(); hosts compensate for that. */
class PitchCorrector
{
public:
    static constexpr double lowestHz = 70.0; // the lowest pitch any voice type can detect
    static constexpr int maxChannels = 2;
    static constexpr double maxShiftSemitones = 12.0;

    /** Reported on the audio thread for each grain, for displays. */
    struct GrainInfo
    {
        std::int64_t outputSample = 0; // where the grain's centre lands in the output stream
        float inputPitch = -1.0f;      // detected pitch in MIDI semitones, < 0 when unvoiced
        float outputPitch = -1.0f;     // pitch after correction, < 0 when unvoiced
        int targetNote = -1;           // the note being tuned towards, < 0 when none
    };

    struct Listener
    {
        virtual ~Listener() = default;
        virtual void grainProduced (const GrainInfo&) = 0;
    };

    /** Allocates. Call before processing, and whenever the sample rate changes. */
    void prepare (double sampleRate, int numChannels);
    void reset();

    int getLatencySamples() const { return latency; }

    /** Safe to call from the audio thread between process() calls, and before prepare(). */
    void setSettings (const CorrectorSettings&);
    void setListener (Listener* l) { listener = l; }

    /** While bypassed, the output is the input delayed by the latency, so it stays in line
        with everything else. Analysis carries on, so the correction picks up seamlessly
        when the bypass ends. Switching crossfades over a few milliseconds. */
    void setBypassed (bool shouldBypass) { bypassed = shouldBypass; }

    /** MIDI notes take effect at the next input sample, so call these between process()
        calls at the right sample positions. A note stays held until it has had as many
        note-offs as note-ons, so the order of a note-off and note-on at the same sample
        doesn't matter. */
    void noteOn (int note);
    void noteOff (int note);
    void allNotesOff();

    /** input and output may point to the same buffers. */
    void process (const float* const* input, float* const* output, int numChannels, int numSamples);

    /** Times the lookahead wasn't enough to build a grain properly. Should always be 0. */
    std::int64_t getLookaheadShortfalls() const { return lookaheadShortfalls; }

private:
    struct Mark
    {
        std::int64_t position = 0; // sample index in the input stream
        double exact = 0.0;        // position with sub-sample precision, for measuring periods
        double period = 0.0;       // the detector's period estimate when the mark was placed
        bool voiced = false;
    };

    struct NoteEvent
    {
        std::int64_t time = 0;
        int note = 0;
        enum Type : std::uint8_t { on, off, allOff } type = on;
    };

    void placeMarks();
    void pushMark (const Mark&);
    const Mark& markAt (std::int64_t index) const { return marks[(size_t) (index & markMask)]; }
    std::int64_t nearestMark (double position);
    double bestLag (std::int64_t reference, int minLagSamples, int maxLagSamples, int window) const;
    double correlationScore (std::int64_t reference, int lag, int window, int stride) const;
    double periodicity (std::int64_t reference, int lag, int length) const;

    void addGrain (std::int64_t outputTime);
    double ratioForVoicedGrain (double inputPitch, double seconds, GrainInfo&);
    void relaxCorrection (double seconds);
    double retuneAlpha (double seconds) const;
    void applyNoteEventsUpTo (std::int64_t time);
    int currentHeldNote() const { return numHeld > 0 ? held[(size_t) numHeld - 1] : -1; }
    void queueNoteEvent (NoteEvent);

    double sampleRate = 44100.0;
    int channels = 1;
    int maxSpacing = 0;      // longest allowed distance between marks, in samples
    int maxDetectPeriod = 0; // longest period the detector can report
    int unvoicedSpacing = 0;
    int latency = 0;

    CorrectorSettings settings;
    PitchDetector detector;
    double detectedPeriod = 0.0;
    bool detectedVoiced = false;

    // Input history, per channel and as a mono mix for analysis. Indexed by sample number.
    std::array<std::vector<float>, maxChannels> inputRing;
    std::vector<float> monoRing;
    std::int64_t inputMask = 0;
    std::int64_t received = 0; // input samples so far; the newest is received - 1

    // Overlap-add accumulator, indexed by output time (input time minus latency).
    std::array<std::vector<float>, maxChannels> outputRing;
    std::int64_t outputMask = 0;

    std::vector<Mark> marks;
    std::int64_t markMask = 0;
    std::int64_t markCount = 0;
    std::int64_t searchHint = 0;

    double nextGrainExact = 0.0;  // centre of the next grain to synthesise (input time)
    std::int64_t lastGrainCentre = 0;
    std::vector<float> window;    // scratch space for one grain's window

    // Correction state.
    double correction = 0.0;     // semitones, smoothed according to the retune speed
    int scaleNote = -1;          // current target in scale mode, for hysteresis
    double unvoicedSeconds = 0.0; // how long since the last voiced grain

    std::array<NoteEvent, 256> noteEvents {};
    size_t noteEventsRead = 0, noteEventsWritten = 0;
    std::array<int, 16> held {}; // held notes, oldest first
    int numHeld = 0;
    std::array<std::uint8_t, 128> heldCount {}; // note-ons not yet matched by a note-off, per note

    bool bypassed = false;
    double bypassMix = 0.0;  // 0 = corrected, 1 = dry; ramps towards `bypassed`
    double bypassStep = 0.0; // per sample

    Listener* listener = nullptr;
    std::int64_t lookaheadShortfalls = 0;
};
} // namespace tuneafish
