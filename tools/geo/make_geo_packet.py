#!/usr/bin/env python3
"""Builds a real-earth "overmap_geo" packet (.cmap + JSON) for Cataclysm: BN.

Data sources (numeric classes baked into the packet, follows C++ parser in
src/overmap_geo.cpp, format "BNCG" version 3, cell span = 8 OMT = 192 m):

  * ESA WorldCover 10 m 2021 v200 (3x3 deg COG tiles, public S3 bucket),
    remapped into zone classes: forest / water / wetland / built-up.
    (c) ESA WorldCover project 2021 / Contains modified Copernicus Sentinel
    data (2021) processed by ESA WorldCover consortium.  CC-BY 4.0.
  * OpenStreetMap waterways (river, canal) -> simplified river polylines with
    widths from the OSM `width` tag, and place
    nodes (city, town) -> city list (informational, stage 4).
    (c) OpenStreetMap contributors, ODbL.

Heights are projected with an equirectangular approximation around the bbox
center: 1 OMT = 24 m, macro cell = 8 OMT.  The packet is anchored so that the
center of the bbox lands on absolute OMT (0, 0) (the world start).
"""

import argparse
import json
import math
import os
import re
import struct
import sys
import time
import urllib.parse
import urllib.request

# --- constants matching the C++ parser (src/overmap_geo.cpp) --------------- #
MAGIC = b"BNCG"
# Packet format: 1 = mask+rivers, 2 = + tiered roads, 3 = + per-river widths
# and zigzag-varint vertex encoding.
VERSION = 3
SPAN = 8            # OMTs per macro cell side
OMT_M = 24          # meters per OMT cell
# Douglas-Peucker simplification tolerance in OMT cells (12 m): polylines are
# thinned before encoding to shrink the packet; a 0.5-cell tolerance keeps
# every shape within half a cell of its source line.
MAX_ERROR_CELLS = 0.5

ZONE_NONE = 0
ZONE_FOREST = 1
ZONE_WATER = 2
ZONE_WETLAND = 3
ZONE_BUILTUP = 4    # unpainted for now; stage 4 city guidance
ZONE_COUNT = 5

# WorldCover land cover class -> packet zone.
WC_ZONE = {
    10: ZONE_FOREST,     # trees
    20: ZONE_FOREST,     # shrubland
    50: ZONE_BUILTUP,    # built-up
    80: ZONE_WATER,      # permanent water bodies
    90: ZONE_WETLAND,    # herbaceous wetland
}

WORLDCOVER_TILE = "https://esa-worldcover.s3.eu-central-1.amazonaws.com" \
    "/v200/2021/map/ESA_WorldCover_10m_2021_v200_{tile}_Map.tif"
OVERPASS_ENDPOINTS = [
    "https://overpass-api.de/api/interpreter",
    "https://maps.mail.ru/osm/tools/overpass/api/interpreter",
    "https://overpass.kumi.systems/api/interpreter",
    "https://overpass.private.coffee/api/interpreter",
    "https://overpass.openstreetmap.ru/api/interpreter",
]

M_PER_DEG_LAT = 110574.0


def m_per_deg_lon(lat: float) -> float:
    return 111320.0 * math.cos(math.radians(lat))


# --- worldcover raster ---------------------------------------------------- #

