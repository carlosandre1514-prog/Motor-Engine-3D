#!/usr/bin/env python3
"""Pinta as roupas do personagem base direto na textura do corpo (uma textura .jpg por visual).

Usa o mapa de regiões (cabeça, tronco, braço, mão, perna, pé) e o mapa de posições (onde cada pixel da
textura fica no corpo, em metros) que o tools/personagem_fbx grava. Assim as mangas, a barra da calça e o
buraco dos olhos da balaclava saem em linhas retas no corpo, e textos/números são projetados de frente ou de costas.

Uso: gerar_roupas.py PELE.png REGIOES.png POSICOES.png PASTA_SAIDA [--lado 1024]
"""
import math, os, sys
import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont

R_HEAD, R_NECK, R_HIPS, R_TORSO, R_UPPER, R_FORE, R_HAND, R_THIGH, R_SHIN, R_FOOT = range(1, 11)

args = [a for a in sys.argv[1:] if not a.startswith('--')]
side = int(sys.argv[sys.argv.index('--lado') + 1]) if '--lado' in sys.argv else 1024
skin_path, reg_path, pos_path, out_dir = args[:4]
os.makedirs(out_dir, exist_ok=True)
FONT_DIR = os.path.join(os.path.dirname(__file__), '..', 'shaders', 'materials', 'fontes')

N = 1024
skin = np.asarray(Image.open(skin_path).convert('RGB').resize((N, N), Image.LANCZOS)).astype(np.float32) / 255.0
reg_img = np.asarray(Image.open(reg_path))
REG = reg_img[..., 0] // 20
SIDE = reg_img[..., 1] // 100           # 0 meio, 1 esquerdo, 2 direito
HAS = reg_img[..., 3] > 0
pos_img = np.asarray(Image.open(pos_path)).astype(np.float32)
with open(pos_path + '.txt') as f:
    lines = f.read().split('\n')
mn = np.array([float(v) for v in lines[0].split()])
mx = np.array([float(v) for v in lines[1].split()])
BONES = {}
for ln in lines[2:]:
    p = ln.split()
    if len(p) == 4:
        BONES[p[0]] = np.array([float(v) for v in p[1:]])
P = mn + pos_img[..., :3] / 255.0 * (mx - mn)
# a borda das ilhas (alargada no mapa de regiões) não tem posição: copia a do vizinho
_hp = pos_img[..., 3] > 0
for _ in range(6):
    acc = np.zeros_like(P); cnt = np.zeros(_hp.shape, np.float32)
    for dy, dx in ((1, 0), (-1, 0), (0, 1), (0, -1)):
        sh = np.roll(_hp, (dy, dx), (0, 1)); sp = np.roll(P, (dy, dx), (0, 1))
        m = ~_hp & sh
        acc[m] += sp[m]; cnt[m] += 1
    m = cnt > 0
    P[m] = acc[m] / cnt[m][..., None]
    _hp |= m
X, Y, Z = P[..., 0], P[..., 1], P[..., 2]
AX = np.abs(X)

# sombreado do corpo (dobras, oclusão) tirado da pele: fica nas roupas
lum = skin @ np.array([0.3, 0.59, 0.11], np.float32)
lum_b = np.asarray(Image.fromarray((lum * 255).astype(np.uint8)).filter(ImageFilter.GaussianBlur(9))).astype(np.float32) / 255.0
shade = np.clip(lum_b / max(1e-3, np.median(lum_b[HAS & (REG == R_TORSO)])), 0.78, 1.08)
rng = np.random.default_rng(5)
grain = np.asarray(Image.fromarray((rng.random((N, N)) * 255).astype(np.uint8)).filter(ImageFilter.GaussianBlur(0.7))).astype(np.float32) / 255.0 - 0.5

def hexc(h):
    h = h.lstrip('#')
    return np.array([int(h[i:i + 2], 16) for i in (0, 2, 4)], np.float32) / 255.0

