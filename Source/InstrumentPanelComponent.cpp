#include "InstrumentPanelComponent.h"

bool InstrumentPanelComponent::SearchEditor::keyPressed(const juce::KeyPress& key)
{
    // Same g/b mnemonic as this app's usual prev/next pairing elsewhere,
    // Cmd-modified so it doesn't collide with typing a search query that
    // happens to contain 'g' or 'b' (and to match every other modifier
    // shortcut in the app, which is always Cmd, never Ctrl). Up/Down arrow
    // keys work too -- an explicit exception to the app's usual no-arrow-
    // keys rule, made for this candidate-list picker specifically (see
    // this class's own declaration).
    if ((key.getModifiers().isCommandDown() && key.isKeyCode('G')) || key == juce::KeyPress::upKey) { if (onMoveHighlight) onMoveHighlight(-1); return true; }
    if ((key.getModifiers().isCommandDown() && key.isKeyCode('B')) || key == juce::KeyPress::downKey) { if (onMoveHighlight) onMoveHighlight(1); return true; }
    // Tab also confirms the highlighted instrument, same as Enter -- Tab is
    // already this app's "confirm/advance" key elsewhere (e.g. play/stop),
    // so accepting it here too avoids forcing a reach for Enter specifically.
    if (key == juce::KeyPress::returnKey || key == juce::KeyPress::tabKey) { if (onEnterKey) onEnterKey(); return true; }
    // Cmd+W (macOS's standard "close window" shortcut) instead of Escape --
    // Escape is a hand-off-home-row reach; Cmd+W stays close and is also
    // the idiomatic Mac convention for this exact action.
    if (key.getModifiers().isCommandDown() && key.isKeyCode('W')) { if (onEscapeKey) onEscapeKey(); return true; }
    if (key == juce::KeyPress::escapeKey) { if (onEscapeKey) onEscapeKey(); return true; }

    return juce::TextEditor::keyPressed(key);
}

InstrumentPanelComponent::InstrumentPanelComponent(PluginHost& pluginHostIn,
                                                     juce::String trackNameIn,
                                                     juce::String currentInstrumentNameIn,
                                                     std::function<void(const juce::PluginDescription&)> onLoadIn,
                                                     std::function<void()> onShowEditorIn,
                                                     std::function<void()> onRemoveIn,
                                                     std::function<void()> onRequestCloseIn)
    : pluginHost(pluginHostIn),
      trackName(std::move(trackNameIn)),
      currentInstrumentName(std::move(currentInstrumentNameIn)),
      onLoad(std::move(onLoadIn)),
      onShowEditor(std::move(onShowEditorIn)),
      onRemove(std::move(onRemoveIn)),
      onRequestClose(std::move(onRequestCloseIn))
{
    headerLabel.setText(trackName + ": " + (currentInstrumentName.isEmpty() ? "(built-in synth)" : currentInstrumentName),
                         juce::dontSendNotification);
    addAndMakeVisible(headerLabel);

    addAndMakeVisible(searchEditor);
    searchEditor.setTextToShowWhenEmpty("Type to search by name or manufacturer...", juce::Colours::grey);
    searchEditor.onTextChange = [this] { updateFilter(); };
    searchEditor.onMoveHighlight = [this](int delta) { moveHighlight(delta); };
    searchEditor.onEnterKey = [this] { loadHighlighted(); };
    searchEditor.onEscapeKey = [this] { if (onRequestClose) onRequestClose(); };

    addAndMakeVisible(scanButton);
    scanButton.onClick = [this] { scanClicked(); };

    addAndMakeVisible(showEditorButton);
    showEditorButton.setEnabled(currentInstrumentName.isNotEmpty());
    showEditorButton.onClick = [this] { if (onShowEditor) onShowEditor(); };

    addAndMakeVisible(removeButton);
    removeButton.setEnabled(currentInstrumentName.isNotEmpty());
    removeButton.onClick = [this] { if (onRemove) onRemove(); };

    addAndMakeVisible(listBox);
    listBox.setRowHeight(22);

    updateFilter();

    setSize(460, 520);
}

