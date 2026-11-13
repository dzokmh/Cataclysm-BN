#include "catch/catch.hpp"
#include "coordinates.h"
#include "overmap/overmap_geo.h"
#include "point.h"

#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

namespace {
/// Appends little-endian bytes of each value: @p width bytes per value.
auto put_bytes(std::string& data, const std::vector<std::uint64_t>& values, int width) -> void {
    for (std::uint64_t value : values) {
        for (int byte = 0; byte < width; byte++) {
            data.push_back(static_cast<char>((value >> (8 * byte)) & 0xFF));
        }
    }
}

/// Zigzag-maps a signed delta onto the unsigned domain and appends its
/// LEB128 base-128 varint.
auto put_varint(std::string& data, std::int32_t delta) -> void {
    const auto raw = static_cast<std::uint32_t>(delta);
    auto zig = static_cast<std::uint32_t>((raw << 1) ^ static_cast<std::uint32_t>(delta >> 31));
    while (zig > 0x7FU) {
        data.push_back(static_cast<char>((zig & 0x7FU) | 0x80U));
        zig >>= 7;
    }
    data.push_back(static_cast<char>(zig));
}

/// Appends a polyline as zigzag-varint deltas; @p offset is the encoded
/// anchor counterpart (0, 0) for anchor-relative polylines.
auto put_varint_points(
    std::string& data, const std::vector<std::pair<std::int32_t, std::int32_t>>& points) -> void {
    std::int32_t prev_x = 0;
    std::int32_t prev_y = 0;
    for (const auto& [x, y] : points) {
        put_varint(data, x - prev_x);
        put_varint(data, y - prev_y);
        prev_x = x;
        prev_y = y;
    }
}

/// Builds a minimal valid 2x2-cell geo packet binary: zone table of 2 classes,
/// mask "0 1 1 0", one 3-vertex river polyline plus optional road entries.
auto packet_bytes(
    std::int32_t anchor_x = 100, std::int32_t anchor_y = -40, int span = 2,
    const std::vector<std::pair<std::uint8_t, std::vector<std::pair<std::int32_t, std::int32_t>>>>&
        roads = {},
    int river_width = 2) -> std::string {
    std::string data;
    put_bytes(data, {'B', 'N', 'C', 'G'}, 1); // magic "BNCG" little-endian
    put_bytes(data, {3}, 2);                  // version
    put_bytes(data, {static_cast<std::uint64_t>(anchor_x)}, 4);
    put_bytes(data, {static_cast<std::uint64_t>(anchor_y)}, 4);
    put_bytes(data, {static_cast<std::uint64_t>(span)}, 2);
    put_bytes(data, {2}, 4); // cells_x
    put_bytes(data, {2}, 4); // cells_y
    put_bytes(data, {2}, 2); // zone table size
    put_bytes(data, {1}, 2); // river count
    // RLE mask: one cell of zone 0, then two of zone 1, then one of zone 0.
    data.push_back(static_cast<char>(1));
    data.push_back(static_cast<char>(0));
    data.push_back(static_cast<char>(2));
    data.push_back(static_cast<char>(1));
    data.push_back(static_cast<char>(1));
    data.push_back(static_cast<char>(0));
    // River: width byte, 3 vertices at (0,0), (4,0), (4,4) relative to the
    // anchor, encoded as zigzag-varint deltas.
    put_bytes(data, {3}, 2);
    data.push_back(static_cast<char>(river_width));
    put_varint_points(data, {{0, 0}, {4, 0}, {4, 4}});
    // Roads: tier byte, varint vertex count, zigzag-varint vertex deltas.
    put_bytes(data, {roads.size()}, 4);
    for (const auto& [tier, points] : roads) {
        data.push_back(static_cast<char>(tier));
        put_varint(data, static_cast<std::int32_t>(points.size()));
        put_varint_points(data, points);
    }
    return data;
}
} // namespace

