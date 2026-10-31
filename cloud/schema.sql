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
