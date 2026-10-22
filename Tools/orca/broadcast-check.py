#!/usr/bin/env python3
# Copyright 2026 YouGame
# SPDX-License-Identifier: GPL-2.0-or-later
"""Launch an offline product replay with an app-format ring and inspect its media (no dependencies)."""
import argparse
import os
from pathlib import Path
import queue
import struct
import subprocess
import tempfile
import threading
import time

HEADER = 256
PREFIX = 64
VIDEO_BYTES = 1280 * 720 * 4
AUDIO_BYTES = 480 * 2 * 2
VIDEO_STRIDE = PREFIX + VIDEO_BYTES
AUDIO_STRIDE = PREFIX + AUDIO_BYTES
AUDIO_OFFSET = HEADER + 3 * VIDEO_STRIDE
SIZE = AUDIO_OFFSET + 64 * AUDIO_STRIDE
MAX_TICKET = 0x7fffff80
FIELDS = (0x31424759, 1, 1280, 720, 30, 48000, 2, 3, 64, VIDEO_BYTES, 480, SIZE)


def create_ring(file):
    # App: exclusive 0600 file, fully allocated/zeroed, twelve LE words in a 256-byte header.
    data = bytearray(SIZE)
    struct.pack_into('<12I', data, 0, *FIELDS)
    fd = os.open(file, os.O_CREAT | os.O_EXCL | os.O_RDWR, 0o600)
    with os.fdopen(fd, 'wb') as handle:
        handle.write(data)


def packet(handle, audio, ticket):
    slots, stride, base, size, count = ((64, AUDIO_STRIDE, AUDIO_OFFSET, AUDIO_BYTES, 480)
                                       if audio else (3, VIDEO_STRIDE, HEADER, VIDEO_BYTES, 1))
    offset = base + ((ticket - 1) % slots) * stride
    handle.seek(offset)
    prefix = handle.read(PREFIX)
    if len(prefix) != PREFIX:
        return None
    guard, length, timestamp, frames = struct.unpack_from('<IIQI', prefix)
    if guard != ticket * 2 or length != size or frames != count:
        return None
    data = handle.read(size)
    handle.seek(offset)
    after = handle.read(4)
    if len(data) != size or after != struct.pack('<I', guard):
        return None
    assert timestamp <= 2**53 - 1, 'App cannot represent the timestamp'
    return timestamp, data


def distance(newer, older):
    return newer - older if newer >= older else MAX_TICKET - older + newer


