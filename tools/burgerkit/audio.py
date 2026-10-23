"""Every sound, synthesised: impacts, footsteps, the kitchen's sizzles and dings, the till, and the
customers' gibberish voices. Written as data/sounds.psnd (psp/src/audio.c), mono 16-bit at RATE.

The engine looks its sounds up by name (step_concrete, impact_metal, vo_male_alert...); names with
several variants pick one at random, as in tools/sounds.txt.
"""

import struct

import numpy as np

from ..bspconv.pmap import PmapWriter

RATE = 22050
PSND_MAGIC = b"PSND"
PSND_VERSION = 1


def t(secs):
    return np.arange(int(secs * RATE)) / RATE


def env(n, attack=0.005, decay=0.2):
    x = np.arange(n) / RATE
    a = np.clip(x / max(attack, 1e-4), 0, 1)
    return a * np.exp(-x / max(decay, 1e-4))


def lowpass(x, k):
    """A cheap low-pass: a moving average k samples wide (bigger k, duller)."""
    if k <= 1:
        return x
    return np.convolve(x, np.ones(k) / k, mode="same")


def noise(secs, seed):
    return np.random.default_rng(seed).uniform(-1, 1, int(secs * RATE))


def tone(freq, secs, shape="sine", partials=None):
    x = t(secs)
    if shape == "saw":
        return 2.0 * ((x * freq) % 1.0) - 1.0
    if shape == "square":
        return np.sign(np.sin(2 * np.pi * freq * x))
    out = np.zeros_like(x)
    for mult, amp in partials or [(1.0, 1.0)]:
        out += amp * np.sin(2 * np.pi * freq * mult * x)
    return out


def glide(f0, f1, secs, shape="sine"):
    x = t(secs)
    f = np.linspace(f0, f1, len(x))
    ph = 2 * np.pi * np.cumsum(f) / RATE
    if shape == "saw":
        return 2.0 * ((ph / (2 * np.pi)) % 1.0) - 1.0
    return np.sin(ph)


def norm(x, peak=0.9):
    m = np.abs(x).max() or 1.0
    return x / m * peak


def cat(*parts, gap=0.0):
    out = []
    for p in parts:
        out.append(p)
        if gap:
            out.append(np.zeros(int(gap * RATE)))
    return np.concatenate(out)


def mix(*parts):
    n = max(len(p) for p in parts)
    out = np.zeros(n)
    for p in parts:
        out[: len(p)] += p
    return out


# --- The sounds --------------------------------------------------------------------------------------------

def thump(seed, freq=90, secs=0.25, bright=40):
    n = int(secs * RATE)
    return norm(lowpass(noise(secs, seed), bright) * env(n, 0.002, secs / 4) * 3 + tone(freq, secs) * env(n, 0.002, secs / 5))


def clang(seed, freq):
    n = int(0.6 * RATE)
    body = tone(freq, 0.6, partials=[(1, 1), (2.76, 0.6), (5.4, 0.3), (8.9, 0.15)]) * env(n, 0.001, 0.15)
    return norm(body + lowpass(noise(0.6, seed), 3) * env(n, 0.001, 0.02))


def knock(seed, freq):
    n = int(0.18 * RATE)
    return norm(tone(freq, 0.18, partials=[(1, 1), (2.3, 0.4)]) * env(n, 0.001, 0.04) +
                lowpass(noise(0.18, seed), 6) * env(n, 0.001, 0.015) * 0.6)


def click(freq=2500, secs=0.03):
    n = int(secs * RATE)
    return norm(tone(freq, secs) * env(n, 0.001, secs / 4))


def blip(freqs, each=0.06):
    return norm(cat(*[tone(f, each, "square") * env(int(each * RATE), 0.002, each / 2) * 0.5 for f in freqs]))


def bell(freqs, secs=1.2):
    parts = []
    for i, f in enumerate(freqs):
        n = int(secs * RATE)
        b = tone(f, secs, partials=[(1, 1), (2, 0.4), (3.01, 0.2), (4.2, 0.1)]) * env(n, 0.002, secs / 4)
        parts.append(np.concatenate([np.zeros(int(i * 0.12 * RATE)), b]))
    return norm(mix(*parts))


def step(seed):
    n = int(0.09 * RATE)
    return norm(lowpass(noise(0.09, seed), 8) * env(n, 0.003, 0.02)) * 0.6


def sizzle(seed, secs=1.4):
    x = noise(secs, seed)
    hiss = x - lowpass(x, 4)  # high-passed: a hiss
    crackle = (np.random.default_rng(seed + 1).random(len(x)) > 0.996) * np.random.default_rng(seed + 2).uniform(-1, 1, len(x))
    n = len(x)
    shape = np.minimum(1, np.arange(n) / (0.05 * RATE)) * np.minimum(1, (n - np.arange(n)) / (0.4 * RATE))
    return norm((hiss * 0.5 + lowpass(crackle, 2) * 2) * shape) * 0.7


