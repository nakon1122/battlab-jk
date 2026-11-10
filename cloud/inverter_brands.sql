-- BattLab Cloud: inverters of other brands (next to LuxPower).
--   cloud link  : SmartESS/DessMonitor (PowMr, Easun, MPP Solar ...), Deye Cloud, SolisCloud, Solarman (Sofar ...)
--                 polled by the edge function inv-poll, one row per owner + brand in inv_accounts
--   home dongle : a BattLab board reads the inverter on the home network and uploads it in p_data->'inv'
-- Nothing of the LuxPower link (lux_accounts, lux-poll, p_data->'lux') is changed.

create table if not exists public.inv_accounts (
  id uuid primary key default gen_random_uuid(),
  owner uuid not null references auth.users(id) on delete cascade,
  brand text not null check (brand in ('smartess','deye','solis','solarman')),
  username text not null,
  secret_id uuid not null,
  app_id text,
  app_secret_id uuid,
  server text,
  status text not null default 'pending',
  last_ok timestamptz,
  last_try timestamptz,
  last_error text,
  inverters integer not null default 0,
  debug jsonb,
  created_at timestamptz not null default now(),
  unique (owner, brand)
);
alter table public.inv_accounts enable row level security;
revoke all on public.inv_accounts from anon, authenticated;
grant select on public.inv_accounts to authenticated;
drop policy if exists inv_accounts_owner_select on public.inv_accounts;
create policy inv_accounts_owner_select on public.inv_accounts for select to authenticated
  using (owner = (select auth.uid()));

-- login tokens of the brand clouds: server side only (no policy, no grant)
create table if not exists public.inv_tokens (
  account_id uuid primary key references public.inv_accounts(id) on delete cascade,
  token text, secret text, exp timestamptz
);
alter table public.inv_tokens enable row level security;
revoke all on public.inv_tokens from anon, authenticated;

create or replace function public.inv_link(p_brand text, p_user text, p_pass text,
  p_app_id text default null, p_app_secret text default null, p_server text default null)
returns text language plpgsql security definer set search_path to 'public' as $$
declare uid uuid := auth.uid(); sid uuid; aid uuid; rid uuid;
begin
  if uid is null then raise exception 'not signed in'; end if;
  if p_brand not in ('smartess','deye','solis','solarman') then raise exception 'unknown brand'; end if;
  if p_user is null or length(trim(p_user)) not between 3 and 120 or p_pass is null or length(p_pass) not between 1 and 200 then
    raise exception 'bad input';
  end if;
  if p_brand in ('deye','solarman') and (coalesce(length(trim(p_app_id)), 0) not between 3 and 80 or coalesce(length(trim(p_app_secret)), 0) not between 3 and 200) then
    raise exception 'AppId and AppSecret are required';
  end if;
  if p_server is not null and p_server !~ '^https://[a-z0-9.-]{4,80}(:[0-9]{2,5})?(/v1\.0)?$' then raise exception 'bad server'; end if;
  select id, secret_id, app_secret_id into rid, sid, aid from public.inv_accounts where owner = uid and brand = p_brand;
  if sid is null then
    sid := vault.create_secret(p_pass, 'inv_'||p_brand||'_'||uid::text, 'inverter cloud password for BattLab user');
  else
    perform vault.update_secret(sid, p_pass);
  end if;
  if nullif(trim(coalesce(p_app_secret, '')), '') is not null then
    if aid is null then
      aid := vault.create_secret(trim(p_app_secret), 'inv_app_'||p_brand||'_'||uid::text, 'inverter cloud app secret for BattLab user');
    else
      perform vault.update_secret(aid, trim(p_app_secret));
    end if;
  end if;
  insert into public.inv_accounts(owner, brand, username, secret_id, app_id, app_secret_id, server, status, last_error, last_try)
       values (uid, p_brand, trim(p_user), sid, nullif(trim(coalesce(p_app_id, '')), ''), aid, p_server, 'pending', null, null)
  on conflict (owner, brand) do update set username = excluded.username, secret_id = excluded.secret_id, app_id = excluded.app_id,
       app_secret_id = excluded.app_secret_id, server = excluded.server, status = 'pending', last_error = null, last_try = null
  returning id into rid;
  delete from public.inv_tokens where account_id = rid;
  return 'ok';
end $$;

