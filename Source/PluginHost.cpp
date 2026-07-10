#include "PluginHost.h"

PluginHost::PluginHost()
{
    formatManager.addDefaultFormats();

    // userApplicationDataDirectory resolves to ~/Library itself on macOS, not
    // ~/Library/Application Support -- append the conventional subpath ourselves.
    auto appDataDir = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                           .getChildFile("Application Support")
                           .getChildFile("PianoRollMaestro");
    appDataDir.createDirectory();

    cacheFile = appDataDir.getChildFile("KnownPlugins.xml");
    deadMansPedalFile = appDataDir.getChildFile("DeadMansPedal.txt");

    loadCache();
}

void PluginHost::loadCache()
{
    if (auto xml = juce::XmlDocument::parse(cacheFile))
        knownPluginList.recreateFromXml(*xml);

    refreshKnownInstruments();
}

void PluginHost::saveCache()
{
    if (auto xml = knownPluginList.createXml())
        xml->writeTo(cacheFile);
}

void PluginHost::refreshKnownInstruments()
{
    knownInstruments.clear();

    for (auto& description : knownPluginList.getTypes())
        if (description.isInstrument)
            knownInstruments.add(description);
}

void PluginHost::scanForInstruments()
{
    for (auto* format : formatManager.getFormats())
    {
        juce::PluginDirectoryScanner scanner(knownPluginList,
                                              *format,
                                              format->getDefaultLocationsToSearch(),
                                              true,
                                              deadMansPedalFile,
                                              false);

        juce::String pluginBeingScanned;
        while (scanner.scanNextFile(true, pluginBeingScanned))
        {
            DBG("Scanning: " << pluginBeingScanned);

            // Save after every file, not just at the end -- some plugins
            // (observed: Native Instruments Kontakt/Maschine/MIDIculous,
            // which share IPC infrastructure) can crash the whole process
            // mid-scan. Saving incrementally means a crash only loses the
            // one in-flight file, not everything found so far; combined
            // with deadMansPedalFile, a re-run skips the culprit and keeps
            // making forward progress.
            saveCache();
        }
    }

    refreshKnownInstruments();
    saveCache();

    DBG("Found " << knownInstruments.size() << " instrument(s):");
    for (auto& description : knownInstruments)
        DBG("  " << description.name << " (" << description.pluginFormatName << ")");
}

void PluginHost::createInstrument(const juce::PluginDescription& description,
                                   double sampleRate,
                                   int blockSize,
                                   juce::AudioPluginFormat::PluginCreationCallback callback)
{
    formatManager.createPluginInstanceAsync(description, sampleRate, blockSize, std::move(callback));
}
