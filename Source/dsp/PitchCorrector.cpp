// SPDX-License-Identifier: AGPL-3.0-or-later

#include "PitchCorrector.h"

#include <algorithm>
#include <cmath>

namespace tuneafish
{
namespace
{
constexpr double pi = 3.14159265358979323846;

constexpr double unvoicedSpacingSeconds = 0.005;

// A voiced mark is searched for within this fraction of a period of where we expect it.
// Wide enough to follow fast slides between detector updates.
constexpr double searchFraction = 0.25;

// Marks may be up to this much further apart than the longest detectable period.
constexpr double spacingAllowance = 1.125;

// In scale mode, stay on the current note until another is this much closer (semitones).
constexpr double hysteresis = 0.1;

// A period counts as voiced only if it correlates with the one before at least this well.
constexpr double minPeriodicity = 0.5;

constexpr size_t markCapacity = 1024;

struct VoiceRange
{
    double minHz, maxHz;
};

constexpr VoiceRange voiceRanges[] {
    { 150.0, 1100.0 }, // soprano
    { 90.0, 750.0 },   // alto/tenor
    { 70.0, 450.0 },   // low
};

std::int64_t nextPowerOfTwo (std::int64_t n)
{
    std::int64_t p = 1;

    while (p < n)
        p <<= 1;

    return p;
}
} // namespace

void PitchCorrector::prepare (double newSampleRate, int numChannels)
{
    sampleRate = newSampleRate;
    channels = std::clamp (numChannels, 1, maxChannels);

    maxDetectPeriod = (int) std::ceil (sampleRate / lowestHz);
    maxSpacing = (int) std::ceil (maxDetectPeriod * spacingAllowance);
    unvoicedSpacing = std::max (1, (int) std::lround (sampleRate * unvoicedSpacingSeconds));

    // When a grain is added (at most maxSpacing before its centre), we need the marks up to
    // half a spacing past its centre, and the input up to a full spacing past its mark.
    // Marks trail the newest input by up to one period. See addGrain().
    latency = (int) std::ceil (2.5 * maxSpacing) + 8;

    detector.prepare (sampleRate, lowestHz);

    const auto inputSize = nextPowerOfTwo (latency + 4 * (std::int64_t) maxSpacing + 64);
    const auto outputSize = nextPowerOfTwo (4 * (std::int64_t) maxSpacing + 64);
    inputMask = inputSize - 1;
    outputMask = outputSize - 1;

    for (int ch = 0; ch < maxChannels; ++ch)
    {
        inputRing[(size_t) ch].assign ((size_t) inputSize, 0.0f);
        outputRing[(size_t) ch].assign ((size_t) outputSize, 0.0f);
    }

    monoRing.assign ((size_t) inputSize, 0.0f);
    marks.assign (markCapacity, {});
    markMask = (std::int64_t) markCapacity - 1;
    window.assign ((size_t) (2 * maxSpacing + 4), 0.0f);

    setSettings (settings);
    reset();
}

void PitchCorrector::reset()
{
    detector.reset();
    detectedPeriod = 0.0;
    detectedVoiced = false;

    for (auto& ring : inputRing)
        std::fill (ring.begin(), ring.end(), 0.0f);

    for (auto& ring : outputRing)
        std::fill (ring.begin(), ring.end(), 0.0f);

    std::fill (monoRing.begin(), monoRing.end(), 0.0f);
    received = 0;

    // Start with an unvoiced mark at the very first sample, and a grain centred on it.
    markCount = 0;
    pushMark ({ 0, 0.0, 0.0, false });
    searchHint = 0;
    nextGrainExact = 0.0;
    lastGrainCentre = -unvoicedSpacing;

    correction = 0.0;
    scaleNote = -1;
    noteEventsRead = noteEventsWritten = 0;
    numHeld = 0;
    lookaheadShortfalls = 0;
}

void PitchCorrector::setSettings (const CorrectorSettings& newSettings)
{
    settings = newSettings;
    settings.key = ((settings.key % 12) + 12) % 12;
    settings.amount = std::clamp (settings.amount, 0.0, 1.0);
    settings.retuneMs = std::max (0.0, settings.retuneMs);

    const auto& range = voiceRanges[(size_t) std::clamp ((int) settings.voice, 0, (int) VoiceType::count - 1)];
    detector.setRange (range.minHz, range.maxHz);
}

//==============================================================================
void PitchCorrector::queueNoteEvent (NoteEvent event)
{
    // If the queue is full, apply the oldest event early rather than lose it.
    if (noteEventsWritten - noteEventsRead == noteEvents.size())
        applyNoteEventsUpTo (noteEvents[noteEventsRead % noteEvents.size()].time);

    event.time = received;
    noteEvents[noteEventsWritten % noteEvents.size()] = event;
    ++noteEventsWritten;
}

void PitchCorrector::noteOn (int note) { queueNoteEvent ({ 0, note, NoteEvent::on }); }
void PitchCorrector::noteOff (int note) { queueNoteEvent ({ 0, note, NoteEvent::off }); }
void PitchCorrector::allNotesOff() { queueNoteEvent ({ 0, 0, NoteEvent::allOff }); }

void PitchCorrector::applyNoteEventsUpTo (std::int64_t time)
{
    while (noteEventsRead != noteEventsWritten)
    {
        const auto& event = noteEvents[noteEventsRead % noteEvents.size()];

        if (event.time > time)
            break;

        ++noteEventsRead;

        if (event.type == NoteEvent::allOff)
        {
            numHeld = 0;
            continue;
        }

        // Remove the note if it's already held, then (for note-ons) put it on top.
        const auto end = held.begin() + numHeld;
        numHeld = (int) (std::remove (held.begin(), end, event.note) - held.begin());

        if (event.type == NoteEvent::on)
        {
            if (numHeld == (int) held.size())
            {
                std::move (held.begin() + 1, held.end(), held.begin());
                --numHeld;
            }

            held[(size_t) numHeld++] = event.note;
        }
    }
}

//==============================================================================
void PitchCorrector::process (const float* const* input, float* const* output, int numChannels, int numSamples)
{
    const int used = std::clamp (numChannels, 1, channels);
    const float monoScale = 1.0f / (float) used;

    for (int i = 0; i < numSamples; ++i)
    {
        const auto slot = (size_t) (received & inputMask);
        float mono = 0.0f;

        for (int ch = 0; ch < used; ++ch)
        {
            const float x = input[ch][i];
            inputRing[(size_t) ch][slot] = x;
            mono += x;
        }

        mono *= monoScale;
        monoRing[slot] = mono;

        if (detector.push (mono))
        {
            const auto& estimate = detector.estimate();
            detectedVoiced = estimate.voiced;

            if (estimate.voiced)
                detectedPeriod = std::clamp (estimate.period, 2.0, (double) maxDetectPeriod);
        }

        ++received;
        placeMarks();

        // The output sample we emit now. Add every grain that starts at or before it.
        const std::int64_t now = received - 1 - latency;

        for (;;)
        {
            const auto centre = (std::int64_t) std::llround (nextGrainExact);
            const auto start = centre - std::min<std::int64_t> (centre - lastGrainCentre, maxSpacing);

            if (start > now)
                break;

            addGrain (now);
        }

        const auto outSlot = (size_t) (now & outputMask);

        for (int ch = 0; ch < used; ++ch)
        {
            output[ch][i] = outputRing[(size_t) ch][outSlot];
            outputRing[(size_t) ch][outSlot] = 0.0f;
        }

        for (int ch = used; ch < numChannels; ++ch)
            output[ch][i] = output[0][i];
    }
}

//==============================================================================
void PitchCorrector::pushMark (const Mark& mark)
{
    marks[(size_t) (markCount & markMask)] = mark;
    ++markCount;
}

void PitchCorrector::placeMarks()
{
    const std::int64_t newest = received - 1;

    for (;;)
    {
        const Mark last = markAt (markCount - 1);

        if (! detectedVoiced)
        {
            const auto position = last.position + unvoicedSpacing;

            if (position > newest)
                return;

            pushMark ({ position, (double) position, 0.0, false });
            continue;
        }

        const double period = detectedPeriod;

        if (! last.voiced)
        {
            // Voicing just started. Lock onto the strongest peak about a period on.
            const auto from = last.position + std::max (1, (int) (0.5 * period));
            const auto to = last.position + std::min ((int) std::ceil (1.5 * period), maxSpacing);

            if (to > newest)
                return;

            auto best = from;

            for (auto p = from + 1; p <= to; ++p)
                if (monoRing[(size_t) (p & inputMask)] > monoRing[(size_t) (best & inputMask)])
                    best = p;

            const int lag = std::max (2, (int) std::lround (period));
            pushMark ({ best, (double) best, period, periodicity (best - lag, lag, lag) >= minPeriodicity });
            continue;
        }

        // Find the lag around one period that best lines up the waveform with the
        // previous mark, so every mark sits at the same point in its cycle.
        const int reach = std::max (1, (int) (period * searchFraction));
        const int longest = std::min ((int) std::ceil (period) + reach, maxSpacing);
        const int shortest = std::clamp ((int) std::floor (period) - reach, 2, longest);
        const int length = std::max (2, (int) std::lround (period));

        if (last.position + longest > newest)
            return;

        const double lag = bestLag (last.position, shortest, longest, length);
        const double exact = last.exact + lag;
        const auto position = std::clamp ((std::int64_t) std::llround (exact), last.position + 1, newest);

        // The detector reacts a few milliseconds late when a voice stops. Check that this
        // period really does repeat the last one, so we don't invent pitches in the decay.
        const bool periodic = periodicity (last.position, (int) (position - last.position), length) >= minPeriodicity;

        pushMark ({ position, exact, period, periodic });
    }
}

double PitchCorrector::periodicity (std::int64_t reference, int lag, int length) const
{
    // Normalised correlation between the stretch ending at the reference and the stretch
    // ending one lag later: 1 when they're identical, around 0 for noise or silence.
    double xx = 0.0, yy = 0.0, xy = 0.0;

    for (int i = 0; i < length; ++i)
    {
        const double a = monoRing[(size_t) ((reference - i) & inputMask)];
        const double b = monoRing[(size_t) ((reference + lag - i) & inputMask)];
        xx += a * a;
        yy += b * b;
        xy += a * b;
    }

    return xx > 1.0e-12 && yy > 1.0e-12 ? xy / std::sqrt (xx * yy) : 0.0;
}

double PitchCorrector::correlationScore (std::int64_t reference, int lag, int length, int stride) const
{
    // Compares the stretch of signal ending at the reference with the stretch ending one
    // lag later. Only looks backwards, so it never needs input beyond reference + lag.
    double xy = 0.0, yy = 0.0;

    for (int i = 0; i < length; i += stride)
    {
        const double a = monoRing[(size_t) ((reference - i) & inputMask)];
        const double b = monoRing[(size_t) ((reference + lag - i) & inputMask)];
        xy += a * b;
        yy += b * b;
    }

    return yy > 1.0e-20 ? xy / std::sqrt (yy) : 0.0;
}

double PitchCorrector::bestLag (std::int64_t reference, int shortest, int longest, int length) const
{
    // Coarse search first, on a thinned-out signal, then refine around the winner.
    const int step = std::clamp (length / 64, 1, 64);
    int best = shortest;
    double bestScore = -1.0e30;

    for (int lag = shortest; lag <= longest; lag += step)
    {
        const double score = correlationScore (reference, lag, length, step);

        if (score > bestScore)
        {
            bestScore = score;
            best = lag;
        }
    }

    const int from = std::max (shortest, best - step);
    const int to = std::min (longest, best + step);
    bestScore = -1.0e30;

    for (int lag = from; lag <= to; ++lag)
    {
        const double score = correlationScore (reference, lag, length, 1);

        if (score > bestScore)
        {
            bestScore = score;
            best = lag;
        }
    }

    double result = best;

    if (best > shortest && best < longest)
    {
        const double a = correlationScore (reference, best - 1, length, 1);
        const double c = correlationScore (reference, best + 1, length, 1);
        const double denominator = a - 2.0 * bestScore + c;

        if (denominator < 0.0)
            result += std::clamp (0.5 * (a - c) / denominator, -0.5, 0.5);
    }

    return result;
}

std::int64_t PitchCorrector::nearestMark (double position)
{
    const auto oldest = std::max<std::int64_t> (0, markCount - (std::int64_t) markCapacity + 2);
    auto j = std::clamp (searchHint, oldest, markCount - 1);

    while (j > oldest && markAt (j).exact > position)
        --j;

    while (j + 1 < markCount && std::abs (markAt (j + 1).exact - position) <= std::abs (markAt (j).exact - position))
        ++j;

    searchHint = j;
    return j;
}

//==============================================================================
void PitchCorrector::relaxCorrection (double seconds)
{
    const double alpha = settings.retuneMs < 0.5 ? 1.0 : 1.0 - std::exp (-seconds * 3000.0 / settings.retuneMs);
    correction -= alpha * correction;
    scaleNote = -1;
}

double PitchCorrector::ratioForVoicedGrain (double inputPitch, double seconds, GrainInfo& info)
{
    int target = -1;

    if (settings.midiMode)
    {
        target = currentHeldNote();
    }
    else
    {
        const int candidate = nearestNoteInScale (inputPitch, settings.key, settings.scale);
        const bool keepCurrent = scaleNote >= 0 && scaleNote != candidate
                                 && isInScale (scaleNote, settings.key, settings.scale)
                                 && std::abs (inputPitch - scaleNote) <= std::abs (inputPitch - candidate) + hysteresis;
        target = keepCurrent ? scaleNote : candidate;
        scaleNote = target;
    }

    const double wanted = target >= 0 ? std::clamp (target - inputPitch, -maxShiftSemitones, maxShiftSemitones) : 0.0;

    // The retune speed is roughly the time to get 95% of the way to the target (three time
    // constants). At zero the correction is immediate, which gives the hard, robotic effect.
    const double alpha = settings.retuneMs < 0.5 ? 1.0 : 1.0 - std::exp (-seconds * 3000.0 / settings.retuneMs);
    correction += alpha * (wanted - correction);

    const double shift = settings.amount * correction;
    info.inputPitch = (float) inputPitch;
    info.outputPitch = (float) (inputPitch + shift);
    info.targetNote = target;
    return std::exp2 (shift / 12.0);
}

void PitchCorrector::addGrain (std::int64_t now)
{
    const std::int64_t newest = received - 1;
    const double centreExact = nextGrainExact;
    const auto centre = (std::int64_t) std::llround (centreExact);

    const auto j = nearestMark (centreExact);
    const Mark mark = markAt (j);
    const auto oldest = markCount - (std::int64_t) markCapacity + 1;

    // We need the marks either side of this one. The latency is chosen so the next mark has
    // always been placed by now; see prepare().
    const bool hasNext = j + 1 < markCount;
    const bool hasPrevious = j >= 1 && j - 1 >= oldest;

    if (! hasNext)
        ++lookaheadShortfalls;

    const Mark previous = hasPrevious ? markAt (j - 1) : mark;
    const Mark next = hasNext ? markAt (j + 1) : mark;
    const double backward = hasPrevious ? std::clamp (mark.exact - previous.exact, 1.0, (double) maxSpacing)
                                        : (double) unvoicedSpacing;
    const double forward = hasNext ? std::clamp (next.exact - mark.exact, 1.0, (double) maxSpacing) : backward;

    applyNoteEventsUpTo (mark.position);

    GrainInfo info;
    info.outputSample = centre + latency;
    double ratio = 1.0;

    if (mark.voiced)
    {
        // The local period: the distance to a neighbouring voiced mark.
        const double period = hasNext && next.voiced ? forward : (hasPrevious && previous.voiced ? backward : mark.period);
        ratio = ratioForVoicedGrain (frequencyToPitch (sampleRate / period), forward / sampleRate, info);
    }
    else
    {
        relaxCorrection (forward / sampleRate);
        info.targetNote = settings.midiMode ? currentHeldNote() : -1;
    }

    // The next grain goes one analysis period later, scaled by the pitch ratio. Using the
    // period that follows this mark means that with no correction, the grains land exactly
    // where their marks are (plus a constant offset), however the pitch is moving.
    const double followingExact = centreExact + forward / ratio;
    const auto followingCentre = (std::int64_t) std::llround (followingExact);

    // The grain fades in from the previous grain's centre and out to the next one's, so
    // neighbouring grains always crossfade to exactly 1. When lowering the pitch spreads the
    // grains further apart than a period, the fades are capped at about a period, leaving
    // short gaps rather than smearing neighbouring periods into each other.
    const auto cap = (std::int64_t) std::min (std::ceil (std::max (forward, backward)) + 1.0, (double) maxSpacing);
    auto left = std::min (centre - lastGrainCentre, cap);
    auto right = std::min (followingCentre - centre, cap);

    if (left > centre - now)
    {
        left = centre - now;
        ++lookaheadShortfalls;
    }

    // The grain is moved from the mark to the synthesis position with sub-sample precision,
    // so its pulse lands exactly where it should. Rounding to whole samples would add
    // jitter to the output pitch, which is audible on high notes.
    const double offset = centreExact - mark.exact;
    const double readStart = (double) (centre - left) - offset;
    const auto base = (std::int64_t) std::floor (readStart);
    const auto t = (float) (readStart - (double) base);

    // Catmull-Rom interpolation weights for the four samples around each read position.
    const float w0 = t * (-0.5f + t * (1.0f - 0.5f * t));
    const float w1 = 1.0f + t * t * (-2.5f + 1.5f * t);
    const float w2 = t * (0.5f + t * (2.0f - 1.5f * t));
    const float w3 = t * t * (-0.5f + 0.5f * t);

    const auto available = newest - (base + left + 2); // the last read is base + length + 2

    if (right > available)
    {
        right = available;
        ++lookaheadShortfalls;
    }

    left = std::max<std::int64_t> (left, 0);
    right = std::max<std::int64_t> (right, 0);

    // Window: a raised-cosine rise over `left` samples, then a fall over `right` samples.
    float* w = window.data();

    for (std::int64_t i = 0; i < left; ++i)
        w[i] = (float) (0.5 - 0.5 * std::cos (pi * (double) i / (double) left));

    for (std::int64_t i = 0; i < right; ++i)
        w[left + i] = (float) (0.5 + 0.5 * std::cos (pi * (double) i / (double) right));

    const auto length = left + right;
    const auto outStart = centre - left;

    for (int ch = 0; ch < channels; ++ch)
    {
        const float* in = inputRing[(size_t) ch].data();
        float* out = outputRing[(size_t) ch].data();

        for (std::int64_t i = 0; i < length; ++i)
        {
            const auto k = base + i;
            const float sample = w0 * in[(k - 1) & inputMask] + w1 * in[k & inputMask]
                                 + w2 * in[(k + 1) & inputMask] + w3 * in[(k + 2) & inputMask];
            out[(outStart + i) & outputMask] += w[i] * sample;
        }
    }

    lastGrainCentre = centre;
    nextGrainExact = followingExact;

    if (listener != nullptr)
        listener->grainProduced (info);
}
} // namespace tuneafish
