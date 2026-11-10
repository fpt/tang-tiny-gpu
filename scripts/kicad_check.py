#!/usr/bin/env python3
"""KiCad の基板の検査 (ハーネスの基板、docs/HARNESS2.md)

usage: kicad_check.py <board.kicad_pcb> [--pins pins.json] [--gerbers DIR] [--clearance MM]

1. 配線表 (--pins): 信号ごとに Pico の物理ピンと Tang Nano 20K のピン (FPGA のピン番号) が同じネットか、
   series_r があれば直列抵抗 (値も) を挟むか、pulldown があれば GND への抵抗 (値も) があるか。
   ほかの Pico / Tang のピンが混ざっていないか、GND のピン、電源どうし (3V3 / 5V / VBUS / VSYS) をつないでいないか
2. 配線 (.kicad_pcb): ネットごとに、パッドが銅 (配線、円弧、ビア、パッド、ゾーンの塗り) で 1 つにつながっているか。どこにもつながらない銅
3. 短絡と間隔: 違うネットの銅が触れていないか、ネットクラスの clearance (.kicad_pro、--clearance で上書き) より近くないか。
   パッドの形 (丸角、楕円、カスタム) はそのまま扱う
4. ガーバー (--gerbers): 発注したファイルで 2 と 3 をやり直す (KiCad の X2 属性 %TO.N / %TO.P でネットとパッドが分かる)。
   パッドのネットが .kicad_pcb と同じか (古いガーバーでないか)

標準ライブラリだけ。結果を標準出力に出し、最後に PASS: / FAIL: の行。エラーがあれば終了コード 1。
KiCad の DRC の代わりではない (基板の外形との間隔、穴と銅の間隔、サーマルのスポーク、シルクは見ない)。
"""
import argparse
import collections
import glob
import json
import math
import os
import re
import sys

errors, warnings = [], []


def err(msg):
    errors.append(msg)
    print('FAIL  ' + msg)


def warn(msg):
    warnings.append(msg)
    print('WARN  ' + msg)


def ok(msg):
    print('OK    ' + msg)


# ---------------------------------------------------------------- 幾何
# 銅の形は「芯」と半径の組の並び: ('pt', (x, y)), ('seg', (a, b)), ('poly', Poly)。銅は芯から r 以内

def dseg(p, a, b):
    dx, dy = b[0] - a[0], b[1] - a[1]
    L = dx * dx + dy * dy
    t = 0.0 if L == 0 else max(0.0, min(1.0, ((p[0] - a[0]) * dx + (p[1] - a[1]) * dy) / L))
    return math.hypot(p[0] - a[0] - t * dx, p[1] - a[1] - t * dy)


def seg_seg(a, b, c, d):
    def cross(p, q, r):
        return (q[0] - p[0]) * (r[1] - p[1]) - (q[1] - p[1]) * (r[0] - p[0])
    d1, d2, d3, d4 = cross(c, d, a), cross(c, d, b), cross(a, b, c), cross(a, b, d)
    if ((d1 > 0 and d2 < 0) or (d1 < 0 and d2 > 0)) and ((d3 > 0 and d4 < 0) or (d3 < 0 and d4 > 0)):
        return 0.0
    return min(dseg(a, c, d), dseg(b, c, d), dseg(c, a, b), dseg(d, a, b))


def bb_of(points, r=0.0):
    xs = [p[0] for p in points]
    ys = [p[1] for p in points]
    return (min(xs) - r, min(ys) - r, max(xs) + r, max(ys) + r)


def bb_hit(a, b, m=0.0):
    return not (a[2] + m < b[0] or b[2] + m < a[0] or a[3] + m < b[1] or b[3] + m < a[1])


class Poly:
    CELL = 2.0

    def __init__(self, pts):
        if len(pts) > 1 and pts[0] == pts[-1]:
            pts = pts[:-1]
        self.pts = pts
        self.edges = [(pts[i], pts[(i + 1) % len(pts)]) for i in range(len(pts))]
        self.bb = bb_of(pts)
        self.grid = None
        if len(self.edges) > 32:
            self.grid = collections.defaultdict(list)
            for k, (a, b) in enumerate(self.edges):
                for cell in cells(bb_of((a, b)), self.CELL):
                    self.grid[cell].append(k)

    def edges_near(self, bb):
        if self.grid is None:
            return [e for e in self.edges if bb_hit(bb_of(e), bb)]
        ks = set()
        for cell in cells(bb, self.CELL):
            ks.update(self.grid.get(cell, ()))
        return [self.edges[k] for k in ks]

    def contains(self, p):
        if not (self.bb[0] <= p[0] <= self.bb[2] and self.bb[1] <= p[1] <= self.bb[3]):
            return False
        x, y = p
        inside = False
        for (x1, y1), (x2, y2) in self.edges:
            if (y1 > y) != (y2 > y) and x < (x2 - x1) * (y - y1) / (y2 - y1) + x1:
                inside = not inside
        return inside


def cells(bb, size):
    for i in range(int(math.floor(bb[0] / size)), int(math.floor(bb[2] / size)) + 1):
        for j in range(int(math.floor(bb[1] / size)), int(math.floor(bb[3] / size)) + 1):
            yield (i, j)


def core_pts(core):
    k, v = core
    if k == 'pt':
        return [v]
    if k == 'seg':
        return list(v)
    return v.pts


