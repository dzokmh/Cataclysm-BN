#pragma once

#include "enums.h"
#include "json.h"

#include <climits>
#include <string>
#include <vector>

class JsonValue;
class mapgendata;

/**
 * A piece of the @ref mapgendata generation context that a
 * @ref mapgen_weight_condition inspects.
 */
enum class mapgen_context_feature : int {
    north,
    east,
    south,
    west,
    north_east,
    south_east,
    south_west,
    north_west,
    /// Any of the eight neighboring overmap terrains matches.
    any_neighbor,
    /// All of the eight neighboring overmap terrains match.
    all_neighbors,
    /// The overmap terrain one z level above.
    above,
    /// The overmap terrain one z level below.
    below,
    /// The absolute z level of the generated overmap terrain.
    z_level,
    /// The generated terrain itself (@ref mapgendata::terrain_type).
    terrain,
    num
};

/**
 * A single condition of a @ref mapgen_weight_rule.
 *
 * Terrain conditions (every feature except z_level) hold when any of
 * @ref patterns matches, via @ref is_ot_match, the overmap terrain seen from
 * the feature's position in the generation context.  The z_level condition
 * holds when the z level of the generated terrain is within the inclusive
 * @ref z_min / @ref z_max range.
 */
struct mapgen_weight_condition {
    mapgen_context_feature feature = mapgen_context_feature::north;
    /// Terrain patterns this condition accepts (any of them).  Non-empty for
    /// every terrain feature; unused for the z_level feature.
    std::vector<std::string> patterns;
    /// How pattern matching behaves, see @ref is_ot_match.
    ot_match_type match_type = ot_match_type::type;
    /// Inclusive z level range, only used by the z_level feature.
    int z_min = INT_MIN;
    /// Inclusive z level range, only used by the z_level feature.
    int z_max = INT_MAX;
};

/**
 * A single "context_weights" rule: when all of its conditions hold for the
 * @ref mapgendata being generated, the mapgen function's weight becomes
 * @ref weight instead of its base weight.
 */
struct mapgen_weight_rule {
    /// Weight to use when this rule matches.  Non-negative, zero disables the
    /// mapgen in that context.
    int weight = 0;
    /// All of these conditions must hold for the rule to match.
    std::vector<mapgen_weight_condition> conditions;
};

/**
 * Context-dependent weight rules for a @ref mapgen_function, loaded from the
 * "context_weights" JSON member.  An instance with no rules falls back to the
 * base weight for every generation context.
 *
 * Rules are evaluated in the order in which they are defined and the first
 * matching rule wins, so rules can override previously matching ones by being
 * listed earlier.
 */
class mapgen_context_weights {
public:
    /// Parses a "context_weights" JSON value (@p member), which is either a
    /// single rule object or an array of rule objects.
    void load(const JsonValue& member);

    /**
     * Evaluates the rules against the context of the overmap terrain
     * generation described by @p dat.
     * @return The weight of the first rule whose conditions all hold, or
     * @p base_weight when no rule matches (or when no rules are defined).
     */
    auto get_weight(const mapgendata& dat, int base_weight) const -> int;

    /// Reports (via debugmsg) rules whose terrain patterns do not match any
    /// loaded overmap terrain.  @p oter_name is the mapgen's overmap terrain
    /// used to identify the offender in messages.
    void check(const std::string& oter_name) const;

    /// @return Whether no rules are defined (base weights always apply).
    auto empty() const -> bool { return rules.empty(); }

private:
    std::vector<mapgen_weight_rule> rules;
};
