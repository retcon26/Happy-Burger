"""Sound pack (.psnd) builder: Source .wav files -> mono 16-bit PCM at one sample rate.

It handles PCM (8/16-bit) and Microsoft ADPCM, which most HL2/GMod sounds use, and MP3 (TF2's voice lines)
through mpg123 if it's installed (sudo apt install mpg123).
A path can name the game it's from: "tf:sound/vo/scout_painsharp01.mp3" is looked for in TF2's files.
Layout matches psp/src/audio.c: a sectioned container (see pmap.py) holding:
  "SNDS": {char name[24]; u32 first_sample; u32 num_samples; u32 flags} per sound: flags bit 0 loops,
          bits 8-11 n: stored at the pack's rate / 2^n (NPC voices, named vo_*)
  "PCM ": int16 samples for all sounds, back to back
"""

import os
import shutil
import struct
import subprocess
import tempfile

import numpy as np

from .pmap import PmapWriter
from .vpk import GameFS

PSND_MAGIC = b"PSND"
PSND_VERSION = 1
VOICE_MAX_SECONDS = 1.0
ALERT_MAX_SECONDS = 1.8  # battle cries are longer (TF2's "Incoming!")

MSADPCM_ADAPT = [230, 230, 230, 230, 307, 409, 512, 614, 768, 614, 512, 409, 307, 230, 230, 230]
MSADPCM_COEF = [(256, 0), (512, -256), (0, 0), (192, 64), (240, 0), (460, -208), (392, -232)]


def read_sound_list(path):
    """Lines: name  loop(0/1)  sound/path1.wav [sound/path2.wav ...]. Several paths make random variants."""
    out = []
    with open(path) as f:
        for line in f:
            line = line.split("#", 1)[0].split()
            if len(line) >= 3:
                out.append((line[0], int(line[1]), line[2:]))
    return out


