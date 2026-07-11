#include "PitchDetector.h"
#include <cmath>
#include <vector>

PitchDetector::Result PitchDetector::detectPitch(const float* samples, int numSamples, double sampleRate) const
{
    Result result;

    int maxLag = numSamples / 2;
    if (maxLag < 2)
        return result;

    // 0. Silence/noise-floor gate: skip YIN entirely below this level so
    // room tone/fan noise never gets reported as a "detected" pitch.
    {
        double sumSquares = 0.0;
        for (int i = 0; i < numSamples; ++i)
            sumSquares += (double) samples[i] * (double) samples[i];
        auto rms = std::sqrt(sumSquares / numSamples);
        if (rms < minimumRms)
            return result;
    }

    // 1. Difference function: d(tau) = sum_j (x[j] - x[j+tau])^2
    std::vector<double> difference((size_t) maxLag, 0.0);
    for (int tau = 1; tau < maxLag; ++tau)
    {
        double sum = 0.0;
        for (int j = 0; j < maxLag; ++j)
        {
            double delta = (double) samples[j] - (double) samples[j + tau];
            sum += delta * delta;
        }
        difference[(size_t) tau] = sum;
    }

    // 2. Cumulative mean normalized difference function.
    std::vector<double> cmnd((size_t) maxLag, 1.0);
    double runningSum = 0.0;
    for (int tau = 1; tau < maxLag; ++tau)
    {
        runningSum += difference[(size_t) tau];
        cmnd[(size_t) tau] = runningSum > 0.0 ? difference[(size_t) tau] * tau / runningSum : 1.0;
    }

    // 3. Absolute threshold: first dip below yinThreshold, walked to its local minimum.
    int tauEstimate = -1;
    for (int tau = 2; tau < maxLag - 1; ++tau)
    {
        if (cmnd[(size_t) tau] < yinThreshold)
        {
            while (tau + 1 < maxLag && cmnd[(size_t) (tau + 1)] < cmnd[(size_t) tau])
                ++tau;
            tauEstimate = tau;
            break;
        }
    }

    if (tauEstimate < 0)
        return result; // unvoiced -- no dip found below threshold

    // 3b. Octave-error correction: humming/singing often has a weak
    // fundamental relative to its 2nd harmonic, so the first-dip search
    // above can lock onto the harmonic (tau) instead of the true, longer-
    // period fundamental (2*tau, one octave lower). Naively "prefer 2*tau if
    // it's also below threshold" is too aggressive -- for an exactly
    // periodic signal (e.g. a clean sine, see the unit test), cmnd is
    // trivially near-zero at EVERY integer multiple of the true period, so
    // that check alone would always chase the octave down indefinitely.
    // Require the doubled period's dip to be MEANINGFULLY deeper (not just
    // "also under threshold") before trusting it as the real fundamental.
    while (tauEstimate * 2 < maxLag - 1 && cmnd[(size_t) (tauEstimate * 2)] < cmnd[(size_t) tauEstimate] * 0.8)
    {
        int doubled = tauEstimate * 2;
        while (doubled + 1 < maxLag && cmnd[(size_t) (doubled + 1)] < cmnd[(size_t) doubled])
            ++doubled;
        tauEstimate = doubled;
    }

    // 4. Parabolic interpolation around tauEstimate for sub-sample precision.
    double betterTau = (double) tauEstimate;
    if (tauEstimate > 0 && tauEstimate < maxLag - 1)
    {
        auto s0 = cmnd[(size_t) (tauEstimate - 1)];
        auto s1 = cmnd[(size_t) tauEstimate];
        auto s2 = cmnd[(size_t) (tauEstimate + 1)];
        auto denom = 2.0 * (2.0 * s1 - s2 - s0);
        if (std::abs(denom) > 1.0e-12)
            betterTau = (double) tauEstimate + (s2 - s0) / denom;
    }

    if (betterTau <= 0.0)
        return result;

    result.voiced = true;
    result.frequencyHz = sampleRate / betterTau;
    return result;
}
