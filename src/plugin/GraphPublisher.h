// ---------------------------------------------------------------------------
// Handing a finished graph to the audio thread (SPEC 4.1).
//
// This is the mechanism that makes invariant I1 true: the LLM, the validator
// and the builder all run somewhere else, and the only thing that ever crosses
// into the audio thread is one pointer, published with a single atomic
// exchange.
//
// Retired graphs are never deleted by the audio thread. They go into a list
// owned by the message thread and are freed only once the audio thread has
// provably moved on - two full blocks after they stopped being referenced.
// ---------------------------------------------------------------------------
#pragma once

#include "core/dsp/GraphInstance.h"

#include <juce_core/juce_core.h>

#include <atomic>
#include <memory>
#include <vector>

namespace forge {

class GraphPublisher {
public:
    GraphPublisher() = default;
    ~GraphPublisher();

    // --- message / worker thread -------------------------------------------

    /// Takes ownership. The previously active graph becomes the "retiring"
    /// graph so the audio thread can crossfade out of it.
    void publish(std::unique_ptr<GraphInstance> graph);

    /// Drops everything. Only safe once the processor has stopped calling
    /// process(), i.e. from releaseResources or the destructor.
    void clear();

    /// Frees retired graphs the audio thread can no longer be holding. Call
    /// from a message-thread timer.
    void collectGarbage();

    bool hasGraph() const noexcept { return current_.load(std::memory_order_acquire) != nullptr; }

    // --- audio thread -------------------------------------------------------

    GraphInstance* current()  const noexcept { return current_.load(std::memory_order_acquire); }
    GraphInstance* retiring() const noexcept { return retiring_.load(std::memory_order_acquire); }

    /// Called by the audio thread when the crossfade out of `g` has finished.
    /// Uses compare-exchange so a publish that happened in the meantime is not
    /// clobbered.
    void finishRetiring(GraphInstance* g) noexcept {
        retiring_.compare_exchange_strong(g, nullptr, std::memory_order_acq_rel);
    }

    /// Bumped once per processBlock. The garbage collector uses it to prove the
    /// audio thread has moved past a retired pointer.
    void tick() noexcept { blockCounter_.fetch_add(1, std::memory_order_release); }

private:
    struct Owned {
        std::unique_ptr<GraphInstance> graph;
        uint64_t retiredAtBlock = 0;
        bool     retired = false;
    };

    std::atomic<GraphInstance*> current_{nullptr};
    std::atomic<GraphInstance*> retiring_{nullptr};
    std::atomic<uint64_t>       blockCounter_{0};

    /// Touched only on the message/worker thread (publish and collectGarbage),
    /// serialised by this lock. The audio thread never takes it.
    juce::CriticalSection lock_;
    std::vector<Owned>    owned_;
};

} // namespace forge
