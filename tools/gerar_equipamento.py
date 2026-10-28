#!/usr/bin/env python3
"""Equipamento 3D do personagem base (colete, cinto tático com carregadores e radinho, capacete, boné para trás,
corrente e relógio de ouro, brincos). Cada peça é um .glb na pose de repouso do personagem (metros, Y para cima,
de frente para +Z) e vai presa a um osso; o motor segue o osso nas animações.

As medidas vêm do próprio corpo: o mapa de posições e o de regiões que o tools/personagem_fbx grava. O colete e o
cinto seguem a seção do tronco em cada altura (ficam justos no corpo).

Uso: gerar_equipamento.py REGIOES.png POSICOES.png PASTA_SAIDA
Grava PASTA_SAIDA/<peça>.glb e PASTA_SAIDA/equipamento.txt (peça e osso).
"""
import json, math, os, struct, sys
import numpy as np
from PIL import Image, ImageDraw, ImageFont

reg_path, pos_path, out_dir = sys.argv[1:4]
os.makedirs(out_dir, exist_ok=True)
FONT_DIR = os.path.join(os.path.dirname(__file__), '..', 'shaders', 'materials', 'fontes')
R_HEAD, R_NECK, R_HIPS, R_TORSO, R_UPPER, R_FORE, R_HAND, R_THIGH, R_SHIN, R_FOOT = range(1, 11)

reg = np.asarray(Image.open(reg_path))
pos = np.asarray(Image.open(pos_path)).astype(np.float32)
with open(pos_path + '.txt') as f:
    lines = f.read().split('\n')
mn = np.array([float(v) for v in lines[0].split()]); mx = np.array([float(v) for v in lines[1].split()])
BONES = {}
for ln in lines[2:]:
    p = ln.split()
    if len(p) == 4:
        BONES[p[0].split(':')[-1]] = np.array([float(v) for v in p[1:]])
