# Real-Earth Overmap Packets

An `overmap_geo` definition replaces the procedural noise pipeline with a
real-world region for the world map: actual land cover, real rivers and real OSM
highways are painted onto the overmap, and cities are placed at their real
coordinates. The data lives in one binary sidecar file (`.cmap`) next to the
JSON definition.

Real-earth generation is gated behind the `GEOGRAPHY_MODE` world option: the
packets are only used when it is set to `real_earth`. The default `procedural`
mode keeps the classic noise generator untouched.

The easiest way to produce a region is the converter script in `tools/geo` (see
[Converter](#converter)), which downloads ESA WorldCover land cover and
OpenStreetMap rivers, roads and cities for a bounding box.

## JSON Definition

| Identifier | Description |
| --- | --- |
| id | (_required_) Unique ID of the packet. |
| file | (_required_) Path of the `.cmap` binary sidecar, relative to this JSON file. |
| zones | (_optional_) Generation rules per zone class index (`"0"`..`"255"`, decimal keys). |
| river | (_optional_) How the river polylines of the packet are painted. Absent means "no rivers". |
| cities | (_optional_) Real-world city placements. Coordinates are relative to the packet anchor, same space as river vertices. |

```json
[
  {
    "type": "overmap_geo",
    "id": "geo_new_england",
    "file": "geo_new_england.cmap",
    "zones": {
      "1": { "paint": "forest" },
      "2": { "paint": "lake_surface" },
      "3": { "paint": "forest_water" }
    },
    "river": { "paint": "river_center", "width": 3 },
    "cities": [
      { "name": "Boston", "x": 5964, "y": 2497, "size": 16, "finale": true }
    ]
  }
]
```

### zones

Each member is a zone class index from the binary mask. Currently one member is
understood:

| Identifier | Description |
| --- | --- |
| paint | (_optional_) Base overmap terrain painted on every covered cell of the zone. When omitted, the zone leaves the default terrain untouched, and the regular procedural passes keep providing micro texture. |

Zone classes without a rule are left unpainted as well.

### river

| Identifier | Description |
| --- | --- |
| paint | (_required_) Overmap terrain painted along the river polylines. |
| width | (_optional_) Default river width in OMT cells. Defaults to 1, minimum 1. Used only for rivers that carry no width of their own; the converter harvests per-river widths from the OSM `width` tag and stores them in the binary packet. |

### cities

An array of city seeds:

| Identifier | Description |
| --- | --- |
| name | (_optional_) Real place name shown as the city label. |
| x, y | (_required_) City center, in OMT cells relative to the packet anchor (north-west corner of the mask). |
| size | (_optional_) Vanilla city radius in OMT cells, like `CITY_SIZE` rolls, 1..16. Defaults to 1. |
| finale | (_optional_) Whether special finale placement (vault etc.) is attempted here. Defaults to false. |

Cities are placed with the regular procedural city machinery at their real
coordinates. When a packet provides city guidance, the vanilla
`place_isolated_cities` pass is skipped so extra procedural cities do not appear
on top of real geography.

## Binary Packet Format (`.cmap`)

The sidecar is a little-endian stream parsed by `src/overmap_geo.cpp`
(`overmap_geo::parse_binary`). The current version is 3; older versions are
rejected at load, and a region has to be regenerated with the converter.  The
converter also thins long source polylines with Douglas-Peucker (0.5 OMT
tolerance, i.e. 12 m) and stores vertex deltas as zigzag varints, keeping a
New-England-sized region around 1 MB.

1. `u32` magic `"BNCG"`
2. `u16` format version (3)
3. `i32` anchor X, anchor Y — absolute OMT coordinate of the first
   (north-west) mask cell; every vertex coordinate in the file is given as a
   delta from this anchor
4. `u16` cell span — OMT cells spanned by one mask macro cell side
   (the converter uses 8, i.e. 8×8 OMT of 24 m each = 192 m per macro cell)
5. `u32` mask cells X, Y — mask dimensions in macro cells
6. `u16` zone count — size of the zone class table
7. `u16` river count
8. Run-length encoded mask, row-major from the north-west corner: repeated
   `[u8 run, u8 zone index]` byte pairs until `cells_x * cells_y` cells are
   filled; zero-length runs and undefined zone indexes are rejected
9. River polylines, one after another: `u16` vertex count (at least 2), a
   `u8` river width in OMT cells (0 = use the JSON `river` rule width as a
   fallback), then per vertex a zigzag-varint X delta and zigzag-varint Y
   delta; the first delta is relative to the anchor, later ones to the
   previous vertex
10. Road polylines: `u32` road count, then per road a `u8`
    road tier (`0..2`, values above 2 are rejected), a zigzag-varint vertex
    count (at least 2) and per vertex a zigzag-varint X delta and
    zigzag-varint Y delta, first relative to the anchor then to the previous
    vertex

Trailing bytes after the last section are rejected.  Zigzag varints encode
signed deltas as non-negative LEB128 base-128 numbers (0, -1, 1, -2, 2 ...
become 0, 1, 2, 3, 4 ...), so smooth polylines cost a byte or two per
coordinate.

Road tiers encode the OSM `highway` class of the way:

| Tier | Value | OSM classes |
| --- | --- | --- |
| highway | 0 | `motorway`, `trunk`, incl. `_link` |
| main | 1 | `primary`, `secondary`, incl. `_link` |
| local | 2 | `tertiary`, incl. `_link` |

## Generation Behavior

A packet is only used for an overmap when it intersects it, and only with the
`GEOGRAPHY_MODE` world option set to `real_earth`. Within a covered overmap:

* Zones are painted as their rules request; unpainted classes keep default
  terrain and procedural micro texture.
* The vanilla river and lake noise passes are skipped for covered overmaps;
  rivers come from the packet instead. Polylines are painted at their
  per-river width (falling back to the `river` rule width when a river
  carries none) and polished like vanilla rivers.
* Road polylines are rasterized cell by cell. Roads over water become bridges:
  the span gets elevated bridge terrain with ramps at both ends, and water under
  the deck gets the under-bridge terrain. When a bridge run crosses water
  diagonally, it is rebuilt as an axis-aligned (Manhattan) path so the vanilla
  bridgehead and ramp logic applies cleanly.
* Vanilla `place_roads` is skipped for covered overmaps, so procedural roads do
  not cut across the real network.
* Road ends at the edges of the covered overmap are exposed through
  `local_road` `connections_out` entries, so adjacent procedurally generated
  overmaps pick the real roads up at their borders and extend them outward.
* Inside big procedural city footprints (city seed and neighbors of radius
  8+), `local`-tier roads are dropped to let the procedural street grid own the
  area. Higher tiers pass through.
* Cities from the packet are placed with the vanilla city machinery; the
  vanilla `place_isolated_cities` pass is disabled while any packet provides
  city guidance.

## Converter

The converter lives in `tools/geo/make_geo_packet.py` and requires a Python
environment with `rasterio`:

```sh
python3 -m venv .venv && .venv/bin/pip install rasterio
.venv/bin/python tools/geo/make_geo_packet.py \
  --bbox -72.80 42.03 -70.95 42.90 \
  --out-dir ~/.local/share/cataclysm-bn/mods/geo_new_england \
  --mod-id geo_new_england
```

| Argument | Description |
| --- | --- |
| --bbox | (_required_) Region bounding box, `west south east north` in degrees. The bbox center is anchored to absolute OMT (0, 0), the world spawn point. |
| --out-dir | (_required_) Output mod directory; receives the `.cmap` sidecar, the mod JSON and a `.ppm` preview. |
| --mod-id | Mod identifier, used in generated JSON. Defaults to `geo_new_england`. |
| --cache-dir | Cache for downloaded tiles and Overpass responses. Defaults to `~/.cache/cbn-geo`. |
| --no-rivers | Skip the Overpass rivers/cities query and write no `river`/`cities` members. |

The `.ppm` preview (P6, north up) is a quick human check: convert it with
`ffmpeg -i x_preview.ppm x.png`.

Both road and river polylines are thinned with Douglas-Peucker at a 0.5 OMT
(12 m) tolerance before encoding, and river widths are harvested from the OSM
`width` tag where present.

Data sources and licenses:

* Land cover mask: [ESA WorldCover 10 m 2021 v200](https://esa-worldcover.org)
  3°x3° COG tiles (public S3 bucket, downloaded on demand), majority-sampled
  into zone cells of 8×8 OMT (192 m). © ESA WorldCover project 2021 / Contains
  modified Copernicus Sentinel data (2021) processed by ESA WorldCover
  consortium — [CC-BY 4.0](https://creativecommons.org/licenses/by/4.0/).
* Rivers (`waterway=river|canal` polylines) and city/town place nodes from
  OpenStreetMap via the Overpass API. © OpenStreetMap contributors,
  [ODbL](https://opendatacommons.org/licenses/odbl/).
* Roads (`highway` in `motorway|trunk|primary|secondary|tertiary`, incl.
  `_link`) from OpenStreetMap via the Overpass API, under the same ODbL terms.
