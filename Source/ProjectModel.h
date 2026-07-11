#pragma once
#include <JuceHeader.h>
#include <vector>

// One note in a step. A step with multiple StepNotes is a chord.
struct StepNote
{
    int pitch = 60;         // MIDI note number 0-127
    float velocity = 0.8f;  // 0.0-1.0
};

// One slot in a MidiClip's step grid.
struct Step
{
    std::vector<StepNote> notes;   // empty = rest, >1 entry = chord
    int lengthInSteps = 1;
    bool tiedFromPrevious = false; // true = continuation of the previous step's note
};

// An ordered sequence of steps at a fixed step resolution.
class MidiClip
{
public:
    // 12 (not a plain power-of-2 like 4) so both 16th notes (3 steps) AND
    // eighth-note triplets (4 steps -- a triplet doesn't divide evenly into
    // a 4-per-quarter grid at all) are exact integer step counts.
    int stepsPerQuarterNote = 12;
    std::vector<Step> steps;

    int totalLengthInSteps() const;
    double stepDurationSeconds(double bpm) const;

    juce::ValueTree toValueTree() const;
    void loadFromValueTree(const juce::ValueTree& tree);
};

// v1: exactly one clip per track, no multi-clip arrangement.
class Track
{
public:
    juce::String name = "Track 1";
    int midiChannel = 1;
    MidiClip clip;

    // Instrument assignment (Milestone 2). Data only -- the live
    // juce::AudioPluginInstance lives in PlaybackEngine, not here.
    // instrumentDescription.name.isEmpty() means "no plugin, use the
    // built-in fallback synth."
    juce::PluginDescription instrumentDescription;
    juce::MemoryBlock instrumentState;

    juce::ValueTree toValueTree() const;
    void loadFromValueTree(const juce::ValueTree& tree);
};

class Project
{
public:
    double tempoBpm = 120.0;
    std::vector<Track> tracks;

    juce::ValueTree toValueTree() const;
    void loadFromValueTree(const juce::ValueTree& tree);

    static constexpr const char* fileExtension = ".pianoroll";
};