ok = pos[..., 3] > 0
PTS = (mn + pos[..., :3] / 255.0 * (mx - mn))[ok]
REG = (reg[..., 0] // 20)[ok]

def pts(*regions):
    m = np.isin(REG, regions)
    return PTS[m]

# ---------------- malha
class Mesh:
    def __init__(self):
        self.prims = {}  # material -> (pos, nrm, uv, idx)
    def add(self, mat, P, N, UV, I):
        p = self.prims.setdefault(mat, [[], [], [], []])
        base = len(p[0])
        p[0] += [list(map(float, v)) for v in P]; p[1] += [list(map(float, v)) for v in N]
        p[2] += [list(map(float, v)) for v in UV]; p[3] += [i + base for i in I]

def normalize(v):
    l = np.linalg.norm(v, axis=-1, keepdims=True)
    return v / np.maximum(l, 1e-9)

def grid_mesh(mesh, mat, G, close_u=True, uv_scale=(1, 1)):
    """G[i][j] = ponto (anel i, ângulo j). Normais pela média das faces."""
    G = np.asarray(G, np.float64)
    ni, nj = G.shape[:2]
    jn = nj if close_u else nj - 1
    N = np.zeros_like(G)
    for i in range(ni - 1):
        for j in range(jn):
            j2 = (j + 1) % nj
            a, b, c, d = G[i, j], G[i, j2], G[i + 1, j2], G[i + 1, j]
            n = np.cross(b - a, d - a) + np.cross(c - b, a - b)
            for (ii, jj) in ((i, j), (i, j2), (i + 1, j2), (i + 1, j)):
                N[ii, jj] += n
    N = normalize(N)
    P, NN, UV, I = [], [], [], []
    idx = {}
    for i in range(ni):
        for j in range(nj):
            idx[(i, j)] = len(P)
            P.append(G[i, j]); NN.append(N[i, j]); UV.append((j / max(1, nj - 1) * uv_scale[0], i / max(1, ni - 1) * uv_scale[1]))
    for i in range(ni - 1):
        for j in range(jn):
            j2 = (j + 1) % nj
            a, b, c, d = idx[(i, j)], idx[(i, j2)], idx[(i + 1, j2)], idx[(i + 1, j)]
            I += [a, d, b, b, d, c]
    mesh.add(mat, P, NN, UV, I)

def box(mesh, mat, center, size, rot_y=0.0, tilt=None):
    c = np.array(center, float); h = np.array(size, float) / 2
    cy, sy = math.cos(rot_y), math.sin(rot_y)
    R = np.array([[cy, 0, sy], [0, 1, 0], [-sy, 0, cy]])
    if tilt is not None:  # gira em X
        ct, st = math.cos(tilt), math.sin(tilt)
        R = R @ np.array([[1, 0, 0], [0, ct, -st], [0, st, ct]])
    faces = [((1, 0, 0), (0, 1, 0), (0, 0, 1)), ((-1, 0, 0), (0, 0, 1), (0, 1, 0)), ((0, 1, 0), (0, 0, 1), (1, 0, 0)),
             ((0, -1, 0), (1, 0, 0), (0, 0, 1)), ((0, 0, 1), (1, 0, 0), (0, 1, 0)), ((0, 0, -1), (0, 1, 0), (1, 0, 0))]
    P, N, UV, I = [], [], [], []
    for n, u, v in faces:
        n, u, v = np.array(n, float), np.array(u, float), np.array(v, float)
        b = len(P)
        for su, sv in ((-1, -1), (1, -1), (1, 1), (-1, 1)):
            local = n * h + u * h * su + v * h * sv
            P.append(c + R @ local); N.append(R @ n); UV.append(((su + 1) / 2, (sv + 1) / 2))
        # sentido anti-horário visto de fora
        if np.dot(np.cross(u, v), n) > 0:
            I += [b, b + 1, b + 2, b, b + 2, b + 3]
        else:
            I += [b, b + 2, b + 1, b, b + 3, b + 2]
    mesh.add(mat, P, N, UV, I)

def tube(mesh, mat, path, radius, seg=8, closed=False, bumps=0.0):
    """Tubo ao longo de `path` (corrente, alça, antena). bumps: elos (raio varia)."""
    path = np.asarray(path, float)
    n = len(path)
    G = []
    for i in range(n):
        a = path[(i - 1) % n] if (closed or i > 0) else path[i]
        b = path[(i + 1) % n] if (closed or i < n - 1) else path[i]
        t = normalize(b - a)
        ref = np.array([0, 1, 0]) if abs(t[1]) < 0.9 else np.array([1, 0, 0])
        u = normalize(np.cross(t, ref)); v = np.cross(t, u)
        r = radius * (1 + bumps * (0.5 + 0.5 * math.cos(i * math.pi)))
        G.append([path[i] + r * (math.cos(2 * math.pi * k / seg) * u + math.sin(2 * math.pi * k / seg) * v) for k in range(seg)])
    if closed:
        G.append(G[0])
    grid_mesh(mesh, mat, G)

def ellipsoid_cap(mesh, mat, center, radii, keep, rings=12, seg=28):
    """Calota de elipsoide; keep(theta, phi) diz se o ponto fica (corta a aba/rosto)."""
    G = []
    for i in range(rings + 1):
        phi = math.pi / 2 * i / rings  # 0 = topo
        row = []
        for j in range(seg):
            th = 2 * math.pi * j / seg
            d = np.array([math.sin(phi) * math.sin(th), math.cos(phi), math.sin(phi) * math.cos(th)])
            row.append(np.array(center) + d * np.array(radii))
        G.append(row)
    G = np.asarray(G)
    # aba inferior: cada coluna desce até onde keep permite
    for j in range(seg):
        th = 2 * math.pi * j / seg
        lim = keep(th)  # fração 0..1 de quanto desce (1 = até o equador)
        for i in range(rings + 1):
            phi_max = math.pi / 2 * lim
            phi = min(math.pi / 2 * i / rings, phi_max)
            d = np.array([math.sin(phi) * math.sin(th), math.cos(phi), math.sin(phi) * math.cos(th)])
            G[i, j] = np.array(center) + d * np.array(radii)
    grid_mesh(mesh, mat, G)

# ---------------- seção do tronco em cada altura
def section(y, regions, sectors=36, band=0.012, center=None, extra=0.0):
    P = pts(*regions)
    sel = P[np.abs(P[:, 1] - y) < band]
    if len(sel) < 20:
        sel = P[np.abs(P[:, 1] - y) < band * 3]
    cx, cz = (np.median(sel[:, 0]), np.median(sel[:, 2])) if center is None else center
    ang = np.arctan2(sel[:, 0] - cx, sel[:, 2] - cz)
    r = np.hypot(sel[:, 0] - cx, sel[:, 2] - cz)
    out = np.zeros(sectors)
    for k in range(sectors):
        a0 = -math.pi + 2 * math.pi * k / sectors
        m = (ang >= a0) & (ang < a0 + 2 * math.pi / sectors)
        out[k] = np.percentile(r[m], 95) if m.sum() > 2 else np.nan
    # buracos (setores vazios): interpola
    good = ~np.isnan(out)
    if good.sum() < 3:
        out[:] = np.nanmean(out) if good.any() else 0.12
    else:
        idx = np.arange(sectors)
        out = np.interp(idx, idx[good], out[good], period=sectors)
    # suaviza e aplica folga
    out = np.convolve(np.r_[out[-2:], out, out[:2]], np.ones(5) / 5, mode='valid')
    return (cx, cz), out + extra

def loft(mesh, mat, ys, regions, extra, sectors=36):
    G = []
    for y in ys:
        (cx, cz), r = section(y, regions, sectors, extra=extra)
        row = []
        for k in range(sectors):
            a = -math.pi + 2 * math.pi * (k + 0.5) / sectors
            row.append((cx + r[k] * math.sin(a), y, cz + r[k] * math.cos(a)))
        G.append(row)
    G = G[::-1]  # de cima para baixo: normais para fora
    grid_mesh(mesh, mat, G)
    return G

def surface_at(y, angle, regions, extra):
    (cx, cz), r = section(y, regions, 72, extra=extra)
    k = int((angle + math.pi) / (2 * math.pi) * 72) % 72
    return np.array([cx + r[k] * math.sin(angle), y, cz + r[k] * math.cos(angle)]), np.array([math.sin(angle), 0, math.cos(angle)])

# ---------------- glb
def write_glb(path, mesh, materials, textures=None):
    textures = textures or {}
    g = {'asset': {'version': '2.0', 'generator': 'Motor 3D gerar_equipamento'}, 'scene': 0, 'scenes': [{'nodes': [0]}],
         'nodes': [{'name': os.path.splitext(os.path.basename(path))[0], 'mesh': 0}], 'meshes': [{'primitives': []}],
         'materials': [], 'accessors': [], 'bufferViews': [], 'buffers': []}
    bin_ = bytearray()
    def view(data, target):
        while len(bin_) % 4:
            bin_.append(0)
        g['bufferViews'].append({'buffer': 0, 'byteOffset': len(bin_), 'byteLength': len(data), **({'target': target} if target else {})})
        bin_.extend(data)
        return len(g['bufferViews']) - 1
    tex_index = {}
    for name, png in textures.items():
        if 'images' not in g:
            g['images'], g['textures'], g['samplers'] = [], [], [{'magFilter': 9729, 'minFilter': 9987}]
        g['images'].append({'bufferView': view(png, 0), 'mimeType': 'image/png'})
        g['textures'].append({'sampler': 0, 'source': len(g['images']) - 1})
        tex_index[name] = len(g['textures']) - 1
    for mat, (P, N, UV, I) in mesh.prims.items():
        md = materials[mat]
        m = {'name': mat, 'pbrMetallicRoughness': {'baseColorFactor': md['cor'] + [1.0], 'metallicFactor': md.get('metal', 0.0),
                                                   'roughnessFactor': md.get('rug', 0.8)}, 'doubleSided': True}
        if md.get('textura'):
            m['pbrMetallicRoughness']['baseColorTexture'] = {'index': tex_index[md['textura']]}
            m['alphaMode'] = 'MASK'; m['alphaCutoff'] = 0.5
        g['materials'].append(m)
        P = np.asarray(P, np.float32); N = np.asarray(N, np.float32); UV = np.asarray(UV, np.float32); I = np.asarray(I, np.uint32)
        a = len(g['accessors'])
        g['accessors'] += [
            {'bufferView': view(P.tobytes(), 34962), 'componentType': 5126, 'count': len(P), 'type': 'VEC3', 'min': P.min(0).tolist(), 'max': P.max(0).tolist()},
            {'bufferView': view(N.tobytes(), 34962), 'componentType': 5126, 'count': len(N), 'type': 'VEC3'},
            {'bufferView': view(UV.tobytes(), 34962), 'componentType': 5126, 'count': len(UV), 'type': 'VEC2'},
            {'bufferView': view(I.tobytes(), 34963), 'componentType': 5125, 'count': len(I), 'type': 'SCALAR'}]
        g['meshes'][0]['primitives'].append({'attributes': {'POSITION': a, 'NORMAL': a + 1, 'TEXCOORD_0': a + 2}, 'indices': a + 3,
                                             'material': len(g['materials']) - 1})
    while len(bin_) % 4:
        bin_.append(0)
    g['buffers'] = [{'byteLength': len(bin_)}]
    js = json.dumps(g, separators=(',', ':')).encode()
    while len(js) % 4:
        js += b' '
    with open(path, 'wb') as f:
        f.write(struct.pack('<III', 0x46546C67, 2, 12 + 8 + len(js) + 8 + len(bin_)))
        f.write(struct.pack('<II', len(js), 0x4E4F534A)); f.write(js)
        f.write(struct.pack('<II', len(bin_), 0x004E4942)); f.write(bytes(bin_))

def label_png(text, color=(235, 235, 235), w=512, h=128):
    fonts = sorted(f for f in os.listdir(FONT_DIR) if f.lower().endswith('.ttf'))
    im = Image.new('RGBA', (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    size = 110
    while size > 20:
        fnt = ImageFont.truetype(os.path.join(FONT_DIR, fonts[0]), size)
        if d.textlength(text, font=fnt) < w * 0.92:
            break
        size -= 6
    tw = d.textlength(text, font=fnt)
    d.text(((w - tw) / 2, (h - size) / 2 - 8), text, fill=color + (255,), font=fnt)
    import io
    b = io.BytesIO(); im.save(b, 'PNG')
    return b.getvalue()

def quad(mesh, mat, center, right, up, w, h, normal):
    c = np.array(center); r = np.array(right) * w / 2; u = np.array(up) * h / 2
    P = [c - r - u, c + r - u, c + r + u, c - r + u]
    N = [normal] * 4
    UV = [(0, 1), (1, 1), (1, 0), (0, 0)]
    mesh.add(mat, P, N, UV, [0, 1, 2, 0, 2, 3])

# ---------------- medidas
hip = BONES['Hips']; spine2 = BONES['Spine2']; neck = BONES['Neck']; head = BONES['Head']
head_pts = pts(R_HEAD)
head_top = head_pts[:, 1].max()
hc = np.array([np.median(head_pts[:, 0]), (head_top + head[1]) / 2 + 0.03, np.median(head_pts[head_pts[:, 1] > head[1] + 0.05][:, 2])])
hr = np.array([np.percentile(np.abs(head_pts[:, 0] - hc[0]), 99), head_top - hc[1], np.percentile(np.abs(head_pts[:, 2] - hc[2]), 99)])
# crânio: esfera achatada que passa pelo topo da cabeça (centro na altura das orelhas)
skull_r = np.array([hr[0], max(hr[0], hr[2]) * 1.05, hr[2]])
skull_c = np.array([hc[0], head_top - skull_r[1], hc[2]])
eye_y = BONES.get('LeftEye', np.array([0, head[1] + 0.07, 0.09]))[1]
TORSO = (R_TORSO, R_HIPS, R_NECK)
manifest = []
out = {}

def save(name, bone, mesh, mats, tex=None):
    write_glb(os.path.join(out_dir, name + '.glb'), mesh, mats, tex)
    manifest.append((name, 'mixamorig:' + bone))
    print(f'{name}.glb -> {bone}: {sum(len(p[3]) // 3 for p in mesh.prims.values())} triângulos')

# colete (polícia): casca justa no tronco, bolsos na frente, texto nas costas
def vest(name, color, text, text_color, pouch='#202328'):
    m = Mesh()
    top = neck[1] - 0.075
    ys = np.linspace(hip[1] + 0.09, top, 9)
    loft(m, 'colete', ys, TORSO, 0.022)
    # alças por cima dos ombros: da frente até as costas, passando rente ao trapézio
    for sx in (1, -1):
        x = sx * 0.095
        pf, nf = surface_at(top, math.atan2(x, 0.10), TORSO, 0.024)
        pb, nb = surface_at(top, math.atan2(x, -0.10), TORSO, 0.024)
        mid = np.array([x, neck[1] - 0.005, (pf[2] + pb[2]) / 2])
        (cx, cz), r = section(neck[1] - 0.02, TORSO, 36, extra=0.0)
        path = [pf + np.array([0, -0.01, 0])]
        for t in np.linspace(0.15, 0.85, 7):
            q = (1 - t) ** 2 * pf + 2 * (1 - t) * t * (mid + np.array([0, 0.03, 0])) + t * t * pb
            path.append(q)
        path.append(pb + np.array([0, -0.01, 0]))
        G = []
        for q in path:  # faixa larga (5 cm) e fina
            G.append([q + np.array([-0.025, 0, 0]), q + np.array([0.025, 0, 0]), q + np.array([0.025, 0.012, 0]), q + np.array([-0.025, 0.012, 0])])
        grid_mesh(m, 'colete', G)
    for i in range(3):  # 3 porta-carregadores na frente
        x = (i - 1) * 0.07
        p, n = surface_at(hip[1] + 0.17, math.atan2(x, 0.12), TORSO, 0.035)
        box(m, 'bolso', p + n * 0.012, (0.06, 0.09, 0.03), rot_y=math.atan2(n[0], n[2]))
    p, n = surface_at(hip[1] + 0.33, 0.45, TORSO, 0.03)  # rádio no peito
    box(m, 'bolso', p + n * 0.015, (0.05, 0.08, 0.025), rot_y=math.atan2(n[0], n[2]))
    pb, nb = surface_at(hip[1] + 0.33, math.pi, TORSO, 0.026)  # placa de texto nas costas
    quad(m, 'texto', pb + nb * 0.004, (-1, 0, 0), (0, 1, 0), 0.26, 0.065, nb)
    mats = {'colete': {'cor': hexc(color), 'rug': 0.9}, 'bolso': {'cor': hexc(pouch), 'rug': 0.9},
            'texto': {'cor': [1, 1, 1], 'rug': 0.9, 'textura': 'texto'}}
    save(name, 'Spine1', m, mats, {'texto': label_png(text, text_color)})

def hexc(h):
    h = h.lstrip('#')
    return [int(h[i:i + 2], 16) / 255.0 for i in (0, 2, 4)]

vest('colete_pm', '#2b323c', 'POLÍCIA', (240, 240, 240))
vest('colete_bope', '#121315', 'BOPE', (220, 220, 220), '#0c0c0d')
vest('colete_swat', '#1a2236', 'SWAT', (245, 245, 245), '#121a2a')
vest('colete_civil', '#151515', 'POLÍCIA CIVIL', (242, 201, 76), '#101010')

# cinto tático: carregadores, radinho com antena, coldre e fivela
def belt(name, color, pouch, radio='#1a1a1a'):
    m = Mesh()
    y = hip[1] + 0.035
    loft(m, 'cinto', np.linspace(y - 0.022, y + 0.022, 3), (R_HIPS, R_TORSO, R_THIGH), 0.018)
    for i in range(3):  # carregadores na frente/esquerda
        ang = 0.35 + i * 0.32
        p, n = surface_at(y, ang, (R_HIPS, R_TORSO, R_THIGH), 0.03)
        box(m, 'bolso', p + n * 0.015 + np.array([0, -0.035, 0]), (0.045, 0.10, 0.028), rot_y=math.atan2(n[0], n[2]))
    p, n = surface_at(y, 1.75, (R_HIPS, R_TORSO, R_THIGH), 0.03)  # radinho no lado esquerdo
    rp = p + n * 0.02 + np.array([0, -0.02, 0])
    box(m, 'radio', rp, (0.055, 0.11, 0.03), rot_y=math.atan2(n[0], n[2]))
    tube(m, 'radio', [rp + np.array([0.012, 0.055, 0]), rp + np.array([0.012, 0.16, 0])], 0.005, seg=6)
    p, n = surface_at(y, -1.75, (R_HIPS, R_TORSO, R_THIGH), 0.03)  # coldre direito
    box(m, 'bolso', p + n * 0.025 + np.array([0, -0.07, 0]), (0.05, 0.17, 0.045), rot_y=math.atan2(n[0], n[2]))
    p, n = surface_at(y, 0.0, (R_HIPS, R_TORSO, R_THIGH), 0.022)  # fivela
    box(m, 'fivela', p + n * 0.006, (0.05, 0.04, 0.01))
    mats = {'cinto': {'cor': hexc(color), 'rug': 0.85}, 'bolso': {'cor': hexc(pouch), 'rug': 0.9},
            'radio': {'cor': hexc(radio), 'rug': 0.5, 'metal': 0.2}, 'fivela': {'cor': [0.55, 0.56, 0.58], 'metal': 0.9, 'rug': 0.35}}
    save(name, 'Hips', m, mats)

belt('cinto_tatico', '#141414', '#1d1f22')
belt('cinto_tatico_marrom', '#4b3a2a', '#2a2420', '#202020')

# capacete (BOPE/SWAT): calota cobrindo do alto até as orelhas, aberta no rosto
def helmet(name, color):
    m = Mesh()
    rad = skull_r + np.array([0.02, 0.022, 0.02])
    def keep(th):  # th = 0 frente (+Z): na testa sobe, nas orelhas e na nuca desce
        front = math.cos(th)
        return 0.55 if front > 0.6 else 0.98 if front < -0.2 else 0.8
    ellipsoid_cap(m, 'casco', skull_c, rad, keep)
    mats = {'casco': {'cor': hexc(color), 'rug': 0.55, 'metal': 0.05}}
    save(name, 'Head', m, mats)

helmet('capacete_preto', '#18191b')
helmet('capacete_azul', '#1c2438')

# boné com a aba para trás
def cap(name, color, brim):
    m = Mesh()
    rad = skull_r + np.array([0.012, 0.012, 0.012])
    ellipsoid_cap(m, 'cupula', skull_c, rad, lambda th: 0.62 if math.cos(th) > 0.3 else 0.7, rings=8)
    # aba: meia elipse atrás (-Z), levemente inclinada
    G = []
    base_y = skull_c[1] + rad[1] * math.cos(math.pi / 2 * 0.7) + 0.004
    for i in range(2):
        row = []
        for j in range(13):
            a = math.pi / 2 + math.pi * j / 12  # de +X para -X passando por -Z
            ring = math.sin(math.pi / 2 * 0.7)
            rr = np.array([rad[0], rad[2]]) * ring
            ext = 0.0 if i == 0 else 0.075 * max(0.0, -math.cos(a)) ** 0.7
            x = math.sin(a) * (rr[0] + ext * abs(math.sin(a)) * 0.3)
            z = math.cos(a) * (rr[1] + ext)
            row.append((skull_c[0] + x, base_y - (0.012 if i else 0.0), skull_c[2] + z))
        G.append(row)
    grid_mesh(m, 'aba', G, close_u=False)
    grid_mesh(m, 'aba', [G[1], G[0]], close_u=False)  # verso
    mats = {'cupula': {'cor': hexc(color), 'rug': 0.9}, 'aba': {'cor': hexc(brim), 'rug': 0.9}}
    save(name, 'Head', m, mats)

cap('bone_tras_preto', '#151515', '#151515')
cap('bone_tras_vermelho', '#b3121b', '#141414')
cap('bone_tras_branco', '#eeeeee', '#1c5cc0')

GOLD = {'cor': [1.0, 0.8, 0.36], 'metal': 0.55, 'rug': 0.32}

# corrente de ouro grossa com pingente
def chain(name, thick):
    m = Mesh()
    nk = pts(R_NECK, R_TORSO, R_HEAD)
    base_y = neck[1] - 0.01
    sel = nk[(np.abs(nk[:, 1] - base_y) < 0.015) & (np.abs(nk[:, 0]) < 0.075)]
    cx, cz = np.median(sel[:, 0]), np.median(sel[:, 2])
    rx = np.percentile(np.abs(sel[:, 0] - cx), 97) + 0.012
    rz = np.percentile(np.abs(sel[:, 2] - cz), 97) + 0.012
    path = []
    for k in range(48):
        t = 2 * math.pi * k / 48
        front = max(0.0, math.cos(t))
        x = cx + math.sin(t) * rx * (1 + 0.5 * front)
        y = base_y - 0.11 * front ** 1.6
        z = cz + math.cos(t) * rz + 0.03 * front ** 2
        # encosta no peito
        if front > 0.2:
            p, n = surface_at(y, math.atan2(x - cx, max(0.01, z - cz)), TORSO, thick + 0.004)
            z = max(z, p[2])
        path.append((x, y, z))
    tube(m, 'ouro', path, thick, seg=6, closed=True, bumps=0.35)
    front = np.array(path[0])
    tube(m, 'ouro', [front + np.array([0, -0.004, 0.004]), front + np.array([0, -0.045, 0.006])], 0.012, seg=10)  # pingente
    save(name, 'Spine2', m, {'ouro': GOLD})

chain('corrente_ouro', 0.0045)
chain('corrente_ouro_grossa', 0.0065)

# relógio de ouro no pulso esquerdo
def watch(name):
    m = Mesh()
    fa = pts(R_FORE)
    wx = abs(BONES['LeftHand'][0]) - 0.045
    sel = fa[(fa[:, 0] > 0) & (np.abs(fa[:, 0] - wx) < 0.01)]
    cy, cz = np.median(sel[:, 1]), np.median(sel[:, 2])
    ry = np.percentile(np.abs(sel[:, 1] - cy), 95) + 0.006
    rz = np.percentile(np.abs(sel[:, 2] - cz), 95) + 0.006
    path = [(wx, cy + ry * math.cos(2 * math.pi * k / 24), cz + rz * math.sin(2 * math.pi * k / 24)) for k in range(24)]
    tube(m, 'ouro', path, 0.006, seg=6, closed=True)
    # mostrador em cima (costas da mão para cima na pose T)
    top = np.array([wx, cy + ry + 0.004, cz])
    box(m, 'ouro', top, (0.034, 0.012, 0.036))
    box(m, 'vidro', top + np.array([0, 0.0065, 0]), (0.026, 0.002, 0.028))
    save(name, 'LeftForeArm', m, {'ouro': GOLD, 'vidro': {'cor': [0.05, 0.06, 0.08], 'metal': 0.2, 'rug': 0.1}})

watch('relogio_ouro')

# brincos (as duas orelhas)
def earrings(name, diamond):
    m = Mesh()
    hp = pts(R_HEAD)
    for sgn in (1, -1):
        band = hp[(np.sign(hp[:, 0]) == sgn) & (hp[:, 1] > eye_y - 0.075) & (hp[:, 1] < eye_y - 0.035)]
        k = np.argmax(np.abs(band[:, 0]))
        p = band[k] + np.array([0.004 * sgn, -0.008, 0.0])
        if diamond:
            box(m, 'pedra', p, (0.011, 0.011, 0.011))
        else:
            path = [p + np.array([0.0, -0.009 * (1 - math.cos(2 * math.pi * k / 16)), 0.009 * math.sin(2 * math.pi * k / 16)]) for k in range(16)]
            tube(m, 'ouro', path, 0.0018, seg=5, closed=True)
    mats = {'ouro': GOLD, 'pedra': {'cor': [0.92, 0.95, 1.0], 'metal': 0.6, 'rug': 0.05}}
    save(name, 'Head', m, mats)

earrings('brinco_argola', False)
earrings('brinco_diamante', True)

with open(os.path.join(out_dir, 'equipamento.txt'), 'w') as f:
    for name, bone in manifest:
        f.write(f'{name} {bone}\n')
