#!/usr/bin/env python3
"""Gera os sons do jogo (WAV 16 bits, mono, 44,1 kHz) em app/src/main/assets/sons/.

Tudo é sintetizado aqui (sem gravações de terceiros), então não há crédito a dar.
Tiro = estalo supersônico (fuzis) + explosão de gás no cano (ruído com ataque instantâneo) + "soco" grave
+ mecanismo (ferrolho/ slide) + ecos nas paredes da favela e cauda de reverberação. Cada arma tem a sua receita.
Uso:  python3 tools/gerar_sons.py
"""
import os
import wave

import numpy as np

SR = 44100
RNG = np.random.default_rng(1514)
OUT = os.path.join(os.path.dirname(__file__), "..", "app", "src", "main", "assets", "sons")


def t_axis(sec):
    return np.arange(int(SR * sec)) / SR


def noise(sec):
    return RNG.standard_normal(int(SR * sec))


def filt(x, lo=None, hi=None, order=2):
    """Passa-faixa suave no domínio da frequência (sem fase: bom para ruído e impulsos)."""
    n = len(x)
    X = np.fft.rfft(x, n * 2)
    f = np.fft.rfftfreq(n * 2, 1 / SR)
    f[0] = 1e-3
    H = np.ones_like(f)
    if lo:
        H *= 1 / np.sqrt(1 + (lo / f) ** (2 * order))
    if hi:
        H *= 1 / np.sqrt(1 + (f / hi) ** (2 * order))
    return np.fft.irfft(X * H)[:n]


def conv(x, ir):
    n = len(x) + len(ir) - 1
    m = 1 << (n - 1).bit_length()
    return np.fft.irfft(np.fft.rfft(x, m) * np.fft.rfft(ir, m))[:n]


def pad(x, sec):
    n = int(SR * sec)
    return np.pad(x, (0, max(0, n - len(x))))[:n]


def place(buf, x, at):
    i = int(at * SR)
    if i >= len(buf):
        return
    m = min(len(x), len(buf) - i)
    buf[i:i + m] += x[:m]


def reverb(sec, rt60, damp_hz, early, seed_gain=1.0):
    """Resposta de um beco de favela: reflexões discretas nas paredes + cauda que escurece com o tempo."""
    t = t_axis(sec)
    lo_tail = filt(noise(sec), hi=damp_hz) * np.exp(-6.9 * t / rt60)
    hi_tail = filt(noise(sec), lo=damp_hz) * np.exp(-6.9 * t / (rt60 * 0.35))
    ir = (lo_tail + 0.5 * hi_tail) * 0.03 * seed_gain
    ir[: int(0.012 * SR)] *= np.linspace(0, 1, int(0.012 * SR))  # sem cauda antes das primeiras reflexões
    for ms, g in early:
        i = int(ms / 1000 * SR)
        if i < len(ir):
            ir[i] += g * (1 if RNG.random() > 0.3 else -1)
    return ir


def click(sec, f_ring, tau_ring, amp):
    """Peça de metal batendo: impulso + ressonâncias."""
    t = t_axis(sec)
    x = filt(noise(sec), 1500, 9000) * np.exp(-t / 0.002) * 0.6
    for f, a in ((f_ring, 1.0), (f_ring * 1.63, 0.6), (f_ring * 2.41, 0.35)):
        x += a * np.sin(2 * np.pi * f * t + RNG.random() * 6) * np.exp(-t / tau_ring)
    return x * amp


