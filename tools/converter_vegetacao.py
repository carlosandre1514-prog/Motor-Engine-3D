#!/usr/bin/env python3
"""Converte o kit de plantas (Obj.obj do 3ds Max, ~330 MB, tudo cinza) em modelos leves para o celular.

Para cada planta escolhida gera três .glb em shaders/kit/vegetacao/:
  <nome>.glb              perto (poucos milhares de triângulos)
  <nome>_longe.glb        longe (cerca de mil)
  <nome>_muito_longe.glb  muito longe (poucas centenas)
O que faz:
  - separa folhas (pedacinhos soltos da malha) de tronco/galhos (pedaços grandes);
  - folhas: fica só uma parte delas, cada uma maior (cobre a mesma área), normal "inflada" para fora (copa macia);
  - tronco: simplificado juntando vértices numa grade;
  - pinta por vértice (o arquivo não tem texturas): folhas em verdes variados, tronco marrom, capim
    verde-amarelado, floreira de concreto com terra; unidades de milímetro para metro, base no chão.
Uso:
  python3 tools/converter_vegetacao.py Obj.obj            (separa e converte)
"""
import json
import os
import struct
import sys

import numpy as np

OUT = os.path.join(os.path.dirname(__file__), "..", "shaders", "kit", "vegetacao")
RNG = np.random.default_rng(7)

# nome de saída: (objeto no OBJ, tipo, folhas perto, folhas longe)
PLANTS = {
    "arvore_1": ("plant1", "arvore", 3300, 900),
    "arvore_2": ("plant2", "arvore", 3300, 900),
    "arvore_3": ("plant4", "arvore", 3300, 900),
    "arvore_4": ("palnts3", "arvore", 3300, 900),
    "arvore_5": ("Object021", "arvore", 2750, 800),
    "arvores_dupla": ("Object020", "arvore", 3300, 900),
    "arvores_trio": ("plant4_2", "arvore", 3300, 900),
    "arbusto_redondo_1": ("plant007", "redondo", 2750, 700),
    "arbusto_redondo_2": ("plant008", "redondo", 2750, 700),
    "arbusto_1": ("plant_009", "arbusto", 2750, 700),
    "arbusto_2": ("plant_017", "arbusto", 2750, 700),
    "arbusto_3": ("plant_018", "arbusto", 2750, 700),
    "capim_1": ("plant_011", "capim", 1650, 500),
    "capim_2": ("plant_013", "capim", 1650, 500),
    "capim_3": ("plant_016", "capim", 1650, 500),
    "capim_florido": ("plant3", "florido", 2750, 700),
    "forracao_1": ("Object012", "forracao", 2200, 600),
    "forracao_2": ("Object024", "forracao", 2200, 600),
    "forracao_3": ("Object025", "forracao", 2200, 600),
    "floreira_1": ("Box003", "floreira", 0, 0),
    "floreira_2": ("Box004", "floreira", 0, 0),
    "floreira_3": ("Object019", "floreira", 0, 0),
}


def read_obj(path, wanted):
    """Lê só os objetos pedidos (posições + faces trianguladas)."""
    V, objs, cur = [], {}, None
    seen = {}
    with open(path, "r", errors="ignore") as f:
        for line in f:
            c = line[:2]
            if c == "v ":
                V.append(line)
            elif c in ("o ", "g "):
                name = line[2:].strip()
                if cur is None or cur[0] != name:
                    seen[name] = seen.get(name, 0) + 1
                    key = name if seen[name] == 1 else f"{name}_{seen[name]}"
                    cur = (name, key)
                    if key in wanted:
                        objs[key] = []
            elif c == "f " and cur and cur[1] in objs:
                idx = [int(p.split("/")[0]) - 1 for p in line.split()[1:]]
                for k in range(1, len(idx) - 1):
                    objs[cur[1]].append((idx[0], idx[k], idx[k + 1]))
    P = np.array([l.split()[1:4] for l in V], dtype=np.float32)
    out = {}
    for key, faces in objs.items():
        F = np.array(faces, dtype=np.int64)
        used = np.unique(F)
        remap = np.zeros(F.max() + 1, dtype=np.int64)
        remap[used] = np.arange(len(used))
        out[key] = (P[used].astype(np.float64), remap[F])
    return out


