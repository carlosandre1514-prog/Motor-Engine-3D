# Converte as texturas do Downtown City MegaKit (Quaternius, CC0) para shaders/materials (tamanho de celular).
# Uso: python3 tools/convert_material_textures.py <pasta_com_os_T_*.png> shaders/materials
import sys, os
from PIL import Image
src, dst = sys.argv[1], sys.argv[2]
os.makedirs(dst, exist_ok=True)
def img(n): return Image.open(os.path.join(src, n))
def save(im, name, size, q=88):
    im = im.resize((size, size), Image.LANCZOS)
    if name.endswith('.png'): im.save(os.path.join(dst, name), optimize=True)
    else: im.convert('RGB').save(os.path.join(dst, name), quality=q)
sets = {'concreto': 'Concrete', 'tijolo_vermelho': 'RedBrick', 'ardosia': 'RoofSlate', 'marmore': 'MarbleFloor', 'terra': 'Dirt'}
for out, k in sets.items():
    save(img(f'T_{k}_BaseColor.png'), f'{out}_alb.jpg', 512)
    save(img(f'T_{k}_Normal.png').convert('RGB'), f'{out}_nrm.jpg', 512, 92)
    save(img(f'T_{k}_ORM.png').convert('RGB'), f'{out}_orm.jpg', 512)
save(img('T_Concrete_Asphalt_BaseColor.png'), 'asfalto_alb.jpg', 512)
save(img('T_lit_interior_1.png'), 'interior_1.jpg', 256)
save(img('T_lit_interior_2.png'), 'interior_2.jpg', 256)
save(img('T_dark_interior.png'), 'interior_escuro.jpg', 256)
cur = img('T_Curtains.png').convert('RGBA'); h = cur.size[0] // 2
save(cur.crop((0, 0, h, h)), 'cortina_aberta.png', 256)
save(cur.crop((0, h, h, 2 * h)), 'cortina_lados.png', 256)
save(img('T_Noise_Drips.png').convert('L'), 'escorrido.jpg', 256)
