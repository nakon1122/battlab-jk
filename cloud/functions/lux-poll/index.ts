// BattLab x LuxPower: poll LuxPower cloud (same web API the LuxPower View app uses) and store
// each inverter as a BattLab device, plus one combined device per parallel group.
// Runs every minute from pg_cron (x-cron-secret) or on demand by a signed-in user (own account only).
// When a BattLab board reads the inverters straight from their Wi-Fi dongles (ingest2 -> lux_store with
// src = dongle) the cloud login is skipped and fresh dongle data is never overwritten.
import { createClient } from "npm:@supabase/supabase-js@2.45.4";

const SERVERS = [
  "https://sea.luxpowertek.com", "https://server.luxpowertek.com", "https://as.luxpowertek.com",
  "https://eu.luxpowertek.com", "https://us.luxpowertek.com", "https://na.luxpowertek.com", "https://af.luxpowertek.com",
];
const sb = createClient(Deno.env.get("SUPABASE_URL")!, Deno.env.get("SUPABASE_SERVICE_ROLE_KEY")!, { auth: { persistSession: false } });
const CORS = { "Access-Control-Allow-Origin": "*", "Access-Control-Allow-Headers": "authorization, x-client-info, apikey, content-type, x-cron-secret" };

type Sess = { base: string; cookie: string };

async function post(base: string, path: string, body: Record<string, unknown>, cookie = "") {
  const ctl = new AbortController();
  const t = setTimeout(() => ctl.abort(), 12000);
  try {
    const r = await fetch(base + path, {
      method: "POST",
      headers: { "Content-Type": "application/x-www-form-urlencoded; charset=UTF-8", "User-Agent": "Mozilla/5.0 BattLab", ...(cookie ? { Cookie: cookie } : {}) },
      body: new URLSearchParams(Object.fromEntries(Object.entries(body).map(([k, v]) => [k, String(v)]))),
      redirect: "manual", signal: ctl.signal,
    });
    const sc = (r.headers as any).getSetCookie?.() as string[] | undefined;
    const set = (sc && sc.length ? sc : (r.headers.get("set-cookie") ? [r.headers.get("set-cookie")!] : [])).map((c) => c.split(";")[0]).join("; ");
    const text = await r.text();
    let json: any = null;
    try { json = JSON.parse(text); } catch { /* html = session expired / redirect */ }
    return { status: r.status, json, set };
  } finally { clearTimeout(t); }
}

async function login(user: string, pass: string, known: string | null): Promise<{ s?: Sess; err?: string }> {
  const list = known ? [known, ...SERVERS.filter((x) => x !== known)] : SERVERS;
  let last = "no server answered";
  for (const base of list) {
    try {
      const r = await post(base, "/WManage/api/login", { account: user, password: pass, language: "ENGLISH" });
      if (r.json && r.json.success && r.set) return { s: { base, cookie: r.set } };
      if (r.json && r.json.success === false) last = r.json.msg || r.json.message || "login refused";
    } catch (e) { last = String(e); }
  }
  return { err: last };
}

const n = (x: unknown) => (typeof x === "number" && isFinite(x) ? x : (typeof x === "string" && x !== "" && isFinite(+x) ? +x : null));
const e10 = (en: any, k: string) => (n(en?.[k]) != null ? +((n(en[k])! / 10).toFixed(2)) : null);

function modelName(rt: any, item: any) {
  const m = String(rt?.modelText || "");
  if (m && !/^0x/i.test(m)) return m;
  return item?.deviceTypeText4APP || item?.deviceTypeText || m || "LuxPower";
}

