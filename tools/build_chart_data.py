#!/usr/bin/env python3
# ============================================================================
#  build_chart_data.py — generate chart_data.h from public internet datasets.
#
#  Builds a small PYRAMID of level-of-detail (LOD) datasets — one per zoom bucket
#  — instead of one national dataset filtered at runtime. Each LOD holds the
#  features appropriate for that zoom, simplified to a resolution matched to it:
#  coarse geometry far out (cheap, so lots of context can persist) and fine
#  geometry close in. The renderer just draws LOD_*[gMapLod] directly.
#
#  Sources (all public domain / open):
#    * OurAirports   — airports, runways, navaids, radio frequencies (CSV)
#    * Natural Earth — coast, ocean, lakes, state/country lines, rivers, roads,
#                      cities (GeoJSON, 1:50m / 1:10m)
#    * FAA           — Prohibited/Restricted special-use airspace (best effort)
#
#  Stdlib only. Downloads cached in tools/.cache/.  Run: python3 build_chart_data.py
# ============================================================================
import csv, json, math, os, sys, urllib.request, urllib.parse

HERE   = os.path.dirname(os.path.abspath(__file__))
CACHE  = os.path.join(HERE, ".cache")
OUTPUT = os.path.normpath(os.path.join(HERE, "..", "chart_data.h"))

OA = "https://davidmegginson.github.io/ourairports-data/"
NE = "https://raw.githubusercontent.com/nvkelso/natural-earth-vector/master/geojson/"

SOURCES = {
    "airports":  OA + "airports.csv",
    "runways":   OA + "runways.csv",
    "freqs":     OA + "airport-frequencies.csv",
    "navaids":   OA + "navaids.csv",
    "states":    NE + "ne_50m_admin_1_states_provinces_lines.geojson",
    "borders":   NE + "ne_50m_admin_0_boundary_lines_land.geojson",   # US-Canada / US-Mexico
    "coast":     NE + "ne_50m_coastline.geojson",
    "ocean":     NE + "ne_50m_ocean.geojson",                          # water polygons (fill)
    "lakes":     NE + "ne_50m_lakes.geojson",
    "rivers":    NE + "ne_10m_rivers_lake_centerlines.geojson",
    "roads":     NE + "ne_10m_roads.geojson",
    "cities":    NE + "ne_50m_populated_places_simple.geojson",
}

FAA_SUA = ("https://services6.arcgis.com/ssFJjBXIUyZDrSYZ/arcgis/rest/services/"
           "Special_Use_Airspace/FeatureServer/0/query?where=TYPE_CODE+IN+%28%27R%27%2C%27P%27%29"
           "&outFields=NAME,TYPE_CODE&outSR=4326&f=geojson&resultRecordCount=4000")

