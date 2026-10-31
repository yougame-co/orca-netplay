#!/usr/bin/env python3
"""Prepare isolated matched users, then run priority-3 Metal cold/warm ABBA measurements."""
import argparse
import datetime
import hashlib
import json
import os
import pathlib
import plistlib
import shutil
import subprocess
import sys
import time

parser = argparse.ArgumentParser(description=__doc__)
for name in ('root', 'base-app', 'new-app', 'disc', 'heavy-slot', 'gate', 'yield-script'):
    parser.add_argument('--' + name, type=pathlib.Path, required=True)
parser.add_argument('--diagnostic', action='store_true', help='Opt in to tracing in an instrumented build')
args = parser.parse_args()
root = args.root.resolve()
if root.exists():
    parser.error('root must not exist; refusing to overwrite any earlier evidence')
for p in (args.base_app, args.new_app, args.disc, args.heavy_slot, args.gate, args.yield_script):
    if not p.exists():
        parser.error(f'missing input: {p}')
root.mkdir(parents=True)
script_dir = pathlib.Path(__file__).resolve().parent
source_input = script_dir.parent / 'inputs/bf-cpu-loop.txt'
shutil.copyfile(source_input, root / 'input.txt')
(root / 'input-manifest.json').write_text(json.dumps(dict(source=str(source_input), sha256=hashlib.sha256(source_input.read_bytes()).hexdigest(), seed='0x03981031'), indent=2) + '\n')
(root / 'probe.txt').write_text('''# Read-only game-state snapshots; no screen capture.
@scMelee:1 30 dump [[805a00e0]+8] 208 match-init
@scMelee:1 30 dump 805a00bc 4 rng-30
@scMelee:1 600 dump 805a00bc 4 rng-600
''')
(root / 'disc').mkdir()
disc = root / 'disc/AGENT-TEST-RSBE01-rev2.wbfs'
# macOS clonefile avoids a second full disc copy; only this owned clone is made read-only.
subprocess.run(['cp', '-c', str(args.disc.resolve()), str(disc)], check=True)
disc.chmod(0o444)
cache_base = pathlib.Path(subprocess.check_output(['getconf', 'DARWIN_USER_CACHE_DIR'], text=True).strip())
namespace = hashlib.sha256(str(root).encode()).hexdigest()[:12]
for arm, source in [('BASE', args.base_app), ('NEW', args.new_app)]:
    arm_dir = root / arm
    arm_dir.mkdir()
    app = arm_dir / 'AGENT TEST Orca.app'
    subprocess.run(['ditto', str(source.resolve()), str(app)], check=True)
    plist = app / 'Contents/Info.plist'
    data = plistlib.loads(plist.read_bytes())
    bundle = f'co.yougame.orca.shaderbench.{namespace}.{arm.lower()}'
    data.update(CFBundleIdentifier=bundle, CFBundleDisplayName='AGENT TEST Orca', CFBundleName='AGENT TEST Orca')
    plist.write_bytes(plistlib.dumps(data))
    profile = app / 'Contents/Resources/Sys/Orca/RSBE01.ini'
    profile.write_text(profile.read_text().replace('[Profile]\n', '[Profile]\nTitle = AGENT TEST Brawl shader cache\n', 1))
    user = arm_dir / 'user'
    for folder in ('home', 'tmp', 'Cache', 'Config'):
        (user / folder).mkdir(parents=True, exist_ok=True)
    (user / 'Config/Dolphin.ini').write_text('[Display]\nRenderWindowWidth = 1280\nRenderWindowHeight = 720\n[General]\nUseDiscordPresence = False\n[Analytics]\nEnabled = False\n')
    apple = cache_base / bundle
    if apple.exists():
        raise RuntimeError('Apple test namespace already exists; choose a new root')
    (arm_dir / 'manifest.json').write_text(json.dumps(dict(arm=arm, source=str(source.resolve()), app=str(app), binary_sha256=hashlib.sha256((app / 'Contents/MacOS/Orca').read_bytes()).hexdigest(), bundle_version=data.get('CFBundleVersion'), bundle_id=bundle, apple_cache=str(apple), dolphin_cache=str(user / 'Cache')), indent=2) + '\n')

started = time.time()
deadline = started + 10800
order = [('BASE', 1), ('NEW', 1), ('NEW', 2), ('BASE', 2), ('BASE', 3), ('NEW', 3), ('NEW', 4), ('BASE', 4)]
(root / 'run-plan.json').write_text(json.dumps(dict(start_utc=datetime.datetime.fromtimestamp(started, datetime.timezone.utc).isoformat(), deadline_utc=datetime.datetime.fromtimestamp(deadline, datetime.timezone.utc).isoformat(), order=order, diagnostic=args.diagnostic), indent=2) + '\n')
# Only benchmark control paths enter this environment; the engine gets launch.cjs's allowlist.
child_env = os.environ.copy()
child_env.update(ORCA_BENCH_ROOT=str(root), ORCA_BENCH_GATE=str(args.gate.resolve()), ORCA_BENCH_YIELD=str(args.yield_script.resolve()))
if args.diagnostic:
    child_env['ORCA_BENCH_TRACE'] = '1'
else:
    child_env.pop('ORCA_BENCH_TRACE', None)
with (root / 'queue.log').open('a', buffering=1) as log:
    for arm, n in order:
        while time.time() < deadline:
            command = ['bash', str(args.heavy_slot.resolve()), 'run', '--pool', 'gpu', '--game', '--prio', '3', '--mac-idle', '600', '--label', 'orca:metal-shaderbin', '--', 'node', str(script_dir / 'launch.cjs'), arm, str(n)]
            log.write(f'{datetime.datetime.now(datetime.timezone.utc).isoformat()} queue {arm} {n}\n')
            try:
                rc = subprocess.run(command, env=child_env, stdout=log, stderr=subprocess.STDOUT, timeout=max(1, deadline - time.time())).returncode
            except subprocess.TimeoutExpired:
                log.write('STOP: 3-hour deadline reached\n')
                sys.exit(3)
            if rc == 0:
                break
            if rc != 3:
                log.write(f'STOP: launch failed rc={rc}; evidence preserved\n')
                sys.exit(rc)
            time.sleep(min(30, max(0, deadline - time.time())))
        else:
            sys.exit(3)
subprocess.run([sys.executable, str(script_dir / 'summarize.py'), str(root)], check=True)
