// ---------------------------------------------------------------------------
// Getting a usable object out of a response that did not arrive intact.
//
// "The model did not return a JSON object" is the worst failure this product
// has, because it throws away a complete design over a formatting accident and
// drops the musician onto a fallback instrument that looks nothing like what
// they asked for. In practice the response is almost never garbage - it is
// wrapped in prose, fenced in markdown, or cut off partway through because a
// read timed out or the token ceiling was hit.
//
// A patch that is 95% complete is worth far more than a canned instrument, so
// this module tries hard, in a strictly ordered set of attempts, and reports
// exactly which one succeeded so the UI can be honest about it.
// ---------------------------------------------------------------------------
#pragma once

#include <string>

namespace forge::llm {

enum class Salvage {
    None,        ///< nothing object-shaped in there at all
    Clean,       ///< a balanced object, exactly as sent
    Unwrapped,   ///< found inside prose or a markdown fence
    Repaired     ///< the response was cut off; the tail was reconstructed
};

struct SalvageResult {
    Salvage     how = Salvage::None;
    std::string json;         ///< empty unless `how != None`
    std::string note;         ///< human-readable account of what was done

    explicit operator bool() const { return how != Salvage::None; }
};

/// Best-effort extraction of one JSON object from a raw model response.
///
/// Order of attempts:
///   1. The whole thing parses as an object.
///   2. The largest balanced `{...}` span anywhere in the text.
///   3. Truncation repair: close whatever was left open and drop the partial
///      trailing member, then require the result to actually parse.
///
/// Never throws. Never returns text that does not parse as a JSON object.
SalvageResult salvageJsonObject(const std::string& raw);

} // namespace forge::llm