# ---- LOD pyramid -----------------------------------------------------------
#  One dataset per zoom band (map_zoom.cpp maps each range to one). Lower index =
#  zoomed further OUT = coarser geometry + fewer features. Detail is added a band at
#  a time per the requested progression. tol = Douglas-Peucker tolerance (deg).
#  apt = airports drawn as a symbol; runways/glides/rings = which of those get a
#  landing strip / approach path / distance circle; rivers = [(srMin, srMax, tol)].
LODS = [
    # 0: 2560 km (1382 NM)  — full zoom-out: ONLY capitals + borders (+ water)
    dict(apt=set(), runways=set(), glides=set(), rings=set(),
         city_max=99, caps_only=True, navaids=False, landmarks=False, restricted=False,
         roads={}, rivers=[],
         coast_tol=0.13, state_tol=0.13, fill_tol=0.13, lakes_sr=1),
    # 1: 1280 km (691 NM)  — + major rivers, major cities
    dict(apt=set(), runways=set(), glides=set(), rings=set(),
         city_max=2, navaids=False, landmarks=False, restricted=False,
         roads={}, rivers=[(0, 3, 0.04)],
         coast_tol=0.08, state_tol=0.08, fill_tol=0.08, lakes_sr=1),
    # 2: 640 km (352 NM)  — + major-airport locations, restricted airspace, more rivers
    dict(apt={"large"}, runways=set(), glides=set(), rings=set(),
         city_max=2, navaids=False, landmarks=False, restricted=True,
         roads={}, rivers=[(0, 4, 0.03)],
         coast_tol=0.055, state_tol=0.05, fill_tol=0.055, lakes_sr=2),
    # 3: 320 km (176 NM)  — + major-airport models (strips/glides/rings), interstates,
    #    more river + state detail
    dict(apt={"large"}, runways={"large"}, glides={"large"}, rings={"large"},
         city_max=3, navaids=False, landmarks=False, restricted=True,
         roads={"Interstate": 0.018}, rivers=[(0, 5, 0.022)],
         coast_tol=0.04, state_tol=0.035, fill_tol=0.04, lakes_sr=3),
    # 4: 160 km (88 NM)  — + medium airports, more roads, major landmarks, navaids
    dict(apt={"large", "medium"}, runways={"large", "medium"}, glides={"large", "medium"}, rings={"large"},
         city_max=4, navaids=True, landmarks=True, restricted=True,
         roads={"Interstate": 0.014, "Federal": 0.022}, rivers=[(0, 6, 0.017)],
         coast_tol=0.028, state_tol=0.025, fill_tol=0.028, lakes_sr=3),
    # 5: 80 km (44 NM)  — + decommissioned + SMALL airport locations, full road/river geom.
    #    (small dots appear here; their distance circles arrive at the next level, 40 km)
    dict(apt={"large", "medium", "small", "closed"}, runways={"large", "medium", "closed"},
         glides={"large", "medium"}, rings={"large", "medium"},
         city_max=5, navaids=True, landmarks=True, restricted=True,
         roads={"Interstate": 0.011, "Federal": 0.016, "State": 0.026}, rivers=[(0, 99, 0.013)],
         coast_tol=0.018, state_tol=0.016, fill_tol=0.018, lakes_sr=4),
    # 6: <=40 km (<=22 NM)  — full detail: small-airport distance circles + strips/glides
    dict(apt={"large", "medium", "small", "closed"}, runways={"large", "medium", "small", "closed"},
         glides={"large", "medium", "small"}, rings={"large", "medium", "small"},
         city_max=5, navaids=True, landmarks=True, restricted=True,
         roads={"Interstate": 0.008, "Federal": 0.012, "State": 0.02}, rivers=[(0, 99, 0.009)],
         coast_tol=0.012, state_tol=0.011, fill_tol=0.012, lakes_sr=5),
]
NLOD = len(LODS)

# Per-LOD spatial grid: cell size (deg) ~ that LOD's range, so the view box overlaps a
# few cells. The numerous point-type arrays (points/rings/runways) are sorted by grid
# cell and a cell->index table is emitted, so the renderer jumps straight to the visible
# cells (O(on-screen)) instead of scanning the whole latitude strip across the country.
GRID_DEG = [23.0, 11.5, 5.8, 2.9, 1.4, 0.72, 0.36]   # one per LOD (indices 0..6)
GRID_LA0, GRID_LO0 = 24.0, -125.0                    # CONUS grid origin (the point box min)
GRID_LASPAN, GRID_LOSPAN = 25.5, 58.5

def grid_sort(rows, deg):
    """Sort point-rows (lat = rows[i][0], lon = rows[i][1]) by row-major grid cell and
    return (sorted_rows, cellOff, NX, NY). cellOff[k] = first index with cell >= k, so
    cell (cy,cx) occupies [cellOff[cy*NX+cx], cellOff[cy*NX+cx+1])."""
    NX = max(1, int(math.ceil(GRID_LOSPAN / deg)))
    NY = max(1, int(math.ceil(GRID_LASPAN / deg)))
    def cell(r):
        cx = int((r[1] - GRID_LO0) / deg); cy = int((r[0] - GRID_LA0) / deg)
        cx = 0 if cx < 0 else NX - 1 if cx >= NX else cx
        cy = 0 if cy < 0 else NY - 1 if cy >= NY else cy
        return cy * NX + cx
    rows = sorted(rows, key=cell)
    cells = [cell(r) for r in rows]
    off, j = [], 0
    for k in range(NX * NY + 1):
        while j < len(rows) and cells[j] < k: j += 1
        off.append(j)
    return rows, off, NX, NY

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
    """VFR-visible theme parks (e.g. Kings Island) from OpenStreetMap. Best effort."""
    path = os.path.join(CACHE, "landmarks.json")
    if not os.path.exists(path):
        os.makedirs(CACHE, exist_ok=True)
        s, w, n, e = box[0], box[2], box[1], box[3]
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

