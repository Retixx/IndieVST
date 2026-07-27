// ---------------------------------------------------------------------------
// Pulling the specification out of whatever the model actually sent.
//
// Even with strict structured-output modes, responses arrive wrapped in
// markdown fences, prefaced with "Here's your instrument:", or with a trailing
// explanation. Being tolerant here costs nothing and saves entire generations.
// ---------------------------------------------------------------------------
#pragma once

#include <string>

namespace forge::llm {

/// Returns the first balanced top-level JSON object found in `raw`, ignoring
/// braces inside strings and stripping markdown fences. Empty if there is none.
std::string extractJsonObject(const std::string& raw);

/// Extracts the assistant text from an Anthropic Messages API response,
/// preferring tool-use input when present.
std::string extractAnthropicContent(const std::string& responseBody, std::string& errorOut);

/// Extracts the assistant message content from an OpenAI-compatible chat
/// completion response. Also handles the Ollama /api/chat shape.
std::string extractOpenAiContent(const std::string& responseBody, std::string& errorOut);
std::string extractOllamaContent(const std::string& responseBody, std::string& errorOut);

} // namespace forge::llm
