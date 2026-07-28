#include "core/llm/JsonSalvage.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <vector>

namespace forge::llm {
namespace {

using nlohmann::json;

bool parsesAsObject(const std::string& text) {
    const auto j = json::parse(text, nullptr, /*allow_exceptions*/ false, /*ignore_comments*/ true);
    return !j.is_discarded() && j.is_object();
}

std::string trimmed(std::string s) {
    const auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    const auto e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

/// Walks the text once, tracking string state, and records the span of every
/// balanced top-level object it completes.
struct Scan {
    struct Span { size_t start, end; };   // end is one past the closing brace
    std::vector<Span> balanced;

    /// State at the point the text ran out, used by the repair pass.
    size_t              firstStart = std::string::npos;
    std::vector<char>   openStack;        // '{' and '[' still open
    bool                endedInString = false;
    bool                endedEscaped  = false;
};

Scan scan(const std::string& raw) {
    Scan out;
    bool   inString = false, escaped = false;
    int    depth = 0;
    size_t start = std::string::npos;

    for (size_t i = 0; i < raw.size(); ++i) {
        const char c = raw[i];
        if (inString) {
            if (escaped)        escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"')  inString = false;
            continue;
        }
        switch (c) {
            case '"': inString = true; break;
            case '{':
                if (depth == 0) {
                    start = i;
                    if (out.firstStart == std::string::npos) out.firstStart = i;
                }
                ++depth;
                out.openStack.push_back('{');
                break;
            case '[':
                if (depth > 0) out.openStack.push_back('[');
                break;
            case ']':
                if (!out.openStack.empty() && out.openStack.back() == '[') out.openStack.pop_back();
                break;
            case '}':
                if (!out.openStack.empty() && out.openStack.back() == '{') out.openStack.pop_back();
                if (depth > 0 && --depth == 0 && start != std::string::npos) {
                    out.balanced.push_back({start, i + 1});
                    start = std::string::npos;
                }
                break;
            default: break;
        }
    }
    out.endedInString = inString;
    out.endedEscaped  = escaped;
    return out;
}

/// Reconstructs the tail of a response that stopped mid-object.
///
/// The last member is almost always incomplete - a half-written key, or a key
/// with no value - so it is discarded back to the previous comma or opening
/// bracket, and then every still-open bracket is closed. Anything this produces
/// still has to parse before it is accepted.
std::string repairTruncated(const std::string& raw, const Scan& state) {
    if (state.firstStart == std::string::npos) return {};

    std::string body = raw.substr(state.firstStart);
    auto stack = state.openStack;

    if (state.endedEscaped && !body.empty()) body.pop_back();      // dangling backslash
    if (state.endedInString) body += '"';                          // close the open string

    // Walk back over a trailing partial member. Stop at a comma, or at the
    // bracket that opened the container we are inside.
    auto isStructural = [](char c) { return c == ',' || c == '{' || c == '[' || c == ':'; };
    size_t cut = body.size();
    while (cut > 0 && !isStructural(body[cut - 1])) --cut;

    // If we landed on a comma or a colon, the member before it is the last
    // complete one, so drop the separator too.
    if (cut > 0 && (body[cut - 1] == ',' || body[cut - 1] == ':')) {
        const bool afterColon = body[cut - 1] == ':';
        --cut;
        if (afterColon) {
            // "key": <nothing> - the key itself has to go as well.
            while (cut > 0 && !isStructural(body[cut - 1])) --cut;
            if (cut > 0 && body[cut - 1] == ',') --cut;
        }
    }
    body.resize(cut);

    // Trailing comma or colon left behind by the trim.
    while (!body.empty() && (body.back() == ',' || body.back() == ':' || body.back() == ' '
                            || body.back() == '\n' || body.back() == '\r' || body.back() == '\t'))
        body.pop_back();

    if (body.empty()) return {};

    for (auto it = stack.rbegin(); it != stack.rend(); ++it)
        body += (*it == '{') ? '}' : ']';

    return parsesAsObject(body) ? body : std::string{};
}

} // namespace

SalvageResult salvageJsonObject(const std::string& raw) {
    SalvageResult out;
    const auto text = trimmed(raw);
    if (text.empty()) {
        out.note = "The model returned nothing at all.";
        return out;
    }

    // 1. Exactly what we asked for.
    if (parsesAsObject(text)) {
        out.how  = Salvage::Clean;
        out.json = text;
        return out;
    }

    const auto state = scan(text);

    // 2. Largest balanced object anywhere in the text. Largest rather than
    //    first: a response that opens with "Here is {the idea}: {...}" has a
    //    decoy object in the prose, and the real patch is always the big one.
    const Scan::Span* pick = nullptr;
    for (const auto& span : state.balanced) {
        if (pick == nullptr || (span.end - span.start) > (pick->end - pick->start)) {
            auto candidate = text.substr(span.start, span.end - span.start);
            if (parsesAsObject(candidate)) pick = &span;
        }
    }
    if (pick != nullptr) {
        out.how  = Salvage::Unwrapped;
        out.json = text.substr(pick->start, pick->end - pick->start);
        out.note = "The specification was wrapped in extra text; unwrapped it.";
        return out;
    }

    // 3. Cut off partway through. Rebuild the tail.
    if (auto repaired = repairTruncated(text, state); !repaired.empty()) {
        out.how  = Salvage::Repaired;
        out.json = std::move(repaired);
        out.note = "The response was cut off partway through - recovered everything "
                   "up to the break. Some settings may have fallen back to defaults.";
        return out;
    }

    out.note = state.firstStart == std::string::npos
                 ? "The model replied with text instead of a specification."
                 : "The response was cut off too early to recover anything from.";
    return out;
}

} // namespace forge::llm
