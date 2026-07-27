// ---------------------------------------------------------------------------
// The trust boundary (SPEC 8.1 - 8.2).
//
// Nothing generated reaches the audio thread without passing through here.
// The validator answers one question: "can this graph be built and rendered
// without crashing, hanging, or producing something dangerous?"
//
// It deliberately does not answer "does this sound good" - that is the prompt's
// job, and conflating the two would make the safety layer negotiable.
// ---------------------------------------------------------------------------
#pragma once

#include "core/ir/Ir.h"

namespace forge::ir {

/// Node types whose output is delayed by at least one control block. A cycle in
/// the audio graph is legal if and only if it passes through one of these.
bool isLatencyNode(const std::string& type);

struct ValidationResult {
    bool ok = false;
    /// Execution order for global-scope nodes (indices into Instrument::nodes).
    std::vector<int> globalOrder;
    /// Execution order for voice-scope nodes.
    std::vector<int> voiceOrder;
    /// Voice-scope nodes whose audio feeds the global section (the voice sum).
    std::vector<int> voiceTerminals;
    /// Index of the mandatory out.master node, or -1 if absent.
    int masterIndex = -1;
};

/// Full structural + legality pass. Never mutates the instrument.
bool validate(const Instrument& inst, IrReport& report, ValidationResult* result = nullptr);

/// Topological sort of one scope with edges into latency nodes removed, which
/// is what makes delay/comb feedback loops both legal and schedulable.
/// Returns false if a cycle remains.
bool topoSort(const Instrument& inst, Scope scope, std::vector<int>& orderOut,
              std::vector<std::pair<int, int>>* cycleEdgesOut = nullptr);

} // namespace forge::ir
