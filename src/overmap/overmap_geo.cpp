#include "overmap/overmap_geo.h"

#include "debug.h"
#include "json.h"
#include "json_source_location.h"
#include "path_info.h"
#include "string_id.h"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <fstream>
#include <istream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace {
// Magic signature and format version of the ".cmap" binary sidecar.
constexpr std::uint32_t GEO_PACKET_MAGIC = 'B' | ('N' << 8) | ('C' << 16) | ('G' << 24);
// Magic signature bytes "BNCG" (little-endian layout of the packet header).
// Version 3: per-river widths and zigzag-varint vertex encoding.
constexpr std::uint16_t GEO_PACKET_VERSION = 3;

/// Reads 1 unsigned byte, failing with a label-prefixed error.
auto read_u8(std::istream& in, const std::string& label) -> int {
    const auto value = in.get();
    if (value == std::char_traits<char>::eof()) {
        throw std::runtime_error(label + ": unexpected end of binary data");
    }
    return value;
}

/// Reads a little-endian unsigned 16-bit integer.
auto read_le16(std::istream& in, const std::string& label) -> std::uint16_t {
    const auto lo = read_u8(in, label);
    const auto hi = read_u8(in, label);
    return static_cast<std::uint16_t>(lo | (hi << 8));
}

/// Reads a little-endian signed 32-bit integer.
auto read_le32(std::istream& in, const std::string& label) -> std::int32_t {
    const auto b3 = read_u8(in, label);
    const auto b2 = read_u8(in, label);
    const auto b1 = read_u8(in, label);
    const auto b0 = read_u8(in, label);
    return static_cast<std::int32_t>(
        static_cast<std::uint32_t>(b3) | (static_cast<std::uint32_t>(b2) << 8)
        | (static_cast<std::uint32_t>(b1) << 16) | (static_cast<std::uint32_t>(b0) << 24));
}

/// Reverses the varint zigzag mapping back onto the signed domain: 0, -1,
/// 1, -2, 2 ... were written as 0, 1, 2, 3, 4 ..., so small signed deltas
/// stay small unsigned.  The writer side lives in
/// tools/geo/make_geo_packet.py.
auto unzigzag32(std::uint32_t zig) -> std::int32_t {
    const auto value =
        static_cast<std::uint32_t>(zig >> 1) ^ static_cast<std::uint32_t>(0 - (zig & 1)); // *NOPAD*
    return static_cast<std::int32_t>(value);
}

/// Reads a little-endian LEB128 base-128 varint, failing with a
/// label-prefixed error.
auto read_uvarint(std::istream& in, const std::string& label) -> std::uint64_t {
    std::uint64_t value = 0;
    int shift = 0;
    while (true) {
        if (shift > 57) { throw std::runtime_error(label + ": overlong varint"); }
        const int byte = read_u8(in, label);
        value |= static_cast<std::uint64_t>(byte & 0x7F) << shift;
        if (byte <= 0x7F) { return value; }
        shift += 7;
    }
}

/// Reads one polyline of zigzag-varint deltas; the first delta is relative to
/// @p from (the anchor), later ones to the previous vertex, so long smooth
/// polylines stay compact on disk.
auto read_varint_polyline(
    std::istream& in, const std::string& label, std::uint32_t vertices, const point_abs_omt& from)
    -> std::vector<point_abs_omt> {
    auto polyline = std::vector<point_abs_omt>{};
    polyline.reserve(vertices);
    point_abs_omt last = from;
    for (std::uint32_t vertex = 0; vertex < vertices; ++vertex) {
        const auto dx = unzigzag32(static_cast<std::uint32_t>(read_uvarint(in, label))); // *NOPAD*
        const auto dy = unzigzag32(static_cast<std::uint32_t>(read_uvarint(in, label))); // *NOPAD*
        last = point_abs_omt(last.x() + dx, last.y() + dy);
        polyline.push_back(last);
    }
    return polyline;
}

/// Directory part of a path, without the trailing separator.
auto parent_directory(const std::string& path) -> std::string {
    const auto pos = path.rfind('/');
    if (pos == std::string::npos) { return "."; }
    return path.substr(0, pos);
}

/// Resolves the binary sidecar path relative to the source JSON file.
auto resolve_packet_file(const JsonObject& jo, const std::string& file) -> std::string {
    if (file.empty()) {
        jo.throw_error(R"(overmap_geo requires a "file" member pointing to its binary mask)");
    }
    if (file.front() == '/') { return file; }
    const json_source_location loc = jo.get_source_location();
    if (!loc.path || loc.path->empty()) {
        // In-memory definitions (tests): resolve against the game data dir.
        return PATH_INFO::datadir() + "/" + file;
    }
    return parent_directory(*loc.path) + "/" + file;
}

