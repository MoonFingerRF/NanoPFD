#!/usr/bin/env python3
# ============================================================================
#  build_chart_data.py — generate chart_data.h from public internet datasets.
#
#  Instead of hand-writing the moving-map data, this pulls it from public-domain
#  sources, simplifies the geometry, assigns level-of-detail tiers, and writes
#  ../chart_data.h (a drop-in replacement for the hand-curated file).
#
#  Sources (all public domain / open):
#    * OurAirports   — airports, runways, navaids, radio frequencies (CSV)
#         https://davidmegginson.github.io/ourairports-data/
#    * Natural Earth — state lines, coastline/borders, rivers, roads, cities
#         https://github.com/nvkelso/natural-earth-vector  (GeoJSON, 1:50m / 1:10m)
#
#  Usage:
#    python3 build_chart_data.py                 # CONUS (lower-48)
#    python3 build_chart_data.py --lat 39.1 --lon -84.5 --radius-km 600
#    python3 build_chart_data.py --max-airports 400 --simplify 0.04
#
#  Stdlib only (urllib, csv, json, gzip). Downloads are cached in tools/.cache/.
#  Frequencies/abbreviations come straight from the source data.
# ============================================================================
import argparse, csv, io, json, math, os, sys, urllib.request, urllib.parse, gzip

HERE   = os.path.dirname(os.path.abspath(__file__))
CACHE  = os.path.join(HERE, ".cache")
OUTPUT = os.path.normpath(os.path.join(HERE, "..", "chart_data.h"))

OA = "https://davidmegginson.github.io/ourairports-data/"
NE = "https://raw.githubusercontent.com/nvkelso/natural-earth-vector/master/geojson/"

SOURCES = {
    "airports":    OA + "airports.csv",
    "runways":     OA + "runways.csv",
    "freqs":       OA + "airport-frequencies.csv",
    "navaids":     OA + "navaids.csv",
    "states":      NE + "ne_50m_admin_1_states_provinces_lines.geojson",
    "coast":       NE + "ne_50m_coastline.geojson",
    "rivers":      NE + "ne_10m_rivers_lake_centerlines.geojson",
    "roads":       NE + "ne_10m_roads.geojson",
    "cities":      NE + "ne_50m_populated_places_simple.geojson",
}

