# yicam-bridge (ESP32)

This firmware turns a cheap ESP32 DevKit into a small motion bridge for a Yi camera running [yi-hack-Allwinner](https://github.com/roleoroleo/yi-hack-Allwinner). It saves motion photos and the camera's video clips to Google Drive:

```
Yi camera ──MQTT motion_start/stop──▶ ESP32 (MQTT broker)
ESP32 ──snapshot.sh?file=…──▶ Yi camera saves the JPEG on its own SD card
ESP32 ── streams the file ──▶ Google Drive API ─▶ Drive/YiCam/<date>/   (1-hour token from the Apps Script)
Yi camera ──MQTT motion_files──▶ ESP32 queues the event's video clips, same path
ESP32 ──"saved" report──▶ Apps Script (email notifications, day folders, cleanup)
```

- No PC needs to stay on, and nothing goes through Yi's cloud.
- Plain HTTP and MQTT never leave your LAN. The only traffic to the internet is HTTPS to Google, and the ESP32 verifies Google's certificate.
- Target board: ESP32 DevKit with the ESP-WROOM-32 module and CP2102 USB chip (no PSRAM). Files never live on the ESP32: they're streamed from the camera to Drive in small pieces.
- During a motion event the ESP32 takes a shot every `interval` seconds, up to `maxShots`, until `motion_stop`. The AI labels `human`, `vehicle` and `animal`, which the camera sends on the same topic, also count as motion.
- The Drive token comes from your Apps Script and only reaches the script's own `YiCam` folder (`drive.file`). The bridge keeps it in RAM only and gets a new one every hour.

## 1. Google Drive receiver (Apps Script)

The script runs with only the `drive.file` permission, so it can access only the files and folders it creates. It makes its own `YiCam` folder and works inside it; the rest of your Drive is invisible to it. It uses the Advanced Drive service (Drive API v3) because `DriveApp` would force full Drive access.

1. Go to <https://script.google.com> and create a **New project**.
2. In **Project Settings**, tick **Show "appsscript.json" manifest file in editor**. Replace that file with [`apps-script/appsscript.json`](apps-script/appsscript.json). It sets the permissions, enables the Advanced Drive service, and sets the time zone (`Etc/GMT+3` = UTC-3).
3. Replace `Code.gs` with [`apps-script/Code.gs`](apps-script/Code.gs).
4. In **Project Settings → Script properties**, add:
   - `TOKEN`: a long random string, for example from `openssl rand -hex 24`.
   - Optional: `RETENTION_DAYS`, the number of days of images to keep (default 30).
   - Optional: `NOTIFY_EMAIL`, an address that gets one email per motion event with its photos attached.
   - Optional: `NOTIFY_COOLDOWN_MIN`, the minutes without emails after one is sent (default 60, `0` = off).
   - `ROOT_FOLDER_ID`, `NOTIFY_STATE`, `NOTIFY_LAST_SENT` and `NOTIFY_SUPPRESSED` are managed by the script; don't set them.
5. Run `setup` once from the editor. This authorizes the script, creates the `YiCam` folder, and installs the daily cleanup trigger. Drive access on the consent screen should cover only files used with this app. It also asks to send email as you and to connect to an external service, which the email notifications need to attach photos.
6. Click **Deploy → New deployment → Web app**. Set *Execute as*: **Me** and *Who has access*: **Anyone**, then copy the `/exec` URL.
7. Optional: with `NOTIFY_EMAIL` set, run `testNotification` to get a test email with the latest photo.

Anyone who has both the URL and the token can upload to that folder, so keep them private.

Notifications collect each motion event's photos and send them in one email once no new photo has arrived for a minute (at most 5 minutes after the first). There are up to 10 attachments per email, and the email links to the day's folder for the rest.

## 2. Build & flash (on the computer the board is plugged into)

```bash
pip install platformio            # or: brew install platformio
cd yicam-esp32
pio run -e usb -t upload          # first flash over USB
pio device monitor                # serial log, 115200 baud
```

On macOS the CP2102 board usually appears as `/dev/cu.usbserial-*` or `/dev/cu.SLAB_USBtoUART`. If the upload stalls at `Connecting...`, hold the **IO0/BOOT** button until it starts.

The serial monitor prints the setup network's password at every boot:
`[boot] setup network "YiCam-Setup", password xxxx-xxxx-xxxx`. Each bridge generates its own random one on first boot, so there's no shared default. It's also on the dashboard's Settings page.

## 3. First-boot setup portal

1. On first boot the ESP32 creates a Wi-Fi network named **`YiCam-Setup`**. Connect to it with the password from the serial monitor.
2. A setup page opens; if it doesn't, browse to `192.168.4.1`. Fill in:
   - Your Wi-Fi network (2.4 GHz)
   - **Camera IP**, e.g. `192.168.0.163`
   - **Apps Script URL** and **token** from step 1
   - **MQTT user / password**: credentials the *camera* will use to connect to the ESP32. Leaving the user blank makes the broker open to anything on your LAN.
   - Resolution (`high` or `low`), seconds between shots, max shots per event, timezone (POSIX, default `<-03>3` = UTC-3), and an OTA password
3. Save. The board joins your Wi-Fi.

To reopen the portal later, press **EN** (reset) and hold **IO0/BOOT** during the first 2 seconds. Fields marked *secret* are never shown; leave them blank to keep the stored value.

Reserve a fixed IP for the ESP32 in your router (DHCP reservation), because the camera points at it.

## 4. Point the camera at the ESP32

In the yi-hack web UI (`http://192.168.0.163`), open **MQTT settings**:

| Setting | Value |
|---|---|
| MQTT | enabled |
| Broker IP | the ESP32's IP |
| Port | `1883` |
| User / password | the MQTT credentials from the portal |
| Prefix / motion topic | `yicam` / `motion_detection` (the defaults; must match the portal's *Motion topic*) |

Clear the motion image topic (`motion_detection_image`). Otherwise the camera pushes a full JPEG to the ESP32 at every motion start, which the ESP32 doesn't use. Keep the **video list** topic (`motion_files`, the default): the bridge uses it to find each event's clips.

## 5. Test

Open the dashboard at **http://yicam-bridge.local** (or the bridge's IP) and set a dashboard password when it asks. Press **Take snapshot**, then walk in front of the camera. Photos should appear in Drive under `YiCam/<yyyy-mm-dd>/` within a few seconds, and the event's video clips a minute or two after motion stops.

See [SETUP.md](SETUP.md) for the full step-by-step guide.

## Updates over Wi-Fi

```bash
PLATFORMIO_UPLOAD_FLAGS="--auth=<ota-password>" pio run -e ota -t upload
```

## Notes & limits

- Photos: the camera saves each shot to its SD card under one of 30 reused names (`yicam-bridge-00.jpg` … `-29.jpg`, about 8 MB in total). A name is reused only after its photo is in Drive. If all 30 are waiting, for example because Drive is unreachable, new shots are skipped so the first shots survive; check `dropped`.
- Video clips: about 70 s after motion stops, the camera lists the event's clips on `<prefix>/motion_files`. They're queued (up to 60, and the daily limit in Settings, default 50) and uploaded with a resumable upload once no photos are waiting. A clip upload pauses when motion starts and resumes from where it stopped. The camera's recording folders are in UTC; the bridge names clips in local time.
- The camera only keeps recordings until its SD card is full. If the bridge falls far behind, the oldest clips may be gone before they're uploaded; these count as `failed`.
- The built-in broker is small. It handles the camera plus a couple of LAN clients (for example your own app subscribing to `yicam/#`) and doesn't store messages for offline clients.
- Uploads verify Google's TLS chain against GTS Root R1/R4 and GlobalSign ECC R4, which are embedded in `src/root_certs.h` and valid until 2036/2038.
