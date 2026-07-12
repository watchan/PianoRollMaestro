#pragma once
#include "ProjectModel.h"
#include <vector>

// One chord guess's span, in step-grid coordinates. Spans can be wider than
// a single analysis segment -- see ChordEstimator::estimate() -- so the
// label is only shown once, at startStep, rather than repeated.
struct ChordEstimate
{
    int startStep = 0;
    int lengthInSteps = 0;
    juce::String label; // e.g. "C", "Am", "G7" -- empty means silence (no notes sounding in this span)
};

// Simple template-matching harmonic analysis, purely for a rough at-a-glance
// chord-progression readout -- not trying to be a real music-theory engine
// (no key context). Analyzes in fine (0.5-beat) segments so a chord change
// mid-bar is actually caught, pools the pitch classes sounding across every
// track with Track::includeInChordEstimate set (a track can be excluded,
// e.g. drums/percussion, via Cmd+A) within each segment, and picks whichever
// (root, quality) template
// covers the most of them with the fewest leftovers -- see
// bestChordLabel()'s bass-note tie-break and slash/"on-chord" notation for
// how ties and inversions are handled. Consecutive segments that land on the
// same label are merged into one wider span before being returned, so a
// sustained chord is reported once (at the step it actually starts) instead
// of repeating the same label on every single segment. Two filters keep
// fleeting/thin readings from being called a "chord": fewer than 3 distinct
// pitch classes (a single note or a dyad) never gets a label, and a merged
// span shorter than a full beat gets its label cleared even if it matched
// a template.
class ChordEstimator
{
public:
    // segmentLengthInSteps is the fine analysis granularity -- normally
    // stepsPerQuarterNote / 2 (half a beat), fine enough to catch a chord
    // change mid-bar. Merged spans in the result can end up much longer
    // than this once consecutive segments share a label.
    static std::vector<ChordEstimate> estimate(const Project& project, int segmentLengthInSteps);
};
