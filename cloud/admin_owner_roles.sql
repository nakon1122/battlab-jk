-- BattLab back office, part 2: move/release devices and grant admin roles.
-- Paste the whole file into Supabase -> SQL Editor -> Run.
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

revoke all on function public.admin_set_owner(uuid,text) from public, anon;
revoke all on function public.admin_set_role(text,text) from public, anon;
grant execute on function public.admin_set_owner(uuid,text) to authenticated;
grant execute on function public.admin_set_role(text,text) to authenticated;
