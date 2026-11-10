// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "dsp/PitchCorrector.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <atomic>

/** Grain reports from the audio thread, for the pitch display. Lock-free: the audio thread
    writes, the message thread reads, and if the display falls behind, reports are dropped. */
class PitchHistory
{
public:
    using Grain = tuneafish::PitchCorrector::GrainInfo;

    void push (const Grain& grain)
    {
        const auto scope = fifo.write (1);

        if (scope.blockSize1 > 0)
            buffer[(size_t) scope.startIndex1] = grain;
    }

    template <typename Callback>
    void popAll (Callback&& callback)
    {
        const auto scope = fifo.read (fifo.getNumReady());

        for (int i = 0; i < scope.blockSize1; ++i)
            callback (buffer[(size_t) (scope.startIndex1 + i)]);

        for (int i = 0; i < scope.blockSize2; ++i)
            callback (buffer[(size_t) (scope.startIndex2 + i)]);
    }

    std::atomic<std::int64_t> outputPosition { 0 }; // samples output since the last reset
    std::atomic<double> sampleRate { 44100.0 };

private:
    static constexpr int capacity = 8192;
    juce::AbstractFifo fifo { capacity };
    std::array<Grain, capacity> buffer {};
};

class TuneAFishProcessor final : public juce::AudioProcessor,
                                 private tuneafish::PitchCorrector::Listener
{
public:
    TuneAFishProcessor();
    ~TuneAFishProcessor() override = default;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;

    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    /** Note names use Live's convention, where middle C (60) is C3. */
    static juce::String noteName (int note);

    juce::AudioProcessorValueTreeState params;
    PitchHistory history;

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();
    void grainProduced (const tuneafish::PitchCorrector::GrainInfo&) override;
    tuneafish::CorrectorSettings currentSettings() const;
    void handleMidi (const juce::MidiMessage&);

    tuneafish::PitchCorrector corrector;
    int preparedChannels = 0;

    std::atomic<float>* mode = nullptr;
    std::atomic<float>* key = nullptr;
    std::atomic<float>* scale = nullptr;
    std::atomic<float>* retune = nullptr;
    std::atomic<float>* amount = nullptr;
    std::atomic<float>* voice = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TuneAFishProcessor)
};