create or replace function public.inv_unlink(p_brand text)
returns text language plpgsql security definer set search_path to 'public' as $$
declare uid uuid := auth.uid(); sid uuid; aid uuid;
begin
  if uid is null then raise exception 'not signed in'; end if;
  select secret_id, app_secret_id into sid, aid from public.inv_accounts where owner = uid and brand = p_brand;
  delete from public.inv_accounts where owner = uid and brand = p_brand;
  if sid is not null then delete from vault.secrets where id = sid; end if;
  if aid is not null then delete from vault.secrets where id = aid; end if;
  return 'ok';
end $$;

-- accounts to poll now. Cron (p_owner null): only those whose turn it is, oldest first.
create or replace function public.inv_due(p_owner uuid default null, p_brand text default null)
returns table(id uuid, owner uuid, brand text, username text, password text, app_id text, app_secret text,
              server text, token text, token_secret text, token_exp timestamptz)
language plpgsql security definer set search_path to 'public' as $$
#variable_conflict use_column
begin
  return query
  with pick as (
    select a.id from public.inv_accounts a
     where (p_owner is null or a.owner = p_owner) and (p_brand is null or a.brand = p_brand)
       and (p_owner is not null or a.last_try is null or a.last_try < now() - case a.brand
              when 'smartess' then interval '110 seconds' when 'deye' then interval '110 seconds'
              when 'solis' then interval '170 seconds' else interval '290 seconds' end
              * case when a.status = 'error' then 5 else 1 end)
     order by a.last_try nulls first limit 12),
  upd as (update public.inv_accounts a set last_try = now() from pick where a.id = pick.id returning a.*)
  select u.id, u.owner, u.brand, u.username, s.decrypted_secret, u.app_id, s2.decrypted_secret, u.server, t.token, t.secret, t.exp
    from upd u join vault.decrypted_secrets s on s.id = u.secret_id
    left join vault.decrypted_secrets s2 on s2.id = u.app_secret_id
    left join public.inv_tokens t on t.account_id = u.id;
end $$;

create or replace function public.inv_status(p_id uuid, p_status text, p_error text, p_server text, p_inverters integer, p_debug jsonb default null)
returns void language sql security definer set search_path to 'public' as $$
  update public.inv_accounts set status = p_status, last_error = left(p_error, 300),
         server = coalesce(p_server, server), inverters = coalesce(p_inverters, inverters),
         debug = coalesce(p_debug, debug),
         last_ok = case when p_status = 'ok' then now() else last_ok end
   where id = p_id;
$$;

create or replace function public.inv_token(p_id uuid, p_token text, p_secret text, p_exp timestamptz)
returns void language sql security definer set search_path to 'public' as $$
  insert into public.inv_tokens(account_id, token, secret, exp) values (p_id, p_token, p_secret, p_exp)
  on conflict (account_id) do update set token = excluded.token, secret = excluded.secret, exp = excluded.exp;
$$;

-- one inverter card (same shape as the LuxPower cards). Fresh home-dongle data is never overwritten by cloud data.
create or replace function public.inv_store(p_owner uuid, p_brand text, p_sn text, p_model text, p_data jsonb)
returns uuid language plpgsql security definer set search_path to 'public' as $$
declare did uuid; old jsonb; old_ts timestamptz; dongle boolean := coalesce(p_data->>'src','') = 'dongle'; d jsonb;
begin
  if p_sn is null or p_sn !~ '^[A-Za-z0-9._:-]{4,40}$' then return null; end if;
  select id into did from public.devices where owner = p_owner and ext_id = p_sn;
  if did is null then
    insert into public.devices(token_hash, pair_code, owner, name, kind, ext_id, settings, last_seen)
    values ('inv:'||p_brand||':'||p_owner||':'||p_sn, 'IV'||upper(substr(md5(p_owner::text||p_brand||p_sn),1,8)), p_owner,
            left(coalesce(nullif(p_model,''),'Inverter'), 40)||' '||right(p_sn,4), 'inverter', p_sn,
            jsonb_build_object('icon','☀️','profile',jsonb_build_object('type','solar')), now())
    returning id into did;
  end if;
  select data, ts into old, old_ts from public.latest where device_id = did;
  if not dongle and coalesce(old->>'src','') = 'dongle' and old_ts > now() - interval '90 seconds' then
    return did;
  end if;
  d := case when dongle then coalesce(old, '{}'::jsonb) || p_data else p_data end;
  update public.devices set last_seen = now() where id = did;
  insert into public.latest(device_id, ts, data) values (did, now(), d)
  on conflict (device_id) do update set ts = excluded.ts, data = excluded.data;
  if coalesce(d->>'state','') = 'LIVE'
     and not exists (select 1 from public.readings where device_id = did and ts > now() - interval '55 seconds') then
    insert into public.readings(device_id, soc, voltage, current, power, temp)
    values (did, (d->>'soc')::real, (d->>'v')::real, (d->>'i')::real, (d->>'p')::real, (d->>'t1')::real);
    insert into public.inv_readings(device_id, ppv, pbat, pload, pgrid, soc)
    values (did, (d->>'ppv')::real, (d->>'p')::real, (d->>'pload')::real, (d->>'pgrid')::real, (d->>'soc')::real);
    if random() < 0.02 then delete from public.inv_readings where device_id = did and ts < now() - interval '30 days'; end if;
  end if;
  return did;
