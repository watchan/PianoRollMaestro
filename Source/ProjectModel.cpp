#include "ProjectModel.h"

int MidiClip::effectiveLengthInSteps() const
{
    return explicitLengthInSteps > 0 ? explicitLengthInSteps : (int) steps.size();
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
        tree.setProperty("durationSteps", note.durationSteps, nullptr);
        return tree;
    }

    StepNote stepNoteFromValueTree(const juce::ValueTree& tree)
    {
        StepNote note;
        note.pitch = (int) tree.getProperty("pitch", 60);
        note.velocity = (float) (double) tree.getProperty("velocity", 0.8);
        note.durationSteps = (int) tree.getProperty("durationSteps", -1);
        return note;
    }

    juce::ValueTree stepToValueTree(const Step& step)
    {
        juce::ValueTree tree("Step");
        tree.setProperty("lengthInSteps", step.lengthInSteps, nullptr);
        tree.setProperty("tiedFromPrevious", step.tiedFromPrevious, nullptr);
        tree.setProperty("quantizedFromStep", step.quantizedFromStep, nullptr);

        for (auto& note : step.notes)
            tree.appendChild(stepNoteToValueTree(note), nullptr);

        return tree;
    }

    Step stepFromValueTree(const juce::ValueTree& tree)
    {
        Step step;
        step.lengthInSteps = (int) tree.getProperty("lengthInSteps", 1);
        step.tiedFromPrevious = (bool) tree.getProperty("tiedFromPrevious", false);
        step.quantizedFromStep = (int) tree.getProperty("quantizedFromStep", -1);

        for (int i = 0; i < tree.getNumChildren(); ++i)
            step.notes.push_back(stepNoteFromValueTree(tree.getChild(i)));

        return step;
    }

    // Shared by pitchBendPoints/filterCutoffPoints -- both use the exact
    // same {stepIndex, value} shape, just wrapped in a differently-named
    // container (containerName) so they don't collide with each other or
    // with SustainEvents when read back.
    juce::ValueTree automationPointsToValueTree(const juce::String& containerName, const std::vector<AutomationPoint>& points)
    {
        juce::ValueTree tree(containerName);
        for (auto& point : points)
        {
            juce::ValueTree pointTree("Point");
            pointTree.setProperty("stepIndex", point.stepIndex, nullptr);
            pointTree.setProperty("value", point.value, nullptr);
            pointTree.setProperty("curveType", (int) point.curveType, nullptr);
            pointTree.setProperty("curveAmount", point.curveAmount, nullptr);
            tree.appendChild(pointTree, nullptr);
        }
        return tree;
    }

    std::vector<AutomationPoint> automationPointsFromValueTree(const juce::ValueTree& tree)
    {
        std::vector<AutomationPoint> points;
        for (int i = 0; i < tree.getNumChildren(); ++i)
        {
            auto pointTree = tree.getChild(i);
            AutomationPoint point;
            point.stepIndex = (int) pointTree.getProperty("stepIndex", 0);
            point.value = (int) pointTree.getProperty("value", 0);
            if (pointTree.hasProperty("curveAmount"))
            {
                // Current format -- curveType is already the 2-way
                // Curve/Step enum, curveAmount stored directly.
                point.curveType = (AutomationCurveType) (int) pointTree.getProperty("curveType", 0);
                point.curveAmount = (float) (double) pointTree.getProperty("curveAmount", 0.0);
            }
            else
            {
                // A file saved under the old 4-way discrete Linear/EaseIn/
                // EaseOut/Step curveType (0/1/2/3), before curveAmount
                // existed -- map each onto the new Curve/Step +
                // curveAmount representation so old projects keep exactly
                // their prior shape. EaseIn/EaseOut were always a fixed
                // exponent-2 power curve, which solves to curveAmount =
                // 1/automationCurveAmountExponentScale under the new
                // formula (exponent = 1 + |amount| * scale).
                auto legacyCurveType = (int) pointTree.getProperty("curveType", 0);
                auto legacyEaseAmount = (float) (1.0 / automationCurveAmountExponentScale);
                switch (legacyCurveType)
                {
                    case 1: point.curveType = AutomationCurveType::Curve; point.curveAmount = legacyEaseAmount; break;  // old EaseIn
                    case 2: point.curveType = AutomationCurveType::Curve; point.curveAmount = -legacyEaseAmount; break; // old EaseOut
                    case 3: point.curveType = AutomationCurveType::Step; point.curveAmount = 0.0f; break;               // old Step
                    default: point.curveType = AutomationCurveType::Curve; point.curveAmount = 0.0f; break;             // old Linear
                }
            }
            points.push_back(point);
        }
        return points;
    }

    juce::ValueTree parameterLanesToValueTree(const std::vector<ParameterAutomationLane>& lanes)
    {
        juce::ValueTree tree("ParameterLanes");
        for (auto& lane : lanes)
        {
            juce::ValueTree laneTree("Lane");
            laneTree.setProperty("parameterID", lane.parameterID, nullptr);
            laneTree.setProperty("parameterName", lane.parameterName, nullptr);
            for (auto& point : lane.points)
            {
                juce::ValueTree pointTree("Point");
                pointTree.setProperty("stepIndex", point.stepIndex, nullptr);
                pointTree.setProperty("value", point.value, nullptr);
                pointTree.setProperty("curveType", (int) point.curveType, nullptr);
                pointTree.setProperty("curveAmount", point.curveAmount, nullptr);
                laneTree.appendChild(pointTree, nullptr);
            }
            tree.appendChild(laneTree, nullptr);
        }
        return tree;
    }

    std::vector<ParameterAutomationLane> parameterLanesFromValueTree(const juce::ValueTree& tree)
    {
        std::vector<ParameterAutomationLane> lanes;
        for (int i = 0; i < tree.getNumChildren(); ++i)
        {
            auto laneTree = tree.getChild(i);
            ParameterAutomationLane lane;
            lane.parameterID = laneTree.getProperty("parameterID", juce::String());
            lane.parameterName = laneTree.getProperty("parameterName", juce::String());
            for (int p = 0; p < laneTree.getNumChildren(); ++p)
            {
                auto pointTree = laneTree.getChild(p);
                ParameterAutomationPoint point;
                point.stepIndex = (int) pointTree.getProperty("stepIndex", 0);
                point.value = (float) (double) pointTree.getProperty("value", 0.0);
                point.curveType = (AutomationCurveType) (int) pointTree.getProperty("curveType", 0);
                point.curveAmount = (float) (double) pointTree.getProperty("curveAmount", 0.0);
                lane.points.push_back(point);
            }
            lanes.push_back(lane);
        }
        return lanes;
    }
}