def components(F, nv):
    lab = np.arange(nv)
    while True:
        m = lab[F].min(1)
        new = lab.copy()
        for k in range(3):
            np.minimum.at(new, F[:, k], m)
        while True:
            n2 = new[new]
            if np.array_equal(n2, new):
                break
            new = n2
        if np.array_equal(new, lab):
            return lab
        lab = new


def cluster(P, F, cell):
    """Simplifica juntando vértices numa grade (tronco/galhos)."""
    if len(F) == 0:
        return P[:0], F[:0]
    key = np.floor(P / cell).astype(np.int64)
    _, inv = np.unique(key, axis=0, return_inverse=True)
    inv = inv.reshape(-1)
    n = inv.max() + 1
    Q = np.zeros((n, 3))
    np.add.at(Q, inv, P)
    Q /= np.bincount(inv, minlength=n)[:, None]
    G = inv[F]
    ok = (G[:, 0] != G[:, 1]) & (G[:, 1] != G[:, 2]) & (G[:, 0] != G[:, 2])
    G = np.unique(np.sort(G[ok], axis=1), axis=0) if ok.any() else G[:0]
    return Q, G


def limit_wood(P, F, target):
    if len(F) <= target:
        return P, F
    ext = (P.max(0) - P.min(0)).max()
    cell = ext / 400
    for _ in range(14):
        Q, G = cluster(P, F, cell)
        if len(G) <= target:
            return Q, G
        cell *= 1.35
    return Q, G


def thin_leaves(P, F, lab, keep_tris, grow_max):
    """Mantém só parte das folhas (pedaços inteiros), aumentando as que ficam."""
    fl = lab[F[:, 0]]
    comps, start = np.unique(fl, return_index=True)
    total = len(F)
    if total <= keep_tris:
        return P, F, fl
    frac = keep_tris / total
    keep = RNG.random(len(comps)) < frac
    kept_ids = comps[keep]
    mask = np.isin(fl, kept_ids)
    F2 = F[mask]
    fl2 = fl[mask]
    grow = min(grow_max, frac ** -0.42)
    P2 = P.copy()
    # cada folha cresce em volta do próprio centro
    used = np.unique(F2)
    vlab = lab[used]
    cs = {}
    sums = np.zeros((lab.max() + 1, 3)); cnt = np.zeros(lab.max() + 1)
    np.add.at(sums, vlab, P[used]); np.add.at(cnt, vlab, 1)
    cen = sums[vlab] / np.maximum(cnt[vlab], 1)[:, None]
    P2[used] = cen + (P[used] - cen) * grow
    return P2, F2, fl2


def compact(P, F):
    used, inv = np.unique(F, return_inverse=True)
    return P[used], inv.reshape(F.shape)


def soft_normals(P, F, center, puff):
    """Normal da face misturada com a direção para fora da copa (folhagem com luz macia)."""
    n = np.zeros_like(P)
    T = P[F]
    fn = np.cross(T[:, 1] - T[:, 0], T[:, 2] - T[:, 0])
    for k in range(3):
        np.add.at(n, F[:, k], fn)
    n /= np.linalg.norm(n, axis=1, keepdims=True) + 1e-12
    out = P - center
    out /= np.linalg.norm(out, axis=1, keepdims=True) + 1e-12
    n = np.where((np.sum(n * out, 1) < 0)[:, None], -n, n)  # folha de dupla face: vira para fora
    m = n * (1 - puff) + out * puff
    return m / (np.linalg.norm(m, axis=1, keepdims=True) + 1e-12)


