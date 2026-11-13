#include "mapgen_context_weights.h"

#include "debug.h"
#include "mapgendata.h"
#include "overmap/omdata.h"
#include "overmap/overmap.h"

#include <algorithm>
#include <array>
#include <map>
#include <string>

namespace {

const std::map<std::string, mapgen_context_feature>& feature_keys() {
    static const std::map<std::string, mapgen_context_feature> keys = {
        {"north", mapgen_context_feature::north},
        {"east", mapgen_context_feature::east},
        {"south", mapgen_context_feature::south},
        {"west", mapgen_context_feature::west},
        {"north_east", mapgen_context_feature::north_east},
        {"south_east", mapgen_context_feature::south_east},
        {"south_west", mapgen_context_feature::south_west},
        {"north_west", mapgen_context_feature::north_west},
        {"any_neighbor", mapgen_context_feature::any_neighbor},
        {"all_neighbors", mapgen_context_feature::all_neighbors},
        {"above", mapgen_context_feature::above},
        {"below", mapgen_context_feature::below},
        {"z", mapgen_context_feature::z_level},
        {"terrain", mapgen_context_feature::terrain},
    };
    return keys;
}

/// @return The overmap terrain the given terrain feature observes from
/// @p dat.  Must not be called for non-terrain features.
auto oter_for_feature(const mapgen_context_feature feature, const mapgendata& dat)
    -> const oter_id& {
    switch (feature) {
        case mapgen_context_feature::north:
            return dat.north();
        case mapgen_context_feature::east:
            return dat.east();
        case mapgen_context_feature::south:
            return dat.south();
        case mapgen_context_feature::west:
            return dat.west();
        case mapgen_context_feature::north_east:
            return dat.neast();
        case mapgen_context_feature::south_east:
            return dat.seast();
        case mapgen_context_feature::south_west:
            return dat.swest();
        case mapgen_context_feature::north_west:
            return dat.nwest();
        case mapgen_context_feature::any_neighbor:
        case mapgen_context_feature::all_neighbors:
        case mapgen_context_feature::z_level:
            break;
        case mapgen_context_feature::above:
            return dat.above();
        case mapgen_context_feature::below:
            return dat.below();
        case mapgen_context_feature::terrain:
            return dat.terrain_type();
        case mapgen_context_feature::num:
            break;
    }
    debugmsg("mapgen context_weights: feature %d has no overmap terrain",
             static_cast<int>(feature));
    return dat.terrain_type();
}

auto condition_matches(const mapgen_weight_condition& cond, const mapgendata& dat) -> bool {
    if (cond.feature == mapgen_context_feature::z_level) {
        const int z = dat.zlevel();
        return z >= cond.z_min && z <= cond.z_max;
    }

    if (cond.feature == mapgen_context_feature::any_neighbor
        || cond.feature == mapgen_context_feature::all_neighbors) {
        const std::array<const oter_id*, 8> neighbors = {
            &dat.north(), &dat.east(),  &dat.south(), &dat.west(),
            &dat.neast(), &dat.seast(), &dat.swest(), &dat.nwest(),
        };
        const auto neighbor_matches = [&cond](const oter_id* const neighbor) {
            return std::ranges::any_of(cond.patterns, [&](const std::string& pattern) {
                return is_ot_match(pattern, *neighbor, cond.match_type);
            });
        };
        return cond.feature == mapgen_context_feature::all_neighbors
                 ? std::ranges::all_of(neighbors, neighbor_matches)
                 : std::ranges::any_of(neighbors, neighbor_matches);
    }

    return std::ranges::any_of(cond.patterns, [&](const std::string& pattern) {
        return is_ot_match(pattern, oter_for_feature(cond.feature, dat), cond.match_type);
    });
}

/// Parses a terrain pattern specification, which is either a single string, an
/// array of strings, or an object with a "match" member (string or array of
/// strings) and an optional "match_type" member.  The object form overwrites
/// the default @p match_type.
auto load_patterns(
    std::vector<std::string>& patterns, ot_match_type& match_type, const JsonValue& member)
    -> void {
    if (member.test_string()) {
        patterns.push_back(member.get_string());
        return;
    }
    if (member.test_array()) {
        for (const JsonValue& value : member.get_array()) {
            patterns.push_back(value.get_string());
        }
        return;
    }

    const JsonObject jo = member.get_object();
    if (jo.has_member("match")) {
        load_patterns(patterns, match_type, jo.get_member("match"));
    } else {
        jo.throw_error("terrain condition object requires a \"match\" member");
    }
    if (jo.has_member("match_type")) {
        match_type = jo.get_enum_value<ot_match_type>("match_type");
    }
}

/// Parses a z level condition value, which is either one of the "surface",
/// "underground", "above" constants or a specific z level number.
auto load_z_range(int& z_min, int& z_max, const JsonValue& member) -> void {
    if (member.test_string()) {
        const std::string kind = member.get_string();
        if (kind == "surface") {
            z_min = 0;
            z_max = 0;
            return;
        }
        if (kind == "underground") {
            z_min = INT_MIN;
            z_max = -1;
            return;
        }
        if (kind == "above") {
            z_min = 1;
            z_max = INT_MAX;
            return;
        }
        member.throw_error("\"z\" string must be one of \"surface\", \"underground\", \"above\"");
    }

    const int z = member.get_int();
    z_min = z;
    z_max = z;
}

auto load_condition(
    mapgen_weight_rule& rule, const std::string& name, const JsonValue& value,
    const JsonObject& ifs) -> void {
    const auto feature_it = feature_keys().find(name);
    if (feature_it == feature_keys().end()) { ifs.throw_error("unknown context condition", name); }

    auto cond = mapgen_weight_condition{};
    cond.feature = feature_it->second;
    if (cond.feature == mapgen_context_feature::z_level) {
        load_z_range(cond.z_min, cond.z_max, value);
    } else {
        load_patterns(cond.patterns, cond.match_type, value);
    }
    rule.conditions.push_back(std::move(cond));
}

auto load_rule(const JsonObject& jo) -> mapgen_weight_rule {
    auto rule = mapgen_weight_rule{};
    rule.weight = jo.get_int("weight");
    if (rule.weight < 0) { jo.throw_error("\"weight\" must be at least 0"); }

    if (jo.has_member("if")) {
        const JsonObject ifs = jo.get_object("if");
        for (const JsonMember& member : ifs) { load_condition(rule, member.name(), member, ifs); }
    }

    return rule;
}

} // namespace

