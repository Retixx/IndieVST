// ---------------------------------------------------------------------------
// The deterministic repairer (SPEC 8.5).
//
// Rejecting a graph is a bad user experience and a worse demo. Before we tell
// anyone "no", we try to make the graph legal ourselves - and we record every
// single change so the UI can show exactly what was adjusted. That transparency
// is the point: the safety layer should be visible, not silent.
// ---------------------------------------------------------------------------
#pragma once

#include "core/ir/Ir.h"

namespace forge::ir {

struct RepairOptions {
    /// Insert an amplitude envelope + VCA when the model forgot one. Without
    /// this a note would sustain forever, which is the single most common way
    /// a generated instrument is unusable.
    bool insertGateIfMissing = true;

    /// Remove nodes that cannot reach the output and are not used as
    /// modulation sources.
    bool pruneUnreachable = true;

    /// Enforce the MVP node/param/macro caps by dropping the overflow.
    bool enforceCaps = true;

    /// Surface oscillator wave, filter mode and similar settings as selectors
    /// when the model did not. Without this a generated instrument has no way
    /// to switch from saw to square.
    bool exposeSwitches = true;
};

/// Repairs in place. Always safe to call, including on a graph that already
/// validates - in that case it changes nothing and reports nothing.
void repair(Instrument& inst, IrReport& report, const RepairOptions& opts = {});

} // namespace forge::ir
