#pragma once
#include <JuceHeader.h>

// Owns plugin format/scanning machinery. In-process scanning only (no
// crash-safe subprocess relaunch, per Milestone 2's scope) -- acceptable
// for a personal tool where the plugin set is known and scanned rarely.
class PluginHost
{
public:
    PluginHost();

    // Synchronous, in-process scan of the default AU/VST3 search locations.
    // Populates getKnownInstruments() and persists the result to disk so
    // subsequent launches don't have to rescan.
    void scanForInstruments();

    const juce::Array<juce::PluginDescription>& getKnownInstruments() const { return knownInstruments; }

    void createInstrument(const juce::PluginDescription& description,
                           double sampleRate,
                           int blockSize,
                           juce::AudioPluginFormat::PluginCreationCallback callback);

private:
    void loadCache();
    void saveCache();
    void refreshKnownInstruments();

    juce::AudioPluginFormatManager formatManager;
    juce::KnownPluginList knownPluginList;
    juce::Array<juce::PluginDescription> knownInstruments;

    juce::File cacheFile;
    juce::File deadMansPedalFile;
};
