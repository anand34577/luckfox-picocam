# Troubleshooting

Start here whenever something does not work. Almost every problem is listed below. If yours is not, run the self-check on the board,
it names the likely cause in plain words:

```sh
ssh root@<board-ip>
sh /oem/picocam/doctor.sh
```

Paste its output into an issue if you need help.

**Jump to:** [Connecting](#connecting) · [Flashing](#flashing-the-board) · [Installing](#installing) · [Web page](#the-web-page) ·
[Video](#video) · [AI](#ai) · [Storage](#storage-and-recording) · [Network](#network) · [Updates and recovery](#updates-and-recovery)

---

## Connecting

### I can't reach 172.32.0.93
Work through this in order:

1. **Cable.** Many USB cables only charge. Try another one that you know transfers data, and a different USB port (a port on the PC itself,
   not a hub).
2. **Wait.** After power-on the board needs 20-30 seconds before the USB network appears.
3. **A new network adapter must appear** on your PC. Windows: *Settings > Network & Internet > Ethernet*; if instead Device Manager shows
   "Unknown device" or "USB Serial Device", install the driver by hand ([Install guide, step 2](INSTALL.md#step-2-connect-the-board-to-your-pc)).
   Linux: `ip -br link` shows a new `enx...` or `usb0`. **macOS has no driver for it**: use a board with Ethernet.
4. **Give the PC side a fixed address**: `172.32.0.100`, mask `255.255.255.0`, no gateway. The board does not hand out addresses over USB.
5. `ping 172.32.0.93`. Still nothing? Disconnect other VPNs and virtual adapters that use `172.32.x.x` (some Docker/VPN setups do), and
   check that the PC's Ethernet firewall profile allows ping.
6. If you changed the camera's USB address earlier in *Network & Time*, it is no longer 172.32.0.93. Use the new one, or reach the board
   over Ethernet and reset it.

### ssh says "Permission denied"
The password is `luckfox` on a fresh Luckfox image. **After PicoCam's first start the root password is the admin password you chose**
(unless you turned that off in *System > Security*). Typing a wrong password too often can lock the login for a while.

### ssh says "REMOTE HOST IDENTIFICATION HAS CHANGED"
You re-flashed the board or connected a different one at the same address, so the stored fingerprint does not match. The deploy scripts
ignore this. For a manual `ssh`: `ssh-keygen -R 172.32.0.93` (or your board's address), then connect again.

### "Connection timed out" over the network cable
Look up the board's address in your router's client list. Boards can take up to a minute to get an address. If your router isolates
devices ("AP isolation" / "client isolation"), turn that off or use the USB cable.

---

## Flashing the board

### The flashing tool does not see the board
Hold the **BOOT** button *before* plugging in USB and release it after a second. Install the Rockchip USB driver (DriverAssistant on
Windows). Try another cable and port. The tool should report a *Loader* or *Maskrom* device.

### The board does not boot after flashing
Wait a full minute. Use the image that matches your board **and** its storage (SPI NAND / eMMC / SD). Re-flash with the complete
`update.img`. If you use an SD card, make sure the image was written completely (re-write it) and the card is in before power-on.

### I flashed the Ubuntu image
PicoCam needs the Buildroot image. Flash it ([Install guide, step 1](INSTALL.md#step-1-put-the-luckfox-buildroot-image-on-the-board)).

---

## Installing

### The installer says "not enough space in /oem"
The board's flash is small. Use the lite package (`deploy.sh --lite`, `deploy.ps1 -Lite`) and add only the models you need later
([Models](MODELS.md)). Free space: `df -h /oem /userdata`.

### The installer says "/oem is read-only"
Run `mount -o remount,rw /oem` on the board, then the installer again.

### The installer says "this is the Ubuntu image"
Flash the Buildroot image first. See above.

### "No way to start PicoCam at boot on this image"
Your image has a read-only `/etc` *and* an unfamiliar launcher script. Please open an issue and include the output of
`cat /oem/usr/bin/RkLunch.sh; ls /etc/init.d /oem/usr/etc/init.d`; this is fixable but needs your layout.

### PowerShell says "running scripts is disabled"
`powershell -ExecutionPolicy Bypass -File .\picocam\tools\deploy.ps1`

### "The OpenSSH client is missing" (Windows)
*Settings > Apps > Optional features > Add a feature > OpenSSH Client*, then run it again.

### The download failed
The scripts fetch the newest release from GitHub. Check your internet connection, or download `picocam.tar.gz` yourself from the
[Releases page](https://github.com/anand34577/luckfox-picocam/releases) and run `deploy.sh -p picocam.tar.gz`.

---

## The web page

### The page does not open
1. Is the service running? `sh /oem/picocam/doctor.sh` on the board tells you. Start it with `sh /oem/picocam/service.sh restart`.
2. The first start after install or update takes up to a minute (a new HTTPS certificate is created).
3. Wrong port? The portal is on `http://<ip>/` (80) and `https://<ip>/` (443). If 80 was busy it uses 8080: `http://<ip>:8080/`.
4. Your PC and the board must be in the same network (see *Connecting*).

### "Your connection is not private" in the browser
Expected: the camera signs its own HTTPS certificate. *Advanced > Continue*, or use `http://`, or install your own certificate in
*System > Security*.

### I forgot the admin password
`ssh root@<board-ip>`, then `sh /oem/picocam/doctor.sh reset-password`. It removes the accounts only; settings and recordings stay.

### Login keeps failing
After several wrong passwords the login is locked for a short time (it protects against guessing). Wait a few minutes. Passwords are case sensitive.

### The page shows "Reconnecting..." at the bottom left
The live connection to the camera dropped (reboot, network change or a restart of the video after changing settings). It reconnects by itself.

---

## Video

### Live view does not play
- **It says "Connecting" forever:** the camera may be restarting its video (changing resolution or AI does that); wait ten seconds. Press
  **Reconnect** (on the picture) once. If it stays, run `doctor.sh`: *"no /dev/video*"* means the camera module is not detected.
- **"This browser cannot play H.265":** set the stream to H.264, or use Chrome/Edge.
- **Black picture:** check the lens cap and, at night, whether the scene is simply dark. *Image > Scene > Low light* helps.
- **Delay of a second or more:** you are probably on plain HTTP over a slow link, or a busy Wi-Fi. Try the sub stream or HTTPS.

### The camera module is not detected (`no /dev/video*`)
Power off. Reseat the flat cable: lift the latch, push the cable in fully and straight, with the contacts facing the way Luckfox's wiring picture for your board shows, then close the latch. Make sure the module
is one the Luckfox image supports. Power on again. Do not connect or disconnect the module while the board is powered.

### No video after I changed a setting
Look at the **AI** page for a media-memory message. Lower the main-stream resolution or the number of AI stages, or raise the media memory
where your board allows it.

### The picture is too bright/dark, or flickers
*Image > Scene*: pick the right place. Flicker under indoor lights: *Exposure > anti-flicker* to 50 Hz (or 60 Hz in the Americas). Blown-out
sky: leave *Smart Exposure* on.

### The picture is upside-down
*Image* has mirror and flip.

### The time on the picture is wrong
The board has no battery clock. Log in as admin once (the browser sets it), or *Network & Time > Sync*. Set the time zone there too.

### VLC lags
Network caching to 150-300 ms. See [Connecting](CONNECTING.md#watching-video).

### An NVR or app cannot find the camera
Add it by address (ONVIF port 80). Automatic discovery needs the camera and the NVR in the same network segment and multicast allowed.

---

## AI

### AI does not start
Open *AI & Analytics*; the *Engine* panel says why. The usual reasons:
- **Not enough media memory.** Lower the main resolution, remove a stage, or raise the media memory ([Boards](BOARDS.md#media-memory-cma)).
- **Model not valid.** Wrong chip. Convert with `--platform rv1103` or `rv1106` to match *System > Overview* ([Models](MODELS.md)).
- **No model chosen.** Pick a template, or choose a detector.

### Too many false detections / missing detections
Raise or lower *Confidence threshold*; draw a **detection area** so the model only looks at the road or gate; ignore tiny boxes
(*Ignore boxes smaller than*). Good light and a steady camera matter more than any setting.

### Plates are wrong at night
Use *Image > Scene > Road / ANPR* (short shutter) and add light. A short shutter in darkness needs an IR lamp or street light.

---

## Storage and recording

### "SD card unreadable"
Probably exFAT. *Storage & Recording > Format* (FAT32 or ext4). This erases the card.

### Nothing is recorded
Check *Storage & Recording*: recording mode must not be *off*, the card must show free space, and for *on events* an AI rule or motion detection
must trigger. A *schedule* only records in painted hours.

### "Frames dropped: the card is too slow"
Use a Class 10 / U1 or better card, or record the sub stream, or lower the main bitrate.

### Events are gone after a reboot
On SPI NAND boards without an SD card, pictures are kept in RAM on purpose. Insert an SD card, or enable *Keep event pictures in internal flash* (this
wears the flash).

### The card is not detected after pulling and re-inserting
Press **Mount** on the Storage page. If it is a different card, it mounts by itself within a few seconds.

---

## Network

### I changed the IP address and lost the camera
Unless you press *Keep new settings* within 60 seconds on the new address, the old settings come back. If that did not help, use the
other interface (USB cable `172.32.0.93`) to correct them, or `sh /oem/picocam/doctor.sh factory-reset` on the board.

### Port 80/443/554 is already used
PicoCam uses the next free web port (80, then 8080) so the page is never lost. Change ports in *Security* / *Integrations*. A leftover vendor
app (`rkipc`) holding RTSP: reboot once; PicoCam stops it at start-up.

### Webhooks/Telegram fail with a certificate error
The camera checks HTTPS certificates. A **wrong clock** makes every certificate look expired: fix the time first. For a server with its own
certificate, choose *pin fingerprint* or *skip verification* in that target's settings.

---

## Updates and recovery

### The update from the web page failed with "not enough free space"
The updater needs about three times the package size on `/oem` (current version, previous version, new version). It drops the rollback copy
by itself if that is what it takes; if it still does not fit, use the lite package.

### The camera keeps restarting after an update
It rolls back to the previous version by itself after several quick crashes. You can also press *Roll back* (System > Maintenance). Details:
`/userdata/picocam/crash.log` and `sh /oem/picocam/doctor.sh`.

### Everything is broken: start over
```sh
sh /oem/picocam/doctor.sh factory-reset        # default settings, keeps accounts/models/recordings
sh /oem/picocam/uninstall.sh --purge && reboot # remove PicoCam completely, back to the stock camera app
```
Then reinstall with `deploy.sh`. Last resort: re-flash the Luckfox image.

### Where are the logs?
Web: *System > Logs*. Board: `tail -f /tmp/picocam.log`. Crash reports survive reboots in `/userdata/picocam/crash.log`.

---

Still stuck? [Open an issue](https://github.com/anand34577/luckfox-picocam/issues/new/choose) with the output of `sh /oem/picocam/doctor.sh`.
