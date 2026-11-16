#!/usr/bin/env python3
"""LTC node carrier board: XIAO ESP32C3 + output stage + mic pass-through +
JST-PH battery + power switch. Generates JLCPCB-ready fabrication files.

    pip install shapely matplotlib gerbonara
    python3 hardware/pcb/ltc_node_pcb.py

Outputs (hardware/pcb/out/):
    gerber/                     Gerber RS-274X + Excellon drill files
    ltc-node-pcb-gerber.zip     upload this to JLCPCB
    bom_jlcpcb.csv, cpl_jlcpcb.csv   SMT assembly (JLCPCB format)
    bom_handsolder.csv          parts you solder yourself
    preview_top.png, preview_bottom.png
    drc_report.txt              clearance + connectivity check results

Coordinates: mm, origin at the bottom-left board corner, Y up, viewed from
the top. Footprint data comes from the KiCad library (y-down) and Seeed's
official XIAO footprint; it is converted to y-up without mirroring.
"""

import csv
import math
import os
import zipfile
from dataclasses import dataclass, field
from pathlib import Path

from shapely.geometry import LineString, MultiPolygon, Point, Polygon, box
from shapely.ops import unary_union
from shapely import affinity

HERE = Path(__file__).resolve().parent
OUT = HERE / "out"
GERBER = OUT / "gerber"

BOARD_W, BOARD_H, CORNER_R = 50.0, 34.0, 2.0
CLEARANCE = 0.2  # min copper-copper between nets (JLC allows 0.127)
POUR_CLEARANCE = 0.3
SIGNAL_W, POWER_W = 0.25, 0.4
VIA_DRILL, VIA_PAD = 0.3, 0.6
MASK_EXP = 0.05
REVISION = "v0.4"


# ---------------------------------------------------------------------------
# data model
# ---------------------------------------------------------------------------
@dataclass
class Pad:
    ref: str
    name: str
    x: float
    y: float
    w: float
    h: float
    shape: str  # rect | roundrect | oval | circle
    net: str | None
    tht: bool = False
    drill: float | None = None  # round hole
    slot: tuple | None = None  # (length along y or x, width, 'v'|'h')
    paste: bool = True

    def geom(self, grow=0.0):
        w, h = self.w + 2 * grow, self.h + 2 * grow
        if self.shape == "circle":
            return Point(self.x, self.y).buffer(w / 2, quad_segs=16)
        if self.shape == "oval":
            r = min(w, h) / 2
            if w >= h:
                return LineString([(self.x - w / 2 + r, self.y), (self.x + w / 2 - r, self.y)]).buffer(r, quad_segs=16)
            return LineString([(self.x, self.y - h / 2 + r), (self.x, self.y + h / 2 - r)]).buffer(r, quad_segs=16)
        if self.shape == "roundrect":
            r = 0.25 * min(w, h)
            return box(self.x - w / 2 + r, self.y - h / 2 + r, self.x + w / 2 - r, self.y + h / 2 - r).buffer(r, quad_segs=8)
        return box(self.x - w / 2, self.y - h / 2, self.x + w / 2, self.y + h / 2)


@dataclass
class Part:
    ref: str
    value: str
    footprint: str
    x: float
    y: float
    rot: float
    lcsc: str = ""
    mpn: str = ""
    smt: bool = True  # assembled by JLCPCB
    pads: list = field(default_factory=list)


@dataclass
class Track:
    net: str
    pts: list
    width: float
    layer: str  # top | bottom


@dataclass
class Via:
    net: str
    x: float
    y: float


parts: list[Part] = []
tracks: list[Track] = []
vias: list[Via] = []
mount_holes = [(2.5, 2.0), (2.5, 32.0), (47.5, 2.0), (47.5, 32.0)]
MOUNT_DRILL = 2.2


def place(ref, value, fp, x, y, rot, pads_local, nets, **kw):
    """pads_local: list of (name, lx, ly_kicad, w, h, shape, tht, drill, slot, paste).
    KiCad y-down local coords are converted to y-up, then rotated by rot (deg CCW)."""
    p = Part(ref, value, fp, x, y, rot, **kw)
    c, s = math.cos(math.radians(rot)), math.sin(math.radians(rot))
    for name, lx, lyk, w, h, shape, tht, drill, slot, paste in pads_local:
        ly = -lyk
        gx, gy = x + lx * c - ly * s, y + lx * s + ly * c
        if abs(round(rot) % 180) == 90:
            w, h = h, w
        sl = None
        if slot:
            length, width, axis = slot  # axis in local frame
            if abs(round(rot) % 180) == 90:
                axis = "v" if axis == "h" else "h"
            sl = (length, width, axis)
        p.pads.append(Pad(ref, name, round(gx, 4), round(gy, 4), w, h, shape, nets.get(name), tht, drill, sl, paste))
    parts.append(p)
    return p


