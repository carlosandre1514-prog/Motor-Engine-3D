#!/usr/bin/env python3
"""Converte uma skin de personagem (.glb com esqueleto Mixamo, roupas e acessórios) para o formato do motor.

- um esqueleto só (as cópias do mesmo osso, ex.: mixamorigHead / _1 / _2, viram um), nomes "mixamorig:Osso";
- centímetros -> metros;
- cada malha com esqueleto vai para a pose de repouso desse esqueleto; peças rígidas penduradas num osso
  (boné, rádio, coldre, cinto...) viram parte da malha presas a esse osso (peso 1);
- as malhas são juntadas por material (poucas chamadas de desenho) e as texturas reduzidas para .jpg.

Uso: converter_skin.py entrada.glb saida.glb [--tex 1024] [--escala E (padrão: automático)]
"""
import io, json, struct, sys
import numpy as np
from PIL import Image

args = [a for a in sys.argv[1:] if not a.startswith('--')]
src, dst = args[:2]
opt = lambda k, d: type(d)(sys.argv[sys.argv.index(k) + 1]) if k in sys.argv else d
TEX = opt('--tex', 1024)
UNIT = opt('--escala', 0.0)  # 0 = automático pela altura do quadril

data = open(src, 'rb').read()
jl, = struct.unpack('<I', data[12:16])
G = json.loads(data[20:20 + jl])
BIN = data[20 + jl + 8:]

CT = {5120: np.int8, 5121: np.uint8, 5122: np.int16, 5123: np.uint16, 5125: np.uint32, 5126: np.float32}
NC = {'SCALAR': 1, 'VEC2': 2, 'VEC3': 3, 'VEC4': 4, 'MAT4': 16}

def acc(i):
    a = G['accessors'][i]
    bv = G['bufferViews'][a['bufferView']]
    dt = np.dtype(CT[a['componentType']])
    n = NC[a['type']]
    off = bv.get('byteOffset', 0) + a.get('byteOffset', 0)
    stride = bv.get('byteStride', 0) or dt.itemsize * n
    raw = np.frombuffer(BIN, np.uint8, count=stride * (a['count'] - 1) + dt.itemsize * n, offset=off)
    out = np.lib.stride_tricks.as_strided(raw, (a['count'], dt.itemsize * n), (stride, 1)).copy().view(dt).reshape(a['count'], n)
    if a.get('normalized'):
        out = out.astype(np.float32) / np.iinfo(dt).max
    return out

