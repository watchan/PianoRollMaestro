#pragma once
#include <JuceHeader.h>

class SimpleSineSound : public juce::SynthesiserSound
{
public:
    bool appliesToNote(int) override { return true; }
    bool appliesToChannel(int) override { return true; }
};

class SimpleSineVoice : public juce::SynthesiserVoice
{
public:
    SimpleSineVoice();

    bool canPlaySound(juce::SynthesiserSound* sound) override;
    void startNote(int midiNoteNumber, float velocity, juce::SynthesiserSound*, int currentPitchWheelPosition) override;
    void stopNote(float velocity, bool allowTailOff) override;
    void pitchWheelMoved(int newPitchWheelValue) override;
    void controllerMoved(int, int) override {}
    void renderNextBlock(juce::AudioBuffer<float>& outputBuffer, int startSample, int numSamples) override;

private:
    double currentAngle = 0.0;
    double angleDelta = 0.0;
    float level = 0.0f;
    juce::ADSR adsr;

    // Base note this voice is currently playing, re-applied (bent) every
    // time startNote()/pitchWheelMoved() sets angleDelta -- unlike a
    // hosted plugin, this built-in synth has no other way to know what
    // frequency to bend FROM (pitchWheelMoved() used to be a no-op here,
    // so the message was being sent correctly the whole time but this
    // voice just ignored it).
    int currentNoteNumber = -1;
    void updateAngleDelta(int pitchWheelValue);
};