def r0603(ref, value, x, y, rot, n1, n2, lcsc, kind="R"):
    fp = {"R": "R_0603_1608Metric", "C": "C_0603_1608Metric", "D": "LED_0603_1608Metric"}[kind]
    w, h = (0.8, 0.95) if kind != "D" else (0.8, 0.95)
    pads = [("1", -0.825, 0, w, h, "roundrect", False, None, None, True), ("2", 0.825, 0, w, h, "roundrect", False, None, None, True)]
    return place(ref, value, fp, x, y, rot, pads, {"1": n1, "2": n2}, lcsc=lcsc)


def track(net, pts, width=SIGNAL_W, layer="top"):
    tracks.append(Track(net, pts, width, layer))


def via(net, x, y):
    vias.append(Via(net, x, y))


# ---------------------------------------------------------------------------
# footprints
# ---------------------------------------------------------------------------
# Seeed XIAO ESP32-C3 SMD (castellated) - Seeed OPL_Kicad_Library XIAO-ESP32-C3-SMD
XIAO_PADS = []
for i in range(7):
    XIAO_PADS.append((str(1 + i), 0.835, -18.12 + 2.54 * i, 2.75, 2.0, "roundrect", False, None, None, False))
for i in range(7):
    XIAO_PADS.append((str(8 + i), 17.0, -2.88 - 2.54 * i, 2.75, 2.0, "roundrect", False, None, None, False))
# The module's BAT+/BAT- pads sit underneath it and cannot be reached with an
# iron. Plated through-holes in the carrier pads let you solder them from the
# back (Seeed forum thread 267939: works when the hole is kept small; an oversize
# 1.75 x 0.75 mm slot let the solder run past the pad). 1.0 mm drill, 0.3 mm ring.
XIAO_PADS += [
    ("21", 4.445, -10.882, 2.5, 1.6, "roundrect", True, 1.0, None, False),  # VBAT (BAT+ pad under the module)
    ("22", 4.445, -12.787, 2.5, 1.6, "roundrect", True, 1.0, None, False),  # GND (BAT- pad)
]
# CUI SJ1-3533NG (KiCad Connector_Audio:Jack_3.5mm_CUI_SJ1-3533NG_Horizontal)
JACK_PADS = [
    ("S", 0.0, 0.0, 2.8, 1.8, "oval", True, None, (2.0, 1.0, "h"), False),
    ("T", 2.0, 2.4, 2.8, 1.8, "oval", True, None, (2.0, 1.0, "h"), False),
    ("R", 2.0, 7.9, 2.8, 1.8, "oval", True, None, (2.0, 1.0, "h"), False),
]
# JST PH B2B-PH-K-S vertical (KiCad Connector_JST:JST_PH_B2B-PH-K_1x02_P2.00mm_Vertical)
JST_PADS = [
    ("1", 0.0, 0.0, 1.2, 1.75, "roundrect", True, 0.75, None, False),
    ("2", 2.0, 0.0, 1.2, 1.75, "oval", True, 0.75, None, False),
]
# C&K OS102011MS2QN1 (KiCad Button_Switch_THT:SW_Slide_1P2T_CK_OS102011MS2Q)
SW_PADS = [
    ("1", 0.0, 0.0, 1.5, 2.5, "rect", True, 0.8, None, False),
    ("2", 2.0, 0.0, 1.5, 2.5, "oval", True, 0.8, None, False),
    ("3", 4.0, 0.0, 1.5, 2.5, "oval", True, 0.8, None, False),
    ("M1", -2.1, 0.0, 2.2, 3.5, "oval", True, 1.5, None, False),
    ("M2", 6.1, 0.0, 2.2, 3.5, "oval", True, 1.5, None, False),
]

