// Run by heavy-slot, one process per launch. Environment is an explicit nonsecret allowlist.
const fs=require('fs'), path=require('path'), cp=require('child_process');
const root=process.env.ORCA_BENCH_ROOT;
if(!root||!process.env.ORCA_BENCH_GATE||!process.env.ORCA_BENCH_YIELD)throw Error('runner environment missing');
if(fs.existsSync(path.join(root,'STOP'))){console.log('stopped between launches');process.exit(5);}
const [arm,n]=process.argv.slice(2);
if (!['BASE','NEW'].includes(arm)|| !/^[1-4]$/.test(n)) throw Error('arm/launch invalid');
const dir=path.join(root,arm), user=path.join(dir,'user');
if(n==='1') {
 const manifest=JSON.parse(fs.readFileSync(path.join(dir,'manifest.json'),'utf8'));
 const initial={dolphin_entries:fs.readdirSync(path.join(user,'Cache')),
  apple_entries:fs.existsSync(manifest.apple_cache)?fs.readdirSync(manifest.apple_cache):[]};
 fs.writeFileSync(path.join(dir,'cold-cache-before.json'),JSON.stringify(initial,null,2)+'\n');
 if(initial.dolphin_entries.length||initial.apple_entries.length)throw Error('cold test caches are not empty; refuse launch');
}
const gate=cp.spawnSync('/bin/bash',[process.env.ORCA_BENCH_GATE],{encoding:'utf8'});
fs.writeFileSync(path.join(dir,`gate-${n}.txt`),gate.stdout+gate.stderr);
if(gate.status!==0){console.log('gate denied');process.exit(3);}
const env={HOME:path.join(user,'home'),PATH:'/usr/bin:/bin',TMPDIR:path.join(user,'tmp'),
 ORCA_PROFILE:'RSBE01',ORCA_SESSION:'1',ORCA_NO_LOCAL_INPUT:'1',ORCA_TEST_COMMANDS:'1',
 ORCA_TEST_DEV_GAME:'1',ORCA_TEST_RNG_SEED:'0x03981031',
 ORCA_UX_PROBE:path.join(root,'probe.txt'),
 ORCA_TEST_PERF:'15,900',ORCA_PERF_AUTO:'0',YG_SCENES:'1',YOUGAME_STDIO:'1',
 SDL_JOYSTICK_HIDAPI:'0',SDL_JOYSTICK_HIDAPI_GAMECUBE:'0',SDL_HIDAPI_LIBUSB:'0',
 SDL_JOYSTICK_IOKIT:'0',SDL_JOYSTICK_MFI:'0',SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT:'0x0000/0x0000',
 YG_INPUT:path.join(root,'input.txt'),YG_EXIT_AFTER:'3000',YG_FRAMETIME:'0',YG_THROTTLE:'1'};
if(process.env.ORCA_BENCH_TRACE==='1')env.ORCA_SHADER_CACHE_TRACE='1';
const exe=path.join(dir,'AGENT TEST Orca.app/Contents/MacOS/Orca');
const args=['-p','macos','-v','Metal','-u',user,
 '-C','Dolphin.DSP.Backend=No Audio Output','-C','Dolphin.Core.GFXBackend=Metal',
 '-C','Dolphin.General.UseDiscordPresence=False','-C','Dolphin.Analytics.Enabled=False',
 '-C','Logger.Logs.ROLLBACK=True','-C','Logger.Logs.VIDEO=True',
 '-C','Logger.Options.Verbosity=4','-C','Logger.Options.WriteToConsole=True',
 '-e',path.join(root,'disc/AGENT-TEST-RSBE01-rev2.wbfs')];
fs.writeFileSync(path.join(dir,`config-${n}.json`),JSON.stringify({exe,args,env},null,2)+'\n');
const log=fs.openSync(path.join(dir,`console-${n}.log`),'w');
const start=Date.now(), timings={start_utc:new Date().toISOString(),arm,launch:+n,ready_ms:null,first_frame_ms:null,
 protocol_requests:{},protocol_unexpected:[],perf_windows:0,sequence_ok:false};