TEST_CASE("geo_packet_parse", "[overmap_geo]") {
    const auto bytes = packet_bytes();
    std::istringstream in(bytes);
    const auto packet = overmap_geo::parse_binary(in, "test.cmap");

    SECTION("anchor and bounds") {
        const geo_bounds b = packet.bounds();
        CHECK(b.min.x() == 100);
        CHECK(b.min.y() == -40);
        // 2x2 cells of span 2 cover 4x4 OMTs: max is min + span - 1.
        CHECK(b.max.x() == 103);
        CHECK(b.max.y() == -37);
    }

    SECTION("zone mask decoded row-major") {
        // North-west cell: zone 0. North-east: zone 1 (run of 2). South-west:
        // zone 1 (run continues). South-east: zone 0.
        CHECK(packet.zone_at(point_abs_omt(100, -40)) == 0);
        CHECK(packet.zone_at(point_abs_omt(102, -40)) == 1);
        CHECK(packet.zone_at(point_abs_omt(100, -38)) == 1);
        CHECK(packet.zone_at(point_abs_omt(102, -38)) == 0);
    }

    SECTION("outside coverage is rejected") {
        CHECK_FALSE(packet.covers(point_abs_omt(99, -40)));
        CHECK_FALSE(packet.zone_at(point_abs_omt(104, -40)).has_value());
        CHECK_FALSE(packet.covers(point_abs_omt(100, -41)));
    }

    SECTION("no roads declared") { CHECK(packet.roads().empty()); }
}

TEST_CASE("geo_packet_river_decoded", "[overmap_geo]") {
    std::istringstream in(packet_bytes());
    const auto packet = overmap_geo::parse_binary(in, "test.cmap");

    const std::vector<geo_river>& rivers = packet.rivers();
    REQUIRE(rivers.size() == 1);
    REQUIRE(rivers[0].points.size() == 3);
    CHECK(rivers[0].width == 2);
    CHECK(rivers[0].points[0] == point_abs_omt(100, -40));
    CHECK(rivers[0].points[1] == point_abs_omt(104, -40));
    CHECK(rivers[0].points[2] == point_abs_omt(104, -36));
}

TEST_CASE("geo_packet_river_width_fallback", "[overmap_geo]") {
    // Width byte 0 means "use the packet's river rule width" for painters.
    std::istringstream in(packet_bytes(100, -40, 2, {}, 0));
    const auto packet = overmap_geo::parse_binary(in, "test.cmap");
    CHECK(packet.rivers().size() == 1);
    CHECK(packet.rivers()[0].width == 0);
}

TEST_CASE("geo_packet_road_decoded", "[overmap_geo]") {
    // One tier-0 road with 3 anchor-relative vertices, first negative.
    const auto bytes =
        packet_bytes(100, -40, 2, {{0, {{-7728, 5536}, {-7724, 5536}, {-7724, 5540}}}});
    std::istringstream in(bytes);
    const auto packet = overmap_geo::parse_binary(in, "test.cmap");

    const std::vector<geo_road>& roads = packet.roads();
    REQUIRE(roads.size() == 1);
    CHECK(roads[0].tier == geo_road_tier::highway);
    REQUIRE(roads[0].points.size() == 3);
    CHECK(roads[0].points[0] == point_abs_omt(100 - 7728, -40 + 5536));
    CHECK(roads[0].points[2] == point_abs_omt(100 - 7724, -40 + 5540));
}

TEST_CASE("geo_packet_bad_road_tier", "[overmap_geo]") {
    const auto bytes = packet_bytes(100, -40, 2, {{5, {{0, 0}, {4, 0}}}});
    std::istringstream in(bytes);
    CHECK_THROWS(overmap_geo::parse_binary(in, "test.cmap"));
}

TEST_CASE("geo_packet_bad_magic", "[overmap_geo]") {
    std::string data = packet_bytes();
    data.front() = 'X';
    std::istringstream in(data);
    CHECK_THROWS(overmap_geo::parse_binary(in, "test.cmap"));
}

TEST_CASE("geo_packet_truncated_mask", "[overmap_geo]") {
    std::string data = packet_bytes();
    data.resize(data.size() - 2); // drop the last mask run pair
    std::istringstream in(data);
    CHECK_THROWS(overmap_geo::parse_binary(in, "test.cmap"));
}

TEST_CASE("geo_packet_mask_overflow", "[overmap_geo]") {
    // Declared 4 cells but runs sum to 5: the third run's count byte is at
    // byte 32 (28 header bytes + 4 mask bytes into the stream).
    std::string data = packet_bytes();
    data[32] = static_cast<char>(2); // last run: 1 -> 2 cells
    std::istringstream in(data);
    CHECK_THROWS(overmap_geo::parse_binary(in, "test.cmap"));
}

TEST_CASE("geo_packet_unknown_zone_rejected", "[overmap_geo]") {
    std::string data = packet_bytes();
    // Zone table holds 2 classes; a mask cell references class 5.
    data[33] = static_cast<char>(5);
    std::istringstream in(data);
    CHECK_THROWS(overmap_geo::parse_binary(in, "test.cmap"));
}
