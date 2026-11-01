// Bambu Monitor - remote control panel (needs the control key).
// Commands go: dashboard -> Supabase queue -> ESP32 bridge (MQTT) or Raspberry Pi hub (files / camera).
(function () {
  const css = `
#panel{position:fixed;inset:0;z-index:50;background:rgba(0,0,0,.55);display:flex;justify-content:flex-end}
#panel[hidden]{display:none}
.pn{width:min(560px,100%);height:100%;background:var(--bg);border-left:1px solid var(--line);display:flex;flex-direction:column}
.pnh{padding:14px 16px 10px;border-bottom:1px solid var(--line)}
.pnh .row1{display:flex;align-items:center;gap:10px}.pnh .x{margin-left:auto;background:none;border:0;color:var(--muted);font-size:26px;cursor:pointer;line-height:1}
.pnh .live{font-size:13px;color:var(--muted);margin-top:6px}
.tabs{display:flex;gap:4px;padding:8px 10px;border-bottom:1px solid var(--line);overflow-x:auto}
.tabs button{background:none;border:0;color:var(--muted);padding:8px 12px;border-radius:8px;font:inherit;font-size:14px;cursor:pointer;white-space:nowrap}
.tabs button.on{background:var(--card2);color:var(--text)}
.pb{flex:1;overflow-y:auto;padding:14px 16px 40px}
.sec{background:var(--card);border:1px solid var(--line);border-radius:12px;padding:12px;margin-bottom:12px}
.sec h3{margin:0 0 10px;font-size:14px;color:var(--muted);font-weight:600}
.btns{display:flex;gap:8px;flex-wrap:wrap}
.b{padding:10px 14px;border-radius:9px;border:1px solid var(--line);background:var(--card2);color:var(--text);font:inherit;font-size:14px;cursor:pointer}
.b:hover{border-color:var(--muted)}.b.g{background:var(--bambu);border-color:var(--bambu);color:#fff}.b.r{background:#5a1f22;border-color:var(--red);color:#ffd0d0}
.b.y{background:#4a3d0a;border-color:var(--yellow);color:#ffe9a0}.b.on{border-color:var(--bambu);color:var(--bambu)}
.b:disabled{opacity:.4;cursor:default}
.kv2{display:grid;grid-template-columns:1fr auto auto;gap:8px;align-items:center;margin-bottom:8px}
.kv2 input,.pb input[type=text],.pb input[type=number],.pb select{padding:9px;border-radius:8px;border:1px solid var(--line);background:#0f141b;color:var(--text);font:inherit;font-size:15px;width:100%}
.kv2 input{width:90px}
.rng{display:grid;grid-template-columns:110px 1fr 46px;gap:10px;align-items:center;margin:6px 0}
.rng input{width:100%;accent-color:var(--bambu)}
.jog{display:grid;grid-template-columns:repeat(4,1fr);gap:6px}
.cam{background:#000;border-radius:12px;overflow:hidden;aspect-ratio:16/9;display:flex;align-items:center;justify-content:center;color:var(--muted);position:relative}
.cam img,.cam iframe{width:100%;height:100%;object-fit:contain;border:0;display:block}
.cam .ts{position:absolute;left:8px;bottom:6px;font-size:12px;background:rgba(0,0,0,.6);padding:2px 6px;border-radius:5px;color:#ddd}
.fl{display:grid;grid-template-columns:1fr auto;gap:8px;align-items:center;padding:9px 0;border-bottom:1px solid var(--line)}
.fl:last-child{border:0}.fl .nm{word-break:break-all;font-size:14px}.fl .sub{font-size:12px}
.slot{display:grid;grid-template-columns:44px 1fr auto;gap:10px;align-items:center;padding:8px 0;border-bottom:1px solid var(--line)}
.slot:last-child{border:0}.sw{width:44px;height:44px;border-radius:9px;border:1px solid rgba(255,255,255,.15);display:flex;align-items:center;justify-content:center;font-size:11px;font-weight:700}
.tg{display:flex;justify-content:space-between;align-items:center;padding:9px 0;border-bottom:1px solid var(--line);gap:10px}
.tg:last-child{border:0}.tg small{display:block;color:var(--muted)}
.note{font-size:12px;color:var(--muted);margin-top:8px;line-height:1.5}
.mapr{display:grid;grid-template-columns:30px 1fr 1.3fr;gap:8px;align-items:center;margin:6px 0}
.dot2{width:22px;height:22px;border-radius:6px;border:1px solid rgba(255,255,255,.2)}
#toast{position:fixed;left:50%;bottom:22px;transform:translateX(-50%);z-index:60;display:flex;flex-direction:column;gap:6px;align-items:center;pointer-events:none}
#toast div{background:#222c38;border:1px solid var(--line);padding:9px 14px;border-radius:10px;font-size:14px;max-width:90vw}
#toast .ok{border-color:var(--bambu)}#toast .er{border-color:var(--red)}
.pbar{height:6px;background:var(--line);border-radius:3px;overflow:hidden;margin-top:8px}.pbar i{display:block;height:100%;background:var(--bambu);width:0}
`;
  const st = document.createElement('style');
  st.textContent = css;
  document.head.appendChild(st);

  const $p = document.getElementById('panel');
  let cur = null, tab = store.get('bambu_tab') || 'ctl', camTimer = null, camSince = null, filesCache = null, fileOp = '';
  let ctl = store.get('bambu_ctl_key') || '';

  // ------------------------------------------------------------ helpers --
  function toast(msg, cls = '', ms = 3500) {
    const t = document.createElement('div');
    t.className = cls;
    t.textContent = msg;
    document.getElementById('toast').appendChild(t);
    setTimeout(() => t.remove(), ms);
  }
  async function rpc(fn, body) {
    const r = await fetch(CLOUD + '/rest/v1/rpc/' + fn, {
      method: 'POST', headers: { apikey: ANON, Authorization: 'Bearer ' + ANON, 'Content-Type': 'application/json' },
      body: JSON.stringify(body)
    });
    const j = await r.json().catch(() => ({}));
    if (!r.ok) throw new Error(String(j.message || r.status));
    return j;
  }
  function P() { return (data?.printers || []).find(x => x.serial === cur) || null; }
  function needCtl() {
    if (ctl) return true;
    const k = prompt('ใส่รหัสควบคุม (ขึ้นต้นด้วย bc_ หรือรหัสที่ตั้งเองในหน้าเว็บ Bridge)');
    if (!k) return false;
    ctl = k.trim();
    store.set('bambu_ctl_key', ctl);
    return true;
  }
  const KIND_MSG = { ok: 'สำเร็จ', error: 'ไม่สำเร็จ', expired: 'บอร์ดไม่ได้รับคำสั่ง (ออฟไลน์?)' };
  async function send(label, cmd, target = 'bridge', opts = {}) {
    if (!needCtl()) return null;
    let id;
    try {
      id = (await rpc('bambu_cmd', { p_ctl: ctl, p_serial: cur, p_cmd: cmd, p_target: target })).id;
    } catch (e) {
      if (String(e.message).includes('invalid key')) { ctl = ''; store.set('bambu_ctl_key', ''); toast('รหัสควบคุมไม่ถูกต้อง', 'er'); }
      else toast(label + ': ' + e.message, 'er');
      return null;
    }
    toast(label + ' · ส่งแล้ว');
    const until = Date.now() + (opts.wait || 20000);
    while (Date.now() < until) {
      await new Promise(r => setTimeout(r, 1200));
      try {
        const s = (await rpc('bambu_cmd_status', { p_ctl: ctl, p_ids: [id] }))[0];
        if (s && ['ok', 'error', 'expired'].includes(s.status)) {
          const m = s.result?.msg ? ' · ' + s.result.msg : '';
          toast(label + ' · ' + KIND_MSG[s.status] + m, s.status === 'ok' ? 'ok' : 'er', 5000);
          return s;
        }
      } catch (e) { }
    }
    toast(label + ' · ยังไม่มีคำตอบ', 'er');
    return null;
  }
  const mqtt = payload => ({ type: 'mqtt', payload });
  const gcode = g => mqtt({ print: { command: 'gcode_line', param: g.trim() + '\n' } });
  function confirmDo(msg, fn) { if (confirm(msg)) fn(); }
  const isBusy = p => p && busy(p);

  // ------------------------------------------------------------- panel ---
  window.openPanel = function (serial) {
    cur = serial;
    filesCache = null;
    $p.hidden = false;
    document.body.style.overflow = 'hidden';
    drawPanel();
  };
  function closePanel() {
    $p.hidden = true;
    cur = null;
    stopCam();
    document.body.style.overflow = '';
  }
  $p.addEventListener('click', e => { if (e.target === $p) closePanel(); });
  document.addEventListener('keydown', e => { if (e.key === 'Escape' && cur) closePanel(); });

  function liveLine(p) {
    const s = p.state || {}, g = s.gcode_state;
    const [txt] = p.online ? (STATE[g] || ['รอข้อมูล']) : ['ออฟไลน์'];
    let t = `<b>${txt}</b>`;
    if (busy(p) || g === 'FINISH') t += ` · ${s.mc_percent || 0}% · ${g === 'FINISH' ? 'เสร็จ' : 'เหลือ ' + dur(s.remain) + ' · เสร็จ ' + eta(s.remain)}`;
    t += ` · หัวฉีด ${Math.round(s.noz?.[0] || 0)}°${s.noz2 ? '/' + Math.round(s.noz2[0]) + '°' : ''} · ฐาน ${Math.round(s.bed?.[0] || 0)}°`;
    if (p.error_code) t += ` · <span style="color:var(--red)">ERR ${esc(p.error_code)}</span>`;
    return t;
  }
  const TABS = [['ctl', 'ควบคุม'], ['cam', 'กล้อง'], ['files', 'ไฟล์ / สั่งพิมพ์'], ['ams', 'AMS'], ['set', 'ตั้งค่า']];
  function drawPanel() {
    const p = P();
    if (!p) { $p.innerHTML = `<div class="pn"><div class="pnh"><div class="row1"><b>ไม่พบเครื่อง</b><button class="x">×</button></div></div></div>`; $p.querySelector('.x').onclick = closePanel; return; }
    $p.innerHTML = `<div class="pn">
      <div class="pnh"><div class="row1"><b style="font-size:18px">${esc(p.name)}</b><span class="sub">${esc(p.model || '')}</span>
        <button class="x" title="ปิด">×</button></div><div class="live" id="pnLive">${liveLine(p)}</div></div>
      <div class="tabs">${TABS.map(([k, n]) => `<button data-t="${k}" class="${tab === k ? 'on' : ''}">${n}</button>`).join('')}</div>
      <div class="pb" id="pnBody"></div></div>`;
    $p.querySelector('.x').onclick = closePanel;
    $p.querySelectorAll('[data-t]').forEach(b => b.onclick = () => { tab = b.dataset.t; store.set('bambu_tab', tab); stopCam(); drawPanel(); });
    drawTab();
  }
  window.panelRefresh = function () {
    if (!cur || $p.hidden) return;
    const p = P();
    const l = document.getElementById('pnLive');
    if (p && l) l.innerHTML = liveLine(p);
    if (tab === 'set' || tab === 'ams') {  // these show printer state: redraw unless the user is typing
      const a = document.activeElement;
      if (!a || !$p.contains(a) || !['INPUT', 'SELECT'].includes(a.tagName)) drawTab();
    }
  };
  function drawTab() {
    const body = document.getElementById('pnBody');
    if (!body) return;
    ({ ctl: tabCtl, cam: tabCam, files: tabFiles, ams: tabAms, set: tabSet }[tab] || tabCtl)(body, P());
  }

  // --------------------------------------------------------- control tab --
  function tabCtl(el, p) {
    const s = p.state || {}, g = s.gcode_state, run = ['RUNNING', 'PREPARE'].includes(g), paused = g === 'PAUSE';
    const dual = !!s.noz2;
    el.innerHTML = `
    ${!p.online ? `<div class="err" style="margin-bottom:12px">เครื่องออฟไลน์ — คำสั่งจะไม่ถึงเครื่อง</div>` : ''}
    <div class="sec"><h3>งานพิมพ์</h3><div class="btns">
      <button class="b y" id="cPause" ${run ? '' : 'disabled'}>⏸ หยุดพัก</button>
      <button class="b g" id="cResume" ${paused ? '' : 'disabled'}>▶ พิมพ์ต่อ</button>
      <button class="b r" id="cStop" ${run || paused ? '' : 'disabled'}>⏹ ยกเลิกงาน</button></div></div>
    <div class="sec"><h3>ไฟ และ ความเร็ว</h3><div class="btns">
      <button class="b ${s.light === 1 ? 'on' : ''}" id="cLight">💡 ไฟในเครื่อง ${s.light === 1 ? 'เปิดอยู่' : s.light === 0 ? 'ปิดอยู่' : ''}</button></div>
      <div class="btns" style="margin-top:10px">${[[1, 'Silent'], [2, 'Standard'], [3, 'Sport'], [4, 'Ludicrous']].map(([v, n]) =>
        `<button class="b ${s.spd === v ? 'on' : ''}" data-spd="${v}">${n}</button>`).join('')}</div></div>
    <div class="sec"><h3>อุณหภูมิ</h3>
      ${dual ? `
      <div class="kv2"><span>หัวซ้าย (ตอนนี้ ${Math.round(s.noz?.[0] || 0)}°)</span><input id="tN1" type="number" value="${Math.round(s.noz?.[1] || 0)}"><button class="b" data-temp="N1">ตั้ง</button></div>
      <div class="kv2"><span>หัวขวา (ตอนนี้ ${Math.round(s.noz2?.[0] || 0)}°)</span><input id="tN0" type="number" value="${Math.round(s.noz2?.[1] || 0)}"><button class="b" data-temp="N0">ตั้ง</button></div>`
        : `<div class="kv2"><span>หัวฉีด (ตอนนี้ ${Math.round(s.noz?.[0] || 0)}°)</span><input id="tN" type="number" value="${Math.round(s.noz?.[1] || 0)}"><button class="b" data-temp="N">ตั้ง</button></div>`}
      <div class="kv2"><span>ฐาน (ตอนนี้ ${Math.round(s.bed?.[0] || 0)}°)</span><input id="tB" type="number" value="${Math.round(s.bed?.[1] || 0)}"><button class="b" data-temp="B">ตั้ง</button></div>
      <div class="note">ตั้ง 0 = ปิดฮีตเตอร์</div></div>
    <div class="sec"><h3>พัดลม</h3>
      ${[['P1', 'ชิ้นงาน', s.fan_part], ['P2', 'Aux', s.fan_aux], ['P3', 'Chamber', s.fan_cham]].map(([k, n, v]) =>
        `<div class="rng"><span>${n}</span><input type="range" min="0" max="100" step="10" value="${v ?? 0}" data-fan="${k}"><span id="fv${k}">${v ?? 0}%</span></div>`).join('')}</div>
    <div class="sec"><h3>ขยับแกน ${run || paused ? '<span style="color:var(--yellow)">(ปิดระหว่างพิมพ์)</span>' : ''}</h3>
      <div class="jog">${['X-10', 'X-1', 'X+1', 'X+10', 'Y-10', 'Y-1', 'Y+1', 'Y+10', 'Z-10', 'Z-1', 'Z+1', 'Z+10'].map(m =>
        `<button class="b" data-jog="${m}" ${run || paused ? 'disabled' : ''}>${m}</button>`).join('')}</div>
      <div class="btns" style="margin-top:8px"><button class="b" id="cHome" ${run || paused ? 'disabled' : ''}>🏠 Home ทุกแกน</button></div></div>
    <div class="sec"><h3>G-code (ขั้นสูง)</h3><div class="kv2" style="grid-template-columns:1fr auto">
      <input type="text" id="cG" placeholder="เช่น M106 P1 S255"><button class="b" id="cGo">ส่ง</button></div></div>
    <div class="btns"><button class="b" id="cPush">🔄 ขอข้อมูลล่าสุดจากเครื่อง</button></div>`;
    const q = s2 => el.querySelector(s2);
    q('#cPause').onclick = () => send('หยุดพัก', mqtt({ print: { command: 'pause' } }));
    q('#cResume').onclick = () => send('พิมพ์ต่อ', mqtt({ print: { command: 'resume' } }));
    q('#cStop').onclick = () => confirmDo(`ยกเลิกงานพิมพ์บน ${p.name}? ย้อนกลับไม่ได้`, () => send('ยกเลิกงาน', mqtt({ print: { command: 'stop' } })));
    q('#cLight').onclick = () => send('ไฟ', mqtt({ system: { command: 'ledctrl', led_node: 'chamber_light', led_mode: s.light === 1 ? 'off' : 'on',
      led_on_time: 500, led_off_time: 500, loop_times: 0, interval_time: 0 } }));
    el.querySelectorAll('[data-spd]').forEach(b => b.onclick = () => send('ความเร็ว ' + b.textContent, mqtt({ print: { command: 'print_speed', param: b.dataset.spd } })));
    el.querySelectorAll('[data-temp]').forEach(b => b.onclick = () => {
      const k = b.dataset.temp, v = Math.max(0, Math.min(350, parseInt(q('#t' + k).value || '0', 10)));
      if (k === 'B') return send('ฐาน ' + v + '°', gcode('M140 S' + Math.min(v, 120)));
      if (k === 'N') return send('หัวฉีด ' + v + '°', gcode('M104 S' + v));
      return send((k === 'N1' ? 'หัวซ้าย ' : 'หัวขวา ') + v + '°', gcode(`M104 T${k === 'N1' ? 1 : 0} S${v}`));
    });
    el.querySelectorAll('[data-fan]').forEach(r => {
      r.oninput = () => (q('#fv' + r.dataset.fan).textContent = r.value + '%');
      r.onchange = () => send('พัดลม ' + r.value + '%', gcode(`M106 ${r.dataset.fan} S${Math.round(r.value * 2.55)}`));
    });
    el.querySelectorAll('[data-jog]').forEach(b => b.onclick = () => {
      const m = b.dataset.jog, ax = m[0], d = m.slice(1);
      send('ขยับ ' + m, gcode(`G91\nG0 ${ax}${d} F${ax === 'Z' ? 600 : 3000}\nG90`));
    });
    q('#cHome').onclick = () => send('Home', gcode('G28'));
    q('#cGo').onclick = () => { const g = q('#cG').value.trim(); if (g) confirmDo('ส่ง G-code นี้?\n' + g, () => send('G-code', gcode(g))); };
    q('#cPush').onclick = () => send('ขอข้อมูล', { type: 'pushall' });
  }

  // ---------------------------------------------------------- camera tab --
  function stopCam() { if (camTimer) clearTimeout(camTimer); camTimer = null; camSince = null; }
  function tabCam(el, p) {
    const s = p.state || {}, m = (p.model || '').toUpperCase();
    const jpeg = s.cam === 'jpeg' || /^(A1|P1)/.test(m);
    const hub = data?.bridge?.hub_url;
    if (jpeg) {
      el.innerHTML = `<div class="cam" id="camBox">กำลังเปิดกล้อง… (ใช้เวลา 3-10 วินาที)</div>
        <div class="note">ภาพจากกล้อง ${esc(p.model)} อัปเดตทุก ~2 วินาที ผ่านบอร์ด Bridge (บอร์ดต้องมี PSRAM) — ส่งภาพเฉพาะตอนเปิดหน้านี้</div>`;
      const box = el.querySelector('#camBox');
      let img = null;
      const tick = async () => {
        if (tab !== 'cam' || !cur) return;
        try {
          const r = await rpc('bambu_watch_get', { p_view: key, p_serial: p.serial, p_since: camSince });
          if (r.frame) {
            if (!img) { box.innerHTML = '<img alt=""><span class="ts"></span>'; img = box.querySelector('img'); }
            img.src = 'data:image/jpeg;base64,' + r.frame;
            camSince = r.at;
          }
          const ts = box.querySelector('.ts');
          if (ts && r.at) { const age = Math.round((Date.now() - new Date(r.at)) / 1000); ts.textContent = age <= 5 ? 'สด' : 'ภาพเมื่อ ' + age + ' วินาทีที่แล้ว'; }
        } catch (e) { box.textContent = 'โหลดภาพไม่ได้: ' + e.message; }
        camTimer = setTimeout(tick, 1500);
      };
      tick();
    } else if (hub) {
      el.innerHTML = `<div class="cam"><iframe allow="autoplay; fullscreen" allowfullscreen
          src="${esc(hub)}/stream.html?src=${encodeURIComponent(p.serial)}&mode=mse,hls,mjpeg"></iframe></div>
        <div class="note">วิดีโอสดผ่าน Raspberry Pi (go2rtc + Cloudflare Tunnel)${s.cam === 'rtsp_off' ? '<br><span style="color:var(--yellow)">เครื่องนี้ปิด LAN Liveview อยู่ — เปิดที่จอเครื่อง: Settings → LAN Only Liveview</span>' : ''}</div>`;
    } else {
      el.innerHTML = `<div class="cam">ยังไม่มี Raspberry Pi hub ออนไลน์</div>
        <div class="note">${esc(p.model)} ส่งวิดีโอแบบ RTSP ซึ่ง ESP32 อ่านไม่ได้ — ติดตั้ง Bambu Hub บน Raspberry Pi ตามคู่มือ แล้ววิดีโอจะขึ้นที่นี่</div>`;
    }
  }

  // ----------------------------------------------------------- files tab --
  function trayList(p) {
    const out = [];
    (p.state?.ams || []).forEach(a => (a.trays || []).forEach((t, i) => out.push({ id: a.id * 4 + i, label: `AMS${a.id + 1}-${i + 1}`, t })));
    out.push({ id: 254, label: 'Spool ภายนอก', t: null });
    return out;
  }
  async function loadFiles(p, el) {
    try {
      filesCache = await rpc('bambu_files_get', { p_view: key, p_serial: p.serial });
    } catch (e) { filesCache = { files: [], hub: false, error: e.message }; }
    if (tab === 'files' && cur === p.serial) tabFiles(el, p);
  }
  function tabFiles(el, p) {
    if (!filesCache) { el.innerHTML = '<div class="sub">กำลังโหลด…</div>'; loadFiles(p, el); return; }
    const f = filesCache;
    el.innerHTML = `
    ${!f.hub ? `<div class="err" style="margin-bottom:12px">Raspberry Pi hub ไม่ออนไลน์ — รายการไฟล์ อัปโหลด และสั่งพิมพ์ต้องใช้ hub</div>` : ''}
    <div class="sec"><h3>อัปโหลดไฟล์ใหม่ (.3mf ที่ slice แล้ว / .gcode)</h3>
      <input type="file" id="upF" accept=".3mf,.gcode" style="width:100%">
      <div class="pbar" id="upBar" hidden><i></i></div><div class="note" id="upMsg">ใน Bambu Studio ใช้ "Export plate sliced file" แล้วเลือกไฟล์นี้ — ไฟล์จะถูกส่งไป SD card ของเครื่อง แล้วเลือกพิมพ์ได้</div>
      <div class="btns" style="margin-top:8px"><button class="b g" id="upGo">อัปโหลด</button></div></div>
    <div class="sec"><h3 style="display:flex">ไฟล์ในเครื่อง (${(f.files || []).length})<button class="b" id="fRef" style="margin-left:auto;padding:5px 10px;font-size:13px">🔄 อ่านใหม่</button></h3>
      ${(f.files || []).length ? f.files.map((x, i) => `<div class="fl"><div><div class="nm">${esc(x.path)}</div>
        <div class="sub">${x.size ? (x.size / 1048576).toFixed(1) + ' MB' : ''} ${esc(x.mtime || '')} ${x.info?.plates?.length ? '· ' + x.info.plates.length + ' plate' : ''}</div></div>
        <div class="btns"><button class="b g" data-print="${i}">พิมพ์</button><button class="b" data-del="${i}" title="ลบ">🗑</button></div></div>`).join('')
        : '<div class="sub">ยังไม่มีรายการ — กด "อ่านใหม่"</div>'}</div>`;
    const q = s2 => el.querySelector(s2);
    q('#fRef').onclick = async () => { const r = await send('อ่านรายการไฟล์', { op: 'list' }, 'hub', { wait: 60000 }); if (r) { filesCache = null; tabFiles(el, p); } };
    el.querySelectorAll('[data-del]').forEach(b => b.onclick = () => {
      const x = f.files[+b.dataset.del];
      confirmDo('ลบไฟล์ ' + x.path + ' ออกจากเครื่อง?', async () => { const r = await send('ลบไฟล์', { op: 'delete', path: x.path }, 'hub', { wait: 60000 }); if (r) { filesCache = null; tabFiles(el, p); } });
    });
    el.querySelectorAll('[data-print]').forEach(b => b.onclick = () => printDialog(el, p, f.files[+b.dataset.print]));
    q('#upGo').onclick = () => upload(el, p);
  }
  async function upload(el, p) {
    const file = el.querySelector('#upF').files[0];
    if (!file) return toast('เลือกไฟล์ก่อน', 'er');
    if (!/\.(3mf|gcode)$/i.test(file.name)) return toast('รองรับ .3mf / .gcode', 'er');
    if (!needCtl()) return;
    const bar = el.querySelector('#upBar'), msg = el.querySelector('#upMsg');
    bar.hidden = false;
    msg.textContent = 'ขอที่อัปโหลด…';
    try {
      const r = await fetch(CLOUD + '/functions/v1/bambu-files', {
        method: 'POST', headers: { apikey: ANON, Authorization: 'Bearer ' + ANON, 'Content-Type': 'application/json' },
        body: JSON.stringify({ action: 'upload_url', key: ctl, name: file.name })
      });
      const u = await r.json();
      if (!r.ok) throw new Error(u.error || r.status);
      await new Promise((ok, bad) => {
        const x = new XMLHttpRequest();
        x.open('PUT', u.url);
        x.setRequestHeader('Content-Type', 'application/octet-stream');
        x.upload.onprogress = e => { if (e.lengthComputable) { bar.firstChild.style.width = (e.loaded / e.total * 100) + '%'; msg.textContent = `ส่งขึ้น cloud ${Math.round(e.loaded / e.total * 100)}%`; } };
        x.onload = () => (x.status < 300 ? ok() : bad(new Error('upload ' + x.status + ' ' + x.responseText)));
        x.onerror = () => bad(new Error('network'));
        x.send(file);
      });
      msg.textContent = 'Raspberry Pi กำลังส่งไฟล์เข้าเครื่องพิมพ์…';
      const s = await send('ส่งไฟล์เข้าเครื่อง', { op: 'upload', storage_path: u.path, name: file.name }, 'hub', { wait: 300000 });
      if (s?.status === 'ok') {
        msg.textContent = 'เสร็จแล้ว';
        filesCache = null;
        tabFiles(el, p);
        const path = s.result?.path;
        if (path) printDialog(el, p, { path, info: s.result?.info });
      } else msg.textContent = 'ส่งเข้าเครื่องไม่สำเร็จ';
    } catch (e) {
      msg.textContent = 'อัปโหลดไม่สำเร็จ: ' + e.message;
      if (String(e.message).includes('control key')) { ctl = ''; store.set('bambu_ctl_key', ''); }
    }
  }
  async function printDialog(el, p, file) {
    let info = file.info;
    if (!info && /\.3mf$/i.test(file.path)) {
      el.innerHTML = '<div class="sub">กำลังอ่านข้อมูล plate / filament จากไฟล์ (Raspberry Pi)…</div>';
      const r = await send('อ่านข้อมูลไฟล์', { op: 'inspect', path: file.path, size: file.size, mtime: file.mtime }, 'hub', { wait: 120000 });
      info = r?.result?.info || { plates: [] };
    }
    const plates = info?.plates?.length ? info.plates : [{ index: 1, filaments: [] }];
    const trays = trayList(p);
    const draw = (pi) => {
      const pl = plates[pi] || plates[0];
      const fils = pl.filaments || [];
      const best = f => {  // same type + closest colour, else first tray of same type
        const c = (f.color || '').replace('#', '').slice(0, 6).toUpperCase();
        const same = trays.filter(t => t.t && t.t.t && t.t.t.toUpperCase() === (f.type || '').toUpperCase());
        return (same.find(t => (t.t.c || '').toUpperCase().slice(0, 6) === c) || same[0] || trays[0])?.id ?? 254;
      };
      el.innerHTML = `<div class="sec"><h3>สั่งพิมพ์: ${esc(file.path)}</h3>
        ${plates.length > 1 ? `<label class="sub">Plate</label><select id="pPl">${plates.map((x, i) => `<option value="${i}" ${i === pi ? 'selected' : ''}>Plate ${x.index}${x.prediction_s ? ' · ' + dur(Math.round(x.prediction_s / 60)) : ''}</option>`).join('')}</select>` : ''}
        ${pl.prediction_s ? `<div class="note">ใช้เวลาประมาณ ${dur(Math.round(pl.prediction_s / 60))}${pl.weight_g ? ' · ' + pl.weight_g + ' g' : ''}</div>` : ''}
        <h3 style="margin-top:12px">จับคู่ filament → ช่อง AMS</h3>
        ${fils.length ? fils.map(f => `<div class="mapr"><span class="dot2" style="background:${esc(f.color || '#888')}"></span>
          <span>${esc(f.type || '?')} #${f.id}</span><select data-fil="${f.id}">${trays.map(t => `<option value="${t.id}" ${t.id === best(f) ? 'selected' : ''}>${t.label}${t.t ? ' · ' + esc(t.t.t) : t.id === 254 ? '' : ' · ว่าง'}</option>`).join('')}</select></div>`).join('')
          : `<div class="note">ไม่มีข้อมูล filament ในไฟล์ (ไฟล์ .gcode หรือยังไม่ได้ slice) — <label><input type="checkbox" id="pNoAms"> ไม่ใช้ AMS (spool ภายนอก)</label></div>`}
        <h3 style="margin-top:12px">ตัวเลือก</h3>
        <div class="tg"><span>Bed leveling</span><input type="checkbox" id="oLev" checked></div>
        <div class="tg"><span>Timelapse</span><input type="checkbox" id="oTl"></div>
        <div class="tg"><span>Flow calibration</span><input type="checkbox" id="oFlow"></div>
        <div class="tg"><span>Vibration calibration</span><input type="checkbox" id="oVib"></div>
        <div class="tg"><span>รูปแบบที่อยู่ไฟล์ <small>เปลี่ยนเฉพาะถ้าเครื่องไม่เริ่มพิมพ์</small></span><select id="oUrl" style="width:140px"><option value="auto">อัตโนมัติ</option><option value="sdcard">file:///sdcard</option><option value="ftp">ftp://</option></select></div>
        <div class="btns" style="margin-top:12px"><button class="b g" id="pGo">▶ เริ่มพิมพ์</button><button class="b" id="pBack">ยกเลิก</button></div>
        ${isBusy(p) ? '<div class="err" style="margin-top:10px">เครื่องกำลังพิมพ์อยู่ — ต้องรอให้เสร็จก่อน</div>' : ''}</div>`;
      const q = s2 => el.querySelector(s2);
      if (q('#pPl')) q('#pPl').onchange = () => draw(+q('#pPl').value);
      q('#pBack').onclick = () => tabFiles(el, p);
      q('#pGo').onclick = () => {
        let mapping = null;
        if (fils.length) {
          const maxId = Math.max(...fils.map(f => f.id));
          mapping = Array(maxId).fill(-1);
          el.querySelectorAll('[data-fil]').forEach(s3 => (mapping[+s3.dataset.fil - 1] = +s3.value));
          if (mapping.every(x => x === 254)) mapping = null;
        }
        const opts = { bed_leveling: q('#oLev').checked, timelapse: q('#oTl').checked, flow_cali: q('#oFlow').checked,
          vibration_cali: q('#oVib').checked, url_style: q('#oUrl').value };
        confirmDo(`เริ่มพิมพ์ ${file.path} (plate ${pl.index}) บน ${p.name}?\nตรวจว่าฐานพิมพ์ว่างแล้ว`, async () => {
          const r = await send('สั่งพิมพ์', { op: 'print', path: file.path, plate: pl.index, ams_mapping: mapping, opts }, 'hub', { wait: 60000 });
          if (r?.status === 'ok') { tab = 'ctl'; drawPanel(); }
        });
      };
    };
    draw(0);
  }

  // ------------------------------------------------------------- AMS tab --
  const PRESETS = { PLA: ['GFL99', 190, 230], 'PLA-CF': ['GFL98', 210, 240], PETG: ['GFG99', 220, 260], ABS: ['GFB99', 240, 270],
    ASA: ['GFB98', 240, 270], TPU: ['GFU99', 200, 250], PA: ['GFN99', 260, 290], PC: ['GFC99', 260, 280], PVA: ['GFS99', 190, 210] };
  function tabAms(el, p) {
    const s = p.state || {}, ams = s.ams || [];
    if (!ams.length) { el.innerHTML = '<div class="sub">เครื่องนี้ไม่มี AMS (หรือยังไม่ได้รับข้อมูล)</div>'; return; }
    const run = isBusy(p);
    el.innerHTML = ams.map(a => `<div class="sec"><h3>AMS ${a.id + 1} ${a.hum != null ? '· ความชื้น ' + a.hum + '%' : a.lvl != null ? '· ความชื้นระดับ ' + a.lvl : ''}${a.temp != null ? ' · ' + a.temp + '°C' : ''}${a.dry ? ' · <span style="color:var(--orange)">กำลังอบ เหลือ ' + dur(a.dry) + '</span>' : ''}</h3>
      ${(a.trays || []).map((t, i) => { const id = a.id * 4 + i, act = s.tray_now === id;
        return `<div class="slot"><div class="sw" style="background:${t ? '#' + esc(t.c) : 'transparent'};color:${t && light(t.c) ? '#000' : '#fff'};${act ? 'outline:2px solid var(--text);outline-offset:2px' : ''}">${t ? esc(t.t) : 'ว่าง'}</div>
        <div><div>ช่อง ${i + 1} ${act ? '<b style="color:var(--bambu)">· ใช้อยู่</b>' : ''}</div><div class="sub">${t ? (t.r != null ? 'เหลือ ' + t.r + '%' : '') : 'ไม่มี filament'}</div></div>
        <div class="btns"><button class="b" data-load="${id}" ${run || act || !t ? 'disabled' : ''}>โหลด</button><button class="b" data-edit="${a.id}:${i}">แก้</button></div></div>
        <div id="ed${id}"></div>`; }).join('')}</div>`).join('') +
      `<div class="btns"><button class="b" id="aUnload" ${run || s.tray_now >= 254 ? 'disabled' : ''}>⏏ ถอด filament ที่ใช้อยู่</button></div>
      <div class="note">การอบ AMS (AMS 2 Pro / HT) ยังสั่งจากที่นี่ไม่ได้</div>`;
    el.querySelectorAll('[data-load]').forEach(b => b.onclick = () => {
      const id = +b.dataset.load, t = ams.find(a => a.id === (id >> 2))?.trays?.[id & 3];
      const temp = t?.hi && t?.lo ? Math.round((t.hi + t.lo) / 2) : (PRESETS[(t?.t || '').toUpperCase()] ? Math.round((PRESETS[t.t.toUpperCase()][1] + PRESETS[t.t.toUpperCase()][2]) / 2) : 220);
      send('โหลด filament', mqtt({ print: { command: 'ams_change_filament', target: id, curr_temp: temp, tar_temp: temp } }), 'bridge', { wait: 60000 });
    });
    const un = el.querySelector('#aUnload');
    if (un) un.onclick = () => send('ถอด filament', mqtt({ print: { command: 'ams_change_filament', target: 255, curr_temp: 220, tar_temp: 220 } }), 'bridge', { wait: 60000 });
    el.querySelectorAll('[data-edit]').forEach(b => b.onclick = () => {
      const [aid, slot] = b.dataset.edit.split(':').map(Number), id = aid * 4 + slot;
      const t = ams.find(a => a.id === aid)?.trays?.[slot];
      const box = el.querySelector('#ed' + id);
      box.innerHTML = `<div class="sec" style="margin-top:6px"><div class="kv2" style="grid-template-columns:1fr 1fr auto">
        <select id="eT${id}">${Object.keys(PRESETS).map(k => `<option ${t && t.t && t.t.toUpperCase() === k ? 'selected' : ''}>${k}</option>`).join('')}</select>
        <input type="color" id="eC${id}" value="#${(t?.c || 'FFFFFF').slice(0, 6)}" style="height:40px;padding:2px">
        <button class="b g" id="eS${id}">บันทึก</button></div></div>`;
      box.querySelector('#eS' + id).onclick = () => {
        const type = box.querySelector('#eT' + id).value, col = box.querySelector('#eC' + id).value.replace('#', '').toUpperCase() + 'FF';
        const [idx, lo, hi] = PRESETS[type];
        send('ตั้ง filament ' + type, mqtt({ print: { command: 'ams_filament_setting', ams_id: aid, tray_id: slot, tray_info_idx: idx,
          tray_color: col, nozzle_temp_min: lo, nozzle_temp_max: hi, tray_type: type } }));
      };
    });
  }

  // -------------------------------------------------------- settings tab --
  function tabSet(el, p) {
    const x = p.state?.xcam || {};
    const row = (k, name, sub, val) => `<div class="tg"><span>${name}<small>${sub}</small></span><div class="btns">
      <button class="b ${val === true || val === 1 ? 'on' : ''}" data-k="${k}" data-v="1">เปิด</button>
      <button class="b ${val === false || val === 0 ? 'on' : ''}" data-k="${k}" data-v="0">ปิด</button></div></div>`;
    el.innerHTML = `<div class="sec"><h3>AI / กล้อง</h3>
      ${row('spaghetti_detector', 'ตรวจจับ spaghetti', 'หยุดพิมพ์อัตโนมัติเมื่อเจอเส้นยุ่ง', x.spaghetti)}
      ${row('first_layer_inspector', 'ตรวจชั้นแรก', 'เฉพาะรุ่นที่มี lidar / กล้อง AI', x.first_layer)}
      ${row('buildplate_marker_detector', 'ตรวจชนิดฐานพิมพ์', 'เตือนถ้าใส่ฐานผิด', x.plate_marker)}
      <div class="tg"><span>ความไวการหยุดพิมพ์</span><select id="sSens" style="width:130px">${['low', 'medium', 'high'].map(v => `<option ${x.sens === v ? 'selected' : ''}>${v}</option>`).join('')}</select></div></div>
      <div class="sec"><h3>ตัวเลือกการพิมพ์</h3>
      ${row('auto_recovery', 'กู้งานเมื่อ step หลุด', 'auto recovery', null)}
      ${row('sound_enable', 'เสียงแจ้งเตือน', 'เฉพาะบางรุ่น', null)}
      ${row('filament_tangle_detect', 'ตรวจ filament พันกัน', '', null)}
      ${row('auto_switch_filament', 'สลับ spool อัตโนมัติเมื่อหมด', 'AMS auto refill', null)}
      <div class="note">ช่องที่ไม่มีไฮไลต์ = เครื่องไม่ได้รายงานค่าปัจจุบัน กดเลือกได้ตามต้องการ</div></div>`;
    const XC = ['spaghetti_detector', 'first_layer_inspector', 'buildplate_marker_detector'];
    el.querySelectorAll('[data-k]').forEach(b => b.onclick = () => {
      const k = b.dataset.k, on = b.dataset.v === '1';
      if (XC.includes(k)) send(b.closest('.tg').firstChild.firstChild.textContent + (on ? ' เปิด' : ' ปิด'),
        mqtt({ xcam: { command: 'xcam_control_set', module_name: k, control: on, enable: on, print_halt: true,
          halt_print_sensitivity: el.querySelector('#sSens').value } }));
      else send(b.closest('.tg').firstChild.firstChild.textContent + (on ? ' เปิด' : ' ปิด'), mqtt({ print: { command: 'print_option', [k]: on } }));
    });
    el.querySelector('#sSens').onchange = e => send('ความไว ' + e.target.value, mqtt({ xcam: { command: 'xcam_control_set', module_name: 'spaghetti_detector',
      control: x.spaghetti !== 0 && x.spaghetti !== false, enable: x.spaghetti !== 0 && x.spaghetti !== false, print_halt: true, halt_print_sensitivity: e.target.value } }));
  }
})();
