# Publishing StickPet to M5Burner

M5Burner firmware is published **from inside the M5Burner app**, not from GitHub.
The GitHub repo is only an optional link on the publish form. You need three
things: a compiled binary, an M5Stack community account, and a cover image.

## 1. Prepare a release build

At the top of `stickpet/stickpet.ino`, turn off the dev tooling:

```cpp
#define STICKPET_LOG 0
```

That compiles out the serial logging and the single-key cheats (`k`, `b`, etc.)
so they can't be triggered on a shipped unit.

## 2. Export the binary

In Arduino IDE, with the StickS3 board settings from the README selected:

**Sketch → Export Compiled Binary**

When it finishes, open the sketch folder (**Sketch → Show Sketch Folder**) and
look in `build/esp32.esp32.esp32s3/`. You want the **merged** image:

```
stickpet.ino.merged.bin
```

That single file contains the bootloader, partition table, and app as one
full-flash image written at offset `0x0` — which is what M5Burner expects.
(If your ESP32 core version doesn't produce a `.merged.bin`, update the ESP32
Arduino core to 3.x, which does.)

## 3. Make a cover image

M5Burner shows a cover thumbnail. A simple square PNG (e.g. 512×512) of the pet
or the title screen works — a phone photo of the running device is fine.

## 4. Publish

1. Open **M5Burner**, log in (top-right) with your **M5Stack community account**
2. Bottom-left: **USER CUSTOM → Publish**
3. Fill in the form:
   - **Name**: StickPet
   - **Version**: e.g. 1.0.0
   - **Description**: what it is + the controls (crib from the README)
   - **Device Type**: StickS3
   - **GitHub**: your repo URL (optional but recommended)
   - **FirmWare**: the `stickpet.ino.merged.bin` from step 2
   - **Cover**: your PNG from step 3
4. **Upload**

After it's live you can also generate a **Share Code** so people can burn it
without searching.

## Notes

- Test the exact binary you upload by flashing it once yourself first.
- Bump the **Version** each time you re-publish so users see the update.
