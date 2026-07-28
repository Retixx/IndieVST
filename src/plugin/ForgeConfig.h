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

/// Bumped whenever a stored field needs migrating. See ForgeConfig::load().
inline constexpr int kConfigVersion = 3;

struct ForgeConfig {
    enum class Provider { Anthropic, OpenAi, Ollama, Canned };

    Provider    provider = Provider::Anthropic;
    juce::String apiKey;
    juce::String model;
    juce::String baseUrl;

    /// The whole-generation budget, not a per-read one.
    ///
    /// Responses are streamed now, so a slow model no longer looks like a dead
    /// socket and this is free to be generous: it is the point at which we stop
    /// waiting, not the point at which we mistake waiting for failure. A full
    /// patch is up to 16k output tokens, which is minutes rather than seconds
    /// on a large model, and at 60s a perfectly good generation was being
    /// thrown away and replaced with the offline fallback - which is exactly
    /// what "it timed out and gave me the wrong instrument" was.
    int   timeoutMs   = 150000;
    int   maxRetries  = 1;
    float temperature = 0.7f;
    bool  forceOffline = false;

    /// Anthropic only. "off" disables extended thinking outright; "adaptive"
    /// lets the model decide. Generating an instrument graph is a structured
    /// schema-filling task with worked examples in the prompt, not a reasoning
    /// problem - thinking mostly buys latency and billed output tokens here.
    juce::String thinkingMode = "off";        ///< off | adaptive
    /// Used when thinkingMode is "adaptive": low | medium | high | max
    juce::String effort = "low";

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
