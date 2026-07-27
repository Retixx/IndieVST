#pragma once

#include "llm/LlmProvider.h"
#include "plugin/ForgeConfig.h"

#include <memory>

namespace forge::llm {

/// Anthropic / OpenAI / Ollama over HTTP. One class rather than three because
/// the only real differences are the endpoint, the auth header and where the
/// text lands in the response.
class HttpLlmProvider final : public LlmProvider {
public:
    explicit HttpLlmProvider(ForgeConfig config);

    juce::String name() const override;
    GenerationResult generate(const GenerationRequest&, const CancelCheck&) override;

private:
    /// `includeTuning` false strips every optional knob (thinking, effort,
    /// temperature) so a request rejected for one of them can be retried bare.
    juce::String buildBody(const GenerationRequest&, bool includeTuning) const;
    juce::String endpoint() const;
    juce::String extraHeaders() const;

    ForgeConfig config_;
};

/// The offline library (SPEC 7.1). Also the automatic fallback whenever a real
/// provider fails, which is what makes the demo un-killable.
class CannedProvider final : public LlmProvider {
public:
    juce::String name() const override { return "Offline library"; }
    GenerationResult generate(const GenerationRequest&, const CancelCheck&) override;
};

/// Chooses a provider from configuration. Never returns nullptr.
std::unique_ptr<LlmProvider> makeProvider(const ForgeConfig&);

} // namespace forge::llm
