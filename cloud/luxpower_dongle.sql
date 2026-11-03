-- BattLab x LuxPower phase B: a BattLab board reads the inverter straight from its Wi-Fi dongle
-- and uploads it with its own data (p_data->'lux'). That data goes to the same inverter cards
-- (matched by serial); while it is fresh the LuxPower cloud poller leaves those cards alone.

create or replace function public.lux_store(p_owner uuid, p_sn text, p_model text, p_data jsonb)
returns uuid language plpgsql security definer set search_path to 'public' as $$
declare did uuid; old jsonb; old_ts timestamptz; dongle boolean := coalesce(p_data->>'src','') = 'dongle'; d jsonb;
begin
  select id into did from public.devices where owner = p_owner and ext_id = p_sn;
  if did is null then
    insert into public.devices(token_hash, pair_code, owner, name, kind, ext_id, settings, last_seen)
    values ('lux:'||p_owner||':'||p_sn, 'LX'||upper(substr(md5(p_owner::text||p_sn),1,8)), p_owner,
            coalesce(nullif(p_model,''),'LuxPower')||' '||right(p_sn,4), 'inverter', p_sn,
            jsonb_build_object('icon','☀️','profile',jsonb_build_object('type','solar')), now())
    returning id into did;
  end if;
  select data, ts into old, old_ts from public.latest where device_id = did;
  -- cloud data never overwrites fresh dongle data (dongle is newer and more often)
  if not dongle and coalesce(old->>'src','') = 'dongle' and old_ts > now() - interval '90 seconds' then
    return did;
  end if;
  -- dongle data keeps what only the cloud knows (model, parallel group, members ...)
  d := case when dongle then coalesce(old, '{}'::jsonb) || p_data else p_data end;
  update public.devices set last_seen = now() where id = did;
  insert into public.latest(device_id, ts, data) values (did, now(), d)
  on conflict (device_id) do update set ts = excluded.ts, data = excluded.data;
  if not exists (select 1 from public.readings where device_id = did and ts > now() - interval '55 seconds') then
    insert into public.readings(device_id, soc, voltage, current, power, temp)
    values (did, (d->>'soc')::real, (d->>'v')::real, (d->>'i')::real, (d->>'p')::real, (d->>'t1')::real);
    insert into public.inv_readings(device_id, ppv, pbat, pload, pgrid, soc)
    values (did, (d->>'ppv')::real, (d->>'p')::real, (d->>'pload')::real, (d->>'pgrid')::real, (d->>'soc')::real);
    if random() < 0.02 then delete from public.inv_readings where device_id = did and ts < now() - interval '30 days'; end if;
  end if;
  return did;
end $$;

-- rebuild each parallel-system card of this owner from its members' latest data
create or replace function public.lux_par_refresh(p_owner uuid)
returns void language plpgsql security definer set search_path to 'public' as $$
declare r record; t record; m jsonb; nd jsonb;
begin
  for r in select d.ext_id, l.data from public.devices d join public.latest l on l.device_id = d.id
            where d.owner = p_owner and d.ext_id like 'PAR:%' and jsonb_typeof(l.data->'members') = 'array' loop
    select count(*) n,
           count(*) filter (where ml.data->>'src' = 'dongle' and ml.ts > now() - interval '90 seconds') nd,
           sum((ml.data->>'ppv')::real) ppv, sum((ml.data->>'pload')::real) pload, sum((ml.data->>'pgrid')::real) pgrid,
           sum((ml.data->>'pinv')::real) pinv, sum((ml.data->>'p')::real) p,
           avg((ml.data->>'v')::real) v, avg((ml.data->>'soc')::real) soc, max((ml.data->>'t1')::real) t1, max((ml.data->>'mos')::real) mos,
           sum((ml.data->>'epv')::real) epv, sum((ml.data->>'ein')::real) ein, sum((ml.data->>'eout')::real) eout,
           sum((ml.data->>'eload')::real) eload, sum((ml.data->>'egrid')::real) egrid,
           bool_or(ml.data->>'state' = 'LIVE' and ml.ts > now() - interval '5 minutes') live,
           jsonb_agg(jsonb_build_object('sn', md.ext_id, 'ppv', ml.data->'ppv', 'pload', ml.data->'pload', 'p', ml.data->'p',
                                        'soc', ml.data->'soc', 'status', ml.data->'status') order by md.ext_id) mem
      into t
      from jsonb_array_elements(r.data->'members') x
      join public.devices md on md.owner = p_owner and md.ext_id = x->>'sn'
      join public.latest ml on ml.device_id = md.id;
    if t.n is null or t.n < 2 or t.nd = 0 then continue; end if; -- nothing from a dongle: the cloud poller does it
    nd := jsonb_strip_nulls(jsonb_build_object(
      'src', 'dongle', 'state', case when t.live then 'LIVE' else 'OFFLINE' end,
      'ppv', round(t.ppv::numeric), 'pload', round(t.pload::numeric), 'pgrid', round(t.pgrid::numeric), 'pinv', round(t.pinv::numeric),
      'p', round(t.p::numeric), 'v', round(t.v::numeric, 2), 'soc', round(t.soc::numeric, 1),
      'i', case when t.v > 1 then round((t.p / t.v)::numeric, 2) end, 't1', t.t1, 'mos', t.mos,
      'epv', round(t.epv::numeric, 1), 'ein', round(t.ein::numeric, 1), 'eout', round(t.eout::numeric, 1),
      'eload', round(t.eload::numeric, 1), 'egrid', round(t.egrid::numeric, 1), 'members', t.mem));
    perform public.lux_store(p_owner, r.ext_id, null, nd);
  end loop;
