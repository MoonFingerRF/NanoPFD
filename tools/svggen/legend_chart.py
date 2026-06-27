#!/usr/bin/env python3
# Regenerates legend_chart.h — the map used ONLY by the README ND legend.
#
# Unlike the device's chart_data.h (which is centred on the build location), the
# legend uses a dense, feature-complete metro so every leader line has a real,
# visible target. Philadelphia is ideal: the Delaware River IS the PA/NJ state
# line, so a river + a state line + the towered/non-towered fields, navaids, the
# city, interstates and closed airfields all fall inside one 30 km view.
#
# It's a real OurAirports + Natural Earth chart, built with the same generator as
# the device chart. build.sh does NOT run this (it uses the committed
# legend_chart.h); run it by hand to refresh the legend map.
import os, shutil, subprocess

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
CHART = os.path.join(ROOT, "chart_data.h")
LAT, LON = 39.95, -75.15          # Philadelphia

bak = CHART + ".legendbak"
shutil.copy(CHART, bak)            # build_chart_data.py overwrites chart_data.h
try:
    subprocess.run(["python3", os.path.join(ROOT, "tools", "build_chart_data.py"),
                    "--lat", str(LAT), "--lon", str(LON),
                    "--radius-km", "55", "--detail-km", "55", "--max-airports", "400"], check=True)
    shutil.copy(CHART, os.path.join(HERE, "legend_chart.h"))
    print("wrote legend_chart.h (NYC)")
finally:
    shutil.move(bak, CHART)        # restore the device chart
