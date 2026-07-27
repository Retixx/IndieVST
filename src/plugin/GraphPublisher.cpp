#include "plugin/GraphPublisher.h"

namespace forge {

GraphPublisher::~GraphPublisher() { clear(); }

void GraphPublisher::publish(std::unique_ptr<GraphInstance> graph) {
    if (graph == nullptr) return;

    GraphInstance* raw = graph.get();

    const juce::ScopedLock sl(lock_);
    owned_.push_back({std::move(graph), 0, false});

    // The graph that was live becomes the one we fade out of. If a previous
    // crossfade was still running, its graph is dropped immediately - two
    // generations landing inside 20 ms is not worth a three-way fade.
    GraphInstance* previousCurrent = current_.exchange(raw, std::memory_order_acq_rel);
    retiring_.store(previousCurrent, std::memory_order_release);

    // Everything except the new graph is now on its way out. Stamping them all
    // here (including the one still fading) is what stops a graph from being
    // stranded as un-freeable once the audio thread clears retiring_.
    const uint64_t now = blockCounter_.load(std::memory_order_acquire);
    for (auto& o : owned_) {
        if (o.graph.get() == raw || o.retired) continue;
        o.retired = true;
        o.retiredAtBlock = now;
    }
}

void GraphPublisher::collectGarbage() {
    GraphInstance* live    = current_.load(std::memory_order_acquire);
    GraphInstance* fading   = retiring_.load(std::memory_order_acquire);
    const uint64_t now      = blockCounter_.load(std::memory_order_acquire);

    const juce::ScopedLock sl(lock_);
    for (size_t i = 0; i < owned_.size();) {
        GraphInstance* g = owned_[i].graph.get();
        const bool referenced = (g == live) || (g == fading);
        // Two full blocks of separation guarantees no processBlock call that
        // could still be holding this pointer is in flight.
        const bool settled = owned_[i].retired && (now > owned_[i].retiredAtBlock + 2);
        if (!referenced && settled) {
            owned_.erase(owned_.begin() + static_cast<long>(i));
            continue;
        }
        ++i;
    }
}

void GraphPublisher::clear() {
    current_.store(nullptr, std::memory_order_release);
    retiring_.store(nullptr, std::memory_order_release);
    const juce::ScopedLock sl(lock_);
    owned_.clear();
}

} // namespace forge