def local_matrix(n):
    if 'matrix' in n:
        return np.array(n['matrix'], np.float64).reshape(4, 4).T
    M = np.eye(4)
    if 'scale' in n: M = np.diag(list(n['scale']) + [1.0]) @ M
    if 'rotation' in n:
        x, y, z, w = n['rotation']
        R = np.array([[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
                      [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
                      [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]])
        Rm = np.eye(4); Rm[:3, :3] = R
        M = Rm @ M
    if 'translation' in n: T = np.eye(4); T[:3, 3] = n['translation']; M = T @ M
    return M

nodes = G['nodes']
parent = {}
for i, n in enumerate(nodes):
    for c in n.get('children', []):
        parent[c] = i
world = {}
def W(i):
    if i not in world:
        world[i] = (W(parent[i]) if i in parent else np.eye(4)) @ local_matrix(nodes[i])
    return world[i]

def bone_name(i):
    n = nodes[i]
    nm = (n.get('extras') or {}).get('originalName') or n.get('name') or ''
    if nm.startswith('mixamorig') and ':' not in nm:
        nm = 'mixamorig:' + nm[len('mixamorig'):]
    for suf in ('_1', '_2', '_3'):
        if nm.endswith(suf) and nm[:-len(suf)].startswith('mixamorig:'):
            nm = nm[:-len(suf)]
    return nm

# ---- esqueleto: o da skin com mais ossos; cópias com o mesmo nome viram um
skins = G.get('skins', [])
main = max(range(len(skins)), key=lambda s: len(skins[s]['joints']))
joint_nodes = {}
for s in skins:
    for jn in s['joints']:
        nm = bone_name(jn)
        if nm.startswith('mixamorig:') and (nm not in joint_nodes or jn in skins[main]['joints']):
            joint_nodes[nm] = jn
# hierarquia: pai = osso ancestral mais próximo que também está no esqueleto
names = list(joint_nodes)
def bone_parent(nm):
    i = joint_nodes[nm]
    while i in parent:
        i = parent[i]
        p = bone_name(i)
        if p in joint_nodes and p != nm:
            return p
    return None
order = []
def add(nm):
    if nm in order: return
    p = bone_parent(nm)
    if p: add(p)
    order.append(nm)
for nm in names: add(nm)
idx = {nm: k for k, nm in enumerate(order)}
if UNIT <= 0:  # quadril a ~1 m: se no mundo ele estiver a ~100, o arquivo está em centímetros
    hy = W(joint_nodes['mixamorig:Hips'])[1, 3] if 'mixamorig:Hips' in joint_nodes else 1.0
    UNIT = 0.01 if hy > 10 else 1.0
S = np.diag([UNIT, UNIT, UNIT, 1.0])
def rest_world(nm):  # em metros (só a translação escala)
    M = W(joint_nodes[nm]).copy()
    M[:3, 3] *= UNIT
    # tira escala residual da rotação
    for c in range(3):
        l = np.linalg.norm(M[:3, c]); M[:3, c] /= l if l > 1e-9 else 1
    return M
RW = {nm: rest_world(nm) for nm in order}

def nearest_bone(i):
    """Osso mais próximo subindo a hierarquia a partir do nó i."""
    while True:
        nm = bone_name(i)
        if nm in idx: return idx[nm]
        if i not in parent: return idx.get('mixamorig:Hips', 0)
        i = parent[i]

# ---- malhas -> pose de repouso, juntadas por material
prims = {}  # material -> listas
for ni, n in enumerate(nodes):
    if 'mesh' not in n: continue
    mesh = G['meshes'][n['mesh']]
    for p in mesh['primitives']:
        if p.get('mode', 4) != 4: continue
        A = p['attributes']
        P = acc(A['POSITION']).astype(np.float64)
        N = acc(A['NORMAL']).astype(np.float64) if 'NORMAL' in A else np.zeros_like(P)
        UV = acc(A['TEXCOORD_0']).astype(np.float32) if 'TEXCOORD_0' in A else np.zeros((len(P), 2), np.float32)
        I = acc(p['indices']).reshape(-1).astype(np.uint32) if 'indices' in p else np.arange(len(P), dtype=np.uint32)
        if 'skin' in n and 'JOINTS_0' in A:
            sk = skins[n['skin']]
            J = acc(A['JOINTS_0']).astype(np.int64)
            Wt = acc(A['WEIGHTS_0']).astype(np.float64)
            ibm = acc(sk['inverseBindMatrices']).reshape(-1, 4, 4).transpose(0, 2, 1).astype(np.float64) if 'inverseBindMatrices' in sk else np.tile(np.eye(4), (len(sk['joints']), 1, 1))
            skinM = np.array([W(jn) @ ibm[k] for k, jn in enumerate(sk['joints'])])  # vértice -> mundo na pose de repouso
            remap = np.array([nearest_bone(jn) for jn in sk['joints']])
            Ph = np.c_[P, np.ones(len(P))]
            Pw = np.zeros((len(P), 3)); Nw = np.zeros((len(P), 3))
            for k in range(4):
                M = skinM[J[:, k]]
                Pw += Wt[:, [k]] * np.einsum('nij,nj->ni', M, Ph)[:, :3]
                Nw += Wt[:, [k]] * np.einsum('nij,nj->ni', M[:, :3, :3], N)
            JJ = remap[J]
        else:
            M = W(ni)
            Pw = (np.c_[P, np.ones(len(P))] @ M.T)[:, :3]
            Nw = N @ np.linalg.inv(M[:3, :3])
            b = nearest_bone(ni)
            JJ = np.zeros((len(P), 4), np.int64); JJ[:, 0] = b
            Wt = np.zeros((len(P), 4)); Wt[:, 0] = 1.0
        Pw *= UNIT
        Nw /= np.maximum(np.linalg.norm(Nw, axis=1, keepdims=True), 1e-9)
        # agrupa: peças sem textura num material só (cor por vértice); com textura, pela mesma imagem
        mat = p.get('material', -1)
        md = G['materials'][mat] if mat >= 0 else {}
        pbr = md.get('pbrMetallicRoughness', {})
        if 'baseColorTexture' in pbr:
            img = G['textures'][pbr['baseColorTexture']['index']]['source']
            nimg = G['textures'][md['normalTexture']['index']]['source'] if 'normalTexture' in md else -1
            key = ('tex', img, nimg, md.get('alphaMode', 'OPAQUE') in ('BLEND', 'MASK'))
        else:
            key = ('solido',)
        L = prims.setdefault(key, {'P': [], 'N': [], 'UV': [], 'J': [], 'W': [], 'I': [], 'C': [], 'n': 0, 'mat': mat, 'nomes': set()})
        L['nomes'].add(n.get('name') or '')
        cf = list(pbr.get('baseColorFactor', [1, 1, 1, 1]))
        if md.get('alphaMode') == 'BLEND' and cf[3] < 1:  # vidro (viseira, óculos): o motor ainda não tem transparência; vira fumê
            cf = [c * cf[3] * 0.5 for c in cf[:3]] + [1.0]
        L['C'].append(np.tile(np.array(cf, np.float32), (len(P), 1)))
        L['I'].append(I + L['n']); L['n'] += len(P)
        L['P'].append(Pw.astype(np.float32)); L['N'].append(Nw.astype(np.float32)); L['UV'].append(UV)
        L['J'].append(JJ.astype(np.uint16)); L['W'].append((Wt / np.maximum(Wt.sum(1, keepdims=True), 1e-9)).astype(np.float32))

# ---- glTF de saída
out = {'asset': {'version': '2.0', 'generator': 'Motor 3D converter_skin'}, 'scene': 0, 'nodes': [], 'meshes': [], 'skins': [],
       'materials': [], 'accessors': [], 'bufferViews': [], 'samplers': [{'magFilter': 9729, 'minFilter': 9987}], 'images': [], 'textures': []}
bin_ = bytearray()
def view(b, target=0, stride=0):
    while len(bin_) % 4: bin_.append(0)
    out['bufferViews'].append({'buffer': 0, 'byteOffset': len(bin_), 'byteLength': len(b), **({'target': target} if target else {}),
                               **({'byteStride': stride} if stride else {})})
    bin_.extend(b)
    return len(out['bufferViews']) - 1
def accessor(arr, ctype, typ, target=0, minmax=False, normalized=False):
    arr = np.ascontiguousarray(arr)
    stride = 0
    raw = arr.tobytes()
    if target == 34962 and arr.itemsize * arr.shape[1] % 4:  # atributo de vértice: passo múltiplo de 4 bytes
        stride = (arr.itemsize * arr.shape[1] + 3) // 4 * 4
        pad = np.zeros((arr.shape[0], stride), np.uint8)
        pad[:, :arr.itemsize * arr.shape[1]] = arr.view(np.uint8).reshape(arr.shape[0], -1)
        raw = pad.tobytes()
    a = {'bufferView': view(raw, target, stride), 'componentType': ctype, 'count': int(arr.shape[0]), 'type': typ}
    if normalized: a['normalized'] = True
    if minmax: a['min'] = arr.min(0).tolist(); a['max'] = arr.max(0).tolist()
    out['accessors'].append(a)
    return len(out['accessors']) - 1

# ossos
for nm in order:
    p = bone_parent(nm)
    L = np.linalg.inv(RW[p]) @ RW[nm] if p else RW[nm]
    t = L[:3, 3]; R = L[:3, :3]
    # quaternion
    tr = R.trace()
    if tr > 0:
        s = np.sqrt(tr + 1) * 2; q = [(R[2, 1] - R[1, 2]) / s, (R[0, 2] - R[2, 0]) / s, (R[1, 0] - R[0, 1]) / s, s / 4]
    else:
        k = int(np.argmax(np.diag(R))); i, j, l = k, (k + 1) % 3, (k + 2) % 3
        s = np.sqrt(1 + R[i, i] - R[j, j] - R[l, l]) * 2
        q = [0, 0, 0, 0]; q[i] = s / 4; q[j] = (R[j, i] + R[i, j]) / s; q[l] = (R[l, i] + R[i, l]) / s; q[3] = (R[l, j] - R[j, l]) / s
    q = np.array(q) / np.linalg.norm(q)
    out['nodes'].append({'name': nm, 'translation': t.tolist(), 'rotation': q.tolist()})
for k, nm in enumerate(order):
    kids = [idx[c] for c in order if bone_parent(c) == nm]
    if kids: out['nodes'][k]['children'] = kids
ibm = np.array([np.linalg.inv(RW[nm]).T for nm in order], np.float32)
out['skins'].append({'joints': list(range(len(order))), 'inverseBindMatrices': accessor(ibm.reshape(-1, 16), 5126, 'MAT4'),
                     'skeleton': idx.get('mixamorig:Hips', 0)})

# texturas
tex_map = {}
def texture(ti, normal=False):
    key = (ti, normal)
    if key in tex_map: return tex_map[key]
    img_i = G['textures'][ti]['source']
    im = G['images'][img_i]
    bv = G['bufferViews'][im['bufferView']]
    raw = BIN[bv.get('byteOffset', 0): bv.get('byteOffset', 0) + bv['byteLength']]
    pil = Image.open(io.BytesIO(raw))
    has_alpha = pil.mode in ('RGBA', 'LA') or 'transparency' in pil.info
    pil = pil.convert('RGBA' if has_alpha else 'RGB')
    if max(pil.size) > TEX:
        f = TEX / max(pil.size)
        pil = pil.resize((max(1, int(pil.width * f)), max(1, int(pil.height * f))), Image.LANCZOS)
    b = io.BytesIO()
    if has_alpha: pil.save(b, 'PNG', optimize=True); mime = 'image/png'
    else: pil.save(b, 'JPEG', quality=90 if normal else 86); mime = 'image/jpeg'
    out['images'].append({'bufferView': view(b.getvalue()), 'mimeType': mime})
    out['textures'].append({'sampler': 0, 'source': len(out['images']) - 1})
    tex_map[key] = len(out['textures']) - 1
    return tex_map[key]

primitives = []
total = 0
for key, L in prims.items():
    mat = L['mat']
    P = np.concatenate(L['P']); N = np.concatenate(L['N']); UV = np.concatenate(L['UV'])
    J = np.concatenate(L['J']); Wt = np.concatenate(L['W']); I = np.concatenate(L['I'])
    total += len(I) // 3
    m = {'name': f'material_{mat}', 'pbrMetallicRoughness': {'metallicFactor': 0.0, 'roughnessFactor': 0.7}}
    solid = key[0] == 'solido'
    if solid:
        m = {'name': 'acessorios', 'pbrMetallicRoughness': {'metallicFactor': 0.0, 'roughnessFactor': 0.6}, 'doubleSided': True}
    elif mat >= 0:
        src_m = G['materials'][mat]
        pbr = src_m.get('pbrMetallicRoughness', {})
        m['name'] = src_m.get('name', m['name'])
        for k in ('baseColorFactor', 'metallicFactor', 'roughnessFactor'):
            if k in pbr: m['pbrMetallicRoughness'][k] = pbr[k]
        if 'baseColorTexture' in pbr: m['pbrMetallicRoughness']['baseColorTexture'] = {'index': texture(pbr['baseColorTexture']['index'])}
        if 'normalTexture' in src_m: m['normalTexture'] = {'index': texture(src_m['normalTexture']['index'], True)}
        if src_m.get('alphaMode') in ('BLEND', 'MASK'): m['alphaMode'] = 'MASK'; m['alphaCutoff'] = 0.4
        if src_m.get('doubleSided'): m['doubleSided'] = True
    out['materials'].append(m)
    # compacto: normal em 8 bits, UV em 16 bits, ossos e pesos em 8 bits, índices em 16 bits quando cabe
    q8 = lambda x: np.clip(np.round(x * 127), -127, 127).astype(np.int8)
    a = {'POSITION': accessor(P, 5126, 'VEC3', 34962, True), 'NORMAL': accessor(q8(N), 5120, 'VEC3', 34962, normalized=True)}
    if UV.min() >= 0 and UV.max() <= 1:
        a['TEXCOORD_0'] = accessor(np.round(UV * 65535).astype(np.uint16), 5123, 'VEC2', 34962, normalized=True)
    else:
        a['TEXCOORD_0'] = accessor(UV, 5126, 'VEC2', 34962)
    a['JOINTS_0'] = accessor(J.astype(np.uint8), 5121, 'VEC4', 34962)
    Wq = np.round(Wt * 255).astype(np.int32)
    Wq[:, 0] += 255 - Wq.sum(1)  # soma exata 255
    a['WEIGHTS_0'] = accessor(np.clip(Wq, 0, 255).astype(np.uint8), 5121, 'VEC4', 34962, normalized=True)
    if solid:
        C = np.concatenate(L['C'])
        a['COLOR_0'] = accessor(np.round(np.clip(C, 0, 1) * 255).astype(np.uint8), 5121, 'VEC4', 34962, normalized=True)
    small = len(P) < 65536
    prim = {'attributes': a, 'indices': accessor(I.reshape(-1, 1).astype(np.uint16 if small else np.uint32), 5123 if small else 5125, 'SCALAR', 34963),
            'material': len(out['materials']) - 1}
    # corpo, olhos e boca não podem ser reduzidos (rosto e corpo sem deformar)
    if L['nomes'] & {'Body', 'Eyes', 'default'}:
        prim['extras'] = {'proteger': True}
    primitives.append(prim)
out['meshes'].append({'name': 'personagem', 'primitives': primitives})
out['nodes'].append({'name': 'personagem', 'mesh': 0, 'skin': 0})
roots = [idx[nm] for nm in order if not bone_parent(nm)] + [len(out['nodes']) - 1]
out['scenes'] = [{'nodes': roots}]
while len(bin_) % 4: bin_.append(0)
out['buffers'] = [{'byteLength': len(bin_)}]
js = json.dumps(out, separators=(',', ':')).encode()
while len(js) % 4: js += b' '
with open(dst, 'wb') as f:
    f.write(struct.pack('<III', 0x46546C67, 2, 12 + 8 + len(js) + 8 + len(bin_)))
    f.write(struct.pack('<II', len(js), 0x4E4F534A)); f.write(js)
    f.write(struct.pack('<II', len(bin_), 0x004E4942)); f.write(bytes(bin_))
allP = np.concatenate([np.concatenate(L['P']) for L in prims.values()])
print(f'{dst}: {total} triângulos, {len(primitives)} materiais, {len(order)} ossos, altura {allP[:,1].max()-allP[:,1].min():.2f} m, {len(bin_) // 1024} KB')
