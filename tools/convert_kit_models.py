# Prepara o Downtown City MegaKit (Quaternius, CC0, versão glTF/Godot) para o app: shaders/kit/
#  - .gltf + .bin copiados; texturas reduzidas (512 px; trim e placas 1024) em .jpg (.png se tiver transparência)
#  - cor por vértice (COLOR_0) removida: no kit ela é máscara de desgaste do shader da Unity/Unreal, não cor
#  - vidro (BLEND) vira invisível: o motor ainda não tem transparência e o vidro escuro esconderia as salas
# Uso: python3 tools/convert_kit_models.py "<pasta glTF (Godot)>" shaders/kit
import sys, os, json, glob, shutil
from PIL import Image
src, dst = sys.argv[1], sys.argv[2]
os.makedirs(os.path.join(dst, 'tex'), exist_ok=True)
big = {'T_Trim_BaseColor.png', 'T_Street_Decals.png'}
alpha = {'T_Street_Decals.png'}
done = {}
for f in sorted(glob.glob(os.path.join(src, '*.gltf'))):
    g = json.load(open(f))
    for im in g.get('images', []):
        uri = im['uri']
        if uri not in done:
            out = os.path.splitext(uri)[0] + ('.png' if uri in alpha else '.jpg')
            pic = Image.open(os.path.join(src, uri))
            size = 1024 if uri in big else 512
            pic = pic.resize((size, size), Image.LANCZOS)
            if uri in alpha: pic.convert('RGBA').save(os.path.join(dst, 'tex', out), optimize=True)
            else: pic.convert('RGB').save(os.path.join(dst, 'tex', out), quality=90 if 'Normal' in uri else 85)
            done[uri] = out
        im['uri'] = 'tex/' + done[uri]
        im['mimeType'] = 'image/png' if done[uri].endswith('.png') else 'image/jpeg'
    for m in g.get('materials', []):
        if m.get('name') == 'MI_Glass':
            m['alphaMode'] = 'MASK'; m['alphaCutoff'] = 0.9
    for mesh in g.get('meshes', []):
        for prim in mesh['primitives']:
            prim['attributes'].pop('COLOR_0', None)
    for b in g.get('buffers', []):
        shutil.copy(os.path.join(src, b['uri']), os.path.join(dst, b['uri']))
    json.dump(g, open(os.path.join(dst, os.path.basename(f)), 'w'), separators=(',', ':'))
print(len(done), 'texturas')
