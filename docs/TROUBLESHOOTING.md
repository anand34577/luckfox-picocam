# Troubleshooting

| Symptom | Fix |
|---|---|
| Can't reach `172.32.0.93` | Give your PC's USB network adapter a fixed address in the same subnet (e.g. `172.32.0.100`, mask `255.255.255.0`). The camera's USB address is changeable under **Network > USB link** |
| `ssh` asks for a password | Default is `root` / `luckfox`. After first setup the root password follows the admin password (System > Security) |
| Installer: "not enough space in /oem" | Use the lite package (`--lite`) or `sh install.sh --no-models`; upload your own models later |
| Installer: "/oem is read-only" | `mount -o remount,rw /oem`, then run it again |
| No video after a settings change | Check **AI** for a media-memory message; lower the main resolution or raise media memory |
| Clock shows 1970 | No RTC and no internet: log in as admin once, use *Network > Sync with this computer*, or point NTP at a LAN server |
| Event pictures vanish after a reboot | A SPI-NAND board without an SD card keeps them in RAM on purpose (see BOARDS.md); insert an SD card |
| Model: "not a valid RKNN model" | It was converted for the other chip; rebuild with `--platform` matching **System > Device > SoC** |
| Back to the stock firmware | `sh /oem/picocam/uninstall.sh && reboot` (add `--purge` to delete settings) |
| Logs | `ssh root@<ip> tail -f /tmp/picocam.log`, or **System > Logs**; crash reports persist in `/userdata/picocam/crash.log` |
