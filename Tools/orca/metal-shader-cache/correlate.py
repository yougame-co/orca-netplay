#!/usr/bin/env python3
"""Correlate measured long CPU frame intervals with steady-clock diagnostic spans."""
import json,pathlib,re,sys
root=pathlib.Path(sys.argv[1]);rows=[]
pattern=re.compile(r'\[ShaderTrace\] event=(\S+) tid=(\d+) begin_ns=(\d+) end_ns=(\d+) detail=(\d+)')
for p in sorted(root.glob('*/console-*.log')):
 events=[]
 for m in pattern.finditer(p.read_text(errors='replace')):
  event,tid,begin,end,detail=m.groups();begin=int(begin);end=int(end)
  events.append(dict(event=event,tid=int(tid),begin_ns=begin,end_ns=end,detail=int(detail),ms=(end-begin)/1e6))
 frames=[]
 for frame in [e for e in events if e['event']=='long_frame']:
  overlap=[]
  for e in events:
   if e['event']=='long_frame':continue
   ns=min(frame['end_ns'],e['end_ns'])-max(frame['begin_ns'],e['begin_ns'])
   if ns>0:
    overlap.append(dict(**e,overlap_ms=ns/1e6,same_thread=e['tid']==frame['tid']))
  frames.append(dict(**frame,overlap=sorted(overlap,key=lambda e:e['overlap_ms'],reverse=True)))
 row=dict(log=str(p),events=len(events),long_frames=frames,event_max_ms={k:max(e['ms'] for e in events if e['event']==k) for k in sorted({e['event'] for e in events})})
 rows.append(row)
(root/'trace-correlation.json').write_text(json.dumps(rows,indent=2)+'\n')
lines=['# Diagnostic trace correlation','', 'Spans share std::chrono::steady_clock nanoseconds. Overlap is temporal evidence; different-thread overlap alone does not prove the CPU frame waited for that work. Nested spans must not be added.','']
for r in rows:
 lines += [f"## {pathlib.Path(r['log']).parent.name}/{pathlib.Path(r['log']).name}",'',f"{r['events']} spans; {len(r['long_frames'])} intervals >100 ms.",'', '| Ending frame | Interval ms | Thread | Overlapping work (duration inside interval ms; thread) |', '|---:|---:|---:|---|']
 for f in r['long_frames']:
  spans='; '.join(f"{e['event']} {e['overlap_ms']:.3f} ({e['tid']}{', CPU' if e['same_thread'] else ''})" for e in f['overlap'] if e['overlap_ms']>=0.05)
  lines += [f"| {f['detail']} | {f['ms']:.3f} | {f['tid']} | {spans or 'no traced work'} |"]
 lines += ['', 'Maximum spans ms: '+', '.join(f'{k}={v:.3f}' for k,v in r['event_max_ms'].items())+'.','']
(root/'TRACE-CORRELATION.md').write_text('\n'.join(lines))
print('\n'.join(lines))