# ---------------------------------------------------------------------------
def fetch(name):
    os.makedirs(CACHE, exist_ok=True)
    path = os.path.join(CACHE, name + os.path.splitext(SOURCES[name])[1])
    if not os.path.exists(path):
        sys.stderr.write("downloading %-9s ... " % name); sys.stderr.flush()
        req = urllib.request.Request(SOURCES[name], headers={"User-Agent": "instrumentpanel"})
        data = urllib.request.urlopen(req, timeout=120).read()
        with open(path, "wb") as f: f.write(data)
        sys.stderr.write("%d KB\n" % (len(data) // 1024))
    return path

def load_csv(name):
    with open(fetch(name), newline="", encoding="utf-8") as f:
        return list(csv.DictReader(f))

def load_geojson(name):
    with open(fetch(name), encoding="utf-8") as f:
        return json.load(f)

def fetch_overpass(box):
    """Amusement parks (VFR-visible landmarks like Kings Island) from OpenStreetMap
    via the Overpass API, within the region bbox. Cached in .cache/landmarks.json.
    Returns None (build continues with curated landmarks only) if Overpass is down."""
    path = os.path.join(CACHE, "landmarks.json")
    if not os.path.exists(path):
        os.makedirs(CACHE, exist_ok=True)
        s, w, n, e = box[0], box[2], box[1], box[3]          # south, west, north, east
        q = ("[out:json][timeout:120];("
             'node["tourism"="theme_park"](%f,%f,%f,%f);'
             'way["tourism"="theme_park"](%f,%f,%f,%f);'
             ");out center tags;") % (s, w, n, e, s, w, n, e)
        url = "https://overpass-api.de/api/interpreter?data=" + urllib.parse.quote(q)
        try:
            sys.stderr.write("downloading landmarks(overpass) ... "); sys.stderr.flush()
            req = urllib.request.Request(url, headers={"User-Agent": "instrumentpanel"})
            data = urllib.request.urlopen(req, timeout=180).read()
            with open(path, "wb") as f: f.write(data)
            sys.stderr.write("%d KB\n" % (len(data) // 1024))
        except Exception as ex:
            sys.stderr.write("FAILED (%s) — curated landmarks only\n" % ex)
            return None
    with open(path) as f: return json.load(f)

def short_name(nm, maxlen=12):
    """Compact ASCII uppercase label that breaks on a word boundary (KINGS ISLAND).
    Non-ASCII (curly quotes, accents) is dropped — the classic font is ASCII only."""
    nm = nm.encode("ascii", "ignore").decode()
    nm = " ".join(nm.upper().split())
    if len(nm) <= maxlen: return nm
    out = ""
    for word in nm.split():
        if out and len(out) + 1 + len(word) > maxlen: break
        out = (out + " " + word) if out else word
    return (out or nm)[:maxlen]

# ---- geometry --------------------------------------------------------------
def dp(pts, tol):
    """Douglas-Peucker simplify a [(lon,lat),...] list (tol in degrees)."""
    if len(pts) < 3: return pts
    a, b = pts[0], pts[-1]
    dx, dy = b[0] - a[0], b[1] - a[1]
    den = math.hypot(dx, dy) or 1e-9
    imax, dmax = 0, 0.0
    for i in range(1, len(pts) - 1):
        px, py = pts[i]
        d = abs((px - a[0]) * dy - (py - a[1]) * dx) / den
        if d > dmax: imax, dmax = i, d
    if dmax > tol:
        return dp(pts[:imax + 1], tol)[:-1] + dp(pts[imax:], tol)
    return [a, b]

def iter_lines(geom):
    """Yield coordinate lists from a (Multi)LineString geometry."""
    if not geom: return
    if geom["type"] == "LineString": yield geom["coordinates"]
    elif geom["type"] == "MultiLineString":
        for ls in geom["coordinates"]: yield ls

def in_box(lat, lon, box):
    return box[0] <= lat <= box[1] and box[2] <= lon <= box[3]

# ---- emit helpers ----------------------------------------------------------
def fnum(x):  # compact, always-valid C++ float literal (39.1047f, 90.0f, 0.0f)
    s = ("%.4f" % x).rstrip("0")
    if s.endswith("."): s += "0"
    return s + "f"

def poly_rows(name, lines, box, tol, ptype, tier, label="", maxpts=40):
    """Return (array C source, ChartPoly rows). Long lines are SPLIT into chunks of
    <= maxpts (1-point overlap so chunks connect) — preserves every point instead of
    subsampling, so rivers/borders keep their full resolution."""
    arrs, rows = [], []
    n = 0
    for coords in lines:
        coords = [(c[0], c[1]) for c in coords if in_box(c[1], c[0], box)]
        if len(coords) < 2: continue
        coords = dp(coords, tol)
        for j in range(0, len(coords) - 1, maxpts - 1):
            chunk = coords[j : j + maxpts]
            if len(chunk) < 2: continue
            las = [la for lo, la in chunk]; los = [lo for lo, la in chunk]
            var = "%s_%d" % (name, n); n += 1
            flat = ", ".join("%s,%s" % (fnum(la), fnum(lo)) for lo, la in chunk)
            arrs.append("static const float %s[] = { %s };" % (var, flat))
            rows.append('  { %s, %d, %s, %d, "%s", %s, %s, %s, %s },' % (
                var, len(chunk), ptype, tier, label,
                fnum(min(las)), fnum(max(las)), fnum(min(los)), fnum(max(los))))
    return arrs, rows

# ===========================================================================
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--lat", type=float); ap.add_argument("--lon", type=float)
    ap.add_argument("--radius-km", type=float, default=0)
    ap.add_argument("--detail-km", type=float, default=200,
                    help="small + closed airports and landmarks only within this radius of center")
    ap.add_argument("--max-airports", type=int, default=600)
    ap.add_argument("--simplify", type=float, default=0.05, help="DP tolerance, degrees")
    args = ap.parse_args()

    # region bbox (lat_lo, lat_hi, lon_lo, lon_hi)
    if args.lat is not None and args.radius_km:
        dlat = args.radius_km / 111.0
        dlon = dlat / max(0.2, math.cos(math.radians(args.lat)))
        box = (args.lat - dlat, args.lat + dlat, args.lon - dlon, args.lon + dlon)
    else:
        box = (24.0, 49.5, -125.0, -66.5)          # CONUS

    pts, rwys, rings, segs, arrs, polys = [], [], [], [], [], []

    # ---- airports + frequencies + runways ---------------------------------
    freq_by_apt = {}
    for r in load_csv("freqs"):
        t = r["type"].upper()
        if t in ("TWR", "CTAF", "UNICOM") and r["airport_ident"] not in freq_by_apt:
            freq_by_apt[r["airport_ident"]] = r["frequency_mhz"]
    AP_TIER = {"large_airport": 0, "medium_airport": 1, "small_airport": 2, "closed": 3}
    # Large/medium span the full region; small + closed (decommissioned) airports are
    # close-in detail (they only render at close zoom), so limit them to detail_km of
    # the center to keep the dataset small.
    cla = args.lat if args.lat is not None else (box[0] + box[1]) / 2
    clo = args.lon if args.lon is not None else (box[2] + box[3]) / 2
    detail_box = box
    if args.detail_km:
        dla = args.detail_km / 111.0; dlo = dla / max(0.2, math.cos(math.radians(cla)))
        detail_box = (cla - dla, cla + dla, clo - dlo, clo + dlo)
    closed_pts = []
    for r in load_csv("airports"):
        aptype = r["type"]
        tier = AP_TIER.get(aptype)
        if tier is None: continue
        try: la, lo = float(r["latitude_deg"]), float(r["longitude_deg"])
        except ValueError: continue
        if not in_box(la, lo, box if tier <= 1 else detail_box): continue
        ident = (r["iata_code"] or r["local_code"] or r["ident"])[:4]
        if aptype == "closed":                                   # decommissioned airfield
            closed_pts.append((la, lo, "PT_AIRPORT_CLOSED", tier, ident, "", r["ident"]))
            continue
        twr = r["scheduled_service"] == "yes" or aptype == "large_airport"
        freq = freq_by_apt.get(r["ident"], "")
        pts.append((la, lo, "PT_AIRPORT_TWR" if twr else "PT_AIRPORT_NTWR", tier, ident, freq, r["ident"]))
    # large/medium first, then the NEAREST small up to the cap. Decommissioned fields
    # are close-in only (detail_km), so append them ALL after the cap (else tier-3
    # closed airports always lose the cap to the far more numerous small airports).
    pts.sort(key=lambda p: (p[3], (p[0] - cla) ** 2 + (p[1] - clo) ** 2))
    pts = pts[:args.max_airports] + closed_pts
    kept = {p[6] for p in pts}
    # runways for kept airports (tier 4 = closest detail)
    for r in load_csv("runways"):
        if r["airport_ident"] not in kept: continue
        try:
            la, lo = float(r["le_latitude_deg"]), float(r["le_longitude_deg"])
            hdg, ln = float(r["le_heading_degT"]), float(r["length_ft"]) * 0.3048
        except (ValueError, KeyError): continue
        rwys.append((la, lo, hdg, ln, 4))
        if ln >= 1200:   # extended runway centerline = final-approach / departure path (~8 NM each way)
            hr = math.radians(hdg); cl = math.cos(math.radians(la)) or 1e-3; apr = 14816.0
            a0 = (la + (-apr * math.cos(hr)) / 111320.0, lo + (-apr * math.sin(hr)) / (111320.0 * cl))
            a1 = (la + ((ln + apr) * math.cos(hr)) / 111320.0, lo + ((ln + apr) * math.sin(hr)) / (111320.0 * cl))
            segs.append((a0[0], a0[1], a1[0], a1[1], "SG_GLIDEPATH", 4))
    # Distance circles around airports — large/medium/small, but NOT decommissioned
    # (closed) airports. Colored by class; the renderer keeps any ring whose circle
    # reaches the screen even if its center is off-panel.
    APT_RING = {0: (15000.0, "RG_APT_LARGE", 1), 1: (9000.0, "RG_APT_MEDIUM", 2),
                2: (5000.0, "RG_APT_SMALL", 3)}
    for p in pts:
        if p[2].startswith("PT_AIRPORT") and p[2] != "PT_AIRPORT_CLOSED" and p[3] in APT_RING:
            rad_m, rty, rti = APT_RING[p[3]]
            rings.append((p[0], p[1], rad_m, rty, rti))

    # ---- navaids (VOR family) ---------------------------------------------
    for r in load_csv("navaids"):
        if "VOR" not in r["type"].upper(): continue
        try: la, lo = float(r["latitude_deg"]), float(r["longitude_deg"])
        except ValueError: continue
        if not in_box(la, lo, box): continue
        khz = r.get("frequency_khz", "")
        try: info = "%.2f" % (float(khz) / 1000.0) if khz else ""
        except ValueError: info = ""
        pts.append((la, lo, "PT_NAVAID", 2, r["ident"][:4], info, ""))

    # ---- cities (populated places) ----------------------------------------
    for ft in load_geojson("cities")["features"]:
        pr = ft["properties"]; g = ft["geometry"]
        if g["type"] != "Point": continue
        lo, la = g["coordinates"][:2]
        if not in_box(la, lo, box): continue
        rank = pr.get("scalerank", 5)
        if rank > 4: continue
        tier = 0 if rank <= 1 else 1
        name = (pr.get("name") or "").encode("ascii", "ignore").decode()[:4].upper()
        pts.append((la, lo, "PT_LANDMARK", tier, name, "", ""))

    # ---- landmarks: amusement parks (VFR-visible, e.g. Kings Island) -----------
    # OSM Overpass theme parks within the detail radius, plus a guaranteed curated
    # Kings Island entry so the named example is always present even if Overpass is
    # down. Drawn as yellow landmark squares with a readable name.
    landmarks = [(39.3447, -84.2686, "KINGS ISLAND")]
    ov = fetch_overpass(box)
    if ov:
        for el in ov.get("elements", []):
            if el.get("type") != "way": continue            # areas only: real parks, not small POI nodes
            c = el.get("center") or {}
            la, lo = c.get("lat"), c.get("lon")
            nm = (el.get("tags") or {}).get("name")
            if la is None or lo is None or not nm: continue
            if not in_box(la, lo, detail_box): continue
            landmarks.append((la, lo, short_name(nm)))
    placed = []
    for la, lo, nm in landmarks:
        if any(abs(la - a) < 0.02 and abs(lo - o) < 0.02 for a, o, _ in placed): continue   # dedup ~2 km
        placed.append((la, lo, nm))
        pts.append((la, lo, "PT_LANDMARK", 2, nm, "", ""))

    # ---- rivers -> POLYS (PLY_RIVER) so they draw as smooth splines --------
    a, r = poly_rows("RIV", (l for ft in load_geojson("rivers")["features"] for l in iter_lines(ft["geometry"])),
                     box, args.simplify, "PLY_RIVER", 1); arrs += a; polys += r

    # ---- borders/coast + state lines + Interstates -> POLYS ---------------
    a, r = poly_rows("COAST", (l for ft in load_geojson("coast")["features"] for l in iter_lines(ft["geometry"])),
                     box, args.simplify, "PLY_BORDER", 0); arrs += a; polys += r
    a, r = poly_rows("STATE", (l for ft in load_geojson("states")["features"] for l in iter_lines(ft["geometry"])),
                     box, args.simplify, "PLY_STATE", 1); arrs += a; polys += r
    n = 0
    for ft in load_geojson("roads")["features"]:
        pr = ft["properties"]
        if pr.get("sov_a3") != "USA": continue
        nm0 = (pr.get("name") or "")
        if pr.get("level") != "Interstate" and not nm0.startswith("I-"): continue   # Interstates only
        nm = nm0.replace(" ", "")
        a, r = poly_rows("RD%d" % n, iter_lines(ft["geometry"]), box, args.simplify, "PLY_INTERSTATE", 0, nm)
        arrs += a; polys += r; n += 1

    write_header(pts, rwys, rings, segs, arrs, polys)

# ---------------------------------------------------------------------------
def write_header(pts, rwys, rings, segs, arrs, polys):
    L = []
    L.append("#pragma once\n#include <Arduino.h>\n")
    L.append("// AUTO-GENERATED by tools/build_chart_data.py from OurAirports + Natural Earth.")
    L.append("// Do not hand-edit; re-run the script to refresh. Freqs are from source data.\n")
    L.append("enum ChartPtType   : uint8_t { PT_AIRPORT_TWR, PT_AIRPORT_NTWR, PT_NAVAID, PT_LANDMARK, PT_AIRPORT_CLOSED };")
    L.append("enum ChartSegType  : uint8_t { SG_RIVER, SG_GLIDEPATH, SG_COAST };")
    L.append("enum ChartRingType : uint8_t { RG_APT_LARGE, RG_APT_MEDIUM, RG_APT_SMALL, RG_APT_CLOSED, RG_RESTRICTED };")
    L.append("enum ChartPolyType : uint8_t { PLY_BORDER, PLY_STATE, PLY_INTERSTATE, PLY_HIGHWAY, PLY_RIVER };")
    L.append("struct ChartPt   { float lat, lon; uint8_t type, tier; const char *id, *info; };")
    L.append("struct ChartRwy  { float lat, lon; float hdg, len_m; uint8_t tier; };")
    L.append("struct ChartRing { float lat, lon; float rad_m; uint8_t type, tier; };")
    L.append("struct ChartSeg  { float lat1, lon1, lat2, lon2; uint8_t type, tier; };")
    L.append("struct ChartPoly { const float *pts; uint16_t n; uint8_t type, tier; const char *name; float la0, la1, lo0, lo1; };")
    L.append("struct MapProj   { float clat, clon, cosLat, cosH, sinH, scale; int cx, cy; };\n")
    L.append("#if   MAP_RANGE_M <= 50000\n  #define MAP_MAX_TIER 4")
    L.append("#elif MAP_RANGE_M <= 150000\n  #define MAP_MAX_TIER 3")
    L.append("#elif MAP_RANGE_M <= 400000\n  #define MAP_MAX_TIER 2")
    L.append("#elif MAP_RANGE_M <= 800000\n  #define MAP_MAX_TIER 1")
    L.append("#else\n  #define MAP_MAX_TIER 0\n#endif\n")

    L.append("static const ChartPt CHART_PTS[] = {")
    for la, lo, ty, ti, idv, info, _ in pts:
        L.append('  { %s, %s, %s, %d, "%s", "%s" },' % (fnum(la), fnum(lo), ty, ti, idv, info))
    L.append("};\n")
    L.append("static const ChartRwy CHART_RWYS[] = {")
    for la, lo, hd, ln, ti in rwys:
        L.append("  { %s, %s, %s, %s, %d }," % (fnum(la), fnum(lo), fnum(hd), fnum(ln), ti))
    L.append("};\n")
    L.append("static const ChartRing CHART_RINGS[] = {")
    for la, lo, rd, ty, ti in rings:
        L.append("  { %s, %s, %s, %s, %d }," % (fnum(la), fnum(lo), fnum(rd), ty, ti))
    L.append("};\n")
    L.append("static const ChartSeg CHART_SEGS[] = {")
    for a1, o1, a2, o2, ty, ti in segs:
        L.append("  { %s, %s, %s, %s, %s, %d }," % (fnum(a1), fnum(o1), fnum(a2), fnum(o2), ty, ti))
    L.append("};\n")
    L += arrs + ["", "static const ChartPoly CHART_POLYS[] = {"] + polys + ["};\n"]
    for nm, arr in (("PTS", "CHART_PTS"), ("RWYS", "CHART_RWYS"), ("RINGS", "CHART_RINGS"),
                    ("SEGS", "CHART_SEGS"), ("POLYS", "CHART_POLYS")):
        L.append("#define N_CHART_%-6s (int)(sizeof(%s)/sizeof(%s[0]))" % (nm, arr, arr))

    with open(OUTPUT, "w") as f: f.write("\n".join(L) + "\n")
    sys.stderr.write("wrote %s: %d pts, %d rwys, %d rings, %d segs, %d polys\n"
                     % (OUTPUT, len(pts), len(rwys), len(rings), len(segs), len(polys)))

if __name__ == "__main__":
    main()
