#include "ProjectModel.h"

int MidiClip::totalLengthInSteps() const
{
    int total = 0;
    for (auto& step : steps)
        if (!step.tiedFromPrevious)
            total += step.lengthInSteps;
    return total;
}

double MidiClip::stepDurationSeconds(double bpm) const
{
    return 60.0 / bpm / (double) stepsPerQuarterNote;
}

namespace
{
    juce::ValueTree stepNoteToValueTree(const StepNote& note)
    {
        juce::ValueTree tree("Note");
        tree.setProperty("pitch", note.pitch, nullptr);
        tree.setProperty("velocity", (double) note.velocity, nullptr);
        return tree;
    }

    StepNote stepNoteFromValueTree(const juce::ValueTree& tree)
    {
        StepNote note;
        note.pitch = (int) tree.getProperty("pitch", 60);
        note.velocity = (float) (double) tree.getProperty("velocity", 0.8);
        return note;
    }

    juce::ValueTree stepToValueTree(const Step& step)
    {
        juce::ValueTree tree("Step");
        tree.setProperty("lengthInSteps", step.lengthInSteps, nullptr);
        tree.setProperty("tiedFromPrevious", step.tiedFromPrevious, nullptr);

        for (auto& note : step.notes)
            tree.appendChild(stepNoteToValueTree(note), nullptr);

        return tree;
    }

    Step stepFromValueTree(const juce::ValueTree& tree)
    {
        Step step;
        step.lengthInSteps = (int) tree.getProperty("lengthInSteps", 1);
        step.tiedFromPrevious = (bool) tree.getProperty("tiedFromPrevious", false);

        for (int i = 0; i < tree.getNumChildren(); ++i)
            step.notes.push_back(stepNoteFromValueTree(tree.getChild(i)));

        return step;
    }
}

juce::ValueTree MidiClip::toValueTree() const
{
    juce::ValueTree tree("Clip");
    tree.setProperty("stepsPerQuarterNote", stepsPerQuarterNote, nullptr);

    for (auto& step : steps)
        tree.appendChild(stepToValueTree(step), nullptr);

    return tree;
}

void MidiClip::loadFromValueTree(const juce::ValueTree& tree)
{
    stepsPerQuarterNote = (int) tree.getProperty("stepsPerQuarterNote", 12);

    steps.clear();
    for (int i = 0; i < tree.getNumChildren(); ++i)
        steps.push_back(stepFromValueTree(tree.getChild(i)));
}

juce::ValueTree Track::toValueTree() const
{
    juce::ValueTree tree("Track");
    tree.setProperty("name", name, nullptr);
    tree.setProperty("midiChannel", midiChannel, nullptr);
    tree.setProperty("playingSlotIndex", playingSlotIndex, nullptr);
    tree.setProperty("includeInChordEstimate", includeInChordEstimate, nullptr);
    tree.appendChild(clip.toValueTree(), nullptr);

    // Session View slots, wrapped in their own container so an old file
    // with only the single "Clip" child above still loads correctly (that
    // child is the editing buffer, unrelated to this container).
    juce::ValueTree sceneClipsTree("SceneClips");
    for (auto& slotClip : sceneClips)
        sceneClipsTree.appendChild(slotClip.toValueTree(), nullptr);
    tree.appendChild(sceneClipsTree, nullptr);

    if (instrumentDescription.name.isNotEmpty())
    {
        if (auto descriptionXml = instrumentDescription.createXml())
        {
            juce::ValueTree instrumentTree("Instrument");
            instrumentTree.appendChild(juce::ValueTree::fromXml(*descriptionXml), nullptr);
            instrumentTree.setProperty("state", instrumentState.toBase64Encoding(), nullptr);
            tree.appendChild(instrumentTree, nullptr);
        }
    }

    return tree;
}

void Track::loadFromValueTree(const juce::ValueTree& tree)
{
    name = tree.getProperty("name", "Track 1").toString();
    midiChannel = (int) tree.getProperty("midiChannel", 1);
    playingSlotIndex = (int) tree.getProperty("playingSlotIndex", -1);
    includeInChordEstimate = (bool) tree.getProperty("includeInChordEstimate", true);

    auto clipTree = tree.getChildWithName("Clip");
    if (clipTree.isValid())
        clip.loadFromValueTree(clipTree);

    // Absent on an old file (pre-Session-View) -- sceneClips just stays
    // empty, same as a freshly-constructed Track.
    sceneClips.clear();
    auto sceneClipsTree = tree.getChildWithName("SceneClips");
    if (sceneClipsTree.isValid())
        for (int i = 0; i < sceneClipsTree.getNumChildren(); ++i)
        {
            MidiClip slotClip;
            slotClip.loadFromValueTree(sceneClipsTree.getChild(i));
            sceneClips.push_back(slotClip);
        }

    instrumentDescription = juce::PluginDescription();
    instrumentState.reset();

    auto instrumentTree = tree.getChildWithName("Instrument");
    if (instrumentTree.isValid() && instrumentTree.getNumChildren() > 0)
    {
        if (auto descriptionXml = instrumentTree.getChild(0).createXml())
            instrumentDescription.loadFromXml(*descriptionXml);

        instrumentState.fromBase64Encoding(instrumentTree.getProperty("state", "").toString());
    }
}

juce::ValueTree Project::toValueTree() const
{
    juce::ValueTree tree("Project");
    tree.setProperty("tempoBpm", tempoBpm, nullptr);
    tree.setProperty("loopStartStep", loopStartStep, nullptr);
    tree.setProperty("loopEndStep", loopEndStep, nullptr);
    tree.setProperty("loopEnabled", loopEnabled, nullptr);

    for (auto& track : tracks)
        tree.appendChild(track.toValueTree(), nullptr);

    return tree;
}

void Project::loadFromValueTree(const juce::ValueTree& tree)
{
    tempoBpm = (double) tree.getProperty("tempoBpm", 120.0);
    loopStartStep = (int) tree.getProperty("loopStartStep", 0);
    loopEndStep = (int) tree.getProperty("loopEndStep", 0);
    loopEnabled = (bool) tree.getProperty("loopEnabled", false);

    tracks.clear();
    for (int i = 0; i < tree.getNumChildren(); ++i)
    {
        Track track;
        track.loadFromValueTree(tree.getChild(i));
        tracks.push_back(track);
    }
}
