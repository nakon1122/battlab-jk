-- Module ownership transfer requests (2026-11-11)
-- A customer who adds a module that is still in another account can ask for it.
-- The current owner approves or denies from their home page; a super admin can decide instead.

create table if not exists public.transfer_requests (
  id bigserial primary key,
  device_id uuid not null references public.devices(id) on delete cascade,
  from_owner uuid not null,
  to_user uuid not null,
  name text,
  status text not null default 'pending' check (status in ('pending','approved','denied','cancelled')),
  created_at timestamptz not null default now(),
  decided_at timestamptz,
  decided_by uuid
);
create unique index if not exists transfer_requests_one_pending
  on public.transfer_requests(device_id, to_user) where status = 'pending';
create index if not exists transfer_requests_from on public.transfer_requests(from_owner) where status = 'pending';
alter table public.transfer_requests enable row level security;
revoke all on public.transfer_requests from anon, authenticated;

-- mask an e-mail for the other party: so***@gmail.com
create or replace function public.mask_email(e text) returns text
language sql immutable set search_path to 'public' as $$
  select case when e is null or position('@' in e) = 0 then '—'
              else left(split_part(e, '@', 1), 2) || '***@' || split_part(e, '@', 2) end
$$;

-- what is behind a pair code, from the caller's point of view: none | free | mine | other | pending
create or replace function public.pair_status(p_pair text) returns text
language plpgsql security definer set search_path to 'public' as $$
declare u uuid := (select auth.uid()); d public.devices%rowtype;
begin
  if u is null then raise exception 'sign in first'; end if;
  select * into d from public.devices where pair_code = upper(trim(p_pair)) and coalesce(kind, 'board') = 'board'
   order by (owner is null), last_seen desc nulls last limit 1;
  if not found then return 'none'; end if;
  if d.owner is null then return 'free'; end if;
  if d.owner = u then return 'mine'; end if;
  if exists (select 1 from public.transfer_requests where device_id = d.id and to_user = u and status = 'pending') then return 'pending'; end if;
  return 'other';
end $$;

create or replace function public.request_transfer(p_pair text, p_name text default null) returns jsonb
language plpgsql security definer set search_path to 'public' as $$
declare u uuid := (select auth.uid()); d public.devices%rowtype; rid bigint;
begin
  if u is null then raise exception 'sign in first'; end if;
  select * into d from public.devices where pair_code = upper(trim(p_pair)) and coalesce(kind, 'board') = 'board' and owner is not null
   order by last_seen desc nulls last limit 1;
  if not found then raise exception 'pair code not found'; end if;
  if d.owner = u then raise exception 'already in your account'; end if;
  if (select count(*) from public.transfer_requests where to_user = u and status = 'pending') >= 5 then
    raise exception 'too many pending requests';
  end if;
  select id into rid from public.transfer_requests where device_id = d.id and to_user = u and status = 'pending';
  if rid is null then
    insert into public.transfer_requests(device_id, from_owner, to_user, name)
    values (d.id, d.owner, u, nullif(trim(p_name), '')) returning id into rid;
  end if;
  return jsonb_build_object('id', rid, 'status', 'pending');
end $$;

-- move the module; used by both the owner and an admin
create or replace function public.transfer_apply(p_req bigint, p_by uuid) returns void
language plpgsql security definer set search_path to 'public' as $$
declare r public.transfer_requests%rowtype;
begin
  select * into r from public.transfer_requests where id = p_req and status = 'pending' for update;
  if not found then raise exception 'request not found'; end if;
  update public.devices
     set owner = r.to_user,
         name = coalesce(r.name, name),
         settings = coalesce(settings, '{}'::jsonb) - 'group' - 'order' - 'hidden'
   where id = r.device_id and owner = r.from_owner;
  if not found then
    update public.transfer_requests set status = 'cancelled', decided_at = now(), decided_by = p_by where id = r.id;
    raise exception 'owner changed, request closed';
  end if;
  delete from public.share_links where device_id = r.device_id;
  update public.transfer_requests set status = 'approved', decided_at = now(), decided_by = p_by where id = r.id;
  update public.transfer_requests set status = 'cancelled', decided_at = now(), decided_by = p_by
   where device_id = r.device_id and status = 'pending' and id <> r.id;
