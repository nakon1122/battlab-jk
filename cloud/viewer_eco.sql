-- Cloud Screen data saver (2.8.8): the screen and the website share the mode; the screen reports MB used today.
-- Same function signature as before (old screens keep working): the screen sends p_fw = "<fw>|m<MB today>|e<mode set on the screen>".
alter table public.viewers add column if not exists eco smallint check (eco between 0 and 2);
alter table public.viewers add column if not exists mb_today real;
create or replace function public.viewer_sync(p_token text, p_pair text, p_focus uuid DEFAULT NULL::uuid, p_fast boolean DEFAULT false, p_fw text DEFAULT NULL::text)
 returns jsonb language plpgsql security definer set search_path to 'public', 'extensions' as $function$
declare v public.viewers%rowtype; devs jsonb; foc jsonb; cm jsonb; fid uuid; ein real; eout real; day0 timestamptz; ota text; pk jsonb;
        fwv text := nullif(split_part(coalesce(p_fw, ''), '|', 1), ''); s_mb text := substring(p_fw from '\|m([0-9]{1,6}(\.[0-9])?)');
        s_eco text := substring(p_fw from '\|e([0-2])');
begin
  v := public.viewer_auth(p_token);
  if v.id is null then
    if p_pair is null or p_pair !~ '^[A-HJ-NP-Z2-9]{4}-[A-HJ-NP-Z2-9]{4}$' then raise exception 'bad pair code'; end if;
    insert into public.viewers (token_hash, pair_code) values (encode(digest(p_token, 'sha256'), 'hex'), p_pair) returning * into v;
  end if;
  ota := v.ota_url;
  update public.viewers set last_seen = now(), fw = coalesce(left(fwv, 40), fw), ota_url = null,
         eco = coalesce(s_eco::smallint, eco), mb_today = coalesce(s_mb::real, mb_today)
   where id = v.id returning * into v;
  if v.owner is null then return jsonb_build_object('status', 'unpaired', 'name', v.name, 'eco', v.eco); end if;
  select coalesce(jsonb_agg(jsonb_build_object(
           'id', d.id, 'n', d.name, 'g', coalesce(d.settings->>'group', ''), 'pf', d.settings->'profile',
           'age', case when l.ts is null then -1 else floor(extract(epoch from now() - l.ts))::int end,
           'd', coalesce(l.data, '{}'::jsonb) - 'cells' - 'jk' - 'cfg' - 'devs' - 'wscan' - 'model' - 'heap' - 'blk' - 'up' - 'packs' - 'pa' - 'bd' - 'members')
           order by d.created_at), '[]'::jsonb)
    into devs
    from public.devices d left join public.latest l on l.device_id = d.id
   where d.owner = v.owner;
  if p_focus is not null then
    select d.id into fid from public.devices d where d.id = p_focus and d.owner = v.owner;
  end if;
  if fid is not null then
    select coalesce(l.data, '{}'::jsonb) - 'wscan' - 'bd' into foc from public.latest l where l.device_id = fid;
    if jsonb_typeof(foc->'packs') = 'array' then
      select coalesce(jsonb_agg(e - 'model' - 't2' order by (e->>'a')::int), '[]'::jsonb) into pk
        from jsonb_array_elements(foc->'packs') e;
      foc := jsonb_set(foc, '{packs}', pk);
    end if;
    select coalesce(jsonb_agg(jsonb_build_object('id', c.id, 'c', c.cmd, 's', c.status, 'r', c.result) order by c.id desc), '[]'::jsonb)
      into cm from (select * from public.commands where device_id = fid order by id desc limit 5) c;
    day0 := date_trunc('day', now() at time zone 'Asia/Bangkok') at time zone 'Asia/Bangkok';
    select coalesce(sum(greatest(power, 0)), 0) / 60000.0, coalesce(sum(greatest(-power, 0)), 0) / 60000.0
      into ein, eout from public.readings where device_id = fid and ts >= day0;
    if p_fast then update public.devices set remote_until = now() + interval '2 minutes' where id = fid; end if;
  end if;
  return jsonb_build_object('status', 'ok', 'name', v.name, 'devs', devs, 'focus', fid, 'f', coalesce(foc, '{}'::jsonb),
                            'cmds', coalesce(cm, '[]'::jsonb), 'ein', round(coalesce(ein, 0)::numeric, 2), 'eout', round(coalesce(eout, 0)::numeric, 2),
                            'ota', ota, 'eco', v.eco);
end $function$;
grant update (eco) on public.viewers to authenticated;