def wc_tile_name(lon: float, lat: float) -> str:
    def lat_part() -> str:
        return ("N%02d" if lat >= 0 else "S%02d") % abs(int(lat // 3) * 3)

    def lon_part() -> str:
        return ("E%03d" if lon >= 0 else "W%03d") % abs(int(lon // 3) * 3)

    return lat_part() + lon_part()


def download_tiles(bbox, cache_dir: str) -> list:
    """Downloads the WorldCover raster tiles covering bbox, returns paths."""
    os.makedirs( cache_dir, exist_ok=True )
    lon0, lat0, lon1, lat1 = bbox
    tiles = sorted( { wc_tile_name( min(lon0, lon1), min(lat0, lat1) ),
                      wc_tile_name( max(lon0, lon1), max(lat0, lat1) ) } )
    paths = []
    for tile in tiles:
        path = os.path.join(cache_dir, tile + ".tif")
        if not os.path.exists( path ):
            url = WORLDCOVER_TILE.format( tile=tile )
            print( f"downloading {url}" )
            tmp = path + ".part"
            urllib.request.urlretrieve( url, tmp )
            os.replace( tmp, path )
        paths.append( path )
    return paths


def build_mask(paths, bbox, cells_x: int, cells_y: int, anchor: tuple):
    """Majority-downsamples worldcover tiles into the zone mask (row-major).

    Cell (gx, gy) covers the absolute OMT rectangle
    [anchor + (gx, gy) * SPAN, anchor + (gx + 1, gy + 1) * SPAN),
    with y growing south.
    """
    import numpy as np
    import rasterio
    from rasterio.windows import Window

    west, south, east, north = bbox
    lat_c = ( south + north ) / 2
    lon_c = ( west + east ) / 2

    def cell_bounds( gx: int, gy: int ):
        """lon0, lon1, lat_top, lat_bottom corners of a mask cell."""
        y0 = anchor[1] + gy * SPAN          # north edge is smaller OMT y
        x0 = anchor[0] + gx * SPAN
        lat_top = lat_c - y0 * OMT_M / M_PER_DEG_LAT
        lat_bottom = lat_c - ( y0 + SPAN ) * OMT_M / M_PER_DEG_LAT
        lon0 = lon_c + x0 * OMT_M / m_per_deg_lon( lat_c )
        lon1 = lon_c + ( x0 + SPAN ) * OMT_M / m_per_deg_lon( lat_c )
        return lon0, lon1, lat_top, lat_bottom

    mask = np.zeros( ( cells_y, cells_x ), dtype=np.uint8 )
    for path in paths:
        with rasterio.open( path ) as ds:
            tw, ts, te, tn = ds.bounds
            if te <= west or tw >= east or tn <= south or ts >= north:
                continue
            done = 0
            for gy in range( cells_y ):
                _, _, lat_top, lat_bottom = cell_bounds( 0, gy )
                if lat_bottom >= tn or lat_top <= ts:
                    continue
                # source pixel rows for this cell row
                mid_lat = ( lat_top + lat_bottom ) / 2
                mid_lon = ( max( tw, west ) + min( te, east ) ) / 2
                # note: rasterio's index() takes (lon, lat) -- not (lat, lon)
                r_top = ds.index( mid_lon, lat_top )[0]
                r_bottom = ds.index( mid_lon, lat_bottom )[0]
                if r_top > r_bottom:
                    continue
                # columns of this tile overlapping the bbox
                c_first = ds.index( max( tw, west ), mid_lat )[1]
                c_last = ds.index( min( te, east ), mid_lat )[1]
                if c_first >= c_last:
                    continue
                strip = ds.read( 1, window=Window( c_first, r_top,
                                                   c_last - c_first,
                                                   r_bottom - r_top ) )
                for gx in range( cells_x ):
                    lon0, lon1, _, _ = cell_bounds( gx, gy )
                    if lon1 <= tw or lon0 >= te:
                        continue
                    cl0 = ds.index( lon0, mid_lat )[1]
                    cl1 = ds.index( lon1, mid_lat )[1]
                    block = strip[:, max( cl0 - c_first, 0 ):max( cl1 - c_first, 1 )]
                    if block.size == 0:
                        continue
                    vals, counts = np.unique( block, return_counts=True )
                    zone = WC_ZONE.get( int( vals[np.argmax( counts )] ), ZONE_NONE )
                    mask[gy, gx] = zone
                done += 1
                if done % 60 == 0:
                    print( f"  {os.path.basename(path)}: mask rows {done}",
                           flush=True )
    import collections
    stats = collections.Counter( mask.ravel().tolist() )
    print( "zones: " + ", ".join( f"z{z}={n}" for z, n in sorted( stats.items() ) ) )
    return mask


# --- overpass ------------------------------------------------------------- #

def fetch_overpass( query: str, cache_file: str, retries: int = 2 ) -> dict:
    """Runs one Overpass query with endpoint fallback and an answer cache."""
    if os.path.exists( cache_file ):
        with open( cache_file ) as f:
            print( f"overpass cache: {cache_file}" )
            return json.load( f )
    body = urllib.parse.urlencode( { "data": query } ).encode()
    last = None
    for attempt in range( retries + 1 ):
        for endpoint in OVERPASS_ENDPOINTS:
            try:
                req = urllib.request.Request(
                    endpoint, data=body, method="POST",
                    headers={ "User-Agent": "cataclysm-bn-geo/1.0 (world geography packets)" } )
                with urllib.request.urlopen( req, timeout=180 ) as resp:
                    data = json.loads( resp.read().decode() )
                with open( cache_file, "w" ) as f:
                    json.dump( data, f )
                return data
            except Exception as err:  # noqa: mirror endpoints on any failure
                last = err
                print( f"overpass {endpoint} failed: {err}" )
        time.sleep( 5 * ( attempt + 1 ) )
    raise RuntimeError( f"overpass failed: {last}" )


def overpass( bbox, cache_file: str, retries: int = 2 ) -> dict:
    """Rivers (waterway=river|canal) and city/town place nodes."""
    lon0, lat0, lon1, lat1 = bbox
    q = f"""[out:json][timeout:120];
(
  way["waterway"~"^(river|canal)$"]({lat0},{lon0},{lat1},{lon1});
  node["place"~"^(city|town)$"]({lat0},{lon0},{lat1},{lon1});
);
out geom;
"""
    return fetch_overpass( q, cache_file, retries )


def overpass_roads( bbox, cache_file: str, retries: int = 2 ) -> dict:
    """Regional OSM highways: motorway/trunk/primary/secondary/tertiary + links."""
    lon0, lat0, lon1, lat1 = bbox
    q = f"""[out:json][timeout:180];
way["highway"~"^(motorway|trunk|primary|secondary|tertiary)(_link)?$"]
    ({lat0},{lon0},{lat1},{lon1});
out geom;
"""
    return fetch_overpass( q, cache_file, retries )


def latlon_to_omt( lat: float, lon: float, lat_c: float, lon_c: float ):
    x = ( lon - lon_c ) * m_per_deg_lon( lat_c ) / OMT_M
    y = -( lat - lat_c ) * M_PER_DEG_LAT / OMT_M
    return int( round( x ) ), int( round( y ) )


def width_cells_from_tags( tags ) -> int:
    """River width in OMT cells from the OSM `width` tag (meters); 0 when
    untagged (painter falls back to the JSON river rule width)."""
    match = re.match( r"([0-9.]+)", str( tags.get( "width", "" ) ).strip() )
    if not match:
        return 0
    cells = int( round( float( match.group( 1 ) ) / OMT_M ) )
    return max( 1, min( cells, 255 ) )


def simplify( pts, max_error: float = MAX_ERROR_CELLS ):
    """Iterative Douglas-Peucker polyline thinning; endpoints always kept."""
    n = len( pts )
    if n < 3:
        return list( pts )
    keep = bytearray( n )
    keep[0] = keep[-1] = 1
    threshold = max_error * max_error

    def chord_dist_sq( p, first, last ) -> float:
        x0, y0 = pts[first]
        x1, y1 = pts[last]
        dx, dy = x1 - x0, y1 - y0
        l2 = dx * dx + dy * dy
        if l2 == 0:
            return ( p[0] - x0 ) ** 2 + ( p[1] - y0 ) ** 2
        t = max( 0.0, min( 1.0, ( ( p[0] - x0 ) * dx + ( p[1] - y0 ) * dy ) / l2 ) )
        return ( p[0] - x0 - t * dx ) ** 2 + ( p[1] - y0 - t * dy ) ** 2

    stack = [ ( 0, n - 1 ) ]
    while stack:
        first, last = stack.pop()
        if last - first < 2:
            continue
        worst_idx = -1
        worst_d = threshold
        for i in range( first + 1, last ):
            d = chord_dist_sq( pts[i], first, last )
            if d > worst_d:
                worst_d = d
                worst_idx = i
        if worst_idx >= 0:
            keep[worst_idx] = 1
            stack.append( ( first, worst_idx ) )
            stack.append( ( worst_idx, last ) )
    return [ pts[i] for i in range( n ) if keep[i] ]


def build_rivers( data, lat_c: float, lon_c: float ):
    """Waterway ways -> simplified polylines of OMT points."""
    rivers = []
    skipped = 0
    before = 0
    after = 0
    for el in data.get( "elements", [] ):
        if el.get( "type" ) != "way" or "geometry" not in el:
            continue
        pts = []
        for g in el["geometry"]:
            x, y = latlon_to_omt( g["lat"], g["lon"], lat_c, lon_c )
            if not pts or pts[-1] != ( x, y ):
                pts.append( ( x, y ) )
        if len( pts ) < 2:
            skipped += 1
            continue
        pts = simplify( pts )
        before += len( el["geometry"] )
        after += len( pts )
        rivers.append( { "pts": pts,
                         "width": width_cells_from_tags( el.get( "tags", {} ) ) } )
    print( f"rivers: {len( rivers )} polylines ({skipped} skipped, "
           f"{before} -> {after} vertices)" )
    return rivers


ROAD_TIER_BY_CLASS = {
    "motorway": 0, "motorway_link": 0, "trunk": 0, "trunk_link": 0,
    "primary": 1, "primary_link": 1, "secondary": 1, "secondary_link": 1,
    "tertiary": 2, "tertiary_link": 2,
}


def build_roads( data, lat_c: float, lon_c: float, anchor: tuple,
                 extent: tuple ) -> list:
    """OSM highway ways -> tiered polylines of anchor-relative OMT points.

    Polylines are clipped to the packet coverage rectangle; runs outside it
    are dropped and the way is split into contiguous runs.
    """
    extent_x, extent_y = extent
    roads = []
    clipped = 0
    for el in data.get( "elements", [] ):
        if el.get( "type" ) != "way" or "geometry" not in el:
            continue
        tags = el.get( "tags", {} )
        tier = ROAD_TIER_BY_CLASS.get( tags.get( "highway", "" ) )
        if tier is None:
            continue

        def inside( omt: tuple ) -> bool:
            x, y = omt[0] - anchor[0], omt[1] - anchor[1]
            return 0 <= x < extent_x and 0 <= y < extent_y

        run = []
        last_inside = False
        for g in el["geometry"]:
            omt = latlon_to_omt( g["lat"], g["lon"], lat_c, lon_c )
            here = inside( omt )
            if here:
                pt = ( omt[0] - anchor[0], omt[1] - anchor[1] )
                if not run or run[-1] != pt:
                    run.append( pt )
            elif run and last_inside:
                # Coverage ends: cut the run here (keep the exit vertex).
                run.append( ( omt[0] - anchor[0], omt[1] - anchor[1] ) )
            if not here and run:
                if len( run ) >= 2:
                    roads.append( { "tier": tier, "pts": simplify( run ) } )
                run = []
            last_inside = here
        if len( run ) >= 2:
            roads.append( { "tier": tier, "pts": simplify( run ) } )
            run = []
    if clipped:
        pass
    print( f"roads: {len( roads)} clipped polylines "
           f"(tiers 0/1/2: "
           f"{sum(1 for r in roads if r['tier']==0)}/"
           f"{sum(1 for r in roads if r['tier']==1)}/"
           f"{sum(1 for r in roads if r['tier']==2)})" )
    return roads


def build_cities( data, lat_c: float, lon_c: float ) -> list:
    cities = []
    for el in data.get( "elements", [] ):
        if el.get( "type" ) != "node" or "place" not in el.get( "tags", {} ):
            continue
        tags = el["tags"]
        x, y = latlon_to_omt( el["lat"], el["lon"], lat_c, lon_c )
        cities.append( { "name": tags.get( "name", "?" ),
                         "place": tags["place"],
                         "population": int( tags.get( "population", 0 ) ),
                         "omt": [x, y] } )
    print( f"cities: {len( cities )} place nodes" )
    return cities


# --- packet writer -------------------------------------------------------- #

def rle( mask ) -> list:
    runs = []
    flat = mask.ravel()
    start = 0
    for i in range( 1, len( flat ) + 1 ):
        if i == len( flat ) or flat[i] != flat[start]:
            value = int( flat[start] )
            count = i - start
            while count > 255:
                runs.append( ( 255, value ) )
                count -= 255
            runs.append( ( count, value ) )
            start = i
    return runs


def put_varint( delta: int ) -> bytes:
    """LEB128 base-128 varint of a zigzag-mapped signed delta."""
    zig = ( delta << 1 ) if delta >= 0 else ( ( -delta << 1 ) - 1 )
    out = bytearray()
    while zig > 0x7F:
        out.append( ( zig & 0x7F ) | 0x80 )
        zig >>= 7
    out.append( zig )
    return bytes( out )


def put_varint_points( body: bytearray, pts, prev: tuple ) -> None:
    """Encodes a polyline as zigzag-varint deltas: the first delta is
    relative to @p prev (the anchor), later ones to the previous vertex."""
    last_x, last_y = prev
    for x, y in pts:
        body += put_varint( x - last_x )
        body += put_varint( y - last_y )
        last_x, last_y = x, y


def write_cmap( path, mask, rivers, roads, anchor: tuple ) -> None:
    body = bytearray()
    body += MAGIC
    body += struct.pack( "<H", VERSION )
    body += struct.pack( "<ii", *anchor )
    body += struct.pack( "<H", SPAN )
    body += struct.pack( "<II", mask.shape[1], mask.shape[0] )
    body += struct.pack( "<H", ZONE_COUNT )
    body += struct.pack( "<H", len( rivers ) )
    for count, value in rle( mask ):
        body += struct.pack( "BB", count, value )
    # Version 3: per-river width byte (0 = use the JSON river rule width)
    # and zigzag-varint vertex deltas.
    for river in rivers:
        body += struct.pack( "<H", len( river["pts"] ) )
        body += struct.pack( "<B", river["width"] )
        put_varint_points( body, river["pts"], anchor )
    # Version 2: tiered road polylines after the river section; version 3
    # encodes vertex counts and deltas as zigzag varints.
    body += struct.pack( "<I", len( roads ) )
    for road in roads:
        body += struct.pack( "<B", road["tier"] )
        body += put_varint( len( road["pts"] ) )
        put_varint_points( body, road["pts"], ( 0, 0 ) )
    with open( path, "wb" ) as f:
        f.write( body )
    print( f"binary: {len( body )} bytes -> {path}" )


def city_size_for( population: int, place: str ) -> int:
    """Vanilla city radius (OMT cells) from an OSM place population."""
    if population >= 100000:
        return 16
    if population >= 30000:
        return 12
    if population >= 10000:
        return 8
    if population >= 3000:
        return 5
    if population >= 1000:
        return 3
    if population >= 200:
        return 2
    return 2 if place == "city" else 1


def write_json( path, mod_id, anchor, mask_cells, cities, roads ) -> None:
    extent_x = mask_cells[0] * SPAN
    extent_y = mask_cells[1] * SPAN
    city_entries = []
    for city in sorted( cities, key=lambda c: -c["population"] ):
        x = city["omt"][0] - anchor[0]
        y = city["omt"][1] - anchor[1]
        if not ( 0 <= x < extent_x and 0 <= y < extent_y ):
            print( f"city '{city['name']}' outside the bbox mask, skipped" )
            continue
        size = city_size_for( city["population"], city["place"] )
        city_entries.append( {
            "name": city["name"],
            "x": x,
            "y": y,
            "size": size,
            "finale": size >= 12,
        } )
    geo_def = {
        "type": "overmap_geo",
        "id": mod_id,
        "file": mod_id + ".cmap",
        "zones": {
            str( ZONE_FOREST ): { "paint": "forest" },
            str( ZONE_WATER ): { "paint": "lake_surface" },
            str( ZONE_WETLAND ): { "paint": "forest_water" },
        },
        "river": { "paint": "river_center", "width": 3 },
        "cities": city_entries,
    }
    modinfo = {
        "type": "MOD_INFO",
        "id": mod_id,
        "name": "Real-earth geography: New England",
        "authors": ["dzok"],
        "description": "Real-earth overmap geography built from ESA WorldCover 2021 "
                       "(CC-BY 4.0) and (c) OpenStreetMap contributors (ODbL).",
        "category": "content",
        "dependencies": ["bn"],
    }
    with open( path, "w" ) as f:
        json.dump( [geo_def], f, indent=2, ensure_ascii=False )
        f.write( "\n" )
    with open( os.path.join( os.path.dirname( path ), "modinfo.json" ), "w" ) as f:
        json.dump( modinfo, f, indent=2, ensure_ascii=False )
        f.write( "\n" )
    print( f"json with {len( city_entries )} cities -> {path}" )


def write_preview( path, mask, rivers, anchor, scale: int = 8 ) -> None:
    """Plain PPM preview: zones + rivers, north up."""
    import numpy as np
    colors = {
        ZONE_NONE: ( 240, 230, 190 ),      # field-ish
        ZONE_FOREST: ( 60, 105, 45 ),
        ZONE_WATER: ( 45, 90, 160 ),
        ZONE_WETLAND: ( 110, 140, 170 ),
        ZONE_BUILTUP: ( 170, 170, 170 ),
    }
    img = np.zeros( ( mask.shape[0], mask.shape[1], 3 ), dtype=np.uint8 )
    for zone, color in colors.items():
        img[mask == zone] = color
    for river in rivers:
        for x, y in river["pts"]:
            cy = ( y - anchor[1] ) // SPAN
            cx = ( x - anchor[0] ) // SPAN
            if 0 <= cy < mask.shape[0] and 0 <= cx < mask.shape[1]:
                img[cy, cx] = ( 25, 60, 120 )
    big = np.kron( img, np.ones( ( scale, scale, 1 ), dtype=np.uint8 ) )
    with open( path, "wb" ) as f:
        f.write( b"P6\n" )
        f.write( f"{big.shape[1]} {big.shape[0]}\n255\n".encode() )
        f.write( big.tobytes() )
    print( f"preview (P6 PPM) -> {path}" )


# --- main ------------------------------------------------------------------ #

def main() -> int:
    p = argparse.ArgumentParser( description=__doc__.splitlines()[0] )
    p.add_argument( "--bbox", nargs=4, type=float, required=True,
                    metavar=("LON0", "LAT0", "LON1", "LAT1"),
                    help="west south east north in degrees" )
    p.add_argument( "--out-dir", required=True, help="output mod directory" )
    p.add_argument( "--mod-id", default="geo_new_england" )
    p.add_argument( "--cache-dir", default=os.path.expanduser( "~/.cache/cbn-geo" ) )
    p.add_argument( "--no-rivers", action="store_true" )
    args = p.parse_args()

    lon0, lat0, lon1, lat1 = args.bbox
    assert lon1 > lon0 and lat1 > lat0, "bbox must be west south east north"
    lat_c, lon_c = ( lat0 + lat1 ) / 2, ( lon0 + lon1 ) / 2

    # packet geometry: cell (0,0) anchored so bbox center sits on OMT (0,0)
    width_m = ( lon1 - lon0 ) * m_per_deg_lon( lat_c )
    height_m = ( lat1 - lat0 ) * M_PER_DEG_LAT
    cells_x = math.ceil( width_m / ( SPAN * OMT_M ) )
    cells_y = math.ceil( height_m / ( SPAN * OMT_M ) )
    anchor = ( -cells_x * SPAN // 2, -cells_y * SPAN // 2 )
    print( f"packet: {cells_x}x{cells_y} cells of {SPAN * OMT_M} m "
           f"(= {width_m / 1000:.0f} km x {height_m / 1000:.0f} km), "
           f"anchor OMT ({anchor[0]}, {anchor[1]})" )

    tiles = download_tiles( args.bbox, args.cache_dir )
    mask = build_mask( tiles, args.bbox, cells_x, cells_y, anchor )

    rivers = []
    cities = []
    if not args.no_rivers:
        pad = ( max( lon0 - 0.1, -180 ), max( lat0 - 0.1, -90.0 ),
                min( lon1 + 0.1, 180.0 ), min( lat1 + 0.1, 90.0 ) )
        data = overpass( pad, os.path.join( args.cache_dir, "overpass.json" ) )
        rivers = build_rivers( data, lat_c, lon_c )
        cities = build_cities( data, lat_c, lon_c )

    road_data = overpass_roads( args.bbox,
                                os.path.join( args.cache_dir, "overpass_roads.json" ) )
    roads = build_roads( road_data, lat_c, lon_c, anchor, ( cells_x * SPAN, cells_y * SPAN ) )

    os.makedirs( args.out_dir, exist_ok=True )
    write_cmap( os.path.join( args.out_dir, args.mod_id + ".cmap" ),
                mask, rivers, roads, anchor )
    write_json( os.path.join( args.out_dir, args.mod_id + ".json" ),
                args.mod_id, anchor, ( cells_x, cells_y ), cities, roads )
    write_preview( os.path.join( args.out_dir, args.mod_id + "_preview.ppm" ),
                   mask, rivers, anchor )
    return 0


if __name__ == "__main__":
    sys.exit( main() )
