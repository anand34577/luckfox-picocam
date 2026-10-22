# Changelog

## 1.4.0

- **No more green picture or dead exposure after a settings change.** Saving Video & Audio settings, changing an AI model or a
  scheduled AI profile restarts the video; the camera's colour and exposure engine now restarts with it in the order the chip needs,
  so the picture comes back right instead of green with frozen exposure until a reboot.
- **Self-healing picture:** if the picture still turns green, exposure stops reacting, or the image processor stops answering, the
  camera restarts its video pipeline by itself (a few seconds' break, with growing pauses if it keeps happening) and records it in the
  audit log. Image > status shows the picture health. A video pipeline that fails to start no longer stays dead: the watchdog resets
  the board.
- **Steadier Smart Exposure:** corrections are judged only on pictures taken after the previous one took effect, a passing car or
  headlight no longer moves the whole scene, brightness is measured more finely, and the exposure boost no longer builds up at night
  (mornings no longer start over-exposed). Auto anti-flicker also switches off in bright daylight.
- **Manual white balance** no longer drifts with every slider move: the sliders scale the colours auto white balance had settled on.
- **Less RTSP delay:** a viewer that falls behind skips to a fresh picture once about 0.4 s is waiting (counting what the network
  stack holds too) instead of building up 2-3 s. For the lowest delay also set the player's own buffer low (VLC: Network caching 200 ms).
- **Faster AI on the same NPU:** the picture is scaled and converted for the model by the RGA 2D engine instead of the CPU, checked
  against the CPU result on first use, with automatic CPU fallback (`ai.preprocess`: `auto` or `cpu`). AI & Analytics shows the time
  spent preparing the picture and on the NPU.
- **On-screen text:** one switch turns off the timestamp and camera name together. Turning "Burn AI boxes into the sub stream" on or
  off no longer restarts the video, and the web live view no longer draws a second set of boxes on a sub stream that has them burned in.
- **Sign-in:** you are no longer logged out when the camera sets its clock (from NTP or the browser) after a reboot.

## 1.3.0

- **EdgeAI server: an API for your own application.** Every face and vehicle detection comes with its pictures (whole frame, cropped
  face, cropped plate, cropped vehicle), the person's name once saved, and the plate number with its validity and confidence. People,
  vehicles (one per plate, with owner, label and an allow/block flag), reports, per-camera timelines, CSV export and webhooks are
  available over REST. The full reference is in `docs/API.md`, and the server serves an OpenAPI spec at `/api/v1/openapi.json`.
- **Alerts:** rules send a message to Telegram, ntfy, email or any web address when a blocked plate or an unknown face is seen,
  at certain hours, or when a camera goes offline, runs hot or the server disk fills up. Each rule has a cooldown so one visitor is one alert.
- **Housekeeping:** automatic cleanup by age and by disk size (named people and labelled plates are kept longer), a backup download,
  an audit log of changes, and a call that erases a person or a plate completely.
- **Safer access:** several API keys with read-only or admin rights and an expiry, a sign-in page for the dashboard, expiring signed picture
  links, and a limit on wrong-key attempts. The server still starts open unless you give it `-api-key`.
- **Firmware:** the heartbeat to the EdgeAI server also reports temperature, CPU and NPU load, which the new health alerts use.
  Nothing changes on the camera when EdgeAI is off.

## 1.2.3

- **Docs:** the screenshots, banner and social preview show a real street photo instead of a drawn scene, so the AI boxes and plate
  reads look the way they do on a real camera. The photo is a Pexels stock image (credited in `NOTICE.md`), not PicoCam footage; its
  number plates are covered with fictional ones. No firmware or web UI changes.

## 1.2.2

- **Safer downloads:** the deploy scripts verify the download against the release's `SHA256SUMS`. Releases carry a build provenance
  attestation (`gh attestation verify picocam.tar.gz --repo anand34577/luckfox-picocam`).
- **Small flash:** the deploy scripts pick where to unpack by free space (`/userdata`, else RAM, else a clear message) instead of failing
  half way. The log kept in flash is now capped (30 lines per 5 minutes) and the known-good settings copy is only rewritten when it changed.
- **Tested end to end on a fake board in CI:** `deploy.sh` through a fake ssh into the installer, including the low-space fallback and the
  models-only path. `deploy.ps1` was checked against a fake ssh on Windows. Unit tests for the board detection and sensor limits.
- **Project hygiene:** bug and feature templates, pull request checklist, Dependabot for Actions and npm, private vulnerability reporting,
  `.editorconfig`, current Action versions, and a CI check that `VERSION`, `package.json` and this changelog agree.
- **Docs:** screenshots of the first-run screens, and the password rule (8+ characters, letters and digits).

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