def gunshot(p):
    dry_len = 0.5
    t = t_axis(dry_len)
    x = np.zeros(len(t))
    # estalo supersônico (onda em N de ~0,4 ms)
    if p["crack"] > 0:
        n = int(0.0004 * SR)
        nwave = np.concatenate([np.linspace(1, -1, n), np.zeros(4)])
        place(x, filt(np.pad(nwave, (0, 400)), 900) * p["crack"] * 3.0, 0.0)
    # explosão de gás no cano
    attack = 1 - np.exp(-t / 0.00025)
    blast = filt(noise(dry_len), p["lo"], p["hi"]) * attack * np.exp(-t / p["tau"])
    blast += filt(noise(dry_len), p["lo"], p["hi"] * 0.4) * attack * np.exp(-t / (p["tau"] * 2.5)) * 0.5
    place(x, blast, 0.0007)
    # soco: ruído grave (o "peso" do tiro) + um pouco de varredura descendo e um grave curto
    punch = filt(noise(dry_len), 50, 450) * attack * np.exp(-t / p["tau_t"]) * 1.4
    f = p["f1"] + (p["f0"] - p["f1"]) * np.exp(-t / 0.006)
    sweep = np.sin(2 * np.pi * np.cumsum(f) / SR) * np.exp(-t / (p["tau_t"] * 0.6)) * attack * 0.35
    boom = np.sin(2 * np.pi * p["boom_f"] * t) * np.exp(-t / p["boom_tau"]) * attack * p["boom"] * 0.5
    place(x, punch + sweep + boom, 0.0007)
    # mecanismo (ferrolho / slide)
    if p["mech"] > 0:
        place(x, click(0.06, p["mech_f"], 0.012, p["mech"]), p["mech_at"])
    # compressão (o tiro é alto: "achatado" como numa gravação de verdade)
    x = np.tanh(x / np.max(np.abs(x)) * p["drive"]) / np.tanh(p["drive"])
    # ecos entre as casas + cauda
    total = p["len"]
    early = [(9, 0.32), (17, 0.22), (29, 0.25), (43, 0.15), (61, 0.18), (88, 0.11), (131, 0.12), (190, 0.07), (260, 0.06)]
    ir = reverb(total, p["rt60"], 2200, early)
    wet = conv(x, ir)
    wet = wet / (np.max(np.abs(wet)) + 1e-9) * p["wet"] * 0.75  # o eco nunca passa do tiro
    out = pad(x, total) + pad(wet, total)
    fade = np.ones(len(out))
    fade[-int(0.15 * SR):] = np.linspace(1, 0, int(0.15 * SR))
    return out * fade, x


def distant(dry, p):
    """O mesmo tiro de longe: sem agudos, mais eco, chega um pouco depois."""
    total = p["len"] + 0.4
    d = filt(pad(dry, total), 40, 900, order=3)
    early = [(25, 0.4), (60, 0.35), (110, 0.3), (180, 0.25), (270, 0.2), (390, 0.15)]
    ir = reverb(total, p["rt60"] * 1.4, 700, early, 1.6)
    out = pad(np.zeros(int(0.04 * SR)), 0.04)
    w = conv(d, ir)
    w = w / (np.max(np.abs(w)) + 1e-9) * np.max(np.abs(d)) * 0.8
    out = np.concatenate([out, pad(d, total) + pad(w, total)])[: int(total * SR)]
    fade = np.ones(len(out))
    fade[-int(0.2 * SR):] = np.linspace(1, 0, int(0.2 * SR))
    return out * fade


WEAPONS = {
    # nome: estalo, faixa da explosão, duração, soco (f0->f1, tau), grave, mecanismo, sala
    "pistola": dict(crack=0.0, lo=180, hi=8000, tau=0.011, f0=900, f1=190, tau_t=0.015, boom_f=70, boom=0.2, boom_tau=0.03,
                    mech=0.10, mech_f=3100, mech_at=0.028, drive=2.2, rt60=0.9, wet=0.33, len=1.2),
    "m4": dict(crack=0.6, lo=120, hi=9500, tau=0.016, f0=1200, f1=160, tau_t=0.02, boom_f=55, boom=0.25, boom_tau=0.035,
               mech=0.05, mech_f=2900, mech_at=0.042, drive=2.6, rt60=1.2, wet=0.4, len=1.6),
    "ak47": dict(crack=0.4, lo=75, hi=7000, tau=0.024, f0=800, f1=110, tau_t=0.03, boom_f=45, boom=0.4, boom_tau=0.045,
                 mech=0.07, mech_f=2300, mech_at=0.047, drive=3.0, rt60=1.4, wet=0.45, len=1.7),
    "ak74": dict(crack=0.75, lo=130, hi=10500, tau=0.017, f0=1450, f1=170, tau_t=0.02, boom_f=55, boom=0.25, boom_tau=0.035,
                 mech=0.06, mech_f=2600, mech_at=0.044, drive=3.2, rt60=1.25, wet=0.42, len=1.6),
    "escopeta": dict(crack=0.0, lo=55, hi=6000, tau=0.04, f0=600, f1=80, tau_t=0.05, boom_f=40, boom=0.55, boom_tau=0.06,
                     mech=0.0, mech_f=2000, mech_at=0.0, drive=2.8, rt60=1.8, wet=0.5, len=2.0),
    "sniper": dict(crack=1.0, lo=50, hi=8000, tau=0.035, f0=900, f1=70, tau_t=0.045, boom_f=38, boom=0.6, boom_tau=0.07,
                   mech=0.0, mech_f=2000, mech_at=0.0, drive=3.4, rt60=2.4, wet=0.55, len=2.6),
}


