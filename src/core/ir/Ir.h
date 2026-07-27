// ---------------------------------------------------------------------------
// The Instrument IR (SPEC 5).
//
// This is the contract between the language model and the DSP engine, and it
// is the most important artifact in the system. It has to be expressive enough
// that two prompts produce genuinely different signal paths, and constrained
// enough that a validator can prove the result safe to send to a pair of
// monitors.
//
// Everything that arrives from the model is TREATED AS UNTRUSTED INPUT FROM THE
// INTERNET, because that is exactly what it is.
// ---------------------------------------------------------------------------
#pragma once

#include "core/Common.h"

#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace forge::ir {

inline constexpr const char* kCurrentIrVersion = "0.1";

/// Largest response we will even attempt to parse.
inline constexpr size_t kMaxIrBytes = 256 * 1024;

/// nlohmann's parser is recursive, so deeply nested input is a stack-overflow
/// vector. We pre-scan and reject anything past this before parsing.
inline constexpr int kMaxJsonDepth = 64;

// --- Diagnostics -----------------------------------------------------------

struct IrIssue {
    enum class Level {
        Error,   ///< graph cannot be built as-is
        Warning, ///< legal but suspicious
        Fixed    ///< the repairer or a clamp already dealt with it
    };
    Level       level = Level::Error;
    std::string path;    ///< e.g. "nodes[3].settings.unison"
    std::string message;
};

struct IrReport {
    std::vector<IrIssue> issues;

    void error  (std::string path, std::string msg) { issues.push_back({IrIssue::Level::Error,   std::move(path), std::move(msg)}); }
    void warn   (std::string path, std::string msg) { issues.push_back({IrIssue::Level::Warning, std::move(path), std::move(msg)}); }
    void fixed  (std::string path, std::string msg) { issues.push_back({IrIssue::Level::Fixed,   std::move(path), std::move(msg)}); }

    bool hasErrors() const;
    int  count(IrIssue::Level) const;
    void clear() { issues.clear(); }

    /// Machine-readable error list fed back to the model on a retry (SPEC 7.3).
    std::string toModelFeedback() const;

    /// Short human summary for the "what I fixed" panel in the UI.
    std::string toUserSummary() const;
};

// --- IR structures ---------------------------------------------------------

struct AssetSpec {
    std::string    id;
    std::string    kind;  ///< wavetable | curve | envelope
    nlohmann::json spec;  ///< generative recipe, expanded by the AssetBaker
};

struct NodeSpec {
    std::string    id;
    std::string    type;
    Scope          scope = Scope::Voice;
    nlohmann::json settings = nlohmann::json::object();
};

struct AudioConn {
    std::string from;   ///< node id
    std::string to;     ///< node id
    float       gain = 1.0f;
};

struct ModRoute {
    std::string source;       ///< id of a mod-source node
    std::string targetNode;
    std::string targetParam;
    float       depth   = 0.0f;   ///< -1..1, applied in the target's normalised domain
    Taper       curve   = Taper::Linear;
    bool        bipolar = false;  ///< true: source is remapped from 0..1 to -1..1
};

struct ParamBind {
    std::string node;
    std::string param;
    float       scale  = 1.0f;
    float       offset = 0.0f;
};

struct ParamSpec {
    std::string            id;
    std::string            label;
    std::string            unit;
    float                  min = 0.0f, max = 1.0f, def = 0.5f;
    Taper                  taper   = Taper::Linear;
    std::string            control = "knob";  ///< knob | slider | switch | combo
    std::string            group   = "General";
    std::vector<ParamBind> bind;

    float defaultNormalised() const { return removeTaper(def, min, max, taper); }
};

struct MacroRoute {
    std::string param;             ///< id of an exposed ParamSpec
    float       depth = 0.0f;      ///< -1..1 in the target's normalised domain
    Taper       curve = Taper::Linear;
};

struct MacroSpec {
    std::string             id;
    std::string             label;
    float                   def = 0.0f;   ///< 0..1
    std::vector<MacroRoute> routes;
};

struct UiSection {
    std::string              title;
    std::vector<std::string> params;
};

struct UiSpec {
    std::string              accent = "#E4572E";
    std::string              mood   = "neutral";
    std::vector<UiSection>   sections;
    std::vector<std::string> macroRow;
};

struct Instrument {
    std::string    irVersion   = kCurrentIrVersion;
    std::string    name        = "Untitled";
    std::string    description;
    nlohmann::json meta = nlohmann::json::object();

    std::string voicing   = "poly";   ///< poly | mono | legato
    float       glideMs   = 0.0f;
    int         polyphony = kDefaultVoices;
    uint64_t    seed      = 0x5EEDF00Dull;

    std::vector<AssetSpec> assets;
    std::vector<NodeSpec>  nodes;
    std::vector<AudioConn> audio;
    std::vector<ModRoute>  mod;
    std::vector<ParamSpec> params;
    std::vector<MacroSpec> macros;
    UiSpec                 ui;

    const NodeSpec* findNode(const std::string& id) const;
    NodeSpec*       findNode(const std::string& id);
    const ParamSpec* findParam(const std::string& id) const;
    int              indexOfNode(const std::string& id) const;
};

// --- Serialisation ---------------------------------------------------------

/// Depth-limited, size-limited, exception-free JSON parse. Returns false and
/// fills the report on any problem; never throws, never overflows the stack.
bool parseJsonSafely(const std::string& text, nlohmann::json& out, IrReport& report);

/// Lenient extraction of an Instrument from parsed JSON. "Lenient" means it
/// accepts several spellings the model plausibly produces (a connection written
/// as "osc:out" or just "osc"; a mod target as "filt.cutoff" or as an object)
/// and records what it could not understand rather than giving up. Semantic
/// legality is IrValidator's job, not this function's.
bool fromJson(const nlohmann::json& j, Instrument& out, IrReport& report);

nlohmann::json toJson(const Instrument& inst);

/// Convenience: parse + extract in one call.
bool parse(const std::string& text, Instrument& out, IrReport& report);

// --- Small shared helpers --------------------------------------------------

/// "osc_main:out" -> "osc_main". Also trims whitespace.
std::string stripPort(std::string endpoint);

/// "filt_lp.cutoff" -> {"filt_lp", "cutoff"}. Returns false if there is no dot.
bool splitTarget(const std::string& target, std::string& node, std::string& param);

/// Identifier rule: ^[a-z][a-z0-9_]{0,31}$
bool isValidIdentifier(const std::string& id);

/// Coerces anything vaguely identifier-shaped into a legal identifier so a
/// stray capital letter or hyphen does not cost the user a whole generation.
std::string sanitiseIdentifier(const std::string& id);

} // namespace forge::ir