function mapData(rt: any, en: any, item: any) {
  const vBat = n(rt.vBat); const v = vBat != null ? vBat / 10 : null;
  const pchg = n(rt.pCharge) ?? 0, pdis = n(rt.pDisCharge) ?? 0;
  const p = pchg - pdis; // battery power, + = charging
  const pload = n(rt.consumptionPower) ?? n(rt.peps) ?? n(rt.pToUser);
  const pgrid = (n(rt.pToUser) ?? 0) - (n(rt.pToGrid) ?? 0); // + = import
  const soc = n(rt.soc) ?? n(item?.soc);
  return {
    inv: 1, src: "cloud", fw: "luxpower", state: rt.lost ? "OFFLINE" : "LIVE", bms: "LuxPower",
    model: modelName(rt, item), status: rt.statusText || item?.statusText || "",
    soc, v, i: v ? +(p / v).toFixed(2) : null, p,
    t1: n(rt.tBat), mos: n(rt.tinner),
    ppv: n(rt.ppv), ppv1: n(rt.ppv1), ppv2: n(rt.ppv2), pload, pgrid, peps: n(rt.peps), prec: n(rt.prec), pinv: n(rt.pinv),
    vac: n(rt.vacr) != null ? n(rt.vacr)! / 10 : null, vpv1: n(rt.vpv1) != null ? n(rt.vpv1)! / 10 : null,
    epv: e10(en, "todayYielding"), ein: e10(en, "todayCharging"), eout: e10(en, "todayDischarging"), eload: e10(en, "todayUsage"), egrid: e10(en, "todayImport"),
    chg: true, dsg: true, devtime: rt.deviceTimeText || null,
    pgroup: item?.parallelGroup || null, pidx: item?.parallelIndex ?? null,
  };
}

// one combined record for inverters running in parallel (like the LuxPower app's system view)
function combine(parts: { sn: string; d: any }[], pen: any) {
  const sum = (k: string) => parts.reduce((a, x) => a + (n(x.d[k]) ?? 0), 0);
  const avg = (k: string) => { const v = parts.map((x) => n(x.d[k])).filter((x) => x != null) as number[]; return v.length ? +(v.reduce((a, b) => a + b, 0) / v.length).toFixed(2) : null; };
  const max = (k: string) => { const v = parts.map((x) => n(x.d[k])).filter((x) => x != null) as number[]; return v.length ? Math.max(...v) : null; };
  const v = avg("v"), p = sum("p");
  const en = (k: string, alt: string) => e10(pen, k) ?? (parts.every((x) => n(x.d[alt]) != null) ? +sum(alt).toFixed(2) : null);
  const m = parts[0].d.model;
  return {
    inv: 1, src: "cloud", par: parts.length, fw: "luxpower", bms: "LuxPower", state: parts.some((x) => x.d.state === "LIVE") ? "LIVE" : "OFFLINE",
    model: m + " ×" + parts.length, status: parts.map((x) => x.d.status).filter(Boolean).join(" / "),
    soc: n(pen?.soc) ?? avg("soc"), v, p, i: v ? +(p / v).toFixed(2) : null, t1: max("t1"), mos: max("mos"),
    ppv: sum("ppv"), pload: sum("pload"), pgrid: sum("pgrid"), pinv: sum("pinv"), vac: avg("vac"),
    epv: en("todayYielding", "epv"), ein: en("todayCharging", "ein"), eout: en("todayDischarging", "eout"),
    eload: e10(pen, "todayUsage") ?? parts[0].d.eload, egrid: en("todayImport", "egrid"),
    chg: true, dsg: true, devtime: parts[0].d.devtime,
    members: parts.map((x) => ({ sn: x.sn, ppv: x.d.ppv, pload: x.d.pload, p: x.d.p, soc: x.d.soc, status: x.d.status })),
  };
}

