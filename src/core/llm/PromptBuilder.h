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

/// Patch mode: the instrument architecture is fixed, and the model designs
/// within it - which sections are live, the modulation matrix, the wavetable
/// content and every parameter value. Far more reliable than asking for a
/// whole signal graph, and it yields an instrument with the full control
/// surface instead of the handful of knobs a from-scratch graph tends to have.
std::string buildPatchSystemPrompt();
/// `referenceText` is the measurement block from
/// `audio::ReferenceFeatures::toPromptText()`, or empty when the musician did
/// not supply a recording.
PromptSpec buildPatchPrompt(const std::string& userPrompt,
                            const std::string& currentPatchJson = {},
                            const std::string& referenceText = {});

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