def core_dist(c1, c2, limit):
    """芯どうしの距離。limit 以上なら limit 以上の値を返す (そこまでしか探さない)"""
    order = {'pt': 0, 'seg': 1, 'poly': 2}
    if order[c1[0]] > order[c2[0]]:
        c1, c2 = c2, c1
    k1, v1 = c1
    k2, v2 = c2
    if k1 == 'pt' and k2 == 'pt':
        return math.hypot(v1[0] - v2[0], v1[1] - v2[1])
    if k1 == 'pt' and k2 == 'seg':
        return dseg(v1, *v2)
    if k1 == 'seg' and k2 == 'seg':
        return seg_seg(v1[0], v1[1], v2[0], v2[1])
    # 相手が多角形
    if k1 == 'pt':
        if v2.contains(v1):
            return 0.0
        es = v2.edges_near(bb_of([v1], limit))
        return min((dseg(v1, a, b) for a, b in es), default=limit)
    if k1 == 'seg':
        if v2.contains(v1[0]) or v2.contains(v1[1]):
            return 0.0
        es = v2.edges_near(bb_of(v1, limit))
        return min((seg_seg(v1[0], v1[1], a, b) for a, b in es), default=limit)
    small, big = (v1, v2) if len(v1.pts) <= len(v2.pts) else (v2, v1)
    if any(big.contains(p) for p in small.pts):
        return 0.0
    if any(small.contains(p) for p in big.pts if bb_hit(bb_of([p]), small.bb)):
        return 0.0
    best = limit
    for a, b in small.edges:
        for c, d in big.edges_near(bb_of((a, b), limit)):
            best = min(best, seg_seg(a, b, c, d))
            if best == 0.0:
                return 0.0
    return best


def arc_points(start, mid_or_center, end, ccw=None, step_deg=10.0):
    """円弧を折れ線に。ccw が None なら mid_or_center は弧の途中の点 (KiCad)、そうでなければ中心 (ガーバー)"""
    if ccw is None:
        (ax, ay), (bx, by), (cx, cy) = start, mid_or_center, end
        d = 2 * (ax * (by - cy) + bx * (cy - ay) + cx * (ay - by))
        if abs(d) < 1e-12:
            return [start, end]
        ux = ((ax * ax + ay * ay) * (by - cy) + (bx * bx + by * by) * (cy - ay) + (cx * cx + cy * cy) * (ay - by)) / d
        uy = ((ax * ax + ay * ay) * (cx - bx) + (bx * bx + by * by) * (ax - cx) + (cx * cx + cy * cy) * (bx - ax)) / d
        center = (ux, uy)
        a0 = math.atan2(ay - uy, ax - ux)
        am = math.atan2(by - uy, bx - ux)
        a1 = math.atan2(cy - uy, cx - ux)
        # mid を通る向き
        ccw = ((am - a0) % (2 * math.pi)) < ((a1 - a0) % (2 * math.pi))
    else:
        center = mid_or_center
        a0 = math.atan2(start[1] - center[1], start[0] - center[0])
        a1 = math.atan2(end[1] - center[1], end[0] - center[0])
    r = math.hypot(start[0] - center[0], start[1] - center[1])
    sweep = (a1 - a0) % (2 * math.pi) if ccw else -((a0 - a1) % (2 * math.pi))
    if sweep == 0:
        sweep = 2 * math.pi if ccw else -2 * math.pi
    n = max(2, int(abs(math.degrees(sweep)) / step_deg) + 1)
    pts = [(center[0] + r * math.cos(a0 + sweep * i / n), center[1] + r * math.sin(a0 + sweep * i / n)) for i in range(n + 1)]
    pts[0], pts[-1] = start, end
    return pts


# ---------------------------------------------------------------- 銅の部品
class Obj:
    __slots__ = ('net', 'layers', 'pieces', 'pad', 'kind', 'bb', 'where')

    def __init__(self, net, layers, pieces, kind, where, pad=None):
        self.net, self.layers, self.pieces, self.kind, self.where, self.pad = net, frozenset(layers), pieces, kind, where, pad
        bbs = [bb_of(core_pts(c), r) for c, r in pieces]
        self.bb = (min(b[0] for b in bbs), min(b[1] for b in bbs), max(b[2] for b in bbs), max(b[3] for b in bbs))


def gap(o1, o2, limit):
    best = limit
    for c1, r1 in o1.pieces:
        for c2, r2 in o2.pieces:
            d = core_dist(c1, c2, best + r1 + r2) - r1 - r2
            if d < best:
                best = d
                if best <= 0:
                    return best
    return best


