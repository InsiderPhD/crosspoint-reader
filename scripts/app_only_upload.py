# Upload only the application image, at the ota_0 offset taken from the
# partition table. For boards that keep their vendor bootloader, partition
# table, otadata and recovery partition (Onyx Picco): pio's default upload
# would also write the Arduino bootloader, partition table and boot_app0.bin.
#
# The platform builder has already expanded FLASH_EXTRA_IMAGES into
# UPLOADERFLAGS by the time a post: script runs, so clearing the list alone
# changes nothing; the (offset, image) pairs are stripped from the flags too.
Import("env")

extra = {env.subst(str(image)) for _, image in env.get("FLASH_EXTRA_IMAGES", [])}
env.Replace(FLASH_EXTRA_IMAGES=[])

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
print("app_only_upload: writing only the app at $ESP32_APP_OFFSET (%d extra image(s) skipped)" % len(extra))
