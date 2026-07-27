// ---------------------------------------------------------------------------
// Prompt construction (SPEC 7.2).
//
// The capability manifest is generated from the module registry, never
// hand-maintained. Add a DSP primitive and the model learns about it on the
// next generation with no prompt edit - that property is the whole reason the
// registry exists.
// ---------------------------------------------------------------------------
#pragma once

#include <string>

namespace forge::llm {

struct PromptSpec {
    std::string system;
    std::string user;
};

/// Full system prompt: role, capability manifest, schema, hard constraints,
/// sound-design heuristics and few-shot examples.
std::string buildSystemPrompt();

/// `currentIrJson` empty  -> generate a new instrument.
/// `currentIrJson` set    -> edit it, returning a complete replacement.
PromptSpec buildGenerationPrompt(const std::string& userPrompt,
                                 const std::string& currentIrJson = {});

/// The JSON schema the strict-output modes of the providers are given.
std::string irJsonSchema();

/// Rough token estimate, used to warn during development if the prompt grows
/// past what is sensible to send on every generation.
int estimateTokens(const std::string& text);

} // namespace forge::llm