def analyse(objs, clearance, label, same_fp_pad_clearance=False):
    """銅の部品をつないでネットごとの島にし、短絡と間隔の違反を出す。戻り値は {net: [島ごとのパッドの集合, ...]}"""
    par = list(range(len(objs)))

    def find(i):
        while par[i] != i:
            par[i] = par[par[i]]
            i = par[i]
        return i

    grid = collections.defaultdict(list)
    for i, o in enumerate(objs):
        for cell in cells(o.bb, 4.0):
            grid[cell].append(i)
    seen = set()
    shorts, close = {}, {}
    for members in grid.values():
        for x in range(len(members)):
            for y in range(x + 1, len(members)):
                i, j = members[x], members[y]
                key = (i, j) if i < j else (j, i)
                if key in seen:
                    continue
                seen.add(key)
                a, b = objs[i], objs[j]
                if not (a.layers & b.layers) or not bb_hit(a.bb, b.bb, clearance):
                    continue
                same_pad = a.pad is not None and a.pad == b.pad
                if a.net == b.net or same_pad:
                    if gap(a, b, 0.001) <= 0:
                        par[find(i)] = find(j)
                    continue
                # パッドどうしでフットプリントが同じなら、間隔はフットプリントの作りなので見ない (短絡は見る)
                same_fp = a.pad is not None and b.pad is not None and a.pad[0] == b.pad[0]
                g = gap(a, b, clearance)
                if (a.net or '') > (b.net or ''):
                    a, b = b, a
                if g <= 0:
                    k = tuple(sorted((a.net or '(no net)', b.net or '(no net)')))
                    shorts.setdefault(k, (a, b))
                elif g < clearance - 1e-3 and not (same_fp and not same_fp_pad_clearance):
                    k = tuple(sorted((a.net or '(no net)', b.net or '(no net)')))
                    if k not in close or g < close[k][0]:
                        close[k] = (g, a, b)

    for (n1, n2), (a, b) in sorted(shorts.items()):
        err(f'{label}: short {n1} <-> {n2}: {a.kind} {a.where} touches {b.kind} {b.where} on {"/".join(sorted(a.layers & b.layers))}')
    for (n1, n2), (g, a, b) in sorted(close.items()):
        err(f'{label}: clearance {g:.3f} mm < {clearance} mm: {n1} {a.kind} {a.where} <-> {n2} {b.kind} {b.where}')

    islands = collections.defaultdict(lambda: collections.defaultdict(list))
    for i, o in enumerate(objs):
        if o.net:
            islands[o.net][find(i)].append(o)
    result = {}
    for net in sorted(islands):
        groups = list(islands[net].values())
        pads = [sorted({f'{o.pad[0]}.{o.pad[1]}' for o in g if o.pad}) for g in groups]
        with_pads = [p for p in pads if p]
        result[net] = with_pads
        if net.startswith('unconnected-'):
            continue
        for g, p in zip(groups, pads):
            if not p:
                kinds = collections.Counter(o.kind for o in g)
                desc = ', '.join(f'{n} {k}' for k, n in sorted(kinds.items()))
                warn(f'{label}: {net}: copper connected to no pad ({desc}) at {g[0].where}')
        if len(with_pads) > 1:
            err(f'{label}: {net} is split into {len(with_pads)} islands: ' + ' | '.join(' '.join(p) for p in with_pads))
        elif len(with_pads) == 1 and len(with_pads[0]) == 1:
            warn(f'{label}: {net} has a single pad ({with_pads[0][0]})')
        elif with_pads:
            ok(f'{label}: {net} routed ({" ".join(with_pads[0])})')
    return result


# ---------------------------------------------------------------- .kicad_pcb
def sexp(text):
    tok = re.findall(r'\(|\)|"(?:[^"\\]|\\.)*"|[^\s()]+', text)
    stack = [[]]
    for t in tok:
        if t == '(':
            stack.append([])
        elif t == ')':
            x = stack.pop()
            stack[-1].append(x)
        else:
            stack[-1].append(t[1:-1].replace('\\"', '"') if t.startswith('"') else t)
    return stack[0][0]


def child(e, key):
    return next((f for f in e if isinstance(f, list) and f and f[0] == key), None)


def children(e, key):
    return [f for f in e if isinstance(f, list) and f and f[0] == key]


def xy(e):
    return (float(e[1]), float(e[2]))


def rot(p, deg):
    # KiCad の角度 (y が下向きの画面で反時計回り)
    a = math.radians(-deg)
    return (p[0] * math.cos(a) - p[1] * math.sin(a), p[0] * math.sin(a) + p[1] * math.cos(a))