def paint(img, mask, color, weave=0.06, shading=1.0):
    """Tecido: cor * sombreado do corpo + textura fina do pano."""
    c = hexc(color) if isinstance(color, str) else color
    sh = 1.0 + (shade - 1.0) * shading
    col = c[None, None, :] * sh[..., None] * (1.0 + grain[..., None] * weave * 2)
    img[mask] = np.clip(col[mask], 0, 1)

def pattern_paint(img, mask, rgb):
    sh = shade[..., None]
    img[mask] = np.clip(rgb[mask] * sh[mask], 0, 1)

def camo(colors, scale, seed):
    """Camuflagem por manchas (no espaço do corpo: não estica entre as ilhas da textura)."""
    r = np.random.default_rng(seed)
    out = np.zeros((N, N, 3), np.float32) + hexc(colors[0])
    for k, c in enumerate(colors[1:]):
        ph = r.random(6) * 10
        f = (np.sin(X / scale * 6.1 + ph[0]) * np.sin(Y / scale * 5.3 + ph[1]) + np.sin((X + Z) / scale * 3.7 + ph[2]) * np.cos(Y / scale * 4.4 + ph[3])
             + 0.6 * np.sin(Z / scale * 7.9 + ph[4]) * np.sin((X - Y) / scale * 6.6 + ph[5]))
        out[f > 0.55 + 0.2 * k] = hexc(c)
    return out

def text_on(img, mask, text, center_x, center_y, height, back, color, font='bold'):
    """Escreve um texto projetado no corpo, de frente (z>0) ou de costas (z<0)."""
    fonts = sorted(f for f in os.listdir(FONT_DIR) if f.lower().endswith('.ttf'))
    fp = os.path.join(FONT_DIR, fonts[0])
    for f in fonts:
        if 'bold' in f.lower() or 'black' in f.lower() or 'anton' in f.lower() or 'bebas' in f.lower():
            fp = os.path.join(FONT_DIR, f)
    W, H = 1200, 300
    im = Image.new('L', (W, H), 0)
    d = ImageDraw.Draw(im)
    fnt = ImageFont.truetype(fp, 240)
    tw = d.textlength(text, font=fnt)
    d.text(((W - tw) / 2, 10), text, fill=255, font=fnt)
    a = np.asarray(im).astype(np.float32) / 255.0
    bbox = Image.fromarray((a * 255).astype(np.uint8)).getbbox()
    if not bbox:
        return
    a = a[bbox[1]:bbox[3], bbox[0]:bbox[2]]
    th, tw = a.shape
    width = height * tw / th
    u = (X - center_x) / width + 0.5
    if back:
        u = 1.0 - u  # de costas, quem olha vê o texto direito
    v = (center_y - Y) / height + 0.5
    face = (Z < -0.02) if back else (Z > 0.02)
    m = mask & face & (u >= 0) & (u < 1) & (v >= 0) & (v < 1)
    iu = np.clip((u * (tw - 1)).astype(int), 0, tw - 1)
    iv = np.clip((v * (th - 1)).astype(int), 0, th - 1)
    alpha = np.where(m, a[iv, iu], 0)[..., None]
    img[:] = img * (1 - alpha) + hexc(color)[None, None, :] * shade[..., None] * alpha

def tattoo_arms(img, which='both', density=1.0, seed=3):
    """Tatuagem tribal no braço e antebraço (cilindro em volta do braço)."""
    r = np.random.default_rng(seed)
    arm = HAS & ((REG == R_UPPER) | (REG == R_FORE))
    if which == 'left':
        arm &= X > 0
    elif which == 'right':
        arm &= X < 0
    sh_y = BONES.get('mixamorig:LeftArm', np.array([0.2, 1.42, -0.05]))[1]
    ang = np.arctan2(Z - (-0.06), Y - sh_y)
    t = AX
    ph = r.random(4) * 6
    f = np.sin(t * 38 + np.sin(ang * 3 + ph[0]) * 2.2 + ph[1]) + np.sin(ang * 2 + t * 22 + ph[2]) * 0.8
    ink = arm & (f > 1.15 - 0.25 * density) & (AX > 0.24) & (AX < 0.66)
    col = np.array([0.12, 0.14, 0.18], np.float32)
    img[ink] = img[ink] * 0.35 + col * 0.65

