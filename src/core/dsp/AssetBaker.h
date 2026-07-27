// ---------------------------------------------------------------------------
// Asset baking (SPEC 5.4).
//
// The model emits recipes, not sample data - a harmonic series rather than 2048
// floats. That keeps generations fast and cheap, and it means the baker owns
// the guarantees: everything that comes out of here is band-limited, peak
// normalised and DC free, whatever the model asked for.
// ---------------------------------------------------------------------------
#pragma once

#include "core/dsp/Assets.h"
#include "core/ir/Ir.h"

namespace forge {

/// Expands every asset recipe. Worker thread only - this allocates and does a
/// few million floating point operations.
///
/// Never fails hard: an unusable recipe produces a sane default (a sine table,
/// an identity curve, a simple AD envelope) and a Fixed-level issue, because a
/// dull wavetable is a far better outcome than a failed generation.
void bakeAssets(const std::vector<ir::AssetSpec>& specs, BakedAssets& out, ir::IrReport& report);

// Exposed for the unit tests.
void bakeWavetable(const nlohmann::json& spec, Wavetable& out);
void bakeCurve(const nlohmann::json& spec, Curve& out);
void bakeEnvelope(const nlohmann::json& spec, EnvelopeShape& out);

} // namespace forge
