import json
import math
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import time

binary = sys.argv[1]


def au(samples, rate, length=None):
    frames = b"".join(struct.pack(">hh", *frame) for frame in samples)
    return struct.pack(">6I", 0x2E736E64, 24, len(frames) if length is None else length,
                       3, rate, 2) + frames


def render(base, rate, frames, *args, length=None):
    data = subprocess.run([binary, str(base), *args], input=au(frames, rate, length),
                          capture_output=True, check=True, timeout=10).stdout
    assert struct.unpack(">6I", data[:24]) == (
        0x2E736E64, 32, len(frames) * 6 if length is None else length, 4, rate, 2
    )
    assert len(data) == 32 + len(frames) * 6
    return data[32:]


def wave(rate, length):
    return [(int(11000 * math.sin(2 * math.pi * 620 * i / rate)),
             int(7500 * math.sin(2 * math.pi * 904 * i / rate))) for i in range(length)]


def wait_for(condition, message):
    for _ in range(300):
        if condition():
            return
        time.sleep(.01)
    raise AssertionError(message)


tmpfs = Path("/dev/shm")
probe = subprocess.run([binary, "--is-tmpfs", str(tmpfs)], timeout=5)
scratch = tmpfs if probe.returncode == 0 else None
with tempfile.TemporaryDirectory(dir=scratch) as tmp:
    missing = subprocess.run([binary, "--is-tmpfs", str(Path(tmp) / "missing")], timeout=5)
    assert missing.returncode == 1
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
        assert not base.with_suffix(".history").exists()

    rate = 48000
    signal = wave(rate, 8113)
    settings.write_text("reset 1\nepoch 1\n")
    clean = render(base, rate, signal)
    assert render(base, rate, signal) == clean
    settings.write_text("reset 0\nepoch 1\n")
    assert render(base, rate, signal) == clean
    assert render(base, rate, signal) != clean
    assert render(base, rate, signal, "trim", "3") == clean
    settings.write_text("reset 0\nepoch 2\n")
    assert render(base, rate, signal) == clean

    lease = base.with_suffix(".viewing")

    def renew(session, seconds=30):
        replacement = lease.with_suffix(".new")
        replacement.write_text(f"{session} {time.time() + seconds}\n")
        replacement.replace(lease)

    renew(1)
    # Keep an old helper alive while a new track starts and finishes on the same player.
    settings.write_text("reset 0\nepoch 2\n")
    previous_signal = signal[:1536]
    previous_timeline = base.with_suffix(".219.timeline")
    with subprocess.Popen([binary, str(base), "--stream=219"], stdin=subprocess.PIPE,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE) as previous:
        previous.stdin.write(au(previous_signal, rate, 0xFFFFFFFF))
        previous.stdin.flush()
        for _ in range(100):
            if previous_timeline.with_suffix(".rendering").exists():
                break
            time.sleep(.01)
        else:
            raise AssertionError("Previous helper did not start")
        render(base, 44100, wave(44100, 8192), "--stream=220")
        history_path = base.with_suffix(".history")
        next_history = history_path.read_bytes()
        output, errors = previous.communicate(timeout=5)
        assert previous.returncode == 0, errors
        assert len(output) == 32 + len(previous_signal) * 6
        assert history_path.read_bytes() == next_history, "Old helper replaced the new track's history"

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

    settings.write_text("reset 1\nrevision 17\nazimuth 22\n")
    render(base, rate, [(0, 0)] * (rate // 2) + loud, "--stream=221")
    timeline = base.with_suffix(".221.timeline").read_bytes()
    latest = json.loads(timeline[:512])
    before = json.loads(timeline[2 * 512:3 * 512])
    assert latest["frames"] == rate // 2 + len(loud)
    assert before["slot"] == 1 and before["peak"] == 0
    assert latest["peak"] > 0
    assert latest["revision"] == before["revision"] == 17
    assert latest["azimuth"] == before["azimuth"] == 22

    settings.write_text("reset 0\nrevision 17\n")
    track = au(wave(rate, rate // 3), rate)
    split = 24 + 1536 * 4
    live_path = base.with_suffix(".222.timeline")
    with subprocess.Popen([binary, str(base), "--stream=222"], stdin=subprocess.PIPE,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE) as live:
        live.stdin.write(track[:split])
        live.stdin.flush()
        for _ in range(100):
            if live_path.exists() and live_path.stat().st_size >= 1024:
                break
            time.sleep(.01)
        else:
            live.kill()
            raise AssertionError("First telemetry record was not rendered")
        settings.write_text("reset 1\nrevision 18\ntrim -6\n")
        output, errors = live.communicate(track[split:], timeout=10)
        assert live.returncode == 0, errors
        assert len(output) == 32 + rate // 3 * 6
        assert not base.with_suffix(".history").exists()
    records = live_path.read_bytes()
    latest = json.loads(records[:512])
    initial = json.loads(records[512:1024])
    assert initial["revision"] == 17 and initial["appliedFrame"] == 0
    assert latest["revision"] == 18 and 0 < latest["appliedFrame"] < latest["frames"]
    for offset in range(512, len(records), 512):
        record = json.loads(records[offset:offset + 512])
        if record["frames"] <= latest["appliedFrame"]:
            assert record["revision"] == 17
        else:
            assert record["revision"] == 18 and record["appliedFrame"] == latest["appliedFrame"]

    # Watch, leave, and return during one stream. PCM stays identical throughout.
    lease.unlink()
    settings.write_text("reset 1\n")
    frames = wave(rate, 5 * 8192)
    track = au(frames, rate)
    path = base.with_suffix(".223.timeline")
    with tempfile.TemporaryFile() as pcm, subprocess.Popen(
            [binary, str(base), "--stream=223"], stdin=subprocess.PIPE,
            stdout=pcm, stderr=subprocess.PIPE) as live:
        def feed(start, end):
            live.stdin.write(track[24 + start * 4:24 + end * 4])
            live.stdin.flush()

        live.stdin.write(track[:24])
        feed(0, 8192)
        wait_for(lambda: pcm.tell() >= 32768, "Unwatched audio did not render")
        assert not path.exists()

        renew(2)
        time.sleep(.25)
        feed(8192, 16384)
        wait_for(lambda: path.exists() and path.stat().st_size >= 512,
                 "Watching did not start telemetry")
        wait_for(lambda: pcm.tell() >= 81920, "Watched audio did not render")

        renew(2, seconds=-1)
        time.sleep(.25)
        before = path.read_bytes()
        assert json.loads(before[:512])["viewerSession"] == 2
        feed(16384, 32768)
        wait_for(lambda: pcm.tell() >= 180224, "Audio stopped after the lease expired")
        assert path.read_bytes() == before, "Expired viewer still generated telemetry"

        renew(3)
        time.sleep(.25)
        _, errors = live.communicate(track[24 + 32768 * 4:], timeout=10)
        assert live.returncode == 0, errors
        latest = json.loads(path.read_bytes()[:512])
        assert latest["viewerSession"] == 3 and latest["frames"] == len(frames)
        pcm.seek(32)
        assert pcm.read() == render(base, rate, frames)
