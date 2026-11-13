# Real-earth geography packets

Builds an `overmap_geo` packet (.cmap + mod JSON) from real-world geodata for
the world-map painter (see `src/overmap_geo.cpp`). The game runs on Earth, so
the overmap terrain can be painted from actual land cover instead of noise.

## Usage

Requires `rasterio` (Python 3):

```sh
python3 -m venv .venv && .venv/bin/pip install rasterio
.venv/bin/python tools/geo/make_geo_packet.py \
  --bbox -72.80 42.03 -70.95 42.90 \
  --out-dir ~/.local/share/cataclysm-bn/mods/geo_ne \
  --mod-id geo_new_england
```

`--bbox` takes `west south east north` in degrees. The bbox center is anchored
to absolute OMT (0, 0) — the world start. Tiles are cached in `--cache-dir`
(default `~/.cache/cbn-geo`); Overpass responses in `overpass.json`.

Then enable the produced mod on a world with the `GEOGRAPHY_MODE` world option
set to `real_earth`.

## Data sources

* Land cover mask: [ESA WorldCover 10 m 2021 v200](https://esa-worldcover.org)
  3°×3° COG tiles (public S3 bucket, downloaded on demand), majority-sampled
  into zone cells of 8×8 OMT (192 m).  © ESA WorldCover project 2021 /
  Contains modified Copernicus Sentinel data (2021) processed by ESA
  WorldCover consortium — [CC-BY 4.0](https://creativecommons.org/licenses/by/4.0/).
* Rivers (`waterway=river|canal` polylines, painted at the configured width)
  and city/town place nodes (real-world city seeds, placed with the regular
  city machinery) from OpenStreetMap via the Overpass API.
  © OpenStreetMap contributors,
  [ODbL](https://opendatacommons.org/licenses/odbl/).
* Roads (`highway` in `motorway|trunk|primary|secondary|tertiary`, incl.
  `_link`) as tiered polylines, painted as roads with bridges over water.
  Same ODbL terms.

Player- and modder-facing documentation: `docs/en/mod/json/guides/overmap_geo.md`
(also translated in `docs/ko` and `docs/ja`).

## Format

Binary layout mirrors the C++ reader in `src/overmap_geo.cpp` ("BNCG", version
3): i32 anchor, u16 span, u32 cells, zone count, river count, RLE
`[count, zone]` byte pairs, river polylines as `u16 vertex count` + `u8 width`
(0 = use the JSON river rule width) + zigzag-varint vertex deltas relative to
the previous vertex (first one relative to the anchor), then roads as
`u32 road count` + per road `u8 tier (0..2)` + zigzag-varint vertex count +
zigzag-varint vertex deltas. Polylines are thinned with Douglas-Peucker at
a 0.5-cell (12 m) tolerance before encoding. Cities are NOT in the binary; they
live in the mod JSON (`cities` array) as deltas from the anchor, mirroring the
river vertex space.

A `.ppm` preview (P6, north up) is written next to the packet for a quick
human check (`ffmpeg -i x_preview.ppm x.png` to convert).