juce::ValueTree MidiClip::toValueTree() const
{
    juce::ValueTree tree("Clip");
    tree.setProperty("stepsPerQuarterNote", stepsPerQuarterNote, nullptr);
    tree.setProperty("explicitLengthInSteps", explicitLengthInSteps, nullptr);

    for (auto& step : steps)
        tree.appendChild(stepToValueTree(step), nullptr);

    // Wrapped in its own container (same reasoning as Track::sceneClips)
    // so an old file with only bare "Step" children still loads correctly.
    juce::ValueTree sustainEventsTree("SustainEvents");
    for (auto& event : sustainPedalEvents)
    {
        juce::ValueTree eventTree("Event");
        eventTree.setProperty("stepIndex", event.stepIndex, nullptr);
        eventTree.setProperty("pedalDown", event.pedalDown, nullptr);
        sustainEventsTree.appendChild(eventTree, nullptr);
    }
    tree.appendChild(sustainEventsTree, nullptr);

    tree.appendChild(automationPointsToValueTree("PitchBendPoints", pitchBendPoints), nullptr);
    tree.appendChild(automationPointsToValueTree("FilterCutoffPoints", filterCutoffPoints), nullptr);
    tree.appendChild(parameterLanesToValueTree(parameterLanes), nullptr);

    return tree;
}

void MidiClip::loadFromValueTree(const juce::ValueTree& tree)
{
    stepsPerQuarterNote = (int) tree.getProperty("stepsPerQuarterNote", 12);
    explicitLengthInSteps = (int) tree.getProperty("explicitLengthInSteps", 0);

    steps.clear();
    for (int i = 0; i < tree.getNumChildren(); ++i)
    {
        auto child = tree.getChild(i);
        if (child.hasType("Step"))
            steps.push_back(stepFromValueTree(child));
    }

    sustainPedalEvents.clear();
    if (auto sustainEventsTree = tree.getChildWithName("SustainEvents"); sustainEventsTree.isValid())
    {
        for (int i = 0; i < sustainEventsTree.getNumChildren(); ++i)
        {
            auto eventTree = sustainEventsTree.getChild(i);
            sustainPedalEvents.push_back({ (int) eventTree.getProperty("stepIndex", 0),
                                            (bool) eventTree.getProperty("pedalDown", false) });
        }
    }

    pitchBendPoints = automationPointsFromValueTree(tree.getChildWithName("PitchBendPoints"));
    filterCutoffPoints = automationPointsFromValueTree(tree.getChildWithName("FilterCutoffPoints"));
    parameterLanes = parameterLanesFromValueTree(tree.getChildWithName("ParameterLanes"));
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
    includeInChordEstimate = (bool) tree.getProperty("includeInChordEstimate", false);

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
    tree.setProperty("metronomeEnabled", metronomeEnabled, nullptr);
    tree.setProperty("countInEnabled", countInEnabled, nullptr);

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
    metronomeEnabled = (bool) tree.getProperty("metronomeEnabled", false);
    countInEnabled = (bool) tree.getProperty("countInEnabled", true);

    tracks.clear();
    for (int i = 0; i < tree.getNumChildren(); ++i)
    {
        Track track;
        track.loadFromValueTree(tree.getChild(i));
        tracks.push_back(track);
    }
}