def fetch_osm_roads(box):
    """Regional 'medium' roads (OSM secondary + tertiary) for a metro box — Natural
    Earth has only major highways, so these come from OpenStreetMap via Overpass.
    Cached in .cache/osm_roads.json. Returns [[(lon,lat),...], ...] or [] on failure."""
    path = os.path.join(CACHE, "osm_roads.json")
    if not os.path.exists(path):
        os.makedirs(CACHE, exist_ok=True)
        s, w, n, e = box[0], box[2], box[1], box[3]
        q = ('[out:json][timeout:300];'
             'way["highway"~"^(secondary|tertiary)$"](%f,%f,%f,%f);out geom;' % (s, w, n, e))
        url = "https://overpass-api.de/api/interpreter?data=" + urllib.parse.quote(q)
        try:
            sys.stderr.write("downloading osm_roads(overpass) ... "); sys.stderr.flush()
            req = urllib.request.Request(url, headers={"User-Agent": "instrumentpanel"})
            data = urllib.request.urlopen(req, timeout=360).read()
            with open(path, "wb") as f: f.write(data)
            sys.stderr.write("%d KB\n" % (len(data) // 1024))
        except Exception as ex:
            sys.stderr.write("FAILED (%s) — no medium roads\n" % ex)
            return []
    try:
        d = json.load(open(path))
    except Exception:
        return []
    lines = []
    for el in d.get("elements", []):
        if el.get("type") != "way": continue
        g = el.get("geometry")
        if g: lines.append([(p["lon"], p["lat"]) for p in g])
    return lines

def fetch_faa_sua():
    path = os.path.join(CACHE, "faa_sua.geojson")
    if not os.path.exists(path):
        os.makedirs(CACHE, exist_ok=True)
        try:
            sys.stderr.write("downloading faa_sua  ... "); sys.stderr.flush()
            req = urllib.request.Request(FAA_SUA, headers={"User-Agent": "instrumentpanel"})
            data = urllib.request.urlopen(req, timeout=180).read()
            with open(path, "wb") as f: f.write(data)
            sys.stderr.write("%d KB\n" % (len(data) // 1024))
        except Exception as ex:
            sys.stderr.write("FAILED (%s) — no restricted airspace\n" % ex)
            return []
    try:
        with open(path) as f: return json.load(f).get("features", [])
    except Exception:
        return []

def short_name(nm, maxlen=12):
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
    """Yield coordinate lists from line OR polygon geometry (polygon -> outline rings)."""
    if not geom: return
    t = geom.get("type")
    if   t == "LineString":      yield geom["coordinates"]
    elif t == "MultiLineString":
        for ls in geom["coordinates"]: yield ls
    elif t == "Polygon":
        for ring in geom["coordinates"]: yield ring
    elif t == "MultiPolygon":
        for poly in geom["coordinates"]:
            for ring in poly: yield ring

def in_box(lat, lon, box):
    return box[0] <= lat <= box[1] and box[2] <= lon <= box[3]

def clip_ring(ring, box):
    """Sutherland-Hodgman clip a ring [(lon,lat),...] to box (la0,la1,lo0,lo1).
    Returns a CLOSED clipped ring (box edges fill in where the ring left the box),
    so it stays fillable. Empty if nothing inside."""
    la0, la1, lo0, lo1 = box
    def clip(poly, keep, inter):
        out = []
        if not poly: return out
        for i in range(len(poly)):
            cur, prv = poly[i], poly[i - 1]
            kc, kp = keep(cur), keep(prv)
            if kc:
                if not kp: out.append(inter(prv, cur))
                out.append(cur)
            elif kp:
                out.append(inter(prv, cur))
        return out
    def ix_lon(a, b, x):
        t = (x - a[0]) / (b[0] - a[0]) if b[0] != a[0] else 0.0
        return (x, a[1] + t * (b[1] - a[1]))
    def ix_lat(a, b, y):
        t = (y - a[1]) / (b[1] - a[1]) if b[1] != a[1] else 0.0
        return (a[0] + t * (b[0] - a[0]), y)
    p = ring
    p = clip(p, lambda c: c[0] >= lo0, lambda a, b: ix_lon(a, b, lo0))
    p = clip(p, lambda c: c[0] <= lo1, lambda a, b: ix_lon(a, b, lo1))
    p = clip(p, lambda c: c[1] >= la0, lambda a, b: ix_lat(a, b, la0))
    p = clip(p, lambda c: c[1] <= la1, lambda a, b: ix_lat(a, b, la1))
    return p

# ---- emit helpers ----------------------------------------------------------
def fnum(x):
    s = ("%.4f" % x).rstrip("0")
    if s.endswith("."): s += "0"
    return s + "f"

def emit_floats(var, chunk):
    flat = ", ".join("%s,%s" % (fnum(la), fnum(lo)) for lo, la in chunk)
    return "static const float %s[] = { %s };" % (var, flat)

def build_polys(prefix, items, box, tol, ptype, maxpts=40):
    """items: iterable of (coords, label). Returns (float-array lines, ChartPoly rows).
    Long lines are split into <=maxpts chunks (1-pt overlap) with per-chunk bbox."""
    arrs, rows = [], []
    n = 0
    for coords, label in items:
        coords = [(c[0], c[1]) for c in coords if in_box(c[1], c[0], box)]
        if len(coords) < 2: continue
        coords = dp(coords, tol)
        for j in range(0, len(coords) - 1, maxpts - 1):
            ch = coords[j:j + maxpts]
            if len(ch) < 2: continue
            las = [la for lo, la in ch]; los = [lo for lo, la in ch]
            var = "%s_%d" % (prefix, n); n += 1
            arrs.append(emit_floats(var, ch))
            rows.append((min(las), '  { %s, %d, %s, "%s", %s, %s, %s, %s },' % (
                var, len(ch), ptype, label,
                fnum(min(las)), fnum(max(las)), fnum(min(los)), fnum(max(los)))))
    return arrs, rows                            # rows: (la0, row_str) for lat-sorting

def build_fills(prefix, features, box, tol):
    """Clip each polygon ring to the box, simplify, emit as closed ChartFill rings."""
    arrs, rows = [], []
    n = 0
    for ft in features:
        for ring in iter_lines(ft.get("geometry") if isinstance(ft, dict) else ft):
            ring = clip_ring([(c[0], c[1]) for c in ring], box)
            if len(ring) >= 2 and ring[0] == ring[-1]:
                ring = ring[:-1]          # drop closing dup: dp() needs first != last (else it collapses)
            if len(ring) < 3: continue
            ring = dp(ring, tol)
            if len(ring) < 3: continue
            las = [la for lo, la in ring]; los = [lo for lo, la in ring]
            var = "%s_%d" % (prefix, n); n += 1
            arrs.append(emit_floats(var, ring))
            rows.append('  { %s, %d, %s, %s, %s, %s },' % (
                var, len(ring), fnum(min(las)), fnum(max(las)), fnum(min(los)), fnum(max(los))))
    return arrs, rows

# ===========================================================================
def main():
    box  = (24.0, 49.5, -125.0, -66.5)         # CONUS (US point features)
    gbox = (13.0, 60.0, -130.0, -52.0)         # + Canada/Mexico outlines / ocean

    # ---- pre-extract every source once; LODs are filtered/simplified views of these.
    freq_by_apt = {}
    for r in load_csv("freqs"):
        t = r["type"].upper()
        if t in ("TWR", "CTAF", "UNICOM") and r["airport_ident"] not in freq_by_apt:
            freq_by_apt[r["airport_ident"]] = r["frequency_mhz"]

    CLS = {"large_airport": "large", "medium_airport": "medium",
           "small_airport": "small", "closed": "closed"}
    apts = []                                   # (la, lo, cls, sym, ident, freq, oaid)
    for r in load_csv("airports"):
        if r.get("iso_country") != "US": continue
        cls = CLS.get(r["type"])
        if cls is None: continue
        try: la, lo = float(r["latitude_deg"]), float(r["longitude_deg"])
        except ValueError: continue
        if not in_box(la, lo, box): continue
        ident = (r["iata_code"] or r["local_code"] or r["ident"])[:4]
        if cls == "closed":
            apts.append((la, lo, cls, "PT_AIRPORT_CLOSED", ident, "", r["ident"]))
        else:
            twr = r["scheduled_service"] == "yes" or cls == "large"
            apts.append((la, lo, cls, "PT_AIRPORT_TWR" if twr else "PT_AIRPORT_NTWR",
                         ident, freq_by_apt.get(r["ident"], ""), r["ident"]))
    rwy_by_apt = {}                             # oaid -> [(la, lo, hdg, len_m)]
    for r in load_csv("runways"):
        try:
            la, lo = float(r["le_latitude_deg"]), float(r["le_longitude_deg"])
            hdg, ln = float(r["le_heading_degT"]), float(r["length_ft"]) * 0.3048
        except (ValueError, KeyError): continue
        rwy_by_apt.setdefault(r["airport_ident"], []).append((la, lo, hdg, ln))

    navaids = []
    for r in load_csv("navaids"):
        if "VOR" not in r["type"].upper(): continue
        try: la, lo = float(r["latitude_deg"]), float(r["longitude_deg"])
        except ValueError: continue
        if not in_box(la, lo, box): continue
        khz = r.get("frequency_khz", "")
        try: info = "%.2f" % (float(khz) / 1000.0) if khz else ""
        except ValueError: info = ""
        navaids.append((la, lo, r["ident"][:4], info))

    cities = []                                 # (la, lo, name, scalerank, is_capital)
    for ft in load_geojson("cities")["features"]:
        pr = ft["properties"]; g = ft["geometry"]
        if g["type"] != "Point": continue
        lo, la = g["coordinates"][:2]
        if not in_box(la, lo, box): continue
        if pr.get("adm0name") not in (None, "United States of America"): continue
        cap = pr.get("featurecla", "") in ("Admin-0 capital", "Admin-1 capital")  # state/nat'l capital
        sr = pr.get("scalerank"); sr = 9 if sr is None else sr
        if sr > 5 and not cap: continue          # keep capitals regardless of size
        name = (pr.get("name") or "").encode("ascii", "ignore").decode()[:6].upper()
        cities.append((la, lo, name, sr, cap))

    landmarks = [(39.3447, -84.2686, "KINGS ISLAND")]
    ov = fetch_overpass(box)
    if ov:
        for el in ov.get("elements", []):
            if el.get("type") != "way": continue
            c = el.get("center") or {}
            la, lo = c.get("lat"), c.get("lon")
            nm = (el.get("tags") or {}).get("name")
            if la is None or lo is None or not nm or not in_box(la, lo, box): continue
            landmarks.append((la, lo, short_name(nm)))
    lm, placed = [], []
    for la, lo, nm in landmarks:
        if any(abs(la - a) < 0.02 and abs(lo - o) < 0.02 for a, o, _ in placed): continue
        placed.append((la, lo, nm)); lm.append((la, lo, nm))
    landmarks = lm

    rivers_f = load_geojson("rivers")["features"]
    roads_f  = load_geojson("roads")["features"]
    coast_f  = load_geojson("coast")["features"]
    border_f = load_geojson("borders")["features"]
    state_f  = load_geojson("states")["features"]
    lakes_f  = load_geojson("lakes")["features"]
    ocean_f  = load_geojson("ocean")["features"]
    sua_f    = fetch_faa_sua()

    def riv_lines(lo_r, hi_r):
        for ft in rivers_f:
            sr = ft["properties"].get("scalerank"); sr = 99 if sr is None else sr
            if lo_r <= sr <= hi_r:
                for l in iter_lines(ft["geometry"]): yield (l, "")

    def road_lines(level):
        for ft in roads_f:
            pr = ft["properties"]
            if pr.get("sov_a3") != "USA": continue
            nm0 = (pr.get("name") or "")
            lvl = "Interstate" if (pr.get("level") == "Interstate" or nm0.startswith("I-")) else pr.get("level")
            if lvl != level: continue
            label = nm0.replace(" ", "") if level == "Interstate" else ""
            for l in iter_lines(ft["geometry"]): yield (l, label)

    RING = {"large": (15000.0, "RG_APT_LARGE"), "medium": (9000.0, "RG_APT_MEDIUM"),
            "small": (5000.0, "RG_APT_SMALL")}

    lods = []
    for i, cfg in enumerate(LODS):
        pts, rwys, rings, segs = [], [], [], []
        for (la, lo, cls, sym, ident, freq, oaid) in apts:
            if cls not in cfg["apt"]: continue
            pts.append((la, lo, sym, ident, freq))
            if cls in cfg["rings"] and cls in RING:
                rad, rty = RING[cls]; rings.append((la, lo, rad, rty))
            for (rla, rlo, hdg, ln) in rwy_by_apt.get(oaid, []):
                if cls in cfg["runways"]:
                    rwys.append((rla, rlo, hdg, ln))
                if cls in cfg["glides"] and ln >= 1200:
                    hr = math.radians(hdg); cl = math.cos(math.radians(rla)) or 1e-3; apr = 14816.0
                    a0 = (rla + (-apr * math.cos(hr)) / 111320.0, rlo + (-apr * math.sin(hr)) / (111320.0 * cl))
                    a1 = (rla + ((ln + apr) * math.cos(hr)) / 111320.0, rlo + ((ln + apr) * math.sin(hr)) / (111320.0 * cl))
                    segs.append((a0[0], a0[1], a1[0], a1[1], "SG_GLIDEPATH"))
        if cfg["navaids"]:
            for (la, lo, ident, info) in navaids:
                pts.append((la, lo, "PT_NAVAID", ident, info))
        for (la, lo, name, sr, cap) in cities:
            if cfg.get("caps_only"):
                if not cap: continue             # full zoom-out: capitals only
            elif not cap and sr > cfg["city_max"]:
                continue                         # else: capitals always + cities up to city_max
            pts.append((la, lo, "PT_CITY", name, ""))
        if cfg["landmarks"]:
            for (la, lo, nm) in landmarks:
                pts.append((la, lo, "PT_LANDMARK", nm, ""))

        P = "L%d" % i
        poly_arrs, poly_rows = [], []
        def add_poly(suffix, items, tol, ptype):
            a, r = build_polys("%s%s" % (P, suffix), items, gbox if ptype in ("PLY_BORDER",) else box, tol, ptype)
            poly_arrs.extend(a); poly_rows.extend(r)
        # white shore + country borders, grey state lines, red restricted (tier-0 always)
        add_poly("CO", ((l, "") for ft in coast_f  for l in iter_lines(ft["geometry"])), cfg["coast_tol"], "PLY_BORDER")
        add_poly("CT", ((l, "") for ft in border_f for l in iter_lines(ft["geometry"])), cfg["coast_tol"], "PLY_BORDER")
        add_poly("ST", ((l, "") for ft in state_f  for l in iter_lines(ft["geometry"])), cfg["state_tol"], "PLY_STATE")
        if cfg.get("restricted", True):
            add_poly("SU", ((l, "") for ft in sua_f for l in iter_lines(ft.get("geometry"))), max(cfg["coast_tol"], 0.02), "PLY_RESTRICTED")
        for k, (lo_r, hi_r, tol) in enumerate(cfg["rivers"]):
            add_poly("RV%d" % k, riv_lines(lo_r, hi_r), tol, "PLY_RIVER")
        for level, tol in cfg["roads"].items():
            add_poly("RD%s" % level[0], road_lines(level), tol, "PLY_INTERSTATE")

        # Ocean fill stays coarse at every LOD (big continent rings = the per-frame cost):
        # the crisp shore is the fine white coastline drawn on top, so the blue fill under
        # it only needs to be roughly right.
        ocean_arrs, ocean_rows = build_fills("%sOC" % P, ocean_f, gbox, max(cfg["fill_tol"], 0.05))
        def lake_sr(ft):
            sr = ft["properties"].get("scalerank")
            return 99 if sr is None else sr            # NB: scalerank 0 (Great Lakes) is falsy
        lk = [ft for ft in lakes_f if lake_sr(ft) <= cfg["lakes_sr"]]
        lake_arrs, lake_rows = build_fills("%sLK" % P, lk, gbox, cfg["fill_tol"])

        deg = GRID_DEG[i]                         # numerous point-type arrays -> 2-D grid;
        pts,   pts_off,   gnx, gny = grid_sort(pts,   deg)   # segs/polys stay on the lat-band
        rings, rings_off, _,   _   = grid_sort(rings, deg)
        rwys,  rwys_off,  _,   _   = grid_sort(rwys,  deg)
        segs.sort(key=lambda s: s[0])
        poly_rows.sort(key=lambda t: t[0])       # by la0 -> renderer lat-bands polylines
        lods.append(dict(pts=pts, rwys=rwys, rings=rings, segs=segs,
                         pts_off=pts_off, rings_off=rings_off, rwys_off=rwys_off,
                         gnx=gnx, gny=gny, gdeg=deg,
                         poly_arrs=poly_arrs, polys=[r for _, r in poly_rows],
                         ocean_arrs=ocean_arrs, ocean=ocean_rows,
                         lake_arrs=lake_arrs, lake=lake_rows))
        sys.stderr.write("  LOD %d: %d pts, %d rwys, %d rings, %d segs, %d polys, %d ocean, %d lake\n"
                         % (i, len(pts), len(rwys), len(rings), len(segs),
                            len(poly_rows), len(ocean_rows), len(lake_rows)))

    # ---- regional "medium" roads (OSM secondary/tertiary) for the Cincinnati metro,
    #      drawn only at <= 20 km (a separate close-in layer, NOT part of any LOD).
    CINCY = (39.10, -84.51); rkm = 60.0
    dla = rkm / 111.0; dlo = dla / max(0.2, math.cos(math.radians(CINCY[0])))
    mbox = (CINCY[0] - dla, CINCY[0] + dla, CINCY[1] - dlo, CINCY[1] + dlo)
    med_arrs, med_rows = build_polys("MED", ((l, "") for l in fetch_osm_roads(mbox)), mbox, 0.0012, "PLY_HIGHWAY")
    med_rows.sort(key=lambda t: t[0])
    sys.stderr.write("  MEDROADS (Cincinnati): %d chunks\n" % len(med_rows))
    write_header(lods, med_arrs, [r for _, r in med_rows])

# ---------------------------------------------------------------------------
def write_header(lods, med_arrs, med_rows):
    L = ["#pragma once", "#include <Arduino.h>", ""]
    L.append("// AUTO-GENERATED by tools/build_chart_data.py from OurAirports + Natural Earth + FAA.")
    L.append("// Per-LOD pyramid: LOD_*[lod] arrays (lod = gMapLod, see map_zoom.h). Each LOD is its")
    L.append("// own dataset (features + resolution matched to that zoom). Point/ring/rwy/seg arrays")
    L.append("// are lat-sorted for the binary-search lat-band cull (chart_cull.h). OCEAN/LAKE are")
    L.append("// closed water rings: even-odd scanline fill (blue); coast/country borders draw white.\n")
    L.append("enum ChartPtType   : uint8_t { PT_AIRPORT_TWR, PT_AIRPORT_NTWR, PT_NAVAID, PT_LANDMARK, PT_AIRPORT_CLOSED, PT_CITY };")
    L.append("enum ChartSegType  : uint8_t { SG_RIVER, SG_GLIDEPATH, SG_COAST };")
    L.append("enum ChartRingType : uint8_t { RG_APT_LARGE, RG_APT_MEDIUM, RG_APT_SMALL, RG_APT_CLOSED, RG_RESTRICTED };")
    L.append("enum ChartPolyType : uint8_t { PLY_BORDER, PLY_STATE, PLY_INTERSTATE, PLY_HIGHWAY, PLY_RIVER, PLY_LAKE, PLY_RESTRICTED };")
    L.append("struct ChartPt   { float lat, lon; uint8_t type; const char *id, *info; };")
    L.append("struct ChartRwy  { float lat, lon; float hdg, len_m; };")
    L.append("struct ChartRing { float lat, lon; float rad_m; uint8_t type; };")
    L.append("struct ChartSeg  { float lat1, lon1, lat2, lon2; uint8_t type; };")
    L.append("struct ChartPoly { const float *pts; uint16_t n; uint8_t type; const char *name; float la0, la1, lo0, lo1; };")
    L.append("struct ChartFill { const float *pts; uint16_t n; float la0, la1, lo0, lo1; };")
    L.append("struct MapProj   { float clat, clon, cosLat, cosH, sinH, scale; int cx, cy; };")
    L.append("#define MAP_NLOD %d\n" % NLOD)
    L.append("static const float LOD_DUMMYF[] = { 0.0f, 0.0f };\n")

    # per-LOD typed arrays (with a dummy element when empty so the array isn't 0-size)
    def emit(ctype, var, rows, dummy):
        if rows:
            L.append("static const %s %s[] = {" % (ctype, var)); L.extend(rows); L.append("};")
            return len(rows)
        L.append("static const %s %s[] = { %s };" % (ctype, var, dummy))
        return 0

    counts = {k: [] for k in ("PTS", "RWYS", "RINGS", "SEGS", "POLYS", "OCEAN", "LAKE")}
    grid_nx, grid_ny, grid_deg = [], [], []                # per-LOD flat-grid parameters

    def emit_cells(var, off):                              # cellOff: NX*NY+1 prefix offsets
        L.append("static const uint16_t %s[] = { %s };" % (var, ", ".join(str(o) for o in off)))

    for i, d in enumerate(lods):
        ptrows = ['  { %s, %s, %s, "%s", "%s" },' % (fnum(la), fnum(lo), ty, idv, info)
                  for (la, lo, ty, idv, info) in d["pts"]]
        rwrows = ['  { %s, %s, %s, %s },' % (fnum(la), fnum(lo), fnum(hd), fnum(ln))
                  for (la, lo, hd, ln) in d["rwys"]]
        rgrows = ['  { %s, %s, %s, %s },' % (fnum(la), fnum(lo), fnum(rd), ty)
                  for (la, lo, rd, ty) in d["rings"]]
        sgrows = ['  { %s, %s, %s, %s, %s },' % (fnum(a1), fnum(o1), fnum(a2), fnum(o2), ty)
                  for (a1, o1, a2, o2, ty) in d["segs"]]
        L.extend(d["poly_arrs"]); L.extend(d["ocean_arrs"]); L.extend(d["lake_arrs"])
        counts["PTS"].append(  emit("ChartPt",   "L%d_PTS"   % i, ptrows,      '{ 0.0f,0.0f,0,"","" }'))
        counts["RWYS"].append( emit("ChartRwy",  "L%d_RWYS"  % i, rwrows,      '{ 0.0f,0.0f,0.0f,0.0f }'))
        counts["RINGS"].append(emit("ChartRing", "L%d_RINGS" % i, rgrows,      '{ 0.0f,0.0f,0.0f,0 }'))
        counts["SEGS"].append( emit("ChartSeg",  "L%d_SEGS"  % i, sgrows,      '{ 0.0f,0.0f,0.0f,0.0f,0 }'))
        counts["POLYS"].append(emit("ChartPoly", "L%d_POLYS" % i, d["polys"], '{ LOD_DUMMYF,0,0,"",0.0f,0.0f,0.0f,0.0f }'))
        counts["OCEAN"].append(emit("ChartFill", "L%d_OCEAN" % i, d["ocean"], '{ LOD_DUMMYF,0,0.0f,0.0f,0.0f,0.0f }'))
        counts["LAKE"].append( emit("ChartFill", "L%d_LAKE"  % i, d["lake"],  '{ LOD_DUMMYF,0,0.0f,0.0f,0.0f,0.0f }'))
        emit_cells("L%d_PTS_CELL"   % i, d["pts_off"])     # flat-grid cell index for the
        emit_cells("L%d_RINGS_CELL" % i, d["rings_off"])   # numerous point-type arrays
        emit_cells("L%d_RWYS_CELL"  % i, d["rwys_off"])
        grid_nx.append(d["gnx"]); grid_ny.append(d["gny"]); grid_deg.append(d["gdeg"])
        L.append("")

    # pointer + count tables indexed by gMapLod
    def table(ctype, base):
        L.append("static const %s* const LOD_%s[MAP_NLOD] = { %s };"
                 % (ctype, base, ", ".join("L%d_%s" % (i, base) for i in range(NLOD))))
        L.append("static const uint16_t LOD_N%s[MAP_NLOD] = { %s };"
                 % (base, ", ".join(str(c) for c in counts[base])))
    table("ChartPt",   "PTS")
    table("ChartRwy",  "RWYS")
    table("ChartRing", "RINGS")
    table("ChartSeg",  "SEGS")
    table("ChartPoly", "POLYS")
    table("ChartFill", "OCEAN")
    table("ChartFill", "LAKE")

    # flat per-LOD grid: cellOff pointer tables + grid params (chart_cull.h CHART_GRID)
    L.append("")
    for base in ("PTS", "RINGS", "RWYS"):
        L.append("static const uint16_t* const LOD_%s_CELL[MAP_NLOD] = { %s };"
                 % (base, ", ".join("L%d_%s_CELL" % (i, base) for i in range(NLOD))))
    L.append("static const uint16_t LOD_GRID_NX[MAP_NLOD] = { %s };" % ", ".join(str(x) for x in grid_nx))
    L.append("static const uint16_t LOD_GRID_NY[MAP_NLOD] = { %s };" % ", ".join(str(y) for y in grid_ny))
    L.append("static const float    LOD_GRID_DEG[MAP_NLOD] = { %s };" % ", ".join(fnum(g) for g in grid_deg))
    L.append("#define GRID_LA0 %sf" % repr(GRID_LA0))
    L.append("#define GRID_LO0 %sf" % repr(GRID_LO0))

    # regional medium roads (single array, drawn only at <= 20 km — see instrument_drawer.ino)
    L.append("")
    L.extend(med_arrs)
    L.append("static const ChartPoly MEDROADS[] = {")
    L.extend(med_rows if med_rows else ['  { LOD_DUMMYF, 0, PLY_HIGHWAY, "", 0.0f, 0.0f, 0.0f, 0.0f },'])
    L.append("};")
    L.append("#define N_MEDROADS %d" % len(med_rows))

    with open(OUTPUT, "w") as f: f.write("\n".join(L) + "\n")
    tot = sum(len(d["pts"]) for d in lods)
    sys.stderr.write("wrote %s: %d LODs, %d total pts\n" % (OUTPUT, NLOD, tot))

if __name__ == "__main__":
    main()