def leaf_colors(n, base, var, comp_ids, P, H, kind):
    rnd = RNG.random(comp_ids.max() + 1)
    t = rnd[comp_ids][:, None]
    c = np.array(base)[None, :] * (1 - var + 2 * var * t)
    c[:, 0] *= 1 + 0.35 * (rnd[comp_ids] > 0.85)  # algumas mais amareladas
    # sombra por dentro / embaixo da copa
    h = (P[:, 1] - P[:, 1].min()) / max(H, 1e-6)
    c *= (0.65 + 0.45 * h)[:, None]
    if kind == "florido":  # pontas com flor roxa
        tip = h > 0.72
        c[tip] = np.array([0.32, 0.12, 0.38]) * (0.8 + 0.4 * t[tip])
    return np.clip(c, 0, 1)


def build(P, F, kind, leaf_tris, far):
    P = P * 0.001  # milímetros -> metros
    lo = P.min(0)
    P = P - np.array([(P[:, 0].min() + P[:, 0].max()) / 2, lo[1], (P[:, 2].min() + P[:, 2].max()) / 2])
    H = P[:, 1].max()
    parts = []
    if kind == "floreira":
        T = P[F]
        fn = np.cross(T[:, 1] - T[:, 0], T[:, 2] - T[:, 0])
        fn /= np.linalg.norm(fn, axis=1, keepdims=True) + 1e-12
        top = (fn[:, 1] > 0.9) & (T[:, :, 1].mean(1) < H * 0.97)  # terra dentro (um pouco abaixo da borda)
        Pf = T.reshape(-1, 3); Ff = np.arange(len(Pf)).reshape(-1, 3)
        N = np.repeat(fn, 3, axis=0)
        C = np.repeat(np.where(top[:, None], [0.07, 0.045, 0.03], [0.42, 0.41, 0.39]), 3, axis=0)
        parts.append((Pf, Ff, N, C, False))
        return parts, H
    lab = components(F, len(P))
    fl = lab[F[:, 0]]
    comps, cnt = np.unique(fl, return_counts=True)
    size_of = dict(zip(comps, cnt))
    wood_comp = np.zeros(lab.max() + 1, bool)
    if kind == "arvore":
        wood_comp[comps[cnt >= 24]] = True
    is_wood = wood_comp[fl]
    center = np.array([0, H * (0.62 if kind == "arvore" else 0.5), 0])
    # tronco
    if is_wood.any():
        Pw, Fw = compact(P, F[is_wood])
        Pw, Fw = limit_wood(Pw, Fw, (60 if leaf_tris < 300 else 400) if far else 1500)
        if len(Fw):
            Nw = soft_normals(Pw, Fw, np.array([0, Pw[:, 1].mean(), 0]), 0.0)
            Cw = np.tile([0.16, 0.10, 0.06], (len(Pw), 1)) * (0.8 + 0.4 * RNG.random((len(Pw), 1)))
            parts.append((Pw, Fw, Nw, Cw, False))
    # folhas
    Fl = F[~is_wood]
    if len(Fl):
        grow_max = {"arvore": 3.2, "redondo": 3.6, "arbusto": 3.0, "capim": 1.0, "florido": 1.6, "forracao": 2.2}[kind]
        if kind == "capim":  # lâminas inteiras: fica só uma parte delas, do tamanho normal
            grow_max = 1.0
        P2, F2, comp2 = thin_leaves(P, Fl, lab, leaf_tris, grow_max)
        used, inv = np.unique(F2, return_inverse=True)
        Pl = P2[used]; Fl2 = inv.reshape(F2.shape)
        vcomp = lab[used]
        puff = {"arvore": 0.75, "redondo": 0.85, "arbusto": 0.7, "capim": 0.3, "florido": 0.4, "forracao": 0.6}[kind]
        Nl = soft_normals(Pl, Fl2, center, puff)
        base = {"arvore": [0.07, 0.17, 0.035], "redondo": [0.05, 0.16, 0.035], "arbusto": [0.08, 0.2, 0.045],
                "capim": [0.2, 0.27, 0.06], "florido": [0.12, 0.22, 0.05], "forracao": [0.06, 0.21, 0.05]}[kind]
        Cl = leaf_colors(len(Pl), base, 0.28, vcomp, Pl, H, kind)
        parts.append((Pl, Fl2, Nl, Cl, True))
    return parts, H


