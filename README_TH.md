# Bambu LAN → Cloud Monitor

The bridge reads all 4 printers (A1, P2S, X2D, H2C) over LAN. It then sends their status up to the cloud (Supabase project "data"), so you can watch them from anywhere.

```
เครื่องพิมพ์ x4 ──LAN MQTT──> ESP32-S3 Bridge ──HTTPS──> Supabase ──> เว็บ dashboard (มือถือ)
                                                              ├──> จอ CYD ที่ไหนก็ได้
                                                              └──> LINE แจ้งเตือน
```

## Files in firmware/

| File | Board | Use |
|---|---|---|
| `BAMBU_BRIDGE_S3_INSTALL.bin` | Any ESP32-S3 | Bridge (sits at home, same network as the printers) |
| `BAMBU_SCREEN_CYD28_INSTALL.bin` | ESP32-2432S028 (2.8") | Remote screen |
| `BAMBU_SCREEN_CYD35_INSTALL.bin` | ESP32-3248S035 (3.5") | Remote screen |
| `*_APP_UPDATE.bin` | — | Update without erasing settings (use the UPDATE_*.bat files) |

## Step 1: Prepare the printers

On every printer: **Settings → Network**, turn on **LAN Only** and **Developer Mode** (H2C, X2D, P2S and A1 on new firmware won't let an external device read data until Developer Mode is on).
Write down 3 values from each printer: **IP address**, **Serial number** and **Access Code**.

> Set the printer IPs as fixed addresses (DHCP reservation in the router) so they don't change.

## Step 2: Install the bridge (ESP32-S3)

1. Double-click `FLASH_BRIDGE_S3_WINDOWS.bat` and enter the COM port.
2. Connect your phone to the Wi-Fi network **BAMBU-BRIDGE**. The setup page opens by itself (or go to http://192.168.4.1).
3. Wi-Fi section: pick your home Wi-Fi, enter the password, and press save. The board restarts.
4. Connect your phone back to your home Wi-Fi and open **http://bambubridge.local** (or the board's IP from your router).
5. Fill in all 4 printers (name, model, IP, serial, access code) and press **Save printers**.
   Within a few seconds the status card should turn green.
6. The top of the page shows the **dashboard link** and the **View key** for the CYD screen.

## Step 3: Watch from outside

Open https://nakon1122.github.io/battlab-cloud/bambu/ and enter the View key (starts with `bv_`).
On a phone, choose "Add to Home Screen" and it works like an app.

## Step 4: LINE alerts

LINE Notify has shut down, so this uses the LINE Messaging API (free, with a monthly message limit):

1. Go to https://developers.line.biz → Create a Provider → Create a channel of type **Messaging API**.
2. **Messaging API** tab: press Issue **Channel access token (long-lived)** and copy it.
3. **Basic settings** tab: copy **Your user ID** (starts with `U`).
4. Scan the channel's QR code to add the bot as a friend.
5. On the bridge page → LINE alerts → paste the token and user ID → choose which events to send → **Save LINE** → **Send test message**.

The default is to alert on: print finished, failed, paused, error and lost connection.

## Step 5: CYD screen (optional)

1. Run `FLASH_SCREEN_CYD28_WINDOWS.bat` (2.8") or `FLASH_SCREEN_CYD35_WINDOWS.bat` (3.5").
2. The screen shows the Wi-Fi setup page. Connect your phone to **BAMBU-SCREEN** → pick Wi-Fi → Save.
3. The View key is already set. To change it, open the screen's IP in a browser.

Screen tabs: **ALL** (all printers) · **PRINTER** (one printer, swipe left/right or tap the arrows) · **EVENTS** · **SETUP** (brightness, rotate, calibrate touch, change Wi-Fi).
If the colors look wrong, go to the screen's web page → choose Type B / Invert / BGR.

## Troubleshooting

| Problem | Fix |
|---|---|
| `wrong access code` | The access code changes when you toggle LAN mode. Read it again from the printer screen. |
| `no MQTT answer` | Developer Mode is not on yet. |
| `cannot connect` | Wrong IP, or the printer is off. |
| Dashboard says Bridge offline | The bridge has no internet or has lost power. |
| Nozzle temperatures on the dual-nozzle models (X2D/H2C) are swapped left/right | Report it and it will be fixed in the next build. |
