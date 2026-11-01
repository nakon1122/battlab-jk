#pragma once
// Setup / status page served by the bridge (http://bambubridge.local or 192.168.4.1)
static const char WEB_PAGE[] PROGMEM = R"HTML(<!doctype html><html lang=th><head><meta charset=utf-8>
<meta name=viewport content="width=device-width,initial-scale=1"><title>Bambu Bridge</title>
<style>
:root{--bg:#0b0f14;--card:#161c24;--line:#2a3442;--text:#e8edf2;--muted:#8a96a3;--g:#00ae42;--y:#f5c400;--r:#f04848;--b:#3ca0ff}
*{box-sizing:border-box}body{margin:0;font-family:system-ui,-apple-system,"Segoe UI",sans-serif;background:var(--bg);color:var(--text)}
main{max-width:760px;margin:auto;padding:16px}h1{font-size:22px;margin:4px 0 2px}h1 b{color:var(--g)}
h2{font-size:17px;margin:26px 0 10px}.sub{color:var(--muted);font-size:14px}
.card{background:var(--card);border:1px solid var(--line);border-radius:12px;padding:14px;margin:10px 0}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(220px,1fr));gap:10px}
.pill{display:inline-block;padding:2px 9px;border-radius:99px;font-size:12px;font-weight:600;color:#000}
.ok{background:var(--g)}.warn{background:var(--y)}.bad{background:var(--r);color:#fff}.idle{background:#556;color:#fff}
label{display:block;font-size:13px;color:var(--muted);margin:10px 0 4px}
input,select{width:100%;padding:10px;border-radius:8px;border:1px solid var(--line);background:#0f141b;color:var(--text);font-size:15px}
.row{display:grid;grid-template-columns:1fr 1fr;gap:10px}
button{padding:11px 16px;border:0;border-radius:8px;background:var(--g);color:#fff;font-size:15px;font-weight:600;cursor:pointer}
button.sec{background:#2a3442}a{color:var(--b)}code{background:#0f141b;padding:2px 6px;border-radius:6px;word-break:break-all}
.kv{display:grid;grid-template-columns:110px 1fr;gap:4px 10px;font-size:14px}.kv div:nth-child(odd){color:var(--muted)}
.bar{height:6px;background:#2a3442;border-radius:3px;overflow:hidden;margin-top:8px}.bar i{display:block;height:100%;background:var(--g)}
.chk{display:flex;flex-wrap:wrap;gap:10px 18px;margin-top:8px}.chk label{display:flex;gap:6px;align-items:center;margin:0;color:var(--text)}
.chk input{width:auto}details summary{cursor:pointer;color:var(--muted)}#msg{position:fixed;left:50%;bottom:18px;transform:translateX(-50%);background:#222c38;padding:10px 16px;border-radius:10px;display:none}
</style></head><body><main>
<h1><b>Bambu</b> Bridge</h1><div class=sub>อ่านสถานะเครื่องพิมพ์ผ่าน LAN แล้วส่งขึ้น cloud · <span id=fw></span></div>

<div class="card kv" id=sys></div>
<div class=card><div class=sub>ดูจากนอกบ้าน (เปิดบนมือถือได้เลย)</div><div style="margin-top:6px"><a id=dash target=_blank></a></div>
<div class=sub style="margin-top:8px">View key สำหรับจอ CYD: <code id=vkey></code></div></div>

<h2>เครื่องพิมพ์</h2><div class=grid id=live></div>

<h2>ตั้งค่าเครื่องพิมพ์</h2>
<div class=sub>ดู IP, Serial และ Access Code ได้ที่หน้าจอเครื่อง: Settings → Network (LAN Only / Developer Mode ต้องเปิด)</div>
<div id=slots></div><button onclick=savePrinters()>บันทึกเครื่องพิมพ์</button>

<h2>แจ้งเตือน LINE</h2>
<div class=card><div class=sub>ใช้ LINE Messaging API (LINE Notify ปิดบริการแล้ว) — ใส่ Channel access token และ User ID ของคุณ แล้วเพิ่มบอทเป็นเพื่อนก่อน</div>
<label>Channel access token (long-lived)</label><input id=ltok type=password placeholder="(ไม่เปลี่ยนถ้าเว้นว่าง)">
<label>Your user ID (ขึ้นต้นด้วย U...)</label><input id=lto placeholder="U1234...">
<div class=chk id=kinds></div>
<div style="display:flex;gap:10px;margin-top:12px"><button onclick=saveLine()>บันทึก LINE</button><button class=sec onclick=testLine()>ส่งข้อความทดสอบ</button></div>
<div class=sub id=lstat style="margin-top:10px"></div></div>

<h2>Wi-Fi</h2>
<div class=card><label>ชื่อ Wi-Fi บ้าน</label><select id=nets onchange="ssid.value=this.value"><option value="">-- กดสแกน --</option></select>
<input id=ssid style="margin-top:6px" placeholder="หรือพิมพ์ชื่อ"><label>รหัสผ่าน</label><input id=pass type=password placeholder="(ไม่เปลี่ยนถ้าเว้นว่าง)">
<div style="display:flex;gap:10px;margin-top:12px"><button onclick=saveWifi()>บันทึกและรีสตาร์ต</button><button class=sec onclick=scan()>สแกน</button></div></div>

<details class=card><summary>ขั้นสูง</summary><label>Cloud write key (ปกติไม่ต้องแก้)</label><input id=wkey placeholder="(ไม่เปลี่ยนถ้าเว้นว่าง)">
<div style="display:flex;gap:10px;margin-top:12px"><button onclick="post('/api/save',{wkey:wkey.value}).then(()=>toast('บันทึกแล้ว'))">บันทึก key</button>
<button class=sec onclick="fetch('/api/pushall').then(()=>toast('ขอข้อมูลใหม่จากเครื่องพิมพ์แล้ว'))">ดึงข้อมูลใหม่</button>
<button class=sec onclick="fetch('/api/restart');toast('กำลังรีสตาร์ต')">รีสตาร์ต</button></div></details>
<div id=msg></div>
</main><script>
const MODELS=["A1","A1 mini","P1P","P1S","P2S","X1C","X1E","X2D","H2D","H2C","H2S","Other"];
const KINDS={finished:"พิมพ์เสร็จ",failed:"ล้มเหลว",paused:"หยุดพัก",error:"มี error",offline:"ขาดการเชื่อมต่อ",online:"กลับมาออนไลน์",started:"เริ่มพิมพ์",resumed:"พิมพ์ต่อ"};
const $=id=>document.getElementById(id);let filled=false;
function toast(t){const m=$('msg');m.textContent=t;m.style.display='block';clearTimeout(m._t);m._t=setTimeout(()=>m.style.display='none',3000)}
function post(u,b){return fetch(u,{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(b)}).then(r=>r.json().catch(()=>({})))}
function esc(s){return String(s??'').replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]))}
function stPill(p){if(!p.connected)return'<span class="pill bad">ไม่ได้เชื่อมต่อ</span>';const s=p.state||'';
 const m={RUNNING:['ok','กำลังพิมพ์'],PREPARE:['ok','เตรียมพิมพ์'],PAUSE:['warn','หยุดพัก'],FINISH:['ok','เสร็จแล้ว'],FAILED:['bad','ล้มเหลว'],IDLE:['idle','ว่าง']}[s]||['idle',s||'รอข้อมูล'];
 return `<span class="pill ${m[0]}">${m[1]}</span>`}
function slotHtml(p){const i=p.slot;return `<div class=card><b>ช่อง ${i+1}</b>
<div class=row><div><label>ชื่อที่แสดง</label><input id=n${i} value="${esc(p.name)}" placeholder="เช่น A1 ห้องทำงาน"></div>
<div><label>รุ่น</label><select id=m${i}>${MODELS.map(x=>`<option ${x==p.model?'selected':''}>${x}</option>`).join('')}</select></div></div>
<div class=row><div><label>IP เครื่องพิมพ์</label><input id=i${i} value="${esc(p.ip)}" placeholder="192.168.1.50" inputmode=decimal></div>
<div><label>Serial</label><input id=s${i} value="${esc(p.serial)}" placeholder="01S00C..."></div></div>
<label>Access code ${p.has_code?'(บันทึกแล้ว)':''}</label><input id=c${i} placeholder="${p.has_code?'(ไม่เปลี่ยนถ้าเว้นว่าง)':'8 ตัวอักษร'}">
${p.used?`<label style="display:flex;gap:6px;align-items:center;margin-top:10px"><input type=checkbox id=x${i} style="width:auto">ลบเครื่องนี้</label>`:''}</div>`}
function render(d){$('fw').textContent='v'+d.fw;
 $('sys').innerHTML=`<div>Wi-Fi</div><div>${d.wifi?esc(d.ssid)+' ('+d.rssi+' dBm) · '+d.ip:'<span class="pill bad">ยังไม่ได้ต่อ</span>'}${d.ap?' · เปิด hotspot BAMBU-BRIDGE':''}</div>
 <div>Cloud</div><div>${d.cloud_ok?'<span class="pill ok">OK</span> ส่งล่าสุด '+d.cloud_age+' วินาทีที่แล้ว':'<span class="pill bad">'+esc(d.cloud_err)+'</span>'}</div>
 <div>หน่วยความจำ</div><div>${Math.round(d.heap/1024)} KB ว่าง · ทำงาน ${Math.floor(d.uptime/3600)} ชม. ${Math.floor(d.uptime/60)%60} นาที</div>`;
 const url=d.dashboard+'#'+d.view_key;$('dash').href=url;$('dash').textContent=url;$('vkey').textContent=d.view_key;
 const used=d.printers.filter(p=>p.used);
 $('live').innerHTML=used.length?used.map(p=>`<div class=card><div style="display:flex;justify-content:space-between;gap:8px"><b>${esc(p.name||p.serial)}</b>${stPill(p)}</div>
 <div class=sub>${esc(p.model)} · ${esc(p.ip)}</div>
 ${p.connected&&p.state?`<div style="margin-top:8px">${esc(p.task||'-')}</div><div class=bar><i style="width:${p.pct||0}%"></i></div>
 <div class=sub style="margin-top:6px">${p.pct||0}% · เหลือ ${p.remain||0} นาที · หัวฉีด ${Math.round(p.noz||0)}° · ฐาน ${Math.round(p.bed||0)}°</div>`:''}
 ${p.err?`<div class=sub style="color:var(--r);margin-top:6px">${esc(p.err)}</div>`:''}</div>`).join(''):'<div class=sub>ยังไม่มีเครื่องพิมพ์ — ตั้งค่าด้านล่าง</div>';
 if(!filled){$('slots').innerHTML=d.printers.map(slotHtml).join('');$('ssid').value=d.ssid||'';filled=true}}
function load(){fetch('/api/status').then(r=>r.json()).then(render).catch(()=>{})}
function savePrinters(){const a=[];for(let i=0;i<4;i++)a.push({name:$('n'+i).value,model:$('m'+i).value,ip:$('i'+i).value,serial:$('s'+i).value,code:$('c'+i).value,clear:$('x'+i)?.checked||false});
 post('/api/save',{printers:a}).then(()=>{toast('บันทึกแล้ว กำลังเชื่อมต่อ...');filled=false;setTimeout(load,800)})}
function saveWifi(){if(!$('ssid').value)return toast('ใส่ชื่อ Wi-Fi ก่อน');post('/api/save',{ssid:$('ssid').value,pass:$('pass').value});toast('บันทึกแล้ว กำลังรีสตาร์ต...')}
function scan(){toast('กำลังสแกน...');const go=()=>fetch('/api/scan').then(r=>r.json()).then(d=>{if(d.running)return setTimeout(go,1500);
 $('nets').innerHTML='<option value="">-- เลือก --</option>'+d.nets.map(n=>`<option>${esc(n.ssid)}</option>`).join('')});go()}
$('kinds').innerHTML=Object.entries(KINDS).map(([k,v])=>`<label><input type=checkbox value=${k}>${v}</label>`).join('');
function lineStatus(){post('/api/line',{action:'status'}).then(d=>{if(d.error)return $('lstat').textContent='อ่านสถานะไม่ได้';
 document.querySelectorAll('#kinds input').forEach(c=>c.checked=(d.kinds||[]).includes(c.value));
 $('lstat').innerHTML=d.configured?`ตั้งค่าแล้ว (ส่งถึง …${esc(d.to_tail)})`+(d.last_status?` · ส่งล่าสุด: ${d.last_status==200?'สำเร็จ':'ไม่สำเร็จ ('+d.last_status+') '+esc(d.last_body)}`:''):'ยังไม่ได้ตั้งค่า'})}
function saveLine(){const k=[...document.querySelectorAll('#kinds input:checked')].map(c=>c.value);
 if(!$('lto').value)return toast('ใส่ User ID ก่อน');
 post('/api/line',{action:'save',token:$('ltok').value,to:$('lto').value,kinds:k}).then(d=>{toast(d.error?'บันทึกไม่สำเร็จ':'บันทึกแล้ว');lineStatus()})}
function testLine(){post('/api/line',{action:'test'}).then(d=>{toast(d.ok?'ส่งแล้ว ดูใน LINE':(d.error||'ส่งไม่ได้'));setTimeout(lineStatus,3000)})}
load();lineStatus();setInterval(load,3000);
</script></body></html>)HTML";