/// Parses a zone class index from a "zones" member key like "0".."255".
auto parse_zone_index(const std::string& key, const JsonValue& error_at) -> std::size_t {
    uint8_t value = 0;
    const auto key_view = std::string_view(key);
    const auto result = std::from_chars(key_view.begin(), key_view.end(), value, 10);
    if (result.ec != std::errc() || result.ptr != key_view.end()) {
        error_at.throw_error("overmap_geo zone keys must be decimal indexes 0..255");
    }
    return static_cast<std::size_t>(value);
}

/// Parses the zone rules member "zones" of an "overmap_geo" definition.
auto parse_zone_rules(const JsonObject& jo, std::vector<geo_zone_rule>& zones) -> void {
    if (!jo.has_object("zones")) { return; }
    for (const JsonMember member : jo.get_object("zones")) {
        if (member.is_comment()) { continue; }
        const auto index = parse_zone_index(member.name(), member);
        if (index >= zones.size()) { zones.resize(index + 1); }
        const JsonObject rule = member;
        if (rule.has_string("paint")) {
            zones[index].paint = oter_str_id(rule.get_string("paint"));
        }
    }
}

/// Parses the optional member "river" describing river painting.
auto parse_river_rule(const JsonObject& jo) -> std::optional<geo_river_rule> {
    if (!jo.has_object("river")) { return std::nullopt; }
    const JsonObject rule = jo.get_object("river");
    auto river = geo_river_rule{};
    river.paint = oter_str_id(rule.get_string("paint"));
    river.width = rule.get_int("width", 1);
    if (river.width < 1) { rule.throw_error("river width must be at least 1 OMT cell"); }
    return river;
}

/// Parses the optional member "cities": real-world city placements relative
/// to the packet anchor, mirroring the river vertex coordinate space.
auto parse_cities(const JsonObject& jo, point_abs_omt anchor, std::vector<geo_city>& cities)
    -> void {
    if (!jo.has_array("cities")) { return; }
    for (const JsonObject entry : jo.get_array("cities")) {
        auto city = geo_city{};
        city.name = entry.get_string("name", "");
        city.pos = anchor + point(entry.get_int("x"), entry.get_int("y"));
        city.size = std::max(1, entry.get_int("size", 1));
        city.finale = entry.get_bool("finale", false);
        cities.push_back(city);
    }
}
} // namespace

auto overmap_geo::parse_binary(std::istream& in, const std::string& label) -> overmap_geo {
    auto packet = overmap_geo{};
    const auto magic = read_le32(in, label);
    if (magic != GEO_PACKET_MAGIC) {
        std::ostringstream hex;
        hex << std::hex << std::uppercase << magic;
        throw std::runtime_error(
            label + ": not a geo packet binary (bad magic 0x" + hex.str() + ")");
    }
    const auto version = read_le16(in, label);
    if (version != GEO_PACKET_VERSION) {
        throw std::runtime_error(
            label + ": unsupported geo packet version " + std::to_string(version));
    }
    const auto anchor_x = read_le32(in, label);
    const auto anchor_y = read_le32(in, label);
    packet.anchor_ = point_abs_omt(anchor_x, anchor_y);
    packet.cell_span_ = read_le16(in, label);
    if (packet.cell_span_ < 1) {
        throw std::runtime_error(label + ": cell span must be at least 1 OMT");
    }
    const auto cells_x = read_le32(in, label);
    const auto cells_y = read_le32(in, label);
    if (cells_x == 0 || cells_y == 0) {
        throw std::runtime_error(label + ": mask must not be empty");
    }
    packet.mask_cells_ = point(static_cast<int>(cells_x), static_cast<int>(cells_y));
    const auto zone_count = read_le16(in, label);
    if (zone_count == 0) { throw std::runtime_error(label + ": zone table must not be empty"); }
    packet.zones_.resize(zone_count);
    const auto river_count = read_le16(in, label);

    // Run-length encoded mask, row-major from the north-west corner.
    const auto cell_count = static_cast<std::uint64_t>(cells_x) * cells_y;
    auto& mask = packet.mask_;
    mask.reserve(cell_count);
    while (mask.size() < cell_count) {
        const auto run = read_u8(in, label);
        if (run == 0) { throw std::runtime_error(label + ": zero-length mask run"); }
        const auto zone = read_u8(in, label);
        if (zone >= zone_count) {
            throw std::runtime_error(
                label + ": mask references undefined zone " + std::to_string(zone));
        }
        mask.insert(mask.end(), run, static_cast<geo_zone_index>(zone));
    }
    if (mask.size() != cell_count) {
        throw std::runtime_error(label + ": mask run overflows declared size");
    }

    // River polylines (version 3: per-river width byte and zigzag-varint
    // vertex deltas), vertices relative to the anchor in OMT units.
    for (std::uint16_t river = 0; river < river_count; ++river) {
        const auto vertices = read_le16(in, label);
        if (vertices < 2) {
            throw std::runtime_error(label + ": river polyline needs 2+ vertices");
        }
        const auto width = read_u8(in, label);
        auto polyline =
            read_varint_polyline(in, label, vertices, point_abs_omt(anchor_x, anchor_y));
        packet.rivers_.push_back(
            geo_river{.width = static_cast<int>(width), .points = std::move(polyline)});
    }

    // Road polylines (version 2+), tiered, with zigzag-varint vertex deltas
    // (version 3) and vertices relative to the anchor in OMT units.
    if (version >= 2) {
        const auto road_count = read_le32(in, label);
        packet.roads_.reserve(road_count);
        for (std::uint32_t road = 0; road < road_count; ++road) {
            const auto tier = read_u8(in, label);
            if (tier >= 3) { throw std::runtime_error(label + ": road tier must be 0..2"); }
            const auto vertices = unzigzag32(
                static_cast<std::uint32_t>(read_uvarint(in, label))); // *NOPAD*
            if (vertices < 0 || static_cast<std::uint32_t>(vertices) < 2) {
                throw std::runtime_error(label + ": road polyline needs 2+ vertices");
            }
            auto points = read_varint_polyline(
                in, label, static_cast<std::uint32_t>(vertices), point_abs_omt(anchor_x, anchor_y));
            packet.roads_.push_back(
                geo_road{.tier = static_cast<geo_road_tier>(tier), .points = std::move(points)});
        }
    }
    if (in.peek() != std::char_traits<char>::eof()) {
        throw std::runtime_error(label + ": trailing bytes after the last section");
    }
    return packet;
}