for(const name of ['match-init','rng-30','rng-600']){const f=path.join(user,`probe-${name}.bin`);if(fs.existsSync(f))fs.unlinkSync(f);}
const child=cp.spawn(exe,args,{env,cwd:dir,stdio:['pipe','pipe','pipe']});
child.stdin.on('error',()=>{});
const buffers={stdout:'',stderr:''}, sequence=['scTitle','muMenuMain','scSelctCharacter','scSelStage','scMelee'];let scene_index=0;
const consume=(stream,b)=>{fs.writeSync(log,b);buffers[stream]+=b.toString();let i;while((i=buffers[stream].indexOf('\n'))>=0){
 const line=buffers[stream].slice(0,i);buffers[stream]=buffers[stream].slice(i+1);
 if(/^ready(?:\s|$)/.test(line)&&timings.ready_ms===null)timings.ready_ms=Date.now()-start;
 if(/orca boot phase first-frame /.test(line)&&timings.first_frame_ms===null)timings.first_frame_ms=Date.now()-start;
 const scene=/Scene frame \d+: \S* -> (\S+)/.exec(line);
 if(scene&&scene[1]===sequence[scene_index]){++scene_index;timings.sequence_ok=scene_index===sequence.length;}
 if(line.startsWith('orca perf ')){try{if(JSON.parse(line.slice(10)).k==='pace')++timings.perf_windows;}catch{}}
 if(stream==='stdout'&&line.startsWith('yg ')){
  let msg;try{msg=JSON.parse(line.slice(3));}catch{continue;}
  if(!Number.isInteger(msg.id)||typeof msg.type!=='string')continue;
  timings.protocol_requests[msg.type]=(timings.protocol_requests[msg.type]||0)+1;
  let response;
  if(msg.type==='hello'||msg.type==='mpRoom')response={re:msg.id,ok:true,result:{}};
  else if(msg.type==='mpTicket')response={re:msg.id,ok:false,error:'AGENT TEST offline benchmark: no room'};
  else {timings.protocol_unexpected.push(msg.type);response={re:msg.id,ok:false,error:'AGENT TEST: unsupported request'};}
  try{child.stdin.write('yg '+JSON.stringify(response)+'\n');}catch{}
 }
}};
child.stdout.on('data',b=>consume('stdout',b));child.stderr.on('data',b=>consume('stderr',b));
let timedout=false,playing=false;
const timer=setTimeout(()=>{timedout=true;child.kill('SIGTERM');setTimeout(()=>child.kill('SIGKILL'),5000).unref();},600000);
const check=setInterval(()=>{
 const status=cp.spawnSync('/bin/bash',[process.env.ORCA_BENCH_YIELD,'status'],{encoding:'utf8'});
 if(!/^Luis is not playing$/m.test(status.stdout)){playing=true;child.kill('SIGTERM');}
},5000);
child.on('error',e=>{timings.error=e.code});
child.on('close',(code,signal)=>{clearTimeout(timer);clearInterval(check);fs.closeSync(log);
 Object.assign(timings,{end_utc:new Date().toISOString(),wall_ms:Date.now()-start,code,signal,timedout,playing});
 const fixture={seed_expected:'03981031',seed_observed:[],snapshots:{}};
 const content=fs.readFileSync(path.join(dir,`console-${n}.log`),'utf8');
 fixture.seed_observed=[...content.matchAll(/history's seed ([0-9a-fA-F]+)/g)].map(m=>m[1]);
 const dumpDir=path.join(dir,`fixture-${n}`);fs.mkdirSync(dumpDir,{recursive:true});
 for(const name of ['match-init','rng-30','rng-600']) {
  const f=path.join(user,`probe-${name}.bin`);
  if(fs.existsSync(f)){const b=fs.readFileSync(f);fs.copyFileSync(f,path.join(dumpDir,path.basename(f)));
   fixture.snapshots[name]={bytes:b.length,sha256:require('crypto').createHash('sha256').update(b).digest('hex')};
   if(name==='match-init'&&b.length>=0x208)fixture.match={stage:b[0x1b],ports:[0,1,2,3].map(i=>({character:b[0x98+0x5c*i],type:b[0x99+0x5c*i]}))};
  }
 }
 fixture.ok=fixture.seed_observed.length>0&&fixture.seed_observed.every(s=>s===fixture.seed_expected)&&Object.keys(fixture.snapshots).length===3;
 const reference=path.join(root,'BASE/fixture-1.json');
 if(fs.existsSync(reference)) {
  const ref=JSON.parse(fs.readFileSync(reference,'utf8'));
  fixture.matches_base=JSON.stringify(fixture.match)===JSON.stringify(ref.match)&&['rng-30','rng-600'].every(k=>fixture.snapshots[k]?.sha256===ref.snapshots[k]?.sha256);
  fixture.ok=fixture.ok&&fixture.matches_base;
 }
 timings.fixture=fixture;
 fs.writeFileSync(path.join(dir,`fixture-${n}.json`),JSON.stringify(fixture,null,2)+'\n');
 const files=[];function walk(p,base){if(!fs.existsSync(p))return;for(const e of fs.readdirSync(p,{withFileTypes:true})){const f=path.join(p,e.name);if(e.isDirectory())walk(f,base);else if(e.isFile())files.push({path:path.relative(base,f),bytes:fs.statSync(f).size});}}
 walk(path.join(user,'Cache'),user);fs.writeFileSync(path.join(dir,`cache-${n}.json`),JSON.stringify(files,null,2)+'\n');
 files.length=0;const manifest=JSON.parse(fs.readFileSync(path.join(dir,'manifest.json'),'utf8'));
 walk(manifest.apple_cache,manifest.apple_cache);walk(path.join(user,'home/Library/Caches'),path.join(user,'home'));
 fs.writeFileSync(path.join(dir,`apple-cache-${n}.json`),JSON.stringify(files,null,2)+'\n');
 fs.writeFileSync(path.join(dir,`timings-${n}.json`),JSON.stringify(timings,null,2)+'\n');console.log(JSON.stringify(timings));
 process.exit(code===0&&!timedout&&!playing&&fixture.ok&&timings.sequence_ok&&timings.perf_windows>0&&timings.protocol_unexpected.length===0?0:4);
});
