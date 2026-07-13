#include "KeyboardOverlayComponent.h"
#include "VirtualKeyboardMaps.h"
#include <algorithm>
#include <vector>

namespace
{
    struct KeySpec { char key; float xUnits; };
    struct KeyRow { std::vector<KeySpec> keys; float xOffsetUnits; };

    // Physical QWERTY stagger, in key-widths from each row's own left edge
    // -- matches real keyboard rows closely enough to read at a glance.
    const std::vector<KeyRow>& keyboardRows()
    {
        static const std::vector<KeyRow> rows = {
            { { { '1', 0 }, { '2', 1 }, { '3', 2 }, { '4', 3 }, { '5', 4 }, { '6', 5 }, { '7', 6 },
                { '8', 7 }, { '9', 8 }, { '0', 9 }, { '-', 10 }, { '=', 11 } }, 0.0f },
            { { { 'Q', 0 }, { 'W', 1 }, { 'E', 2 }, { 'R', 3 }, { 'T', 4 }, { 'Y', 5 }, { 'U', 6 },
                { 'I', 7 }, { 'O', 8 }, { 'P', 9 }, { '[', 10 }, { ']', 11 } }, 0.5f },
            { { { 'A', 0 }, { 'S', 1 }, { 'D', 2 }, { 'F', 3 }, { 'G', 4 }, { 'H', 5 }, { 'J', 6 },
                { 'K', 7 }, { 'L', 8 }, { ';', 9 }, { '\'', 10 } }, 0.75f },
            { { { 'Z', 0 }, { 'X', 1 }, { 'C', 2 }, { 'V', 3 }, { 'B', 4 }, { 'N', 5 }, { 'M', 6 },
                { ',', 7 }, { '.', 8 }, { '/', 9 } }, 1.25f },
        };
        return rows;
    }

    // Drum-grid keys are the SHIFTED symbols (see virtualDrumKeyMap()'s
    // comment) -- e.g. the physical ',' key shows as '<' here, so its label
    // needs to be looked up under the row's UNSHIFTED character instead.
    // Mirrors the exact unshifted->shifted mapping the physical keys
    // produce with Shift held, for the punctuation/digit keys that change.
    char toShiftedForDrumLookup(char unshifted)
    {
        switch (unshifted)
        {
            case ',': return '<';
            case '.': return '>';
            case '/': return '?';
            case ';': return ':';
            case '7': return '&';
            case '8': return '*';
            case '9': return '(';
            case '0': return ')';
            default:  return unshifted; // letters are unaffected by Shift for this purpose
        }
    }

    juce::String noteNameFor(int pitch)
    {
        return juce::MidiMessage::getMidiNoteName(pitch, true, true, 3);
    }

    // The single source of truth for "what does pressing this key do right
    // now" -- mirrors MainEditorComponent::keyPressed()'s dispatch exactly
    // (same modifier-tier priority: Ctrl, then Cmd, then Shift+Option, then
    // Option, then Shift, then plain/mode-aware), but as a pure lookup that
    // never executes anything. NOTE: this has to be kept in sync by hand
    // whenever keyPressed() changes -- same accepted tradeoff as
    // ShortcutHelpBarComponent's static help text.
    std::map<char, juce::String> computeKeyLabels(const juce::ModifierKeys& mods, bool isSessionView,
                                                   bool isHumActive, int transposeSemitones)
    {
        std::map<char, juce::String> labels;

        if (mods.isCtrlDown())
        {
            if (mods.isShiftDown())
            {
                labels['Z'] = "Vel-";
                labels['X'] = "Vel+";

                std::vector<std::pair<char, int>> sorted(virtualDrumKeyMap().begin(), virtualDrumKeyMap().end());
                std::sort(sorted.begin(), sorted.end(), [](auto& a, auto& b) { return a.second < b.second; });
                for (size_t i = 0; i < sorted.size(); ++i)
                    labels[sorted[i].first] = "Pad " + juce::String((int) i + 1);
            }
            else
            {
                labels['Z'] = "Transp-";
                labels['X'] = "Transp+";
                labels['F'] = "Sustain";
                for (auto& [ch, offset] : virtualKeyboardKeyMap())
                    labels[ch] = noteNameFor(juce::jlimit(0, 127, 60 + transposeSemitones + offset));
            }
            return labels;
        }

        if (mods.isCommandDown())
        {
            if (mods.isAltDown())
            {
                labels['3'] = "Scroll Up";
                labels['E'] = "Scroll Dn";
            }
            else if (mods.isShiftDown())
            {
                labels['W'] = "Sel Up+";
                labels['E'] = "Sel Dn+";
                labels['S'] = "Save As";
                labels['Z'] = "Redo";
            }
            else
            {
                labels['S'] = "Save";
                labels['O'] = "Open";
                labels['N'] = "New";
                labels['T'] = "Add Track";
                labels['Y'] = "Instrument";
                labels['P'] = "Plugin Ed.";
                labels[','] = "Audio Set.";
                labels['G'] = "Prev Trk";
                labels['B'] = "Next Trk";
                labels['3'] = "Zoom V-";
                labels['E'] = "Zoom V+";
                labels['F'] = "Zoom H+";
                labels['D'] = "Zoom H-";
                labels['M'] = "Scale";
                labels['C'] = "Loop End";
                labels['A'] = "Chord Trk";
                labels['Z'] = "Undo";
                labels['K'] = "Keyboard";
            }
            return labels;
        }

        if (mods.isShiftDown() && mods.isAltDown())
        {
            labels['3'] = labels['W'] = "Oct Up";
            labels['E'] = labels['R'] = "Oct Dn";
            return labels;
        }

        if (mods.isAltDown())
        {
            labels['T'] = "Pitch Up";
            labels['G'] = "Pitch Dn";
            labels['Z'] = "Tempo-";
            labels['X'] = "Tempo+";
            return labels;
        }

        if (mods.isShiftDown())
        {
            labels['Z'] = "Duration-";
            labels['X'] = "Duration+";
            labels['F'] = "Jump Fwd";
            labels['D'] = "Jump Back";
            labels['3'] = labels['W'] = "Prev Trk";
            labels['E'] = "Next Trk";
            labels['C'] = "Loop Start";
            return labels;
        }

        // Plain, mode-aware.
        if (isSessionView)
        {
            labels['D'] = "Prev Slot";
            labels['F'] = "Next Slot";
            labels['A'] = "Del Clip";
            labels['G'] = "Capture";
            labels['T'] = "Load Slot";
            labels['Z'] = "Stop Trk";
            labels['X'] = "Launch";
            labels['B'] = "Duplicate";
            labels['3'] = "Prev Trk";
            labels['E'] = "Next Trk";
        }
        else
        {
            labels['D'] = "Del/Prev";
            labels['F'] = "Place/Next";
            labels['A'] = "Clear Step";
            labels['G'] = "Del+Back";
            labels['T'] = "Tie";
            labels['Z'] = "Oct Dn";
            labels['X'] = "Oct Up";
            labels['B'] = "Clip End";
            labels['3'] = isHumActive ? "Hum Up" : "Sel Up";
            labels['E'] = isHumActive ? "Hum Dn" : "Sel Dn";
        }
        labels['V'] = "Hum";
        labels['C'] = "Loop";
        labels['S'] = "Session";
        labels['W'] = "Metronome";

        return labels;
    }
}