def bubbles(seed, secs=1.5):
    rng = np.random.default_rng(seed)
    out = np.zeros(int(secs * RATE))
    for _ in range(int(secs * 24)):
        start = int(rng.uniform(0, secs - 0.06) * RATE)
        f0 = rng.uniform(400, 900)
        b = glide(f0, f0 * 1.6, 0.05) * env(int(0.05 * RATE), 0.002, 0.015)
        out[start:start + len(b)] += b * rng.uniform(0.3, 1)
    return norm(out + sizzle(seed, secs) * 0.5) * 0.8


def pour(seed, secs=0.9):
    x = lowpass(noise(secs, seed), 3)
    n = len(x)
    wob = 0.7 + 0.3 * np.sin(2 * np.pi * 7 * t(secs))
    shape = np.minimum(1, np.arange(n) / (0.05 * RATE)) * np.minimum(1, (n - np.arange(n)) / (0.2 * RATE))
    return norm(x * wob * shape) * 0.6


def ka_ching():
    drawer = thump(77, 120, 0.25, 20) * 0.6
    ring = bell([1568, 2093], 0.9)
    return norm(mix(drawer, np.concatenate([np.zeros(int(0.08 * RATE)), ring])))


def trombone():
    """The sad trombone: four notes down, the last one wobbling."""
    notes = []
    for f, d in [(392, 0.35), (370, 0.35), (349, 0.35), (330, 1.1)]:
        n = int(d * RATE)
        x = t(d)
        vib = 1 + (0.03 * np.sin(2 * np.pi * 6 * x) if d > 1 else 0)
        ph = 2 * np.pi * np.cumsum(f * vib * np.ones(n)) / RATE
        saw = 2.0 * ((ph / (2 * np.pi)) % 1.0) - 1.0
        notes.append(lowpass(saw, 6) * env(n, 0.03, d * 2) * np.minimum(1, (n - np.arange(n)) / (0.05 * RATE)))
    return norm(np.concatenate(notes)) * 0.8


def fanfare():
    notes = [(523, 0.12), (659, 0.12), (784, 0.12), (1047, 0.5)]
    out = [lowpass(tone(f, d, "square"), 3) * env(int(d * RATE), 0.005, d) * 0.5 for f, d in notes]
    return norm(np.concatenate(out))


def babble(seed, pitch, syllables, speed=1.0, rise=0.0, angry=False):
    """Gibberish speech: voiced syllables through two moving formant-ish resonances."""
    rng = np.random.default_rng(seed)
    out = []
    for k in range(syllables):
        d = rng.uniform(0.07, 0.14) / speed
        f = pitch * (1 + rise * k / max(1, syllables - 1)) * rng.uniform(0.9, 1.15)
        src = glide(f, f * rng.uniform(0.9, 1.1), d, "saw")
        vowel = rng.choice([(700, 1200), (400, 2000), (300, 900), (600, 1700), (500, 1000)])
        x = t(d)
        formant = (np.sin(2 * np.pi * vowel[0] * x) * 0.6 + np.sin(2 * np.pi * vowel[1] * x) * 0.3 + 0.4)
        syl = src * formant * env(len(x), 0.01, d / (1.6 if angry else 2.5))
        out.append(lowpass(syl, 2))
        out.append(np.zeros(int(rng.uniform(0.01, 0.04) * RATE / speed)))
    return norm(np.concatenate(out)) * 0.85


def ouch(seed, pitch, secs=0.45, down=0.55):
    x = glide(pitch * 1.8, pitch * down, secs, "saw")
    n = len(x)
    return norm(lowpass(x, 3) * env(n, 0.01, secs / 2.5)) * 0.85


