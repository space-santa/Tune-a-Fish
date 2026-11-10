// SPDX-License-Identifier: AGPL-3.0-or-later

// tune-a-fish: run an audio file through the pitch corrector, or print its pitch.

#include "dsp/PitchCorrector.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <algorithm>
#include <cstdio>
#include <optional>
#include <vector>

namespace
{
using namespace tuneafish;

constexpr const char* usage = R"(Usage:
  tune-a-fish <input> <output.wav> [options]   tune a file
  tune-a-fish <input> --pitch [options]        print the pitch every 10 ms

Options:
  --key <note>        key for scale mode: C, C#, Db, D ... B (default C)
  --scale <name>      chromatic, major, minor, harmonic-minor, major-pentatonic,
                      minor-pentatonic, blues (default chromatic)
  --retune <ms>       retune speed in milliseconds, 0 = instant (default 50)
  --amount <percent>  how much to correct, 0-100 (default 100)
  --voice <type>      soprano, alto-tenor, low (default alto-tenor)
  --note <note>       MIDI mode: hold this note for the whole file, e.g. A2 or 57
  --midi <file.mid>   MIDI mode: take the notes from a MIDI file

Note names use Ableton Live's convention, where middle C (MIDI 60) is C3.
The output is a 24-bit WAV at the input's sample rate, lined up with the input.
)";

std::optional<int> parsePitchClass (const juce::String& text, int& charsUsed)
{
    const auto t = text.trim().toUpperCase();

    if (t.isEmpty() || t[0] < 'A' || t[0] > 'G')
        return std::nullopt;

    constexpr int semitoneOfLetter[] { 9, 11, 0, 2, 4, 5, 7 }; // A B C D E F G
    int semitone = semitoneOfLetter[t[0] - 'A'];
    charsUsed = 1;

    if (t.length() > 1 && t[1] == '#')      { ++semitone; ++charsUsed; }
    else if (t.length() > 1 && t[1] == 'B') { --semitone; ++charsUsed; } // a flat ("Bb" is upper-cased to "BB")

    return semitone;
}

/** "57", "A2", "F#3" or "Bb1". Middle C is C3. */
std::optional<int> parseNote (const juce::String& text)
{
    const auto t = text.trim();

    if (t.containsOnly ("0123456789"))
        return t.isEmpty() ? std::nullopt : std::optional<int> (juce::jlimit (0, 127, t.getIntValue()));

    int used = 0;
    const auto semitone = parsePitchClass (t, used);
    const auto octave = t.substring (used);

    if (! semitone || octave.isEmpty() || ! octave.containsOnly ("-0123456789"))
        return std::nullopt;

    return juce::jlimit (0, 127, (octave.getIntValue() + 2) * 12 + *semitone);
}

juce::String noteName (int note)
{
    return juce::MidiMessage::getMidiNoteName (note, true, true, 3);
}

/** Matches a typed name against a list of display names, ignoring case and treating
    spaces and slashes as dashes: "harmonic-minor" matches "Harmonic minor". */
template <size_t N>
std::optional<int> matchName (const juce::String& typed, const std::array<const char*, N>& names)
{
    const auto normalise = [] (juce::String s) { return s.toLowerCase().replaceCharacters (" /", "--"); };

    for (size_t i = 0; i < N; ++i)
        if (normalise (names[i]) == normalise (typed))
            return (int) i;

    return std::nullopt;
}

struct NoteEvent
{
    std::int64_t sample;
    int note;
    bool on;
};

std::optional<std::vector<NoteEvent>> readMidiFile (const juce::File& file, double sampleRate)
{
    juce::FileInputStream stream (file);
    juce::MidiFile midi;

    if (! stream.openedOk() || ! midi.readFrom (stream))
        return std::nullopt;

    midi.convertTimestampTicksToSeconds();
    std::vector<NoteEvent> events;

    for (int track = 0; track < midi.getNumTracks(); ++track)
    {
        for (const auto* holder : *midi.getTrack (track))
        {
            const auto& m = holder->message;

            if (m.isNoteOnOrOff())
                events.push_back ({ (std::int64_t) std::llround (m.getTimeStamp() * sampleRate), m.getNoteNumber(), m.isNoteOn() });
        }
    }

    // In time order, keeping the file's order at the same instant. The corrector counts
    // note-ons against note-offs, so a note that ends and restarts at once stays held
    // whichever comes first.
    std::stable_sort (events.begin(), events.end(), [] (const NoteEvent& a, const NoteEvent& b)
                      { return a.sample < b.sample; });
    return events;
}

