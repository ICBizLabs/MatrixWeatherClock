#!/usr/bin/env python3
"""
Builds the clock's voice pack: one short spoken clip per NWS alert event type plus a few fixed phrases
("Weather alert", "Lightning nearby", "Alarm", "Timer finished", the demo scenario names), rendered offline with
Piper TTS and stored as G.711 mu-law (8-bit, mono, 22050 Hz) in a single file the firmware downloads into LittleFS.

    python tools/make_voice_pack.py --voice en_US-ljspeech-medium --out installer

Bump PACK_VERSION whenever the phrase list, the voice or the file format changes: the firmware only re-downloads
when the manifest's voice.version differs from the installed pack (Piper output is not bit-identical between runs,
so the md5 is used to verify the download, not to detect changes).

Pack layout (little-endian, format 2; format 1 was 4-bit IMA ADPCM):
  header 72 bytes: "MWCV", u16 format, u16 header_len, u32 sample_rate, u32 count, u32 pack_version, u32 index_off,
                   u32 names_off, u32 names_len, u32 data_off, u32 file_size, char voice[32]
  index: count x { u32 key (FNV-1a of the normalized phrase), u32 offset, u32 bytes, u32 samples }, sorted by key
  names: count NUL-terminated normalized phrases in index order
  data:  mu-law clips (one byte per sample), 4-byte aligned
"""
import argparse, json, math, os, pathlib, struct, sys, urllib.request, wave
from array import array

ROOT = pathlib.Path(__file__).resolve().parent.parent
EVENTS_FILE = ROOT / "tools" / "nws_event_types.json"
NWS_TYPES_URL = "https://api.weather.gov/alerts/types"
HF_BASE = "https://huggingface.co/rhasspy/piper-voices/resolve/v1.0.0/"

PACK_VERSION = 7
FORMAT = 2
RATE = 22050
MAGIC = b"MWCV"
HEADER = "<4sHHIIIIIIII32s"   # 72 bytes
HEADER_LEN = struct.calcsize(HEADER)
MAX_CLIPS = 1024

# Fixed phrases the firmware speaks besides the NWS event names (keys are normalized, see normalize()).
EXTRA_PHRASES = [
    "weather alert", "test alert", "lightning nearby", "alarm", "timer finished",
    # demo scenario names
    "sunny", "date", "rain", "snow", "thunderstorm", "lightning", "wind", "high and low", "sunrise and sunset",
    "forecast", "hourly graph", "tornado warning", "winter storm watch", "timer", "message", "christmas",
    "fourth of july", "valentine's day", "halloween", "night mode", "indoor", "radar", "air quality", "barometer",
    "air quality poor", "tide", "high tide", "low tide", "moon", "full moon", "new moon",
]
# What Piper reads when it differs from the key (the key stays the NWS name so lookups work).
SPEAK_AS = {
    "911 telephone outage": "nine one one telephone outage",
}

STEP = [7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97,
        107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
        876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428,
        4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350,
        22385, 24623, 27086, 29794, 32767]
IDX = [-1, -1, -1, -1, 2, 4, 6, 8]


def normalize(s):
    return " ".join(s.split()).lower()


def fnv1a(s):
    h = 2166136261
    for b in s.encode("utf-8"):
        h = ((h ^ b) * 16777619) & 0xFFFFFFFF
    return h


def ima_encode(samples):
    pred, idx, out, lo = 0, 0, bytearray(), None
    for s in samples:
        step = STEP[idx]
        diff = s - pred
        code = 0
        if diff < 0:
            code, diff = 8, -diff
        d = step >> 3
        if diff >= step:
            code |= 4; diff -= step; d += step
        step >>= 1
        if diff >= step:
            code |= 2; diff -= step; d += step
        step >>= 1
        if diff >= step:
            code |= 1; d += step
        pred = pred - d if code & 8 else pred + d
        pred = max(-32768, min(32767, pred))
        idx = max(0, min(88, idx + IDX[code & 7]))
        if lo is None:
            lo = code
        else:
            out.append(lo | (code << 4)); lo = None
    if lo is not None:
        out.append(lo)
    return bytes(out)


