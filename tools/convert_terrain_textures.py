# Converte texturas PBR (Poly Haven, 2k: diff .jpg, nor_gl .exr, disp .png) para o formato do terreno:
#   <nome>_alb.jpg  1024x1024  cor (sRGB)
#   <nome>_nh.jpg    512x512   R,G = normal XY (OpenGL), B = altura (para misturar as camadas)
# Uso: python3 tools/convert_terrain_textures.py <pasta_com_as_texturas> <nome_saida> <pasta_destino>
import sys, glob, os
import numpy as np, OpenEXR, Imath
from PIL import Image
src, name, dst = sys.argv[1], sys.argv[2], sys.argv[3]
one = lambda pat: glob.glob(os.path.join(src, pat))[0]
Image.open(one('*_diff_*.jpg')).convert('RGB').resize((1024, 1024), Image.LANCZOS).save(os.path.join(dst, name + '_alb.jpg'), quality=88)
f = OpenEXR.InputFile(one('*_nor_gl_*.exr'))
dw = f.header()['dataWindow']; w, h = dw.max.x - dw.min.x + 1, dw.max.y - dw.min.y + 1
ch = [np.frombuffer(f.channel(c, Imath.PixelType(Imath.PixelType.FLOAT)), np.float32).reshape(h, w) for c in 'RG']
disp = np.asarray(Image.open(one('*_disp_*.png')).convert('I;16'), np.float32)
disp = (disp - disp.min()) / max(1e-6, disp.max() - disp.min())
to8 = lambda a: Image.fromarray((np.clip(a, 0, 1) * 255 + 0.5).astype(np.uint8)).resize((512, 512), Image.LANCZOS)
Image.merge('RGB', [to8(ch[0]), to8(ch[1]), to8(disp)]).save(os.path.join(dst, name + '_nh.jpg'), quality=92)
