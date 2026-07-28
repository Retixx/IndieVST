// ---------------------------------------------------------------------------
// Making the right engine non-negotiable.
//
// Asking a language model nicely, three separate ways, still produced a
// saw-and-lowpass synthesiser when the request said "electric guitar". The
// prompt teaches the recipe, the checker catches the miss, and the retry sends
// it back - and it can still come out wrong, because every one of those is a
// request rather than a guarantee.
//
// This is the guarantee. If the musician named a plucked, struck or bowed
// instrument and the patch still is not built that way after the retry, we
// build it that way ourselves. That is a defensible thing to do because it is
// not a matter of taste: a plucked string is a physical model, not a sawtooth,
// and that knowledge is ours, deterministic, and testable.
//
// The model keeps everything else it decided - the filter, the effects, the
// layout, the macros, the character. Only the sound source is corrected, and
// the change is recorded so the musician is told it happened.
// ---------------------------------------------------------------------------
#pragma once

#include "core/ir/Ir.h"
#include "core/llm/Compliance.h"

#include <string>

namespace forge::llm {

/// Rebuilds the source section of `inst` around the physical model the named
/// instrument actually requires. Does nothing when the prompt names no real
/// instrument, or when the patch already uses the right engine.
///
/// Returns true if anything was changed.
bool enforceInstrumentFamily(ir::Instrument& inst, const std::string& prompt,
                             ir::IrReport& report);

} // namespace forge::llm
