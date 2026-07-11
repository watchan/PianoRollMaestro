#pragma once

// Pure DSP, no JUCE audio-thread specifics -- takes a raw sample buffer and
// returns the detected fundamental frequency (YIN algorithm). Deliberately
// framework-independent so it can be unit-tested standalone.
class PitchDetector
{
public:
    struct Result
    {
        bool voiced = false;
        double frequencyHz = 0.0;
    };

    // numSamples should be at least ~2x the period of the lowest frequency
    // you want to detect (e.g. 2048 samples at 44.1kHz comfortably covers
    // human humming range down to ~80Hz).
    Result detectPitch(const float* samples, int numSamples, double sampleRate) const;

private:
    static constexpr double yinThreshold = 0.15;

    // Below this RMS level, treat the window as silence/noise floor and skip
    // pitch detection entirely rather than let YIN find a spurious "pitch"
    // in room tone/fan noise. ~-31dBFS -- raised from 0.02 after a real
    // headphone-monitoring session locked onto sustained background noise
    // (fan/electrical hum) as a false "note" and held it continuously.
    static constexpr double minimumRms = 0.03;
};