end $$;

-- the poller asks this first: every inverter of this owner fresh from a dongle -> skip the LuxPower login
create or replace function public.lux_dongle_fresh(p_owner uuid)
returns boolean language sql security definer set search_path to 'public' as $$
  select count(*) > 0 and coalesce(bool_and(l.data->>'src' = 'dongle' and l.ts > now() - interval '90 seconds'), false)
    from public.devices d left join public.latest l on l.device_id = d.id
   where d.owner = p_owner and d.kind = 'inverter' and d.ext_id not like 'PAR:%';
$$;

create or replace function public.ingest2(p_token text, p_pair text, p_data jsonb, p_acks jsonb DEFAULT '[]'::jsonb)
 returns jsonb language plpgsql security definer set search_path to 'public', 'extensions' as $function$
declare
  st text; did uuid; own uuid; fast boolean; out_cmds jsonb; a jsonb; pk jsonb; lx jsonb; nlux int := 0;
begin
  if p_data is not null and length(p_data::text) > 32000 then raise exception 'bad data'; end if;
  st := public.ingest(p_token, p_pair, p_data - 'devs' - 'wscan' - 'jk' - 'cfg' - 'packs' - 'pa' - 'bd' - 'lux');
  select id, owner, coalesce(remote_until > now(), false) into did, own, fast
    from public.devices where token_hash = encode(digest(p_token, 'sha256'), 'hex');
  update public.latest set data = p_data - 'lux' where device_id = did;
  -- LuxPower inverters read by this board from their Wi-Fi dongles
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

revoke all on function public.lux_par_refresh(uuid) from public, anon, authenticated;
revoke all on function public.lux_dongle_fresh(uuid) from public, anon, authenticated;
grant execute on function public.lux_dongle_fresh(uuid) to service_role;
grant execute on function public.lux_par_refresh(uuid) to service_role;

-- website can turn the dongle reader on/off, set IPs and start a LAN search on a board
create or replace function public.send_command(p_device uuid, p_cmd text, p_args jsonb DEFAULT '{}'::jsonb)
 returns bigint language plpgsql security definer set search_path to 'public' as $function$
declare cid bigint;
begin
  if (select auth.uid()) is null then raise exception 'sign in first'; end if;
  if not exists (select 1 from public.devices where id = p_device and owner = (select auth.uid())) then
    raise exception 'not your device';
  end if;
  if p_cmd not in ('led','bms_scan','bms_connect','bms_auto','bms_disconnect','bms_param','bms_switch','bms_read',
                   'wifi_ap','radio_max','home_wifi','wifi_scan','display','restart','ping','ota','link','profile','bt_match','bt_clear','jk_codes',
                   'lux_set','lux_scan') then
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