def pump():
    """Escopeta: bomba indo e voltando (shk-shk)."""
    x = np.zeros(int(0.5 * SR))
    for at, f in ((0.0, 1700), (0.17, 2100)):
        t = t_axis(0.09)
        slide = filt(noise(0.09), 1200, 6000) * np.exp(-((t - 0.035) / 0.02) ** 2) * 0.35
        place(x, slide, at)
        place(x, click(0.05, f, 0.01, 0.5), at + 0.07)
    return x


def bolt():
    """Sniper: ferrolho abrindo e fechando."""
    x = np.zeros(int(0.7 * SR))
    place(x, click(0.05, 2400, 0.012, 0.5), 0.0)
    t = t_axis(0.15)
    place(x, filt(noise(0.15), 1500, 7000) * np.exp(-((t - 0.07) / 0.04) ** 2) * 0.25, 0.08)
    place(x, filt(noise(0.12), 1500, 7000) * np.exp(-((t[:len(t) * 4 // 5] - 0.05) / 0.03) ** 2) * 0.25, 0.32)
    place(x, click(0.05, 2700, 0.014, 0.6), 0.45)
    return x


def reload():
    x = np.zeros(int(1.5 * SR))
    place(x, click(0.04, 3400, 0.006, 0.4), 0.0)                       # botão do carregador
    t = t_axis(0.12)
    place(x, filt(noise(0.12), 800, 4000) * np.exp(-((t - 0.05) / 0.03) ** 2) * 0.2, 0.08)  # carregador saindo
    place(x, click(0.06, 1800, 0.018, 0.9), 0.62)                       # carregador entrando (clack)
    place(x, click(0.05, 3100, 0.01, 0.5), 0.66)
    t = t_axis(0.16)
    place(x, filt(noise(0.16), 1200, 6000) * np.exp(-((t - 0.07) / 0.04) ** 2) * 0.3, 0.98)  # puxa o ferrolho
    place(x, click(0.07, 2200, 0.02, 1.0), 1.17)                        # solta: clack
    return x


def impact():
    x = np.zeros(int(0.45 * SR))
    t = t_axis(0.06)
    place(x, filt(noise(0.06), 400, 6000) * np.exp(-t / 0.007), 0.0)
    for _ in range(14):                                                 # farelo de reboco caindo
        tt = t_axis(0.01)
        place(x, filt(noise(0.01), 2000, 9000) * np.exp(-tt / 0.0015) * RNG.random() * 0.25, RNG.uniform(0.01, 0.35))
    return x


def whoosh(f_lo, f_hi, sec):
    t = t_axis(sec)
    n = noise(sec)
    out = np.zeros(len(t))
    bands = 6
    for b in range(bands):
        c = f_lo * (f_hi / f_lo) ** (b / (bands - 1))
        env = np.exp(-((t / sec - (0.25 + 0.5 * b / bands)) / 0.18) ** 2)
        out += filt(n, c * 0.7, c * 1.4) * env
    return out


def thunder():
    sec = 7.5
    t = t_axis(sec)
    brown = np.cumsum(noise(sec))
    brown -= np.convolve(brown, np.ones(2000) / 2000, mode="same")
    rumble = filt(brown, 25, 180, order=3)
    env = np.zeros(len(t))
    for _ in range(9):                                                  # rolos do trovão
        c, w, a = RNG.uniform(0.2, 4.5), RNG.uniform(0.25, 0.9), RNG.uniform(0.4, 1.0)
        env += a * np.exp(-((t - c) / w) ** 2)
    env *= np.exp(-t / 3.0)
    x = rumble / np.max(np.abs(rumble)) * env
    x += filt(noise(sec), 150, 900) * env * 0.25
    crack = filt(noise(0.5), 200, 3500) * np.exp(-t_axis(0.5) / 0.09) * 0.8  # estalo inicial
    place(x, crack, 0.05)
    x = x + conv(x, reverb(2.0, 2.5, 600, [(80, 0.3), (200, 0.25)]))[: len(t)] * 0.8
    fade = np.ones(len(x))
    fade[-int(0.6 * SR):] = np.linspace(1, 0, int(0.6 * SR))
    return x * fade


def rain():
    sec, xf = 9.0, 1.0
    t = t_axis(sec)
    n = noise(sec)
    hiss = filt(n, 500, 11000) * 0.5 + filt(noise(sec), 150, 1500) * 0.35   # chiado da chuva + chão molhado
    hiss *= 1 + 0.12 * np.sin(2 * np.pi * 0.21 * t) + 0.08 * np.sin(2 * np.pi * 0.53 * t + 1)
    x = hiss
    drop_t = t_axis(0.012)
    for _ in range(int(sec * 500)):                                     # gotinhas
        f = RNG.uniform(1800, 7000)
        d = np.sin(2 * np.pi * f * drop_t) * np.exp(-drop_t / RNG.uniform(0.0015, 0.005)) * RNG.random() ** 3 * 0.9
        place(x, d, RNG.uniform(0, sec - 0.02))
    ping_t = t_axis(0.12)
    for _ in range(int(sec * 18)):                                      # telha de zinco / fibrocimento
        f = RNG.uniform(700, 2600)
        d = (np.sin(2 * np.pi * f * ping_t) + 0.5 * np.sin(2 * np.pi * f * 1.5 * ping_t)) * np.exp(-ping_t / RNG.uniform(0.015, 0.05))
        place(x, d * RNG.random() ** 2 * 0.5, RNG.uniform(0, sec - 0.13))
    plop_t = t_axis(0.06)
    for _ in range(int(sec * 3)):                                       # pingos em poça
        f = 300 + 700 * (1 - np.exp(-plop_t / 0.015))
        d = np.sin(2 * np.pi * np.cumsum(f) / SR) * np.exp(-plop_t / 0.02)
        place(x, d * RNG.uniform(0.2, 0.6), RNG.uniform(0, sec - 0.07))
    # loop sem emenda: o último segundo se mistura no começo
    n_xf = int(xf * SR)
    loop = x[: len(x) - n_xf].copy()
    w = np.linspace(0, 1, n_xf)
    loop[:n_xf] = x[:n_xf] * w + x[-n_xf:] * (1 - w)
    return loop


def save(name, x, peak_db=-1.0):
    x = np.asarray(x, dtype=np.float64)
    x = x / (np.max(np.abs(x)) + 1e-9) * 10 ** (peak_db / 20)
    pcm = (np.clip(x, -1, 1) * 32767).astype("<i2")
    os.makedirs(OUT, exist_ok=True)
    with wave.open(os.path.join(OUT, name + ".wav"), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(SR)
        w.writeframes(pcm.tobytes())
    print(f"{name}.wav  {len(x) / SR:.2f} s")


def main():
    for name, p in WEAPONS.items():
        full, dry = gunshot(p)
        if name == "escopeta":
            full = full + pad(np.concatenate([np.zeros(int(0.45 * SR)), pump()]), p["len"]) * 0.25
        if name == "sniper":
            full = full + pad(np.concatenate([np.zeros(int(0.9 * SR)), bolt()]), p["len"]) * 0.2
        save("tiro_" + name, full)
        save("tiro_" + name + "_longe", distant(dry, p), -6.0)
    save("recarga", reload(), -4.0)
    save("impacto", impact(), -6.0)
    save("golpe_faca", whoosh(700, 3000, 0.25), -6.0)
    save("golpe_taco", whoosh(250, 1200, 0.35), -4.0)
    save("trovao", thunder())
    save("chuva", rain(), -3.0)


if __name__ == "__main__":
    main()
