#include "KeyEstimator.h"
#include <array>
#include <cmath>

namespace
{
    using Profile = std::array<double, 12>;

    // Krumhansl & Kessler's empirically-measured tone-profile weights
    // (probe-tone ratings), index 0 = the profile's own tonic.
    const Profile& majorProfile()
    {
        static const Profile p = { 6.35, 2.23, 3.48, 2.33, 4.38, 4.09, 2.52, 5.19, 2.39, 3.66, 2.29, 2.88 };
        return p;
    }

    const Profile& minorProfile()
    {
        static const Profile p = { 6.33, 2.68, 3.52, 5.38, 2.60, 3.53, 2.54, 4.75, 3.98, 2.69, 3.34, 3.17 };
        return p;
    }

    // Pearson correlation between the observed histogram and `profile`
    // rotated so its tonic sits at rootOffset -- highest correlation across
    // every (rootOffset, profile) combination is the key guess.
    double correlate(const std::array<double, 12>& histogram, const Profile& profile, int rootOffset)
    {
        std::array<double, 12> rotated {};
        for (int i = 0; i < 12; ++i)
            rotated[(size_t) i] = profile[(size_t) ((i - rootOffset + 12) % 12)];

        double meanH = 0.0, meanP = 0.0;
        for (int i = 0; i < 12; ++i)
        {
            meanH += histogram[(size_t) i];
            meanP += rotated[(size_t) i];
        }
        meanH /= 12.0;
        meanP /= 12.0;

        double num = 0.0, denomH = 0.0, denomP = 0.0;
        for (int i = 0; i < 12; ++i)
        {
            auto dh = histogram[(size_t) i] - meanH;
            auto dp = rotated[(size_t) i] - meanP;
            num += dh * dp;
            denomH += dh * dh;
            denomP += dp * dp;
        }

        if (denomH <= 0.0 || denomP <= 0.0)
            return -2.0; // a flat (silent, or perfectly even) histogram can't correlate meaningfully

        return num / std::sqrt(denomH * denomP);
    }
}

KeyEstimate KeyEstimator::estimate(const Project& project)
{
    std::array<double, 12> histogram {};
    histogram.fill(0.0);
    double totalWeight = 0.0;

    for (auto& track : project.tracks)
    {
        if (!track.includeInChordEstimate)
            continue;

        auto& steps = track.clip.steps;
        for (int i = 0; i < (int) steps.size(); ++i)
        {
            auto& step = steps[(size_t) i];
            if (step.notes.empty() || step.tiedFromPrevious)
                continue; // not a note start

            // Envelope length (tie-chain), same fallback commitPendingNoteAt()
            // uses when a note has no individually-measured length of its own.
            auto envelopeLength = step.lengthInSteps;
            auto lookahead = i + 1;
            while (lookahead < (int) steps.size() && steps[(size_t) lookahead].tiedFromPrevious)
            {
                envelopeLength += steps[(size_t) lookahead].lengthInSteps;
                ++lookahead;
            }

            for (auto& note : step.notes)
            {
                auto pitchClass = ((note.pitch % 12) + 12) % 12;
                auto weight = (double) juce::jmax(1, note.durationSteps > 0 ? note.durationSteps : envelopeLength);
                histogram[(size_t) pitchClass] += weight;
                totalWeight += weight;
            }
        }
    }

    KeyEstimate result;
    if (totalWeight <= 0.0)
        return result; // hasEnoughData stays false -- nothing to analyze yet

    auto bestScore = -3.0;
    for (int root = 0; root < 12; ++root)
    {
        auto majorScore = correlate(histogram, majorProfile(), root);
        if (majorScore > bestScore)
        {
            bestScore = majorScore;
            result.rootPitchClass = root;
            result.isMinor = false;
        }

        auto minorScore = correlate(histogram, minorProfile(), root);
        if (minorScore > bestScore)
        {
            bestScore = minorScore;
            result.rootPitchClass = root;
            result.isMinor = true;
        }
    }

    result.hasEnoughData = true;
    return result;
}
