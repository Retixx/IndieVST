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
    // Header lines are joined WITHOUT a trailing terminator. JUCE appends its
    // own separator, and a dangling "\r\n" produces a malformed request that
    // WinINet rejects before it ever leaves the machine - which surfaces as a
    // null stream with no status code, i.e. indistinguishable from the network
    // being down.
    juce::StringArray headers;
    headers.add("content-type: application/json");

    switch (config_.provider) {
        case ForgeConfig::Provider::OpenAi:
            headers.add("authorization: Bearer " + config_.apiKey);
            break;
        case ForgeConfig::Provider::Ollama:
            break;   // local, no auth
        default:
            headers.add("x-api-key: " + config_.apiKey);
            headers.add("anthropic-version: 2023-06-01");
            break;
    }
    return headers.joinIntoString("\r\n");
}

juce::String HttpLlmProvider::buildBody(const GenerationRequest& req, bool includeTuning) const {
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
                {"max_tokens", req.maxTokens},
                {"response_format", {{"type", "json_object"}}}
            };
            if (includeTuning) body["temperature"] = req.temperature;
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
                {"options", {{"num_predict", req.maxTokens}}}
            };
            if (includeTuning) body["options"]["temperature"] = req.temperature;
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
                {"max_tokens", req.maxTokens}
            };
            // NOTE: no temperature / top_p / top_k. Claude Sonnet 5 rejects any
            // non-default sampling parameter with a 400 - adaptive thinking is
            // on by default and the two are incompatible. Anthropic's guidance
            // is to constrain output in the prompt instead, which is what the
            // "Return only the JSON object" instruction already does.
            //
            // Thinking is disabled by default. Adaptive thinking pushed a
            // single generation past 60 s, and because JUCE applies its timeout
            // per-read - and no bytes flow while the model thinks - that reads
            // as a dead connection rather than a slow one. This task is
            // schema-filling against worked examples, so the reasoning pass
            // buys latency and billed output tokens more than quality.
            if (includeTuning) {
                if (config_.thinkingMode == "off")
                    body["thinking"] = {{"type", "disabled"}};
                else if (config_.effort.isNotEmpty())
                    body["effort"] = config_.effort.toStdString();
            }
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

    int statusCode = 0;
    juce::String body;

    // Note: this read blocks. Cancellation is checked around it and enforced by
    // the timeout - the session treats a late result as stale rather than
    // trying to tear the socket down underneath JUCE.
    auto send = [&](bool includeTuning) -> bool {
        statusCode = 0;
        juce::StringPairArray responseHeaders;
        juce::URL url = juce::URL(endpoint()).withPOSTData(buildBody(req, includeTuning));

        auto options = juce::URL::InputStreamOptions(juce::URL::ParameterHandling::inPostData)
                           .withExtraHeaders(extraHeaders())
                           .withConnectionTimeoutMs(req.timeoutMs)
                           .withResponseHeaders(&responseHeaders)
                           .withStatusCode(&statusCode)
                           .withNumRedirectsToFollow(3)
                           .withHttpRequestCmd("POST");

        std::unique_ptr<juce::InputStream> stream(url.createInputStream(options));
        if (stream == nullptr) return false;
        body = stream->readEntireStreamAsString();
        return true;
    };

    bool reached = send(true);

    // Provider APIs move. If a request is rejected specifically because of an
    // optional tuning parameter, strip them and try once more rather than
    // failing the generation over a knob we did not need.
    if (reached && statusCode == 400) {
        const auto lower = body.toLowerCase();
        if (lower.contains("thinking") || lower.contains("effort")
            || lower.contains("temperature") || lower.contains("top_p")
            || lower.contains("top_k") || lower.contains("unexpected")) {
            reached = send(false);
        }
    }

    result.latencyMs = juce::Time::getMillisecondCounterHiRes() - start;

    if (!reached) {
        const int elapsed = static_cast<int>(result.latencyMs);
        // Distinguish "gave up waiting" from "never connected": with a large
        // prompt and adaptive thinking, a timeout is by far the more likely of
        // the two and needs a completely different fix.
        if (elapsed >= req.timeoutMs - 500) {
            result.errorMessage = "Timed out after " + std::to_string(elapsed / 1000)
                                + "s with no data received. If FORGE_THINKING is "
                                  "'adaptive', set it to 'off' - JUCE times out per read, "
                                  "and no bytes arrive while the model is thinking.";
        } else {
            result.errorMessage = "Could not reach " + endpoint().toStdString()
                                + " (no response after " + std::to_string(elapsed)
                                + " ms). Check the network, any proxy or firewall, and "
                                  "the base URL.";
        }
        return result;
    }

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
