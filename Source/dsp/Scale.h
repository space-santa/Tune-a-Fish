// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <initializer_list>

/** Pitches are measured in MIDI semitones throughout: 69.0 is A4 (440 Hz), and a pitch
    of 69.25 is a quarter of a semitone (25 cents) sharp of A4. */
namespace tuneafish
{
inline double frequencyToPitch (double hz) { return 69.0 + 12.0 * std::log2 (hz / 440.0); }
inline double pitchToFrequency (double pitch) { return 440.0 * std::exp2 ((pitch - 69.0) / 12.0); }

enum class ScaleType
{
    chromatic,
    major,
    minor,
    harmonicMinor,
    majorPentatonic,
    minorPentatonic,
    blues,
    count
};

namespace detail
{
constexpr std::uint16_t mask (std::initializer_list<int> degrees)
{
    std::uint16_t m = 0;

    for (auto d : degrees)
        m = (std::uint16_t) (m | (1u << d));

    return m;
}
} // namespace detail

/** Bit n is set when the note n semitones above the key is in the scale. */
inline constexpr std::array<std::uint16_t, (size_t) ScaleType::count> scaleMasks {
    detail::mask ({ 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 }), // chromatic
    detail::mask ({ 0, 2, 4, 5, 7, 9, 11 }),                 // major
    detail::mask ({ 0, 2, 3, 5, 7, 8, 10 }),                 // natural minor
    detail::mask ({ 0, 2, 3, 5, 7, 8, 11 }),                 // harmonic minor
    detail::mask ({ 0, 2, 4, 7, 9 }),                        // major pentatonic
    detail::mask ({ 0, 3, 5, 7, 10 }),                       // minor pentatonic
    detail::mask ({ 0, 3, 5, 6, 7, 10 }),                    // blues
};

inline constexpr std::array<const char*, (size_t) ScaleType::count> scaleNames {
    "Chromatic", "Major", "Minor", "Harmonic minor", "Major pentatonic", "Minor pentatonic", "Blues",
};

inline constexpr std::array<const char*, 12> keyNames {
    "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B",
};

/** key: 0 = C, 1 = C#, ... 11 = B. */
inline bool isInScale (int midiNote, int key, ScaleType scale)
{
    const int degree = ((midiNote - key) % 12 + 12) % 12;
    return ((scaleMasks[(size_t) scale] >> degree) & 1u) != 0;
}

/** The note in the scale closest to a pitch. Exact ties go to the lower note. */
inline int nearestNoteInScale (double pitch, int key, ScaleType scale)
{
    const int centre = (int) std::floor (pitch + 0.5);
    int best = centre;
    double bestDistance = 1.0e9;

    // Every scale here has a gap of at most 3 semitones, so the nearest note is close.
    for (int note = centre - 6; note <= centre + 6; ++note)
    {
        if (! isInScale (note, key, scale))
            continue;

        const double distance = std::abs (pitch - note);

        if (distance < bestDistance - 1.0e-9)
        {
            best = note;
            bestDistance = distance;
        }
    }

    return best;
}
} // namespace tuneafish
