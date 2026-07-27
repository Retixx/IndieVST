#include "core/llm/ResponseExtract.h"

#include <nlohmann/json.hpp>

namespace forge::llm {
namespace {

nlohmann::json parseOrNull(const std::string& body) {
    return nlohmann::json::parse(body, nullptr, false, true);
}

std::string errorFromBody(const nlohmann::json& j) {
    if (j.is_discarded()) return "Response was not valid JSON.";
    auto err = j.find("error");
    if (err != j.end()) {
        if (err->is_string()) return err->get<std::string>();
        if (err->is_object()) {
            auto msg = err->find("message");
            if (msg != err->end() && msg->is_string()) return msg->get<std::string>();
            return err->dump();
        }
    }
    return {};
}

} // namespace

std::string extractJsonObject(const std::string& raw) {
    // Strip a leading markdown fence if there is one; the closing fence takes
    // care of itself because we stop at the balanced brace.
    size_t start = raw.find('{');
    if (start == std::string::npos) return {};

    int depth = 0;
    bool inString = false, escaped = false;
    for (size_t i = start; i < raw.size(); ++i) {
        const char c = raw[i];
        if (inString) {
            if (escaped)        escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"')  inString = false;
            continue;
        }
        if (c == '"')      inString = true;
        else if (c == '{') ++depth;
        else if (c == '}') {
            if (--depth == 0) return raw.substr(start, i - start + 1);
            if (depth < 0) return {};
        }
    }
    return {};   // unbalanced - treat as a failed generation rather than guessing
}

std::string extractAnthropicContent(const std::string& responseBody, std::string& errorOut) {
    errorOut.clear();
    const auto j = parseOrNull(responseBody);
    const std::string err = errorFromBody(j);
    if (!err.empty()) { errorOut = err; return {}; }

    auto content = j.find("content");
    if (content == j.end() || !content->is_array()) {
        errorOut = "Unexpected response shape (no content array).";
        return {};
    }

    // Prefer a tool_use block: that is the structured-output path and its input
    // is already a JSON object rather than a string that might be truncated.
    for (const auto& block : *content) {
        if (!block.is_object()) continue;
        auto type = block.find("type");
        if (type != block.end() && type->is_string() && type->get<std::string>() == "tool_use") {
            auto input = block.find("input");
            if (input != block.end() && input->is_object()) return input->dump();
        }
    }

    std::string text;
    for (const auto& block : *content) {
        if (!block.is_object()) continue;
        auto t = block.find("text");
        if (t != block.end() && t->is_string()) text += t->get<std::string>();
    }
    if (text.empty()) errorOut = "Model returned no text.";
    return text;
}

std::string extractOpenAiContent(const std::string& responseBody, std::string& errorOut) {
    errorOut.clear();
    const auto j = parseOrNull(responseBody);
    const std::string err = errorFromBody(j);
    if (!err.empty()) { errorOut = err; return {}; }

    auto choices = j.find("choices");
    if (choices == j.end() || !choices->is_array() || choices->empty()) {
        errorOut = "Unexpected response shape (no choices).";
        return {};
    }
    const auto& first = (*choices)[0];
    auto message = first.find("message");
    if (message == first.end() || !message->is_object()) {
        errorOut = "Unexpected response shape (no message).";
        return {};
    }

    auto toolCalls = message->find("tool_calls");
    if (toolCalls != message->end() && toolCalls->is_array() && !toolCalls->empty()) {
        const auto& fn = (*toolCalls)[0].value("function", nlohmann::json::object());
        if (fn.contains("arguments") && fn["arguments"].is_string())
            return fn["arguments"].get<std::string>();
    }

    auto content = message->find("content");
    if (content != message->end() && content->is_string()) return content->get<std::string>();

    errorOut = "Model returned no content.";
    return {};
}

std::string extractOllamaContent(const std::string& responseBody, std::string& errorOut) {
    errorOut.clear();
    const auto j = parseOrNull(responseBody);
    const std::string err = errorFromBody(j);
    if (!err.empty()) { errorOut = err; return {}; }

    auto message = j.find("message");
    if (message != j.end() && message->is_object()) {
        auto content = message->find("content");
        if (content != message->end() && content->is_string()) return content->get<std::string>();
    }
    auto response = j.find("response");
    if (response != j.end() && response->is_string()) return response->get<std::string>();

    errorOut = "Unexpected response shape.";
    return {};
}

} // namespace forge::llm