void InstrumentPanelComponent::visibilityChanged()
{
    if (isVisible())
        searchEditor.grabKeyboardFocus();
}

void InstrumentPanelComponent::scanClicked()
{
    scanButton.setEnabled(false);
    scanButton.setButtonText("Scanning...");

    // Blocking scan -- acceptable for a manual, infrequent button press on a
    // personal tool; see PluginHost::scanForInstruments for the crash-
    // resilience notes (incremental save + dead man's pedal).
    pluginHost.scanForInstruments();

    scanButton.setEnabled(true);
    scanButton.setButtonText("Scan");
    updateFilter();
    searchEditor.grabKeyboardFocus();
}

void InstrumentPanelComponent::updateFilter()
{
    filteredInstruments.clear();

    auto query = searchEditor.getText().trim().toLowerCase();

    for (auto& description : pluginHost.getKnownInstruments())
    {
        if (query.isEmpty()
            || description.name.toLowerCase().contains(query)
            || description.manufacturerName.toLowerCase().contains(query))
        {
            filteredInstruments.add(description);
        }
    }

    highlightedRow = filteredInstruments.isEmpty() ? -1 : 0;

    listBox.updateContent();
    listBox.selectRow(highlightedRow);
}

void InstrumentPanelComponent::moveHighlight(int delta)
{
    if (filteredInstruments.isEmpty())
        return;

    highlightedRow = juce::jlimit(0, filteredInstruments.size() - 1, highlightedRow + delta);
    listBox.selectRow(highlightedRow);
    listBox.scrollToEnsureRowIsOnscreen(highlightedRow);
}

void InstrumentPanelComponent::loadHighlighted()
{
    if (highlightedRow < 0 || highlightedRow >= filteredInstruments.size())
        return;

    if (onLoad)
        onLoad(filteredInstruments.getReference(highlightedRow));
}

int InstrumentPanelComponent::getNumRows()
{
    return filteredInstruments.size();
}

void InstrumentPanelComponent::paintListBoxItem(int rowNumber, juce::Graphics& g, int width, int height, bool rowIsSelected)
{
    if (rowIsSelected)
        g.fillAll(juce::Colours::dodgerblue.withAlpha(0.4f));

    if (rowNumber < 0 || rowNumber >= filteredInstruments.size())
        return;

    auto& description = filteredInstruments.getReference(rowNumber);

    g.setColour(juce::Colours::white);
    g.drawText(description.name + "  --  " + description.manufacturerName + "  (" + description.pluginFormatName + ")",
               6, 0, width - 12, height, juce::Justification::centredLeft);
}

void InstrumentPanelComponent::listBoxItemDoubleClicked(int row, const juce::MouseEvent&)
{
    if (row < 0 || row >= filteredInstruments.size())
        return;

    highlightedRow = row;
    loadHighlighted();
}

void InstrumentPanelComponent::resized()
{
    auto bounds = getLocalBounds().reduced(8);

    headerLabel.setBounds(bounds.removeFromTop(24));
    bounds.removeFromTop(4);
    searchEditor.setBounds(bounds.removeFromTop(28));
    bounds.removeFromTop(8);

    auto buttonRow = bounds.removeFromBottom(28);
    scanButton.setBounds(buttonRow.removeFromLeft(100));
    buttonRow.removeFromLeft(8);
    showEditorButton.setBounds(buttonRow.removeFromLeft(110));
    buttonRow.removeFromLeft(8);
    removeButton.setBounds(buttonRow.removeFromLeft(90));
    bounds.removeFromBottom(8);

    listBox.setBounds(bounds);
}

void InstrumentPanelComponent::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colours::black.brighter(0.05f));
}
