-- BattLab cloud schema (Supabase / Postgres)
-- Devices push telemetry with a secret device token through ingest().
-- People sign in on the website and add a device with the pair code shown on
-- its screen (claim_device). Row level security keeps each owner's data private.

create extension if not exists pgcrypto with schema extensions;

create table if not exists public.devices (
  id          uuid primary key default gen_random_uuid(),
  token_hash  text not null unique,           -- sha256(device token), never the token
  pair_code   text not null unique,           -- e.g. K7Q2-9XMA, shown on the device
  owner       uuid references auth.users(id) on delete set null,
  name        text not null default 'BattLab',
  created_at  timestamptz not null default now(),
  last_seen   timestamptz
);

create table if not exists public.latest (
  device_id  uuid primary key references public.devices(id) on delete cascade,
  ts         timestamptz not null default now(),
  data       jsonb not null
);

create table if not exists public.readings (
  id         bigint generated always as identity primary key,
  device_id  uuid not null references public.devices(id) on delete cascade,
  ts         timestamptz not null default now(),
  soc real, voltage real, current real, power real,
  temp real, mos real, cell_min real, cell_max real
);
create index if not exists readings_device_ts on public.readings (device_id, ts desc);

alter table public.devices  enable row level security;
alter table public.latest   enable row level security;
alter table public.readings enable row level security;

-- Owners read their own devices and data; they may rename their devices.
drop policy if exists devices_owner_select on public.devices;
create policy devices_owner_select on public.devices
  for select to authenticated using (owner = (select auth.uid()));
drop policy if exists devices_owner_update on public.devices;
create policy devices_owner_update on public.devices
  for update to authenticated using (owner = (select auth.uid()))
  with check (owner = (select auth.uid()));
drop policy if exists latest_owner_select on public.latest;
create policy latest_owner_select on public.latest
  for select to authenticated using (exists (
    select 1 from public.devices d where d.id = device_id and d.owner = (select auth.uid())));
drop policy if exists readings_owner_select on public.readings;
create policy readings_owner_select on public.readings
  for select to authenticated using (exists (
    select 1 from public.devices d where d.id = device_id and d.owner = (select auth.uid())));

-- Only name changes are allowed from the website.
revoke update on public.devices from authenticated;
grant update (name) on public.devices to authenticated;
grant select on public.devices, public.latest, public.readings to authenticated;

-- Device upload. Called with the public anon key; the device token proves
-- which device it is. Registers the device on first contact.
create or replace function public.ingest(p_token text, p_pair text, p_data jsonb)
returns text
language plpgsql
security definer
set search_path = public, extensions
as $$
declare
  h text;
  d public.devices%rowtype;
begin
  if p_token is null or length(p_token) < 32 or length(p_token) > 128 then
    raise exception 'bad token';
  end if;
  if p_data is null or length(p_data::text) > 6000 then
    raise exception 'bad data';
  end if;
  h := encode(digest(p_token, 'sha256'), 'hex');
  select * into d from public.devices where token_hash = h;
  if not found then
    if p_pair is null or p_pair !~ '^[A-HJ-NP-Z2-9]{4}-[A-HJ-NP-Z2-9]{4}$' then
      raise exception 'bad pair code';
    end if;
    insert into public.devices (token_hash, pair_code) values (h, p_pair)
      returning * into d;
  end if;

  update public.devices set last_seen = now() where id = d.id;
  insert into public.latest (device_id, ts, data) values (d.id, now(), p_data)
    on conflict (device_id) do update set ts = excluded.ts, data = excluded.data;

  -- history: one row per minute while live data is present
  if (p_data ? 'v') and not exists (
       select 1 from public.readings
       where device_id = d.id and ts > now() - interval '55 seconds') then
    insert into public.readings (device_id, soc, voltage, current, power, temp, mos, cell_min, cell_max)
    values (d.id, (p_data->>'soc')::real, (p_data->>'v')::real, (p_data->>'i')::real,
            (p_data->>'p')::real, (p_data->>'t1')::real, (p_data->>'mos')::real,
            (p_data->>'cmin')::real, (p_data->>'cmax')::real);
    -- keep 30 days
    if random() < 0.02 then
      delete from public.readings where device_id = d.id and ts < now() - interval '30 days';
    end if;
  end if;

  return case when d.owner is null then 'unpaired' else 'ok' end;
end $$;

