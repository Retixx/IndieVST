// ---------------------------------------------------------------------------
// Audio safety and CPU budget (SPEC 8.3 - 8.4).
//
// The validator answers "is this graph legal". This file answers the louder
// question: "if I build this and someone is wearing headphones, are they fine?"
//
// Note that these checks are belt AND braces. Every clamp here is duplicated by
// a runtime clamp inside the module that owns the parameter, because a value
// that reaches the audio thread is the last place to discover a mistake.
// ---------------------------------------------------------------------------
#pragma once

#include "core/ir/Ir.h"

namespace forge::ir {

/// Cost units per fraction-of-one-core. One unit is roughly one band-limited
/// oscillator instance. Calibrated so that a 16-voice pad with 8 unison
/// oscillators per voice, a filter and a reverb sits just inside a 0.35 budget,
/// which is about where a real plugin should land on a mid-range laptop.
/// tools/benchmark refreshes the per-module weights that feed this.
inline constexpr float kCostUnitsPerCpuFraction = 500.0f;

struct CostEstimate {
    float voiceCost  = 0.0f;   ///< per voice
    float globalCost = 0.0f;
    float totalCost  = 0.0f;   ///< voiceCost * polyphony + globalCost
    float cpuFraction = 0.0f;  ///< estimated fraction of one core
};

CostEstimate estimateCost(const Instrument& inst);

/// Clamps every generated value into its safe range and, if the graph is over
/// budget, reduces it (polyphony, then unison, then duplicate reverbs) before
/// giving up. Returns false only if the graph is still over budget after every
/// reduction has been applied.
///
/// Anything it changes is recorded as an IrIssue::Level::Fixed so the UI can
/// show the user exactly what was adjusted.
bool applySafety(Instrument& inst, IrReport& report, float cpuBudget = 0.35f);

} // namespace forge::ir