void mapgen_context_weights::load(const JsonValue& member) {
    if (member.test_array()) {
        for (const JsonValue& value : member.get_array()) {
            rules.push_back(load_rule(value.get_object()));
        }
        return;
    }
    rules.push_back(load_rule(member.get_object()));
}

auto mapgen_context_weights::get_weight(const mapgendata& dat, int base_weight) const -> int {
    const auto rule_matches = [](const mapgen_weight_rule& rule, const mapgendata& rule_dat) {
        return std::ranges::all_of(rule.conditions, [&](const mapgen_weight_condition& cond) {
            return condition_matches(cond, rule_dat);
        });
    };

    for (const mapgen_weight_rule& rule : rules) {
        if (rule_matches(rule, dat)) { return rule.weight; }
    }
    return base_weight;
}

void mapgen_context_weights::check(const std::string& oter_name) const {
    for (const mapgen_weight_rule& rule : rules) {
        for (const mapgen_weight_condition& cond : rule.conditions) {
            if (cond.feature == mapgen_context_feature::z_level) { continue; }
            for (const std::string& pattern : cond.patterns) {
                const bool found =
                    std::ranges::any_of(overmap_terrains::get_all(), [&](const oter_t& oter) {
                        return is_ot_match(pattern, oter_id(oter.id), cond.match_type);
                    });
                if (!found) {
                    debugmsg("mapgen for %s: context_weights condition references unknown overmap "
                             "terrain \"%s\"",
                             oter_name.c_str(), pattern.c_str());
                }
            }
        }
    }
}
