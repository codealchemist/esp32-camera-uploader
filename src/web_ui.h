// Single-page dashboard served at "/". Lives in flash; the browser does the
// work (polls /status, POSTs /snap and /restart), so the ESP32 only streams
// this string and answers small JSON requests.
#pragma once

static const char INDEX_HTML[] PROGMEM = R"HTML(<!doctype html>
<html lang="en"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>yicam-bridge</title>
<meta name="color-scheme" content="dark">
<style>
:root{--bg:#0e1014;--card:#161a21;--line:#252b36;--tx:#e7e9ee;--mu:#8a93a6;
--ac:#5b9dff;--ok:#3ecf8e;--wa:#f5a524;--bad:#f0506e;--r:14px}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--tx);
font:15px/1.45 -apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,sans-serif;
-webkit-font-smoothing:antialiased}
main{max-width:880px;margin:0 auto;padding:20px 16px 40px}
header{display:flex;align-items:center;justify-content:space-between;gap:12px;margin-bottom:18px}
h1{font-size:20px;margin:0;letter-spacing:-.01em}
h1 small{color:var(--mu);font-weight:500;font-size:13px;margin-left:6px}
h2{font-size:12px;text-transform:uppercase;letter-spacing:.08em;color:var(--mu);margin:0 0 12px;font-weight:600}
.pill{display:inline-flex;align-items:center;gap:7px;padding:5px 11px;border-radius:99px;
background:var(--card);border:1px solid var(--line);font-size:13px;color:var(--mu);white-space:nowrap}
.dot{width:8px;height:8px;border-radius:50%;background:var(--mu);flex:none}
.live .dot{background:var(--ok);box-shadow:0 0 0 3px #3ecf8e33}
.down .dot{background:var(--bad)}
.grid{display:grid;gap:14px;grid-template-columns:1fr 1fr}
.card{background:var(--card);border:1px solid var(--line);border-radius:var(--r);padding:16px}
.wide{grid-column:1/-1}
.hero{display:flex;align-items:center;justify-content:space-between;gap:16px;flex-wrap:wrap}
.state{display:flex;align-items:center;gap:14px}
.big{width:44px;height:44px;border-radius:50%;display:grid;place-items:center;background:#ffffff0d;flex:none}
.big i{width:14px;height:14px;border-radius:50%;background:var(--mu)}
.motion .big{background:#f5a52422}.motion .big i{background:var(--wa);animation:p 1.2s infinite}
@keyframes p{0%{box-shadow:0 0 0 0 #f5a52499}100%{box-shadow:0 0 0 14px #f5a52400}}
.state b{font-size:18px;display:block}.state span{color:var(--mu);font-size:13px}
.btns{display:flex;gap:8px;flex-wrap:wrap}
button{font:inherit;font-weight:600;font-size:14px;border-radius:10px;padding:9px 14px;cursor:pointer;
border:1px solid var(--line);background:#ffffff08;color:var(--tx);transition:.15s}
button:hover{background:#ffffff14}button:disabled{opacity:.5;cursor:default}
.pri{background:var(--ac);border-color:var(--ac);color:#06101f}.pri:hover{background:#7bb0ff}
.dan{color:var(--bad);border-color:#f0506e55}.dan:hover{background:#f0506e1a}
.stats{display:grid;grid-template-columns:repeat(4,1fr);gap:10px}
.stat{background:#ffffff06;border-radius:10px;padding:10px 12px}
.stat b{display:block;font-size:24px;font-variant-numeric:tabular-nums;letter-spacing:-.02em}
.stat span{color:var(--mu);font-size:12px}
.stat.bad b{color:var(--bad)}
.sub{display:flex;gap:16px;flex-wrap:wrap;margin-top:12px;color:var(--mu);font-size:13px}
.sub em{font-style:normal;color:var(--tx);font-variant-numeric:tabular-nums}
.sub .bad em{color:var(--bad)}
dl{margin:0;display:grid;grid-template-columns:auto 1fr;gap:9px 14px;font-size:14px}
dt{color:var(--mu);white-space:nowrap}
dd{margin:0;text-align:right;font-variant-numeric:tabular-nums;min-width:0;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
dd.full{white-space:normal}
a{color:var(--ac);text-decoration:none}a:hover{text-decoration:underline}
.bar{height:6px;background:#ffffff12;border-radius:9px;margin-top:6px;overflow:hidden}
.bar i{display:block;height:100%;background:var(--ac);border-radius:9px;width:0;transition:width .4s}
.sig{display:inline-flex;gap:2px;align-items:flex-end;height:12px;margin-right:6px;vertical-align:-1px}
.sig i{width:3px;background:#ffffff26;border-radius:1px}
.sig i.on{background:var(--ok)}
.err{display:none;margin-top:14px;padding:10px 12px;border-radius:10px;background:#f0506e14;
border:1px solid #f0506e44;color:#ffb3c1;font:13px/1.4 ui-monospace,Menlo,monospace;overflow-wrap:anywhere}
.preview{margin-top:14px;display:none}
.frame{position:relative;border-radius:10px;overflow:hidden;background:#000;aspect-ratio:16/9}
.frame img{width:100%;height:100%;object-fit:contain;display:block}
.frame img:not([src]){visibility:hidden}
.pmsg a{color:#ffd0da;text-decoration:underline}
.tl{position:absolute;left:0;right:0;bottom:0;height:2px;background:#ffffff1f}
.tl i{display:block;height:100%;width:0;background:var(--ac)}
.tl.loading i{width:100%;animation:ld 1s ease-in-out infinite alternate}
@keyframes ld{from{opacity:.25}to{opacity:.9}}
.pmsg{position:absolute;inset:auto 10px 12px;padding:6px 10px;border-radius:8px;background:#000b;
color:#ffb3c1;font-size:13px;display:none}
.pctl{display:flex;align-items:center;justify-content:space-between;gap:10px;margin-top:10px;flex-wrap:wrap}
.chips{display:flex;gap:4px;flex-wrap:wrap}
.chips button{padding:4px 9px;font-size:12px;border-radius:99px;font-weight:600;color:var(--mu)}
.chips button.on{background:var(--ac);border-color:var(--ac);color:#06101f}
.pctl span{color:var(--mu);font-size:12px;font-variant-numeric:tabular-nums}
footer{margin-top:18px;color:var(--mu);font-size:12px;text-align:center}
#toast{position:fixed;left:50%;bottom:20px;transform:translate(-50%,20px);opacity:0;transition:.25s;
background:#232935;border:1px solid var(--line);padding:10px 16px;border-radius:10px;font-size:14px;
box-shadow:0 8px 30px #0008;pointer-events:none;max-width:calc(100% - 32px)}
#toast.show{opacity:1;transform:translate(-50%,0)}
#toast.ok{border-color:#3ecf8e66}#toast.bad{border-color:#f0506e88}
.pmsg.info{color:var(--tx)}
.hd{display:flex;align-items:center;gap:8px}
.ghost{padding:5px 11px;font-size:13px;font-weight:500;border-radius:99px;color:var(--mu)}
.warn{display:none;margin-bottom:14px;padding:12px 14px;border-radius:var(--r);background:#f5a52414;
border:1px solid #f5a52455;color:#ffd99a;font-size:14px}
@media(max-width:640px){.grid{grid-template-columns:1fr}}
@media(max-width:560px){.stats{grid-template-columns:repeat(2,1fr)}.hero .btns{width:100%}.hero button{flex:1;padding:9px 6px;font-size:13px;white-space:nowrap}}
</style></head><body><main>
<header><h1>yicam-bridge<small id="ver"></small></h1>
<div class="hd"><span class="pill" id="conn"><span class="dot"></span><span id="connT">connecting…</span></span>
<a class="ghost" id="setL" href="/settings" style="display:none;border:1px solid var(--line);text-decoration:none">Settings</a>
<form method="post" action="/logout" id="lo" style="display:none;margin:0"><button class="ghost">Sign out</button></form></div></header>
<div class="warn" id="noauth"><b>No dashboard password yet.</b> Anyone on your Wi-Fi can use this page,
and the camera preview stays off until you set one. <a href="/settings" style="color:#ffd99a;text-decoration:underline">Set a password now</a>.</div>

<div class="grid">
<section class="card wide">
 <div class="hero">
  <div class="state" id="state"><div class="big"><i></i></div>
   <div><b id="stateT">—</b><span id="stateS"></span></div></div>
  <div class="btns">
   <button class="pri" id="bSnap">Take snapshot</button>
   <button id="bPrev">Camera preview</button>
   <button class="dan" id="bRst">Restart</button>
  </div>
 </div>
 <div class="preview" id="prev">
  <div class="frame" id="frame"><img id="prevImg" alt="Camera preview">
   <div class="pmsg" id="pmsg"></div><div class="tl" id="tl"><i></i></div></div>
  <div class="pctl"><div class="chips" id="chips"></div>
   <span id="pAge" title="Low-resolution frames fetched by the bridge with its camera login. Not uploaded."></span></div>
 </div>
 <div class="err" id="err"></div>
</section>

<section class="card wide"><h2>Pipeline</h2>
 <div class="stats">
  <div class="stat"><b id="sEv">–</b><span>Motion events</span></div>
  <div class="stat"><b id="sCap">–</b><span>Captured</span></div>
  <div class="stat"><b id="sUp">–</b><span>Uploaded to Drive</span></div>
  <div class="stat"><b id="sQ">–</b><span>Waiting in queue</span></div>
 </div>
 <div class="sub">
  <span id="fCap">Capture failed <em>–</em></span>
  <span id="fUp">Upload failed <em>–</em></span>
  <span id="fDr">Dropped (queue full) <em>–</em></span>
  <span>Last upload <em id="lastUp">–</em></span>
 </div>
 <div class="sub" id="vidRow"><span>Video clips</span>
  <span>uploaded <em id="vUp">–</em></span><span>waiting <em id="vQ">–</em></span>
  <span id="vF">failed <em>–</em></span><span>today <em id="vT">–</em></span><span id="vP"></span>
 </div>
</section>

<section class="card"><h2>Device</h2><dl>
 <dt>IP address</dt><dd id="dIp">–</dd>
 <dt>Wi-Fi signal</dt><dd id="dRssi">–</dd>
 <dt>Uptime</dt><dd id="dUp">–</dd>
 <dt>Clock</dt><dd id="dClk">–</dd>
 <dt>Free memory</dt><dd id="dHeap">–</dd>
 <dt>Photos on camera</dt><dd class="full"><span id="dFs">–</span><div class="bar"><i id="dFsB"></i></div></dd>
</dl></section>

<section class="card"><h2>Setup</h2><dl>
 <dt>Camera</dt><dd id="cCam">–</dd>
 <dt>Camera login</dt><dd id="cLog">–</dd>
 <dt>Camera connected</dt><dd id="cMq">–</dd>
 <dt>Motion topic</dt><dd id="cTop">–</dd>
 <dt>MQTT login</dt><dd id="cAuth">–</dd>
 <dt>Shots per event</dt><dd id="cShots">–</dd>
 <dt>Resolution</dt><dd id="cRes">–</dd>
 <dt>Drive upload</dt><dd id="cDrv">–</dd>
 <dt>Remote control</dt><dd id="cRem">–</dd>
</dl></section>
</div>

<footer>Recovery: hold <b>BOOT</b> while pressing <b>EN</b> to open the setup portal ·
<a href="/status" target="_blank">raw JSON</a></footer>
</main><div id="toast"></div>
<script>
const $=id=>document.getElementById(id);
const set=(id,v)=>{$(id).textContent=v};
let S=null,watch=null,timer=null,restarting=false;

function ago(t){if(!t)return'never';const s=Math.max(0,Date.now()/1e3-t|0);
 if(s<60)return s+'s ago';if(s<3600)return(s/60|0)+' min ago';
 if(s<86400)return(s/3600|0)+' h ago';return(s/86400|0)+' d ago'}
function dur(s){const d=s/86400|0,h=s/3600%24|0,m=s/60%60|0;
 return d?d+'d '+h+'h':h?h+'h '+m+'m':m?m+'m '+(s%60)+'s':s+'s'}
function kb(b){return b>=1048576?(b/1048576).toFixed(1)+' MB':Math.round(b/1024)+' KB'}
function toast(msg,cls){const t=$('toast');t.textContent=msg;t.className='show '+(cls||'');
 clearTimeout(t._h);t._h=setTimeout(()=>t.className=cls||'',3500)}
function bad(id,n){$(id).classList.toggle('bad',n>0)}

function render(s){
 set('ver','v'+s.version);
 $('noauth').style.display=s.auth?'none':'block';
 $('lo').style.display=s.auth?'block':'none';
 $('setL').style.display='inline-block';
 if(s.wifi_result&&!render.wr){render.wr=1;const[k,n]=[s.wifi_result.split(':')[0],s.wifi_result.slice(s.wifi_result.indexOf(':')+1)];
  if(k==='joined')toast('Now on Wi-Fi “'+n+'”','ok');
  else if(k==='reverted')toast('Couldn’t join “'+n+'”, back on “'+s.wifi_ssid+'”','bad')}
 set('cLog',s.camera_login?'Stored on bridge':'Not set');
 const m=s.motion_active;$('state').classList.toggle('motion',m);
 $('state').classList.toggle('motion',m||s.remote_active);
 set('stateT',m?'Motion detected':s.remote_active?'Capturing (remote request)':'Watching');
 set('stateS',s.remote_active&&!m?'Taking a shot every '+s.interval_s+' s':m?'Taking a shot every '+s.interval_s+' s (max '+s.max_shots+')'
  :s.mqtt_clients>0?'Camera connected, waiting for motion':'Waiting for the camera to connect');
 set('sEv',s.events);set('sCap',s.captured);set('sUp',s.uploaded);set('sQ',s.queued);
 $('fCap').lastElementChild.textContent=s.capture_failed;bad('fCap',s.capture_failed);
 $('fUp').lastElementChild.textContent=s.upload_failed;bad('fUp',s.upload_failed);
 $('fDr').lastElementChild.textContent=s.dropped;bad('fDr',s.dropped);
 set('lastUp',s.last_upload?s.last_upload+' · '+ago(s.last_upload_t):'none yet');
 const e=$('err');e.style.display=s.last_error?'block':'none';e.textContent='Last error: '+s.last_error;

 set('dIp',s.ip);
 const r=s.rssi,q=r>=-55?4:r>=-67?3:r>=-75?2:r>=-85?1:0,d=$('dRssi');d.textContent='';
 const g=document.createElement('span');g.className='sig';
 for(let i=0;i<4;i++){const b=document.createElement('i');b.style.height=(4+i*2.7)+'px';
  if(i<q)b.className='on';g.appendChild(b)}
 d.append(g,['Weak','Poor','Fair','Good','Excellent'][q]+' · '+r+' dBm');
 set('dUp',dur(s.uptime_s));
 set('dClk',s.time_synced?s.local_time.slice(11):'not synced yet');
 $('dClk').title=s.local_time;
 set('dHeap',kb(s.heap_free)+' free');
 $('dHeap').title='Largest free block: '+kb(s.heap_max_block);
 set('dFs',s.queued+' of '+s.slots+' waiting for upload');$('dFs').title='Photos are saved on the camera\'s SD card until they are in Drive.';
 $('dFsB').style.width=(s.slots?100*s.queued/s.slots:0)+'%';
 $('vidRow').style.display=s.video_on?'flex':'none';
 set('vUp',s.videos_uploaded);set('vQ',s.videos_queued);
 $('vF').lastElementChild.textContent=s.videos_failed;bad('vF',s.videos_failed);
 set('vT',s.video_today+' of '+s.video_cap);
 set('vP',s.video_progress>=0?'uploading… '+s.video_progress+'%':'');

 const c=$('cCam');c.textContent='';const a=document.createElement('a');
 a.href='http://'+s.camera+'/';a.target='_blank';a.textContent=s.camera;c.appendChild(a);
 set('cMq',s.mqtt_clients>0?'Yes'+(s.mqtt_clients>1?' ('+s.mqtt_clients+' clients)':''):'No');
 $('cMq').style.color=s.mqtt_clients>0?'var(--ok)':'var(--wa)';
 set('cTop',s.topic);$('cTop').title=s.topic;
 set('cAuth',s.mqtt_auth?'Required':'Open (no password)');
 $('cAuth').style.color=s.mqtt_auth?'':'var(--wa)';
 set('cShots',s.max_shots+', every '+s.interval_s+' s');
 set('cRes',s.res);
 set('cDrv',s.script_configured?'Configured':'Not configured');
 $('cDrv').style.color=s.script_configured?'var(--ok)':'var(--bad)';
 set('cRem',!s.remote_on?'Off':s.last_poll_t?'Checked in '+ago(s.last_poll_t):'Not checked in yet');
 $('cRem').style.color=!s.remote_on?'':s.last_poll_t&&Date.now()/1e3-s.last_poll_t<3*s.poll_s?'var(--ok)':'var(--wa)';
}

function online(ok,txt){const c=$('conn');c.className='pill '+(ok?'live':'down');set('connT',txt)}

async function poll(){
 clearTimeout(timer);
 try{
  const ctl=new AbortController(),to=setTimeout(()=>ctl.abort(),5000);
  const r=await fetch('/status',{cache:'no-store',signal:ctl.signal});clearTimeout(to);
  if(r.status===401){location.reload();return}
  const s=await r.json();
  if(restarting&&(!S||s.uptime_s<S.uptime_s+5)){restarting=false;toast('Bridge is back online','ok')}
  if(watch){
   if(s.capture_failed>watch.capture_failed){toast('Snapshot failed: '+(s.last_error||'unknown error'),'bad');watch=null}
   else if(s.dropped>watch.dropped){toast('Snapshot skipped: queue is full','bad');watch=null}
   else if(s.uploaded>watch.uploaded){toast('Snapshot uploaded to Drive','ok');watch=null}
   else if(s.upload_failed>watch.upload_failed){toast('Upload failed, will retry: '+(s.last_error||''),'bad');watch=null}
   else if(s.captured>watch.captured&&!watch.capSeen){watch.capSeen=1;toast('Captured, uploading…')}
   else if(Date.now()>watch.until)watch=null;
  }
  S=s;render(s);online(true,'live');
 }catch(e){online(false,restarting?'restarting…':'offline')}
 if(!document.hidden)timer=setTimeout(poll,watch||restarting?1000:3000);
}

$('bSnap').onclick=async()=>{
 const b=$('bSnap');b.disabled=true;
 try{const r=await fetch('/snap',{method:'POST'});if(r.status===401)return location.reload();if(!r.ok)throw 0;
  watch=Object.assign({},S||{},{until:Date.now()+45000});toast('Snapshot requested…');poll()}
 catch(e){toast('Could not reach the bridge','bad')}
 setTimeout(()=>b.disabled=false,2000)};

$('bRst').onclick=async()=>{
 if(!confirm('Restart the bridge? Queued snapshots are kept.'))return;
 try{const r=await fetch('/restart',{method:'POST'});if(r.status===401)return location.reload()}catch(e){}
 restarting=true;online(false,'restarting…');toast('Restarting…');setTimeout(poll,4000)};

// --- Camera preview, relayed by the bridge (/preview) with its camera login.
// One request at a time; the countdown starts only after the previous frame
// has arrived, so neither the bridge nor the camera can be flooded.
const IV=[3,5,10,15,30,60];
const pv={on:false,iv:5,timer:null,tick:null,gen:0,last:0,fails:0};
try{const v=+localStorage.getItem('pvIv');if(IV.includes(v))pv.iv=v}catch(e){}

function bar(sec){const t=$('tl'),i=t.firstElementChild;t.classList.toggle('loading',sec<0);
 i.style.transition='none';i.style.width='0';
 if(sec>0){void i.offsetWidth;i.style.transition='width '+sec+'s linear';i.style.width='100%'}}
function pvMsg(m,info){const e=$('pmsg');e.textContent=m||'';e.className='pmsg'+(info?' info':'');
 e.style.display=m?'block':'none'}
function pvAge(){if(!pv.last){set('pAge','Loading first frame…');return}
 const s=Math.round((Date.now()-pv.last)/1e3);set('pAge','Updated '+(s<1?'just now':ago(pv.last/1e3)))}
function pvSchedule(sec){clearTimeout(pv.timer);if(!pv.on||document.hidden){bar(0);return}
 bar(sec);pv.timer=setTimeout(pvLoad,sec*1000)}
function pvFail(m){pv.fails++;pvMsg(m);pvSchedule(Math.min(60,pv.iv*Math.pow(2,pv.fails)))}
function pvShow(url,g){const im=new Image();
 im.onload=()=>{if(g!==pv.gen||!pv.on){URL.revokeObjectURL(url);return}
  im.id='prevImg';im.alt='Camera preview';const old=$('prevImg');$('frame').replaceChild(im,old);
  if(old.src.startsWith('blob:'))URL.revokeObjectURL(old.src);
  pv.last=Date.now();pv.fails=0;pvMsg('');pvAge();pvSchedule(pv.iv)};
 im.onerror=()=>{URL.revokeObjectURL(url);if(g===pv.gen&&pv.on)pvFail('The camera sent an unreadable image, retrying…')};
 im.src=url}
async function pvLoad(){
 if(!pv.on||!S||document.hidden)return;
 const g=++pv.gen,ctl=new AbortController(),to=setTimeout(()=>ctl.abort(),12000);bar(-1);
 try{
  const r=await fetch('/preview',{cache:'no-store',signal:ctl.signal});clearTimeout(to);
  if(g!==pv.gen||!pv.on)return;
  if(r.status===401)return location.reload();
  if(r.ok)return pvShow(URL.createObjectURL(await r.blob()),g);
  let k='';try{k=(await r.json()).error}catch(e){}
  if(g!==pv.gen||!pv.on)return;
  if(k==='too_fast')return pvSchedule(2);
  if(k==='busy'){pvMsg('Paused while the bridge captures motion…',1);return pvSchedule(pv.iv)}
  if(k==='no_password'){pvMsg('Set a dashboard password in setup to turn on the preview.');return pvSchedule(60)}
  if(k==='camera_login')return pvFail("The camera rejected the bridge's login. Check the camera user and password in setup.");
  pvFail('Camera not responding, retrying…');
 }catch(e){clearTimeout(to);if(g===pv.gen&&pv.on)pvFail('Bridge or camera not responding, retrying…')}}
function pvChips(){const c=$('chips');c.textContent='';IV.forEach(v=>{
 const b=document.createElement('button');b.textContent=v<60?v+'s':'1m';
 b.className=v===pv.iv?'on':'';b.title='Refresh every '+(v<60?v+' s':'minute');
 b.onclick=()=>{pv.iv=v;try{localStorage.setItem('pvIv',v)}catch(e){}pvChips();
  if(pv.on&&!$('tl').classList.contains('loading'))pvSchedule(v)};
 c.appendChild(b)})}
function pvOpen(on){pv.on=on;pv.gen++;clearTimeout(pv.timer);clearInterval(pv.tick);
 $('prev').style.display=on?'block':'none';set('bPrev',on?'Hide preview':'Camera preview');
 if(on){pv.last=0;pv.fails=0;pvMsg('');pvAge();pv.tick=setInterval(pvAge,1000);pvLoad()}
 else{bar(0);const i=$('prevImg');if(i.src.startsWith('blob:'))URL.revokeObjectURL(i.src);i.removeAttribute('src')}}
pvChips();
$('bPrev').onclick=()=>{if(S)pvOpen(!pv.on)};

document.addEventListener('visibilitychange',()=>{
 if(document.hidden){clearTimeout(pv.timer);pv.gen++;bar(0)}
 else{poll();if(pv.on)pvLoad()}});
poll();
</script></body></html>)HTML";

// Sign-in form shown at "/" when a dashboard password is set and the browser
// has no valid session. A real form POST (not fetch) so password managers
// offer to save the password and fill it next time.
static const char LOGIN_HTML[] PROGMEM = R"HTML(<!doctype html>
<html lang="en"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Sign in · yicam-bridge</title>
<meta name="color-scheme" content="dark">
<style>
:root{--bg:#0e1014;--card:#161a21;--line:#252b36;--tx:#e7e9ee;--mu:#8a93a6;--ac:#5b9dff;--bad:#f0506e}
*{box-sizing:border-box}
body{margin:0;min-height:100vh;display:grid;place-items:center;background:var(--bg);color:var(--tx);
font:15px/1.45 -apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,sans-serif;padding:16px}
form{width:100%;max-width:360px;background:var(--card);border:1px solid var(--line);border-radius:16px;padding:24px}
h1{font-size:20px;margin:0 0 4px}p{color:var(--mu);margin:0 0 20px;font-size:14px}
label{display:block;font-size:13px;color:var(--mu);margin-bottom:6px}
input[type=password]{width:100%;padding:11px 12px;border-radius:10px;border:1px solid var(--line);
background:#0b0d11;color:var(--tx);font:inherit;outline:none}
input[type=password]:focus{border-color:var(--ac)}
.chk{display:flex;gap:8px;align-items:center;margin:14px 0 18px;color:var(--tx);font-size:14px}
.chk input{accent-color:var(--ac);width:16px;height:16px;margin:0}
button{width:100%;padding:11px;border-radius:10px;border:0;background:var(--ac);color:#06101f;
font:inherit;font-weight:600;cursor:pointer}
button:hover{background:#7bb0ff}
.err{display:none;margin:-6px 0 14px;padding:9px 11px;border-radius:9px;background:#f0506e14;
border:1px solid #f0506e44;color:#ffb3c1;font-size:13px}
.u{position:absolute;width:1px;height:1px;opacity:0;pointer-events:none}
</style></head><body>
<form method="post" action="/login">
<h1>yicam-bridge</h1><p>Sign in to see the camera bridge.</p>
<input class="u" type="text" name="username" value="yicam-bridge" autocomplete="username" tabindex="-1" aria-hidden="true">
<label for="pw">Password</label>
<input id="pw" type="password" name="password" autocomplete="current-password" required autofocus>
<label class="chk"><input type="checkbox" name="remember" checked>Keep me signed in on this device for 30 days</label>
<div class="err" id="err"></div>
<button>Sign in</button>
</form>
<script>
const e=new URLSearchParams(location.search).get('e'),m={bad:'Wrong password.',
locked:'Too many wrong attempts. Wait a minute and try again.',
clock:'The bridge is still starting up. Try again in a few seconds.'};
if(m[e]){const d=document.getElementById('err');d.textContent=m[e];d.style.display='block';history.replaceState(null,'','/')}
</script></body></html>)HTML";

// Settings page at /settings (signed-in only). Fields are built from a small
// spec to keep the page compact. Secrets are never sent to the browser;
// leaving one blank keeps the saved value. Saving needs the current dashboard
// password and restarts the bridge.
static const char SETTINGS_HTML[] PROGMEM = R"HTML(<!doctype html>
<html lang="en"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Settings · yicam-bridge</title>
<meta name="color-scheme" content="dark">
<style>
:root{--bg:#0e1014;--card:#161a21;--line:#252b36;--tx:#e7e9ee;--mu:#8a93a6;--ac:#5b9dff;--ok:#3ecf8e;--wa:#f5a524;--bad:#f0506e;--r:14px}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--tx);font:15px/1.45 -apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,sans-serif;-webkit-font-smoothing:antialiased}
main{max-width:720px;margin:0 auto;padding:20px 16px 120px}
header{display:flex;align-items:center;gap:14px;margin-bottom:18px}
h1{font-size:20px;margin:0}
a{color:var(--ac);text-decoration:none}a:hover{text-decoration:underline}
.back{color:var(--mu);font-size:14px}
.card{background:var(--card);border:1px solid var(--line);border-radius:var(--r);padding:16px;margin-bottom:14px}
h2{font-size:12px;text-transform:uppercase;letter-spacing:.08em;color:var(--mu);margin:0 0 14px;font-weight:600}
.f{margin-bottom:14px}.f:last-child{margin-bottom:0}
label{display:block;font-size:13px;color:var(--mu);margin-bottom:6px}
input,select{width:100%;padding:10px 12px;border-radius:10px;border:1px solid var(--line);background:#0b0d11;color:var(--tx);font:inherit;outline:none}
input:focus,select:focus{border-color:var(--ac)}
input.bad,select.bad{border-color:var(--bad)}
.hint{color:var(--mu);font-size:12px;margin:6px 0 0}
.e{color:#ff9fb2;font-size:12px;margin-top:5px;display:none}
.inl{display:flex;gap:8px}.inl select{flex:1}
button{font:inherit;font-weight:600;font-size:14px;border-radius:10px;padding:10px 14px;cursor:pointer;border:1px solid var(--line);background:#ffffff08;color:var(--tx)}
button:hover{background:#ffffff14}button:disabled{opacity:.5;cursor:default}
.pri{background:var(--ac);border-color:var(--ac);color:#06101f}.pri:hover{background:#7bb0ff}
.link{background:none;border:0;color:var(--mu);padding:8px 0;font-weight:500}
.cur{margin-bottom:12px}
.warn{display:none;margin-bottom:14px;padding:12px 14px;border-radius:var(--r);background:#f5a52414;border:1px solid #f5a52455;color:#ffd99a;font-size:14px}
.save{position:fixed;left:0;right:0;bottom:0;background:#0e1014ee;backdrop-filter:blur(8px);border-top:1px solid var(--line)}
.save>div{max-width:720px;margin:0 auto;padding:12px 16px;display:flex;gap:10px;align-items:flex-start}
.save .f{flex:1;margin:0}.save label{display:none}.save button{white-space:nowrap}
.u{position:absolute;width:1px;height:1px;opacity:0;pointer-events:none}
.ov{position:fixed;inset:0;background:#0e1014f2;display:none;place-items:center;padding:16px;z-index:9}
.ov>div{max-width:420px;text-align:center}.ov b{font-size:18px;display:block;margin:14px 0 6px}.ov p{color:var(--mu);margin:0}
.spin{width:34px;height:34px;margin:0 auto;border-radius:50%;border:3px solid #ffffff22;border-top-color:var(--ac);animation:s 1s linear infinite}
@keyframes s{to{transform:rotate(360deg)}}
</style></head><body><main>
<header><a class="back" href="/">← Dashboard</a><h1>Settings</h1></header>
<div class="warn" id="msg"></div>
<form class="card" id="first" hidden>
 <h2>Create a dashboard password</h2>
 <p class="hint" style="margin:0 0 14px">Settings and the camera preview need a password first. Anyone on your Wi-Fi can use the
 dashboard until you set one. Once it's set, changing it needs this password.</p>
 <input class="u" name="username" value="yicam-bridge" autocomplete="username" tabindex="-1" aria-hidden="true">
 <div class="f"><label for="first1">New password</label>
  <input id="first1" type="password" autocomplete="new-password" minlength="12" required>
  <p class="hint">At least 12 characters. Your browser will offer to save it.</p></div>
 <div class="f"><label for="first2">Repeat new password</label>
  <input id="first2" type="password" autocomplete="new-password" required>
  <div class="e" id="e_first"></div></div>
 <button class="pri" id="firstBtn">Set password and sign in</button>
</form>
<form id="f" novalidate>
<input class="u" name="username" value="yicam-bridge" autocomplete="username" tabindex="-1" aria-hidden="true">
<section class="card"><h2>Wi-Fi</h2>
 <div class="cur">Connected to <b id="wSsid">…</b> <span class="hint" id="wSig"></span>
  <p class="hint">Also at <a id="host" href="#"></a> · IP <span id="ip"></span></p>
  <p class="hint">Setup network <b id="apName"></b>, password <code id="apPass">••••-••••-••••</code>
   <button type="button" class="link" id="apShow" style="padding:0 0 0 6px">Show</button><br>
   It opens if the bridge can't join any Wi-Fi, or when you hold BOOT while pressing EN.
   This password is unique to this bridge; keep a copy.</p></div>
 <button type="button" id="wBtn">Change network…</button>
 <div id="wBox" hidden>
  <div class="f"><label for="wSel">Network</label>
   <div class="inl"><select id="wSel"></select><button type="button" id="wScan">Rescan</button></div>
   <input id="wOther" placeholder="Network name" maxlength="32" style="display:none;margin-top:8px">
   <div class="e" id="e_wifiSsid"></div></div>
  <div class="f"><label for="wPass">Wi-Fi password</label>
   <input id="wPass" type="password" autocomplete="new-password" data-lpignore="true" data-1p-ignore maxlength="63">
   <div class="e" id="e_wifiPass"></div></div>
  <p class="hint">The bridge leaves this network to try the new one (3 tries, about a minute). If it can't join,
  it comes back to this network on its own. Only if both fail does it open the <b>YiCam-Setup</b> network.</p>
  <button type="button" class="link" id="wCancel">Keep the current network</button>
 </div>
</section>
<div id="sections"></div>
<div class="save"><div>
 <div class="f"><label for="current">Current dashboard password</label>
  <input id="current" name="current" type="password" autocomplete="current-password" placeholder="Current dashboard password" required>
  <div class="e" id="e_current"></div></div>
 <button class="pri" id="sv">Save and restart</button></div></div>
</form></main>
<div class="ov" id="ov"><div><div class="spin" id="spin"></div><b id="ovT"></b><p id="ovP"></p></div></div>
<script>
const $=id=>document.getElementById(id),sleep=ms=>new Promise(r=>setTimeout(r,ms));
// [name, label, type, hint]; type: text | number | secret | new | select:a|b
const SPEC=[
 ['Camera',[['camHost','Camera IP or host name','text'],
  ['camUser','Camera web user','text','Leave blank if the camera has no web login.'],
  ['camPass','Camera web password','secret'],['res','Snapshot resolution','select:high|low']]],
 ['Google Drive',[['scriptUrl','Apps Script web app URL','text'],['token','Apps Script token','secret']]],
 ['Motion (MQTT)',[['mqttUser','MQTT user for the camera','text','Blank means no login: anything on your network can connect.'],
  ['mqttPass','MQTT password','secret'],['topic','Motion topic','text'],
  ['interval','Seconds between shots','number'],['maxShots','Max shots per motion event','number']]],
 ['Video clips',[['videoOn','Upload motion video clips','select:on|off','The camera records a clip of each motion event; the bridge uploads it to the same Drive day folder after the photos.'],
  ['videoCap','Max clips per day','number','About 5 MB each. Clips over the limit stay on the camera only.']]],
 ['Remote control',[['remoteOn','Check in with the Apps Script','select:on|off','Lets the Apps Script control page request captures. The bridge only makes outgoing requests; nothing is opened to the internet.'],
  ['pollSec','Seconds between check-ins','number','15 to 300. A request from the control page starts within this time.']]],
 ['Device',[['tz','Timezone (POSIX)','text','For example <-03>3 for UTC-3.'],['otaPass','OTA update password','secret']]],
 ['Dashboard password',[['newDash','New password','new','At least 12 characters. Leave blank to keep the current one. Changing it signs every device out.'],
  ['newDash2','Repeat new password','new']]]];
let S=null,curSsid='';

function build(){const root=$('sections');
 for(const[title,fields]of SPEC){const c=document.createElement('section');c.className='card';
  const h=document.createElement('h2');h.textContent=title;c.appendChild(h);
  for(const[n,l,t,hint]of fields){const d=document.createElement('div');d.className='f';
   const lb=document.createElement('label');lb.htmlFor=n;lb.textContent=l;d.appendChild(lb);let el;
   if(t.startsWith('select:')){el=document.createElement('select');
    for(const o of t.slice(7).split('|'))el.add(new Option(o,o))}
   else{el=document.createElement('input');el.type=t==='number'?'number':t==='text'?'text':'password';
    if(t==='secret'||t==='new'){el.autocomplete='new-password';el.setAttribute('data-lpignore','true');el.setAttribute('data-1p-ignore','')}
    else el.autocomplete='off'}
   el.id=n;el.name=n;d.appendChild(el);
   if(hint){const p=document.createElement('p');p.className='hint';p.textContent=hint;d.appendChild(p)}
   const e=document.createElement('div');e.className='e';e.id='e_'+n;d.appendChild(e);c.appendChild(d)}
  root.appendChild(c)}}

function ferr(n,m){const e=$('e_'+n);if(e){e.textContent=m;e.style.display='block'}
 const el=n==='wifiSsid'?$('wSel'):n==='wifiPass'?$('wPass'):$(n);if(el){el.classList.add('bad');el.focus()}}
function clearErr(){document.querySelectorAll('.e').forEach(e=>e.style.display='none');
 document.querySelectorAll('.bad').forEach(e=>e.classList.remove('bad'))}
function msg(m){const e=$('msg');e.textContent=m;e.style.display=m?'block':'none'}
function sig(r){return r>=-55?'excellent':r>=-67?'good':r>=-75?'fair':'weak'}

async function load(){
 const r=await fetch('/api/settings',{cache:'no-store'});
 if(r.status===401){location='/';return}
 if(r.status===403){$('f').hidden=true;$('first').hidden=false;$('first1').focus();return}
 S=await r.json();curSsid=S.wifi_ssid;
 for(const[,fields]of SPEC)for(const[n,,t]of fields){const el=$(n);
  if(t==='secret')el.placeholder=S['has_'+n]?'Saved. Leave blank to keep it':'Not set';
  else if(t!=='new')el.value=S[n]}
 $('wSsid').textContent=S.wifi_ssid;$('wSig').textContent='· '+sig(S.wifi_rssi)+' signal';
 $('host').textContent=S.hostname;$('host').href='http://'+S.hostname+'/';$('ip').textContent=S.ip;
 $('apName').textContent=S.ap_name;
 $('apShow').onclick=()=>{const c=$('apPass'),shown=c.textContent===S.ap_pass;
  c.textContent=shown?'••••-••••-••••':S.ap_pass;$('apShow').textContent=shown?'Show':'Hide'}}

async function scan(){const s=$('wSel');s.length=0;s.add(new Option('Scanning…',''));$('wScan').disabled=true;
 try{for(let i=0;i<20;i++){const r=await fetch('/api/wifi/scan',{cache:'no-store'});
   if(r.status===401){location='/';return}const j=await r.json();
   if(j.networks){s.length=0;s.add(new Option('Choose a network…',''));
    for(const n of j.networks)s.add(new Option(n.ssid+(n.ssid===curSsid?' (current)':'')+' · '+sig(n.rssi)+(n.open?' · open':''),n.ssid));
    s.add(new Option('Other network…','__other__'));return}
   await sleep(1000)}
  s.length=0;s.add(new Option('Scan failed, try Rescan',''))}
 finally{$('wScan').disabled=false}}
function wSsid(){if($('wBox').hidden)return'';const v=$('wSel').value;return v==='__other__'?$('wOther').value.trim():v}

$('wBtn').onclick=()=>{$('wBox').hidden=false;$('wBtn').hidden=true;scan()};
$('wCancel').onclick=()=>{$('wBox').hidden=true;$('wBtn').hidden=false;$('wPass').value='';clearErr()};
$('wScan').onclick=scan;
$('wSel').onchange=()=>{$('wOther').style.display=$('wSel').value==='__other__'?'block':'none'};

$('f').onsubmit=async e=>{e.preventDefault();clearErr();msg('');
 const nd=$('newDash').value;
 if(nd&&nd.length<12)return ferr('newDash','Use at least 12 characters.');
 if(nd!==$('newDash2').value)return ferr('newDash2','The passwords don’t match.');
 const ssid=wSsid();
 if(!$('wBox').hidden&&!ssid)return ferr('wifiSsid','Choose a network, or click “Keep the current network”.');
 const wl=$('wPass').value.length;
 if(ssid&&wl&&(wl<8||wl>63))return ferr('wifiPass','Wi-Fi passwords are 8 to 63 characters (leave blank for an open network).');
 if(!$('current').value)return ferr('current','Enter your current dashboard password to save.');
 if(ssid&&!confirm('The bridge will leave “'+curSsid+'” to try “'+ssid+'”. If it can’t join, it comes back to “'+curSsid+'” after about a minute. Continue?'))return;
 const p=new URLSearchParams();
 for(const el of $('f').elements)if(el.name&&el.name!=='username'&&el.name!=='newDash2')p.append(el.name,el.value);
 p.set('newDash',nd);if(ssid){p.set('wifiSsid',ssid);p.set('wifiPass',$('wPass').value)}
 $('sv').disabled=true;
 try{const r=await fetch('/api/settings',{method:'POST',body:p});
  if(r.status===401){location='/';return}
  let j={};try{j=await r.json()}catch(x){}
  if(!r.ok){$('sv').disabled=false;
   if(j.error==='bad_password')return ferr('current','Wrong password.');
   if(j.error==='locked')return ferr('current','Too many wrong attempts. Wait a minute and try again.');
   if(j.error==='invalid')return ferr(j.field,j.msg);
   return msg('Could not save ('+(j.error||r.status)+').')}
  $('current').value='';restartWait(j,ssid)}
 catch(x){$('sv').disabled=false;msg('Could not reach the bridge.')}};

async function restartWait(j,ssid){$('ov').style.display='grid';
 const T=t=>$('ovT').textContent=t,P=t=>$('ovP').textContent=t;
 T('Saved. Restarting the bridge…');
 P(ssid?'Trying “'+ssid+'”. This page reconnects if the bridge comes back on this network.':'This takes about 10 seconds.');
 await sleep(4000);const end=Date.now()+(ssid?150000:60000);
 while(Date.now()<end){try{const ctl=new AbortController();setTimeout(()=>ctl.abort(),3000);
   const r=await fetch('/status',{cache:'no-store',signal:ctl.signal});
   if(r.status===401||r.ok){location='/';return}}catch(x){}
  await sleep(2000)}
 $('spin').style.display='none';
 if(ssid){T('The bridge is no longer reachable here');
  $('ovP').textContent='It has probably joined “'+ssid+'”. Connect this device to that network and open ';
  const a=document.createElement('a');a.href='http://'+(S?S.hostname:'yicam-bridge.local')+'/';a.textContent=a.href;$('ovP').append(a,'.')}
 else{T('The bridge hasn’t come back yet');P('Give it a minute, then reload this page.')}}

$('first').onsubmit=async e=>{e.preventDefault();clearErr();msg('');
 const a=$('first1').value,b=$('first2').value;
 if(a.length<12)return ferr('first','Use at least 12 characters.');
 if(a!==b)return ferr('first','The passwords don’t match.');
 $('firstBtn').disabled=true;
 try{const r=await fetch('/api/first-password',{method:'POST',body:new URLSearchParams({newDash:a})});
  let j={};try{j=await r.json()}catch(x){}
  if(r.ok||j.error==='already_set'){location.reload();return}
  $('firstBtn').disabled=false;
  if(j.error==='invalid')return ferr('first',j.msg);
  if(j.error==='clock')return ferr('first','The bridge is still starting up. Try again in a few seconds.');
  msg('Could not set the password ('+(j.error||r.status)+').')}
 catch(x){$('firstBtn').disabled=false;msg('Could not reach the bridge.')}};

build();load();
</script></body></html>)HTML";