class Board:
    def __init__(self, path):
        root = sexp(open(path, encoding='utf-8').read())
        self.copper = [l[1] for l in child(root, 'layers')[1:] if isinstance(l, list) and len(l) >= 3 and l[2] in ('signal', 'power', 'mixed', 'jumper')]
        netmap = {n[1]: n[2] for n in children(root, 'net') if len(n) > 2}

        def netname(e):
            n = child(e, 'net')
            if not n or len(n) < 2:
                return None
            v = n[-1] if len(n) > 2 else (netmap.get(n[1], n[1]) if netmap else n[1])
            return v or None

        def layers_of(names):
            out = set()
            for n in names:
                if n == '*.Cu':
                    out.update(self.copper)
                elif n == 'F&B.Cu':
                    out.update(('F.Cu', 'B.Cu'))
                elif n in self.copper:
                    out.add(n)
            return out

        self.objs = []
        self.footprints = {}   # ref -> dict(lib, value, pads: [(num, net, pinfunction)])
        for fp in children(root, 'footprint'):
            props = {p[1]: p[2] for p in children(fp, 'property') if len(p) > 2}
            ref = props.get('Reference', '?')
            at = child(fp, 'at')
            fpos, frot = xy(at), float(at[3]) if len(at) > 3 else 0.0
            info = self.footprints.setdefault(ref, dict(lib=fp[1], value=props.get('Value', ''), pads=[]))
            for p in children(fp, 'pad'):
                num, ptype, shape = p[1], p[2], p[3]
                pa = child(p, 'at')
                local = rot(xy(pa), frot)
                c = (fpos[0] + local[0], fpos[1] + local[1])
                prot = float(pa[3]) if len(pa) > 3 else frot
                net = netname(p)
                pf = child(p, 'pinfunction')
                info['pads'].append((num, net, pf[1] if pf else ''))
                if ptype == 'np_thru_hole':
                    continue
                lay = layers_of(child(p, 'layers')[1:])
                if not lay:
                    continue
                pieces = pad_pieces(p, shape, c, prot)
                self.objs.append(Obj(net, lay, pieces, 'pad', f'{ref}.{num} ({c[0]:.2f}, {c[1]:.2f})', pad=(ref, num)))
        for e in children(root, 'segment'):
            a, b = xy(child(e, 'start')), xy(child(e, 'end'))
            w = float(child(e, 'width')[1])
            self.objs.append(Obj(netname(e), {child(e, 'layer')[1]}, [(('seg', (a, b)), w / 2)], 'track', f'({a[0]:.2f}, {a[1]:.2f})-({b[0]:.2f}, {b[1]:.2f})'))
        for e in children(root, 'arc'):
            pts = arc_points(xy(child(e, 'start')), xy(child(e, 'mid')), xy(child(e, 'end')))
            w = float(child(e, 'width')[1])
            self.objs.append(Obj(netname(e), {child(e, 'layer')[1]}, [(('seg', (pts[k], pts[k + 1])), w / 2) for k in range(len(pts) - 1)],
                                 'arc', f'({pts[0][0]:.2f}, {pts[0][1]:.2f})'))
        for e in children(root, 'via'):
            c = xy(child(e, 'at'))
            names = child(e, 'layers')[1:]
            if len(names) == 2 and all(n in self.copper for n in names):
                i0, i1 = sorted(self.copper.index(n) for n in names)
                lay = set(self.copper[i0:i1 + 1])
            else:
                lay = layers_of(names)
            self.objs.append(Obj(netname(e), lay, [(('pt', c), float(child(e, 'size')[1]) / 2)], 'via', f'({c[0]:.2f}, {c[1]:.2f})'))
        for z in children(root, 'zone'):
            net = netname(z)
            for fpoly in children(z, 'filled_polygon'):
                pts = [xy(q) for q in child(fpoly, 'pts')[1:] if q[0] == 'xy']
                if len(pts) >= 3:
                    self.objs.append(Obj(net, {child(fpoly, 'layer')[1]}, [(('poly', Poly(pts)), 0.0)], 'zone', f'({pts[0][0]:.2f}, {pts[0][1]:.2f})'))
        self.netless_copper = sum(1 for e in root if isinstance(e, list) and e and e[0].startswith('gr_')
                                  and child(e, 'layer') and child(e, 'layer')[1] in self.copper)


def pad_pieces(p, shape, c, prot):
    size = child(p, 'size')
    w, h = float(size[1]), float(size[2]) if len(size) > 2 else float(size[1])

    def world(q):
        r = rot(q, prot)
        return (c[0] + r[0], c[1] + r[1])

    def basic(shape, w, h):
        if shape == 'circle':
            return [(('pt', c), w / 2)]
        if shape == 'oval':
            if abs(w - h) < 1e-9:
                return [(('pt', c), w / 2)]
            d = abs(w - h) / 2
            a, b = ((-d, 0), (d, 0)) if w > h else ((0, -d), (0, d))
            return [(('seg', (world(a), world(b))), min(w, h) / 2)]
        rr = 0.0
        if shape == 'roundrect':
            rr = float(child(p, 'roundrect_rratio')[1]) * min(w, h)
        hw, hh = w / 2 - rr, h / 2 - rr
        if hw <= 1e-9 and hh <= 1e-9:
            return [(('pt', c), rr)]
        if hw <= 1e-9 or hh <= 1e-9:
            a, b = ((-hw, 0), (hw, 0)) if hw > hh else ((0, -hh), (0, hh))
            return [(('seg', (world(a), world(b))), rr)]
        return [(('poly', Poly([world(q) for q in ((-hw, -hh), (hw, -hh), (hw, hh), (-hw, hh))])), rr)]

    if shape != 'custom':
        if shape not in ('circle', 'oval', 'rect', 'roundrect'):
            warn(f'pad shape {shape} treated as rect')
        return basic(shape if shape in ('circle', 'oval', 'roundrect') else 'rect', w, h)
    opts = child(p, 'options')
    anchor = child(opts, 'anchor')[1] if opts and child(opts, 'anchor') else 'circle'
    pieces = basic('circle' if anchor == 'circle' else 'rect', w, h)
    prims = child(p, 'primitives') or []
    for g in prims[1:]:
        width = float(child(g, 'width')[1]) if child(g, 'width') else 0.0
        if g[0] == 'gr_poly':
            pts = [world(xy(q)) for q in child(g, 'pts')[1:] if q[0] == 'xy']
            pieces.append((('poly', Poly(pts)), width / 2))
        elif g[0] == 'gr_circle':
            cc, en = xy(child(g, 'center')), xy(child(g, 'end'))
            pieces.append((('pt', world(cc)), math.hypot(en[0] - cc[0], en[1] - cc[1]) + width / 2))
        elif g[0] == 'gr_line':
            pieces.append((('seg', (world(xy(child(g, 'start'))), world(xy(child(g, 'end'))))), width / 2))
        elif g[0] == 'gr_rect':
            (x0, y0), (x1, y1) = xy(child(g, 'start')), xy(child(g, 'end'))
            pieces.append((('poly', Poly([world(q) for q in ((x0, y0), (x1, y0), (x1, y1), (x0, y1))])), width / 2))
        elif g[0] == 'gr_arc':
            pts = arc_points(xy(child(g, 'start')), xy(child(g, 'mid')), xy(child(g, 'end')))
            pieces += [(('seg', (world(pts[k]), world(pts[k + 1]))), width / 2) for k in range(len(pts) - 1)]
        else:
            warn(f'custom pad primitive {g[0]} ignored')
    return pieces


