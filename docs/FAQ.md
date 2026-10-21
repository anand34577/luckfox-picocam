# Frequently asked questions

## Getting started

### Which boards work?
Luckfox Pico boards with an RV1103 or RV1106 chip (Pico, Mini, Plus, Pro, Max, Ultra and similar). The firmware detects the board by
itself. The author has tested the Pico and Pico Plus; others use the same code but are not yet verified on real hardware. See
[Boards](BOARDS.md), and please send a *Board report* if you try one.

### Do I need to compile anything?
No. `deploy.sh` / `deploy.ps1` download a ready-made release and install it. Compiling is only for developers ([Building](BUILDING.md)).

### Does it replace the whole system on the board?
No. It sits on top of the standard Luckfox Buildroot image and replaces only the camera app (`rkipc`). You can go back at any time:
`sh /oem/picocam/uninstall.sh && reboot`.

### Can I install it on the Ubuntu image?
No. PicoCam needs the Buildroot image (it uses its camera drivers). The installer tells you if it finds Ubuntu. Flash the Buildroot
image ([Install guide, step 1](INSTALL.md#step-1-put-the-luckfox-buildroot-image-on-the-board)).

### What are the default passwords?
Board login: user `root`, password `luckfox` (Luckfox's default). The PicoCam web page has no default: **you choose the admin
password at first start**, and by default it also becomes the `root` password. If you turn that off in *System > Security*, `root` keeps
its old password.

### Which camera modules work?
The one that came with your board (for example the 3 MP SC3336) and others that Luckfox's stock camera app supports. The sensor and
its largest resolution are detected automatically.

### Do I need an SD card?
Not for live view, RTSP, ONVIF, alerts or AI. You need one for **recording** and for a **permanent event history**.

## Connecting

### I can't reach 172.32.0.93.
See [Troubleshooting](TROUBLESHOOTING.md#i-cant-reach-1723293). Usually: a charge-only cable, or your PC has no fixed address
(`172.32.0.100`) on the new USB adapter.

### Can I change the USB address 172.32.0.93?
Yes: *Network & Time > USB link*. Confirm within 60 seconds or it reverts. Your PC's address must then be in the same network.

### Does it work over Wi-Fi?
On boards that have Wi-Fi hardware and a Wi-Fi-enabled image, PicoCam works over the Wi-Fi connection like any other network. It does not
set up the Wi-Fi password itself yet: configure Wi-Fi with Luckfox's tools first.

### Why does my browser say "the connection is not private"?
HTTPS uses a certificate the camera made for itself, which no browser knows. Choose *Advanced > Continue*. The connection is still
encrypted. You can upload your own certificate in *System > Security*, or just use `http://` on your home network. (The lowest-delay live view
needs HTTPS; with plain HTTP the page uses a slightly slower method.)

### Can I use it from outside my home?
Possible, not recommended to expose directly. Use a VPN (WireGuard, Tailscale) to your home network instead. If you must forward a port, use HTTPS,
a long unique password, and the IP allow-list in *System > Security*.

## Video and AI

### Why is there a delay in VLC?
VLC buffers one second by default. Set *Network caching* to 150-300 ms ([Connecting](CONNECTING.md#watching-video)). The web live view is about 0.3 s.

### The live view says my browser can't play H.265.
Use H.264 for the sub stream (*Video & Audio*) or a Chromium browser with hardware H.265. Recording and NVRs are fine with H.265.

### AI doesn't start / "does not fit".
The camera, encoders and AI share a fixed pool called media memory. If a pipeline is too big, video keeps working and the AI page says
why. Lower the main-stream resolution, use fewer stages, or raise the media memory where the board allows it
([Boards](BOARDS.md#media-memory-cma)).

### A model says "not a valid RKNN model".
It was converted for the other chip (RV1103 vs RV1106). Convert again with `--platform` matching *System > Overview*. See [Models](MODELS.md).

### Can it read my country's number plates?
The ready-made reader is trained on Indian plates. For other formats train or find a text-reading model and add it as your own model
([Models](MODELS.md)).

### Can it do audio?
No. These boards have no microphone.

## Storage

### Where do event pictures go?
To the SD card when there is one. Without a card: on **SPI NAND** boards they stay in RAM (lost on reboot) so the flash is not worn out; on
eMMC boards they go to internal storage. You can switch it in *Storage & Recording* on SPI NAND boards.

### My SD card is "unreadable".
It is probably exFAT (common for 64 GB and larger). Use **Format** in *Storage & Recording* (FAT32 or ext4). This erases the card.

### Can I pull the card while it runs?
Press **Safely remove** first. Recording files survive a sudden power cut (only the last few seconds are lost), but the card's file
system is happiest when it is unmounted.

### How long does the SD card last?
Constant recording writes a lot. Use a name-brand "high endurance" card for continuous recording, or record on events only.

## Security and accounts

### I forgot the admin password.
On the board: `ssh root@<board-ip>` then `sh /oem/picocam/doctor.sh reset-password`. It removes the accounts; the next visit to the web page asks for a
new password. Your settings and recordings stay. If you also forgot the board's root password (it was tied to the admin password), use ADB
(`adb shell`, if you left it on), or reflash.

### Is it safe to leave on my network?
It has login lockout, HTTPS, roles, an audit log and an IP allow-list, and every account change is logged. It still is a device on
your network: use a strong password and do not expose it to the internet. Report problems privately ([Security policy](../SECURITY.md)).

### Does it phone home?
No. It contacts only the services **you** configure (an NTP server for the clock, your webhooks/MQTT/Telegram/email, an update only when you start one).

## Updating and recovery

### How do I update?
*System > Maintenance > Update firmware* with the newest `picocam.tar.gz`, or run the deploy script again. Settings are kept.

### An update broke something.
The previous version is kept. If the new one keeps crashing the camera goes back by itself after a few minutes. You can also press
**Roll back** in *System > Maintenance*.

### The camera keeps restarting.
Run `sh /oem/picocam/doctor.sh` on the board; it lists the likely cause. The last crash report is in `/userdata/picocam/crash.log`. A setting that
keeps crashing the camera is replaced by the last settings that ran well.

### How do I reset everything?
*System > Overview > Factory reset* (keeps the network settings if you tick that), or on the board `sh /oem/picocam/doctor.sh factory-reset`.

### How do I remove PicoCam?
`sh /oem/picocam/uninstall.sh && reboot`. Add `--purge` to also delete the settings.

## Project

### Is it free?
Yes, MIT licence. The ready-made AI models have their own licences ([NOTICE](../NOTICE.md)).

### How can I help?
Try it on your board and send a *Board report*, report bugs, improve the docs, or send a pull request ([Contributing](../CONTRIBUTING.md)).
