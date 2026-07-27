// ---------------------------------------------------------------------------
// IR -> runtime graph.
//
// Everything expensive happens here, on a worker thread: instantiating modules,
// baking wavetables, allocating every buffer for every voice, resolving the
// modulation matrix into flat integer indices. By the time the audio thread
// sees the result there is nothing left to decide and nothing left to allocate.
// ---------------------------------------------------------------------------
#pragma once

#include "core/dsp/GraphInstance.h"

namespace forge {

class GraphBuilder {
public:
    /// Returns nullptr if the instrument does not validate. The caller is
    /// expected to have run repair/safety already; this re-validates anyway,
    /// because building an invalid graph is the one mistake we cannot recover
    /// from at runtime.
    static std::unique_ptr<GraphInstance> build(const ir::Instrument& inst,
                                                double sampleRate,
                                                ir::IrReport& report);
};

} // namespace forge
