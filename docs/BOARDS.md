# Supported boards

PicoCam is **not tied to a board name**. At start-up it reads what the hardware reports and adapts. System > Device shows the
result and `GET /api/status` returns it under `device.board`.

| Detected | From | Used for |
|---|---|---|
| Chip (`RV1103` / `RV1106`) | `/proc/device-tree/compatible` | model conversion hint (`--platform`), diagnostics |
| RAM | `MemTotal` + `CmaTotal`, rounded to 64/128/256 MB | model size limit, media-memory choices |
| Boot medium: `spi-nand`, `emmc`, `sd` | kernel command line, `/proc/mtd` | flash-wear policy, SD slot discovery |
| Ethernet / Wi-Fi | `/sys/class/net` | network settings |
| Camera module and largest picture | the ISP (`rkaiq`) | resolution list and limits; a saved size that is too large is scaled down |
| SD slot | `/sys/block/mmcblk*/device/type` | recording; works whether the slot is `mmcblk0` or `mmcblk1` |

## Flash types

**SPI NAND (raw NAND, UBIFS).** Small partitions and limited write cycles. PicoCam therefore

- keeps event pictures in **RAM** (`/tmp`) when there is no SD card, unless you enable *Keep event pictures in internal flash*
  (Storage page). Insert an SD card for permanent history and recordings;
- writes settings only on change (image sliders apply live and save once);
- checks free space before an update and drops the rollback copy rather than fail on a full partition;
- installs without the bundled demo models when `/oem` is too small (the installer decides; or pass `--no-models`, or use the
  `-lite` package).

**eMMC.** Treated like a disk: no restrictions.

**SD-card boot.** The card is the system disk; recordings use its free space and the card cannot be formatted from the UI.

## Media memory (CMA)

The ISP, encoders and NPU share a fixed pool. Offered sizes follow the RAM: 24/28/32 MB on 64 MB boards, up to 40 MB with 128 MB and
up to 64 MB with 256 MB. Changing it edits the U-Boot environment, which works on SD-boot and eMMC images that expose the `env`
partition. On SPI-NAND images the page tells you to change `rk_dma_heap_cma` in the SDK and reflash.

## AI models are chip specific

See also [AI models](MODELS.md).

`.rknn` files are compiled for one chip. The bundled demo models are built for **RV1103**. On an RV1106 board convert your own (or
rebuild the demos) with `models/convert.py --platform rv1106`; loading a wrong-chip model fails with a message that says so.

## Camera modules

The firmware reads the sensor name and size from the ISP; the IQ tuning files come from the vendor image
(`/oem/usr/share/iqfiles`). Any module the stock `rkipc` can drive should come up. Main-stream choices follow the module.

## USB network

Over USB the board is `172.32.0.93`. Give your PC's USB network adapter a fixed address in the same subnet (for example
`172.32.0.100`, mask `255.255.255.0`). The camera's USB address can be changed under **Network > USB link**; the change must be
confirmed within 60 seconds or it reverts.

## Verification status

| Board | Status |
|---|---|
| Luckfox Pico (RV1103, SC3336) | developed and used by the maintainers |
| Luckfox Pico Plus (Ethernet) | developed and used by the maintainers |
| everything else | same code, **not verified on hardware**. Reports welcome |

Worth checking on a new board: the portal opens and live view plays; System > Security shows the watchdog armed; the SD card mounts
and survives removal; IR-cut / IR-LED GPIO numbers (Image > Day/Night) are board specific and not auto-detected.

## Checking your board

On the board, `sh /oem/picocam/doctor.sh` prints the detected hardware, free space, what is running and what looks wrong. The web UI shows the same
under *System > Overview*.

## Reporting a board

If you try PicoCam on a board not marked as verified, please open a [Board report](https://github.com/anand34577/luckfox-picocam/issues/new?template=board-report.yml). It takes two
minutes and tells the next person whether their board works.