# ---------------------------------------------------------------- ガーバー
def macro_eval(expr, args, var):
    def sub(m):
        k = int(m.group(1))
        return repr(var.get(k, args[k - 1] if k - 1 < len(args) else 0.0))
    e = re.sub(r'\$(\d+)', sub, expr.strip()).replace('x', '*').replace('X', '*')
    if not re.fullmatch(r'[\d.eE+\-*/() ]*', e):
        raise ValueError(f'bad macro expression {expr}')
    return float(eval(e, {'__builtins__': {}}, {}))


def macro_pieces(body, args):
    """アパーチャマクロを芯の並びに (原点まわり、ガーバーの座標)"""
    out = []
    var = {}
    for prim in body:
        prim = prim.strip()
        if not prim or prim.startswith('0'):
            continue
        if prim.startswith('$'):
            k, _, v = prim[1:].partition('=')
            var[int(k)] = macro_eval(v, args, var)
            continue
        f = [macro_eval(x, args, var) for x in prim.split(',')]
        code = int(f[0])

        def R(pt, deg):
            a = math.radians(deg)
            return (pt[0] * math.cos(a) - pt[1] * math.sin(a), pt[0] * math.sin(a) + pt[1] * math.cos(a))
        if f[1] == 0:
            warn(f'aperture macro with exposure off ignored (code {code})')
            continue
        if code == 1:
            out.append((('pt', R((f[3], f[4]), f[5] if len(f) > 5 else 0)), f[2] / 2))
        elif code == 4:
            n = int(f[2])
            pts = [R((f[3 + 2 * k], f[4 + 2 * k]), f[3 + 2 * (n + 1)]) for k in range(n + 1)]
            out.append((('poly', Poly(pts)), 0.0))
        elif code == 20:
            out.append((('seg', (R((f[3], f[4]), f[7]), R((f[5], f[6]), f[7]))), f[2] / 2))
        elif code == 21:
            w, h, cx, cy, deg = f[2], f[3], f[4], f[5], f[6]
            out.append((('poly', Poly([R((cx + sx * w / 2, cy + sy * h / 2), deg) for sx, sy in ((-1, -1), (1, -1), (1, 1), (-1, 1))])), 0.0))
        else:
            warn(f'aperture macro primitive {code} treated as nothing')
    return out