revoke all on function public.ingest(text, text, jsonb) from public;
grant execute on function public.ingest(text, text, jsonb) to anon, authenticated;

-- Website: add a device by the pair code shown on its screen.
create or replace function public.claim_device(p_pair text, p_name text default null)
returns uuid
language plpgsql
security definer
set search_path = public
as $$
declare
  did uuid;
begin
  if (select auth.uid()) is null then
    raise exception 'sign in first';
  end if;
  update public.devices
     set owner = (select auth.uid()),
         name  = coalesce(nullif(trim(p_name), ''), name)
   where pair_code = upper(trim(p_pair)) and owner is null
   returning id into did;
  if did is null then
    raise exception 'pair code not found or already added';
  end if;
  return did;
end $$;

revoke all on function public.claim_device(text, text) from public;
grant execute on function public.claim_device(text, text) to authenticated;

-- Website: remove a device from my account (the device can be paired again).
create or replace function public.release_device(p_id uuid)
returns void
language sql
security definer
set search_path = public
as $$
  update public.devices set owner = null where id = p_id and owner = (select auth.uid());
$$;
revoke all on function public.release_device(uuid) from public;
grant execute on function public.release_device(uuid) to authenticated;

-- ---- v2 website: per-device settings, bucketed history, realtime ----
alter table public.devices add column if not exists settings jsonb not null default '{}'::jsonb;
revoke update on public.devices from authenticated;
grant update (name, settings) on public.devices to authenticated;

create or replace function public.history_buckets(p_device uuid, p_from timestamptz, p_to timestamptz, p_bucket int)
returns table(t timestamptz, soc real, voltage real, current real, power real, temp real, mos real, cell_min real, cell_max real, n int)
language sql stable security invoker set search_path = public as $$
  select to_timestamp(floor(extract(epoch from r.ts) / greatest(p_bucket,60)) * greatest(p_bucket,60)) as t,
         avg(r.soc)::real, avg(r.voltage)::real, avg(r.current)::real, avg(r.power)::real,
         avg(r.temp)::real, avg(r.mos)::real, min(r.cell_min)::real, max(r.cell_max)::real, count(*)::int
    from public.readings r
   where r.device_id = p_device and r.ts >= p_from and r.ts < p_to
   group by 1 order by 1
   limit 5000;
$$;
revoke all on function public.history_buckets(uuid, timestamptz, timestamptz, int) from public;
grant execute on function public.history_buckets(uuid, timestamptz, timestamptz, int) to authenticated;
-- live updates on the website
alter publication supabase_realtime add table public.latest;

-- ---- v3: remote control (commands queue, ingest2) ----
alter table public.devices add column if not exists remote_until timestamptz;
create table if not exists public.commands (
  id bigint generated always as identity primary key,
  device_id uuid not null references public.devices(id) on delete cascade,
  created_by uuid references auth.users(id) on delete set null,
  created_at timestamptz not null default now(),
  cmd text not null, args jsonb not null default '{}'::jsonb,
  status text not null default 'pending',   -- pending, sent, done, error, expired
  result text, sent_at timestamptz, done_at timestamptz);
create index if not exists commands_device_status on public.commands (device_id, status, id);
alter table public.commands enable row level security;
create policy commands_owner_select on public.commands for select to authenticated using (exists (
  select 1 from public.devices d where d.id = device_id and d.owner = (select auth.uid())));
revoke insert, update, delete, truncate, references, trigger on public.commands from anon, authenticated;
revoke select on public.commands from anon;
grant select on public.commands to authenticated;
alter publication supabase_realtime add table public.commands;

create or replace function public.send_command(p_device uuid, p_cmd text, p_args jsonb default '{}'::jsonb)
returns bigint language plpgsql security definer set search_path = public as $$
declare cid bigint;
begin
  if (select auth.uid()) is null then raise exception 'sign in first'; end if;
  if not exists (select 1 from public.devices where id = p_device and owner = (select auth.uid())) then
    raise exception 'not your device'; end if;
  if p_cmd not in ('led','bms_scan','bms_connect','bms_auto','bms_disconnect','bms_param','bms_switch','bms_read',
                   'wifi_ap','radio_max','home_wifi','wifi_scan','display','restart','ping') then
    raise exception 'unknown command'; end if;
  if p_args is null or jsonb_typeof(p_args) <> 'object' or length(p_args::text) > 400 then raise exception 'bad args'; end if;
  if (select count(*) from public.commands where device_id = p_device and status in ('pending','sent')) >= 20 then
    raise exception 'too many queued commands'; end if;
  insert into public.commands (device_id, created_by, cmd, args) values (p_device, (select auth.uid()), p_cmd, p_args)
    returning id into cid;
  update public.devices set remote_until = now() + interval '3 minutes' where id = p_device;
  return cid;
