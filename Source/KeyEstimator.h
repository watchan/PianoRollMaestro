#pragma once
#include "ProjectModel.h"

// Whole-piece key guess -- rootPitchClass (0 = C) + isMinor. hasEnoughData
// is false only when there's nothing to analyze yet (no notes at all in any
// included track); callers should leave whatever key context they already
// have unchanged in that case rather than snapping to a meaningless default.
struct KeyEstimate
{
    int rootPitchClass = 0;
    bool isMinor = false;
    bool hasEnoughData = false;
};

// Krumhansl-Schmuckler key-finding: correlates a duration-weighted pitch-
// class histogram (pooled across every track with
// Track::includeInChordEstimate set, same pool ChordEstimator uses) against
// the standard empirically-measured major/minor tone profiles, and picks
// whichever of the 24 (root, mode) combinations correlates best. Purely a
// rough, whole-piece average -- no notion of modulation/key changes over
// time, same "not a real music-theory engine" scope as ChordEstimator.
class KeyEstimator
{
public:
    static KeyEstimate estimate(const Project& project);
};