async function pollOne(a: { owner: string; username: string; password: string; server: string | null }, force = false) {
  // a BattLab board reads every inverter straight from its Wi-Fi dongle: no need to ask the LuxPower cloud
  if (!force) {
    const { data: fresh } = await sb.rpc("lux_dongle_fresh", { p_owner: a.owner });
    if (fresh) {
      await sb.rpc("lux_status", { p_owner: a.owner, p_status: "ok", p_error: null, p_server: null, p_inverters: null });
      return { owner: a.owner, ok: true, dongle: true };
    }
  }
  const lg = await login(a.username, a.password, a.server);
  if (!lg.s) { await sb.rpc("lux_status", { p_owner: a.owner, p_status: "error", p_error: lg.err ?? "login failed", p_server: null, p_inverters: null }); return { owner: a.owner, ok: false, err: lg.err }; }
  const { base, cookie } = lg.s;
  const ls = await post(base, "/WManage/api/inverterOverview/list", { page: 1, rows: 30, plantId: -1, searchText: "", statusText: "all" }, cookie);
  const rows: any[] = ls.json?.rows ?? [];
  let stored = 0;
  const groups = new Map<string, { sn: string; d: any }[]>();
  for (const it of rows.slice(0, 12)) {
    const sn = String(it.serialNum || "");
    if (!/^[A-Za-z0-9]{6,20}$/.test(sn)) continue;
    const [rt, en] = await Promise.all([
      post(base, "/WManage/api/inverter/getInverterRuntime", { serialNum: sn }, cookie),
      post(base, "/WManage/api/inverter/getInverterEnergyInfo", { serialNum: sn }, cookie),
    ]);
    if (!rt.json || rt.json.success === false) continue;
    const data = mapData(rt.json, en.json, it);
    const { error } = await sb.rpc("lux_store", { p_owner: a.owner, p_sn: sn, p_model: data.model, p_data: data });
    if (!error) stored++;
    // first versions named the card after the raw model code (0x....): give it the real model name
    await sb.from("devices").update({ name: data.model + " " + sn.slice(-4) }).eq("owner", a.owner).eq("ext_id", sn).like("name", "0x%");
    const g = it.parallelGroup ? `${it.plantId}:${it.parallelGroup}` : null;
    if (g) { if (!groups.has(g)) groups.set(g, []); groups.get(g)!.push({ sn, d: data }); }
  }
  for (const [g, parts] of groups) {
    if (parts.length < 2) continue;
    const pe = await post(base, "/WManage/api/inverter/getInverterEnergyInfoParallel", { serialNum: parts[0].sn }, cookie);
    const data = combine(parts, pe.json && pe.json.success !== false ? pe.json : null);
    const ext = "PAR:" + g;
    const { error } = await sb.rpc("lux_store", { p_owner: a.owner, p_sn: ext, p_model: data.model, p_data: data });
    // default name from lux_store is "<model> <last 4 of ext id>": replace it once with a friendly one
    if (!error) await sb.from("devices").update({ name: "ระบบขนาน " + data.model }).eq("owner", a.owner).eq("ext_id", ext).eq("name", data.model + " " + ext.slice(-4));
  }
  await sb.rpc("lux_status", { p_owner: a.owner, p_status: rows.length ? "ok" : "no_inverter", p_error: rows.length ? null : "no inverter in this LuxPower account", p_server: base, p_inverters: stored });
  return { owner: a.owner, ok: true, server: base, inverters: stored, parallel: [...groups.values()].filter((x) => x.length > 1).length };
}

Deno.serve(async (req) => {
  if (req.method === "OPTIONS") return new Response("ok", { headers: CORS });
  let owner: string | null = null;
  const cron = req.headers.get("x-cron-secret");
  if (cron) {
    const { data } = await sb.rpc("lux_cron_secret");
    if (!data || data !== cron) return new Response("forbidden", { status: 403, headers: CORS });
  } else {
    const tok = (req.headers.get("authorization") || "").replace(/^Bearer\s+/i, "");
    const { data: u } = await sb.auth.getUser(tok);
    if (!u?.user) return new Response(JSON.stringify({ error: "sign in" }), { status: 401, headers: { ...CORS, "Content-Type": "application/json" } });
    owner = u.user.id;
  }
  const { data: accts, error } = await sb.rpc("lux_due", { p_owner: owner });
  if (error) return new Response(JSON.stringify({ error: error.message }), { status: 500, headers: { ...CORS, "Content-Type": "application/json" } });
  const out = [];
  for (const a of accts ?? []) { try { out.push(await pollOne(a)); } catch (e) { out.push({ owner: a.owner, ok: false, err: String(e) }); } }
  const body = owner ? out[0] ?? { ok: false, err: "no LuxPower account linked" } : { polled: out.length };
  return new Response(JSON.stringify(body), { headers: { ...CORS, "Content-Type": "application/json" } });
});
