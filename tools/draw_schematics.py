#!/usr/bin/env python3
"""Draws the wiring diagrams in docs/img/ (SVG + PNG).

    pip install schemdraw matplotlib
    python3 tools/draw_schematics.py
"""

from pathlib import Path

import schemdraw
import schemdraw.elements as elm

OUT = Path(__file__).resolve().parent.parent / "docs" / "img"


def output_stage(d, x0=0.0, y0=0.0, tip_label="Tip → camera / recorder", ground=True):
    """Two resistor taps + shunt + DC block. Returns (node, tip, ground) anchors."""
    g5 = elm.Dot(open=True).at((x0, y0)).label("GPIO5 (D3)\nmic tap", loc="left")
    d += g5
    r1 = elm.Resistor().right().at(g5.center).label("R1 220 kΩ")
    d += r1
    n = elm.Dot().at(r1.end)
    d += n
    g6 = elm.Dot(open=True).at((x0, y0 - 2.4)).label("GPIO6 (D4)\nline tap", loc="left")
    d += g6
    r2 = elm.Resistor().right().at(g6.center).label("R2 4.7 kΩ")
    d += r2
    p = elm.Dot().at(r2.end)
    d += p
    d += elm.Line().up().toy(n.center).at(p.center)
    # DC block to the jack tip, drawn well to the right
    d += elm.Line().right(1.0).at(n.center)
    c1 = elm.Capacitor().right().label("C1 1 µF")
    d += c1
    d += elm.Line().right(2.2)
    tip = elm.Dot(open=True).label(tip_label, loc="right")
    d += tip
    # shunt to ground: R3 with C2 in parallel
    r3 = elm.Resistor().down().at(p.center).label("R3 1 kΩ", loc="top")
    d += r3
    d += elm.Line().right(2.0).at(p.center)
    c2 = elm.Capacitor().down().label("C2 22 nF", loc="bottom")
    d += c2
    d += elm.Line().left().tox(r3.end).at(c2.end)
    gnd = elm.Dot().at(r3.end)
    d += gnd
    if ground:
        d += elm.Ground().at(gnd.center)
    return n, tip, gnd


def draw_output(path, backend):
    schemdraw.use(backend)
    with schemdraw.Drawing(show=False) as d:
        d.config(unit=2.4, fontsize=11)
        n, tip, gnd = output_stage(d)
        # sleeve
        s = elm.Dot(open=True).at((tip.center[0], gnd.center[1])).label("Sleeve (GND)", loc="right")
        d += s
        d += elm.Line().left().tox(gnd.center).at(s.center)
        d += elm.Label().at((-2.0, gnd.center[1] - 1.6)).label(
            "mic : only GPIO5 drives, GPIO6 floats → ≈ 15 mV p-p\n"
            "line: only GPIO6 drives, GPIO5 floats → ≈ 0.6 V p-p (≈ -10 dBV)\n"
            "C2 sets the edge time to ≈ 20 µs. Ring can carry other audio.",
            halign="left",
        )
        d.save(str(path), dpi=200) if backend == "matplotlib" else d.save(str(path))


def draw_passthrough(path, backend):
    schemdraw.use(backend)
    with schemdraw.Drawing(show=False) as d:
        d.config(unit=2.4, fontsize=11)
        n, tip, gnd = output_stage(d, x0=0.0, y0=0.0, tip_label="Tip  (OUT jack)", ground=False)
        xo = tip.center[0]
        # output jack ring / sleeve, input jack on the far left
        y_ring, y_sl = 2.2, gnd.center[1] - 1.6
        xin = -4.2
        rin = elm.Dot(open=True).at((xin, y_ring)).label("Ring  (IN jack)\nfrom microphone", loc="left")
        d += rin
        d += elm.Line().right().tox(xo).at(rin.center)
        d += elm.Dot(open=True).at((xo, y_ring)).label("Ring  (OUT jack)\nmic audio, passive", loc="right")
        tin = elm.Dot(open=True).at((xin, y_ring - 1.0)).label("Tip  (IN jack)\nnot connected", loc="left")
        d += tin
        sin = elm.Dot(open=True).at((xin, y_sl)).label("Sleeve  (IN jack)", loc="left")
        d += sin
        d += elm.Line().right().tox(xo).at(sin.center)
        d += elm.Dot(open=True).at((xo, y_sl)).label("Sleeve  (OUT jack)", loc="right")
        # single ground point: shunt ground and ESP32 GND meet the sleeve here only
        d += elm.Line().down().toy(y_sl).at(gnd.center)
        star = elm.Dot().at((gnd.center[0], y_sl))
        d += star
        d += elm.Label().at((gnd.center[0] + 0.2, y_sl - 0.35)).label("single ground point\n(ESP32 GND joins here only)", halign="left", valign="top")
        d.save(str(path), dpi=200) if backend == "matplotlib" else d.save(str(path))


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    for name, fn in (("ltc-output", draw_output), ("mic-passthrough", draw_passthrough)):
        fn(OUT / f"{name}.svg", "svg")
        fn(OUT / f"{name}.png", "matplotlib")
        print("wrote", OUT / f"{name}.svg", OUT / f"{name}.png")


if __name__ == "__main__":
    main()