def tattoo_chest(img, text='FÉ', seed=1):
    torso = HAS & (REG == R_TORSO)
    text_on(img, torso, text, 0.0, 1.36, 0.06, False, '#1d2330')

def flag_patch(img, x, y, w, h, back=False):
    m = HAS & (np.abs(X - x) < w / 2) & (np.abs(Y - y) < h / 2) & ((Z < 0) if back else (Z > -0.2))
    img[m] = hexc('#1f8a3b') * shade[m][..., None]
    dx, dy = (X - x) / (w / 2), (Y - y) / (h / 2)
    dia = m & (np.abs(dx) * 0.9 + np.abs(dy) < 0.75)
    img[dia] = hexc('#f2cf1d') * shade[dia][..., None]
    cir = m & (dx * dx * 2.2 + dy * dy * 2.2 < 0.22)
    img[cir] = hexc('#1d4fa8') * shade[cir][..., None]

# ---- partes do corpo (máscaras)
hip_y = BONES.get('mixamorig:Hips', np.array([0, 0.99, 0]))[1]
neck_y = BONES.get('mixamorig:Neck', np.array([0, 1.49, 0]))[1]
knee_y = BONES.get('mixamorig:LeftLeg', np.array([0, 0.53, 0]))[1]
elbow_x = abs(BONES.get('mixamorig:LeftForeArm', np.array([0.46, 1.42, 0]))[0])
wrist_x = abs(BONES.get('mixamorig:LeftHand', np.array([0.73, 1.41, 0]))[0])
ankle_y = BONES.get('mixamorig:LeftFoot', np.array([0, 0.09, 0]))[1]

torso_m = HAS & ((REG == R_TORSO) | ((REG == R_NECK) & (Y < neck_y - 0.02)) | ((REG == R_HIPS) & (Y > hip_y + 0.02)))
arm_m = HAS & ((REG == R_UPPER) | (REG == R_FORE))
def sleeve(length):  # 'regata', 'curta', 'longa'
    if length == 'regata':
        return np.zeros_like(HAS)
    lim = {'curta': (elbow_x + 0.20) * 0.5 + 0.02, 'longa': wrist_x - 0.03}[length]
    return arm_m & (AX < lim)
legs_m = HAS & ((REG == R_HIPS) | (REG == R_THIGH) | (REG == R_SHIN)) & (Y < hip_y + 0.06)
def pants(length):  # 'longa', 'bermuda'
    lim = ankle_y + 0.03 if length == 'longa' else knee_y - 0.06
    return legs_m & (Y > lim)
feet_m = HAS & (REG == R_FOOT)
def boots():
    return HAS & ((REG == R_FOOT) | ((REG == R_SHIN) & (Y < ankle_y + 0.17)))
hands_m = HAS & (REG == R_HAND)
head_m = HAS & ((REG == R_HEAD) | (REG == R_NECK))

def balaclava(img, color='#141414'):
    eye_y = BONES.get('mixamorig:LeftEye', np.array([0.03, 1.668, 0.09]))[1]
    m = head_m & ~((Y > eye_y - 0.03) & (Y < eye_y + 0.025) & (Z > 0.035) & (AX < 0.075))
    # olhos e dentes (ilhas pequenas da textura fora do rosto) continuam
    paint(img, m & (Y > neck_y - 0.06), color, weave=0.12, shading=0.6)

def tank_top(img, color, trim=None):
    m = torso_m.copy()
    # cava funda e decote (regata)
    m &= ~((AX > 0.11) & (Y > 1.30))
    m &= ~((AX < 0.07) & (Y > 1.40) & (Z > 0))
    paint(img, m, color)
    return m

