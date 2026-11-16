#!/usr/bin/env python3
"""3D-printable enclosure for the LTC node carrier board (hardware/pcb).

    pip install manifold3d trimesh numpy matplotlib
    python3 hardware/enclosure/ltc_node_enclosure.py

Outputs (hardware/enclosure/out/): base.stl, lid.stl, enclosure.3mf (both
parts, print layout), preview.png.

Stack-up (z = top of the base floor):
    0.0 ... 6.0   LiPo 503450 (50 x 34 x 5 mm) + 0.5 mm foam tape
    6.0 ... 7.6   carrier PCB (rests on the battery, clamped by lid posts)
    7.6 ... 14.2  components (JST-PH is the tallest at ~6 mm)
The halves split at z = 10.1, the centre line of the 3.5 mm jacks and the
USB-C receptacle, so every side opening is cut half in the base, half in the
lid and the PCB drops straight in. Four M2 x 10 self-tapping screws go through
the lid corners into the base bosses. A standard cold-shoe foot is under the
base. Print both parts in PETG, flat side down, no supports needed except
for the cold-shoe foot overhang (0.6 mm, usually fine without).
"""

from pathlib import Path

import numpy as np
from manifold3d import CrossSection, JoinType, Manifold

HERE = Path(__file__).resolve().parent
OUT = HERE / "out"

# board coordinates (same as hardware/pcb/ltc_node_pcb.py)
PCB_W, PCB_H, PCB_T = 50.0, 34.0, 1.6
PCB_HOLES = [(2.5, 2.0), (2.5, 32.0), (47.5, 2.0), (47.5, 32.0)]
JACK_Y = [26.9, 9.9]          # nose centres (J1 out, J2 in)
USB_Y = 17.0                  # XIAO USB-C centre
LED_XY = (37.6, 5.2)
SWITCH_XY = (27.0, 3.0)       # actuator centre (pin 2)

MARGIN = 1.6                  # PCB edge to inner wall
WALL = 2.0
FLOOR = 1.6
ROOF = 1.6
Z_PCB_BOTTOM = 6.0
Z_PCB_TOP = Z_PCB_BOTTOM + PCB_T
Z_SPLIT = 10.1                # jack / USB centre line
Z_CEILING = 14.2
SEG = 64

X0, Y0 = -MARGIN, -MARGIN
X1, Y1 = PCB_W + MARGIN, PCB_H + MARGIN
OX0, OY0, OX1, OY1 = X0 - WALL, Y0 - WALL, X1 + WALL, Y1 + WALL
OUTER_R = 3.0
BOSSES = [(-1.2, -1.2), (-1.2, PCB_H + 1.2), (PCB_W + 1.2, -1.2), (PCB_W + 1.2, PCB_H + 1.2)]
BOSS_R = 2.1
PILOT_D, CLEAR_D, HEAD_D = 1.6, 2.3, 4.0


def rrect(x0, y0, x1, y1, r):
    cs = CrossSection.square([x1 - x0 - 2 * r, y1 - y0 - 2 * r]).translate([x0 + r, y0 + r])
    return cs.offset(r, JoinType.Round, circular_segments=SEG)


def prism(cs, z0, z1):
    return Manifold.extrude(cs, z1 - z0).translate([0, 0, z0])


def box(x0, y0, z0, x1, y1, z1):
    return Manifold.cube([x1 - x0, y1 - y0, z1 - z0]).translate([x0, y0, z0])


def cyl(x, y, z0, z1, r):
    return Manifold.cylinder(z1 - z0, r, r, SEG).translate([x, y, z0])


def xcyl(y, z, x0, x1, r):
    """cylinder along x"""
    return Manifold.cylinder(x1 - x0, r, r, SEG).rotate([0, 90, 0]).translate([x0, y, z])