def ima_decode(data, n):
    pred, idx, out = 0, 0, array("h")
    for i in range(n):
        code = (data[i >> 1] >> (4 if i & 1 else 0)) & 0xF
        step = STEP[idx]
        d = step >> 3
        if code & 4: d += step
        if code & 2: d += step >> 1
        if code & 1: d += step >> 2
        pred = pred - d if code & 8 else pred + d
        pred = max(-32768, min(32767, pred))
        idx = max(0, min(88, idx + IDX[code & 7]))
        out.append(pred)
    return out


ULAW_BIAS = 0x84
ULAW_CLIP = 32635


def ulaw_encode(samples):
    out = bytearray(len(samples))
    for i, s in enumerate(samples):
        sign = 0x80 if s < 0 else 0
        if s < 0:
            s = -s
        if s > ULAW_CLIP:
            s = ULAW_CLIP
        s += ULAW_BIAS
        exp = 7
        mask = 0x4000
        while exp > 0 and not (s & mask):
            exp -= 1
            mask >>= 1
        mant = (s >> (exp + 3)) & 0x0F
        out[i] = (~(sign | (exp << 4) | mant)) & 0xFF
    return bytes(out)


def ulaw_decode(data, n):
    out = array("h")
    for i in range(n):
        b = (~data[i]) & 0xFF
        exp = (b >> 4) & 7
        s = (((b & 0x0F) << 3) + ULAW_BIAS) << exp
        s -= ULAW_BIAS
        out.append(-s if b & 0x80 else s)
    return out


def snr_db(orig, dec):
    sig = sum(float(a) * a for a in orig)
    err = sum((float(a) - b) ** 2 for a, b in zip(orig, dec))
    if err == 0:
        return 99.0
    if sig == 0:
        return 0.0
    return 10 * math.log10(sig / err)


