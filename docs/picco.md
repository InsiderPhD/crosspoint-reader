# Onyx BOOX Picco (experimental)

The Picco (SDK name "Onyx Picco / BOOX Tiles") is an ESP32-S3 e-reader with a
3.97" 800x480 panel, touch, a capacitive home pad, a warm/cool frontlight and a
slide switch. It builds as its own binary, `[env:picco]`, and runs the X4 Pro's
Full Touch UI.

## Before you flash: back up the stock firmware

**Onyx's firmware cannot be downloaded anywhere.** Once you overwrite it, the
only way back is a dump of your own device taken *before* the first flash.

`pio run -e picco -t upload` does this for you: `scripts/flash_backup.py` reads
the whole 16 MB chip to `device-backups/picco-<mac>.bin` before the first
upload to each device, and refuses to upload if that read fails. Later uploads
see the file and skip the read. Copy the file somewhere safe. It is gitignored,
because it contains Onyx's firmware: never commit or share it.

If you flash with anything other than that pio command, take the backup by hand:

```sh
esptool.py --chip esp32s3 --baud 921600 read-flash 0 0x1000000 picco-stock.bin
```

Restore it with `esptool.py --chip esp32s3 write-flash 0 picco-stock.bin`.

`FLASH_BACKUP_SKIP=1 pio run -e picco -t upload` uploads without a backup. Only
use it if you already have one.

## Getting into download mode

esptool normally resets the Picco into the ROM bootloader over USB on its own.
GPIO0, the usual BOOT strap, is a power-rail enable on this board with no button
on it. If `esptool.py --chip esp32s3 chip-id` cannot connect, try again with
the device in a different state (awake on its home screen, or charging while
off). Check `espefuse.py --chip esp32s3 summary` before the first flash:
`SECURE_BOOT_EN` must be off and `SPI_BOOT_CRYPT_CNT` 0, or a self-built image
will not boot.

## Partition layout: stock, app only

The build uses Onyx's own partition table (`partitions_picco.csv`, read from a
retail unit, via upstream CrossPoint), and `scripts/app_only_upload.py` writes
only the app into the stock `app` (ota_0) slot at `0xE0000`. Onyx's bootloader,
NVS and `safeboot` recovery partition are left alone.

There is a single app slot, so the Picco cannot update itself: "Check for
updates", "SD firmware update" and the Down+Power recovery boot are disabled,
and `FirmwareFlasher` refuses to write over the running app. Update over USB.

### A Picco that was flashed before this layout existed

If a device was flashed with a full upload of an older `[env:picco]` (our own
partition table and bootloader), an app-only upload to `0xE0000` will never
boot. Run the one-off relayout once, then go back to `[env:picco]`:

```sh
pio run -e picco-relayout -t upload
```

It writes the Arduino bootloader, the stock partition table, otadata selecting
`ota_0` and the app. It cannot bring back Onyx's bootloader or `safeboot` image:
only a backup can.

## Controls

The Picco has no home button and no front keys: Power, Up and Down, the
touchscreen and the slide switch.

- **In a book, a single tap of Power opens the book menu** (its Home row leaves
  the book). This is the Picco's default for Reader Controls > Power Short Press.
- **Swipe up from the bottom edge** goes Home outside a book. Inside one it runs
  the Home Button Short Press action.
- **Slide Switch** (Reader Controls) is bound to an on/off function: Frontlight,
  Dark, Status or Disable Page-Turn Gestures. The switch's position sets it.
- Reader Controls always keeps at least one reachable control on the book menu,
  Home or File Browser. A pick that would remove the last one is refused, and a
  settings file with none (for example one copied from an X4 Pro) gets Power
  short press set back to the book menu at boot.

## Status

Built against the freeink-sdk Picco profile, with upstream CrossPoint's Picco
work ported (charger setup, stock layout, slide switch, plugged-in sleep with a
charge strip, touch-controller sleep). Unverified on hardware:

- **Swipe directions.** Assumed to share the tap frame
  (`MappedInputManager::processTouchInput`).
- **GPIO5.** The SDK carries it as a provisional Confirm input; nothing on the
  front of the device is known to drive it.
