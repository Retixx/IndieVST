#include "plugin/InstrumentLibrary.h"

#include "core/llm/CannedLibrary.h"
#include "plugin/ForgeConfig.h"

#include <algorithm>
#include <functional>

namespace forge {
namespace {

juce::String slugify(const juce::String& name) {
    juce::String out;
    for (auto c : name) {
        if (juce::CharacterFunctions::isLetterOrDigit(c)) out << juce::String::charToString(c).toLowerCase();
        else if (out.isNotEmpty() && !out.endsWithChar('-')) out << '-';
    }
    out = out.trimCharactersAtEnd("-");
    return out.isEmpty() ? "instrument" : out.substring(0, 40);
}

} // namespace

juce::String InstrumentLibrary::makeId(const ir::Instrument& instrument) {
    const auto json = ir::toJson(instrument).dump();
    const auto hash = juce::String::toHexString(
        static_cast<juce::int64>(std::hash<std::string>{}(json) & 0xFFFFFFFFull));
    return slugify(juce::String(instrument.name)) + "-" + hash;
}

juce::File InstrumentLibrary::fileFor(const juce::String& id) {
    return ForgeConfig::instrumentsDirectory().getChildFile(id + ".forge.json");
}

void InstrumentLibrary::writeToDisk(const LibraryEntry& entry) const {
    ForgeConfig::instrumentsDirectory().createDirectory();

    auto json = ir::toJson(entry.instrument);
    json["meta"]["prompt"]  = entry.prompt.toStdString();
    json["meta"]["id"]      = entry.id.toStdString();
    json["meta"]["created"] = static_cast<long long>(entry.created);

    fileFor(entry.id).replaceWithText(juce::String(json.dump(2)));
}

void InstrumentLibrary::seedIfEmpty() {
    const auto dir = ForgeConfig::instrumentsDirectory();
    dir.createDirectory();
    if (dir.getNumberOfChildFiles(juce::File::findFiles, "*.forge.json") > 0) return;

    for (const auto& canned : llm::cannedLibrary()) {
        ir::Instrument inst;
        ir::IrReport report;
        if (!ir::parse(canned.json, inst, report)) continue;

        LibraryEntry entry;
        entry.id      = juce::String(canned.id);
        entry.name    = juce::String(canned.title);
        entry.prompt  = "Built-in example";
        entry.created = juce::Time::currentTimeMillis();
        entry.onDisk  = true;
        entry.instrument = std::move(inst);
        writeToDisk(entry);
    }
}

void InstrumentLibrary::loadFromDisk() {
    entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                                  [](const LibraryEntry& e) { return e.onDisk; }),
                   entries_.end());

    const auto dir = ForgeConfig::instrumentsDirectory();
    if (!dir.isDirectory()) return;

    for (const auto& file : dir.findChildFiles(juce::File::findFiles, false, "*.forge.json")) {
        ir::Instrument inst;
        ir::IrReport report;
        if (!ir::parse(file.loadFileAsString().toStdString(), inst, report)) continue;

        LibraryEntry entry;
        entry.id      = file.getFileNameWithoutExtension().upToLastOccurrenceOf(".forge", false, false);
        if (entry.id.isEmpty()) entry.id = file.getFileNameWithoutExtension();
        entry.name    = uniqueName(juce::String(inst.name));
        entry.onDisk  = true;
        entry.created = file.getCreationTime().toMilliseconds();

        if (inst.meta.is_object()) {
            auto p = inst.meta.find("prompt");
            if (p != inst.meta.end() && p->is_string()) entry.prompt = juce::String(p->get<std::string>());
        }
        entry.instrument = std::move(inst);

        // Session entries win over disk entries with the same id.
        if (find(entry.id) == nullptr) entries_.push_back(std::move(entry));
    }

    std::stable_sort(entries_.begin(), entries_.end(),
                     [](const LibraryEntry& a, const LibraryEntry& b) {
                         if (a.onDisk != b.onDisk) return !a.onDisk;   // session first
                         return a.name.compareIgnoreCase(b.name) < 0;
                     });
}