# ---------------------------------------------------------------------------
# placement + nets
# ---------------------------------------------------------------------------
# XIAO: USB-C towards the right board edge. Rotation -90: local +y (USB) -> +x.
# Module occupies x 29..50, y 8.1..25.9.
xiao_nets = {
    "2": "BSENSE",      # D1  GPIO3  battery voltage (ADC1)
    "4": "LTC_MIC",     # D3  GPIO5  LTC mic tap
    "5": "LTC_LINE",    # D4  GPIO6  LTC line tap
    "8": "LED_DRV",     # D7  GPIO20 locked LED
    "11": "VBUS_SENSE", # D10 GPIO10 USB power present
    "13": "GND",
    "14": "VBUS",       # 5 V from USB-C
    "21": "VBAT",
    "22": "GND",
}
place("U1", "XIAO ESP32C3", "XIAO-ESP32-C3-SMD", 29.0, 25.9, -90, XIAO_PADS, xiao_nets,
      mpn="Seeed Studio XIAO ESP32C3", smt=False)
# jacks: plug enters from the left edge (rotation 90)
place("J1", "OUT to camera", "Jack_3.5mm_CUI_SJ1-3533NG", 1.2, 26.0, 90, JACK_PADS,
      {"S": "GND", "T": "TIP_OUT", "R": "RING"}, lcsc="C4992459", mpn="CUI SJ1-3533NG", smt=False)
place("J2", "IN from mic", "Jack_3.5mm_CUI_SJ1-3533NG", 1.2, 9.0, 90, JACK_PADS,
      {"S": "GND", "T": None, "R": "RING"}, lcsc="C4992459", mpn="CUI SJ1-3533NG", smt=False)
# battery connector, housing towards the board interior
place("J3", "Battery JST-PH", "JST_PH_B2B-PH-K_1x02_P2.00mm_Vertical", 18.7, 3.5, 180, JST_PADS,
      {"1": "BAT+", "2": "GND"}, lcsc="C131337", mpn="JST B2B-PH-K-S(LF)(SN)", smt=False)
place("SW1", "Power", "SW_Slide_1P2T_CK_OS102011MS2Q", 25.0, 3.0, 0, SW_PADS,
      {"1": "BAT+", "2": "VBAT", "3": None, "M1": None, "M2": None}, mpn="C&K OS102011MS2QN1", smt=False)

# output stage (docs/hardware.md): R1 mic tap, R2 line tap, R3 shunt, C2 edge, C1 DC block
r0603("R1", "220k", 24.0, 29.4, 0, "NODE", "LTC_MIC", "C22961")
r0603("R2", "4.7k", 24.0, 27.8, 0, "NODE", "LTC_LINE", "C23162")
r0603("R3", "1k", 20.6, 25.0, 0, "GND", "NODE", "C21190")
r0603("C2", "22nF", 20.6, 23.4, 0, "GND", "NODE", "C77571", kind="C")
r0603("C1", "1uF", 20.6, 21.8, 0, "TIP_OUT", "NODE", "C15849", kind="C")
# battery divider (VBAT/2 -> GPIO3)
r0603("R8", "220k", 18.5, 31.0, 0, "VBAT", "BSENSE", "C22961")
r0603("R9", "220k", 21.8, 32.6, 0, "BSENSE", "GND", "C22961")
# USB power sense (5V/2 -> GPIO10)
r0603("R4", "100k", 45.0, 5.0, 0, "VBUS_SENSE", "VBUS", "C25803")
r0603("R5", "100k", 41.5, 3.1, 90, "GND", "VBUS_SENSE", "C25803")
# LED (GPIO20 -> 330R -> LED -> GND); KiCad LED pad 1 = cathode
r0603("R7", "330", 35.0, 5.2, 0, "LED_DRV", "LED_A", "C23138")
r0603("D1", "LED red", 37.6, 5.2, 180, "GND", "LED_A", "C2286", kind="D")

# ---------------------------------------------------------------------------
# routing
# ---------------------------------------------------------------------------
# LTC taps
track("LTC_MIC", [(39.5, 26.44), (39.5, 29.4), (24.825, 29.4)])
track("LTC_LINE", [(36.96, 26.44), (36.96, 27.8), (24.825, 27.8)])
# node bus
track("NODE", [(22.4, 21.8), (22.4, 29.4)])
for y, x in ((29.4, 23.175), (27.8, 23.175), (25.0, 21.425), (23.4, 21.425), (21.8, 21.425)):
    track("NODE", [(x, y), (22.4, y)])