def openings():
    """Side openings, centred on the split plane."""
    cut = Manifold()
    for y in JACK_Y:
        cut += xcyl(y, Z_SPLIT, OX0 - 1, X0 + 0.5, 3.25)                     # plug hole
        cut += xcyl(y, Z_SPLIT, OX0 - 1, OX0 + 1.2, 4.2)                     # counterbore for the plug collar
    cut += box(X1 - 0.5, USB_Y - 4.75, Z_SPLIT - 2.0, OX1 + 1, USB_Y + 4.75, Z_SPLIT + 2.0)       # USB-C
    cut += box(OX1 - 1.5, USB_Y - 6.5, Z_SPLIT - 3.5, OX1 + 1, USB_Y + 6.5, Z_SPLIT + 3.5)        # cable overmold
    return cut


def base():
    outer = prism(rrect(OX0, OY0, OX1, OY1, OUTER_R), -FLOOR, Z_SPLIT)
    cavity = prism(rrect(X0, Y0, X1, Y1, 1.0), 0, Z_SPLIT + 1)
    b = outer - cavity
    for x, y in BOSSES:
        b += cyl(x, y, 0, Z_SPLIT, BOSS_R)
        b -= cyl(x, y, 0.5, Z_SPLIT + 1, PILOT_D / 2)
    b -= openings()
    # cold-shoe foot (ISO 518 style): 18 x 18 x 1.9 plate on a 12 mm neck
    cx, cy = (OX0 + OX1) / 2, (OY0 + OY1) / 2
    neck = box(cx - 6.0, cy - 9.0, -FLOOR - 2.6, cx + 6.0, cy + 9.0, -FLOOR + 0.01)
    plate = box(cx - 9.0, cy - 9.0, -FLOOR - 2.6 - 1.9, cx + 9.0, cy + 9.0, -FLOOR - 2.6)
    b += neck + plate
    return b


def lid():
    outer = prism(rrect(OX0, OY0, OX1, OY1, OUTER_R), Z_SPLIT, Z_CEILING + ROOF)
    cavity = prism(rrect(X0, Y0, X1, Y1, 1.0), Z_SPLIT - 1, Z_CEILING)
    l = outer - cavity
    for x, y in BOSSES:
        l += cyl(x, y, Z_SPLIT, Z_CEILING, BOSS_R)
        l -= cyl(x, y, Z_SPLIT - 1, Z_CEILING + ROOF + 1, CLEAR_D / 2)
        l -= cyl(x, y, Z_CEILING + ROOF - 1.4, Z_CEILING + ROOF + 1, HEAD_D / 2)   # screw head
    # posts that press the PCB onto the battery, with pins into the PCB holes
    for x, y in PCB_HOLES:
        l += cyl(x, y, Z_PCB_TOP + 0.2, Z_CEILING + 0.01, 1.7)
        l += cyl(x, y, Z_PCB_TOP - 1.2, Z_PCB_TOP + 0.21, 0.9)
    l -= openings()
    # LED light pipe (a piece of 1.75 mm clear filament) and switch slot
    l -= cyl(LED_XY[0], LED_XY[1], Z_CEILING - 1, Z_CEILING + ROOF + 1, 0.95)
    sx, sy = SWITCH_XY
    l -= box(sx - 3.5, sy - 1.7, Z_CEILING - 1, sx + 3.5, sy + 1.7, Z_CEILING + ROOF + 1)
    return l


def to_trimesh(m):
    import trimesh

    mesh = m.to_mesh()
    return trimesh.Trimesh(vertices=np.asarray(mesh.vert_properties)[:, :3], faces=np.asarray(mesh.tri_verts), process=True)


