// SPDX-License-Identifier: AGPL-3.0-or-later

#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace
{
using tuneafish::PitchCorrector;
using tuneafish::ScaleType;
using tuneafish::VoiceType;

template <size_t N>
juce::StringArray toStringArray (const std::array<const char*, N>& names)
{
    juce::StringArray result;

    for (auto* name : names)
        result.add (name);

    return result;
}
} // namespace

juce::String TuneAFishProcessor::noteName (int note)
{
    return juce::MidiMessage::getMidiNoteName (note, true, true, 3);
}

TuneAFishProcessor::TuneAFishProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      params (*this, nullptr, "TuneAFish", createParameterLayout())
{
    mode = params.getRawParameterValue ("mode");
    key = params.getRawParameterValue ("key");
    scale = params.getRawParameterValue ("scale");
    retune = params.getRawParameterValue ("retune");
    amount = params.getRawParameterValue ("amount");
    voice = params.getRawParameterValue ("voice");
}

juce::AudioProcessorValueTreeState::ParameterLayout TuneAFishProcessor::createParameterLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    layout.add (std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { "mode", 1 }, "Mode",
                                                              juce::StringArray { "Scale", "MIDI" }, 0),
                std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { "key", 1 }, "Key",
                                                              toStringArray (tuneafish::keyNames), 0),
                std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { "scale", 1 }, "Scale",
                                                              toStringArray (tuneafish::scaleNames), 0));

    const auto retuneText = juce::AudioParameterFloatAttributes {}
                                .withStringFromValueFunction ([] (float value, int)
                                                              {
                                                                  return value < 0.5f ? juce::String ("Instant")
                                                                                      : juce::String (juce::roundToInt (value)) + " ms";
                                                              })
                                .withValueFromStringFunction ([] (const juce::String& text) { return text.getFloatValue(); });

    // Skewed so the useful short times get most of the knob: the middle is 100 ms.
    layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "retune", 1 }, "Retune speed",
                                                             juce::NormalisableRange<float> (0.0f, 400.0f, 1.0f, 0.5f),
                                                             50.0f, retuneText));

    const auto percentText = juce::AudioParameterFloatAttributes {}
                                 .withStringFromValueFunction ([] (float value, int)
                                                               { return juce::String (juce::roundToInt (value)) + "%"; })
                                 .withValueFromStringFunction ([] (const juce::String& text) { return text.getFloatValue(); });

    layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "amount", 1 }, "Amount",
                                                             juce::NormalisableRange<float> (0.0f, 100.0f, 1.0f), 100.0f,
                                                             percentText),
                std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { "voice", 1 }, "Voice",
                                                              toStringArray (tuneafish::voiceNames),
                                                              (int) VoiceType::altoTenor));

    return layout;
}

void TuneAFishProcessor::prepareToPlay (double sampleRate, int)
{
    preparedChannels = juce::jlimit (1, PitchCorrector::maxChannels, getMainBusNumInputChannels());
    corrector.prepare (sampleRate, preparedChannels);
    corrector.setSettings (currentSettings());
    corrector.setListener (this);
    setLatencySamples (corrector.getLatencySamples());

    history.sampleRate = sampleRate;
    history.outputPosition = 0;
}

void TuneAFishProcessor::reset()
{
    // Called by hosts when processing stops (the VST3 wrapper calls it from
    // setProcessing (false)), so nothing from before the stop comes out after it.
    corrector.reset();
    history.outputPosition = 0;
}

bool TuneAFishProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto& in = layouts.getMainInputChannelSet();
    const auto& out = layouts.getMainOutputChannelSet();
    return in == out && (out == juce::AudioChannelSet::mono() || out == juce::AudioChannelSet::stereo());
}

tuneafish::CorrectorSettings TuneAFishProcessor::currentSettings() const
{
    tuneafish::CorrectorSettings s;
    s.midiMode = mode->load() > 0.5f;
    s.key = juce::jlimit (0, 11, juce::roundToInt (key->load()));
    s.scale = (ScaleType) juce::jlimit (0, (int) ScaleType::count - 1, juce::roundToInt (scale->load()));
    s.retuneMs = retune->load();
    s.amount = amount->load() / 100.0;
    s.voice = (VoiceType) juce::jlimit (0, (int) VoiceType::count - 1, juce::roundToInt (voice->load()));
    return s;
}

void TuneAFishProcessor::handleMidi (const juce::MidiMessage& message)
{
    // Any MIDI channel counts.
    if (message.isNoteOn())
        corrector.noteOn (message.getNoteNumber());
    else if (message.isNoteOff())
        corrector.noteOff (message.getNoteNumber());
    else if (message.isAllNotesOff() || message.isAllSoundOff())
        corrector.allNotesOff();
}

void TuneAFishProcessor::process (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi, bool bypassed)
{
    juce::ScopedNoDenormals noDenormals;

    const int numSamples = buffer.getNumSamples();
    const int numChannels = juce::jmin (buffer.getNumChannels(), preparedChannels);

    if (numChannels == 0 || numSamples == 0)
    {
        // No audio, but hosts can still deliver notes this way.
        for (const auto metadata : midi)
            handleMidi (metadata.getMessage());

        return;
    }

    // While bypassed the corrector keeps listening (to notes too), so it picks up seamlessly.
    corrector.setBypassed (bypassed);
    corrector.setSettings (currentSettings());

    // Process the audio between MIDI events, so each note lands on the right sample.
    std::array<float*, PitchCorrector::maxChannels> channels {};
    auto* const* writePointers = buffer.getArrayOfWritePointers();

    const auto processRange = [&] (int start, int length)
    {
        for (int ch = 0; ch < numChannels; ++ch)
            channels[(size_t) ch] = writePointers[ch] + start;

        corrector.process (channels.data(), channels.data(), numChannels, length);
    };

    int position = 0;

    for (const auto metadata : midi)
    {
        const int eventPosition = juce::jlimit (0, numSamples, metadata.samplePosition);

        if (eventPosition > position)
        {
            processRange (position, eventPosition - position);
            position = eventPosition;
        }

        handleMidi (metadata.getMessage());
    }

    if (position < numSamples)
        processRange (position, numSamples - position);

    // Any channels beyond the ones we process (there shouldn't be any) get silence.
    for (int ch = numChannels; ch < buffer.getNumChannels(); ++ch)
        buffer.clear (ch, 0, numSamples);

    history.outputPosition.fetch_add (numSamples);
}

void TuneAFishProcessor::grainProduced (const tuneafish::PitchCorrector::GrainInfo& grain)
{
    history.push (grain);
}

juce::AudioProcessorEditor* TuneAFishProcessor::createEditor()
{
    return new TuneAFishEditor (*this);
}

void TuneAFishProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    if (auto xml = params.copyState().createXml())
        copyXmlToBinary (*xml, destData);
}

void TuneAFishProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (params.state.getType()))
            params.replaceState (juce::ValueTree::fromXml (*xml));
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new TuneAFishProcessor();
}
