#include "plugin/ForgeConfig.h"

namespace forge {
namespace {

juce::String envVar(const char* name) {
    return juce::SystemStats::getEnvironmentVariable(name, {});
}

/// Parses a very small subset of dotenv: KEY=VALUE, # comments, optional quotes.
juce::StringPairArray parseDotEnv(const juce::File& file) {
    juce::StringPairArray out;
    if (!file.existsAsFile()) return out;

    juce::StringArray lines;
    lines.addLines(file.loadFileAsString());
    for (auto line : lines) {
        line = line.trim();
        if (line.isEmpty() || line.startsWithChar('#')) continue;
        const int eq = line.indexOfChar('=');
        if (eq <= 0) continue;
        auto key = line.substring(0, eq).trim();
        auto val = line.substring(eq + 1).trim();
        if (val.startsWithChar('"') && val.endsWithChar('"')) val = val.unquoted();
        out.set(key, val);
    }
    return out;
}

juce::File findDotEnv() {
    // Walk up from the binary looking for a .env, so the Standalone build picks
    // up the repo's file during development without any setup.
    auto dir = juce::File::getSpecialLocation(juce::File::currentExecutableFile).getParentDirectory();
    for (int i = 0; i < 6 && dir.exists(); ++i) {
        const auto candidate = dir.getChildFile(".env");
        if (candidate.existsAsFile()) return candidate;
        dir = dir.getParentDirectory();
    }
    return {};
}

juce::String lookup(const juce::var& json, const juce::StringPairArray& dotenv,
                    const char* jsonKey, const char* envKey) {
    if (auto* obj = json.getDynamicObject()) {
        const auto v = obj->getProperty(jsonKey);
        if (!v.isVoid() && v.toString().isNotEmpty()) return v.toString();
    }
    const auto env = envVar(envKey);
    if (env.isNotEmpty()) return env;
    if (dotenv.containsKey(envKey)) return dotenv[envKey];
    return {};
}

} // namespace

juce::File ForgeConfig::configDirectory() {
    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
             .getChildFile("Forge");
}
juce::File ForgeConfig::configFile()          { return configDirectory().getChildFile("config.json"); }
juce::File ForgeConfig::instrumentsDirectory(){ return configDirectory().getChildFile("instruments"); }
juce::File ForgeConfig::logsDirectory()       { return configDirectory().getChildFile("logs"); }

ForgeConfig::Provider ForgeConfig::providerFromString(const juce::String& s) {
    const auto v = s.trim().toLowerCase();
    if (v == "openai") return Provider::OpenAi;
    if (v == "ollama") return Provider::Ollama;
    if (v == "canned" || v == "offline") return Provider::Canned;
    return Provider::Anthropic;
}

juce::String ForgeConfig::providerToString(Provider p) {
    switch (p) {
        case Provider::OpenAi: return "openai";
        case Provider::Ollama: return "ollama";
        case Provider::Canned: return "canned";
        default:               return "anthropic";
    }
}

juce::String ForgeConfig::providerName() const {
    switch (provider) {
        case Provider::OpenAi: return "OpenAI";
        case Provider::Ollama: return "Ollama (local)";
        case Provider::Canned: return "Offline library";
        default:               return "Anthropic";
    }
}

juce::String ForgeConfig::effectiveBaseUrl() const {
    if (baseUrl.isNotEmpty()) return baseUrl;
    switch (provider) {
        case Provider::OpenAi: return "https://api.openai.com";
        case Provider::Ollama: return "http://localhost:11434";
        default:               return "https://api.anthropic.com";
    }
}

juce::String ForgeConfig::effectiveModel() const {
    if (model.isNotEmpty()) return model;
    switch (provider) {
        case Provider::OpenAi: return "gpt-4.1";
        case Provider::Ollama: return "llama3.1:8b";
        default:               return "claude-sonnet-5";
    }
}

bool ForgeConfig::isUsable() const {
    if (forceOffline) return false;
    switch (provider) {
        case Provider::Canned: return false;
        case Provider::Ollama: return true;              // local, no key needed
        default:               return apiKey.isNotEmpty();
    }
}

ForgeConfig ForgeConfig::load() {
    ForgeConfig cfg;

    juce::var json;
    const auto file = configFile();
    if (file.existsAsFile())
        json = juce::JSON::parse(file.loadFileAsString());

    const auto dotenv = parseDotEnv(findDotEnv());

    cfg.provider = providerFromString(lookup(json, dotenv, "provider", "FORGE_LLM_PROVIDER"));

    const char* keyEnv  = "FORGE_ANTHROPIC_API_KEY";
    const char* modelEnv = "FORGE_ANTHROPIC_MODEL";
    const char* urlEnv   = "FORGE_ANTHROPIC_BASE_URL";
    if (cfg.provider == Provider::OpenAi) {
        keyEnv = "FORGE_OPENAI_API_KEY"; modelEnv = "FORGE_OPENAI_MODEL"; urlEnv = "FORGE_OPENAI_BASE_URL";
    } else if (cfg.provider == Provider::Ollama) {
        keyEnv = "FORGE_OLLAMA_API_KEY"; modelEnv = "FORGE_OLLAMA_MODEL"; urlEnv = "FORGE_OLLAMA_BASE_URL";
    }

    cfg.apiKey  = lookup(json, dotenv, "api_key",  keyEnv);
    cfg.model   = lookup(json, dotenv, "model",    modelEnv);
    cfg.baseUrl = lookup(json, dotenv, "base_url", urlEnv);

    auto intOr = [&](const char* jsonKey, const char* envKey, int fallback) {
        const auto s = lookup(json, dotenv, jsonKey, envKey);
        return s.isNotEmpty() ? s.getIntValue() : fallback;
    };
    auto floatOr = [&](const char* jsonKey, const char* envKey, float fallback) {
        const auto s = lookup(json, dotenv, jsonKey, envKey);
        return s.isNotEmpty() ? static_cast<float>(s.getDoubleValue()) : fallback;
    };

    cfg.timeoutMs    = juce::jlimit(2000,  120000, intOr("timeout_ms", "FORGE_GENERATION_TIMEOUT_MS", 12000));
    cfg.maxRetries   = juce::jlimit(0,     3,      intOr("max_retries", "FORGE_MAX_RETRIES", 1));
    cfg.temperature  = juce::jlimit(0.0f,  1.5f,   floatOr("temperature", "FORGE_TEMPERATURE", 0.7f));
    cfg.polyphony    = juce::jlimit(1,     32,     intOr("polyphony", "FORGE_POLYPHONY", 16));
    cfg.cpuBudget    = juce::jlimit(0.05f, 0.9f,   floatOr("cpu_budget", "FORGE_CPU_BUDGET", 0.35f));
    cfg.forceOffline = intOr("force_offline", "FORGE_FORCE_OFFLINE", 0) != 0;
    cfg.logRawResponses = intOr("log_raw_responses", "FORGE_LOG_RAW_RESPONSES", 0) != 0;

    return cfg;
}

void ForgeConfig::save() const {
    auto* obj = new juce::DynamicObject();
    obj->setProperty("provider",    providerToString(provider));
    obj->setProperty("api_key",     apiKey);
    obj->setProperty("model",       model);
    obj->setProperty("base_url",    baseUrl);
    obj->setProperty("timeout_ms",  timeoutMs);
    obj->setProperty("max_retries", maxRetries);
    obj->setProperty("temperature", temperature);
    obj->setProperty("polyphony",   polyphony);
    obj->setProperty("cpu_budget",  cpuBudget);
    obj->setProperty("force_offline", forceOffline ? 1 : 0);
    obj->setProperty("log_raw_responses", logRawResponses ? 1 : 0);

    configDirectory().createDirectory();
    configFile().replaceWithText(juce::JSON::toString(juce::var(obj), false));
}

} // namespace forge
