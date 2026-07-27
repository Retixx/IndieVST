// ---------------------------------------------------------------------------
// The primitive registry (SPEC 6.5).
//
// This is the extensibility hinge of the whole product. Adding a new DSP
// primitive means adding a class and a manifest entry - and the validator, the
// LLM prompt and the UI all pick it up automatically, because all three read
// from this table. Nothing about a module is ever hand-duplicated elsewhere.
// ---------------------------------------------------------------------------
#pragma once

#include "core/dsp/Module.h"

#include <map>
#include <string>
#include <vector>

namespace forge {

class Registry {
public:
    /// Built once, on first use, on whatever thread asks first (in practice the
    /// message thread during plugin construction). Immutable thereafter, so it
    /// is safe to read from the audio thread.
    static const Registry& instance();

    const ModuleManifest* find(const std::string& type) const noexcept;
    const std::vector<ModuleManifest>& all() const noexcept { return modules_; }

    std::unique_ptr<Module> create(const std::string& type) const;

    /// The capability manifest handed to the LLM. Compact on purpose: this
    /// lands in every prompt, so every token has to earn its place.
    nlohmann::json capabilityJson() const;

    /// Human-readable dump, used by tools/print_manifest and the tests.
    std::string capabilityMarkdown() const;

private:
    Registry();
    std::vector<ModuleManifest>  modules_;
    std::map<std::string, size_t> index_;
};

/// Implemented in Modules.cpp - the core synthesis primitives.
void registerBuiltinModules(std::vector<ModuleManifest>& out);

/// Implemented in ModulesFx.cpp - the production effects and models that make
/// generated instruments sound finished rather than merely correct.
void registerProductionModules(std::vector<ModuleManifest>& out);

} // namespace forge