# shunt ground
track("GND", [(19.775, 25.0), (19.775, 23.4)])
track("GND", [(19.775, 24.2), (18.6, 24.2)])
via("GND", 18.6, 24.2)
# tip to J1
track("TIP_OUT", [(19.775, 21.8), (3.6, 21.8), (3.6, 26.6)])
# ring pass-through on the bottom layer
track("RING", [(9.1, 11.0), (9.1, 28.0)], layer="bottom")
# battery: J3.1 -> SW1.1 ; SW1.2 -> XIAO VBAT pad (under the module)
track("BAT+", [(18.7, 3.5), (18.7, 0.7), (25.0, 0.7), (25.0, 3.0)], POWER_W)
track("VBAT", [(27.0, 3.0), (27.0, 6.4), (28.3, 6.4), (28.3, 21.455), (39.882, 21.455)], POWER_W)
# VBAT branch to the battery divider via the bottom layer
track("VBAT", [(27.0, 6.4), (16.5, 6.4)])
via("VBAT", 16.5, 6.4)
track("VBAT", [(16.5, 6.4), (16.5, 31.0)], layer="bottom")
via("VBAT", 16.5, 31.0)
track("VBAT", [(16.5, 31.0), (17.675, 31.0)])
# battery sense
track("BSENSE", [(44.58, 26.44), (44.58, 31.0), (19.325, 31.0)])
track("BSENSE", [(20.975, 32.6), (20.975, 31.0)])
track("GND", [(22.625, 32.6), (23.8, 32.6)])
via("GND", 23.8, 32.6)
# XIAO ground pad 22 -> via
track("GND", [(41.787, 21.455), (41.787, 19.6), (40.8, 19.0)])
via("GND", 40.8, 19.0)
track("GND", [(44.58, 8.9), (44.58, 6.6)])
via("GND", 44.58, 6.6)
# USB power sense
track("VBUS", [(47.12, 8.9), (47.12, 5.0), (45.825, 5.0)])
track("VBUS_SENSE", [(44.175, 5.0), (39.5, 5.0), (39.5, 8.9)])
track("VBUS_SENSE", [(41.5, 5.0), (41.5, 3.925)])
track("GND", [(41.5, 2.275), (41.5, 1.0)])
via("GND", 41.5, 1.0)
# LED
track("LED_DRV", [(31.88, 8.9), (31.88, 5.2), (34.175, 5.2)])
track("LED_A", [(35.825, 5.2), (36.775, 5.2)])
track("GND", [(38.425, 5.2), (38.425, 3.6)])
via("GND", 38.425, 3.6)

# ---------------------------------------------------------------------------
# geometry
# ---------------------------------------------------------------------------
def rounded_board():
    return box(CORNER_R, CORNER_R, BOARD_W - CORNER_R, BOARD_H - CORNER_R).buffer(CORNER_R, quad_segs=16)


BOARD = rounded_board()
ALL_PADS = [p for part in parts for p in part.pads]


def pad_layers(p):
    return ("top", "bottom") if p.tht else ("top",)


def features(layer):
    """(net, geometry) for every copper feature on a layer except pours."""
    out = []
    for p in ALL_PADS:
        if layer in pad_layers(p):
            out.append((p.net, p.geom()))
    for t in tracks:
        if t.layer == layer:
            out.append((t.net, LineString(t.pts).buffer(t.width / 2, quad_segs=8)))
    for v in vias:
        out.append((v.net, Point(v.x, v.y).buffer(VIA_PAD / 2, quad_segs=16)))
    return out


def holes():
    """(net, geometry, kind) for all drilled holes."""
    out = []
    for p in ALL_PADS:
        if p.drill:
            out.append((p.net, Point(p.x, p.y).buffer(p.drill / 2), "pth"))
        if p.slot:
            length, width, axis = p.slot
            r = width / 2
            if axis == "h":
                g = LineString([(p.x - length / 2 + r, p.y), (p.x + length / 2 - r, p.y)]).buffer(r)
            else:
                g = LineString([(p.x, p.y - length / 2 + r), (p.x, p.y + length / 2 - r)]).buffer(r)
            out.append((p.net, g, "pth"))
    for v in vias:
        out.append((v.net, Point(v.x, v.y).buffer(VIA_DRILL / 2), "pth"))
    for x, y in mount_holes:
        out.append((None, Point(x, y).buffer(MOUNT_DRILL / 2), "npth"))
    return out