juce::String InstrumentLibrary::uniqueName(const juce::String& wanted,
                                           const juce::String& ignoreId) const {
    // Strip any existing " (n)" so repeated regeneration does not stack up
    // into "Bass (1) (1) (1)".
    juce::String base = wanted.trim();
    if (base.endsWithChar(')')) {
        const int open = base.lastIndexOfChar('(');
        if (open > 0 && base.substring(open + 1, base.length() - 1).containsOnly("0123456789"))
            base = base.substring(0, open).trim();
    }
    if (base.isEmpty()) base = "Instrument";

    auto taken = [this, &ignoreId](const juce::String& candidate) {
        for (const auto& e : entries_)
            if (e.id != ignoreId && e.name.equalsIgnoreCase(candidate)) return true;
        return false;
    };

    if (!taken(base)) return base;
    for (int n = 1; n < 1000; ++n) {
        const auto candidate = base + " (" + juce::String(n) + ")";
        if (!taken(candidate)) return candidate;
    }
    return base;
}

juce::String InstrumentLibrary::add(const ir::Instrument& instrument,
                                    const juce::String& prompt,
                                    bool persist) {
    LibraryEntry entry;
    entry.id      = makeId(instrument);
    entry.prompt  = prompt;
    entry.created = juce::Time::currentTimeMillis();
    entry.onDisk  = persist;
    entry.instrument = instrument;

    // Regenerating byte-identical output should not fill the list with clones.
    for (auto& existing : entries_) {
        if (existing.id == entry.id) {
            existing.prompt = prompt;
            return existing.id;
        }
    }

    entry.name = uniqueName(juce::String(instrument.name));
    entry.instrument.name = entry.name.toStdString();

    if (persist) writeToDisk(entry);
    const auto id = entry.id;
    entries_.insert(entries_.begin(), std::move(entry));
    return id;
}

bool InstrumentLibrary::remove(const juce::String& id) {
    for (size_t i = 0; i < entries_.size(); ++i) {
        if (entries_[i].id != id) continue;
        if (entries_[i].onDisk) fileFor(id).deleteFile();
        entries_.erase(entries_.begin() + static_cast<long>(i));
        return true;
    }
    return false;
}

bool InstrumentLibrary::rename(const juce::String& id, const juce::String& newName) {
    for (auto& e : entries_) {
        if (e.id != id) continue;
        e.name = uniqueName(newName, id);
        e.instrument.name = e.name.toStdString();
        if (e.onDisk) writeToDisk(e);
        return true;
    }
    return false;
}

bool InstrumentLibrary::updateInstrument(const juce::String& id, const ir::Instrument& instrument) {
    for (auto& e : entries_) {
        if (e.id != id) continue;
        const auto keptName = e.name;      // edits must not silently rename it
        e.instrument = instrument;
        e.instrument.name = keptName.toStdString();
        if (e.onDisk) writeToDisk(e);
        return true;
    }
    return false;
}

const LibraryEntry* InstrumentLibrary::find(const juce::String& id) const {
    for (const auto& e : entries_) if (e.id == id) return &e;
    return nullptr;
}

juce::var InstrumentLibrary::toState() const {
    juce::Array<juce::var> list;
    for (const auto& e : entries_) {
        if (e.onDisk) continue;   // the disk library reloads itself
        auto* obj = new juce::DynamicObject();
        obj->setProperty("id", e.id);
        obj->setProperty("name", e.name);
        obj->setProperty("prompt", e.prompt);
        obj->setProperty("created", static_cast<juce::int64>(e.created));
        obj->setProperty("ir", juce::String(ir::toJson(e.instrument).dump()));
        list.add(juce::var(obj));
    }
    return juce::var(list);
}

void InstrumentLibrary::fromState(const juce::var& state) {
    if (const auto* list = state.getArray()) {
        for (const auto& item : *list) {
            auto* obj = item.getDynamicObject();
            if (obj == nullptr) continue;

            ir::Instrument inst;
            ir::IrReport report;
            const auto irText = obj->getProperty("ir").toString();
            if (!ir::parse(irText.toStdString(), inst, report)) continue;

            LibraryEntry entry;
            entry.id      = obj->getProperty("id").toString();
            entry.name    = obj->getProperty("name").toString();
            entry.prompt  = obj->getProperty("prompt").toString();
            entry.created = static_cast<juce::int64>(obj->getProperty("created"));
            entry.onDisk  = false;
            entry.instrument = std::move(inst);
            if (entry.id.isEmpty()) entry.id = makeId(entry.instrument);
            if (find(entry.id) == nullptr) entries_.push_back(std::move(entry));
        }
    }
}

} // namespace forge