def read_gerber(path, layer):
    text = open(path, encoding='utf-8').read()
    macros, aps = {}, {}
    scale, dec = 1.0, 6
    objs = []
    attrs = {}
    cur = None
    x = y = 0.0
    mode = 'G01'
    region = None
    for tok in re.findall(r'%[^%]*%|[^%*]*\*', text):
        tok = tok.strip()
        if tok.startswith('%'):
            body = tok[1:-1].strip()
            if body.startswith('AM'):
                parts = re.sub(r'\s+', '', body).split('*')
                macros[parts[0][2:]] = parts[1:]
            elif body.startswith('ADD'):
                m = re.match(r'ADD(\d+)([^,*]+),?([^*]*)\*', body)
                num, name, params = int(m.group(1)), m.group(2), m.group(3)
                args = [float(v) for v in params.split('X') if v.strip()]
                if name == 'C':
                    pcs = [(('pt', (0.0, 0.0)), args[0] / 2)]
                elif name == 'R':
                    w, h = args[0] / 2, args[1] / 2
                    pcs = [(('poly', Poly([(-w, -h), (w, -h), (w, h), (-w, h)])), 0.0)]
                elif name == 'O':
                    w, h = args[0], args[1]
                    d = abs(w - h) / 2
                    pcs = [(('seg', ((-d, 0.0), (d, 0.0)) if w > h else ((0.0, -d), (0.0, d))), min(w, h) / 2)]
                elif name == 'P':
                    pcs = [(('pt', (0.0, 0.0)), args[0] / 2)]
                    warn(f'{os.path.basename(path)}: polygon aperture D{num} treated as a circle')
                elif name in macros:
                    pcs = macro_pieces(macros[name], args)
                else:
                    raise SystemExit(f'{path}: unknown aperture {name}')
                aps[num] = (name, args, pcs)
            elif body.startswith('FS'):
                m = re.search(r'X(\d)(\d)', body)
                dec = int(m.group(2))
            elif body.startswith('MO'):
                scale = 25.4 if 'IN' in body else 1.0
            elif body.startswith('LPC'):
                warn(f'{os.path.basename(path)}: clear polarity (LPC) not supported; result may be wrong')
            elif body.startswith('TO.'):
                k, _, v = body[3:].rstrip('*').partition(',')
                attrs[k] = v
            elif body.startswith('TD'):
                k = body[2:].rstrip('*').lstrip('.')
                if k:
                    attrs.pop(k, None)
                else:
                    attrs = {}
            continue
        t = tok[:-1].strip()
        if not t or t.startswith('G04') or t.startswith('M02'):
            continue
        words = dict(re.findall(r'([GXYIJD])(-?\d+)', t))
        if 'G' in words:
            g = int(words['G'])
            if g in (1, 2, 3):
                mode = f'G0{g}'
            elif g == 36:
                region = [[]]
                continue
            elif g == 37:
                for contour in region:
                    if len(contour) >= 3:
                        objs.append(Obj(attrs.get('N'), {layer}, [(('poly', Poly(contour)), 0.0)], 'region', f'({contour[0][0]:.2f}, {-contour[0][1]:.2f})'))
                region = None
                continue
        if 'D' not in words and 'X' not in words and 'Y' not in words:
            continue
        d = int(words['D']) if 'D' in words else 1
        if d >= 10 and 'X' not in words and 'Y' not in words:
            cur = d
            continue
        nx = int(words['X']) / 10 ** dec * scale if 'X' in words else x
        ny = int(words['Y']) / 10 ** dec * scale if 'Y' in words else y
        if d == 1:
            if mode == 'G01':
                path_pts = [(x, y), (nx, ny)]
            else:
                ci = int(words.get('I', 0)) / 10 ** dec * scale
                cj = int(words.get('J', 0)) / 10 ** dec * scale
                path_pts = arc_points((x, y), (x + ci, y + cj), (nx, ny), ccw=(mode == 'G03'))
            if region is not None:
                if not region[-1]:
                    region[-1].append((x, y))
                region[-1].extend(path_pts[1:])
            else:
                name, args, pcs = aps[cur]
                r = args[0] / 2
                if name != 'C':
                    warn(f'{os.path.basename(path)}: draw with non-circular aperture D{cur} treated as round')
                objs.append(Obj(attrs.get('N'), {layer}, [(('seg', (path_pts[k], path_pts[k + 1])), r) for k in range(len(path_pts) - 1)],
                                'track', f'({x:.2f}, {-y:.2f})-({nx:.2f}, {-ny:.2f})'))
        elif d == 2:
            if region is not None:
                region.append([])
        elif d == 3:
            name, args, pcs = aps[cur]
            moved = []
            for (k, v), r in pcs:
                if k == 'pt':
                    moved.append((('pt', (v[0] + nx, v[1] + ny)), r))
                elif k == 'seg':
                    moved.append((('seg', ((v[0][0] + nx, v[0][1] + ny), (v[1][0] + nx, v[1][1] + ny))), r))
                else:
                    moved.append((('poly', Poly([(q[0] + nx, q[1] + ny) for q in v.pts])), r))
            pad = None
            if 'P' in attrs:
                pp = attrs['P'].split(',')
                pad = (pp[0], pp[1])
            where = f'{pad[0]}.{pad[1]} ({nx:.2f}, {-ny:.2f})' if pad else f'({nx:.2f}, {-ny:.2f})'
            objs.append(Obj(attrs.get('N'), {layer}, moved, 'pad' if pad else 'flash', where, pad=pad))
        x, y = nx, ny
    return objs


def read_drill(path):
    holes, tools, cur = [], {}, None
    for line in open(path, encoding='utf-8'):
        line = line.strip()
        m = re.match(r'^T(\d+)C([\d.]+)', line)
        if m:
            tools[int(m.group(1))] = float(m.group(2))
            continue
        m = re.match(r'^T(\d+)$', line)
        if m:
            cur = int(m.group(1))
            continue
        m = re.match(r'^X(-?[\d.]+)Y(-?[\d.]+)', line)
        if m and cur in tools:
            if '.' not in m.group(1):
                warn(f'{os.path.basename(path)}: drill coordinates without decimal point are not supported')
                continue
            holes.append(((float(m.group(1)), float(m.group(2))), tools[cur] / 2))
    return holes


