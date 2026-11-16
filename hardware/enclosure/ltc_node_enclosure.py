#!/usr/bin/env python3
"""3D-printable enclosure for the LTC node carrier board (hardware/pcb).

    pip install build123d trimesh numpy matplotlib
    python3 hardware/enclosure/ltc_node_enclosure.py

Outputs (hardware/enclosure/out/):
    base.step, lid.step   exact solids (B-rep): edit in Fusion 360 / FreeCAD /
                          Onshape, or upload to JLC3DP / any print service
    base.stl, lid.stl     meshes (JLC3DP's recommended upload format)
    enclosure.3mf         both parts laid out for a home printer
    preview.png

Stack-up (z = top of the base floor):
    0.0 ... 6.0   LiPo 503450 (50 x 34 x 5 mm) + 0.5 mm foam tape
    6.0 ... 7.6   carrier PCB (rests on the battery, clamped by lid posts)
    7.6 ... 14.2  components (JST-PH is the tallest at ~6 mm)
The halves split at z = 10.1, the centre line of the 3.5 mm jacks and the
USB-C receptacle, so every side opening is cut half in the base, half in the
lid and the PCB drops straight in. Four M2 x 10 self-tapping screws go through
the lid corners into the base bosses (heads sit on the lid). A standard
cold-shoe foot is under the base. Every wall is >= 0.8 mm (JLC3DP: walls
> 1.2 mm, thinnest part >= 0.8 mm; main walls here are 2 mm). Home printing:
PETG, base open side up, lid roof down, no supports.
"""

from pathlib import Path

import numpy as np
from build123d import Align, Box, Cylinder, Pos, RectangleRounded, Rot, export_step, export_stl, extrude

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


MIN3 = (Align.MIN, Align.MIN, Align.MIN)
CTR_MIN = (Align.CENTER, Align.CENTER, Align.MIN)


def prism_rrect(x0, y0, x1, y1, r, z0, z1):
    sk = RectangleRounded(x1 - x0, y1 - y0, r)
    return Pos((x0 + x1) / 2, (y0 + y1) / 2, z0) * extrude(sk, amount=z1 - z0)


def box(x0, y0, z0, x1, y1, z1):
    return Pos(x0, y0, z0) * Box(x1 - x0, y1 - y0, z1 - z0, align=MIN3)


def cyl(x, y, z0, z1, r):
    return Pos(x, y, z0) * Cylinder(r, z1 - z0, align=CTR_MIN)


def xcyl(y, z, x0, x1, r):
    """cylinder along +x"""
    return Pos(x0, y, z) * Rot(0, 90, 0) * Cylinder(r, x1 - x0, align=CTR_MIN)


def openings():
    """Side openings, centred on the split plane."""
    cut = None
    for y in JACK_Y:
        c = (xcyl(y, Z_SPLIT, OX0 - 1, X0 + 0.5, 3.25)        # plug hole
             + xcyl(y, Z_SPLIT, OX0 - 1, OX0 + 1.2, 4.2))     # counterbore for the plug collar
        cut = c if cut is None else cut + c
    cut += box(X1 - 0.5, USB_Y - 4.75, Z_SPLIT - 2.0, OX1 + 1, USB_Y + 4.75, Z_SPLIT + 2.0)       # USB-C
    cut += box(OX1 - 1.2, USB_Y - 6.5, Z_SPLIT - 3.5, OX1 + 1, USB_Y + 6.5, Z_SPLIT + 3.5)        # cable overmold (0.8 mm wall left)
    return cut


def base():
    outer = prism_rrect(OX0, OY0, OX1, OY1, OUTER_R, -FLOOR, Z_SPLIT)
    cavity = prism_rrect(X0, Y0, X1, Y1, 1.0, 0, Z_SPLIT + 1)
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
    outer = prism_rrect(OX0, OY0, OX1, OY1, OUTER_R, Z_SPLIT, Z_CEILING + ROOF)
    cavity = prism_rrect(X0, Y0, X1, Y1, 1.0, Z_SPLIT - 1, Z_CEILING)
    l = outer - cavity
    for x, y in BOSSES:
        l += cyl(x, y, Z_SPLIT, Z_CEILING, BOSS_R)
        l -= cyl(x, y, Z_SPLIT - 1, Z_CEILING + ROOF + 1, CLEAR_D / 2)
        # no head counterbore: it would leave < 0.8 mm wall at the corners
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


def to_trimesh(part, path):
    import trimesh

    export_stl(part, str(path), tolerance=0.01, angular_tolerance=0.1)
    return trimesh.load(str(path))


def main():
    import trimesh

    OUT.mkdir(parents=True, exist_ok=True)
    b, l = base(), lid()
    meshes = {}
    for name, part in (("base", b), ("lid", l)):
        assert part.is_valid, f"{name}: invalid solid"
        export_step(part, str(OUT / f"{name}.step"))
        t = to_trimesh(part, OUT / f"{name}.stl")
        assert t.is_watertight, f"{name} is not watertight"
        print(f"{name}: {np.round(t.bounds[1] - t.bounds[0], 2)} mm, volume {part.volume / 1000:.1f} cm3, "
              f"watertight, STEP + STL")
        meshes[name] = t
    # print layout: base upright, lid flipped (roof on the bed), side by side
    tb = meshes["base"].copy()
    tb.apply_translation([0, 0, -tb.bounds[0][2]])
    tl = meshes["lid"].copy()
    tl.apply_transform(trimesh.transformations.rotation_matrix(np.pi, [1, 0, 0]))
    tl.apply_translation([0, 0, -tl.bounds[0][2]])
    tl.apply_translation([tb.bounds[1][0] - tl.bounds[0][0] + 8, tb.bounds[0][1] - tl.bounds[0][1], 0])
    trimesh.Scene({"base": tb, "lid": tl}).export(OUT / "enclosure.3mf")
    render(meshes["base"], meshes["lid"])


