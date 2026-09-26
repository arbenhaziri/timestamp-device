#pragma once
const char PAGE[] PROGMEM = R"HTML(<!doctype html><html lang=en><meta charset=utf-8>
<meta name=viewport content="width=device-width,initial-scale=1"><title>Timestamp terminal</title>
<style>
*{box-sizing:border-box}body{background:#0f1216;color:#d7dde5;font:14px system-ui,sans-serif;margin:0;padding:20px;max-width:1100px;margin:auto}
h1{font-size:22px}h2{font-size:17px}.row{display:flex;gap:10px;flex-wrap:wrap;margin:16px 0}
.card{background:#171c23;border:1px solid #293240;border-radius:8px;padding:14px;flex:1;min-width:140px}.card b{display:block;color:#95a6bc;font-size:12px;margin-bottom:6px}
.v{font:15px ui-monospace,monospace;overflow-wrap:anywhere}.ok{color:#4ade80}.bad{color:#fca5a5}.muted{color:#a0aaba}
button{background:#1f6feb;color:white;border:0;border-radius:6px;padding:10px 16px;cursor:pointer}button.g{background:#303b4b}
input,select{background:#0f1216;color:#d7dde5;border:1px solid #45536a;border-radius:6px;padding:10px;width:100%;margin-top:5px}
label{display:block;margin-bottom:14px}.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(260px,1fr));gap:0 18px}
#log{background:#090d12;padding:14px;border:1px solid #293240;border-radius:8px;min-height:170px;max-height:360px;overflow:auto;white-space:pre-wrap;font:12px/1.7 ui-monospace,monospace}
a{color:#91bbff}#legacy{padding:12px;border:1px solid #bd9646;border-radius:6px}#cfgMsg,#connection{margin:12px 0}
</style>
<h1>Timestamp terminal</h1><div class=muted>Fingerprint and RFID punches · device time shown in UTC</div>
<div id=connection role=status></div>
<div class=row>
<div class=card><b>Device name</b><div class=v id=deviceName>…</div></div>
<div class=card><b>Device code</b><div class=v id=deviceCode>…</div></div>
<div class=card><b>Time (UTC)</b><div class=v id=clock>Waiting for clock…</div></div>
<div class=card><b>Punch mode</b><div class=v id=mode>…</div></div>
</div>
<div class=row>
<div class=card><b>Fingerprint</b><div class=v id=fp>…</div><small class=muted id=touch>Waiting</small></div>
<div class=card><b>RFID</b><div class=v id=nfc>…</div><small class=muted>Last card: <span id=cardId>—</span></small></div>
<div class=card><b>WiFi</b><div class=v id=wifi>…</div></div>
<div class=card><b>Punch status</b><div class=v id=serverStatus>Not yet sent</div></div>
<div class=card><b>Queued punches</b><div class=v id=queue>…</div></div>
<div class=card><b>Storage</b><div class=v id=storage>…</div></div>
</div>
<div id=legacy hidden>Old-format punches retained: <b id=legacyCount></b>. <a href=/api/legacy-queue download>Download old queue</a>. These records need reconciliation in your dashboard.</div>
<p>Last saved punch: <span id=lastPunch>—</span></p>
<h2>Fingerprint</h2>
<div class=row><button id=enroll disabled>Enroll</button><button class=g id=cancelEnroll hidden>Cancel</button></div>
<p id=fingerMessage role=status>Connecting…</p>
<p>Enrolled: <span id=fingerCount>—</span> · New ID: <b id=enrolledId>—</b> · Matched ID: <b id=fingerId>—</b></p>
<div class=row><button class=g id=flush>Send queue</button><button class=g id=clear>Clear log</button></div>
<div id=log aria-label="Device terminal log"></div>
<h2>Settings</h2>
<form id=config><div class=grid>
<label>WiFi name<input id=cWifiSsid maxlength=32 autocomplete=off></label>
<label>WiFi password <span id=wifiPassState class=muted></span><input type=password id=cWifiPass maxlength=63 placeholder="Blank keeps current" autocomplete=new-password></label>
<label>Server URL<input id=cServer type=url maxlength=192 autocomplete=off></label>
<label>Punch path<input id=cPunchesPath maxlength=96></label>
<label>Device code<input id=cDeviceId maxlength=32 pattern="[A-Za-z0-9_.\-]+" required></label>
<label>Direction<select id=cEventType required><option value="">Choose</option><option value=1>IN (1)</option><option value=0>OUT (0)</option></select></label>
<label>API key <span id=tokenState class=muted></span><input type=password id=cToken maxlength=192 placeholder="Blank keeps current" autocomplete=new-password></label>
</div><button id=save type=submit>Save</button><div id=cfgMsg role=status></div></form>
<p class=muted>Server URL is blank for editing. Leave it blank to keep the saved URL.</p>
<p class=muted>Click Enroll, then follow the three touch-and-lift prompts. Enrollment shows the new ID without sending a punch. Future fingerprint matches send that ID as cardRefCode. RFID scans send the card UID as a decimal string in cardRefCode. No ID means no punch.</p>
<p class=muted>HTTPS certificate checks are disabled. Connect WiFi to synchronize the clock before scanning.</p>
<p class=muted>The page stays available at the local WiFi IP while connected. Setup WiFi: <span id=apSsid></span> at <span id=apIp></span>.<br>Hold BOOT 3s (cyan) to open setup WiFi; release before red. Factory reset: release at red (12s, empty queue) or purple (20s, erases queued records). For upload mode only: hold BOOT while resetting, then release promptly.</p>
<script>
const el=id=>document.getElementById(id);let csrf='',seq=-1,loaded=false,polling=false,enrollRequest=false;
function status(id,text,ok){el(id).textContent=text;el(id).className='v '+(ok?'ok':'bad');}
async function command(path){try{const r=await fetch(path,{method:'POST',headers:{'X-CSRF-Token':csrf}});if(!r.ok)throw Error(await r.text());await tick();}catch(e){el('connection').textContent=e.message;}}
el('flush').onclick=()=>command('/api/flush');el('clear').onclick=()=>command('/api/clear');
el('enroll').onclick=async()=>{if(enrollRequest)return;enrollRequest=true;el('enroll').disabled=true;await command('/api/enroll');enrollRequest=false;await tick();};
el('cancelEnroll').onclick=()=>command('/api/enroll/cancel');
async function tick(){if(polling)return;polling=true;try{
const r=await fetch('/api/state');if(!r.ok)throw Error(await r.text());const s=await r.json();
el('connection').textContent='';csrf=s.csrf;el('deviceName').textContent=s.deviceName;el('deviceCode').textContent=s.deviceId;
status('clock',s.clock||'Waiting for clock sync',!!s.clock);status('mode',s.eventType===1?'IN (1)':s.eventType===0?'OUT (0)':'Choose IN / OUT',s.eventType===0||s.eventType===1);
status('fp',s.fp?'Connected':'Not found',s.fp);status('wifi',s.wifi?s.ip:'Disconnected',s.wifi);
status('nfc',s.nfc?'Connected':'Not found',s.nfc);el('cardId').textContent=s.cardId||'—';
el('touch').textContent=s.fingerTouch?'Finger touching':'Waiting for finger';
el('fingerMessage').textContent=s.fingerMessage;el('fingerCount').textContent=s.fingerCount<0?'—':s.fingerCount;
el('fingerId').textContent=s.fingerId||'—';el('enrolledId').textContent=s.enrolledId||'—';
el('enroll').disabled=enrollRequest||!s.canEnroll;el('cancelEnroll').hidden=!s.enrolling;
status('serverStatus',s.punchFeedback===2?'Accepted':s.punchFeedback===3?'Failed':s.punchFeedback===1?'Sending':'Not yet sent',s.punchFeedback===2);status('queue',s.queue<0?'Unavailable':String(s.queue),s.queue===0);status('storage',s.fsReady?'Ready':'FAULT',s.fsReady);
el('legacy').hidden=s.legacyQueue===0;el('legacyCount').textContent=s.legacyQueue<0?'unavailable':s.legacyQueue;el('lastPunch').textContent=s.lastPunch;
if(!loaded){el('cPunchesPath').value=s.punchesPath;el('cDeviceId').value=s.deviceId;el('cWifiSsid').value=s.wifiSsid;el('cEventType').value=s.eventType===0||s.eventType===1?String(s.eventType):'';loaded=true;}
el('tokenState').textContent=s.tokenSet?'(set)':'(empty)';el('wifiPassState').textContent=s.wifiPassSet?'(set)':'(empty)';el('apSsid').textContent=s.apSsid;el('apIp').textContent=s.apIp;
if(s.seq!==undefined&&s.seq!==seq){seq=s.seq;const log=el('log'),end=log.scrollTop+log.clientHeight>=log.scrollHeight-30;log.textContent=s.log.join('\n');if(end)log.scrollTop=log.scrollHeight;}
}catch(e){el('connection').textContent='Terminal unavailable: '+e.message;}finally{polling=false;}}
el('config').onsubmit=async e=>{e.preventDefault();el('save').disabled=true;try{
const body={punches_path:el('cPunchesPath').value.trim(),device_id:el('cDeviceId').value.trim(),event_type:Number(el('cEventType').value),wifi_ssid:el('cWifiSsid').value};
if(el('cServer').value.trim())body.server=el('cServer').value.trim();
for(const [id,key] of [['cToken','token'],['cWifiPass','wifi_pass']])if(el(id).value.length)body[key]=el(id).value;
const r=await fetch('/api/config',{method:'POST',headers:{'Content-Type':'application/json','X-CSRF-Token':csrf},body:JSON.stringify(body)});if(!r.ok)throw Error(await r.text());
el('cfgMsg').textContent='Saved. Scans will use this punch mode.';for(const id of ['cToken','cWifiPass'])el(id).value='';loaded=false;await tick();
}catch(e){el('cfgMsg').textContent='Could not save: '+e.message;}finally{el('save').disabled=false;}};
tick();setInterval(tick,1000);
</script></html>)HTML";