def main():
    import trimesh

    OUT.mkdir(parents=True, exist_ok=True)
    b, l = base(), lid()
    for name, m in (("base", b), ("lid", l)):
        assert m.status().name == "NoError", m.status()
        t = to_trimesh(m)
        assert t.is_watertight, f"{name} is not watertight"
        t.export(OUT / f"{name}.stl")
        print(f"{name}: {t.bounds[1] - t.bounds[0]} mm, volume {t.volume / 1000:.1f} cm3, watertight")
    # print layout: base upright, lid flipped (roof on the bed), side by side
    tb = to_trimesh(b)
    tb.apply_translation([0, 0, -tb.bounds[0][2]])
    tl = to_trimesh(l)
    tl.apply_transform(trimesh.transformations.rotation_matrix(np.pi, [1, 0, 0]))
    tl.apply_translation([0, 0, -tl.bounds[0][2]])
    tl.apply_translation([tb.bounds[1][0] - tl.bounds[0][0] + 8, tb.bounds[0][1] - tl.bounds[0][1], 0])
    scene = trimesh.Scene({"base": tb, "lid": tl})
    try:
        scene.export(OUT / "enclosure.3mf")
    except Exception as e:  # 3MF export needs optional deps
        print("3mf export skipped:", e)
    render(b, l)


def render(b, l):
    """Painter's-algorithm render (sorted triangles), no OpenGL needed."""
    import matplotlib

    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    from matplotlib.collections import PolyCollection

    def view_matrix(elev, azim):
        e, a = np.radians(elev), np.radians(azim)
        rz = np.array([[np.cos(a), -np.sin(a), 0], [np.sin(a), np.cos(a), 0], [0, 0, 1]])
        rx = np.array([[1, 0, 0], [0, np.cos(e), -np.sin(e)], [0, np.sin(e), np.cos(e)]])
        return rx @ rz

    def draw(ax, items, elev, azim):
        R = view_matrix(elev, azim)
        polys, colors, depth = [], [], []
        light = np.array([0.3, -0.4, 0.85])
        light /= np.linalg.norm(light)
        for m, color, dz in items:
            mesh = m.to_mesh()
            v = np.asarray(mesh.vert_properties)[:, :3].copy()
            v[:, 2] += dz
            f = np.asarray(mesh.tri_verts)
            tri = v[f]
            n = np.cross(tri[:, 1] - tri[:, 0], tri[:, 2] - tri[:, 0])
            n /= np.linalg.norm(n, axis=1)[:, None] + 1e-12
            vt = tri @ R.T  # camera space: x right, y up, z towards viewer
            nt = n @ R.T
            visible = nt[:, 2] > 0
            shade = np.clip(n @ light, 0, 1) * 0.65 + 0.35
            rgb = np.array(matplotlib.colors.to_rgb(color))
            for t, sh, d in zip(vt[visible], shade[visible], vt[visible][:, :, 2].mean(axis=1)):
                polys.append(t[:, :2])
                colors.append(rgb * sh)
                depth.append(d)
        order = np.argsort(depth)
        ax.add_collection(PolyCollection([polys[i] for i in order], facecolors=[colors[i] for i in order], edgecolors="none"))
        allp = np.concatenate(polys)
        ax.set_xlim(allp[:, 0].min() - 2, allp[:, 0].max() + 2)
        ax.set_ylim(allp[:, 1].min() - 2, allp[:, 1].max() + 2)
        ax.set_aspect("equal")
        ax.axis("off")

    # rotate the camera: elevation tilts the scene towards the viewer
    fig, axes = plt.subplots(1, 3, figsize=(15, 5), dpi=150)
    draw(axes[0], [(b, "#4a6fa5", 0), (l, "#d9a441", 14)], -62, -35)
    axes[0].set_title("assembled, lid lifted (jack side left, USB-C right)")
    draw(axes[1], [(b, "#4a6fa5", 0)], -62, -35)
    axes[1].set_title("base with cold-shoe foot")
    draw(axes[2], [(l, "#d9a441", 0)], 118, -35)
    axes[2].set_title("lid from below (PCB clamp posts)")
    fig.tight_layout()
    fig.savefig(OUT / "preview.png", facecolor="white")


if __name__ == "__main__":
    main()
