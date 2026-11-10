// BattLab Cloud: inverters of other brands, read from the brand's own cloud.
//   smartess : SmartESS / DessMonitor (PowMr, Easun, MPP Solar, Must, Fronus ... with an Eybond Wi-Fi collector)
//   deye     : Deye Cloud developer API (AppId + AppSecret of the customer's own app)
//   solis    : SolisCloud API (KeyID + KeySecret)
//   solarman : Solarman OpenAPI (Sofar and other brands on a Solarman logger)
// Runs every minute from pg_cron (x-cron-secret) for the accounts whose turn it is,
// or on demand for the signed-in user ({brand}). Read only: nothing is ever written to an inverter.
import { createClient } from "npm:@supabase/supabase-js@2.45.4";
import { createHash, createHmac } from "node:crypto";

const sb = createClient(Deno.env.get("SUPABASE_URL")!, Deno.env.get("SUPABASE_SERVICE_ROLE_KEY")!, { auth: { persistSession: false } });
const CORS = { "Access-Control-Allow-Origin": "*", "Access-Control-Allow-Headers": "authorization, x-client-info, apikey, content-type, x-cron-secret" };
const LABEL: Record<string, string> = { smartess: "SmartESS", deye: "Deye", solis: "Solis", solarman: "Solarman" };

type Acc = { id: string; owner: string; brand: string; username: string; password: string; app_id: string | null; app_secret: string | null;
  server: string | null; token: string | null; token_secret: string | null; token_exp: string | null };
type Item = { sn: string; model: string; data: Record<string, unknown> };
type Res = { items: Item[]; server?: string; debug?: unknown };

const sha1 = (s: string) => createHash("sha1").update(s, "utf8").digest("hex");
const sha256 = (s: string) => createHash("sha256").update(s, "utf8").digest("hex");
const norm = (s: unknown) => String(s ?? "").toLowerCase().replace(/[^a-z0-9]/g, "");
const num = (x: unknown): number | null => {
  if (typeof x === "number") return isFinite(x) ? x : null;
  if (typeof x === "string" && x.trim() !== "" && isFinite(+x)) return +x;
  return null;
};
const r1 = (x: number | null, d = 1) => (x == null ? null : +x.toFixed(d));
// value + unit -> W (cloud APIs mix W and kW)
const watts = (v: unknown, unit: unknown): number | null => {
  const n = num(v); if (n == null) return null;
  const u = String(unit ?? "").trim().toLowerCase();
  return u === "kw" ? Math.round(n * 1000) : u === "mw" ? Math.round(n * 1e6) : Math.round(n);
};

async function http(url: string, init: RequestInit = {}, ms = 12000): Promise<{ status: number; json: any; text: string }> {
  const ctl = new AbortController();
  const t = setTimeout(() => ctl.abort(), ms);
  try {
    const r = await fetch(url, { ...init, signal: ctl.signal });
    const text = await r.text();
    let json: any = null;
    try { json = JSON.parse(text); } catch { /* not json */ }
    return { status: r.status, json, text: text.slice(0, 300) };
  } finally { clearTimeout(t); }
}
const postJson = (url: string, body: unknown, headers: Record<string, string> = {}) =>
  http(url, { method: "POST", headers: { "Content-Type": "application/json", ...headers }, body: JSON.stringify(body) });

// shared card shape (same keys as the LuxPower cards the website already draws)
function card(brand: string, model: string, live: boolean, f: Record<string, number | string | null | undefined>) {
  const v = num(f.v), p = num(f.p);
  const d: Record<string, unknown> = {
    inv: 1, src: "cloud", fw: brand, bms: LABEL[brand] ?? brand, state: live ? "LIVE" : "OFFLINE", model, chg: true, dsg: true,
    status: f.status ?? "", soc: r1(num(f.soc)), v: r1(v, 2), p: p == null ? null : Math.round(p),
    i: num(f.i) != null ? r1(num(f.i), 2) : (v && p != null ? r1(p / v, 2) : null),
    ppv: num(f.ppv), pload: num(f.pload), pgrid: num(f.pgrid), vac: r1(num(f.vac)), vpv1: r1(num(f.vpv1)),
    t1: r1(num(f.t1)), mos: r1(num(f.mos)),
    epv: r1(num(f.epv), 2), ein: r1(num(f.ein), 2), eout: r1(num(f.eout), 2), eload: r1(num(f.eload), 2), egrid: r1(num(f.egrid), 2),
  };
  if (f.pgrid_est) d.pgrid_est = 1;
  for (const k of Object.keys(d)) if (d[k] === null || d[k] === undefined) delete d[k];
  return d;
}