struct GrainCollector final : PitchCorrector::Listener
{
    std::vector<PitchCorrector::GrainInfo> grains;
    void grainProduced (const PitchCorrector::GrainInfo& g) override { grains.push_back (g); }
};

void printPitch (const std::vector<PitchCorrector::GrainInfo>& grains, int latency, double sampleRate,
                 std::int64_t length)
{
    std::printf ("%8s  %9s  %-12s  %-6s  %9s\n", "time", "sung Hz", "sung", "target", "out Hz");

    const auto step = (std::int64_t) (0.01 * sampleRate);
    size_t g = 0;

    // The last grain centred at or before the current moment (grain times include the
    // latency). Low voices and downward shifts space grains further apart than the 10 ms
    // step, so it carries over from one step to the next.
    const PitchCorrector::GrainInfo* latest = nullptr;

    for (std::int64_t time = 0; time < length; time += step)
    {
        while (g < grains.size() && grains[g].outputSample - latency <= time)
            latest = &grains[g++];

        if (latest == nullptr)
            continue;

        if (latest->inputPitch < 0.0f)
        {
            std::printf ("%8.3f  %9s\n", (double) time / sampleRate, "-");
            continue;
        }

        const int nearest = juce::roundToInt (latest->inputPitch);
        const int cents = juce::roundToInt (100.0f * (latest->inputPitch - (float) nearest));
        const auto sung = noteName (nearest) + (cents >= 0 ? " +" : " ") + juce::String (cents) + "c";
        const auto target = latest->targetNote >= 0 ? noteName (latest->targetNote) : juce::String ("-");

        std::printf ("%8.3f  %9.2f  %-12s  %-6s  %9.2f\n", (double) time / sampleRate,
                     pitchToFrequency (latest->inputPitch), sung.toRawUTF8(), target.toRawUTF8(),
                     pitchToFrequency (latest->outputPitch));
    }
}

int fail (const juce::String& message)
{
    std::fprintf (stderr, "tune-a-fish: %s\n\n%s", message.toRawUTF8(), usage);
    return 1;
}
} // namespace