def check_gerbers(gdir, board, clearance):
    copper = []
    for f in sorted(glob.glob(os.path.join(gdir, '*.gbr')) + glob.glob(os.path.join(gdir, '*.gtl')) + glob.glob(os.path.join(gdir, '*.gbl'))):
        head = open(f, encoding='utf-8').read(4000)
        m = re.search(r'%TF\.FileFunction,Copper,L(\d+),(\w+)', head)
        if m:
            copper.append((int(m.group(1)), f))
    if not copper:
        err(f'gerbers: no copper layer (%TF.FileFunction,Copper) in {gdir}')
        return
    copper.sort()
    objs = []
    for n, f in copper:
        objs += read_gerber(f, f'L{n}')
    layers = {f'L{n}' for n, _ in copper}
    # 穴は、同じ場所にあるパッドやビアのネットにして、全部の層をつなぐ
    drills = [f for f in glob.glob(os.path.join(gdir, '*.drl')) if 'NPTH' not in os.path.basename(f).upper()]
    nholes = 0
    for f in drills:
        for c, r in read_drill(f):
            host = next((o for o in objs if o.kind in ('pad', 'flash') and o.pieces[0][0][0] == 'pt'
                         and math.hypot(o.pieces[0][0][1][0] - c[0], o.pieces[0][0][1][1] - c[1]) < 1e-3), None)
            host = host or next((o for o in objs if o.kind in ('pad', 'flash') and gap(o, Obj(None, layers, [(('pt', c), 0.0)], 'hole', ''), 0.01) <= 0), None)
            if host is None:
                warn(f'gerbers: plated hole at ({c[0]:.2f}, {-c[1]:.2f}) has no pad around it')
                continue
            objs.append(Obj(host.net, layers, [(('pt', c), r)], 'hole', f'({c[0]:.2f}, {-c[1]:.2f})', pad=host.pad))
            nholes += 1
    print(f'--    gerbers: {len(copper)} copper layers, {len(objs)} objects, {nholes} plated holes ({gdir})')
    nets = analyse(objs, clearance, 'gerber')
    # .kicad_pcb とパッドのネットを比べる
    if board:
        # 未接続のピン (unconnected-...) は名前が付け方で変わるので、どちらも「なし」として比べる
        def real(net):
            return None if not net or net.startswith('unconnected-') else net

        pcb = collections.defaultdict(set)
        for ref, info in board.footprints.items():
            for num, net, _ in info['pads']:
                pcb[(ref, num)].add(real(net))
        gbr = collections.defaultdict(set)
        for o in objs:
            if o.pad and o.kind == 'pad':
                gbr[o.pad].add(real(o.net))
        stale = [(p, gbr[p], pcb.get(p)) for p in sorted(gbr) if not gbr[p] <= pcb.get(p, set())]
        for p, g, k in stale:
            err(f'gerbers differ from the PCB: pad {p[0]}.{p[1]} is {sorted(n or "-" for n in g)} in the gerbers, '
                f'{sorted(n or "-" for n in k or [])} in the PCB (stale gerbers?)')
        missing = [p for p in pcb if p not in gbr and any(n for n in pcb[p])]
        smd_or_tht = {p for p in missing if any(o.pad == p for o in board.objs)}
        for p in sorted(smd_or_tht):
            err(f'gerbers differ from the PCB: pad {p[0]}.{p[1]} is not in the gerbers')
        if not stale and not smd_or_tht:
            ok('gerbers: every pad has the same net as in the PCB')
    return nets


# ---------------------------------------------------------------- 配線表
def ohms(v):
    m = re.fullmatch(r'\s*([\d.]+)\s*([kKmMrRΩ]?)\s*(\d*)\s*(ohm|Ω)?\s*', str(v))
    if not m:
        return None
    base = float(m.group(1))
    mult = {'k': 1e3, 'K': 1e3, 'M': 1e6, 'm': 1e-3}.get(m.group(2), 1.0)
    if m.group(3):   # 4k7
        base = float(f'{m.group(1)}.{m.group(3)}')
    return base * mult


