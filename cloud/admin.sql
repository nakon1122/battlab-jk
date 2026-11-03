-- BattLab Cloud back office: super admin can see every customer, board and screen, edit them and send commands.
-- Everything goes through security-definer functions that check is_admin(); tables keep their per-owner RLS.
create table if not exists public.admins (
  user_id uuid primary key references auth.users(id) on delete cascade,
  role text not null default 'super' check (role in ('super','support')),
  created_at timestamptz not null default now()
);
alter table public.admins enable row level security; -- no policies: only the functions below read it

create table if not exists public.admin_log (
  id bigserial primary key,
  admin uuid not null,
  at timestamptz not null default now(),
  action text not null,
  target text,
  detail jsonb
);
alter table public.admin_log enable row level security;

create or replace function public.is_admin() returns boolean
language sql stable security definer set search_path to 'public' as $$
  select exists (select 1 from public.admins where user_id = (select auth.uid()));
$$;

create or replace function public.admin_check() returns uuid
language plpgsql stable security definer set search_path to 'public' as $$
declare u uuid := (select auth.uid());
begin
  if u is null or not exists (select 1 from public.admins where user_id = u) then raise exception 'admin only'; end if;
  return u;
end $$;

create or replace function public.admin_note(p_action text, p_target text, p_detail jsonb) returns void
language sql security definer set search_path to 'public' as $$
  insert into public.admin_log(admin, action, target, detail) values ((select auth.uid()), p_action, p_target, p_detail);
$$;

-- totals for the dashboard
create or replace function public.admin_overview() returns jsonb
language plpgsql security definer set search_path to 'public', 'auth' as $$
begin
  perform public.admin_check();
  return jsonb_build_object(
    'users', (select count(*) from auth.users),
    'users_7d', (select count(*) from auth.users where last_sign_in_at > now() - interval '7 days'),
    'boards', (select count(*) from public.devices where kind = 'board' and owner is not null),
    'boards_online', (select count(*) from public.devices d join public.latest l on l.device_id = d.id where d.kind = 'board' and l.ts > now() - interval '3 minutes'),
    'boards_unpaired', (select count(*) from public.devices where kind = 'board' and owner is null),
    'inverters', (select count(*) from public.devices where kind = 'inverter'),
    'screens', (select count(*) from public.viewers where owner is not null),
    'screens_online', (select count(*) from public.viewers where last_seen > now() - interval '3 minutes'),
    'lux_accounts', (select count(*) from public.lux_accounts),
    'shares', (select count(*) from public.share_links),
    'cmds_24h', (select count(*) from public.commands where created_at > now() - interval '1 day'),
    'readings_24h', (select count(*) from public.readings where ts > now() - interval '1 day'),
    'db_mb', round(pg_database_size(current_database()) / 1048576.0, 1),
    'fw', (select coalesce(jsonb_object_agg(f, n), '{}'::jsonb) from (select coalesce(l.data->>'fw', '?') f, count(*) n from public.devices d left join public.latest l on l.device_id = d.id where d.kind = 'board' group by 1) x)
  );
end $$;

-- every customer account
create or replace function public.admin_users() returns jsonb
language plpgsql security definer set search_path to 'public', 'auth' as $$
begin
  perform public.admin_check();
  return (select coalesce(jsonb_agg(jsonb_build_object(
      'id', u.id, 'email', u.email, 'created', u.created_at, 'last_login', u.last_sign_in_at,
      'confirmed', u.email_confirmed_at is not null,
      'admin', exists (select 1 from public.admins a where a.user_id = u.id),
      'boards', (select count(*) from public.devices d where d.owner = u.id and d.kind = 'board'),
      'inverters', (select count(*) from public.devices d where d.owner = u.id and d.kind = 'inverter'),
      'online', (select count(*) from public.devices d join public.latest l on l.device_id = d.id where d.owner = u.id and l.ts > now() - interval '3 minutes'),
      'screens', (select count(*) from public.viewers v where v.owner = u.id),
      'lux', (select status from public.lux_accounts x where x.owner = u.id),
      'last_data', (select max(l.ts) from public.devices d join public.latest l on l.device_id = d.id where d.owner = u.id)
    ) order by u.created_at), '[]'::jsonb) from auth.users u);
end $$;