end $$;

create or replace function public.inv_tick()
returns void language plpgsql security definer set search_path to 'public' as $$
begin
  if not exists (select 1 from public.inv_accounts) then return; end if;
  perform net.http_post(
    url := 'https://zpsxcdzkayqhzbdcdoed.supabase.co/functions/v1/inv-poll',
    headers := jsonb_build_object('Content-Type','application/json','x-cron-secret', (select decrypted_secret from vault.decrypted_secrets where name = 'lux_cron_secret' limit 1)),
    body := '{}'::jsonb, timeout_milliseconds := 55000);
end $$;

revoke all on function public.inv_link(text, text, text, text, text, text) from public, anon;
revoke all on function public.inv_unlink(text) from public, anon;
grant execute on function public.inv_link(text, text, text, text, text, text) to authenticated;
grant execute on function public.inv_unlink(text) to authenticated;
revoke all on function public.inv_due(uuid, text) from public, anon, authenticated;
revoke all on function public.inv_status(uuid, text, text, text, integer, jsonb) from public, anon, authenticated;
revoke all on function public.inv_token(uuid, text, text, timestamptz) from public, anon, authenticated;
revoke all on function public.inv_store(uuid, text, text, text, jsonb) from public, anon, authenticated;
revoke all on function public.inv_tick() from public, anon, authenticated;
grant execute on function public.inv_due(uuid, text) to service_role;
grant execute on function public.inv_status(uuid, text, text, text, integer, jsonb) to service_role;
grant execute on function public.inv_token(uuid, text, text, timestamptz) to service_role;
grant execute on function public.inv_store(uuid, text, text, text, jsonb) to service_role;

-- board upload: p_data->'inv' = inverters this board read from a home dongle (Deye/Sofar/Solis/PowMr ...).
-- The new block cannot break the upload of the board itself: any error in it is swallowed.
create or replace function public.ingest2(p_token text, p_pair text, p_data jsonb, p_acks jsonb DEFAULT '[]'::jsonb)
 returns jsonb language plpgsql security definer set search_path to 'public', 'extensions' as $function$
declare
  st text; did uuid; own uuid; fast boolean; out_cmds jsonb; a jsonb; pk jsonb; lx jsonb; nlux int := 0;
  brand text; label text;