void overmap_geo::load(const JsonObject& jo) {
    const std::string id = jo.get_string("id");
    const std::string file = resolve_packet_file(jo, jo.get_string("file"));
    std::ifstream in(file, std::ios::binary);
    if (!in.good()) {
        jo.throw_error("overmap_geo '" + id + "': can't open binary mask '" + file + "'");
    }
    try {
        auto packet = parse_binary(in, file);
        packet.id_ = id;
        parse_zone_rules(jo, packet.zones_);
        packet.river_rule_ = parse_river_rule(jo);
        parse_cities(jo, packet.anchor_, packet.cities_);
        registry().push_back(std::move(packet));
    } catch (const std::runtime_error& err) {
        jo.throw_error("overmap_geo '" + id + "': " + err.what());
    }
}

auto overmap_geo::all() -> const std::vector<overmap_geo>& { return registry(); }

auto overmap_geo::registry() -> std::vector<overmap_geo>& {
    static std::vector<overmap_geo> packets;
    return packets;
}

void overmap_geo::reset() { registry().clear(); }

void overmap_geo::check_consistency() {
    for (const overmap_geo& packet : all()) {
        for (const geo_zone_rule& rule : packet.zones_) {
            if (rule.painted() && !rule.paint->is_valid()) {
                debugmsg("overmap_geo '%s': zone paints unknown overmap terrain '%s'",
                         packet.id_.c_str(), rule.paint->str().c_str());
            }
        }
        if (packet.river_rule_ && !packet.river_rule_->paint.is_valid()) {
            debugmsg("overmap_geo '%s': river paints unknown overmap terrain '%s'",
                     packet.id_.c_str(), packet.river_rule_->paint.str().c_str());
        }
        if (packet.river_rule_ && packet.river_rule_->width < 2) {
            debugmsg(
                "overmap_geo '%s': river width %d is narrower than the vanilla river "
                "polishing pass supports; rivers 1 OMT cell wide are converted to "
                "swamp. Use width >= 3.",
                packet.id_.c_str(), packet.river_rule_->width);
        }
        for (const geo_city& city : packet.cities_) {
            if (!packet.covers(city.pos)) {
                debugmsg("overmap_geo '%s': city '%s' outside the packet coverage",
                         packet.id_.c_str(), city.name.c_str());
            }
        }
    }
}

auto overmap_geo::bounds() const -> geo_bounds {
    const auto extent = point(mask_cells_.x * cell_span_, mask_cells_.y * cell_span_);
    return geo_bounds{.min = anchor_, .max = anchor_ + extent - point(1, 1)};
}

auto overmap_geo::covers(const point_abs_omt& p) const -> bool { return zone_at(p).has_value(); }

auto overmap_geo::zone_at(const point_abs_omt& p) const -> std::optional<geo_zone_index> {
    const auto rel = p - anchor_;
    const auto extent_x = mask_cells_.x * cell_span_;
    const auto extent_y = mask_cells_.y * cell_span_;
    if (rel.x() < 0 || rel.y() < 0 || rel.x() >= extent_x || rel.y() >= extent_y) {
        return std::nullopt;
    }
    const auto cell_x = rel.x() / cell_span_;
    const auto cell_y = rel.y() / cell_span_;
    return mask_[static_cast<std::size_t>(cell_y) * mask_cells_.x + cell_x];
}

auto overmap_geo::rule_for(geo_zone_index zone) const -> const geo_zone_rule* {
    if (zone >= zones_.size()) { return nullptr; }
    return &zones_[zone];
}

auto overmap_geo::rivers() const -> const std::vector<geo_river>& { return rivers_; }

auto overmap_geo::river_rule() const -> const geo_river_rule* {
    if (!river_rule_) { return nullptr; }
    return &*river_rule_;
}
