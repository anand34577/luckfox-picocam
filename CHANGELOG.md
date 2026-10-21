# Changelog

## 1.2.0

- **Boards:** run-time detection of chip, RAM, boot flash, Ethernet/Wi-Fi and camera module; one package for the whole Pico family
  (docs/BOARDS.md). Resolution limits and choices follow the sensor, the SD slot is found via sysfs, media-memory sizes follow the RAM.
- **SPI NAND:** event pictures stay in RAM without an SD card (opt-in for flash); an update checks free space and can drop the
  rollback copy; the installer sizes the install to `/oem` and there is a `-lite` package without models.
- **Network:** the USB link address (default 172.32.0.93) can now be changed in the UI, with the 60 s confirm-or-revert safety.
- **Installer:** preflight checks, read-only `/etc` fallback (start from the launcher hook), no scp needed, `deploy.sh` for
  Linux/macOS/WSL, `deploy.ps1` downloads releases; both stream the package over SSH.
- **UI fixes:** *Reconnect* in live view no longer loops forever; the status badges no longer cover the camera's time overlay.
- A wrong-chip `.rknn` model now produces an explanatory error; `convert.py --platform` is validated.
- Repo prepared for open source: MIT licence, CI, docs; real-world sample footage removed from the tree.

## 1.1.0 and earlier

See the git history.
