#!/usr/bin/env python3
# replay_stream.py: reads a YG Baiji replay stream (.ygr, Session/Keyframe.h "Replay streams") in
# Python, for tests: the header, every frame (each seat's pad bytes, ports, typed events) and the
# session checksums it carries. Chunk payloads are zstd; this uses the zstd command line tool.
#
#   python3 Tools/orca/replay_stream.py <file.ygr>     prints a summary
import json
import struct
import subprocess
import sys

STREAM_MAGIC = b"YGRS"
CHUNK_MAGIC = b"YGRC"
PADS_BYTES = 4 * 8
# Magic, first frame, frame count, live frame, checksum count, payload size, payload XXH3.
PREAMBLE = 4 + 4 + 4 + 4 + 4 + 4 + 8


class Stream:
    def __init__(self):
        self.info = None
        self.frames = []  # (pads bytes, ports tuple, header tuple|None, clear, menu_exit, seed)
        self.checksums = {}
        self.chunks = 0
        self.bytes = 0
        self.live = -1

    @property
    def first(self):
        return self.info["first_frame"]

    @property
    def end(self):
        return self.first + len(self.frames)


def _unzstd(payload):
    out = subprocess.run(["zstd", "-dq", "-c"], input=payload, stdout=subprocess.PIPE, check=True)
    return out.stdout


def _frames(raw, count, checks):
    pos = 0

    def take(n):
        nonlocal pos
        if pos + n > len(raw):
            raise ValueError("truncated chunk")
        b = raw[pos:pos + n]
        pos += n
        return b

    def u32():
        return struct.unpack("<I", take(4))[0]

    def u64():
        return struct.unpack("<Q", take(8))[0]

    frames = []
    for _ in range(count):
        pads = take(PADS_BYTES)
        ports = []
        for _ in range(u32()):
            port = u32()
            name = take(u32())
            controls = take(u64())
            queue = take(u64())
            ports.append((port, name, controls, queue))
        header = tuple(u32() for _ in range(5)) if u32() else None
        clear = u32()
        tag_count = u32()
        tags = tuple(u32() for _ in range(tag_count - 1)) if tag_count else None
        menu_exit, seed = u32(), u32()
        frames.append((pads, tuple(ports), header, clear, menu_exit, seed, tags))
    checksums = {}
    for _ in range(checks):
        frame = u32()
        checksums[frame] = u64()
    if pos != len(raw):
        raise ValueError("trailing bytes in chunk")
    return frames, checksums


def read(path):
    data = open(path, "rb").read()
    s = Stream()
    s.bytes = len(data)
    if data[:4] != STREAM_MAGIC:
        raise ValueError("not a replay stream")
    version, size = struct.unpack("<II", data[4:12])
    if version != 1:
        raise ValueError(f"version {version}")
    s.info = json.loads(data[12:12 + size])
    # The frame layout is the header's "v" (2 = ORP3), but the site rewrites a tournament
    # download's to 1: this tool reads only 0.3.46+ recordings (compat 31 and up).
    pos = 12 + size
    while len(data) - pos >= PREAMBLE:
        if data[pos:pos + 4] != CHUNK_MAGIC:
            raise ValueError(f"bad chunk at byte {pos}")
        first, count, live, checks, size = struct.unpack("<IIIII", data[pos + 4:pos + 24])
        if len(data) - pos - PREAMBLE < size:
            break  # a partial chunk at the end of a live file
        if first != s.end:
            raise ValueError(f"chunk at frame {first}, expected {s.end}")
        payload = data[pos + PREAMBLE:pos + PREAMBLE + size]
        frames, checksums = _frames(_unzstd(payload), count, checks)
        s.frames.extend(frames)
        s.checksums.update(checksums)
        s.chunks += 1
        s.live = max(s.live, live)
        pos += PREAMBLE + size
    return s


def summary(s):
    seconds = len(s.frames) / 60
    return (f"{s.info['game']} build {s.info['build'][:10]} frames {s.first}-{s.end} "
            f"({seconds:.0f} s), {len(s.checksums)} checksums, {s.chunks} chunks, {s.bytes} bytes "
            f"({s.bytes / max(seconds / 60, 1e-9) / 1024:.1f} KB per minute)")


if __name__ == "__main__":
    print(summary(read(sys.argv[1])))
