# Enxuga um .glb para o app: mantém só a sub-árvore de um nó (ex.: uma arma de uma cena de vitrine), troca a
# rotação desse nó (deixa a peça deitada certinha) e reduz as texturas para jpg/png de N px.
# Uso: python3 tools/compact_glb.py entrada.glb saida.glb --no base --rot x,y,z,w --tex 1024
import sys, json, struct, io, argparse
from PIL import Image
ap = argparse.ArgumentParser()
ap.add_argument('src'); ap.add_argument('dst'); ap.add_argument('--no', required=True)
ap.add_argument('--rot', default=None); ap.add_argument('--tex', type=int, default=1024)
a = ap.parse_args()
d = open(a.src, 'rb').read()
jl = struct.unpack('<I', d[12:16])[0]
g = json.loads(d[20:20 + jl])
bin0 = d[20 + jl + 8:]
root = next(i for i, n in enumerate(g['nodes']) if n.get('name') == a.no)
keep, stack = [], [root]
while stack:
    i = stack.pop(); keep.append(i); stack += g['nodes'][i].get('children', [])
keep.sort()
nmap = {o: k for k, o in enumerate(keep)}
nodes = []
for o in keep:
    n = dict(g['nodes'][o])
    if 'children' in n: n['children'] = [nmap[c] for c in n['children']]
    nodes.append(n)
r = nodes[nmap[root]]
for k in ('matrix', 'translation', 'scale'): r.pop(k, None)
if a.rot: r['rotation'] = [float(x) for x in a.rot.split(',')]
# malhas, materiais, texturas, imagens e acessores usados
meshes = sorted({n['mesh'] for n in nodes if 'mesh' in n})
mmap = {o: k for k, o in enumerate(meshes)}
for n in nodes:
    if 'mesh' in n: n['mesh'] = mmap[n['mesh']]
mats, accs = set(), set()
for m in meshes:
    for p in g['meshes'][m]['primitives']:
        if 'material' in p: mats.add(p['material'])
        accs |= set(p['attributes'].values())
        if 'indices' in p: accs.add(p['indices'])
mats = sorted(mats)
texs = set()
def texrefs(o):
    if isinstance(o, dict):
        if 'index' in o and len(o) <= 4 and isinstance(o['index'], int) and ('texCoord' in o or 'scale' in o or 'strength' in o or len(o) == 1): texs.add(o['index'])
        for v in o.values(): texrefs(v)
    elif isinstance(o, list):
        for v in o: texrefs(v)
for m in mats: texrefs(g['materials'][m])
texs = sorted(texs)
imgs = sorted({g['textures'][t]['source'] for t in texs})
out = bytearray(); views = []; vmap = {}
def addview(data, extra):
    while len(out) % 4: out.append(0)
    views.append(dict(buffer=0, byteOffset=len(out), byteLength=len(data), **extra)); out.extend(data)
    return len(views) - 1
accessors = []
amap = {}
for o in sorted(accs):
    ac = dict(g['accessors'][o]); bv = g['bufferViews'][ac['bufferView']]
    if ac['bufferView'] not in vmap:
        extra = {k: bv[k] for k in ('byteStride', 'target') if k in bv}
        vmap[ac['bufferView']] = addview(bin0[bv.get('byteOffset', 0):bv.get('byteOffset', 0) + bv['byteLength']], extra)
    ac['bufferView'] = vmap[ac['bufferView']]; amap[o] = len(accessors); accessors.append(ac)
new_meshes = []
for m in meshes:
    mm = json.loads(json.dumps(g['meshes'][m]))
    for p in mm['primitives']:
        p['attributes'] = {k: amap[v] for k, v in p['attributes'].items()}
        if 'indices' in p: p['indices'] = amap[p['indices']]
        if 'material' in p: p['material'] = mats.index(p['material'])
        p.pop('targets', None)
    new_meshes.append(mm)
images = []
for o in imgs:
    im = g['images'][o]; bv = g['bufferViews'][im['bufferView']]
    pic = Image.open(io.BytesIO(bin0[bv.get('byteOffset', 0):bv.get('byteOffset', 0) + bv['byteLength']]))
    if max(pic.size) > a.tex: pic = pic.resize((min(a.tex, pic.size[0]), min(a.tex, pic.size[1])), Image.LANCZOS)
    buf = io.BytesIO()
    alpha = pic.mode in ('RGBA', 'LA') and pic.getchannel('A').getextrema()[0] < 250
    if alpha: pic.save(buf, 'PNG', optimize=True); mime = 'image/png'
    else: pic.convert('RGB').save(buf, 'JPEG', quality=88); mime = 'image/jpeg'
    images.append({'bufferView': addview(buf.getvalue(), {}), 'mimeType': mime})
textures = [dict(g['textures'][t], source=imgs.index(g['textures'][t]['source'])) for t in texs]
def remap(o):
    if isinstance(o, dict):
        return {k: (texs.index(v) if k == 'index' and isinstance(v, int) and v in texs else remap(v)) for k, v in o.items()}
    if isinstance(o, list): return [remap(v) for v in o]
    return o
materials = [remap(g['materials'][m]) for m in mats]
while len(out) % 4: out.append(0)
ng = {'asset': g['asset'], 'scene': 0, 'scenes': [{'nodes': [nmap[root]]}], 'nodes': nodes, 'meshes': new_meshes,
      'materials': materials, 'textures': textures, 'images': images, 'samplers': g.get('samplers', []),
      'accessors': accessors, 'bufferViews': views, 'buffers': [{'byteLength': len(out)}]}
if not ng['samplers']: del ng['samplers']
js = json.dumps(ng, separators=(',', ':')).encode()
while len(js) % 4: js += b' '
glb = struct.pack('<III', 0x46546C67, 2, 12 + 8 + len(js) + 8 + len(out)) + struct.pack('<II', len(js), 0x4E4F534A) + js + struct.pack('<II', len(out), 0x004E4942) + out
open(a.dst, 'wb').write(glb)
print(len(nodes), 'nós', len(new_meshes), 'malhas', len(images), 'imagens', len(glb) // 1024, 'KB')