def check_pins(board, pins_path):
    spec = json.load(open(pins_path, encoding='utf-8'))
    fps = board.footprints
    pico = [r for r, f in fps.items() if 'RaspberryPi_Pico' in f['lib'] or 'Pico' in f['value']]
    tang = [r for r, f in fps.items() if 'TangNano20K' in f['lib'] or 'Tang_Nano' in f['value']]
    if len(pico) != 1 or len(tang) != 1:
        err(f'pins: need one Pico and one Tang Nano 20K footprint, found Pico {pico}, Tang {tang}')
        return
    pico, tang = pico[0], tang[0]

    def pico_nets(pin):
        return {n for num, n, _ in fps[pico]['pads'] if num == str(pin)}

    def func(pf, num):
        # KiCad 10 はピンの機能の名前のあとに _<番号> を付ける (GND_39)
        return re.sub(rf'_{re.escape(num)}$', '', pf).upper()

    tang_by_fpga, tang_power = {}, []
    for num, net, pf in fps[tang]['pads']:
        m = re.match(r'(\d+)_', pf)
        if m:
            tang_by_fpga[int(m.group(1))] = (num, net)
        elif func(pf, num) in ('GND', '3V3', '5V', 'VCC', 'VIN'):
            tang_power.append((num, net, func(pf, num)))
    pads_on = collections.defaultdict(list)   # net -> [(ref, num)]
    for ref, f in fps.items():
        for num, net, _ in f['pads']:
            if net:
                pads_on[net].append((ref, num))
    resistors = [(ref, f) for ref, f in fps.items() if ref.startswith('R') and len(f['pads']) == 2]

    def find_res(n1, n2):
        return [(ref, f) for ref, f in resistors if {f['pads'][0][1], f['pads'][1][1]} == {n1, n2}]

    def board_pads(net):
        return sorted(f'{r}.{n}' for r, n in pads_on.get(net, []) if r in (pico, tang))

    used_nets = set()
    print(f'--    pins: {len(spec["pins"])} signals from {pins_path} (Pico = {pico}, Tang = {tang})')
    for s in spec['pins']:
        name, fpin, ppin = s['signal'], s['fpga_pin'], s['pico_pin']
        pn = pico_nets(ppin)
        real = {n for n in pn if n and not n.startswith('unconnected-')}
        if fpin not in tang_by_fpga:
            err(f'{name}: FPGA pin {fpin} is not on the Tang header footprint')
            continue
        tnum, tn = tang_by_fpga[fpin]
        if len(real) != 1:
            err(f'{name}: Pico pin {ppin} is {"not connected" if not real else "on several nets " + str(sorted(real))}')
            continue
        pnet = real.pop()
        if not tn or tn.startswith('unconnected-'):
            err(f'{name}: Tang pin {tnum} (FPGA {fpin}) is not connected')
            continue
        used_nets.update((pnet, tn))
        path = f'Pico {ppin} -> Tang {tnum} (FPGA {fpin})'
        good = True
        if s.get('series_r'):
            rs = find_res(pnet, tn)
            if len(rs) != 1:
                err(f'{name}: expected one series resistor between {pnet} (Pico {ppin}) and {tn} (Tang {tnum}), found {[r for r, _ in rs]}')
                good = False
            else:
                want, have = ohms(s['series_r']), ohms(rs[0][1]['value'])
                if want is None or have is None or abs(want - have) > 1e-6 * max(want, 1):
                    err(f'{name}: series resistor {rs[0][0]} is {rs[0][1]["value"]}, pins says {s["series_r"]}')
                    good = False
                path = f'Pico {ppin} -> {rs[0][0]} {rs[0][1]["value"]} -> Tang {tnum} (FPGA {fpin})'
        elif pnet != tn:
            err(f'{name}: Pico {ppin} is on {pnet}, Tang {tnum} (FPGA {fpin}) is on {tn}')
            continue
        if s.get('pulldown'):
            rs = find_res(tn, 'GND') or find_res(pnet, 'GND')
            if len(rs) != 1:
                err(f'{name}: expected one pull-down resistor to GND, found {[r for r, _ in rs]}')
                good = False
            elif ohms(rs[0][1]['value']) != ohms(s['pulldown']):
                err(f'{name}: pull-down {rs[0][0]} is {rs[0][1]["value"]}, pins says {s["pulldown"]}')
                good = False
            else:
                path += f', pull-down {rs[0][0]} {rs[0][1]["value"]}'
        expect = {f'{pico}.{ppin}', f'{tang}.{tnum}'}
        extra = sorted(set(board_pads(pnet) + board_pads(tn)) - expect)
        if extra:
            err(f'{name}: other Pico/Tang pins on the same net: {extra}')
            good = False
        if good:
            ok(f'{name}: {path}')

    for pin in spec.get('gnd_pico_pins', []):
        if 'GND' not in pico_nets(pin):
            err(f'GND: Pico pin {pin} is not on GND ({sorted(n or "-" for n in pico_nets(pin))})')
    bad_gnd = [(num, net) for num, net, pf in tang_power if pf == 'GND' and net != 'GND']
    for num, net in bad_gnd:
        err(f'GND: Tang pin {num} (GND) is on {net}')
    if not bad_gnd and all('GND' in pico_nets(p) for p in spec.get('gnd_pico_pins', [])):
        ok(f'GND: Pico {spec.get("gnd_pico_pins", [])}, Tang {sorted((n for n, _, pf in tang_power if pf == "GND"), key=int)}')

    # 電源どうしをつないでいないか (ハーネスは USB を 2 本にして GND だけ共通にする)
    rails = [(f'{pico}.{num}', net, pf) for num, net, pf in fps[pico]['pads'] if func(pf, num) in ('VBUS', 'VSYS', '3V3')]
    rails += [(f'{tang}.{num}', net, pf) for num, net, pf in tang_power if pf != 'GND']
    by_net = collections.defaultdict(set)
    for pad, net, pf in rails:
        if net and not net.startswith('unconnected-'):
            by_net[net].add(pad.split('.')[0])
    tied = {n: r for n, r in by_net.items() if len(r) > 1}
    for n, r in tied.items():
        err(f'power: {n} ties the Pico and Tang supplies together')
    if not tied:
        ok('power: no supply pin is tied between the Pico and the Tang')
    for net, pads in sorted(pads_on.items()):
        if net in used_nets or net == 'GND' or net.startswith('unconnected-'):
            continue
        on_boards = [f'{r}.{n}' for r, n in pads if r in (pico, tang)]
        if on_boards:
            print(f'INFO  net {net} is not in the pins file: {" ".join(sorted({f"{r}.{n}" for r, n in pads}))}')


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('pcb')
    ap.add_argument('--pins')
    ap.add_argument('--gerbers')
    ap.add_argument('--clearance', type=float)
    a = ap.parse_args()

    clearance = a.clearance
    pro = os.path.splitext(a.pcb)[0] + '.kicad_pro'
    if clearance is None and os.path.exists(pro):
        classes = json.load(open(pro, encoding='utf-8')).get('net_settings', {}).get('classes', [])
        default = next((c for c in classes if c.get('name') == 'Default'), classes[0] if classes else {})
        clearance = default.get('clearance')
    clearance = clearance or 0.2
    print(f'--    {a.pcb} (clearance {clearance} mm)')

    board = Board(a.pcb)
    if board.netless_copper:
        warn(f'pcb: {board.netless_copper} graphic items on copper layers are not checked')
    if a.pins:
        check_pins(board, a.pins)
    analyse(board.objs, clearance, 'pcb')
    if a.gerbers:
        check_gerbers(a.gerbers, board, clearance)

    if errors:
        print(f'FAIL: {len(errors)} errors, {len(warnings)} warnings')
        return 1
    print(f'PASS: {len(warnings)} warnings')
    return 0


if __name__ == '__main__':
    sys.exit(main())
