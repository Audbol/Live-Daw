"""WAV reading and writing for the audio comparison kit.

Uses the soundfile package when it is installed; otherwise reads RIFF/RF64
files itself: 16/24/32-bit PCM and 32/64-bit float, plain or
WAVE_FORMAT_EXTENSIBLE (what consoles, DAWs and Sidecar's render write).
Samples come back as float64 in -1..1, shaped (frames, channels).
"""

import struct

import numpy as np

try:  # optional
    import soundfile as _sf
except Exception:  # pragma: no cover - depends on the machine
    _sf = None

PCM = 1
FLOAT = 3
EXTENSIBLE = 0xFFFE


def read_wav(path, use_soundfile=True):
    """Returns (data, sample_rate) with data as float64 (frames, channels)."""
    if use_soundfile and _sf is not None:
        data, sr = _sf.read(str(path), dtype="float64", always_2d=True)
        return data, int(sr)
    with open(path, "rb") as f:
        raw = f.read()
    return _parse(raw, str(path))


def _parse(raw, name):
    if len(raw) < 12 or raw[8:12] != b"WAVE" or raw[0:4] not in (b"RIFF", b"RF64"):
        raise ValueError(f"{name}: not a WAV file")
    pos = 12
    fmt = None
    data_size64 = None
    data = None
    while pos + 8 <= len(raw):
        cid = raw[pos:pos + 4]
        size = struct.unpack("<I", raw[pos + 4:pos + 8])[0]
        body = pos + 8
        if cid == b"ds64":
            data_size64 = struct.unpack("<Q", raw[body + 8:body + 16])[0]
        elif cid == b"fmt ":
            tag, channels, sr, _, _, bits = struct.unpack("<HHIIHH", raw[body:body + 16])
            if tag == EXTENSIBLE and size >= 40:
                tag = struct.unpack("<H", raw[body + 24:body + 26])[0]
            fmt = (tag, channels, sr, bits)
        elif cid == b"data":
            if size == 0xFFFFFFFF and data_size64 is not None:
                size = data_size64
            size = min(size, len(raw) - body)
            data = raw[body:body + size]
            break
        pos = body + size + (size & 1)
    if fmt is None or data is None:
        raise ValueError(f"{name}: no fmt or data chunk")
    tag, channels, sr, bits = fmt
    width = bits // 8
    frames = len(data) // (width * channels)
    data = data[:frames * width * channels]
    if tag == FLOAT and bits == 32:
        x = np.frombuffer(data, dtype="<f4").astype(np.float64)
    elif tag == FLOAT and bits == 64:
        x = np.frombuffer(data, dtype="<f8").copy()
    elif tag == PCM and bits == 16:
        x = np.frombuffer(data, dtype="<i2") / 32768.0
    elif tag == PCM and bits == 24:
        b = np.frombuffer(data, dtype=np.uint8).reshape(-1, 3).astype(np.int32)
        v = b[:, 0] | (b[:, 1] << 8) | (b[:, 2] << 16)
        v = np.where(v >= 1 << 23, v - (1 << 24), v)
        x = v / 8388608.0
    elif tag == PCM and bits == 32:
        x = np.frombuffer(data, dtype="<i4") / 2147483648.0
    elif tag == PCM and bits == 8:
        x = (np.frombuffer(data, dtype=np.uint8).astype(np.float64) - 128.0) / 128.0
    else:
        raise ValueError(f"{name}: unsupported WAV format (tag {tag}, {bits} bit)")
    return x.reshape(frames, channels), int(sr)


def write_wav(path, data, sr, bits=24):
    """Writes float data (frames[, channels]) as 16/24/32-bit PCM or bits='float'."""
    x = np.asarray(data, dtype=np.float64)
    if x.ndim == 1:
        x = x[:, None]
    frames, channels = x.shape
    if bits == "float":
        tag, width, payload = FLOAT, 4, x.astype("<f4").tobytes()
    else:
        tag, width = PCM, bits // 8
        scale = float(1 << (bits - 1))
        v = np.clip(np.round(x * scale), -scale, scale - 1).astype(np.int64)
        if bits == 16:
            payload = v.astype("<i2").tobytes()
        elif bits == 24:
            u = (v & 0xFFFFFF).astype(np.uint32)
            b = np.stack([u & 0xFF, (u >> 8) & 0xFF, (u >> 16) & 0xFF], axis=-1).astype(np.uint8)
            payload = b.tobytes()
        elif bits == 32:
            payload = v.astype("<i4").tobytes()
        else:
            raise ValueError("bits must be 16, 24, 32 or 'float'")
    block = width * channels
    fmt = struct.pack("<HHIIHH", tag, channels, int(sr), int(sr) * block, block, width * 8)
    chunks = b"fmt " + struct.pack("<I", len(fmt)) + fmt
    if tag == FLOAT:
        chunks += b"fact" + struct.pack("<II", 4, frames)
    chunks += b"data" + struct.pack("<I", len(payload)) + payload
    if len(payload) & 1:
        chunks += b"\0"
    with open(path, "wb") as f:
        f.write(b"RIFF" + struct.pack("<I", 4 + len(chunks)) + b"WAVE" + chunks)
