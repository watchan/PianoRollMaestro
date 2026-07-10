#include "SimpleSineVoice.h"

SimpleSineVoice::SimpleSineVoice()
{
    juce::ADSR::Parameters params;
    params.attack = 0.005f;
    params.decay = 0.05f;
    params.sustain = 0.8f;
    params.release = 0.08f;
    adsr.setParameters(params);
}

bool SimpleSineVoice::canPlaySound(juce::SynthesiserSound* sound)
{
    return dynamic_cast<SimpleSineSound*>(sound) != nullptr;
}

void SimpleSineVoice::startNote(int midiNoteNumber, float velocity, juce::SynthesiserSound*, int)
{
    level = velocity;
    currentAngle = 0.0;

    auto frequency = juce::MidiMessage::getMidiNoteInHertz(midiNoteNumber);
    angleDelta = juce::MathConstants<double>::twoPi * frequency / getSampleRate();

    adsr.setSampleRate(getSampleRate());
    adsr.noteOn();
}

void SimpleSineVoice::stopNote(float, bool allowTailOff)
{
    adsr.noteOff();

    if (!allowTailOff)
        clearCurrentNote();
}

void SimpleSineVoice::renderNextBlock(juce::AudioBuffer<float>& outputBuffer, int startSample, int numSamples)
{
    if (angleDelta == 0.0)
        return;

    for (int i = 0; i < numSamples; ++i)
    {
        auto envelope = adsr.getNextSample();
        auto sample = (float) std::sin(currentAngle) * level * envelope;

        for (int ch = 0; ch < outputBuffer.getNumChannels(); ++ch)
            outputBuffer.addSample(ch, startSample + i, sample);

        currentAngle += angleDelta;

        if (!adsr.isActive())
        {
            clearCurrentNote();
            angleDelta = 0.0;
            break;
        }
    }
}