int main (int argc, char* argv[])
{
    juce::StringArray positional;
    CorrectorSettings settings;
    std::optional<int> heldNote;
    juce::File midiFile;
    bool pitchOnly = false;

    for (int i = 1; i < argc; ++i)
    {
        const juce::String arg (argv[i]);

        if (arg == "-h" || arg == "--help")
        {
            std::printf ("%s", usage);
            return 0;
        }

        if (arg == "--pitch")
        {
            pitchOnly = true;
            continue;
        }

        if (! arg.startsWith ("--"))
        {
            positional.add (arg);
            continue;
        }

        if (i + 1 >= argc)
            return fail (arg + " needs a value");

        const juce::String value (argv[++i]);

        if (arg == "--key")
        {
            int used = 0;
            const auto pitchClass = parsePitchClass (value, used);

            if (! pitchClass || used != value.trim().length())
                return fail ("unknown key: " + value);

            settings.key = (*pitchClass + 12) % 12;
        }
        else if (arg == "--scale")
        {
            const auto scale = matchName (value, scaleNames);

            if (! scale)
                return fail ("unknown scale: " + value);

            settings.scale = (ScaleType) *scale;
        }
        else if (arg == "--voice")
        {
            const auto voice = matchName (value, voiceNames);

            if (! voice)
                return fail ("unknown voice: " + value);

            settings.voice = (VoiceType) *voice;
        }
        else if (arg == "--retune")
        {
            settings.retuneMs = juce::jlimit (0.0, 400.0, value.getDoubleValue());
        }
        else if (arg == "--amount")
        {
            settings.amount = juce::jlimit (0.0, 100.0, value.getDoubleValue()) / 100.0;
        }
        else if (arg == "--note")
        {
            heldNote = parseNote (value);

            if (! heldNote)
                return fail ("not a note: " + value);

            settings.midiMode = true;
        }
        else if (arg == "--midi")
        {
            midiFile = juce::File::getCurrentWorkingDirectory().getChildFile (value);
            settings.midiMode = true;
        }
        else
        {
            return fail ("unknown option: " + arg);
        }
    }

    if (positional.size() != (pitchOnly ? 1 : 2))
        return fail (pitchOnly ? "give one input file" : "give an input and an output file");

    const auto inputFile = juce::File::getCurrentWorkingDirectory().getChildFile (positional[0]);

    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (inputFile));

    if (reader == nullptr)
        return fail ("can't read " + inputFile.getFullPathName());

    const double sampleRate = reader->sampleRate;
    const auto length = (std::int64_t) reader->lengthInSamples;
    const int numChannels = juce::jlimit (1, PitchCorrector::maxChannels, (int) reader->numChannels);

    std::vector<NoteEvent> notes;

    if (heldNote)
        notes.push_back ({ 0, *heldNote, true });

    if (midiFile != juce::File())
    {
        const auto events = readMidiFile (midiFile, sampleRate);

        if (! events)
            return fail ("can't read MIDI file " + midiFile.getFullPathName());

        notes.insert (notes.end(), events->begin(), events->end());
    }

    PitchCorrector corrector;
    corrector.prepare (sampleRate, numChannels);
    corrector.setSettings (settings);

    GrainCollector collector;

    if (pitchOnly)
        corrector.setListener (&collector);

    // Pad the end with the latency, so the tail of the input comes out too.
    const int latency = corrector.getLatencySamples();
    juce::AudioBuffer<float> buffer (numChannels, (int) length + latency);
    buffer.clear();
    reader->read (&buffer, 0, (int) length, 0, true, numChannels > 1);

    constexpr int blockSize = 512;
    const auto total = (std::int64_t) buffer.getNumSamples();
    size_t nextNote = 0;

    for (std::int64_t position = 0; position < total;)
    {
        while (nextNote < notes.size() && notes[nextNote].sample <= position)
        {
            const auto& n = notes[nextNote++];

            if (n.on)
                corrector.noteOn (n.note);
            else
                corrector.noteOff (n.note);
        }

        auto end = std::min (total, position + blockSize);

        if (nextNote < notes.size())
            end = std::min (end, notes[nextNote].sample);

        std::array<float*, PitchCorrector::maxChannels> channels {};

        for (int ch = 0; ch < numChannels; ++ch)
            channels[(size_t) ch] = buffer.getWritePointer (ch, (int) position);

        corrector.process (channels.data(), channels.data(), numChannels, (int) (end - position));
        position = end;
    }

    if (corrector.getLookaheadShortfalls() > 0)
        std::fprintf (stderr, "tune-a-fish: warning: %lld grains were cut short (a bug, please report it)\n",
                      (long long) corrector.getLookaheadShortfalls());

    if (pitchOnly)
    {
        printPitch (collector.grains, latency, sampleRate, length);
        return 0;
    }

    const auto outputFile = juce::File::getCurrentWorkingDirectory().getChildFile (positional[1]);
    outputFile.deleteFile();

    std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (outputFile);

    if (! static_cast<juce::FileOutputStream&> (*stream).openedOk())
        return fail ("can't write " + outputFile.getFullPathName());

    juce::WavAudioFormat wav;
    const auto writer = wav.createWriterFor (stream, juce::AudioFormatWriterOptions {}
                                                         .withSampleRate (sampleRate)
                                                         .withNumChannels (numChannels)
                                                         .withBitsPerSample (24));

    if (writer == nullptr || ! writer->writeFromAudioSampleBuffer (buffer, latency, (int) length))
        return fail ("can't write " + outputFile.getFullPathName());

    std::printf ("Wrote %s (%.1f s, %d channel%s)\n", outputFile.getFullPathName().toRawUTF8(), (double) length / sampleRate,
                 numChannels, numChannels == 1 ? "" : "s");
    return 0;
}
