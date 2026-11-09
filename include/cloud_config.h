#pragma once
// Supabase project "data". The anon key is public by design: row level
// security and the ingest() function decide what it can do.
#define CLOUD_URL "https://zpsxcdzkayqhzbdcdoed.supabase.co"
#define CLOUD_ANON_KEY "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6Inpwc3hjZHprYXlxaHpiZGNkb2VkIiwicm9sZSI6ImFub24iLCJpYXQiOjE3OTM0NTMyODgsImV4cCI6MjEwOTAyOTI4OH0.aAXESY_MJ1IHlvF_8CVdVAGWdfz5ZM6ysoOki7kUv08"
#define CLOUD_SITE "batterycloud.battlab.net"
// Older site: still accepted for firmware downloads so boards keep updating
// from either address while customers move over.
#define CLOUD_SITE_OLD "nakon1122.github.io/battlab-cloud"
// true when url is a .bin under /fw/ on the BattLab site (new or old address)
#define CLOUD_FW_URL_OK(u) (((u).startsWith(String("https://") + CLOUD_SITE + "/fw/") || (u).startsWith(String("https://") + CLOUD_SITE_OLD + "/fw/")) && (u).endsWith(".bin"))