-- boards / inverters (optionally of one customer), with their latest values
create or replace function public.admin_devices(p_owner uuid default null) returns jsonb
language plpgsql security definer set search_path to 'public', 'auth' as $$
begin
  perform public.admin_check();
  return (select coalesce(jsonb_agg(jsonb_build_object(
      'id', d.id, 'name', d.name, 'kind', d.kind, 'pair', d.pair_code, 'ext', d.ext_id, 'owner', d.owner,
      'email', (select email from auth.users u where u.id = d.owner), 'created', d.created_at,
      'group', d.settings->>'group', 'icon', d.settings->>'icon', 'ts', l.ts,
      'fw', l.data->>'fw', 'state', l.data->>'state', 'bms', l.data->>'bms', 'board', l.data#>>'{cfg,board}',
      'soc', l.data->'soc', 'v', l.data->'v', 'i', l.data->'i', 'p', l.data->'p', 'src', l.data->>'src',
      'ip', l.data#>>'{cfg,ip}', 'ssid', l.data#>>'{cfg,ssid}', 'rssi', l.data#>'{cfg,wrssi}'
    ) order by d.owner nulls last, d.created_at), '[]'::jsonb)
    from public.devices d left join public.latest l on l.device_id = d.id
   where p_owner is null or d.owner = p_owner);
end $$;

-- one device in full: row, latest data, recent commands
create or replace function public.admin_device(p_id uuid) returns jsonb
language plpgsql security definer set search_path to 'public', 'auth' as $$
declare r jsonb;
begin
  perform public.admin_check();
  select jsonb_build_object(
      'id', d.id, 'name', d.name, 'kind', d.kind, 'pair', d.pair_code, 'ext', d.ext_id, 'owner', d.owner,
      'email', (select email from auth.users u where u.id = d.owner), 'created', d.created_at, 'last_seen', d.last_seen,
      'settings', d.settings, 'ts', l.ts, 'data', l.data,
      'cmds', (select coalesce(jsonb_agg(jsonb_build_object('id', c.id, 'cmd', c.cmd, 'args', c.args, 'status', c.status, 'result', c.result, 'at', c.created_at) order by c.id desc), '[]'::jsonb)
                 from (select * from public.commands where device_id = d.id order by id desc limit 20) c),
      'readings_24h', (select count(*) from public.readings where device_id = d.id and ts > now() - interval '1 day'))
    into r
    from public.devices d left join public.latest l on l.device_id = d.id where d.id = p_id;
  if r is null then raise exception 'device not found'; end if;
  return r;
end $$;

create or replace function public.admin_update_device(p_id uuid, p_name text, p_settings jsonb) returns void
language plpgsql security definer set search_path to 'public' as $$
declare old jsonb;
begin
  perform public.admin_check();
  if p_name is not null and length(trim(p_name)) not between 1 and 60 then raise exception 'name 1-60 characters'; end if;
  if p_settings is not null and (jsonb_typeof(p_settings) <> 'object' or length(p_settings::text) > 4000) then raise exception 'bad settings'; end if;
  select jsonb_build_object('name', name, 'settings', settings) into old from public.devices where id = p_id;
  if old is null then raise exception 'device not found'; end if;
  update public.devices set name = coalesce(trim(p_name), name), settings = coalesce(p_settings, settings) where id = p_id;
  perform public.admin_note('update_device', p_id::text, jsonb_build_object('before', old, 'name', p_name, 'settings', p_settings));
end $$;

-- move a device to another customer (by e-mail) or release it (p_email = null: back to unpaired)
create or replace function public.admin_set_owner(p_id uuid, p_email text) returns text
language plpgsql security definer set search_path to 'public', 'auth' as $$
declare nu uuid; old uuid;
begin
  perform public.admin_check();
  select owner into old from public.devices where id = p_id;
  if not found then raise exception 'device not found'; end if;
  if p_email is not null then
    select id into nu from auth.users where lower(email) = lower(trim(p_email));
    if nu is null then raise exception 'no account with that e-mail'; end if;
  end if;
  update public.devices set owner = nu where id = p_id;
  delete from public.share_links where device_id = p_id;
  perform public.admin_note(case when nu is null then 'release_device' else 'transfer_device' end, p_id::text,
                            jsonb_build_object('from', old, 'to', nu, 'email', p_email));
  return case when nu is null then 'released' else 'moved' end;
end $$;