KeyboardOverlayComponent::KeyboardOverlayComponent(std::function<bool()> isSessionViewIn,
                                                     std::function<bool()> isHumActiveIn,
                                                     std::function<int()> transposeSemitonesIn,
                                                     std::function<std::vector<int>()> highlightedKeyCodesIn)
    : isSessionView(std::move(isSessionViewIn)),
      isHumActive(std::move(isHumActiveIn)),
      transposeSemitones(std::move(transposeSemitonesIn)),
      highlightedKeyCodes(std::move(highlightedKeyCodesIn))
{
    setSize(640, 300);
    startTimerHz(15);
}

void KeyboardOverlayComponent::timerCallback()
{
    auto mods = juce::ModifierKeys::getCurrentModifiers();
    auto session = isSessionView();
    auto hum = isHumActive();
    auto highlighted = highlightedKeyCodes();

    if (mods != lastMods || session != lastSessionView || hum != lastHumActive || highlighted != lastHighlighted)
    {
        lastMods = mods;
        lastSessionView = session;
        lastHumActive = hum;
        lastHighlighted = std::move(highlighted);
        repaint();
    }
}

void KeyboardOverlayComponent::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colours::black);

    auto mods = juce::ModifierKeys::getCurrentModifiers();
    auto drumTier = mods.isCtrlDown() && mods.isShiftDown();
    auto labels = computeKeyLabels(mods, isSessionView(), isHumActive(), transposeSemitones());
    auto highlighted = highlightedKeyCodes(); // fresh, not the cached lastHighlighted (that's purely for timerCallback()'s change-detection)

    constexpr float keyUnit = 46.0f;
    constexpr float keySize = 42.0f;
    constexpr float topMargin = 10.0f;

    g.setFont(juce::FontOptions(10.0f));

    for (size_t rowIndex = 0; rowIndex < keyboardRows().size(); ++rowIndex)
    {
        auto& row = keyboardRows()[rowIndex];
        auto y = topMargin + (float) rowIndex * keyUnit;

        for (auto& spec : row.keys)
        {
            auto x = 10.0f + (row.xOffsetUnits + spec.xUnits) * keyUnit;
            auto bounds = juce::Rectangle<float>(x, y, keySize, keySize);

            // Ctrl (+Shift) tier keys not in either performance map show
            // dim/empty -- everything else in the grid is inert while
            // playing notes, same as the real app (see keyPressed()'s
            // Ctrl-guard, which claims every Ctrl combo whether or not it
            // maps to a note).
            auto lookupKey = (mods.isCtrlDown() && drumTier) ? toShiftedForDrumLookup(spec.key) : spec.key;
            auto it = labels.find(lookupKey);
            auto hasLabel = it != labels.end();

            // Currently-highlighted keys -- a distinct bright colour
            // overriding the normal tier colour, so it's obvious at a
            // glance what you're playing/just pressed. Can be MORE THAN
            // ONE at once: a held chord on the virtual keyboard highlights
            // every one of its keys simultaneously (see
            // MainEditorComponent's highlightedKeyCodes getter, which
            // unions currently-held note/drum keys with the last plain
            // editing-command keypress -- the latter stays lit until the
            // next one, same "persists until overwritten" convention
            // ShortcutHelpBarComponent's last-action text uses). Compared
            // against lookupKey, not spec.key -- drum-tier note presses are
            // tracked by their SHIFTED character (see
            // pollVirtualKeyboardInput()'s pollOneMap, which stores
            // whatever virtualDrumKeyMap() itself is keyed by), same as the
            // label lookup just above.
            auto isLastPressed = std::find(highlighted.begin(), highlighted.end(), (int) lookupKey) != highlighted.end();

            if (isLastPressed)
            {
                g.setColour(juce::Colours::yellow);
            }
            else if (mods.isCtrlDown())
            {
                g.setColour(hasLabel ? (drumTier ? juce::Colours::mediumpurple : juce::Colours::mediumseagreen)
                                      : juce::Colours::grey.withAlpha(0.15f));
            }
            else
            {
                g.setColour(hasLabel ? juce::Colours::dodgerblue.withAlpha(0.55f) : juce::Colours::grey.withAlpha(0.15f));
            }
            g.fillRoundedRectangle(bounds, 4.0f);

            g.setColour(juce::Colours::white.withAlpha(0.25f));
            g.drawRoundedRectangle(bounds, 4.0f, 1.0f);

            if (hasLabel)
            {
                g.setColour(isLastPressed ? juce::Colours::black : juce::Colours::white);
                g.drawFittedText(it->second, bounds.toNearestInt().reduced(2), juce::Justification::centred, 3, 0.7f);
            }
            else
            {
                g.setColour(isLastPressed ? juce::Colours::black : juce::Colours::grey);
                g.drawText(juce::String::charToString((juce::juce_wchar) spec.key),
                           bounds, juce::Justification::centred);
            }
        }
    }

    // Space / Tab -- always the same meaning regardless of modifier tier or
    // mode, so drawn separately rather than routed through computeKeyLabels().
    auto bottomY = topMargin + (float) keyboardRows().size() * keyUnit + 6.0f;
    auto spaceBounds = juce::Rectangle<float>(10.0f + 1.25f * keyUnit, bottomY, keyUnit * 5.0f, keySize);
    auto tabBounds = juce::Rectangle<float>(10.0f, bottomY, keyUnit * 1.0f, keySize);

    const std::vector<std::pair<juce::Rectangle<float>, juce::String>> extraKeys = {
        { spaceBounds, "Advance" }, { tabBounds, "Play" }
    };
    for (auto& [bounds, label] : extraKeys)
    {
        g.setColour(juce::Colours::dodgerblue.withAlpha(0.55f));
        g.fillRoundedRectangle(bounds, 4.0f);
        g.setColour(juce::Colours::white.withAlpha(0.25f));
        g.drawRoundedRectangle(bounds, 4.0f, 1.0f);
        g.setColour(juce::Colours::white);
        g.drawFittedText(label, bounds.toNearestInt().reduced(2), juce::Justification::centred, 2, 0.7f);
    }

    // Modifier status strip -- filled when currently held, so it's obvious
    // at a glance which tier the labels above belong to.
    auto modY = bottomY + keySize + 10.0f;
    struct ModifierBadge { const char* name; bool held; };
    std::vector<ModifierBadge> badges = {
        { "SHIFT", mods.isShiftDown() }, { "CTRL", mods.isCtrlDown() },
        { "OPTION", mods.isAltDown() }, { "CMD", mods.isCommandDown() },
    };

    auto badgeX = 10.0f;
    g.setFont(juce::FontOptions(12.0f, juce::Font::bold));
    for (auto& badge : badges)
    {
        auto width = 80.0f;
        auto bounds = juce::Rectangle<float>(badgeX, modY, width, 26.0f);

        if (badge.held)
        {
            g.setColour(juce::Colours::orange);
            g.fillRoundedRectangle(bounds, 4.0f);
            g.setColour(juce::Colours::black);
        }
        else
        {
            g.setColour(juce::Colours::grey.withAlpha(0.5f));
            g.drawRoundedRectangle(bounds, 4.0f, 1.0f);
            g.setColour(juce::Colours::grey);
        }
        g.drawText(badge.name, bounds, juce::Justification::centred);
        badgeX += width + 8.0f;
    }

    g.setColour(juce::Colours::lightgrey);
    g.setFont(juce::FontOptions(12.0f));
    g.drawText(juce::String(isSessionView() ? "Session View" : "Piano Roll") + (isHumActive() ? "  |  HUM ON" : ""),
               juce::Rectangle<float>(badgeX, modY, 200.0f, 26.0f), juce::Justification::centredLeft);
}
