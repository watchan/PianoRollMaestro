#include "ChordEstimator.h"
#include <array>

namespace
{
    using PitchClassSet = std::array<bool, 12>;

    struct StepActivity
    {
        PitchClassSet pitchClasses {};
        int lowestPitch = -1; // -1 = nothing sounding at this step
    };

    // Marks every step covered by each note in `steps` (walking tied
    // continuation chains the same way MainEditorComponent's note-length
    // helpers do) into `activity`, OR-ing pitch classes and tracking the
    // lowest raw pitch on top of whatever's already there so multiple
    // tracks can be pooled into one array.
    void markActiveStepActivity(const std::vector<Step>& steps, std::vector<StepActivity>& activity)
    {
        for (int i = 0; i < (int) steps.size(); ++i)
        {
            auto& step = steps[(size_t) i];
            if (step.notes.empty() || step.tiedFromPrevious)
                continue; // not a note start -- either silence or a continuation already covered below

            auto totalLength = step.lengthInSteps;
            auto lookahead = i + 1;
            while (lookahead < (int) steps.size() && steps[(size_t) lookahead].tiedFromPrevious)
            {
                totalLength += steps[(size_t) lookahead].lengthInSteps;
                ++lookahead;
            }

            for (auto& note : step.notes)
            {
                auto pitchClass = ((note.pitch % 12) + 12) % 12;
                for (int s = i; s < i + totalLength && s < (int) activity.size(); ++s)
                {
                    auto& stepActivity = activity[(size_t) s];
                    stepActivity.pitchClasses[(size_t) pitchClass] = true;
                    if (stepActivity.lowestPitch < 0 || note.pitch < stepActivity.lowestPitch)
                        stepActivity.lowestPitch = note.pitch;
                }
            }
        }
    }

    struct ChordTemplate { const char* qualitySuffix; std::vector<int> intervals; };

    const std::vector<ChordTemplate>& chordTemplates()
    {
        // Ordered roughly common-to-rare so ties in score favour the more
        // common reading (major/minor triads beat sus/dim/aug on a tie).
        static const std::vector<ChordTemplate> templates = {
            { "",     { 0, 4, 7 } },      // major
            { "m",    { 0, 3, 7 } },      // minor
            { "7",    { 0, 4, 7, 10 } },  // dominant 7th
            { "maj7", { 0, 4, 7, 11 } },  // major 7th
            { "m7",   { 0, 3, 7, 10 } },  // minor 7th
            { "6",    { 0, 4, 7, 9 } },   // major 6th
            { "m6",   { 0, 3, 7, 9 } },   // minor 6th
            { "sus4", { 0, 5, 7 } },
            { "sus2", { 0, 2, 7 } },
            { "dim",  { 0, 3, 6 } },
            { "aug",  { 0, 4, 8 } },
        };
        return templates;
    }

    struct ChordMatch
    {
        juce::String label;
        int root = -1;
        const char* qualitySuffix = nullptr; // nullptr = no match (see bestChordMatch's numActive < 3 early-out)
    };

    // bassPitchClass = pitch class of the lowest note actually sounding in
    // this bar, or -1 if nothing is (silence). -1 never matches a root
    // (0-11), so it just disables the bonus below.
    ChordMatch bestChordMatch(const PitchClassSet& active, int bassPitchClass)
    {
        static const char* const noteNames[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };

        auto numActive = 0;
        for (auto b : active)
            if (b) ++numActive;
        if (numActive < 3)
            return {}; // a single note or a dyad isn't a "chord" for this purpose

        juce::String bestLabel;
        auto bestScore = -1000;
        auto bestRoot = -1;
        const char* bestQuality = "";

        for (int root = 0; root < 12; ++root)
        {
            for (auto& chordTemplate : chordTemplates())
            {
                auto matched = 0;
                for (auto interval : chordTemplate.intervals)
                    if (active[(size_t) ((root + interval) % 12)])
                        ++matched;

                auto missing = (int) chordTemplate.intervals.size() - matched;
                auto extra = numActive - matched; // played pitch classes the template doesn't explain

                // Reward covering the template's own tones, penalise both
                // the ones it's missing and any leftover notes it doesn't
                // account for -- keeps a sparse two-note dyad from matching
                // an unrelated 4-note template just because it partially overlaps.
                auto score = matched * 2 - missing * 2 - extra;

                // Prefer the reading whose root matches the lowest sounding
                // note. Real-world chords are named after their bass note
                // far more often than not, and this is the only signal
                // available to break an otherwise-exact tie between two
                // equally-well-supported readings (e.g. C+D+E+F+A scores
                // identically for Dm7 and Fmaj7 on note coverage alone --
                // whichever one is actually in the bass resolves it). Small
                // enough not to override a genuinely better-fitting root.
                if (root == bassPitchClass)
                    score += 1;

                if (score > bestScore)
                {
                    bestScore = score;
                    bestRoot = root;
                    bestQuality = chordTemplate.qualitySuffix;
                    bestLabel = juce::String(noteNames[root]) + chordTemplate.qualitySuffix;
                }
            }
        }

        // Slash/"on-chord" notation: if the bass note isn't the chord's own
        // root (e.g. a Dm6 voiced with F in the bass), say so explicitly --
        // "Dm6" alone would silently claim D is in the bass when it isn't.
        if (bassPitchClass >= 0 && bassPitchClass != bestRoot)
            bestLabel << "/" << noteNames[bassPitchClass];

        return { bestLabel, bestRoot, bestQuality };
    }

