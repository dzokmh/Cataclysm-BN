#include "calendar.h"
#include "catch/catch.hpp"
#include "coordinates.h"
#include "json.h"
#include "map/mapbuffer_registry.h"
#include "mapgen/mapgen_constructor.h"
#include "mapgen/mapgen_context_weights.h"
#include "mapgen/mapgendata.h"
#include "overmap/omdata.h"
#include "point.h"
#include "state_helpers.h"
#include "type_id.h"

#include <sstream>
#include <string>

/**
 * Helper to parse a "context_weights" JSON value from a raw string and load it
 * into a fresh set of weight rules.
 */
static auto weights_from_json(const std::string& json) -> mapgen_context_weights {
    std::istringstream stream(json);
    JsonIn jsin(stream);
    const JsonObject jo = jsin.get_object();
    auto weights = mapgen_context_weights{};
    weights.load(jo.get_member("context_weights"));
    return weights;
}

/**
 * The "context_weights" rules are evaluated against a @ref mapgendata; the
 * dummy constructor provides a generation context whose terrain and every
 * neighbor is "field" at z level 0.
 */
struct mapgendata_fixture {
    mapgen_constructor tm{MAPBUFFER_REGISTRY.get(mapbuffer_registry::primary_dimension_id())};
    mapgendata dat;

    mapgendata_fixture(): dat(tm, mapgendata::dummy_settings) {}

    auto set_all_neighbors(const oter_id& oter) -> void {
        for (oter_id& neighbor : dat.t_nesw) { neighbor = oter; }
    }
};

TEST_CASE("context_weights_none", "[mapgen][context_weights]") {
    clear_all_state();
    mapgendata_fixture fx{};

    mapgen_context_weights weights;
    CHECK(weights.empty());
    // Without any rules the base weight always applies.
    CHECK(weights.get_weight(fx.dat, 1000) == 1000);
}

TEST_CASE("context_weights_fallback", "[mapgen][context_weights]") {
    clear_all_state();
    mapgendata_fixture fx{};

    // Only the "forest" terrain matches, but the generated terrain is "field".
    const mapgen_context_weights weights = weights_from_json(
        R"({ "context_weights": [
            { "weight": 3000, "if": { "terrain": "forest" } }
        ] })");
    CHECK_FALSE(weights.empty());
    CHECK(weights.get_weight(fx.dat, 1000) == 1000);
}

TEST_CASE("context_weights_terrain", "[mapgen][context_weights]") {
    clear_all_state();
    mapgendata_fixture fx{};

    const mapgen_context_weights weights = weights_from_json(
        R"({ "context_weights": [
            { "weight": 3000, "if": { "terrain": "field" } }
        ] })");
    CHECK(weights.get_weight(fx.dat, 1000) == 3000);
}

TEST_CASE("context_weights_first_rule_wins", "[mapgen][context_weights]") {
    clear_all_state();
    mapgendata_fixture fx{};

    const mapgen_context_weights weights = weights_from_json(
        R"({ "context_weights": [
            { "weight": 10, "if": { "terrain": "field" } },
            { "weight": 20, "if": { "terrain": "field" } }
        ] })");
    CHECK(weights.get_weight(fx.dat, 1000) == 10);
}

TEST_CASE("context_weights_zero_weight", "[mapgen][context_weights]") {
    clear_all_state();
    mapgendata_fixture fx{};

    // A matching rule with weight 0 disables the mapgen in that context.
    const mapgen_context_weights weights = weights_from_json(
        R"({ "context_weights": [
            { "weight": 0, "if": { "terrain": "field" } }
        ] })");
    CHECK(weights.get_weight(fx.dat, 1000) == 0);
}

