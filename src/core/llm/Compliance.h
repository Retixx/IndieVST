// ---------------------------------------------------------------------------
// Did it actually do what was asked?
//
// A producer wrote a precise brief - four macros named Weight, Bite, Motion and
// Space; glide for overlapping notes; the mod wheel mapped to distortion,
// brightness and movement - and got back an instrument with none of those,
// plus a description confidently claiming all of them.
//
// Two separate failures. One was capability: the schema had no `macros` field,
// so compliance was impossible. The other is structural, and this module exists
// for it: NOTHING was checking. A long brief is a list of promises, and a model
// that satisfies eighty percent of a list and narrates a hundred percent will
// pass unnoticed every single time unless something verifies it.
//
// So: pull the checkable requests out of the prompt, check them against the
// instrument that was actually built, and hand any misses back to the model as
// a specific, itemised correction. Deterministic, and deliberately narrow - it
// only tests things that are unambiguous in the text and unambiguous in the
// result. It would rather miss a request than invent one.
// ---------------------------------------------------------------------------
#pragma once

#include "core/ir/Ir.h"

#include <string>
#include <vector>

namespace forge::llm {

struct Requirement {
    std::string id;        ///< stable key, e.g. "macros.named"
    std::string asked;     ///< what the musician asked for, in their words
    std::string howToFix;  ///< concrete instruction if it was not met
    bool        met = false;
};

struct ComplianceReport {
    std::vector<Requirement> requirements;

    int  unmetCount() const;
    bool allMet() const { return unmetCount() == 0; }

    /// Itemised correction for a retry. Empty when everything was satisfied.
    std::string toModelFeedback() const;

    /// One line for the status strip, e.g. "3 of 4 requests satisfied".
    std::string toUserSummary() const;
};

/// Reads the prompt for explicit, checkable requests and verifies each against
/// the built instrument.
ComplianceReport check(const std::string& prompt, const ir::Instrument&);

/// Named macros found in a prompt: "macros: Weight, Bite, Motion and Space"
/// -> {"Weight","Bite","Motion","Space"}. Exposed for testing.
std::vector<std::string> requestedMacroNames(const std::string& prompt);

/// How a real instrument named in the prompt is physically produced.
///
/// One source of truth, because the checker and the corrector must agree: a
/// requirement that the corrector cannot satisfy would send the model round in
/// circles, and a correction the checker does not accept would loop forever.
enum class Family { None, Plucked, Bowed, Struck, Mallet, Wind, Reed };

struct NamedInstrument {
    Family      family = Family::None;
    std::string word;            ///< the word that matched, for messages
    explicit operator bool() const { return family != Family::None; }
};

/// Empty when the prompt names no real instrument, or explicitly asks for a
/// synth ("synth bass", "808", "analog").
NamedInstrument namedInstrument(const std::string& prompt);

} // namespace forge::llm
