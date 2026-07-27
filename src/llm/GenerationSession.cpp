#include "llm/GenerationSession.h"

#include "core/dsp/GraphBuilder.h"
#include "core/ir/IrRepair.h"
#include "core/ir/IrSafety.h"
#include "core/ir/IrValidator.h"
#include "core/llm/PromptBuilder.h"
#include "llm/HttpLlmProvider.h"

namespace forge {
namespace {

juce::String humaniseErrors(const ir::IrReport& report, int maxItems = 3) {
    juce::StringArray lines;
    for (const auto& i : report.issues) {
        if (i.level != ir::IrIssue::Level::Error) continue;
        lines.add(juce::String(i.message));
        if (lines.size() >= maxItems) break;
    }
    return lines.joinIntoString("  ");
}

juce::String fullReport(const ir::IrReport& report) {
    juce::StringArray lines;
    for (const auto& i : report.issues) {
        const char* level = i.level == ir::IrIssue::Level::Error   ? "ERROR"
                          : i.level == ir::IrIssue::Level::Warning ? "warn "
                                                                   : "fixed";
        lines.add(juce::String(level) + "  [" + juce::String(i.path) + "] "
                  + juce::String(i.message));
    }
    return lines.joinIntoString("\n");
}

/// A failed generation that leaves no trace cannot be fixed. This always runs.
juce::String writeGenerationLog(const juce::String& prompt,
                                const juce::String& provider,
                                const std::string& raw,
                                const ir::IrReport& report,
                                const juce::String& outcome) {
    const auto dir = ForgeConfig::logsDirectory();
    if (!dir.createDirectory().wasOk()) return {};

    const auto file = dir.getChildFile(
        "generation-" + juce::Time::getCurrentTime().formatted("%Y%m%d-%H%M%S") + ".log");

    juce::String text;
    text << "prompt   : " << prompt << "\n"
         << "provider : " << provider << "\n"
         << "outcome  : " << outcome << "\n"
         << "\n--- validator ---\n" << fullReport(report)
         << "\n\n--- raw model response ---\n"
         << (raw.empty() ? juce::String("(none)") : juce::String(raw)) << "\n";

    return file.replaceWithText(text) ? file.getFullPathName() : juce::String();
}

} // namespace

// ---------------------------------------------------------------------------

class GenerationSession::Job final : public juce::ThreadPoolJob {
public:
    Job(ForgeConfig config, double sampleRate,
        juce::String prompt, juce::String currentIr,
        ProgressFn onProgress, CompleteFn onComplete,
        std::shared_ptr<std::atomic<bool>> cancelFlag,
        std::atomic<bool>* runningFlag)
        : juce::ThreadPoolJob("forge-generate"),
          config_(std::move(config)),
          sampleRate_(sampleRate),
          prompt_(std::move(prompt)),
          currentIr_(std::move(currentIr)),
          onProgress_(std::move(onProgress)),
          onComplete_(std::move(onComplete)),
          cancelFlag_(std::move(cancelFlag)),
          runningFlag_(runningFlag) {}

    JobStatus runJob() override {
        auto result = std::make_shared<Result>();
        run(*result);

        if (runningFlag_ != nullptr) runningFlag_->store(false, std::memory_order_release);

        if (!cancelled()) {
            auto complete = onComplete_;
            juce::MessageManager::callAsync([result, complete]() {
                if (complete) complete(*result);
            });
        }
        return jobHasFinished;
    }

private:
    bool cancelled() const {
        return shouldExit() || (cancelFlag_ && cancelFlag_->load(std::memory_order_acquire));
    }

    void progress(const juce::String& text) {
        if (cancelled()) return;
        auto fn = onProgress_;
        juce::MessageManager::callAsync([fn, text]() { if (fn) fn(text); });
    }

    /// One full attempt: model -> extract -> parse -> repair -> safety ->
    /// validate.
    ///
    /// Repair runs BEFORE the verdict on purpose. Judging the raw response and
    /// only repairing after every retry had been spent meant a graph missing
    /// nothing but an output connection burned a whole extra API call before
    /// anyone tried to fix it. Repair is deterministic and free; the model is
    /// neither.
    bool attempt(llm::LlmProvider& provider,
                 llm::GenerationRequest& request,
                 ir::Instrument& inst,
                 ir::IrReport& errors,
                 ir::IrReport& repairs,
                 Result& out,
                 std::string& rawOut) {
        const auto response = provider.generate(request, [this] { return cancelled(); });
        out.attempts++;
        out.latencyMs += response.latencyMs;
        out.providerName = response.providerName.empty()
                             ? provider.name() : juce::String(response.providerName);
        out.usedFallback = out.usedFallback || response.usedFallback;

        if (!response.ok) {
            // A transport or API error (bad key, rejected parameter, no
            // network) will fail identically on a retry. Flag it so we do not
            // burn another round trip - and several seconds of the
            // time-to-first-sound budget - proving that.
            transportFailure_ = true;
            errors.error("", response.errorMessage.empty() ? "The model did not respond."
                                                           : response.errorMessage);
            return false;
        }
        transportFailure_ = false;
        rawOut = response.irJson;

        if (!ir::parse(response.irJson, inst, errors)) return false;

        ir::repair(inst, repairs);
        if (!ir::applySafety(inst, repairs, config_.cpuBudget)) {
            for (const auto& i : repairs.issues)
                if (i.level == ir::IrIssue::Level::Error) errors.issues.push_back(i);
            return false;
        }
        return ir::validate(inst, errors);
    }