end $$;
revoke all on function public.transfer_apply(bigint, uuid) from public, anon, authenticated;

-- owner (or super admin) approves / denies
create or replace function public.decide_transfer(p_id bigint, p_approve boolean) returns text
language plpgsql security definer set search_path to 'public' as $$
declare u uuid := (select auth.uid()); r public.transfer_requests%rowtype; adm boolean;
begin
  if u is null then raise exception 'sign in first'; end if;
  select * into r from public.transfer_requests where id = p_id and status = 'pending';
  if not found then raise exception 'request not found'; end if;
  adm := exists (select 1 from public.admins where user_id = u and role = 'super');
  if r.from_owner <> u and not adm then raise exception 'not allowed'; end if;
  if p_approve then
    perform public.transfer_apply(p_id, u);
  else
    update public.transfer_requests set status = 'denied', decided_at = now(), decided_by = u where id = p_id;
  end if;
  if adm and r.from_owner <> u then
    insert into public.admin_log(admin, action, target, detail)
    values (u, 'transfer_request', r.device_id::text,
            jsonb_build_object('request', p_id, 'approve', p_approve, 'from', r.from_owner, 'to', r.to_user));
  end if;
  return case when p_approve then 'approved' else 'denied' end;
end $$;

create or replace function public.cancel_transfer(p_id bigint) returns void
language sql security definer set search_path to 'public' as $$
  update public.transfer_requests set status = 'cancelled', decided_at = now(), decided_by = (select auth.uid())
   where id = p_id and to_user = (select auth.uid()) and status = 'pending';
$$;

-- for the home page: requests on my modules (incoming) and my own requests (outgoing)
create or replace function public.my_transfers() returns jsonb
language plpgsql security definer set search_path to 'public', 'auth' as $$
declare u uuid := (select auth.uid());
begin
  if u is null then return '{"in":[],"out":[]}'::jsonb; end if;
  return jsonb_build_object(
    'in', coalesce((select jsonb_agg(jsonb_build_object('id', r.id, 'device', d.name, 'pair', d.pair_code,
              'who', public.mask_email(au.email), 'at', r.created_at) order by r.id)
        from public.transfer_requests r join public.devices d on d.id = r.device_id
        left join auth.users au on au.id = r.to_user
       where r.from_owner = u and r.status = 'pending'), '[]'::jsonb),
    'out', coalesce((select jsonb_agg(jsonb_build_object('id', r.id, 'pair', d.pair_code, 'name', coalesce(r.name, d.name),
              'status', r.status, 'at', coalesce(r.decided_at, r.created_at)) order by r.id desc)
        from public.transfer_requests r join public.devices d on d.id = r.device_id
       where r.to_user = u and (r.status = 'pending' or r.decided_at > now() - interval '3 days')), '[]'::jsonb));
end $$;

-- back office: every pending request with full e-mails
create or replace function public.admin_transfers() returns jsonb
language plpgsql security definer set search_path to 'public', 'auth' as $$
begin
  perform public.admin_check();
  return coalesce((select jsonb_agg(jsonb_build_object('id', r.id, 'device', d.name, 'pair', d.pair_code,
            'from', fu.email, 'to', tu.email, 'at', r.created_at,
            'online', exists (select 1 from public.latest l where l.device_id = d.id and l.ts > now() - interval '3 minutes')) order by r.id)
      from public.transfer_requests r join public.devices d on d.id = r.device_id
      left join auth.users fu on fu.id = r.from_owner left join auth.users tu on tu.id = r.to_user
     where r.status = 'pending'), '[]'::jsonb);
end $$;

revoke all on function public.pair_status(text), public.request_transfer(text, text), public.decide_transfer(bigint, boolean),
  public.cancel_transfer(bigint), public.my_transfers(), public.admin_transfers() from public, anon;
grant execute on function public.pair_status(text), public.request_transfer(text, text), public.decide_transfer(bigint, boolean),
  public.cancel_transfer(bigint), public.my_transfers(), public.admin_transfers() to authenticated;
