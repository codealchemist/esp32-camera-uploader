# yicam-bridge: setup guide

This guide sets up the bridge from scratch: a Yi camera running yi-hack sends motion events to an ESP32, which saves photos and the camera's video clips to Google Drive. Email alerts are optional. [README.md](README.md) explains how it works.

**What you need**

- A Yi camera running [yi-hack-Allwinner](https://github.com/roleoroleo/yi-hack-Allwinner) with MQTT support. Tested with yi-hack-Allwinner 0.4.1 on a Yi 1080p Home (`y20ga`).
- An ESP32 DevKit with the ESP-WROOM-32 module (no PSRAM needed), and a USB **data** cable.
- A Google account.
- A computer with Python 3 for [PlatformIO](https://platformio.org/).
- A 2.4 GHz Wi-Fi network. The ESP32 doesn't support 5 GHz.

> **Keep secrets out of chats, issues and screenshots.** The Apps Script URL and token, the passwords and the setup network password belong in a password manager and the pages named below.
>
> For random secrets you can use `openssl rand -hex 24`, or your password manager's generator.

---

## 1. Flash the ESP32

```bash
pip install platformio
```

```bash
pio run -e usb -t upload && pio device monitor
```

On macOS the board usually appears as `/dev/cu.usbserial-*`, and `platformio.ini` looks for that name. On other systems, adjust `upload_port` there. If the upload stalls at `Connecting...`, hold **BOOT** until it starts.

The serial monitor shows a line like this:

```
[boot] setup network "YiCam-Setup", password abcd-efgh-jkmn
```

**Save that password.** Each bridge generates its own random setup password on first boot, so there's no shared default. It's also shown later on the dashboard's Settings page, and erasing the flash generates a new one.

## 2. Google Drive receiver (Apps Script)

The bridge uploads to Drive using a short-lived token from a small Apps Script that you deploy in your own account. That script also handles day folders, cleanup and email alerts.

The script only gets Google's narrow `drive.file` permission: it can see and change **only the files and folders it creates itself**. It creates its own `YiCam` folder in My Drive and works inside it; the rest of your Drive is invisible to it.

1. Go to <https://script.google.com> and click **New project**. Rename it, for example to *YiCam receiver*.
2. **Show the manifest:** **Project Settings** (gear icon) → tick **Show "appsscript.json" manifest file in editor**.
3. **Paste the manifest:** in the **Editor**, replace `appsscript.json` with [`apps-script/appsscript.json`](apps-script/appsscript.json).
   - **Set your time zone** in its `timeZone` field, for example `Europe/Madrid` or `America/New_York`. Day folders and email times use it. The file ships with `Etc/GMT+3`, which means UTC-3.
4. **Paste the code:** replace `Code.gs` with [`apps-script/Code.gs`](apps-script/Code.gs) and save.
5. **Script properties:** **Project Settings → Script properties → Add script property**:

   | Property | Value |
   |---|---|
   | `TOKEN` | a long random string (required). The bridge sends it with every request. |
   | `RETENTION_DAYS` | optional: days of photos and clips to keep (default `30`) |
   | `NOTIFY_EMAIL` | optional: the address for motion emails (see *Email alerts* below) |
   | `NOTIFY_COOLDOWN_MIN` | optional: minutes without emails after one is sent (default `60`, `0` = off) |

   `ROOT_FOLDER_ID`, `OWNER_EMAIL`, `NOTIFY_STATE`, `NOTIFY_LAST_SENT` and `NOTIFY_SUPPRESSED` are managed by the script; don't add them.
6. **Run setup once:** choose `setup` in the function list next to **Debug** and click **Run**.
   - If Google warns that the app isn't verified, click **Advanced → Go to … (unsafe)**. It's your own script.
   - Check the permission screen. For Drive it should only ask for *the specific Google Drive files you use with this app*. It also asks to run when you're not present (cleanup), to send email as you (alerts) and to connect to an external service (attaching photos to alerts). **If it asks for access to all your Google Drive files, stop:** the manifest wasn't saved.
   - The execution log shows a link to the new `YiCam` folder.
7. **Deploy:** **Deploy → New deployment →** gear menu next to *Select type* → **Web app**. Set *Execute as* to **Me** and *Who has access* to **Anyone**.
8. Copy the **Web app URL**, the one that ends in `/exec`.
9. **Check it:** open the `/exec` URL in a browser. It should show `{"ok":true,"service":"yicam-bridge receiver"}`.

Notes:
- You can rename or move the `YiCam` folder and the script keeps working. If you delete it, the next upload creates a new one.
- Cleanup runs daily around 04:00. It moves day folders (`yyyy-mm-dd`) older than `RETENTION_DAYS` to the Drive bin, where they can still be restored for 30 days. It never touches anything else.
- Anyone with both the URL and the token can upload into that folder and get a token for it. Keep both private.
- **After editing `Code.gs`, always publish a new version:** **Deploy → Manage deployments →** edit the web app → **Version: New version → Deploy**. The URL stays the same.

### Email alerts (optional)

With `NOTIFY_EMAIL` set, you get **one email per motion event** with its photos attached (up to 10) and a link to the day's folder. It's sent once no new photo has arrived for a minute, or 5 minutes after the first one. After an email, there are no more for `NOTIFY_COOLDOWN_MIN` minutes (default 60); the next email says how many photos were saved in between.

To test it, run `testNotification` in the editor. You should get a "[Test] YiCam" email with the most recent photo. To stop the emails, delete `NOTIFY_EMAIL`.

### Control page for your phone (optional)

A second deployment of the same script gives you a page, signed in with your Google account, with:
- **Live view:** while the page is open and in the foreground, the bridge takes a frame every 3, 5, 10 or 30 s (your choice, remembered on each device), and the panel shows the newest one. **Pause** stops it.
  - How it works: the page's regular status requests tell the script "watching, a frame every N s". Each bridge check-in (every 15 s while live) reads that state: live, or nobody watching.
  - It also stops when you switch apps or lock the phone, and by itself about a minute after the page is closed.
  - A spinner only shows when there's no image at all yet. The first frame arrives within one check-in.
  - Frames are named `…-l.jpg` and kept out of emails and the timeline, and frames older than 15 minutes are deleted.
  - Live frames are low resolution (~30–50 KB), so at 5 s it's roughly 30 MB of mobile data per hour of watching. Motion and requested photos keep your full resolution.
  - The bridge takes a new frame only once the previous one is uploaded, so frames come every *N* s or as fast as uploads allow, and never pile up. A waiting frame uploads before other photos, and only the newest one.
  - The panel shows the last image without covering it. It's in black and white while you wait if it's older than 5 minutes. A thin line at the bottom counts down to the next frame (or the next check-in), next to the frame's time and the live status.
- **Capture requests:** a snapshot, or 30 s / 1 min of shots at the bridge's *Seconds between shots*.
- **Timeline:** photos grouped by event, with the event's video clips. Move between days with ‹ › or the calendar (days with photos are highlighted), and open the day's folder with **Drive ↗**.
- **Viewer:** tap a photo or clip to open it on the page. Step through an event with ‹ › or by swiping; clips (~5 MB) take a few seconds to load. **Open in Drive** is always there.

How it works:
- The bridge can't be reached from outside, so it **checks in** with the script every 30 s (adjustable in the bridge's Settings), using its usual token.
- A capture you request starts at the next check-in, so within about 30 s, and runs at the bridge's *Seconds between shots*.
- Requests expire after 2 minutes, so an offline bridge never takes surprise photos later.
- Requested photos are named `…-r.jpg` and marked *requested* on the timeline.
- Nothing in your home network is opened to the internet.

Setup:
1. In the Apps Script editor, add an HTML file: **Files → + → HTML**, named **`Control`**. Paste [`apps-script/Control.html`](apps-script/Control.html) into it.
2. Paste the current `Code.gs` and `appsscript.json`, and save. The manifest adds one permission, **your email address**: the page uses it to check that the person signed in is you.
3. Run `setup` once in the editor, signed in as yourself. It approves the new permission and records your account as the only one the control page opens for (`OWNER_EMAIL` in Script properties).
4. **Update the bridge's deployment:** **Deploy → Manage deployments →** edit the existing web app → **Version: New version**, keep *Who has access* = **Anyone** → **Deploy**. Same URL as before.
5. **Create the control deployment:** **Deploy → New deployment → Web app**, *Execute as* **Me**, *Who has access* **Only myself** → **Deploy**. Its `/exec` URL is your control page.
6. Open that URL on your phone, signed in to the same Google account, and add it to the home screen:
   - iPhone: Safari → Share → **Add to Home Screen**
   - Android: Chrome → ⋮ → **Add to home screen**

Notes:
- **Check both deployments in a private browser window:**
  - the bridge's URL should show `{"ok":true,"service":"yicam-bridge receiver"}`
  - the control URL should ask you to sign in to Google

  Every page function also refuses anyone who isn't the script's owner, so a wrong access setting can't expose the page.
- **If the control URL shows the health check instead of the page,** add `?diag=1` to it. It lists, as yes/no only: code version, `Control` file present, owner recorded (if no, run `setup`), your email readable (if no, approve the email permission), and whether you're the owner.
- If the page says "unable to open the file", your phone's browser is probably signed in to several Google accounts. Make the owner account the only one, or the first one, in that browser.
- After each `Code.gs` update, publish a new version of **both** deployments, choosing the same access as before for each.

## 3. Bridge setup portal

1. Power the ESP32. On your phone or laptop, join **`YiCam-Setup`** with the password from step 1.
2. A setup page should open by itself. If it doesn't, browse to **http://192.168.4.1**.
3. Tap **Configure WiFi** and fill in:

   | Field | What to enter |
   |---|---|
   | Wi-Fi network / password | your 2.4 GHz network |
   | Camera IP / host | the camera's address |
   | Camera web user / password | the yi-hack web login, if you set one |
   | Snapshot resolution | `high` or `low` |
   | Apps Script web app URL / token | from step 2 |
   | MQTT user / password | make up a pair. The camera will use them to connect to the bridge (step 6). |
   | Motion topic | `yicam/motion_detection` (the yi-hack default) |
   | Seconds between shots / Max shots per event | for example `3` and `10` |
   | POSIX timezone | your timezone in POSIX form, for example `<-03>3` (UTC-3), `CET-1CEST,M3.5.0,M10.5.0/3` (Central Europe) or `EST5EDT,M3.2.0,M11.1.0` (US Eastern) |
   | OTA update password | for firmware updates over Wi-Fi |
   | Dashboard password | optional here; you can also set it from the dashboard (step 4). At least 12 characters. |

4. Click **Save**. The bridge joins your Wi-Fi and the setup network disappears.

The portal closes after 10 minutes and the board restarts; it reopens until the setup is saved. Later on you'll rarely need it: everything is also in the dashboard's **Settings**.

## 4. Dashboard and sign-in

Open **http://yicam-bridge.local**, or the bridge's IP from your router.

1. Until a dashboard password exists, a warning offers **Set a password now**. Choose a strong one, at least 12 characters, and let your browser's password manager save it. You're signed in for 30 days.
2. From then on, the dashboard, Settings, camera preview, snapshot and restart all require sign-in. Saving settings asks for the current password again.

Notes:
- Only a salted PBKDF2 hash of the password is stored on the ESP32. Setting a new password signs out every device.
- After 5 wrong attempts, sign-in locks for 30 seconds, doubling with each further miss (up to 16 minutes).
- This runs over plain HTTP on your home network, like the camera's own web login. It keeps other people on your Wi-Fi out, but isn't encrypted.
- Passkeys aren't possible: browsers only allow them on HTTPS sites with a domain name.

## 5. Give the bridge a fixed IP

In your router, add a DHCP reservation for **`yicam-bridge`** so the camera can always reach it. Reserving the camera's address too is a good idea.

## 6. Point the camera at the bridge

In the yi-hack web UI, open **Configuration → MQTT**:

| Setting | Value |
|---|---|
| Enable MQTT | on |
| Broker IP / host | the bridge's IP |
| Port | `1883` |
| Username / password | the MQTT pair from step 3 |
| Topic prefix | `yicam` |
| Motion topic suffix | `motion_detection` |
| Topic suffix for motion image | **empty**. Otherwise the camera pushes a full JPEG to the bridge at every motion start, which the bridge doesn't use. |
| Topic suffix for video list message | `motion_files` (the default). The bridge uses it to upload each event's clips. |

Save. The dashboard's **Camera connected** line should turn green within a minute or two. yi-hack connects with two MQTT clients. If it stays "No", restart the camera so its MQTT client picks up the new settings.

## 7. Test

1. On the dashboard, press **Take snapshot**. **Captured** and then **Uploaded to Drive** go up, and the photo appears in Drive under `YiCam/<today>/`.
2. Walk in front of the camera. Photos arrive every *Seconds between shots* while there's motion.
3. About 70 seconds after motion stops, the camera announces the event's video clips. The **Video clips** row shows them waiting, then uploading. A 5 MB clip takes about a minute.

## Changing settings later

Open **Settings** on the dashboard. Saving restarts the bridge (about 10 seconds).

- **Secrets** are never shown; leave a field blank to keep its value.
- **Changing Wi-Fi:** pick a network from the scan (or **Other network…**). The bridge tries it 3 times, about a minute. If it can't join, it **returns to the previous network by itself**; only if both fail does it open the setup network.
- **The setup network's password** is in the Wi-Fi card (**Show**).
- **Recovery:** hold **BOOT** while pressing **EN** to open the setup portal, for example if you forget the dashboard password.

## Updating

**Firmware over Wi-Fi**, with the OTA password from step 3:

```bash
PLATFORMIO_UPLOAD_FLAGS="--auth=<ota-password>" pio run -e ota -t upload
```

**Firmware over USB:** `pio run -e usb -t upload`. Settings, sessions and pending uploads are kept.

**Apps Script:** paste the new `Code.gs` (plus `appsscript.json` and `Control.html` if they changed), then **Deploy → Manage deployments → New version** for each deployment. Bridge firmware 0.5 and later needs a script with the `token`, `folder` and `saved` requests. With an older script, the dashboard shows "the Apps Script is out of date".

## Troubleshooting

Start with **Last error** on the dashboard (`last_error` in the raw JSON). For the live log, connect over USB and run `pio device monitor`. Opening the port restarts the board.

| Symptom | Likely cause and fix |
|---|---|
| `capture: … HTTP 401` | The camera has a web login the bridge doesn't know. Set the camera user and password in Settings. |
| `capture: the camera didn't save the snapshot` | The camera is offline or the address is wrong, or its yi-hack doesn't support `snapshot.sh?file=`. |
| `the Apps Script is out of date` | Paste the current `Code.gs` and deploy a new version. |
| `the Apps Script rejected the token` | The token in Settings doesn't match the `TOKEN` script property. |
| `no answer from the Apps Script` | The web app isn't deployed with access **Anyone**, or the URL isn't the `/exec` one. |
| `Drive answered HTTP 401/403` | Usually temporary (expired token); the bridge fetches a new one. If it persists, run `setup` again in the editor to re-approve permissions. |
| **Motion events** stays at 0 | The camera isn't reaching the broker. Check the MQTT settings (step 6). The serial log shows `[mqtt] client connected` or `rejected`. |
| **Dropped** goes up | All 30 photo slots are waiting, usually because Drive was unreachable for a while. Uploads catch up once it's back. |
| No **Video clips** after motion | Check `video_last_msg` in the raw JSON. If it's empty, the camera didn't announce a clip: check its video-list topic (step 6) and that it records on motion. |
| Control page shows the bridge **offline** | Check **Remote control** in the bridge's Settings and the dashboard's "Remote control" line. With a script older than the control page, the bridge checks in only every 10 minutes: update `Code.gs` and deploy a new version. |
| The bridge never joins Wi-Fi | It only supports 2.4 GHz. Use the setup portal (BOOT + EN) to fix the network and password. |

## Camera notes

- yi-hack-Allwinner runs from the camera's microSD card (FAT32). Taking the card out returns the camera to its stock firmware.
- Snapshots use the camera's RAM. If the camera restarts during busy motion, use `low` snapshot resolution, fewer shots per event, or yi-hack's swap file option.
- The bridge saves photos on the camera's card under 30 reused names (`yicam-bridge-00.jpg` … `-29.jpg`, about 8 MB in total) until they're in Drive.
