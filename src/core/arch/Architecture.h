// ---------------------------------------------------------------------------
// The fixed instrument architecture.
//
// A change of approach, and worth stating plainly. Originally the model
// invented the whole signal path per prompt. That is elegant, and it produced
// instruments with sixteen knobs - technically bespoke, practically unusable.
// A producer does not want a different synth every time; they want THE synth,
// with the sections that suit the sound switched on.
//
// So the rack below is always the same: oscillators, two filters, four
// envelopes, four LFOs, a modulation matrix and a full effects chain. The model
// no longer designs topology. It chooses which sections are active, sets the
// values, adds modulation routes, and picks the styling - which is a far more
// reliable task than graph invention, and produces something a producer can
// actually work in.
//
// The engine underneath is unchanged: the same 39 audited primitives, the same
// validator, the same safety layer. This is a large IR, not a new runtime.
// ---------------------------------------------------------------------------
#pragma once

#include "core/ir/Ir.h"

#include <string>
#include <vector>

namespace forge::arch {

/// UI groups, in tab order. These become the tabs across the top of the rack.
struct Section {
    std::string id;
    std::string title;
    /// Sections the model may switch off entirely when they do not suit the
    /// sound. The core voice path is never optional.
    bool optional = false;
};

const std::vector<Section>& sections();

/// Builds the complete rack. Every node, every exposed control, every selector
/// and a set of sensible default modulation routes.
ir::Instrument buildFullArchitecture();

/// Removes the nodes belonging to sections the patch did not enable, together
/// with their controls and routes, and bridges the audio chain across the gap.
/// Keeping every effect instantiated would cost CPU for silence.
void pruneDisabledSections(ir::Instrument&, const std::vector<std::string>& enabledSections);

/// Applies a generated patch: name, description, accent, enabled sections,
/// parameter values, selector positions and extra modulation routes.
/// Unknown ids are reported and ignored rather than failing the generation.
bool applyPatch(ir::Instrument&, const nlohmann::json& patch, ir::IrReport&);

/// The compact schema the model fills in, for the prompt.
std::string patchSchemaDoc();

/// A patch derived from the words in the prompt, with no model involved.
///
/// This is the floor of the product. When the network is down, the key is
/// wrong, or a response arrives unusable, the musician still gets a complete,
/// laid-out, playable instrument that bears some relation to what they asked
/// for - not a hand-authored toy with a dozen knobs on it. It is deliberately
/// simple and deliberately deterministic: the same words always give the same
/// instrument, which is exactly what you want from a safety net.
nlohmann::json heuristicPatch(const std::string& prompt);

} // namespace forge::arch
