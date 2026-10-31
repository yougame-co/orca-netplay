#!/usr/bin/env python3
import json,pathlib,re,statistics,sys
root=pathlib.Path(sys.argv[1]).resolve()
notes=root
rows=[]
order=[('BASE',1),('NEW',1),('NEW',2),('BASE',2),('BASE',3),('NEW',3),('NEW',4),('BASE',4)]
for arm,n in order:
 d=root/arm;p=d/f'timings-{n}.json'
 if not p.exists():continue
 r=json.loads(p.read_text());log=(d/f'console-{n}.log').read_text(errors='replace');gate=(d/f'gate-{n}.txt').read_text()
 one=lambda pattern,cast: next((cast(v) for v in re.findall(pattern,log)),None)
 r.update(phase_first_frame_ms=one(r'orca boot phase first-frame (\d+)',int),shader_boot_wait_s=one(r'Orca: compiled .*?before the first frame in ([\d.]+) s',float),frame_p99_ms=one(r'Diag frame times .*?p99 ([\d.]+)',float),frame_max_ms=one(r'Diag frame times .*?max ([\d.]+)',float),origin_ms=one(r'Drop-in: origin at frame \d+: RAM [\da-f]+, NAND \d+ KB \(([\d.]+) ms\)',float))
 r['origin']=re.findall(r'Drop-in: origin at frame .*',log)
 r['scenes']=[dict(frame=int(f),scene=s) for f,s in re.findall(r'Scene frame (\d+): \S* -> (\S+)',log)]
 r['cache_acceptance']=[dict(accepted=int(a),rejected=int(b),records=int(c)) for a,b,c in re.findall(r'Metal shader cache: accepted=(\d+) rejected=(\d+) records=(\d+)',log)]
 perf=[]
 for line in log.splitlines():
  if line.startswith('orca perf '):
   try:perf.append(json.loads(line[10:]))
   except ValueError:pass
 pace=[p for p in perf if p.get('k')=='pace'];r.update(sc=sum(p.get('sc',0) for p in pace),sm_ms=sum(p.get('sm',0) for p in pace),pace_dropped=sum(p.get('d',0) for p in pace))
 r['shader_cache_files']=[f for f in json.loads((d/f'cache-{n}.json').read_text()) if f['path'].startswith('Cache/Shaders/')]
 r['shader_cache_bytes']=sum(f['bytes'] for f in r['shader_cache_files'])
 def val(pat,cast):
  m=re.search(pat,gate);return cast(m[1]) if m else None
 r['gates']=dict(idle_s=val(r'idle (\d+)s',int),load=val(r'load=([\d.]+)',float),free_pct=val(r'free_pct=([\d.]+)',float),locked_yes='locked=yes' in gate,not_playing='Luis is not playing' in gate,raw=gate)
 rows.append(r)
(notes/'results.json').write_text(json.dumps(rows,indent=2)+'\n')
lines=['# Follow-up benchmark','',f'Raw artifacts: `{root}`.','', '| Arm | Launch | Spawn→first frame s | Engine→first frame s | Boot shader wait s | Frame p99 / max ms | Origin capture ms | sc / sm ms | Shader bytes |', '|---|---|---:|---:|---:|---:|---:|---:|---:|']
f=lambda v,scale=1:'N/A' if v is None else f'{v/scale:.3f}'
for r in rows:
 lines.append(f"| {r['arm']} | {'cold' if r['launch']==1 else 'warm '+str(r['launch']-1)} | {f(r['first_frame_ms'],1000)} | {f(r['phase_first_frame_ms'],1000)} | {f(r['shader_boot_wait_s'])} | {f(r['frame_p99_ms'])} / {f(r['frame_max_ms'])} | {f(r['origin_ms'])} | {r['sc']} / {r['sm_ms']:.3f} | {r['shader_cache_bytes']} |")
lines += ['', 'All launches use the exact scene-driven bf-cpu-loop.txt script, seed 0x03981031, isolated empty initial Dolphin/Apple caches, and alternating order BASE, NEW, NEW, BASE, BASE, NEW, NEW, BASE. Read-only probes record stage, four character/type slots, and gameplay RNG at match offsets 30 and 600. Each later run must match the BASE cold stage/roster and both RNG snapshots. Literal ready is not emitted in this harness mode; first-frame is named separately. Whole-run frame summaries include boot, origin capture, menus and the match. Final binaries have debug tracing absent; diagnostic traces are separate.','']
for r in rows:
 lines += [f"## {r['arm']} launch {r['launch']}",'',f"Exit {r['code']}; sequence={r['sequence_ok']}; fixture={json.dumps(r.get('fixture'),sort_keys=True)}.",'', 'Gates: '+json.dumps({k:v for k,v in r['gates'].items() if k!='raw'},sort_keys=True)+'.','', 'Scenes: '+json.dumps(r['scenes'])+'.','', 'Origin: '+str(r['origin'])+'.','', 'Metal cache accepted: '+json.dumps(r['cache_acceptance'])+'.','']
if len(rows)==8:
 summary={}
 for arm in ['BASE','NEW']:
  warm=[r for r in rows if r['arm']==arm and r['launch']>1]
  summary[arm]={k:statistics.median(r[k] for r in warm) for k in ['first_frame_ms','phase_first_frame_ms','frame_p99_ms','frame_max_ms','origin_ms']}
 summary['all_fixture_valid']=all(r.get('fixture',{}).get('ok',False) for r in rows)
 (notes/'summary.json').write_text(json.dumps(summary,indent=2)+'\n')
(notes/'BENCHMARK.md').write_text('\n'.join(lines)+'\n')
print('\n'.join(lines[:14]))