def _decode_msadpcm(data, channels, block_align, coefs):
    out = []
    pos = 0
    while pos + 7 * channels <= len(data):
        block = data[pos : pos + block_align]
        pos += block_align
        if len(block) < 7 * channels:
            break
        pred = list(block[0:channels])
        o = channels
        delta = [struct.unpack_from("<h", block, o + 2 * c)[0] for c in range(channels)]
        o += 2 * channels
        s1 = [struct.unpack_from("<h", block, o + 2 * c)[0] for c in range(channels)]
        o += 2 * channels
        s2 = [struct.unpack_from("<h", block, o + 2 * c)[0] for c in range(channels)]
        o += 2 * channels
        frames = [list(s2), list(s1)]
        c1 = [coefs[min(p, len(coefs) - 1)][0] for p in pred]
        c2 = [coefs[min(p, len(coefs) - 1)][1] for p in pred]
        nibbles = []
        for byte in block[o:]:
            nibbles.append(byte >> 4)
            nibbles.append(byte & 15)
        for i in range(0, len(nibbles) - channels + 1, channels):
            frame = []
            for c in range(channels):
                n = nibbles[i + c]
                sn = n - 16 if n & 8 else n
                p = (s1[c] * c1[c] + s2[c] * c2[c]) // 256 + sn * delta[c]
                p = max(-32768, min(32767, p))
                s2[c] = s1[c]
                s1[c] = p
                delta[c] = max(16, (MSADPCM_ADAPT[n] * delta[c]) // 256)
                frame.append(p)
            frames.append(frame)
        out.extend(frames)
    return np.array(out, dtype=np.float32).reshape(-1, channels) if out else np.zeros((0, channels), np.float32)


def decode_wav(data):
    """Returns (mono float32 samples in -1..1, sample rate), or None if the format is unsupported."""
    if data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        return None
    pos = 12
    fmt = None
    pcm = None
    while pos + 8 <= len(data):
        cid, size = struct.unpack_from("<4sI", data, pos)
        body = data[pos + 8 : pos + 8 + size]
        if cid == b"fmt ":
            fmt = body
        elif cid == b"data":
            pcm = body
        pos += 8 + size + (size & 1)
    if fmt is None or pcm is None:
        return None
    tag, channels, rate, _bps, block_align, bits = struct.unpack_from("<HHIIHH", fmt, 0)
    if tag == 1:
        if bits == 16:
            s = np.frombuffer(pcm[: len(pcm) // (2 * channels) * 2 * channels], dtype="<i2").astype(np.float32) / 32768.0
        elif bits == 8:
            s = (np.frombuffer(pcm, dtype=np.uint8).astype(np.float32) - 128.0) / 128.0
        else:
            return None
        s = s[: len(s) // channels * channels].reshape(-1, channels)
    elif tag == 2:
        coefs = MSADPCM_COEF
        if len(fmt) >= 22:
            ncoef = struct.unpack_from("<H", fmt, 20)[0]
            coefs = [struct.unpack_from("<hh", fmt, 22 + 4 * i) for i in range(ncoef)]
        s = _decode_msadpcm(pcm, channels, block_align, coefs) / 32768.0
    else:
        return None
    return s.mean(axis=1), rate


def resample(samples, src_rate, dst_rate):
    if src_rate == dst_rate or len(samples) == 0:
        return samples
    n = int(len(samples) * dst_rate / src_rate)
    x = np.linspace(0, len(samples) - 1, n)
    return np.interp(x, np.arange(len(samples)), samples).astype(np.float32)


def decode_mp3(data):
    """(mono float samples, rate) through mpg123, or None without it."""
    exe = shutil.which("mpg123")
    if not exe:
        return None
    with tempfile.TemporaryDirectory() as d:
        src, dst = os.path.join(d, "in.mp3"), os.path.join(d, "out.wav")
        with open(src, "wb") as f:
            f.write(data)
        if subprocess.run([exe, "-q", "-m", "-w", dst, src], capture_output=True).returncode != 0 or not os.path.exists(dst):
            return None
        with open(dst, "rb") as f:
            return decode_wav(f.read())


def build_pack(sound_list, gmod_root, out_path, rate, log=print):
    from .npcbuild import GAME_DIRS
    from .steam import game_dir

    fss = {"": GameFS(gmod_root)}
    entries = bytearray()
    pcm = []
    total = 0
    count = 0
    no_mp3 = 0
    for name, loop, paths in sound_list:
        for vi, path in enumerate(paths):
            game, _, rel = path.rpartition(":")
            if game not in fss:
                fss[game] = GameFS(gmod_root, [game_dir(d) for d in GAME_DIRS.get(game, [])])
            data = fss[game].read(rel)
            if data is None:
                log(f"  missing {path}")
                continue
            if rel.lower().endswith(".mp3"):
                dec = decode_mp3(data)
                if dec is None:
                    no_mp3 += 1
                    continue
            else:
                dec = decode_wav(data)
            if dec is None:
                log(f"  unsupported format {path}")
                continue
            s, src_rate = dec
            # NPC voices (vo_*) are kept at a quarter of a full-rate pack's rate (half of a reduced one's)
            # and at most VOICE_MAX_SECONDS: a whole cast of them would otherwise cost megabytes
            shift = (2 if rate >= 32000 else 1) if name.startswith("vo_") else 0
            s = resample(s, src_rate, rate >> shift)
            if shift:
                s = s[: int((ALERT_MAX_SECONDS if name.endswith("_alert") else VOICE_MAX_SECONDS) * (rate >> shift))]
            peak = float(np.abs(s).max()) if len(s) else 0.0
            if peak > 1.0:
                s = s / peak
            i16 = np.clip(s * 32767.0, -32768, 32767).astype("<i2")
            # Variants share a name and are told apart by index at runtime ("impact_wood" #0..n).
            entries += struct.pack("<24s3I", name.encode()[:23], total, len(i16), loop | (shift << 8))
            pcm.append(i16.tobytes())
            total += len(i16)
            count += 1
    if no_mp3:
        log(f"  skipped {no_mp3} MP3 sounds: install mpg123 to include them (sudo apt install mpg123)")
    w = PmapWriter(PSND_MAGIC, PSND_VERSION)
    w.add("SNDS", entries, count)

    w.add("PCM ", b"".join(pcm), total)
    w.add("RATE", struct.pack("<I", rate), 1)
    size = w.write(out_path)
    log(f"  wrote {out_path}: {count} sounds, {total} samples at {rate} Hz (voices lower), {size / 1024:.0f} KB")
