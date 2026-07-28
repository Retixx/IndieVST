// ---------------------------------------------------------------------------
// The generation pipeline (SPEC 7.3, 7.4).
//
//   prompt -> model -> extract -> validate -> [retry with the validator's own
//   error list] -> repair -> safety -> build -> publish
//
// All of it on a worker thread. The audio thread is never blocked, never
// allocates on behalf of this, and never learns that any of it happened until
// a finished GraphInstance appears.
//
// The ladder always terminates in something playable: if every retry fails, the
// offline library answers instead. A failed generation should cost the user a
// few seconds, never the demo.
// ---------------------------------------------------------------------------
#pragma once

#include "core/dsp/GraphInstance.h"
#include "core/ir/Ir.h"
#include "plugin/ForgeConfig.h"

#include <atomic>
#include <functional>
#include <juce_events/juce_events.h>
#include <memory>

namespace forge {

class GenerationSession {
public:
    struct Result {
        bool            ok = false;
        ir::Instrument  instrument;
        std::unique_ptr<GraphInstance> graph;

        juce::String message;        ///< user-facing outcome line
        juce::String repairSummary;  ///< "what I fixed", may be empty
        juce::String providerName;
        /// Written on every failure. Contains the prompt, the raw model
        /// response and the full validator output - without it a failed
        /// generation is unfixable guesswork.
        juce::String logPath;
        bool         usedFallback = false;
        double       latencyMs    = 0.0;
        int          attempts     = 0;
    };

    using ProgressFn = std::function<void(const juce::String&)>;
    using CompleteFn = std::function<void(Result&)>;

    GenerationSession();
    ~GenerationSession();

    /// `currentIrJson` empty = generate new, otherwise edit.
    /// Callbacks are always delivered on the message thread.
    void start(const ForgeConfig& config,
               double sampleRate,
               const juce::String& prompt,
               const juce::String& currentIrJson,
               const juce::String& referenceText,
               ProgressFn onProgress,
               CompleteFn onComplete);

    /// The in-flight job stops as soon as it next checks. A network read
    /// already in progress is left to time out; its result is discarded.
    void cancel();

    bool isRunning() const noexcept { return running_.load(std::memory_order_acquire); }

private:
    class Job;

    juce::ThreadPool                  pool_{1};
    std::shared_ptr<std::atomic<bool>> cancelFlag_;
    std::atomic<bool>                 running_{false};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(GenerationSession)
};

} // namespace forge