    // Roman-numeral scale degree of chordRoot relative to (keyRoot,
    // keyIsMinor) -- see ChordEstimate::degreeLabel's declaration for the
    // simplifications this makes. Case reflects the CHORD's own quality
    // (qualitySuffix), not the key's diatonic function at that degree.
    juce::String degreeLabelFor(int chordRoot, const char* qualitySuffix, int keyRoot, bool keyIsMinor)
    {
        static const int majorOffsets[7] = { 0, 2, 4, 5, 7, 9, 11 };
        static const int minorOffsets[7] = { 0, 2, 3, 5, 7, 8, 10 };
        static const char* const numeralBases[7] = { "I", "II", "III", "IV", "V", "VI", "VII" };

        auto* offsets = keyIsMinor ? minorOffsets : majorOffsets;
        auto semitoneOffset = ((chordRoot - keyRoot) % 12 + 12) % 12;

        juce::String numeral;
        auto exactIndex = -1;
        for (int i = 0; i < 7; ++i)
            if (offsets[i] == semitoneOffset) { exactIndex = i; break; }

        if (exactIndex >= 0)
        {
            numeral = numeralBases[exactIndex];
        }
        else
        {
            // Chromatic (non-diatonic) root -- flatten the next diatonic
            // degree above it (standard borrowed-chord shorthand, e.g. the
            // semitone between I and II becomes "bII"). Wraps to the
            // octave-up tonic ("bI") if nothing above it in this octave.
            auto aboveIndex = 0;
            for (int i = 0; i < 7; ++i)
                if (offsets[i] > semitoneOffset) { aboveIndex = i; break; }
            numeral = juce::String(juce::CharPointer_UTF8("\xe2\x99\xad")) + numeralBases[aboveIndex]; // U+266D FLAT SIGN
        }

        juce::String quality(qualitySuffix);
        auto isMinorish = quality == "m" || quality == "m7" || quality == "m6" || quality == "dim";
        if (isMinorish)
            numeral = numeral.toLowerCase();
        if (quality == "dim")
            numeral += juce::String(juce::CharPointer_UTF8("\xc2\xb0")); // U+00B0 DEGREE SIGN
        else if (quality == "aug")
            numeral << "+";

        return numeral;
    }
}

std::vector<ChordEstimate> ChordEstimator::estimate(const Project& project, int segmentLengthInSteps,
                                                     int keyRootPitchClass, bool keyIsMinor, bool keyIsSet)
{
    std::vector<ChordEstimate> segments;
    if (segmentLengthInSteps <= 0)
        return segments;

    int totalSteps = 0;
    for (auto& track : project.tracks)
        if (track.includeInChordEstimate)
            totalSteps = juce::jmax(totalSteps, track.clip.effectiveLengthInSteps());
    if (totalSteps == 0)
        return segments;

    std::vector<std::vector<StepActivity>> perTrackActivity;
    perTrackActivity.reserve(project.tracks.size());
    for (auto& track : project.tracks)
    {
        if (!track.includeInChordEstimate)
            continue;
        std::vector<StepActivity> activity(track.clip.steps.size());
        markActiveStepActivity(track.clip.steps, activity);
        perTrackActivity.push_back(std::move(activity));
    }

    for (int segmentStart = 0; segmentStart < totalSteps; segmentStart += segmentLengthInSteps)
    {
        PitchClassSet combined {};
        combined.fill(false);
        auto lowestPitchInSegment = -1;

        for (auto& activity : perTrackActivity)
            for (int s = segmentStart; s < segmentStart + segmentLengthInSteps && s < (int) activity.size(); ++s)
            {
                auto& stepActivity = activity[(size_t) s];
                for (int pc = 0; pc < 12; ++pc)
                    if (stepActivity.pitchClasses[(size_t) pc])
                        combined[(size_t) pc] = true;

                if (stepActivity.lowestPitch >= 0 && (lowestPitchInSegment < 0 || stepActivity.lowestPitch < lowestPitchInSegment))
                    lowestPitchInSegment = stepActivity.lowestPitch;
            }

        auto bassPitchClass = lowestPitchInSegment >= 0 ? ((lowestPitchInSegment % 12) + 12) % 12 : -1;

        auto match = bestChordMatch(combined, bassPitchClass);

        ChordEstimate segment;
        segment.startStep = segmentStart;
        segment.lengthInSteps = juce::jmin(segmentLengthInSteps, totalSteps - segmentStart);
        segment.label = match.label;
        if (keyIsSet && match.root >= 0)
            segment.degreeLabel = degreeLabelFor(match.root, match.qualitySuffix, keyRootPitchClass, keyIsMinor);
        segments.push_back(segment);
    }

    // Merge consecutive segments that landed on the same label into one
    // wider span, so a sustained chord is reported once, at the step it
    // actually starts, instead of repeating the same label on every
    // fine-grained segment.
    std::vector<ChordEstimate> merged;
    for (auto& segment : segments)
    {
        if (!merged.empty() && merged.back().label == segment.label)
            merged.back().lengthInSteps += segment.lengthInSteps;
        else
            merged.push_back(segment);
    }

    // A span shorter than a full beat is a fleeting passing harmony, not a
    // "chord" for progression purposes -- segmentLengthInSteps is always
    // half a beat (see ChordEstimator.h), so a full beat is twice that.
    auto beatLengthInSteps = segmentLengthInSteps * 2;
    for (auto& span : merged)
        if (span.lengthInSteps < beatLengthInSteps)
        {
            span.label = {};
            span.degreeLabel = {};
        }

    // Re-merge: the filter above can turn a real label into "", making it
    // adjacent to a neighbouring silent/filtered span that should combine
    // with it into one wider gap instead of staying two separate entries.
    std::vector<ChordEstimate> result;
    for (auto& span : merged)
    {
        if (!result.empty() && result.back().label == span.label)
            result.back().lengthInSteps += span.lengthInSteps;
        else
            result.push_back(span);
    }

    return result;
}