def sounds():
    """name, loop, [variants]"""
    s = []
    add = lambda name, *v, loop=0: s.append((name, loop, list(v)))  # noqa: E731
    add("menu_move", blip([880]))
    add("menu_select", blip([660, 990]))
    add("button", click(1800, 0.04), click(1500, 0.04))
    add("spawn", blip([520, 780], 0.05))
    add("undo", blip([780, 520], 0.05))
    add("remove", blip([400, 300], 0.06))
    add("screenshot", cat(click(3000, 0.02), click(1200, 0.05), gap=0.05))
    for name, f in [("impact_wood", 260), ("impact_plastic", 700), ("impact_cardboard", 180), ("impact_rubber", 120)]:
        add(name, knock(1, f), knock(2, f * 1.15))
    add("impact_metal", clang(3, 420), clang(4, 530), clang(5, 610))
    add("impact_concrete", thump(6, 70, 0.2, 10), thump(7, 80, 0.2, 10))
    add("impact_glass", clang(8, 1900), clang(9, 2300))
    add("impact_flesh", thump(10, 110, 0.18, 60), thump(11, 90, 0.18, 60))
    add("ragdoll_hit", thump(12, 80, 0.25, 50), thump(13, 70, 0.25, 50))
    add("break_hard", norm(noise(0.4, 14) * env(int(0.4 * RATE), 0.001, 0.1)))
    add("break_glass", norm(mix(*[clang(15 + k, 1800 + 300 * k) for k in range(4)])))
    add("break_flesh", thump(20, 100, 0.3, 80))
    add("step_concrete", *[step(30 + k) for k in range(4)])
    add("step_grass", *[norm(lowpass(noise(0.12, 40 + k), 3) * env(int(0.12 * RATE), 0.01, 0.03)) * 0.4 for k in range(4)])
    add("step_water", norm(lowpass(noise(0.2, 50), 2) * env(int(0.2 * RATE), 0.01, 0.06)) * 0.5)
    add("jump_land", thump(60, 70, 0.2, 30))
    add("physgun_pickup", click(1200, 0.05))
    add("physgun_drop", knock(61, 340))
    add("physgun_dryfire", click(500, 0.06))
    add("empty", click(500, 0.06))
    add("splash", bubbles(62, 0.6))
    add("door_move", pour(63, 0.5))
    add("door_stop", thump(64, 60, 0.3, 20))
    # The kitchen, the till and the day
    add("sizzle", sizzle(70), sizzle(71))
    add("fryer", bubbles(72, 1.2))
    add("fries_ready", bell([1175], 0.6))
    add("fridge", mix(thump(73, 55, 0.35, 25), np.concatenate([np.zeros(int(0.1 * RATE)), knock(74, 300) * 0.5])))
    add("dispense", pour(75))
    add("serve", blip([660, 880, 1320], 0.05))
    add("assemble", blip([523, 659, 784, 1047], 0.04))
    add("ingredient", knock(80, 420), knock(81, 520))
    add("cash", ka_ching())
    add("ding", bell([1319]))
    add("door_bell", bell([1047, 784], 1.0))
    add("trash", mix(clang(76, 300), thump(78, 90, 0.3, 30)))
    add("reject", blip([300, 220], 0.08))
    add("fired", trombone())
    add("day_done", fanfare())
    add("charge_done_suit", bell([1319]))
    add("ammo_pickup", blip([660, 880, 1320], 0.05))
    # Voices: a male and a female set of gibberish (npc.c finds them as vo_<voice>_<pain|die|alert>)
    for voice, pitch, seed in [("male", 115, 100), ("female", 210, 200)]:
        add(f"vo_{voice}_alert", *[babble(seed + k, pitch * 1.25, 5, 1.3, 0.3, True) for k in range(3)])
        add(f"vo_{voice}_pain", *[ouch(seed + 10 + k, pitch * (1 + 0.1 * k)) for k in range(3)])
        add(f"vo_{voice}_die", ouch(seed + 20, pitch, 0.9, 0.35))
        add(f"vo_{voice}_order", *[babble(seed + 30 + k, pitch, 6 + k, 1.0, 0.15) for k in range(4)])
        add(f"vo_{voice}_happy", *[babble(seed + 40 + k, pitch * 1.15, 3, 1.1, 0.4) for k in range(3)])
    return s


def build(out_path, log=print):
    entries = bytearray()
    pcm = []
    total = count = 0
    for name, loop, variants in sounds():
        for v in variants:
            i16 = np.clip(np.asarray(v) * 32767.0, -32768, 32767).astype("<i2")
            entries += struct.pack("<24s3I", name.encode()[:23], total, len(i16), loop)
            pcm.append(i16.tobytes())
            total += len(i16)
            count += 1
    w = PmapWriter(PSND_MAGIC, PSND_VERSION)
    w.add("SNDS", entries, count)
    w.add("PCM ", b"".join(pcm), total)
    w.add("RATE", struct.pack("<I", RATE), 1)
    size = w.write(out_path)
    log(f"  wrote {out_path}: {count} sounds, {total / RATE:.1f} s at {RATE} Hz, {size / 1024:.0f} KB")


def preview_wavs(folder):
    """Each sound's first variant as a .wav, to listen to on a PC."""
    import os
    import wave

    os.makedirs(folder, exist_ok=True)
    for name, _loop, variants in sounds():
        with wave.open(os.path.join(folder, name + ".wav"), "wb") as f:
            f.setnchannels(1)
            f.setsampwidth(2)
            f.setframerate(RATE)
            f.writeframes(np.clip(variants[0] * 32767, -32768, 32767).astype("<i2").tobytes())
