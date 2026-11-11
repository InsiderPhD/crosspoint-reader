# Upload only the application image, at the ota_0 offset taken from the
# partition table, plus boot_app0.bin (otadata selecting ota_0). For boards
# that keep the bootloader, partition table, NVS and recovery partition already
# on the device (Onyx Picco): pio's default upload would also write the Arduino
# bootloader and partition table.
#
# boot_app0 stays because the Picco table has two OTA slots: after an OTA or SD
# update the device boots ota_1, and an app written to ota_0 alone would never
# run. Set board_upload.arduino.boot_app0 to the table's otadata offset.
#
# The platform builder has already expanded FLASH_EXTRA_IMAGES into
# UPLOADERFLAGS by the time a post: script runs, so clearing the list alone
# changes nothing; the (offset, image) pairs are stripped from the flags too.
Import("env")

import os

images = env.get("FLASH_EXTRA_IMAGES", [])
keep = [(off, img) for off, img in images if os.path.basename(env.subst(str(img))) == "boot_app0.bin"]
extra = {env.subst(str(img)) for _, img in images} - {env.subst(str(img)) for _, img in keep}
env.Replace(FLASH_EXTRA_IMAGES=keep)

flags = list(env.get("UPLOADERFLAGS", []))
kept = []
i = 0
while i < len(flags):
    if i + 1 < len(flags) and env.subst(str(flags[i + 1])) in extra:
        i += 2  # drop "<offset> <image>"
        continue
    kept.append(flags[i])
    i += 1
env.Replace(UPLOADERFLAGS=kept)
print(
    "app_only_upload: writing the app at $ESP32_APP_OFFSET%s (%d extra image(s) skipped)"
    % (" and boot_app0 at %s" % keep[0][0] if keep else "", len(extra))
)