def pour(layer):
    """GND pour: board minus edge margin minus clearance around other-net copper
    and around NPTH holes; islands not touching GND copper are dropped."""
    area = BOARD.buffer(-0.5)
    keep_out = []
    for net, g in features(layer):
        if net != "GND":
            keep_out.append(g.buffer(POUR_CLEARANCE, quad_segs=8))
    for net, g, kind in holes():
        if kind == "npth":
            keep_out.append(g.buffer(1.5))  # screw heads
    # no copper under the XIAO's own bottom pads 15-20 / antenna side on top
    if layer == "top":
        keep_out.append(box(42.0, 14.4, 49.6, 19.6))
    # thermal relief on GND through-hole pads: a 0.3 mm gap with four 0.4 mm
    # spokes, so the pour does not soak up the iron's heat (the XIAO BAT- pad
    # is soldered from the back through its hole)
    reliefs, spokes = [], []
    for pd in ALL_PADS:
        if pd.net == "GND" and pd.tht and layer in pad_layers(pd):
            g = pd.geom()
            reliefs.append(g.buffer(POUR_CLEARANCE, quad_segs=8))
            reach = g.buffer(POUR_CLEARANCE + 0.2)
            cross = unary_union([box(pd.x - 0.2, pd.y - 5, pd.x + 0.2, pd.y + 5),
                                 box(pd.x - 5, pd.y - 0.2, pd.x + 5, pd.y + 0.2)])
            spokes.append(cross.intersection(reach))
    blocked = unary_union(keep_out)
    p = area.difference(unary_union(keep_out + reliefs))
    if spokes:
        p = p.union(unary_union(spokes).intersection(area).difference(blocked))
    gnd = unary_union([g for net, g in features(layer) if net == "GND"])
    polys = list(p.geoms) if isinstance(p, MultiPolygon) else [p]
    kept = [q for q in polys if q.intersects(gnd) and q.area > 1.0]
    return unary_union(kept)


POURS = {layer: pour(layer) for layer in ("top", "bottom")}


def copper(layer):
    geoms = [g for _, g in features(layer)] + [POURS[layer]]
    return unary_union(geoms)


# ---------------------------------------------------------------------------
# DRC: clearance + connectivity
# ---------------------------------------------------------------------------
def drc():
    report = []
    errors = 0
    for layer in ("top", "bottom"):
        feats = features(layer) + [("GND", POURS[layer])]
        bynet = {}
        for net, g in feats:
            bynet.setdefault(net or f"_nc_{id(g)}", []).append(g)
        nets = {k: unary_union(v) for k, v in bynet.items()}
        keys = list(nets)
        for i in range(len(keys)):
            for j in range(i + 1, len(keys)):
                d = nets[keys[i]].distance(nets[keys[j]])
                if d < CLEARANCE - 1e-6:
                    errors += 1
                    report.append(f"CLEARANCE {layer}: {keys[i]} <-> {keys[j]} = {d:.3f} mm")
        # copper to board edge
        edge = BOARD.exterior
        for k, g in nets.items():
            d = g.distance(edge)
            if d < 0.3 - 1e-6:
                errors += 1
                report.append(f"EDGE {layer}: {k} {d:.3f} mm from the board edge")
    # hole to other-net copper (drill wander)
    for net, h, kind in holes():
        for layer in ("top", "bottom"):
            for n2, g in features(layer):
                if n2 != net and net is not None and n2 is not None and h.distance(g) < 0.15 and not h.intersects(g):
                    errors += 1
                    report.append(f"HOLE {layer}: {net} hole near {n2} ({h.distance(g):.3f})")
    # connectivity per net: islands of copper (both layers joined by holes)
    netnames = sorted({p.net for p in ALL_PADS if p.net} | {t.net for t in tracks})
    for net in netnames:
        items = []
        for layer in ("top", "bottom"):
            for n, g in features(layer):
                if n == net:
                    items.append((layer, g))
            if net == "GND":
                items.append((layer, POURS[layer]))
        hole_geoms = [h for n, h, k in holes() if n == net]
        # union-find
        parent = list(range(len(items)))

        def find(a):
            while parent[a] != a:
                parent[a] = parent[parent[a]]
                a = parent[a]
            return a

        for i in range(len(items)):
            for j in range(i + 1, len(items)):
                li, gi = items[i]
                lj, gj = items[j]
                joined = False
                if li == lj and gi.intersects(gj):
                    joined = True
                elif li != lj and any(h.intersects(gi) and h.intersects(gj) for h in hole_geoms):
                    joined = True
                if joined:
                    parent[find(i)] = find(j)
        groups = {find(i) for i in range(len(items))}
        pads_of_net = [f"{p.ref}.{p.name}" for p in ALL_PADS if p.net == net]
        status = "ok" if len(groups) == 1 else f"SPLIT into {len(groups)} islands"
        if len(groups) != 1:
            errors += 1
        report.append(f"NET {net:<11} {status:<20} pads: {', '.join(pads_of_net)}")
    report.insert(0, f"DRC: {errors} error(s); clearance >= {CLEARANCE} mm, pour clearance {POUR_CLEARANCE} mm")
    return errors, report


