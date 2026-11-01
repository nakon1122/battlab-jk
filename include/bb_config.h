#pragma once
// Bambu LAN -> Cloud bridge / remote screen: shared settings.
#define BB_FW_VERSION "1.2.0"

// Supabase project "data". The anon key is public by design: the tables are
// closed to it, and the bambu_push / bambu_get functions check the bridge's
// write key / view key before they touch anything.
#define BB_CLOUD_URL "https://zpsxcdzkayqhzbdcdoed.supabase.co"
#define BB_CLOUD_ANON "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6Inpwc3hjZHprYXlxaHpiZGNkb2VkIiwicm9sZSI6ImFub24iLCJpYXQiOjE3OTM0NTMyODgsImV4cCI6MjEwOTAyOTI4OH0.aAXESY_MJ1IHlvF_8CVdVAGWdfz5ZM6ysoOki7kUv08"
#define BB_DASHBOARD_URL "https://nakon1122.github.io/battlab-cloud/bambu/"

// Keys of the bridge created for this install (can be changed on the web page).
#define BB_DEFAULT_WRITE_KEY "bw_2c7860d45bdd19a44eccf069985b73eddcf9"
#define BB_DEFAULT_VIEW_KEY "bv_bab6c8bcad9d32f9aa2c9e71"

// Local time (POSIX TZ). Thailand = UTC+7.
#define BB_TZ "ICT-7"

// printers per bridge board (use more boards for more printers: 4 boards = 16)
#define BB_MAX_PRINTERS 4