def trim_normalize(pcm):
    """Cuts leading/trailing silence (below -45 dBFS), fades the edges and normalizes the peak to -3 dBFS."""
    win = RATE * 20 // 1000
    thr = 184
    n = len(pcm)
    first, last = None, None
    for i in range(0, n, win):
        if max(abs(v) for v in pcm[i:i + win]) >= thr:
            if first is None:
                first = i
            last = min(n, i + win)
    if first is None:
        return array("h")
    a = max(0, first - RATE * 60 // 1000)
    b = min(n, last + RATE * 120 // 1000)
    out = array("h", pcm[a:b])
    fade = RATE * 10 // 1000
    for i in range(min(fade, len(out))):
        out[i] = int(out[i] * i / fade)
        out[-1 - i] = int(out[-1 - i] * i / fade)
    peak = max(1, max(abs(v) for v in out))
    g = 23197 / peak
    for i in range(len(out)):
        out[i] = max(-32768, min(32767, int(round(out[i] * g))))
    return out


def read_wav(path):
    with wave.open(str(path), "rb") as w:
        if w.getframerate() != RATE or w.getnchannels() != 1 or w.getsampwidth() != 2:
            sys.exit(f"{path}: expected {RATE} Hz mono 16-bit, got {w.getframerate()} Hz {w.getnchannels()} ch {w.getsampwidth() * 8}-bit")
        pcm = array("h")
        pcm.frombytes(w.readframes(w.getnframes()))
        return pcm


def write_wav(path, pcm):
    with wave.open(str(path), "wb") as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(RATE)
        w.writeframes(pcm.tobytes())


def voice_model(voice, voices_dir):
    """Returns the .onnx path, downloading model + config from the piper-voices repository when missing."""
    voices_dir = pathlib.Path(voices_dir).expanduser()
    voices_dir.mkdir(parents=True, exist_ok=True)
    onnx = voices_dir / f"{voice}.onnx"
    cfg = voices_dir / f"{voice}.onnx.json"
    if onnx.exists() and cfg.exists():
        return onnx
    lang, name, quality = voice.split("-", 2)
    base = f"{HF_BASE}{lang.split('_')[0]}/{lang}/{name}/{quality}/"
    for p in (onnx, cfg):
        url = base + p.name
        print(f"downloading {url}")
        req = urllib.request.Request(url, headers={"User-Agent": "matrix-weather-clock-tools"})
        with urllib.request.urlopen(req, timeout=120) as r, open(p, "wb") as f:
            while True:
                chunk = r.read(1 << 20)
                if not chunk:
                    break
                f.write(chunk)
    return onnx


def load_events(refresh, offline):
    if refresh and not offline:
        req = urllib.request.Request(NWS_TYPES_URL, headers={"User-Agent": "matrix-weather-clock-tools", "Accept": "application/ld+json"})
        with urllib.request.urlopen(req, timeout=30) as r:
            types = sorted(set(json.load(r)["eventTypes"]))
        EVENTS_FILE.write_text(json.dumps({"source": NWS_TYPES_URL, "eventTypes": types}, indent=1) + "\n")
        print(f"refreshed {EVENTS_FILE.name}: {len(types)} event types")
    return json.loads(EVENTS_FILE.read_text())["eventTypes"]


def phrase_list(events):
    keys = {}
    for text in list(events) + EXTRA_PHRASES:
        k = normalize(text)
        if k in keys:
            continue
        h = fnv1a(k)
        for other, oh in keys.items():
            if oh == h:
                sys.exit(f"FNV-1a collision between '{k}' and '{other}'")
        keys[k] = h
    return sorted(keys)   # normalized phrases


def synthesize_all(voice_name, onnx, phrases, tmpdir):
    from piper import PiperVoice
    v = PiperVoice.load(onnx)
    clips = {}
    for i, key in enumerate(phrases):
        text = SPEAK_AS.get(key, key)
        wav = tmpdir / f"{i:03d}.wav"
        with wave.open(str(wav), "wb") as w:
            v.synthesize_wav(text, w)
        pcm = trim_normalize(read_wav(wav))
        if len(pcm) < RATE // 10:
            sys.exit(f"'{key}': synthesized clip too short")
        clips[key] = pcm
        print(f"  [{i + 1}/{len(phrases)}] {key} ({len(pcm) / RATE:.2f} s)")
    return clips


def build_pack(voice_name, clips, pack_version):
    entries = []
    for key, pcm in clips.items():
        entries.append([fnv1a(key), key, ulaw_encode(pcm), len(pcm)])
    entries.sort(key=lambda e: e[0])
    count = len(entries)
    if count > MAX_CLIPS:
        sys.exit(f"too many clips ({count} > {MAX_CLIPS})")
    names = b"".join(e[1].encode("utf-8") + b"\0" for e in entries)
    index_off = HEADER_LEN
    names_off = index_off + 16 * count
    data_off = (names_off + len(names) + 3) & ~3
    data = bytearray()
    index = bytearray()
    for h, key, adpcm, samples in entries:
        off = data_off + len(data)
        index += struct.pack("<IIII", h, off, len(adpcm), samples)
        data += adpcm
        while len(data) & 3:
            data.append(0)
    file_size = data_off + len(data)
    header = struct.pack(HEADER, MAGIC, FORMAT, HEADER_LEN, RATE, count, pack_version, index_off, names_off,
                         len(names), data_off, file_size, voice_name.encode("ascii")[:31])
    blob = header + index + names + b"\0" * (data_off - names_off - len(names)) + data
    assert len(blob) == file_size
    return blob, entries


def read_pack(blob):
    magic, fmt, hlen, rate, count, ver, index_off, names_off, names_len, data_off, file_size, voice = struct.unpack(HEADER, blob[:HEADER_LEN])
    if magic != MAGIC or fmt not in (1, 2) or hlen != HEADER_LEN or file_size != len(blob):
        sys.exit("pack header invalid")
    index = [struct.unpack("<IIII", blob[index_off + 16 * i: index_off + 16 * i + 16]) for i in range(count)]
    names = blob[names_off: names_off + names_len].split(b"\0")[:count]
    return {"rate": rate, "count": count, "version": ver, "voice": voice.rstrip(b"\0").decode(),
            "index": index, "names": [n.decode() for n in names]}


def verify_pack(blob, clips, dump_dir=None):
    p = read_pack(blob)
    worst = 99.0
    for (h, off, nbytes, samples), name in zip(p["index"], p["names"]):
        if fnv1a(name) != h:
            sys.exit(f"index/name mismatch for '{name}'")
        dec = ulaw_decode(blob[off: off + nbytes], samples)
        s = snr_db(clips[name], dec)
        worst = min(worst, s)
        if s < 25:
            sys.exit(f"'{name}': mu-law round trip SNR {s:.1f} dB is too low")
        if dump_dir:
            write_wav(pathlib.Path(dump_dir) / (name.replace(" ", "_").replace("'", "") + ".wav"), dec)
    return p, worst


def selftest():
    # synthetic clip: a decaying 440 Hz tone with a little noise, plus the phrase list key check
    import random
    random.seed(1)
    n = RATE * 2
    pcm = array("h", (int(20000 * math.sin(2 * math.pi * 440 * i / RATE) * math.exp(-i / n) + random.randint(-300, 300)) for i in range(n)))
    enc = ima_encode(pcm)
    dec = ima_decode(enc, n)
    s_adpcm = snr_db(pcm, dec)
    assert len(enc) == n // 2 and s_adpcm >= 15, f"ADPCM round trip SNR {s_adpcm:.1f} dB"
    enc = ulaw_encode(pcm)
    dec = ulaw_decode(enc, n)
    s = snr_db(pcm, dec)
    assert len(enc) == n, "encoded size"
    assert s >= 30, f"mu-law round trip SNR {s:.1f} dB"
    phrases = phrase_list(load_events(False, True))
    clips = {k: pcm for k in phrases[:3]}
    blob, entries = build_pack("selftest-voice", clips, 7)
    p, worst = verify_pack(blob, clips)
    assert p["count"] == 3 and p["version"] == 7 and p["voice"] == "selftest-voice"
    assert [e[0] for e in entries] == sorted(e[0] for e in entries)
    print(f"selftest ok: mu-law round trip SNR {s:.1f} dB (ADPCM {s_adpcm:.1f} dB), {len(phrases)} phrases, no key collisions, pack {len(blob)} bytes")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--voice", default="en_US-ljspeech-medium", help="Piper voice name (default en_US-ljspeech-medium)")
    ap.add_argument("--voices-dir", default="~/.cache/piper-voices", help="where voice models are stored / downloaded")
    ap.add_argument("--out", default="installer", help="output directory (default installer)")
    ap.add_argument("--pack-version", type=int, default=PACK_VERSION)
    ap.add_argument("--refresh-events", action="store_true", help="re-fetch the NWS event type list into tools/nws_event_types.json")
    ap.add_argument("--offline", action="store_true", help="never touch the network (CI): fail if the model is missing")
    ap.add_argument("--dump-wav", metavar="DIR", help="write every clip, decoded from the pack, as WAV for listening")
    ap.add_argument("--selftest", action="store_true", help="test the codec and pack writer without Piper")
    a = ap.parse_args()
    if a.selftest:
        selftest(); return
    phrases = phrase_list(load_events(a.refresh_events, a.offline))
    voices_dir = pathlib.Path(a.voices_dir).expanduser()
    if a.offline and not (voices_dir / f"{a.voice}.onnx").exists():
        sys.exit(f"offline: {voices_dir / (a.voice + '.onnx')} missing")
    onnx = voice_model(a.voice, voices_dir)
    out = (ROOT / a.out) if not os.path.isabs(a.out) else pathlib.Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    import tempfile
    with tempfile.TemporaryDirectory() as td:
        print(f"synthesizing {len(phrases)} phrases with {a.voice}")
        clips = synthesize_all(a.voice, onnx, phrases, pathlib.Path(td))
    blob, entries = build_pack(a.voice, clips, a.pack_version)
    if a.dump_wav:
        pathlib.Path(a.dump_wav).mkdir(parents=True, exist_ok=True)
    p, worst = verify_pack(blob, clips, a.dump_wav)
    short = a.voice.rsplit("-", 1)[0] if a.voice.rsplit("-", 1)[-1] in ("x_low", "low", "medium", "high") else a.voice
    for old in out.glob("voice-*.pack"):
        old.unlink()
    path = out / f"voice-{short}-v{a.pack_version}.pack"
    path.write_bytes(blob)
    secs = sum(len(c) for c in clips.values()) / RATE
    print(f"pack: {path.name}, {p['count']} clips, {len(blob):,} bytes, {secs:.1f} s of audio, worst SNR {worst:.1f} dB")


if __name__ == "__main__":
    main()