def shirt(img, color, sleeves='curta', collar=None):
    m = torso_m | sleeve(sleeves)
    paint(img, m, color)
    if collar:
        c = HAS & (REG == R_NECK) & (Y < neck_y + 0.01) | (torso_m & (Y > neck_y - 0.035))
        paint(img, c, collar)
    return m

def belt(img, color='#141414', y=None):
    y = (hip_y + 0.035) if y is None else y
    m = legs_m & (Y > y - 0.022) & (Y < y + 0.022)
    paint(img, m, color, shading=0.5)

def shoes(img, color, sole='#f2f2f2', boot=False):
    m = boots() if boot else feet_m
    paint(img, m, color)
    s = m & (Y < 0.025)
    paint(img, s, sole)

def gloves(img, color='#151515'):
    paint(img, hands_m | (arm_m & (AX > wrist_x - 0.04)), color, weave=0.1)

def flipflop(img):
    m = feet_m & (Y < 0.018)
    paint(img, m, '#2f6fd6')
    strap = feet_m & (np.abs(X - np.sign(X) * 0.1) < 0.012) & (Y < 0.06) & (Z > 0.0)
    paint(img, strap, '#2f6fd6')

# ---- visuais
VISUAIS = {}
def visual(name):
    def deco(fn):
        VISUAIS[name] = fn
        return fn
    return deco

@visual('pm')
def _pm(img):  # Polícia Militar: cinza-azulado, calça escura, coturno
    shirt(img, '#5d7187', 'longa', collar='#4b5d72')
    paint(img, pants('longa'), '#2b3442')
    belt(img)
    shoes(img, '#121212', '#0a0a0a', boot=True)
    flag_patch(img, 0.27, 1.40, 0.06, 0.04)
    text_on(img, torso_m, 'POLÍCIA MILITAR', 0.0, 1.33, 0.045, True, '#e8e8e8')

@visual('bope')
def _bope(img):  # BOPE: tudo preto, luvas, caveira no braço
    shirt(img, '#16181b', 'longa')
    paint(img, pants('longa'), '#16181b')
    belt(img, '#0b0b0b')
    shoes(img, '#0b0b0b', '#050505', boot=True)
    gloves(img, '#0e0e0e')
    text_on(img, torso_m, 'BOPE', 0.0, 1.32, 0.07, True, '#d9d9d9')
    m = arm_m & (np.hypot(X - 0.27, Y - 1.43) < 0.03) & (Z > -0.2)
    paint(img, m, '#cfcfcf', shading=0.4)

@visual('swat')
def _swat(img):  # tático azul-marinho
    shirt(img, '#1e2a44', 'longa')
    paint(img, pants('longa'), '#1e2a44')
    belt(img, '#0f1420')
    shoes(img, '#111111', '#0a0a0a', boot=True)
    gloves(img, '#151515')
    text_on(img, torso_m, 'SWAT', 0.0, 1.32, 0.075, True, '#f2f2f2')

@visual('civil')
def _civil(img):  # Polícia Civil: camiseta preta, jeans, tênis
    shirt(img, '#1b1d22', 'curta')
    pattern_paint(img, pants('longa'), np.clip(camo(['#3a5a86', '#355079', '#40638f'], 0.03, 2), 0, 1))
    belt(img, '#3b2a1c')
    shoes(img, '#2a2a2a', '#e8e8e8')
    text_on(img, torso_m, 'POLÍCIA CIVIL', 0.0, 1.33, 0.045, True, '#f2c94c')

@visual('pm_camuflada')
def _pm_cam(img):
    cam = camo(['#5a6343', '#3f472d', '#7a6f4e', '#2c2f22'], 0.07, 7)
    m = torso_m | sleeve('longa')
    pattern_paint(img, m, cam)
    pattern_paint(img, pants('longa'), cam)
    belt(img)
    shoes(img, '#1a1a14', '#0a0a0a', boot=True)
    flag_patch(img, 0.27, 1.40, 0.06, 0.04)

