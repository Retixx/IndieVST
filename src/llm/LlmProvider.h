// ---------------------------------------------------------------------------
// Provider abstraction (SPEC 7.1).
//
// Nothing above this interface knows or cares which model is being used, which
// is what keeps the project from being welded to one vendor - and what lets the
// offline library stand in for a real model without anything upstream noticing.
// ---------------------------------------------------------------------------
#pragma once

#include <functional>
#include <juce_core/juce_core.h>
#include <string>

namespace forge::llm {

struct GenerationRequest {
    std::string  systemPrompt;
    std::string  userMessage;
    /// Set on a retry: the validator's error list, appended as a second turn.
    std::string  correctionFeedback;
    /// The specification the correction applies to.
    std::string  previousAttempt;

    int   maxTokens   = 8192;
    float temperature = 0.7f;
    /// Whole-generation budget. Responses stream, so this is when we stop
    /// waiting rather than when a slow model starts looking like a dead socket.
    int   timeoutMs   = 150000;
};

struct GenerationResult {
    bool        ok = false;
    std::string irJson;        ///< raw text; still needs extraction + validation
    std::string errorMessage;
    std::string providerName;
    double      latencyMs   = 0.0;
    bool        usedFallback = false;
    /// Set when the response had to be unwrapped from prose or rebuilt after
    /// being cut off. Shown to the musician, so a recovered patch is never
    /// presented as though it arrived clean.
    std::string salvageNote;
};

using CancelCheck = std::function<bool()>;

class LlmProvider {
public:
    virtual ~LlmProvider() = default;
    virtual juce::String name() const = 0;
    /// Blocking. Called on a worker thread, never on the audio or message
    /// thread. Should poll `shouldCancel` between phases.
    virtual GenerationResult generate(const GenerationRequest&, const CancelCheck& shouldCancel) = 0;
};

} // namespace forge::llm