# ---------------------------------------------------------------------------
# silkscreen text (filled glyph outlines)
# ---------------------------------------------------------------------------
def text_geom(s, x, y, size=1.0, anchor="center", rot=0):
    from matplotlib.font_manager import FontProperties
    from matplotlib.textpath import TextPath

    tp = TextPath((0, 0), s, size=size * 1.38, prop=FontProperties(family="DejaVu Sans", weight="bold"))
    polys = []
    for poly in tp.to_polygons():
        if len(poly) >= 3:
            polys.append(Polygon(poly).buffer(0))
    # even-odd fill: XOR the outlines
    g = Polygon()
    for p in polys:
        g = g.symmetric_difference(p)
    minx, miny, maxx, maxy = g.bounds
    dx = {"center": -(minx + maxx) / 2, "left": -minx, "right": -maxx}[anchor]
    g = affinity.translate(g, dx, -(miny + maxy) / 2)
    if rot:
        g = affinity.rotate(g, rot, origin=(0, 0))
    return affinity.translate(g, x, y)


def silkscreen():
    items = []
    lw = 0.15

    def outline(x0, y0, x1, y1):
        return box(x0, y0, x1, y1).exterior.buffer(lw / 2)

    # component outlines
    items.append(outline(29.0, 8.1, 50.0, 25.9))  # XIAO
    items.append(outline(0.0, 21.8, 14.0, 30.0))   # J1 body
    items.append(outline(0.0, 4.8, 14.0, 13.0))    # J2 body
    items.append(outline(15.25, 1.3, 22.15, 6.8))  # J3 housing
    items += [
        text_geom("OUT > CAM", 7.0, 20.5, 1.0),
        text_geom("MIC IN", 7.0, 14.3, 1.0),
        text_geom("BAT", 15.6, 7.6, 0.9, "left"),
        text_geom("+", 18.7, 5.6, 1.2),
        text_geom("PWR", 28.0, 7.4, 0.9),
        text_geom("U1 XIAO ESP32C3", 39.5, 17.0, 1.1),
        text_geom("USB-C >", 45.2, 13.0, 0.9),
        text_geom(f"LTC NODE {REVISION}", 31.0, 32.6, 1.0, "left"),
    ]
    label_at = {  # refdes positions chosen so that labels never touch each other
        "R1": (26.4, 29.4), "R2": (26.4, 27.8), "R3": (17.9, 25.0), "C2": (17.6, 23.4),
        "C1": (17.9, 21.8), "R8": (18.5, 29.9), "R9": (19.4, 32.9), "R4": (45.0, 3.9),
        "R5": (42.9, 3.1), "R7": (35.0, 6.3), "D1": (37.6, 6.3),
    }
    for p in parts:
        if p.smt:
            x, y = label_at.get(p.ref, (p.x, p.y + 1.1))
            items.append(text_geom(p.ref, x, y, 0.75))
    silk = unary_union(items)
    # never print on pads or holes
    openings = unary_union([p.geom(0.15) for p in ALL_PADS] + [h.buffer(0.2) for _, h, _ in holes()])
    return silk.difference(openings).intersection(BOARD.buffer(-0.2))


# ---------------------------------------------------------------------------
# Gerber / Excellon writers
# ---------------------------------------------------------------------------
def fmt(v):
    return f"{int(round(v * 1e6))}"


