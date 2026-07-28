// ---------------------------------------------------------------------------
// Salvaging a specification from an imperfect response.
//
// Every case here is a real shape a model response takes. The failure this
// guards against - "the model did not return a JSON object" - throws away a
// complete design over a formatting accident, so the bar is: recover anything
// recoverable, and never accept anything that does not parse.
// ---------------------------------------------------------------------------
#include "TestUtil.h"

#include "core/llm/JsonSalvage.h"

#include <nlohmann/json.hpp>

using namespace forge;
using namespace forge::llm;

namespace {

bool isObject(const std::string& s) {
    const auto j = nlohmann::json::parse(s, nullptr, false, true);
    return !j.is_discarded() && j.is_object();
}

} // namespace

TEST_CASE(salvage_passes_clean_json_through_untouched) {
    const auto r = salvageJsonObject(R"({"name":"Cloud Bow","values":{"f1_cutoff":800}})");
    CHECK(r.how == Salvage::Clean);
    CHECK(isObject(r.json));
    CHECK(nlohmann::json::parse(r.json)["name"] == "Cloud Bow");
}

TEST_CASE(salvage_tolerates_surrounding_whitespace) {
    const auto r = salvageJsonObject("\n\n   {\"name\":\"x\"}  \n");
    CHECK(r.how == Salvage::Clean);
    CHECK(isObject(r.json));
}

TEST_CASE(salvage_unwraps_markdown_fences) {
    const auto r = salvageJsonObject("```json\n{\"name\":\"Fenced\",\"polyphony\":8}\n```");
    CHECK(r.how == Salvage::Unwrapped);
    CHECK(nlohmann::json::parse(r.json)["name"] == "Fenced");
}

TEST_CASE(salvage_unwraps_a_preamble_and_a_postscript) {
    const auto r = salvageJsonObject(
        "Here is your instrument:\n\n{\"name\":\"Prosed\"}\n\nLet me know if you want it darker!");
    CHECK(r.how == Salvage::Unwrapped);
    CHECK(nlohmann::json::parse(r.json)["name"] == "Prosed");
}

TEST_CASE(salvage_prefers_the_largest_object_not_the_first) {
    // A decoy object in the prose used to win simply by appearing first.
    const auto r = salvageJsonObject(
        "I considered {\"idea\": \"bell\"} but went with this instead:\n"
        "{\"name\":\"Real\",\"values\":{\"a\":1,\"b\":2,\"c\":3},\"switches\":{\"w\":\"saw\"}}");
    CHECK(r);
    const auto j = nlohmann::json::parse(r.json);
    CHECK(j.contains("values"));
    CHECK(j["name"] == "Real");
}

TEST_CASE(salvage_repairs_a_response_cut_off_mid_value) {
    // The read timed out with the object half written.
    const auto r = salvageJsonObject(
        R"({"name":"Cut Off","voicing":"poly","values":{"f1_cutoff":2400,"f1_reso":0.3,"osc_a_lev)");
    CHECK(r.how == Salvage::Repaired);
    CHECK(isObject(r.json));

    const auto j = nlohmann::json::parse(r.json);
    CHECK(j["name"] == "Cut Off");
    CHECK(j["voicing"] == "poly");
    CHECK(j["values"]["f1_cutoff"] == 2400);      // everything before the break survives
    CHECK(j["values"]["f1_reso"] == 0.3);
}

TEST_CASE(salvage_repairs_a_break_after_a_key_with_no_value) {
    const auto r = salvageJsonObject(
        R"({"name":"Half","values":{"f1_cutoff":800,"f1_reso":)");
    CHECK(r.how == Salvage::Repaired);
    const auto j = nlohmann::json::parse(r.json);
    CHECK(j["values"]["f1_cutoff"] == 800);
    CHECK(!j["values"].contains("f1_reso"));      // never invents a value
}

TEST_CASE(salvage_repairs_a_break_inside_nested_arrays) {
    const auto r = salvageJsonObject(
        R"({"name":"Deep","layout":{"pages":[{"title":"CORE","panels":["OSC A","FILTER 1"]},)"
        R"({"title":"AIR","panels":["REVERB")");
    CHECK(r.how == Salvage::Repaired);
    const auto j = nlohmann::json::parse(r.json);
    CHECK(j["name"] == "Deep");
    CHECK(j["layout"]["pages"][0]["title"] == "CORE");
}

TEST_CASE(salvage_repairs_a_break_inside_a_string) {
    const auto r = salvageJsonObject(
        R"({"name":"Talky","description":"a warm dusty pad that sounds like the end of a fil)");
    CHECK(r.how == Salvage::Repaired);
    CHECK(isObject(r.json));
    CHECK(nlohmann::json::parse(r.json)["name"] == "Talky");
}

TEST_CASE(salvage_gives_up_honestly_on_pure_prose) {
    const auto r = salvageJsonObject(
        "I'd love to help, but could you tell me more about what kind of bass you want?");
    CHECK(!r);
    CHECK(r.how == Salvage::None);
    CHECK(!r.note.empty());
    CHECK(r.json.empty());
}

TEST_CASE(salvage_gives_up_on_empty_input) {
    for (const char* s : {"", "   ", "\n\n"}) {
        const auto r = salvageJsonObject(s);
        CHECK(!r);
        CHECK(r.json.empty());
    }
}

TEST_CASE(salvage_never_returns_something_that_does_not_parse) {
    // Deliberately awkward inputs. Whatever comes back must be a real object.
    const char* nasty[] = {
        "{",
        "{{{{",
        R"({"a":)",
        R"({"a":"\)",
        R"({"a":[[[)",
        "}{",
        R"({"a":1}}}}extra)",
        R"(no braces at all)",
        R"({"nested":{"deep":{"deeper":{"deepest":)",
        R"(prefix {"a":1} middle {"b":2} suffix)",
    };
    for (const char* input : nasty) {
        const auto r = salvageJsonObject(input);
        if (r) CHECK(isObject(r.json));
        else   CHECK(r.json.empty());
    }
}

TEST_CASE(salvage_ignores_braces_inside_strings) {
    const auto r = salvageJsonObject(
        R"({"description":"a } brace { in a string","name":"Tricky"})");
    CHECK(r.how == Salvage::Clean);
    CHECK(nlohmann::json::parse(r.json)["name"] == "Tricky");
}

TEST_CASE(salvage_explains_itself) {
    // The note is shown to the musician, so a repaired patch must not pretend
    // to be a clean one.
    const auto repaired = salvageJsonObject(R"({"name":"x","values":{"a":1,"b")");
    CHECK(repaired.how == Salvage::Repaired);
    CHECK(!repaired.note.empty());

    const auto clean = salvageJsonObject(R"({"name":"x"})");
    CHECK(clean.note.empty());
}