end $$;

create or replace function public.remote_touch(p_device uuid)
returns void language sql security definer set search_path = public as $$
  update public.devices set remote_until = now() + interval '3 minutes'
   where id = p_device and owner = (select auth.uid());
$$;

create or replace function public.ingest2(p_token text, p_pair text, p_data jsonb, p_acks jsonb default '[]'::jsonb)
returns jsonb language plpgsql security definer set search_path = public, extensions as $$
declare st text; did uuid; fast boolean; out_cmds jsonb; a jsonb;
begin
  if p_data is not null and length(p_data::text) > 12000 then raise exception 'bad data'; end if;
  st := public.ingest(p_token, p_pair, p_data - 'devs' - 'wscan' - 'jk' - 'cfg');
  select id, coalesce(remote_until > now(), false) into did, fast
    from public.devices where token_hash = encode(digest(p_token, 'sha256'), 'hex');
  update public.latest set data = p_data where device_id = did;
  if p_acks is not null and jsonb_typeof(p_acks) = 'array' then
    for a in select * from jsonb_array_elements(p_acks) loop
      update public.commands
         set status = case when coalesce((a->>'ok')::boolean, false) then 'done' else 'error' end,
             result = left(a->>'msg', 200), done_at = now()
       where id = (a->>'id')::bigint and device_id = did and status in ('pending','sent');
    end loop;
  end if;
  update public.commands set status = 'expired'
   where device_id = did and status = 'pending' and created_at < now() - interval '5 minutes';
  update public.commands set status = 'error', result = 'no reply from board'
   where device_id = did and status = 'sent' and sent_at < now() - interval '2 minutes';
  if st = 'unpaired' then return jsonb_build_object('status', st, 'fast', false, 'cmds', '[]'::jsonb); end if;
  with picked as (
    update public.commands c set status = 'sent', sent_at = now()
     where c.id in (select id from public.commands where device_id = did and status = 'pending' order by id limit 6)
     returning c.id, c.cmd, c.args)
  select coalesce(jsonb_agg(jsonb_build_object('id', id, 'cmd', cmd, 'args', args) order by id), '[]'::jsonb)
    into out_cmds from picked;
  return jsonb_build_object('status', st, 'fast', fast, 'cmds', out_cmds);
end $$;
revoke all on function public.send_command(uuid, text, jsonb) from public;
grant execute on function public.send_command(uuid, text, jsonb) to authenticated;
revoke all on function public.remote_touch(uuid) from public;
grant execute on function public.remote_touch(uuid) to authenticated;
revoke all on function public.ingest2(text, text, jsonb, jsonb) from public;
grant execute on function public.ingest2(text, text, jsonb, jsonb) to anon, authenticated;

-- ---- v4: BattLab Cloud Screen (viewer) ----
create table if not exists public.viewers (
  id uuid primary key default gen_random_uuid(), token_hash text not null unique, pair_code text not null unique,
  owner uuid references auth.users(id) on delete set null, name text not null default 'BattLab Screen',
  created_at timestamptz not null default now(), last_seen timestamptz, fw text);
alter table public.viewers enable row level security;
create policy viewers_owner_select on public.viewers for select to authenticated using (owner = (select auth.uid()));
create policy viewers_owner_update on public.viewers for update to authenticated using (owner = (select auth.uid())) with check (owner = (select auth.uid()));
-- functions (security definer): viewer_auth(token) [internal], viewer_sync(token, pair, focus, fast, fw) [anon],
-- viewer_history(token, device, hours) [anon], viewer_command(token, device, cmd, args) [anon],
-- claim_viewer(pair, name) [authenticated], release_viewer(id) [authenticated].
-- Full bodies are in the Supabase project; summary of behaviour:
--  viewer_sync registers an unknown token with its pair code, returns {status:'unpaired'} until claimed,
--  then {devs:[{id,n,age,d}], focus, f: full latest data of the focused device, cmds: last 5 commands}.
--  viewer_command applies the same allow-list as send_command (without home_wifi/wifi_scan).