class Gerber:
    def __init__(self, path, function):
        self.path = path
        self.lines = [
            "G04 esphome-ltc-sync LTC node carrier*",
            f"%TF.FileFunction,{function}*%",
            "%FSLAX46Y46*%",
            "%MOMM*%",
            "%LPD*%",
            "%ADD10C,0.100000*%",
        ]

    def region(self, coords, clear=False):
        self.lines.append("%LPC*%" if clear else "%LPD*%")
        self.lines.append("G36*")
        pts = list(coords)
        self.lines.append(f"X{fmt(pts[0][0])}Y{fmt(pts[0][1])}D02*")
        for x, y in pts[1:]:
            self.lines.append(f"G01X{fmt(x)}Y{fmt(y)}D01*")
        self.lines.append("G37*")

    def geometry(self, g):
        polys = list(g.geoms) if hasattr(g, "geoms") else [g]
        polys = [p for p in polys if not p.is_empty and p.geom_type == "Polygon"]
        # bigger first so that islands inside holes are drawn after the clear
        for p in sorted(polys, key=lambda q: -q.area):
            self.region(p.exterior.coords)
            for hole in p.interiors:
                self.region(hole.coords, clear=True)
        self.lines.append("%LPD*%")

    def line_path(self, coords):
        self.lines.append("D10*")
        pts = list(coords)
        self.lines.append(f"X{fmt(pts[0][0])}Y{fmt(pts[0][1])}D02*")
        for x, y in pts[1:]:
            self.lines.append(f"X{fmt(x)}Y{fmt(y)}D01*")

    def save(self):
        self.lines.append("M02*")
        self.path.write_text("\n".join(self.lines) + "\n")


def write_drill(path, kind):
    tools = {}
    for net, g, k in holes():
        if k != kind:
            continue
        minx, miny, maxx, maxy = g.bounds
        w, h = maxx - minx, maxy - miny
        dia = round(min(w, h), 3)
        cx, cy = (minx + maxx) / 2, (miny + maxy) / 2
        if abs(w - h) < 1e-3:
            tools.setdefault(dia, []).append(("hole", cx, cy))
        else:
            r = dia / 2
            if w > h:
                tools.setdefault(dia, []).append(("slot", minx + r, cy, maxx - r, cy))
            else:
                tools.setdefault(dia, []).append(("slot", cx, miny + r, cx, maxy - r))
    func = "Plated,1,2,PTH" if kind == "pth" else "NonPlated,1,2,NPTH"
    lines = ["M48", "; esphome-ltc-sync LTC node carrier", f"; #@! TF.FileFunction,{func}", "FMAT,2", "METRIC"]
    for i, d in enumerate(sorted(tools), 1):
        lines.append(f"T{i}C{d:.3f}")
    lines += ["%", "G90", "G05"]
    for i, d in enumerate(sorted(tools), 1):
        lines.append(f"T{i}")
        for item in tools[d]:
            if item[0] == "hole":
                lines.append(f"X{item[1]:.3f}Y{item[2]:.3f}")
            else:
                lines.append(f"X{item[1]:.3f}Y{item[2]:.3f}G85X{item[3]:.3f}Y{item[4]:.3f}")
    lines.append("M30")
    path.write_text("\n".join(lines) + "\n")


def write_fab():
    GERBER.mkdir(parents=True, exist_ok=True)
    for f in GERBER.iterdir():
        f.unlink()
    files = {}
    for layer, ext, func in (("top", "GTL", "Copper,L1,Top"), ("bottom", "GBL", "Copper,L2,Bot")):
        g = Gerber(GERBER / f"ltc-node.{ext}", func)
        g.geometry(copper(layer))
        g.save()
        files[ext] = g.path
    # solder mask openings (vias tented)
    for layer, ext, func in (("top", "GTS", "Soldermask,Top"), ("bottom", "GBS", "Soldermask,Bot")):
        g = Gerber(GERBER / f"ltc-node.{ext}", func)
        g.geometry(unary_union([p.geom(MASK_EXP) for p in ALL_PADS if layer in pad_layers(p)]))
        g.save()
    g = Gerber(GERBER / "ltc-node.GTP", "Paste,Top")
    g.geometry(unary_union([p.geom() for p in ALL_PADS if p.paste and not p.tht]))
    g.save()
    g = Gerber(GERBER / "ltc-node.GTO", "Legend,Top")
    g.geometry(silkscreen())
    g.save()
    g = Gerber(GERBER / "ltc-node.GKO", "Profile,NP")
    g.line_path(BOARD.exterior.coords)
    g.save()
    write_drill(GERBER / "ltc-node-PTH.DRL", "pth")
    write_drill(GERBER / "ltc-node-NPTH.DRL", "npth")
    zpath = OUT / "ltc-node-pcb-gerber.zip"
    with zipfile.ZipFile(zpath, "w", zipfile.ZIP_DEFLATED) as z:
        for f in sorted(GERBER.iterdir()):
            z.write(f, f.name)
    return zpath