create or replace function public.admin_send_command(p_device uuid, p_cmd text, p_args jsonb default '{}'::jsonb) returns bigint
language plpgsql security definer set search_path to 'public' as $$
declare cid bigint; u uuid := public.admin_check();
begin
  if p_cmd not in ('led','bms_scan','bms_connect','bms_auto','bms_disconnect','bms_read','wifi_ap','radio_max','wifi_scan',
                   'display','restart','ping','ota','link','profile','bt_match','bt_clear','lux_set','lux_scan') then
    raise exception 'command not allowed from the back office';
  end if;
  if p_args is null or jsonb_typeof(p_args) <> 'object' or length(p_args::text) > 400 then raise exception 'bad args'; end if;
  if p_cmd = 'ota' and coalesce(p_args->>'url', '') !~ '^https://nakon1122\.github\.io/battlab-cloud/fw/[A-Za-z0-9._/-]+\.bin$' then
    raise exception 'update URL not allowed';
  end if;
  if not exists (select 1 from public.devices where id = p_device and kind = 'board') then raise exception 'board not found'; end if;
  insert into public.commands (device_id, created_by, cmd, args) values (p_device, u, p_cmd, p_args) returning id into cid;
  update public.devices set remote_until = now() + interval '3 minutes' where id = p_device;
  perform public.admin_note('command', p_device::text, jsonb_build_object('cmd', p_cmd, 'args', p_args, 'id', cid));
  return cid;
end $$;

-- Cloud Screens
create or replace function public.admin_viewers() returns jsonb
language plpgsql security definer set search_path to 'public', 'auth' as $$
begin
  perform public.admin_check();
  return (select coalesce(jsonb_agg(jsonb_build_object(
      'id', v.id, 'name', v.name, 'pair', v.pair_code, 'owner', v.owner, 'email', (select email from auth.users u where u.id = v.owner),
      'fw', v.fw, 'last_seen', v.last_seen, 'eco', v.eco, 'mb', v.mb_today) order by v.created_at), '[]'::jsonb)
    from public.viewers v);
end $$;

create or replace function public.admin_set_viewer(p_id uuid, p_name text, p_eco smallint, p_release boolean default false) returns void
language plpgsql security definer set search_path to 'public' as $$
begin
  perform public.admin_check();
  update public.viewers set name = coalesce(nullif(trim(p_name), ''), name),
         eco = case when p_eco between 0 and 2 then p_eco else eco end,
         owner = case when p_release then null else owner end
   where id = p_id;
  if not found then raise exception 'screen not found'; end if;
  perform public.admin_note(case when p_release then 'release_screen' else 'update_screen' end, p_id::text,
                            jsonb_build_object('name', p_name, 'eco', p_eco));
end $$;

create or replace function public.admin_log_list(p_limit int default 100) returns jsonb
language plpgsql security definer set search_path to 'public', 'auth' as $$
begin
  perform public.admin_check();
  return (select coalesce(jsonb_agg(jsonb_build_object('at', g.at, 'action', g.action, 'target', g.target, 'detail', g.detail,
            'by', (select email from auth.users u where u.id = g.admin)) order by g.id desc), '[]'::jsonb)
    from (select * from public.admin_log order by id desc limit least(greatest(p_limit, 1), 500)) g);
end $$;

-- grant / remove back-office access (super admins only)
create or replace function public.admin_set_role(p_email text, p_role text) returns text
language plpgsql security definer set search_path to 'public', 'auth' as $$
declare u uuid; me uuid := public.admin_check();
begin
  if not exists (select 1 from public.admins where user_id = me and role = 'super') then raise exception 'super admin only'; end if;
  select id into u from auth.users where lower(email) = lower(trim(p_email));
  if u is null then raise exception 'no account with that e-mail'; end if;
  if p_role is null then
    if u = me then raise exception 'cannot remove yourself'; end if;
    delete from public.admins where user_id = u;
  elsif p_role in ('super', 'support') then
    insert into public.admins(user_id, role) values (u, p_role) on conflict (user_id) do update set role = excluded.role;
  else raise exception 'role must be super or support'; end if;
  perform public.admin_note('set_role', u::text, jsonb_build_object('email', p_email, 'role', p_role));
  return coalesce(p_role, 'removed');
end $$;

do $$ declare f text; begin
  foreach f in array array['is_admin()','admin_check()','admin_note(text,text,jsonb)','admin_overview()','admin_users()','admin_devices(uuid)',
    'admin_device(uuid)','admin_update_device(uuid,text,jsonb)','admin_set_owner(uuid,text)','admin_send_command(uuid,text,jsonb)',
    'admin_viewers()','admin_set_viewer(uuid,text,smallint,boolean)','admin_log_list(int)','admin_set_role(text,text)'] loop
    execute format('revoke all on function public.%s from public, anon', f);
    execute format('grant execute on function public.%s to authenticated', f);
  end loop;
  execute 'revoke all on function public.admin_note(text,text,jsonb) from authenticated';
end $$;

-- the owner of BattLab Cloud
insert into public.admins(user_id, role) select id, 'super' from auth.users where lower(email) = 'khunbossz@gmail.com'
on conflict (user_id) do update set role = 'super';