def write_glb(path, parts):
    bufs, views, accs, prims, mats = [], [], [], [], []
    mats.append({"name": "planta", "pbrMetallicRoughness": {"baseColorFactor": [1, 1, 1, 1], "metallicFactor": 0, "roughnessFactor": 0.9},
                 "doubleSided": False})
    mats.append({"name": "folhas", "pbrMetallicRoughness": {"baseColorFactor": [1, 1, 1, 1], "metallicFactor": 0, "roughnessFactor": 0.85},
                 "doubleSided": True})
    off = 0

    def add(data, target, comp, typ, count, mn=None, mx=None):
        nonlocal off
        b = data.tobytes()
        pad = (-len(b)) % 4
        bufs.append(b + b"\0" * pad)
        views.append({"buffer": 0, "byteOffset": off, "byteLength": len(b), "target": target})
        a = {"bufferView": len(views) - 1, "componentType": comp, "count": count, "type": typ}
        if mn is not None:
            a["min"], a["max"] = mn, mx
        accs.append(a)
        off += len(b) + pad
        return len(accs) - 1

    for P, F, N, C, leaves in parts:
        p = add(P.astype("<f4"), 34962, 5126, "VEC3", len(P), P.min(0).tolist(), P.max(0).tolist())
        n = add(N.astype("<f4"), 34962, 5126, "VEC3", len(N))
        c4 = np.concatenate([C, np.ones((len(C), 1))], 1).astype("<f4")
        c = add(c4, 34962, 5126, "VEC4", len(C))
        i = add(F.astype("<u4").reshape(-1), 34963, 5125, "SCALAR", F.size)
        prims.append({"attributes": {"POSITION": p, "NORMAL": n, "COLOR_0": c}, "indices": i, "material": 1 if leaves else 0})
    gl = {"asset": {"version": "2.0", "generator": "converter_vegetacao.py"}, "scene": 0, "scenes": [{"nodes": [0]}],
          "nodes": [{"mesh": 0, "name": os.path.basename(path)[:-4]}], "meshes": [{"primitives": prims}],
          "materials": mats, "accessors": accs, "bufferViews": views, "buffers": [{"byteLength": off}]}
    js = json.dumps(gl, separators=(",", ":")).encode()
    js += b" " * ((-len(js)) % 4)
    binb = b"".join(bufs)
    with open(path, "wb") as f:
        f.write(struct.pack("<III", 0x46546C67, 2, 12 + 8 + len(js) + 8 + len(binb)))
        f.write(struct.pack("<II", len(js), 0x4E4F534A)); f.write(js)
        f.write(struct.pack("<II", len(binb), 0x004E4942)); f.write(binb)


def main():
    os.makedirs(OUT, exist_ok=True)
    wanted = {v[0] for v in PLANTS.values()}
    objs = read_obj(sys.argv[1], wanted)
    for name, (src, kind, near, far) in PLANTS.items():
        if src not in objs:
            print("faltou", src)
            continue
        P, F = objs[src]
        for suffix, leaves, is_far in (("", near, False), ("_longe", far, True), ("_muito_longe", max(far // 2, 80), True)):
            if kind == "floreira" and is_far:
                continue
            parts, H = build(P, F, kind, leaves, is_far)
            write_glb(os.path.join(OUT, name + suffix + ".glb"), parts)
            tris = sum(len(p[1]) for p in parts)
            print(f"{name + suffix:24s} altura {H:5.2f} m  triângulos {tris}")


if __name__ == "__main__":
    main()
