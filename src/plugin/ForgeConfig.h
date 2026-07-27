// ---------------------------------------------------------------------------
// Configuration and secrets.
//
// Resolution order (first hit wins):
//   1. %APPDATA%\Forge\config.json   - written by the plugin's Settings panel
//   2. environment variables         - convenient for the Standalone dev build
//   3. a .env file next to the binary or in the repo root  - development only
//
// API keys never touch the DAW project state and are never written into a
// saved instrument.
// ---------------------------------------------------------------------------
#pragma once

#include <juce_core/juce_core.h>

namespace forge {

struct ForgeConfig {
    enum class Provider { Anthropic, OpenAi, Ollama, Canned };

    Provider    provider = Provider::Anthropic;
    juce::String apiKey;
    juce::String model;
    juce::String baseUrl;

    int   timeoutMs   = 12000;
    int   maxRetries  = 1;
    float temperature = 0.7f;
    bool  forceOffline = false;

    int   polyphony  = 16;
    float cpuBudget  = 0.35f;
    bool  logRawResponses = false;

    static juce::File configDirectory();
    static juce::File configFile();
    static juce::File instrumentsDirectory();
    static juce::File logsDirectory();

    /// Never throws and never fails: missing configuration simply leaves the
    /// defaults in place, and the provider layer falls back to offline mode.
    static ForgeConfig load();
    void save() const;

    /// True when we have everything needed to actually reach a model.
    bool isUsable() const;

    juce::String providerName() const;
    static Provider providerFromString(const juce::String&);
    static juce::String providerToString(Provider);

    /// Default endpoint/model for the selected provider, used when the user has
    /// not overridden them.
    juce::String effectiveBaseUrl() const;
    juce::String effectiveModel() const;
};

} // namespace forge
