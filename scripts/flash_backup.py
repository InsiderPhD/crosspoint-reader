# Read the whole flash chip to device-backups/<board>-<mac>.bin before the
# first upload to a device, and refuse to upload if that backup fails.
#
# For boards that ship vendor firmware nobody can download again (Onyx Picco):
# once our image is written there is no way back without a dump taken first.
# One backup per device (keyed on its MAC); later uploads skip the read. Set
# FLASH_BACKUP_SKIP=1 to upload without one (you will not get the stock
# firmware back). device-backups/ is gitignored: it holds vendor firmware.
import os
import re
import shlex
import subprocess

Import("env")

FLASH_SIZE = "0x1000000"  # 16 MB: the whole chip, bootloader to coredump


def _esptool(env, port, *args):
    cmd = shlex.split(env.subst("$UPLOADER")) + ["--chip", env.BoardConfig().get("build.mcu"), "--port", port]
    return cmd + list(args)


def backup_before_upload(source, target, env):
    if os.environ.get("FLASH_BACKUP_SKIP") == "1":
        print("flash_backup: FLASH_BACKUP_SKIP=1, uploading WITHOUT a backup")
        return
    env.AutodetectUploadPort()
    port = env.subst("$UPLOAD_PORT")
    if not port:
        print("flash_backup: no upload port found; refusing to upload without a backup")
        env.Exit(1)

    try:
        out = subprocess.run(_esptool(env, port, "read-mac"), capture_output=True, text=True, check=True).stdout
    except (OSError, subprocess.CalledProcessError) as exc:
        print("flash_backup: could not read the device MAC (%s); refusing to upload" % exc)
        env.Exit(1)
    match = re.search(r"MAC:\s*([0-9a-fA-F:]{17})", out)
    if not match:
        print("flash_backup: no MAC in esptool output; refusing to upload\n" + out)
        env.Exit(1)
    mac = match.group(1).replace(":", "").lower()

    backup_dir = os.path.join(env.subst("$PROJECT_DIR"), "device-backups")
    # Keyed on the board, not the env: picco and picco-relayout share one backup.
    board = env.subst("$PIOENV").split("-")[0]
    path = os.path.join(backup_dir, "%s-%s.bin" % (board, mac))
    if os.path.isfile(path) and os.path.getsize(path) == int(FLASH_SIZE, 16):
        print("flash_backup: backup already on disk: %s" % path)
        return

    os.makedirs(backup_dir, exist_ok=True)
    partial = path + ".part"
    print("flash_backup: first upload to %s -- reading the full %s flash to %s (a few minutes)" % (mac, FLASH_SIZE, path))
    rc = subprocess.call(_esptool(env, port, "--baud", "921600", "read-flash", "0", FLASH_SIZE, partial))
    if rc != 0 or not os.path.isfile(partial) or os.path.getsize(partial) != int(FLASH_SIZE, 16):
        print("flash_backup: backup FAILED (esptool exit %d); nothing was written to the device" % rc)
        env.Exit(1)
    os.replace(partial, path)
    print("flash_backup: saved %s -- keep it somewhere safe; restore with:\n"
          "  esptool.py --chip esp32s3 write-flash 0 %s" % (path, path))


env.AddPreAction("upload", backup_before_upload)