// lookup over a list of {name/key, value, unit}: first candidate that exists wins
class KV {
  m = new Map<string, { v: unknown; u: unknown }>();
  add(name: unknown, v: unknown, u?: unknown) { const k = norm(name); if (k && !this.m.has(k)) this.m.set(k, { v, u }); }
  n(...names: string[]): number | null { for (const k of names) { const e = this.m.get(k); if (e && num(e.v) != null) return num(e.v); } return null; }
  w(...names: string[]): number | null { for (const k of names) { const e = this.m.get(k); if (e && num(e.v) != null) return watts(e.v, e.u); } return null; }
  s(...names: string[]): string | null { for (const k of names) { const e = this.m.get(k); if (e && e.v != null && String(e.v) !== "") return String(e.v); } return null; }
  sumW(...names: string[]): number | null { let t: number | null = null; for (const k of names) { const x = this.w(k); if (x != null) t = (t ?? 0) + x; } return t; }
  dump(max = 140) { return [...this.m.entries()].slice(0, max).map(([k, e]) => [k, e.v, e.u ?? ""]); }
}

// ---------------------------------------------------------------- SmartESS / DessMonitor
const DESS = "https://api.dessmonitor.com/public/";
const qp = (s: unknown) => encodeURIComponent(String(s)).replace(/[!'()*]/g, (c) => "%" + c.charCodeAt(0).toString(16).toUpperCase()).replace(/%20/g, "+");
const qs = (o: Record<string, unknown>) => Object.entries(o).map(([k, v]) => `&${k}=${qp(v)}`).join("");

async function dessAuth(a: Acc) {
  const salt = Date.now().toString();
  const act = "&action=authSource" + qs({ usr: a.username, "company-key": "bnrl_frRFjEz8Mkn", source: 1, _app_client_: "web", _app_id_: "battlab", _app_version_: "1.0" });
  const sign = sha1(salt + sha1(a.password) + act);
  const r = await http(`${DESS}?sign=${sign}&salt=${salt}${act}`);
  const j = r.json;
  if (!j || j.err !== 0 || !j.dat?.token) throw new Error("SmartESS login: " + (j?.desc || ("HTTP " + r.status)));
  const exp = new Date(Date.now() + Math.max(600, (num(j.dat.expire) ?? 3600) - 600) * 1000).toISOString();
  await sb.rpc("inv_token", { p_id: a.id, p_token: j.dat.token, p_secret: j.dat.secret, p_exp: exp });
  return { token: String(j.dat.token), secret: String(j.dat.secret) };
}
async function smartess(a: Acc): Promise<Res> {
  let tk = a.token && a.token_secret && a.token_exp && Date.parse(a.token_exp) > Date.now() ? { token: a.token, secret: a.token_secret } : await dessAuth(a);
  const call = async (action: string, params: Record<string, unknown> = {}, retry = true): Promise<any> => {
    const salt = Date.now().toString();
    const act = `&action=${action}` + qs(params);
    const sign = sha1(salt + tk.secret + tk.token + act);
    const r = await http(`${DESS}?sign=${sign}&salt=${salt}&token=${tk.token}${act}`);
    const j = r.json;
    if (j && j.err === 0) return j.dat;
    // token no longer valid: sign in once more
    if (retry && j && [0x0002, 0x0004, 0x0005, 0x0010, 0x0011].includes(j.err) || (retry && /TOKEN|SIGN|AUTH/i.test(String(j?.desc ?? "")))) { tk = await dessAuth(a); return call(action, params, false); }
    if (j && (j.err === 0x0101 || /NO_RECORD|NOT_FOUND/i.test(String(j.desc ?? "")))) return null;
    throw new Error(`SmartESS ${action}: ${j?.desc || ("HTTP " + r.status)}`);
  };
  const devs: any[] = [];
  const plants = (await call("queryPlants", { pagesize: 50 }))?.plant ?? [];
  for (const pl of plants.slice(0, 6)) {
    const d = await call("webQueryDeviceEs", { pid: pl.pid, page: 0, pagesize: 50 });
    for (const x of d?.device ?? []) devs.push(x);
  }
  if (!devs.length) { const d = await call("webQueryDeviceEs", { page: 0, pagesize: 50 }); for (const x of d?.device ?? []) devs.push(x); }
  const items: Item[] = []; let debug: unknown = null;
  for (const dv of devs.slice(0, 8)) {
    const sn = String(dv.sn ?? "");
    if (!sn || dv.pn == null || dv.devcode == null) continue;
    const dat = await call("queryDeviceLastData", { pn: dv.pn, devcode: dv.devcode, devaddr: dv.devaddr ?? 1, sn, i18n: "en" });
    const kv = new KV();
    const rows: any[] = Array.isArray(dat) ? dat : Array.isArray(dat?.pars) ? dat.pars : dat?.pars && typeof dat.pars === "object" ? Object.values(dat.pars).flat() as any[] : [];
    for (const x of rows) if (x && typeof x === "object") kv.add(x.title ?? x.par ?? x.name ?? x.id, x.val ?? x.value, x.unit);
    const v = kv.n("batteryvoltage", "batteryvoltagefromscc");
    const ichg = kv.n("batterychargingcurrent", "batterychargecurrent"), idis = kv.n("batterydischargecurrent", "batterydischargingcurrent");
    const i = ichg != null || idis != null ? (ichg ?? 0) - (idis ?? 0) : kv.n("batterycurrent");
    const p = v != null && i != null ? v * i : kv.w("batterypower");
    const ppv = kv.w("pvpower", "totalpvpower", "pvtotalpower", "pvinputpower", "totalpvchargingpower", "pvtotalchargerpower", "pvchargepower", "pvchargingpower")
      ?? kv.sumW("pv1chargerpower", "pv2chargerpower") ?? kv.sumW("pv1inputpower", "pv2inputpower") ?? kv.sumW("pv1chargingpower", "pv2chargingpower") ?? kv.sumW("pv1power", "pv2power");
    const pload = kv.w("outputactivepower", "acoutputactivepower", "totaloutputactivepower", "loadactivepower", "loadpower", "totalacoutputactivepower");
    const vgrid = kv.n("gridvoltage", "acinputvoltage", "mainsvoltage", "gridrvoltage");
    let pgrid = kv.w("gridpower", "gridactivepower", "mainspower"); let est = false;
    if (pgrid == null && pload != null) { est = true; pgrid = vgrid != null && vgrid > 100 ? Math.max(0, Math.round(pload + (p ?? 0) - (ppv ?? 0))) : 0; }
    const mode = kv.s("operatingmode", "workingstate", "workstate", "workingmode", "outputmode", "batterymode", "gridmode");
    const live = num(dv.status) !== 1 && rows.length > 0;
    const model = String(dv.devalias || dv.alias || "").trim() || "SmartESS";
    items.push({ sn, model, data: card("smartess", model, live, {
      status: mode, soc: kv.n("batterycapacity", "soc", "stateofcharge", "batterypercentage", "batterysoc", "battery"), v, i, p, ppv, pload, pgrid, pgrid_est: est ? 1 : null,
      vac: kv.n("outputvoltage", "acoutputvoltage", "invertervoltage"), vpv1: kv.n("pv1voltage", "pvvoltage", "pv1inputvoltage", "pvinputvoltage"),
      mos: kv.n("inverterheatsinktemperature", "invmoduletermperature", "invmoduletemperature", "dcmoduletermperature", "radiatortemperature", "pvradiatortemperature", "temperature"),
      epv: num(dv.energyToday) ?? kv.n("energytoday", "pvenergytoday", "todaygeneration", "dailygeneration"),
    }) });
    if (!debug) debug = { devcode: dv.devcode, status: dv.status, rows: kv.dump() };
  }
  return { items, server: "api.dessmonitor.com", debug };
}

// ---------------------------------------------------------------- Deye Cloud / Solarman OpenAPI (same API family)
// map the realtime station record + the inverter's own data points to one card
function flow(brand: string, model: string, live: boolean, st: any, kv: KV) {
  const sW = (k: string) => (st && num(st[k]) != null ? Math.round(num(st[k])!) : null);
  const chg = sW("chargePower"), dis = sW("dischargePower");
  const bp = kv.w("batterypower", "bp1", "batterypowerbms");
  // station record: chargePower / dischargePower are unsigned; device BatteryPower is + when discharging
  const p = chg != null || dis != null ? (chg ?? 0) - (dis ?? 0) : bp != null ? -bp : sW("batteryPower") != null ? -sW("batteryPower")! : null;
  const imp = sW("purchasePower"), exp = sW("gridPower");
  const pg = kv.w("totalgridpower", "gridpower", "pgpt1", "meterpower", "totalactivepowerofgrid");
  const pgrid = pg ?? (imp != null || exp != null ? (imp ?? 0) - (exp ?? 0) : sW("wirePower"));
  return card(brand, model, live, {
    status: kv.s("runningstatus", "inverterstatus", "workmode", "st1") ?? "",
    soc: kv.n("soc", "batterysoc", "bleftcap1", "bmssoc") ?? (st ? num(st.batterySOC ?? st.batterySoc) : null),
    v: kv.n("batteryvoltage", "bv1", "bmsvoltage"), i: null, p,
    ppv: kv.w("totalsolarpower", "totaldcinputpower", "totalpvpower", "dpit1", "pvpower") ?? kv.sumW("dcpowerpv1", "dcpowerpv2", "dcpowerpv3", "dcpowerpv4") ?? sW("generationPower"),
    pload: kv.w("totalconsumptionpower", "totalloadpower", "loadpower", "epuset1", "consumptionpower") ?? sW("consumptionPower") ?? sW("usePower"),
    pgrid,
    vac: kv.n("acvoltagerua", "gridvoltagel1", "acvoltage1", "av1", "gridvoltage", "loadvoltagel1"), vpv1: kv.n("dcvoltagepv1", "dv1"),
    t1: kv.n("batterytemperature", "temperaturebattery", "bt1"), mos: kv.n("actemperature", "dctemperature", "invt0", "temperatureinverter", "actradiatort"),
    epv: kv.n("dailyactiveproduction", "dailyproductionactive", "dailyproduction", "etdyge1", "dailygeneration"),
    eload: kv.n("dailyconsumption", "dailyloadconsumption", "etdyuse1"),
    ein: kv.n("dailychargingenergy", "dailybatterycharge", "etdycg1"), eout: kv.n("dailydischargingenergy", "dailybatterydischarge", "etdydcg1"),
    egrid: kv.n("dailyenergypurchased", "dailyenergybuy", "dailygridimport", "dailyenergyimport", "etdypu1"),
  });
}

async function openApi(a: Acc, brand: "deye" | "solarman"): Promise<Res> {
  if (!a.app_id || !a.app_secret) throw new Error("AppId / AppSecret missing");
  const bases = brand === "deye"
    ? ["https://eu1-developer.deyecloud.com/v1.0", "https://us1-developer.deyecloud.com/v1.0"]
    : ["https://globalapi.solarmanpv.com", "https://api.solarmanpv.com"];
  const P = brand === "deye"
    ? { token: "/account/token", list: "/station/list", latest: "/station/latest", devs: "/station/device", data: "/device/latest" }
    : { token: "/account/v1.0/token", list: "/station/v1.0/list", latest: "/station/v1.0/realTime", devs: "/station/v1.0/device", data: "/device/v1.0/currentData" };
  const order = a.server && bases.includes(a.server) ? [a.server, ...bases.filter((b) => b !== a.server)] : a.server ? [a.server, ...bases] : bases;
  let base = "", token = a.token && a.token_exp && Date.parse(a.token_exp) > Date.now() && a.server ? a.token : "";
  if (token) base = a.server!;
  const signIn = async () => {
    let last = "no server answered";
    for (const b of order) {
      const who = a.username.includes("@") ? { email: a.username } : brand === "deye" ? { username: a.username } : { email: a.username };
      const r = await postJson(`${b}${P.token}?appId=${encodeURIComponent(a.app_id!)}&language=en`, { appSecret: a.app_secret, ...who, password: sha256(a.password) }).catch((e) => ({ status: 0, json: null, text: String(e) }));
      const j = r.json; const t = j?.accessToken ?? j?.access_token;
      if (t) {
        base = b; token = String(t);
        const sec = num(j.expiresIn ?? j.expires_in) ?? 86400;
        await sb.rpc("inv_token", { p_id: a.id, p_token: token, p_secret: null, p_exp: new Date(Date.now() + Math.max(600, Math.min(sec, 20 * 86400) - 600) * 1000).toISOString() });
        return;
      }
      last = j?.msg || j?.message || ("HTTP " + r.status);
    }
    throw new Error(LABEL[brand] + " login: " + last);
  };
  if (!token) await signIn();
  const call = async (path: string, body: unknown, retry = true): Promise<any> => {
    const r = await postJson(`${base}${path}${brand === "solarman" ? "?language=en" : ""}`, body, { Authorization: (brand === "deye" ? "Bearer " : "bearer ") + token });
    const j = r.json;
    if (retry && (r.status === 401 || /token|auth/i.test(String(j?.msg ?? "")) && j?.success === false)) { token = ""; await signIn(); return call(path, body, false); }
    if (!j || j.success === false) throw new Error(`${LABEL[brand]} ${path}: ${j?.msg || ("HTTP " + r.status)}`);
    return j;
  };
  const stations: any[] = (await call(P.list, { page: 1, size: 20 })).stationList ?? [];
  const items: Item[] = []; let debug: unknown = null;
  for (const s of stations.slice(0, 6)) {
    const sid = s.id ?? s.stationId; if (sid == null) continue;
    const st = await call(P.latest, { stationId: sid }).catch(() => null);
    const dl = await call(P.devs, brand === "deye" ? { page: 1, size: 50, stationIds: [sid] } : { page: 1, size: 50, stationId: sid }).catch(() => null);
    const invs: any[] = (dl?.deviceListItems ?? []).filter((d: any) => /INVERTER/i.test(String(d.deviceType ?? "")) && d.deviceSn);
    const one = invs.length <= 1;
    for (const dv of invs.slice(0, 6)) {
      const sn = String(dv.deviceSn);
      const kv = new KV();
      try {
        const dj = brand === "deye" ? await call(P.data, { deviceList: [sn] }) : await call(P.data, { deviceSn: sn });
        const rows: any[] = brand === "deye" ? (dj.deviceDataList?.[0]?.dataList ?? []) : (dj.dataList ?? []);
        for (const x of rows) { kv.add(x.key, x.value, x.unit); kv.add(x.name, x.value, x.unit); }
      } catch { /* station record alone still gives the power flow */ }
      const live = !/OFFLINE|ALL_OFFLINE/i.test(String(dv.connectStatus ?? s.connectionStatus ?? "")) && num(dv.connectStatus) !== 0;
      const model = String(s.name ?? "").trim().slice(0, 30) || LABEL[brand];
      items.push({ sn, model, data: flow(brand, model, live, one ? st : null, kv) });
      if (!debug) debug = { station: st ? Object.fromEntries(Object.entries(st).filter(([, v]) => typeof v !== "object").slice(0, 40)) : null, rows: kv.dump() };
    }
    // a station without a listed inverter still has a power flow: show it as one card
    if (!invs.length && st && num(st.generationPower) != null) {
      const model = String(s.name ?? "").trim().slice(0, 30) || LABEL[brand];
      items.push({ sn: `${brand === "deye" ? "DY" : "SM"}-${sid}`, model, data: flow(brand, model, true, st, new KV()) });
    }
  }
  return { items, server: base, debug };
}

// ---------------------------------------------------------------- SolisCloud
async function solis(a: Acc): Promise<Res> {
  const base = a.server || "https://www.soliscloud.com:13333";
  const call = async (path: string, body: unknown) => {
    const raw = JSON.stringify(body);
    const md5 = createHash("md5").update(raw, "utf8").digest("base64");
    const date = new Date().toUTCString();
    const sign = createHmac("sha1", a.password).update(`POST\n${md5}\napplication/json\n${date}\n${path}`, "utf8").digest("base64");
    const r = await http(base + path, { method: "POST", body: raw, headers: { "Content-MD5": md5, "Content-Type": "application/json", "Date": date, "Authorization": `API ${a.username}:${sign}` } });
    const j = r.json;
    if (!j || String(j.code) !== "0") throw new Error(`Solis ${path}: ${j?.msg || ("HTTP " + r.status)}`);
    return j.data;
  };
  const d = await call("/v1/api/inverterDetailList", { pageNo: 1, pageSize: 20 });
  const recs: any[] = d?.records ?? d?.page?.records ?? [];
  const items: Item[] = []; let debug: unknown = null;
  for (const x of recs.slice(0, 8)) {
    const sn = String(x.sn ?? ""); if (!sn) continue;
    const W = (k: string) => watts(x[k], x[k + "Str"] ?? "kW");
    const pv = [1, 2, 3, 4].map((n) => num(x["pow" + n])).filter((v) => v != null) as number[];
    const psum = W("psum");
    const model = String(x.stationName ?? x.sno ?? "").trim().slice(0, 30) || "Solis";
    items.push({ sn, model, data: card("solis", model, num(x.state) === 1 || num(x.state) === 3, {
      status: num(x.state) === 3 ? "alarm" : "", soc: num(x.batteryCapacitySoc), v: num(x.storageBatteryVoltage), i: null,
      p: W("batteryPower"), ppv: pv.length ? Math.round(pv.reduce((s, v) => s + v, 0)) : W("pac"),
      pload: W("familyLoadPower"), pgrid: psum == null ? null : -psum, // SolisCloud: psum > 0 = feeding the grid
      vac: num(x.uAc1), vpv1: num(x.uPv1), mos: num(x.inverterTemperature),
      epv: num(x.eToday), eload: num(x.homeLoadTodayEnergy ?? x.homeLoadEnergy), ein: num(x.batteryTodayChargeEnergy), eout: num(x.batteryTodayDischargeEnergy),
      egrid: num(x.gridPurchasedTodayEnergy ?? x.gridPurchasedDayEnergy),
    }) });
    if (!debug) debug = { rows: Object.entries(x).filter(([, v]) => typeof v !== "object" && v !== null && v !== "").slice(0, 160) };
  }
  return { items, server: base, debug };
}

async function pollOne(a: Acc) {
  try {
    const r = a.brand === "smartess" ? await smartess(a) : a.brand === "deye" ? await openApi(a, "deye")
      : a.brand === "solarman" ? await openApi(a, "solarman") : a.brand === "solis" ? await solis(a) : null;
    if (!r) throw new Error("unknown brand");
    let stored = 0;
    for (const it of r.items) {
      const { error } = await sb.rpc("inv_store", { p_owner: a.owner, p_brand: a.brand, p_sn: it.sn, p_model: it.model, p_data: it.data });
      if (!error) stored++;
    }
    const none = r.items.length === 0;
    await sb.rpc("inv_status", { p_id: a.id, p_status: none ? "no_inverter" : "ok", p_error: none ? "no inverter in this account" : null,
      p_server: r.server ?? null, p_inverters: stored, p_debug: r.debug ? { t: new Date().toISOString(), ...(r.debug as object) } : null });
    return { brand: a.brand, ok: !none, inverters: stored, err: none ? "no inverter in this account" : undefined };
  } catch (e) {
    const msg = String((e as Error)?.message ?? e).slice(0, 280);
    await sb.rpc("inv_status", { p_id: a.id, p_status: "error", p_error: msg, p_server: null, p_inverters: null, p_debug: null });
    return { brand: a.brand, ok: false, err: msg };
  }
}

Deno.serve(async (req) => {
  if (req.method === "OPTIONS") return new Response("ok", { headers: CORS });
  const J = (b: unknown, status = 200) => new Response(JSON.stringify(b), { status, headers: { ...CORS, "Content-Type": "application/json" } });
  let owner: string | null = null, brand: string | null = null;
  const cron = req.headers.get("x-cron-secret");
  if (cron) {
    const { data } = await sb.rpc("lux_cron_secret");
    if (!data || data !== cron) return new Response("forbidden", { status: 403, headers: CORS });
  } else {
    const tok = (req.headers.get("authorization") || "").replace(/^Bearer\s+/i, "");
    const { data: u } = await sb.auth.getUser(tok);
    if (!u?.user) return J({ error: "sign in" }, 401);
    owner = u.user.id;
    try { const b = await req.json(); if (b && typeof b.brand === "string" && LABEL[b.brand]) brand = b.brand; } catch { /* no body */ }
  }
  const { data: accts, error } = await sb.rpc("inv_due", { p_owner: owner, p_brand: brand });
  if (error) return J({ error: error.message }, 500);
  const t0 = Date.now(); const out = [];
  for (const a of (accts ?? []) as Acc[]) {
    if (Date.now() - t0 > 40000) break; // the rest is picked up by the next minute's run (oldest first)
    out.push(await pollOne(a));
  }
  return J(owner ? (out[0] ?? { ok: false, err: "no account linked" }) : { polled: out.length });
});