begin
  if p_data is not null and length(p_data::text) > 32000 then raise exception 'bad data'; end if;
  st := public.ingest(p_token, p_pair, p_data - 'devs' - 'wscan' - 'jk' - 'cfg' - 'packs' - 'pa' - 'bd' - 'lux' - 'inv');
  select id, owner, coalesce(remote_until > now(), false) into did, own, fast
    from public.devices where token_hash = encode(digest(p_token, 'sha256'), 'hex');
  update public.latest set data = p_data - 'lux' - 'inv' where device_id = did;
  if own is not null and jsonb_typeof(p_data->'lux') = 'array' and jsonb_array_length(p_data->'lux') <= 4 then
    for lx in select * from jsonb_array_elements(p_data->'lux') loop
      if (lx->>'sn') ~ '^[A-Za-z0-9]{10}$' and length(lx::text) < 3000 and coalesce((lx->>'age')::int, 999) < 90 then
        perform public.lux_store(own, lx->>'sn', 'LuxPower',
          (lx - 'sn' - 'age') || jsonb_build_object('inv', 1, 'src', 'dongle', 'fw', 'luxpower', 'bms', 'LuxPower',
                                                     'state', 'LIVE', 'chg', true, 'dsg', true, 'via', did));
        nlux := nlux + 1;
      end if;
    end loop;
    if nlux > 0 then perform public.lux_par_refresh(own); end if;
  end if;
  if own is not null and jsonb_typeof(p_data->'inv') = 'array' and jsonb_array_length(p_data->'inv') <= 4 then
    begin
      for lx in select * from jsonb_array_elements(p_data->'inv') loop
        brand := lower(coalesce(lx->>'brand', ''));
        if brand in ('deye','sofar','solis','powmr') and (lx->>'sn') ~ '^[A-Za-z0-9._:-]{4,40}$'
           and length(lx::text) < 3000 and coalesce((lx->>'age')::int, 999) < 90 then
          label := case brand when 'deye' then 'Deye' when 'sofar' then 'Sofar' when 'solis' then 'Solis' else 'PowMr' end;
          perform public.inv_store(own, brand, lx->>'sn', coalesce(nullif(lx->>'model', ''), label),
            (lx - 'sn' - 'age' - 'brand') || jsonb_build_object('inv', 1, 'src', 'dongle', 'fw', brand, 'bms', label,
                                                                  'state', 'LIVE', 'chg', true, 'dsg', true, 'via', did));
        end if;
      end loop;
    exception when others then null;
    end;
  end if;
  if jsonb_typeof(p_data->'packs') = 'array' and jsonb_array_length(p_data->'packs') <= 16
     and not exists (select 1 from public.pack_readings where device_id = did and ts > now() - interval '55 seconds') then
    for pk in select * from jsonb_array_elements(p_data->'packs') loop
      if coalesce((pk->>'age')::int, 999) < 120 and (pk->>'a') ~ '^[0-9]{1,2}$' then
        insert into public.pack_readings (device_id, addr, soc, voltage, current, temp, mos, cell_min, cell_max)
        values (did, (pk->>'a')::smallint, (pk->>'soc')::real, (pk->>'v')::real, (pk->>'i')::real,
                (pk->>'t1')::real, (pk->>'mos')::real, (pk->>'cmin')::real, (pk->>'cmax')::real);
      end if;
    end loop;
    if random() < 0.02 then
      delete from public.pack_readings where device_id = did and ts < now() - interval '30 days';
    end if;
  end if;
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
  if st = 'unpaired' then
    return jsonb_build_object('status', st, 'fast', false, 'cmds', '[]'::jsonb);
  end if;
  with picked as (
    update public.commands c set status = 'sent', sent_at = now()
     where c.id in (select id from public.commands where device_id = did and status = 'pending' order by id limit 6)
     returning c.id, c.cmd, c.args)
  select coalesce(jsonb_agg(jsonb_build_object('id', id, 'cmd', cmd, 'args', args) order by id), '[]'::jsonb)
    into out_cmds from picked;
  return jsonb_build_object('status', st, 'fast', fast, 'cmds', out_cmds);
end $function$;

-- website -> board: inv_set (home dongle sources) and inv_scan (look for dongles on the LAN)
create or replace function public.send_command(p_device uuid, p_cmd text, p_args jsonb DEFAULT '{}'::jsonb)
 returns bigint language plpgsql security definer set search_path to 'public' as $function$
declare cid bigint;
begin
  if (select auth.uid()) is null then raise exception 'sign in first'; end if;
  if not exists (select 1 from public.devices where id = p_device and owner = (select auth.uid())) then
    raise exception 'not your device';
  end if;
  -- bms_runtime_reset writes to the BMS runtime register: super admins only
  if p_cmd = 'bms_runtime_reset' and not public.is_admin() then raise exception 'admin only'; end if;
  if p_cmd not in ('led','bms_scan','bms_connect','bms_auto','bms_disconnect','bms_param','bms_switch','bms_read',
                   'wifi_ap','radio_max','home_wifi','wifi_scan','display','restart','ping','ota','link','profile','bt_match','bt_clear','jk_codes',
                   'lux_set','lux_scan','bms_runtime_reset','inv_set','inv_scan') then
    raise exception 'unknown command';
  end if;
  if p_args is null or jsonb_typeof(p_args) <> 'object' or length(p_args::text) > 400 then
    raise exception 'bad args';
  end if;
  if p_cmd = 'ota' and coalesce(p_args->>'url', '') !~ '^https://nakon1122\.github\.io/battlab-cloud/fw/[A-Za-z0-9._/-]+\.bin$' then
    raise exception 'update URL not allowed';
  end if;
  if (select count(*) from public.commands where device_id = p_device and status in ('pending','sent')) >= 20 then
    raise exception 'too many queued commands';
  end if;
  insert into public.commands (device_id, created_by, cmd, args) values (p_device, (select auth.uid()), p_cmd, p_args)
    returning id into cid;
  update public.devices set remote_until = now() + interval '3 minutes' where id = p_device;
  return cid;
end $function$;

select cron.schedule('inv-poll-every-minute', '* * * * *', 'select public.inv_tick()')
 where not exists (select 1 from cron.job where jobname = 'inv-poll-every-minute');