def launch_env(temp, ring):
    # Explicit allowlist: no inherited app/session variables or credentials.
    result = {'PATH': os.defpath, 'TMPDIR': str(temp), 'TEMP': str(temp), 'TMP': str(temp),
              'ORCA_SESSION': '1', 'ORCA_TEST_COMMANDS': '1', 'ORCA_NO_LOCAL_INPUT': '1',
              'SDL_JOYSTICK_HIDAPI': '0', 'SDL_JOYSTICK_HIDAPI_GAMECUBE': '0',
              'SDL_HIDAPI_LIBUSB': '0', 'SDL_JOYSTICK_IOKIT': '0', 'SDL_JOYSTICK_MFI': '0',
              'SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT': '0x0000/0x0000',
              'ORCA_BROADCAST_SINK': str(ring)}
    if os.name == 'nt':
        result['SystemRoot'] = os.environ.get('SystemRoot', r'C:\Windows')
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    parser.add_argument('disc', type=Path)
    parser.add_argument('replay', type=Path)
    parser.add_argument('--backend', default='Null', choices=['Null', 'D3D', 'D3D12', 'Vulkan'])
    parser.add_argument('--seconds', type=float, default=20)
    parser.add_argument('--no-audio', action='store_true', help='Mac Null smoke only')
    parser.add_argument('--save-frame', type=Path, help='Save the last accepted RGBA frame for inspection')
    args = parser.parse_args()
    assert args.seconds >= 5
    with tempfile.TemporaryDirectory(prefix='orca-broadcast-') as temp_name:
        temp = Path(temp_name)
        ring = temp / 'sink.bin'
        create_ring(ring)
        env = launch_env(temp, ring)
        env['ORCA_REPLAY_SOURCE'] = str(args.replay.resolve())
        command = [str(args.binary.resolve()), '-p', 'headless', '-v', args.backend,
                   '-u', str(temp / 'user'), '-e', str(args.disc.resolve())]
        if args.no_audio:
            command += ['-C', 'Dolphin.DSP.Backend=No Audio Output']
        lines = queue.Queue()
        with (temp / 'stderr.log').open('w') as log:
            child = subprocess.Popen(command, env=env, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=log, text=True, bufsize=1)
            def read_lines():
                for line in child.stdout:
                    lines.put(line.rstrip())
            threading.Thread(target=read_lines, daemon=True).start()
            latest = [0, 0]
            stamps = [[], []]
            tickets = [[], []]
            nonzero_audio = False
            last_video = None
            caps = started = done = False
            start = time.monotonic()
            media_start = None
            try:
                with ring.open('rb', buffering=0) as handle:
                    while time.monotonic() - start < 180:
                        while not lines.empty():
                            line = lines.get_nowait()
                            if line.startswith('orca caps '):
                                print(line, flush=True)
                                caps = 'broadcast' in line.split()[2:]
                                child.stdin.write('caps replay broadcast\n')
                                child.stdin.flush()
                            if line.startswith('orca replay start '):
                                started = True
                                media_start = time.monotonic()
                                print(line, flush=True)
                            if line.startswith('orca replay done '):
                                print(line, flush=True)
                                done = True
                                assert 'done ok ' in line, line
                            if line.startswith('orca error '):
                                raise AssertionError(line)
                        handle.seek(0)
                        header = handle.read(HEADER)
                        assert struct.unpack_from('<12I', header) == FIELDS
                        for kind in (0, 1):
                            new = struct.unpack_from('<I', header, 64 + kind * 4)[0]
                            assert new <= MAX_TICKET
                            if not new or new == latest[kind]:
                                continue
                            # App reads newest video and at most 100ms of audio, in order.
                            pending = min(distance(new, latest[kind]), 10) if kind else 1
                            first = ((new - pending) % MAX_TICKET) + 1
                            for i in range(pending):
                                ticket = ((first - 1 + i) % MAX_TICKET) + 1
                                result = packet(handle, bool(kind), ticket)
                                if result:
                                    timestamp, data = result
                                    if stamps[kind]:
                                        assert timestamp > stamps[kind][-1], 'Nonmonotonic timestamp'
                                    stamps[kind].append(timestamp)
                                    tickets[kind].append(ticket)
                                    if kind:
                                        nonzero_audio |= any(data)
                                    else:
                                        last_video = data
                            latest[kind] = new
                        if done or child.poll() is not None:
                            break
                        if media_start and time.monotonic() - media_start >= args.seconds:
                            break
                        time.sleep(0.005)
                print('ring latest video/audio:', latest, 'accepted packets:', list(map(len, stamps)),
                      'drops video/audio:', struct.unpack_from('<2I', header, 72),
                      'state:', struct.unpack_from('<I', header, 80)[0], flush=True)
                assert caps and started, 'Missing broadcast cap or replay start'
                if args.backend != 'Null':
                    assert not args.no_audio, 'Real GPU check requires the normal audio mixer'
                    for kind, target in ((0, 30), (1, 100)):
                        assert len(stamps[kind]) >= 2, 'No media packets'
                        duration = (stamps[kind][-1] - stamps[kind][0]) / 1_000_000
                        assert duration >= 5, 'Use a replay with at least five seconds of gameplay'
                        produced = distance(tickets[kind][-1], tickets[kind][0])
                        rate = produced / duration
                        print(('video fps' if kind == 0 else 'audio chunks/sec'), rate, flush=True)
                        assert target * 0.9 <= rate <= target * 1.1, 'Unexpected media cadence'
                        assert len(stamps[kind]) >= produced * 0.9, 'Too many rejected/missed packets'
                    assert nonzero_audio, 'Replay must contain audible game sound'
            finally:
                if child.poll() is None:
                    try:
                        child.stdin.write('quit\n')
                        child.stdin.flush()
                        child.wait(timeout=20)
                    except (BrokenPipeError, subprocess.TimeoutExpired):
                        child.kill()  # Only the child started above.
                        child.wait()
                child.stdout.close()
                child.stdin.close()
            assert child.returncode == 0, f'Engine exit {child.returncode}'
            if args.save_frame:
                assert last_video is not None, 'No RGBA frame to save'
                args.save_frame.write_bytes(last_video)
            with ring.open('rb') as handle:
                handle.seek(80)
                state = struct.unpack('<I', handle.read(4))[0]
            print('ring state after shutdown:', state, flush=True)
            if done:
                assert state == 2, 'Completed replay did not stop the sink'
        print('PASS', args.backend, 'broadcast ring')


if __name__ == '__main__':
    main()