    void run(Result& out) {
        if (cancelled()) { out.message = "Cancelled."; return; }

        progress("Designing...");

        auto provider = llm::makeProvider(config_);
        const auto promptSpec = llm::buildGenerationPrompt(prompt_.toStdString(),
                                                           currentIr_.toStdString());

        llm::GenerationRequest request;
        request.systemPrompt = promptSpec.system;
        request.userMessage  = promptSpec.user;
        request.temperature  = config_.temperature;
        request.timeoutMs    = config_.timeoutMs;

        ir::Instrument inst;
        ir::IrReport   errors, repairReport;
        std::string    raw;
        bool valid = attempt(*provider, request, inst, errors, repairReport, out, raw);

        // --- retry with the validator's own error list -----------------------
        for (int retry = 0; !valid && !transportFailure_
                            && retry < config_.maxRetries && !cancelled(); ++retry) {
            progress("Fixing...");
            request.correctionFeedback = errors.toModelFeedback();
            request.previousAttempt    = raw;
            inst = ir::Instrument{};
            errors.clear();
            repairReport.clear();
            valid = attempt(*provider, request, inst, errors, repairReport, out, raw);
        }

        if (cancelled()) { out.message = "Cancelled."; return; }
        progress("Validating...");

        // Always leave a trace of a failure, and of a success when asked.
        if (!valid || config_.logRawResponses) {
            ir::IrReport combined = errors;
            for (const auto& i : repairReport.issues) combined.issues.push_back(i);
            out.logPath = writeGenerationLog(prompt_, out.providerName, raw, combined,
                                             valid ? "ok" : "failed validation");
        }

        // --- last resort: the offline library --------------------------------
        if (!valid) {
            progress("Falling back...");
            llm::CannedProvider canned;
            llm::GenerationRequest cannedRequest = request;
            cannedRequest.correctionFeedback.clear();
            cannedRequest.userMessage = prompt_.toStdString();

            const auto fallback = canned.generate(cannedRequest, [this] { return cancelled(); });
            if (fallback.ok) {
                ir::Instrument fb;
                ir::IrReport   fbReport;
                if (ir::parse(fallback.irJson, fb, fbReport)) {
                    ir::repair(fb, fbReport);
                    ir::applySafety(fb, fbReport, config_.cpuBudget);
                    ir::IrReport check;
                    if (ir::validate(fb, check)) {
                        inst = std::move(fb);
                        out.usedFallback = true;
                        const juce::String why = humaniseErrors(errors);
                        out.message = why.isEmpty()
                            ? "I had trouble with that one - here's a starting point you can edit."
                            : "Fell back: " + why;
                    }
                }
            }
            if (!out.usedFallback) {
                out.message = "Generation failed: " + humaniseErrors(errors);
                return;
            }
        }

        if (cancelled()) { out.message = "Cancelled."; return; }

        // --- build ------------------------------------------------------------
        progress("Building...");
        ir::IrReport buildReport;
        auto graph = GraphBuilder::build(inst, sampleRate_, buildReport);
        if (graph == nullptr) {
            out.message = "Could not build the instrument: " + humaniseErrors(buildReport);
            return;
        }

        out.ok         = true;
        out.instrument = inst;
        out.graph      = std::move(graph);
        out.repairSummary = juce::String(repairReport.toUserSummary());
        if (out.message.isEmpty()) {
            out.message = out.usedFallback
                ? "Loaded from the offline library."
                : "Ready.";
        }
    }

    ForgeConfig  config_;
    double       sampleRate_;
    juce::String prompt_, currentIr_;
    ProgressFn   onProgress_;
    CompleteFn   onComplete_;
    bool transportFailure_ = false;
    std::shared_ptr<std::atomic<bool>> cancelFlag_;
    std::atomic<bool>* runningFlag_;
};

// ---------------------------------------------------------------------------

GenerationSession::GenerationSession() = default;

GenerationSession::~GenerationSession() {
    cancel();
    pool_.removeAllJobs(true, 3000);
}

void GenerationSession::start(const ForgeConfig& config,
                              double sampleRate,
                              const juce::String& prompt,
                              const juce::String& currentIrJson,
                              ProgressFn onProgress,
                              CompleteFn onComplete) {
    cancel();
    pool_.removeAllJobs(true, 2000);

    cancelFlag_ = std::make_shared<std::atomic<bool>>(false);
    running_.store(true, std::memory_order_release);

    pool_.addJob(new Job(config, sampleRate, prompt, currentIrJson,
                         std::move(onProgress), std::move(onComplete),
                         cancelFlag_, &running_),
                 true);
}

void GenerationSession::cancel() {
    if (cancelFlag_) cancelFlag_->store(true, std::memory_order_release);
    running_.store(false, std::memory_order_release);
}

} // namespace forge