def write_bom_cpl():
    smt = [p for p in parts if p.smt]
    groups = {}
    for p in smt:
        groups.setdefault((p.value, p.footprint, p.lcsc), []).append(p.ref)
    with open(OUT / "bom_jlcpcb.csv", "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["Comment", "Designator", "Footprint", "LCSC Part #"])
        for (value, fp, lcsc), refs in sorted(groups.items(), key=lambda kv: kv[1][0]):
            w.writerow([value, ",".join(sorted(refs)), fp, lcsc])
    with open(OUT / "cpl_jlcpcb.csv", "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["Designator", "Mid X", "Mid Y", "Layer", "Rotation"])
        for p in smt:
            w.writerow([p.ref, f"{p.x:.3f}mm", f"{p.y:.3f}mm", "Top", f"{p.rot % 360:.0f}"])
    with open(OUT / "bom_handsolder.csv", "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["Designator", "Part", "MPN", "LCSC", "Qty", "Note"])
        for p in parts:
            if not p.smt:
                note = {"U1": "solder BAT+/BAT- from the back through the plated holes, then the castellated edge",
                        "J1": "3.5 mm out to camera", "J2": "3.5 mm in from microphone",
                        "J3": "check the battery's +/- against the '+' mark before plugging in",
                        "SW1": "charging needs the switch ON"}.get(p.ref, "")
                w.writerow([p.ref, p.value, p.mpn, p.lcsc, 1, note])
        w.writerow(["BT1", "LiPo 1000 mAh 503450 with JST-PH 2.0 plug", "", "", 1, "about 50 x 34 x 5 mm, sits under the board"])
        w.writerow(["ANT", "2.4 GHz FPC antenna (comes with the XIAO)", "", "", 1, "stick to the inside of the lid"])
        w.writerow(["H1-H4", "M2 x 10 self-tapping screw (lid to base)", "", "", 4, "see hardware/README.md"])


def render_previews():
    import matplotlib

    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    from matplotlib.patches import PathPatch
    from matplotlib.path import Path as MPath

    def draw(ax, g, **kw):
        polys = list(g.geoms) if hasattr(g, "geoms") else [g]
        for p in polys:
            if p.is_empty or p.geom_type != "Polygon":
                continue
            verts, codes = [], []
            for ring in [p.exterior] + list(p.interiors):
                c = list(ring.coords)
                verts += c
                codes += [MPath.MOVETO] + [MPath.LINETO] * (len(c) - 2) + [MPath.CLOSEPOLY]
            ax.add_patch(PathPatch(MPath(verts, codes), **kw))

    hole_g = unary_union([h for _, h, _ in holes()])
    for side in ("top", "bottom"):
        fig, ax = plt.subplots(figsize=(10, 7), dpi=150)
        fig.patch.set_facecolor("white")
        draw(ax, BOARD, facecolor="#1b5e20", edgecolor="black", lw=1)
        draw(ax, copper(side), facecolor="#c9a227", edgecolor="none", alpha=0.9)
        other = "bottom" if side == "top" else "top"
        mask_open = unary_union([p.geom(MASK_EXP) for p in ALL_PADS if side in pad_layers(p)])
        draw(ax, mask_open, facecolor="#e8c860", edgecolor="none")
        if side == "top":
            draw(ax, silkscreen(), facecolor="white", edgecolor="none")
        draw(ax, hole_g, facecolor="white", edgecolor="#555", lw=0.3)
        ax.set_xlim(-2, BOARD_W + 2)
        ax.set_ylim(-2, BOARD_H + 2)
        ax.set_aspect("equal")
        if side == "bottom":
            ax.invert_xaxis()  # as seen from below
        ax.set_title(f"LTC node {REVISION} — {side} ({'viewed from below' if side == 'bottom' else 'viewed from above'}), mm")
        ax.grid(alpha=0.2)
        fig.tight_layout()
        fig.savefig(OUT / f"preview_{side}.png")
        plt.close(fig)


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    errors, report = drc()
    (OUT / "drc_report.txt").write_text("\n".join(report) + "\n")
    print("\n".join(report))
    zpath = write_fab()
    write_bom_cpl()
    render_previews()
    print("wrote", zpath)
    return 1 if errors else 0


if __name__ == "__main__":
    raise SystemExit(main())