def render(b, l):
    """Software z-buffer render (no OpenGL): per-pixel depth test, headlight
    shading, and outlines where depth or surface normal changes."""
    import matplotlib

    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    def view_matrix(elev, azim):
        e, a = np.radians(elev), np.radians(azim)
        rz = np.array([[np.cos(a), -np.sin(a), 0], [np.sin(a), np.cos(a), 0], [0, 0, 1]])
        rx = np.array([[1, 0, 0], [0, np.cos(e), -np.sin(e)], [0, np.sin(e), np.cos(e)]])
        return rx @ rz

    def raster(items, elev, azim, px=8.0):
        R = view_matrix(elev, azim)
        tris, cols, nrm = [], [], []
        for m, color, dz in items:
            v = np.asarray(m.vertices, float).copy()
            v[:, 2] += dz
            t = v[np.asarray(m.faces)] @ R.T  # camera: x right, y up, z to viewer
            n = np.cross(t[:, 1] - t[:, 0], t[:, 2] - t[:, 0])
            n /= np.linalg.norm(n, axis=1)[:, None] + 1e-12
            keep = n[:, 2] > 1e-6  # back-face cull
            tris.append(t[keep]); nrm.append(n[keep])
            cols.append(np.tile(matplotlib.colors.to_rgb(color), (keep.sum(), 1)))
        T, N, C = np.concatenate(tris), np.concatenate(nrm), np.concatenate(cols)
        lo = T[:, :, :2].reshape(-1, 2).min(0) - 3
        hi = T[:, :, :2].reshape(-1, 2).max(0) + 3
        W, H = (np.ceil((hi - lo) * px)).astype(int)
        S = (T[:, :, :2] - lo) * px
        S[:, :, 1] = H - S[:, :, 1]
        zbuf = np.full((H, W), -np.inf)
        fid = np.full((H, W), -1)
        for i, (s, z) in enumerate(zip(S, T[:, :, 2])):
            x0, y0 = np.floor(s.min(0)).astype(int)
            x1, y1 = np.ceil(s.max(0)).astype(int)
            x0, y0 = max(x0, 0), max(y0, 0)
            x1, y1 = min(x1, W - 1), min(y1, H - 1)
            if x1 < x0 or y1 < y0:
                continue
            xs, ys = np.meshgrid(np.arange(x0, x1 + 1) + 0.5, np.arange(y0, y1 + 1) + 0.5)
            (ax_, ay), (bx, by), (cx, cy) = s
            den = (by - cy) * (ax_ - cx) + (cx - bx) * (ay - cy)
            if abs(den) < 1e-12:
                continue
            w0 = ((by - cy) * (xs - cx) + (cx - bx) * (ys - cy)) / den
            w1 = ((cy - ay) * (xs - cx) + (ax_ - cx) * (ys - cy)) / den
            w2 = 1 - w0 - w1
            inside = (w0 >= -1e-4) & (w1 >= -1e-4) & (w2 >= -1e-4)
            zz = w0 * z[0] + w1 * z[1] + w2 * z[2]
            sub = zbuf[y0:y1 + 1, x0:x1 + 1]
            win = inside & (zz > sub)
            sub[win] = zz[win]
            fid[y0:y1 + 1, x0:x1 + 1][win] = i
        img = np.ones((H, W, 3))
        hit = fid >= 0
        light = np.array([-0.35, 0.45, 0.82]); light /= np.linalg.norm(light)
        shade = 0.38 + 0.62 * np.clip(N @ light, 0, 1)
        img[hit] = C[fid[hit]] * shade[fid[hit]][:, None]
        # outlines: silhouette, depth steps and creases (normal change > ~25 deg)
        edge = np.zeros_like(hit)
        nb = np.where(hit[..., None], N[np.clip(fid, 0, None)], 0)
        zb = np.where(hit, zbuf, 0.0)
        for dy, dx in ((0, 1), (1, 0)):
            a = (slice(0, H - dy), slice(0, W - dx)); b_ = (slice(dy, H), slice(dx, W))
            e = hit[a] != hit[b_]
            both = hit[a] & hit[b_]
            e |= both & (np.abs(zb[a] - zb[b_]) > 0.6)
            e |= both & ((nb[a] * nb[b_]).sum(-1) < 0.9)
            edge[a] |= e
        img[edge] = img[edge] * 0.35
        return img

    views = [
        ("assembled, lid lifted (jacks left, USB-C right)",
         [(b, "#4a6fa5", 0), (l, "#d9a441", 14)], -62, -35),
        ("base from below (cold-shoe foot)", [(b, "#4a6fa5", 0)], 118, -35),
        ("lid from below (PCB clamp posts)", [(l, "#d9a441", 0)], 118, -35),
    ]
    fig, axes = plt.subplots(1, 3, figsize=(15, 5), dpi=150)
    for ax, (title, items, el, az) in zip(axes, views):
        ax.imshow(raster(items, el, az), interpolation="lanczos")
        ax.set_title(title)
        ax.axis("off")
    fig.tight_layout()
    fig.savefig(OUT / "preview.png", facecolor="white")


if __name__ == "__main__":
    main()
