# Changelog

## 1.2.1

- **Documentation rewritten for first-time users:** a step-by-step install and flashing guide (Windows, Linux, macOS notes, USB driver, SPI NAND /
  eMMC / SD), a user guide with pictures of every page, an FAQ, a troubleshooting guide, and pages on connecting other software, AI models and
  boards. The same pages are published to the project wiki.
- **Releases are built by GitHub Actions.** Pushing a version tag builds the firmware and web UI from source at pinned SDK revisions, runs the
  tests and publishes `picocam.tar.gz`, `picocam-lite.tar.gz`, `picocam-models.tar.gz` and `SHA256SUMS`.
- **Easier install:** the deploy scripts need one password prompt, ignore stale SSH host keys, check the download, and explain every failure in
  plain words. `deploy.sh --models` / `deploy.ps1 -Models` add the ready-made AI models to a lite install.
- **`doctor.sh` on the board:** a self-check that names the likely cause of a problem, plus `reset-password` and `factory-reset` recovery commands.
- **Installer fixes** found by simulating it on a fake board: installing the lite package no longer fails; the patched vendor launcher is
  syntax-checked before it replaces the original; the Ubuntu image is refused with an explanation; a missing device-tree file no longer aborts it.
- **UI:** the phone header no longer squeezes the page title; System > Overview shows memory and boot flash; a demo mode (`npm run mock`) shows the
  web UI without a board.
- Demo screenshots, banner and social preview are generated from made-up data; no real footage is in the repository.

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
