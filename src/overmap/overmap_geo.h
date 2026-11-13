#pragma once

#include "coordinates.h"
#include "overmap/omdata.h"
#include "point.h"
#include "type_id.h"

#include <cstdint>
#include <iosfwd>
#include <optional>
#include <string>
#include <vector>

class JsonObject;

/// Index of a land-cover zone class inside a geo packet zone table.
using geo_zone_index = std::uint8_t;

/// How a zone class is translated into overmap generation.
struct geo_zone_rule {
    /// Base overmap terrain painted on every overmap terrain cell of this
    /// zone.  When not set, the zone leaves the default terrain untouched,
    /// letting the regular procedural passes provide micro texture.
    std::optional<oter_str_id> paint;

    auto painted() const -> bool { return paint.has_value(); }
};

/// How a river referenced by a geo packet is painted.
struct geo_river_rule {
    oter_str_id paint;
    /// Fallback river width in overmap terrain cells (at least 1).  Used
    /// only for rivers whose packet entry carries no width of its own.
    int width = 1;
};

/// A river polyline referenced by a geo packet.
struct geo_river {
    /// Width in overmap terrain cells harvested from the source data
    /// (OSM `width` tag); 0 falls back to the packet's river rule width.
    int width = 0;
    /// Polyline vertices in absolute OMT coordinates.
    std::vector<point_abs_omt> points;
};

/// Road importance class of a real-world road from OSM highway data.
enum class geo_road_tier : std::uint8_t {
    /// Motorways and trunks.
    highway = 0,
    /// Primary and secondary routes.
    main = 1,
    /// Tertiary connectors between towns.
    local = 2,
};

/// A real-world road (an OSM highway way) referenced by a geo packet.
struct geo_road {
    /// Importance class, reserved for width and visual upgrades.
    geo_road_tier tier = geo_road_tier::main;
    /// Polyline vertices in absolute OMT coordinates.
    std::vector<point_abs_omt> points;
};

/// A real-world city from a geo packet, placed procedurally on the geography.
struct geo_city {
    /// Real place name (shown as the city label); empty when unknown.
    std::string name;
    /// Absolute OMT of the city center.
    point_abs_omt pos;
    /// Vanilla city radius in OMT cells (like CITY_SIZE rolls, 1..16).
    int size = 1;
    /// Whether special finale placement (vault etc.) is attempted here.
    bool finale = false;
};

/// Bounds of a geo packet in absolute overmap terrain coordinates.
struct geo_bounds {
    /// Absolute OMT of the first covered cell.
    point_abs_omt min;
    /// Absolute OMT of the last covered cell (inclusive).
    point_abs_omt max;
};

/// A georeferenced "geo packet": a coarse real-earth land-cover mask plus
/// river polylines, anchored in absolute overmap terrain coordinates.
///
/// The mask is loaded from a compact binary sidecar file (versioned "BNCG"
/// format) next to the JSON definition.  Zone classes are translated into
/// generation rules by the "zones" members of the JSON definition.
class overmap_geo {
public:
    /// Parses one "overmap_geo" JSON definition and registers it.
    static void load(const JsonObject& jo);
    /// Drops all registered packets (static data reset).
    static void reset();
    /// Validates paint terrain references of all registered packets.
    static void check_consistency();

    /// All packets in definition order.
    static auto all() -> const std::vector<overmap_geo>&; /// Parses the binary mask sidecar from a
                                                          /// stream.  Exposed for tests.
    static auto parse_binary(std::istream& in, const std::string& label) -> overmap_geo;

    /// Zone class at an absolute position, or nullopt when out of bounds.
    auto zone_at(const point_abs_omt& p) const -> std::optional<geo_zone_index>;
    /// Generation rule of a zone class, or nullptr for unknown classes.
    auto rule_for(geo_zone_index zone) const -> const geo_zone_rule*;

    /// Absolute coverage rectangle of this packet.
    auto bounds() const -> geo_bounds;
    /// True when the position is covered by this packet.
    auto covers(const point_abs_omt& p) const -> bool;

    /// Rivers referenced by this packet, in absolute OMT coordinates.
    auto rivers() const -> const std::vector<geo_river>&;
    /// How rivers are painted, or nullptr when the packet has no rivers.
    auto river_rule() const -> const geo_river_rule*;

    /// Real-world cities placed by this packet (empty when unknown).
    auto cities() const -> const std::vector<geo_city>& { return cities_; }
    /// Real-world roads (OSM highway ways) referenced by this packet.
    auto roads() const -> const std::vector<geo_road>& { return roads_; }

    auto id() const -> const std::string& { return id_; }

private:
    /// Mutable registry backing @ref all().
    static auto registry() -> std::vector<overmap_geo>&;

    std::string id_;
    /// Absolute OMT of the first (north-west) mask macro cell.
    point_abs_omt anchor_;
    /// Overmap terrain cells spanned by one macro cell side.
    int cell_span_ = 0;
    /// Mask dimensions in macro cells.
    point mask_cells_;
    /// Zone class index per macro cell, row-major.
    std::vector<geo_zone_index> mask_;
    /// Generation rules indexed by zone class.
    std::vector<geo_zone_rule> zones_;
    /// River polylines in absolute OMT coordinates.
    std::vector<geo_river> rivers_;
    std::optional<geo_river_rule> river_rule_;
    /// Real-world cities, absolute OMT coordinates.
    std::vector<geo_city> cities_;
    /// Real-world roads, absolute OMT polyline vertices.
    std::vector<geo_road> roads_;
};
