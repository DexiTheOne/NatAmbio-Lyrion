import json
import math
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

binary = sys.argv[1]


def au(samples, rate, length=None):
    frames = b"".join(struct.pack(">hh", *frame) for frame in samples)
    return struct.pack(">6I", 0x2E736E64, 24, len(frames) if length is None else length,
                       3, rate, 2) + frames


def render(base, rate, frames, *args, length=None):
    data = subprocess.run([binary, str(base), *args], input=au(frames, rate, length),
                          capture_output=True, check=True).stdout
    assert struct.unpack(">6I", data[:24]) == (
        0x2E736E64, 32, len(frames) * 6 if length is None else length, 4, rate, 2
    )
    assert len(data) == 32 + len(frames) * 6
    return data[32:]


def wave(rate, length):
    return [(int(11000 * math.sin(2 * math.pi * 620 * i / rate)),
             int(7500 * math.sin(2 * math.pi * 904 * i / rate))) for i in range(length)]


with tempfile.TemporaryDirectory() as tmp:
    base = Path(tmp) / "player"
    settings = base.with_suffix(".settings")
    for rate in (44100, 48000, 88200, 96000, 176400, 192000):
        signal = wave(rate, rate // 7 + 29)
        settings.write_text("bypass 1\nreset 1\n")
        expected = b"".join(x.to_bytes(3, "big", signed=True) for pair in signal
                            for x in (pair[0] << 8, pair[1] << 8))
        assert render(base, rate, signal) == expected
        assert render(base, rate, signal[:31], length=0xFFFFFFFF) == expected[:31 * 6]

        settings.write_text("reset 1\n")
        baseline = render(base, rate, signal)
        assert baseline != expected and any(baseline)
        for control in ("nae_bypass 1", "xtc_bypass 1", "preamp -6"):
            settings.write_text(f"reset 1\n{control}\n")
            assert render(base, rate, signal) != baseline, control
        data = json.loads(base.with_suffix(".telemetry").read_text())
        assert data["rate"] == rate and 0 <= data["frames"] <= len(signal)

    rate = 48000
    signal = wave(rate, 8113)
    settings.write_text("reset 1\nepoch 1\n")
    clean = render(base, rate, signal)
    assert render(base, rate, signal) == clean
    settings.write_text("reset 0\nepoch 1\n")
    assert render(base, rate, signal) != clean
    assert render(base, rate, signal, "trim", "3") == clean
    settings.write_text("reset 0\nepoch 2\n")
    assert render(base, rate, signal) == clean

    settings.write_text("reset 1\nnae_bypass 1\nxtc_bypass 1\ntrim 6\n")
    loud = [(left * 2, right * 2) for left, right in wave(rate, rate // 5)]
    render(base, rate, loud)
    active = json.loads(base.with_suffix(".telemetry").read_text())
    render(base, rate, loud + [(0, 0)] * (rate // 2))
    quiet = json.loads(base.with_suffix(".telemetry").read_text())
    assert active["clipped"] and quiet["clipped"]
    assert quiet["peak"] < active["peak"] / 3
    assert quiet["left"] < active["left"] / 5
    assert quiet["cpu"] >= 0
