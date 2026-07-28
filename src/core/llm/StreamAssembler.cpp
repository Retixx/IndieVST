#include "core/llm/StreamAssembler.h"

#include <nlohmann/json.hpp>

namespace forge::llm {

using nlohmann::json;

void StreamAssembler::feed(const char* data, size_t numBytes) {
    if (data == nullptr || numBytes == 0) return;
    pending_.append(data, numBytes);

    size_t nl;
    while ((nl = pending_.find('\n')) != std::string::npos) {
        std::string line = pending_.substr(0, nl);
        pending_.erase(0, nl + 1);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        consume(line);
    }
}

void StreamAssembler::finish() {
    if (pending_.empty()) return;
    std::string line;
    line.swap(pending_);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    consume(line);
}

void StreamAssembler::consume(const std::string& line) {
    std::string payload;

    if (wire_ == Wire::OllamaJsonLines) {
        if (line.empty()) return;
        payload = line;
    } else {
        // Server-sent events. The `event:` line carries the type, but so does
        // the data object on every provider here, so only `data:` is read -
        // which also means an unknown event type costs nothing.
        if (line.rfind("data:", 0) != 0) return;
        payload = line.substr(5);
        while (!payload.empty() && (payload.front() == ' ' || payload.front() == '\t'))
            payload.erase(0, 1);
        if (payload == "[DONE]") { done_ = true; return; }
    }
    if (payload.empty()) return;

    // Never throws: a half-written or unrecognised event is skipped, not fatal.
    const auto j = json::parse(payload, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return;

    // An error can arrive mid-stream, long after a 200. Reported rather than
    // silently truncating the design and pretending it finished.
    if (j.contains("error") && !j["error"].is_null()) {
        const auto& e = j["error"];
        if (e.is_object() && e.contains("message") && e["message"].is_string())
            error_ = e["message"].get<std::string>();
        else if (e.is_string())
            error_ = e.get<std::string>();
        else
            error_ = "The provider reported an error mid-stream.";
        done_ = true;
        return;
    }

    switch (wire_) {
        case Wire::AnthropicSse: {
            const auto type = j.value("type", std::string());
            if (type == "content_block_delta" && j.contains("delta")
                && j["delta"].is_object()) {
                const auto& d = j["delta"];
                // text_delta when the model writes prose or JSON directly,
                // input_json_delta when it is filling a tool schema.
                for (const char* key : {"text", "partial_json"})
                    if (d.contains(key) && d[key].is_string())
                        text_ += d[key].get<std::string>();
            } else if (type == "message_stop") {
                done_ = true;
            }
            break;
        }
        case Wire::OpenAiSse: {
            if (!j.contains("choices") || !j["choices"].is_array()) break;
            for (const auto& c : j["choices"]) {
                if (!c.is_object() || !c.contains("delta") || !c["delta"].is_object()) continue;
                const auto& d = c["delta"];
                if (d.contains("content") && d["content"].is_string())
                    text_ += d["content"].get<std::string>();
            }
            break;
        }
        case Wire::OllamaJsonLines: {
            if (j.contains("message") && j["message"].is_object()) {
                const auto& m = j["message"];
                if (m.contains("content") && m["content"].is_string())
                    text_ += m["content"].get<std::string>();
            }
            if (j.value("done", false)) done_ = true;
            break;
        }
    }
}

} // namespace forge::llm
