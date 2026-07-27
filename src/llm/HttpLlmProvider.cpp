#include "llm/HttpLlmProvider.h"

#include "core/llm/CannedLibrary.h"
#include "core/llm/PromptBuilder.h"
#include "core/llm/ResponseExtract.h"

#include <nlohmann/json.hpp>

namespace forge::llm {
namespace {

using nlohmann::json;

json textBlock(const std::string& role, const std::string& text) {
    return json{{"role", role}, {"content", text}};
}

} // namespace

// --- HttpLlmProvider -------------------------------------------------------

HttpLlmProvider::HttpLlmProvider(ForgeConfig config) : config_(std::move(config)) {}

juce::String HttpLlmProvider::name() const {
    return config_.providerName() + " (" + config_.effectiveModel() + ")";
}

juce::String HttpLlmProvider::endpoint() const {
    const auto base = config_.effectiveBaseUrl().trimCharactersAtEnd("/");
    switch (config_.provider) {
        case ForgeConfig::Provider::OpenAi: return base + "/v1/chat/completions";
        case ForgeConfig::Provider::Ollama: return base + "/api/chat";
        default:                            return base + "/v1/messages";
    }
}

juce::String HttpLlmProvider::extraHeaders() const {
    juce::String h = "Content-Type: application/json\r\n";
    switch (config_.provider) {
        case ForgeConfig::Provider::OpenAi:
            h += "Authorization: Bearer " + config_.apiKey + "\r\n";
            break;
        case ForgeConfig::Provider::Ollama:
            break;   // local, no auth
        default:
            h += "x-api-key: " + config_.apiKey + "\r\n";
            h += "anthropic-version: 2023-06-01\r\n";
            break;
    }
    return h;
}

juce::String HttpLlmProvider::buildBody(const GenerationRequest& req) const {
    json messages = json::array();
    messages.push_back(textBlock("user", req.userMessage));

    // On a retry we replay our own bad answer and the validator's complaint, so
    // the model can see exactly what it got wrong rather than guessing.
    if (!req.correctionFeedback.empty()) {
        if (!req.previousAttempt.empty())
            messages.push_back(textBlock("assistant", req.previousAttempt));
        messages.push_back(textBlock("user", req.correctionFeedback));
    }

    json body;
    switch (config_.provider) {
        case ForgeConfig::Provider::OpenAi: {
            json all = json::array();
            all.push_back(textBlock("system", req.systemPrompt));
            for (auto& m : messages) all.push_back(m);
            body = {
                {"model", config_.effectiveModel().toStdString()},
                {"messages", all},
                {"temperature", req.temperature},
                {"max_tokens", req.maxTokens},
                {"response_format", {{"type", "json_object"}}}
            };
            break;
        }
        case ForgeConfig::Provider::Ollama: {
            json all = json::array();
            all.push_back(textBlock("system", req.systemPrompt));
            for (auto& m : messages) all.push_back(m);
            body = {
                {"model", config_.effectiveModel().toStdString()},
                {"messages", all},
                {"stream", false},
                {"format", "json"},
                {"options", {{"temperature", req.temperature},
                             {"num_predict", req.maxTokens}}}
            };
            break;
        }
        default: {
            // The system prompt is ~10k tokens of capability manifest and
            // few-shot examples, and it is byte-identical on every request.
            // Marking it as a cache breakpoint cuts the input cost of repeated
            // generations by ~90% and shaves latency off the time-to-first-
            // sound, which matters more than the money when someone is
            // standing on a stage waiting for a noise.
            //
            // The cache lives ~5 minutes, so it helps exactly when you are
            // iterating - a first call after a long idle still pays full price.
            body = {
                {"model", config_.effectiveModel().toStdString()},
                {"system", json::array({
                    {{"type", "text"},
                     {"text", req.systemPrompt},
                     {"cache_control", {{"type", "ephemeral"}}}}
                })},
                {"messages", messages},
                {"temperature", req.temperature},
                {"max_tokens", req.maxTokens}
            };
            break;
        }
    }
    return juce::String(body.dump());
}

GenerationResult HttpLlmProvider::generate(const GenerationRequest& req,
                                           const CancelCheck& shouldCancel) {
    GenerationResult result;
    result.providerName = name().toStdString();

    const auto start = juce::Time::getMillisecondCounterHiRes();
    if (shouldCancel && shouldCancel()) {
        result.errorMessage = "Cancelled.";
        return result;
    }

    juce::URL url(endpoint());
    url = url.withPOSTData(buildBody(req));

    int statusCode = 0;
    juce::StringPairArray responseHeaders;

    auto options = juce::URL::InputStreamOptions(juce::URL::ParameterHandling::inPostData)
                       .withExtraHeaders(extraHeaders())
                       .withConnectionTimeoutMs(req.timeoutMs)
                       .withResponseHeaders(&responseHeaders)
                       .withStatusCode(&statusCode)
                       .withNumRedirectsToFollow(3)
                       .withHttpRequestCmd("POST");

    // Note: this read blocks. Cancellation is checked around it and enforced by
    // the connection timeout - the session treats a late result as stale rather
    // than trying to tear the socket down underneath JUCE.
    std::unique_ptr<juce::InputStream> stream(url.createInputStream(options));
    result.latencyMs = juce::Time::getMillisecondCounterHiRes() - start;

    if (stream == nullptr) {
        result.errorMessage = "Could not reach " + endpoint().toStdString()
                            + ". Check the network and the base URL.";
        return result;
    }

    const juce::String body = stream->readEntireStreamAsString();

    if (shouldCancel && shouldCancel()) {
        result.errorMessage = "Cancelled.";
        return result;
    }

    if (statusCode >= 400) {
        std::string detail;
        switch (config_.provider) {
            case ForgeConfig::Provider::OpenAi: extractOpenAiContent(body.toStdString(), detail); break;
            case ForgeConfig::Provider::Ollama: extractOllamaContent(body.toStdString(), detail); break;
            default:                            extractAnthropicContent(body.toStdString(), detail); break;
        }
        result.errorMessage = "HTTP " + std::to_string(statusCode)
                            + (detail.empty() ? std::string() : (": " + detail));
        if (statusCode == 401 || statusCode == 403)
            result.errorMessage += "  (check your API key in Settings)";
        return result;
    }

    std::string error;
    std::string content;
    switch (config_.provider) {
        case ForgeConfig::Provider::OpenAi: content = extractOpenAiContent(body.toStdString(), error); break;
        case ForgeConfig::Provider::Ollama: content = extractOllamaContent(body.toStdString(), error); break;
        default:                            content = extractAnthropicContent(body.toStdString(), error); break;
    }

    if (!error.empty()) { result.errorMessage = error; return result; }

    const std::string extracted = extractJsonObject(content);
    if (extracted.empty()) {
        result.errorMessage = "The model did not return a JSON object.";
        return result;
    }

    if (config_.logRawResponses) {
        ForgeConfig::logsDirectory().createDirectory();
        const auto file = ForgeConfig::logsDirectory().getChildFile(
            "response-" + juce::Time::getCurrentTime().formatted("%Y%m%d-%H%M%S") + ".json");
        file.replaceWithText(body);
    }

    result.ok     = true;
    result.irJson = extracted;
    return result;
}

// --- CannedProvider --------------------------------------------------------

GenerationResult CannedProvider::generate(const GenerationRequest& req, const CancelCheck&) {
    GenerationResult result;
    result.providerName = "Offline library";
    result.usedFallback = true;

    const auto* match = bestCannedMatch(req.userMessage);
    if (match == nullptr) {
        result.errorMessage = "The offline library is empty.";
        return result;
    }
    result.ok     = true;
    result.irJson = match->json;
    return result;
}

// ---------------------------------------------------------------------------

std::unique_ptr<LlmProvider> makeProvider(const ForgeConfig& config) {
    if (!config.isUsable()) return std::make_unique<CannedProvider>();
    return std::make_unique<HttpLlmProvider>(config);
}

} // namespace forge::llm
