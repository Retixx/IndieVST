// ---------------------------------------------------------------------------
// The offline instrument library (SPEC 7.1).
//
// These hand-authored instruments do three jobs at once:
//   1. they are the CannedProvider's fallback when the network or the API key
//      is unavailable - the demo cannot be killed by hotel wi-fi;
//   2. they are the few-shot examples injected into the LLM prompt, which is
//      the single biggest lever on generation quality;
//   3. they seed a new user's instrument library so the plugin is never empty.
//
// Because they are prompt material, the test suite validates every one of them
// on every build. A broken example here would quietly poison every generation.
// ---------------------------------------------------------------------------
#pragma once

#include <string>
#include <vector>

namespace forge::llm {

struct CannedInstrument {
    const char* id;
    const char* title;
    /// Words that route a prompt to this instrument in offline mode.
    std::vector<std::string> keywords;
    /// True if this one is used as a few-shot example in the prompt. Keeping
    /// this to two examples holds the prompt to a sensible size.
    bool useAsExample = false;
    std::string json;
};

const std::vector<CannedInstrument>& cannedLibrary();

/// Best keyword match for a prompt, falling back to the first entry. Never
/// returns nullptr as long as the library is non-empty.
const CannedInstrument* bestCannedMatch(const std::string& prompt);

} // namespace forge::llm