@visual('camisa_time')
def _jersey(img):  # camisa amarela com gola verde, número 10 nas costas, bermuda azul, chinelo
    shirt(img, '#f2cf1d', 'curta', collar='#1b8a3a')
    cuff = arm_m & (np.abs(AX - ((elbow_x + 0.20) * 0.5 + 0.02 - 0.012)) < 0.012)
    paint(img, cuff, '#1b8a3a')
    text_on(img, torso_m, '10', 0.0, 1.27, 0.15, True, '#1b8a3a')
    paint(img, pants('bermuda'), '#1c5cc0')
    flipflop(img)
    tattoo_arms(img, 'right', 0.9, 4)

@visual('regata')
def _tank(img):  # regata branca, bermuda jeans, tatuagens nos dois braços
    tank_top(img, '#ececec')
    pattern_paint(img, pants('bermuda'), np.clip(camo(['#4a6c98', '#3e5d86', '#5679a6'], 0.025, 3), 0, 1))
    shoes(img, '#f2f2f2', '#d02020')
    tattoo_arms(img, 'both', 1.2, 8)
    tattoo_chest(img, 'FÉ')

@visual('moletom')
def _hoodie(img):  # moletom cinza, calça de moletom preta, tênis branco
    shirt(img, '#4a4f57', 'longa')
    pocket = torso_m & (Z > 0.03) & (AX < 0.10) & (Y > hip_y + 0.08) & (Y < hip_y + 0.2)
    paint(img, pocket, '#3f444b')
    paint(img, pants('longa'), '#15171b')
    shoes(img, '#f0f0f0', '#d8d8d8')

@visual('vermelho_preto')
def _rubro(img):  # camisa listrada vermelha e preta, bermuda preta
    m = shirt(img, '#b3121b', 'curta')
    stripes = m & (((Y * 100).astype(int) // 4) % 2 == 0)
    paint(img, stripes, '#141414')
    paint(img, pants('bermuda'), '#141414')
    shoes(img, '#141414', '#f2f2f2')
    tattoo_arms(img, 'left', 1.0, 11)

@visual('tatico_preto')
def _black(img):  # camiseta preta, calça cargo preta, coturno
    shirt(img, '#121212', 'curta')
    paint(img, pants('longa'), '#1b1b1b')
    belt(img, '#0b0b0b')
    shoes(img, '#0b0b0b', '#050505', boot=True)
    tattoo_arms(img, 'both', 0.8, 13)

@visual('balaclava_preta')
def _bala(img):  # bandido de balaclava e moletom preto
    shirt(img, '#141414', 'longa')
    paint(img, pants('longa'), '#22252b')
    shoes(img, '#f0f0f0', '#d8d8d8')
    balaclava(img, '#111111')

@visual('regata_balaclava')
def _tank_bala(img):
    _tank(img)
    balaclava(img, '#1b1b1b')

def pad_islands(img, px=8):
    """Fora das ilhas da textura repete a cor da borda (senão a pele vaza nas costuras ao reduzir a textura)."""
    have = HAS.copy()
    out = img.copy()
    for _ in range(px):
        nxt = have.copy()
        acc = np.zeros_like(out); cnt = np.zeros(have.shape, np.float32)
        for dy, dx in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            sh_have = np.roll(have, (dy, dx), (0, 1)); sh_img = np.roll(out, (dy, dx), (0, 1))
            m = ~have & sh_have
            acc[m] += sh_img[m]; cnt[m] += 1
        m = cnt > 0
        out[m] = acc[m] / cnt[m][..., None]
        nxt |= m
        have = nxt
    return out

for name, fn in VISUAIS.items():
    img = skin.copy()
    fn(img)
    img = pad_islands(img)
    out = Image.fromarray((np.clip(img, 0, 1) * 255).astype(np.uint8))
    if side != N:
        out = out.resize((side, side), Image.LANCZOS)
    path = os.path.join(out_dir, name + '.jpg')
    out.save(path, quality=86)
    print(f'{path}: {os.path.getsize(path) // 1024} KB')
