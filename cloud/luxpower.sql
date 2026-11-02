-- BattLab × LuxPower (เฟส A) : วางทั้งไฟล์ใน Supabase → SQL Editor → Run
-- สร้าง: ตารางบัญชี LuxPower (รหัสผ่านเก็บใน Vault เข้ารหัส), ตารางประวัติอินเวอร์เตอร์, ฟังก์ชันเชื่อม/ยกเลิก/บันทึก
create extension if not exists pg_cron;

alter table public.devices add column if not exists kind text not null default 'board';
alter table public.devices add column if not exists ext_id text;
create unique index if not exists devices_owner_ext on public.devices(owner, ext_id) where ext_id is not null;

create table if not exists public.lux_accounts (
  owner uuid primary key references auth.users(id) on delete cascade,
  username text not null,
  secret_id uuid,
  server text,
  status text not null default 'pending',
  last_ok timestamptz,
  last_error text,
  inverters int not null default 0,
  created_at timestamptz not null default now()
);
alter table public.lux_accounts enable row level security;
drop policy if exists lux_owner_select on public.lux_accounts;
create policy lux_owner_select on public.lux_accounts for select to authenticated using (owner = (select auth.uid()));

create table if not exists public.inv_readings (
  id bigserial primary key,
  device_id uuid not null references public.devices(id) on delete cascade,
  ts timestamptz not null default now(),
  ppv real, pbat real, pload real, pgrid real, soc real
);
create index if not exists inv_readings_dev_ts on public.inv_readings(device_id, ts);
alter table public.inv_readings enable row level security;
drop policy if exists inv_owner_select on public.inv_readings;
create policy inv_owner_select on public.inv_readings for select to authenticated
  using (exists (select 1 from public.devices d where d.id = device_id and d.owner = (select auth.uid())));

create or replace function public.lux_link(p_user text, p_pass text)
returns text language plpgsql security definer set search_path to 'public' as $$
declare uid uuid := auth.uid(); sid uuid;
begin
  if uid is null then raise exception 'not signed in'; end if;
  if p_user is null or length(p_user) not between 3 and 100 or p_pass is null or length(p_pass) not between 1 and 100 then raise exception 'bad input'; end if;
  select secret_id into sid from public.lux_accounts where owner = uid;
  if sid is null then
    sid := vault.create_secret(p_pass, 'lux_'||uid::text, 'LuxPower password for BattLab user');
  else
    perform vault.update_secret(sid, p_pass);
  end if;
  insert into public.lux_accounts(owner, username, secret_id, status, last_error, server)
       values (uid, trim(p_user), sid, 'pending', null, null)
  on conflict (owner) do update set username = excluded.username, secret_id = excluded.secret_id, status = 'pending', last_error = null, server = null;
  return 'ok';
end $$;

create or replace function public.lux_unlink()
returns text language plpgsql security definer set search_path to 'public' as $$
declare uid uuid := auth.uid(); sid uuid;
begin
  if uid is null then raise exception 'not signed in'; end if;
  select secret_id into sid from public.lux_accounts where owner = uid;
  if sid is not null then delete from vault.secrets where id = sid; end if;
  delete from public.lux_accounts where owner = uid;
  return 'ok';
end $$;

create or replace function public.lux_due(p_owner uuid default null)
returns table(owner uuid, username text, password text, server text)
language sql security definer set search_path to 'public' as $$
  select a.owner, a.username, s.decrypted_secret, a.server
    from public.lux_accounts a join vault.decrypted_secrets s on s.id = a.secret_id
   where p_owner is null or a.owner = p_owner;
$$;

create or replace function public.lux_status(p_owner uuid, p_status text, p_error text, p_server text, p_inverters int)
returns void language sql security definer set search_path to 'public' as $$
  update public.lux_accounts set status = p_status, last_error = left(p_error, 300),
         server = coalesce(p_server, server), inverters = coalesce(p_inverters, inverters),
         last_ok = case when p_status = 'ok' then now() else last_ok end
   where owner = p_owner;
$$;

create or replace function public.lux_store(p_owner uuid, p_sn text, p_model text, p_data jsonb)
returns uuid language plpgsql security definer set search_path to 'public' as $$
declare did uuid;
begin
  select id into did from public.devices where owner = p_owner and ext_id = p_sn;
  if did is null then
    insert into public.devices(token_hash, pair_code, owner, name, kind, ext_id, settings, last_seen)
    values ('lux:'||p_owner||':'||p_sn, 'LX'||upper(substr(md5(p_owner::text||p_sn),1,8)), p_owner,
            coalesce(nullif(p_model,''),'LuxPower')||' '||right(p_sn,4), 'inverter', p_sn,
            jsonb_build_object('icon','☀️','profile',jsonb_build_object('type','solar')), now())
    returning id into did;
  end if;
  update public.devices set last_seen = now() where id = did;
  insert into public.latest(device_id, ts, data) values (did, now(), p_data)
  on conflict (device_id) do update set ts = excluded.ts, data = excluded.data;
  if not exists (select 1 from public.readings where device_id = did and ts > now() - interval '55 seconds') then
    insert into public.readings(device_id, soc, voltage, current, power, temp)
    values (did, (p_data->>'soc')::real, (p_data->>'v')::real, (p_data->>'i')::real, (p_data->>'p')::real, (p_data->>'t1')::real);
    insert into public.inv_readings(device_id, ppv, pbat, pload, pgrid, soc)
    values (did, (p_data->>'ppv')::real, (p_data->>'p')::real, (p_data->>'pload')::real, (p_data->>'pgrid')::real, (p_data->>'soc')::real);
    if random() < 0.02 then delete from public.inv_readings where device_id = did and ts < now() - interval '30 days'; end if;
  end if;
  return did;
end $$;

revoke all on function public.lux_link(text,text) from public, anon;
revoke all on function public.lux_unlink() from public, anon;
revoke all on function public.lux_due(uuid) from public, anon, authenticated;
revoke all on function public.lux_status(uuid,text,text,text,int) from public, anon, authenticated;
revoke all on function public.lux_store(uuid,text,text,jsonb) from public, anon, authenticated;
grant execute on function public.lux_link(text,text) to authenticated;
grant execute on function public.lux_unlink() to authenticated;
grant execute on function public.lux_due(uuid) to service_role;
grant execute on function public.lux_status(uuid,text,text,text,int) to service_role;
grant execute on function public.lux_store(uuid,text,text,jsonb) to service_role;