TEST_CASE("context_weights_directions", "[mapgen][context_weights]") {
    clear_all_state();
    mapgendata_fixture fx{};

    const oter_id forest("forest");
    fx.dat.t_nesw[0] = forest; // north

    const mapgen_context_weights north_rules = weights_from_json(
        R"({ "context_weights": [
            { "weight": 2000, "if": { "north": "forest" } },
            { "weight": 500, "if": { "north_east": "forest" } }
        ] })");
    CHECK(north_rules.get_weight(fx.dat, 1000) == 2000);

    // "forest_thick" is a separate overmap terrain, only reached via prefix
    // matching.
    fx.dat.t_nesw[0] = oter_id("forest_thick");
    const mapgen_context_weights prefix_rules = weights_from_json(
        R"({ "context_weights": [
            { "weight": 2000,
              "if": { "north": { "match": "forest", "match_type": "PREFIX" } } }
        ] })");
    CHECK(prefix_rules.get_weight(fx.dat, 1000) == 2000);

    // Without prefix matching "forest" does not match "forest_thick".
    const mapgen_context_weights exact_rules = weights_from_json(
        R"({ "context_weights": [
            { "weight": 2000, "if": { "north": "forest" } }
        ] })");
    CHECK(exact_rules.get_weight(fx.dat, 1000) == 1000);
}

TEST_CASE("context_weights_any_neighbor", "[mapgen][context_weights]") {
    clear_all_state();
    mapgendata_fixture fx{};

    const mapgen_context_weights weights = weights_from_json(
        R"({ "context_weights": [
            { "weight": 2000, "if": { "any_neighbor": [ "forest", "forest_thick" ] } }
        ] })");

    // Only one neighbor is a forest.
    fx.dat.t_nesw[0] = oter_id("forest");
    CHECK(weights.get_weight(fx.dat, 1000) == 2000);

    // No neighbors are forests and the terrain itself is not part of the
    // condition.
    fx.set_all_neighbors(oter_id("field"));
    CHECK(weights.get_weight(fx.dat, 1000) == 1000);
}

TEST_CASE("context_weights_all_neighbors", "[mapgen][context_weights]") {
    clear_all_state();
    mapgendata_fixture fx{};

    const mapgen_context_weights weights = weights_from_json(
        R"({ "context_weights": [
            { "weight": 2000, "if": { "all_neighbors": "forest" } }
        ] })");

    fx.dat.t_nesw[0] = oter_id("forest");
    CHECK(weights.get_weight(fx.dat, 1000) == 1000);

    fx.set_all_neighbors(oter_id("forest"));
    CHECK(weights.get_weight(fx.dat, 1000) == 2000);
}

TEST_CASE("context_weights_z_level", "[mapgen][context_weights]") {
    clear_all_state();
    mapgendata_fixture fx{};

    // The dummy mapgendata describes surface generation (z level 0).
    const mapgen_context_weights surface_rules = weights_from_json(
        R"({ "context_weights": [
            { "weight": 2000, "if": { "z": "surface" } }
        ] })");
    CHECK(surface_rules.get_weight(fx.dat, 1000) == 2000);

    const mapgen_context_weights zero_rules = weights_from_json(
        R"({ "context_weights": [
            { "weight": 2000, "if": { "z": 0 } }
        ] })");
    CHECK(zero_rules.get_weight(fx.dat, 1000) == 2000);

    const mapgen_context_weights one_rules = weights_from_json(
        R"({ "context_weights": [
            { "weight": 2000, "if": { "z": 1 } }
        ] })");
    CHECK(one_rules.get_weight(fx.dat, 1000) == 1000);

    const mapgen_context_weights under_rules = weights_from_json(
        R"({ "context_weights": [
            { "weight": 2000, "if": { "z": "underground" } }
        ] })");
    CHECK(under_rules.get_weight(fx.dat, 1000) == 1000);
}

TEST_CASE("context_weights_above_below", "[mapgen][context_weights]") {
    clear_all_state();
    mapgendata_fixture fx{};

    const mapgen_context_weights weights = weights_from_json(
        R"({ "context_weights": [
            { "weight": 2000, "if": { "above": "field", "below": "field" } }
        ] })");
    // The dummy mapgendata uses "field" for the terrain itself and both
    // vertical neighbors.
    CHECK(weights.get_weight(fx.dat, 1000) == 2000);
}
