# StickPet

A virtual pet **and** idle RPG for the [M5Stack StickS3](https://docs.m5stack.com/en/core/StickS3).
Care for PIXEL and it grows from an egg into an adult; send it off adventuring
and it fights blobs on its own — while a well-fed, happy pet fights harder.

Runs on the StickS3's built-in screen and speaker. **M5Unified is the only
dependency.** Everything is drawn from primitives to an off-screen canvas, so it
stays smooth and flicker-free, and the whole state persists to flash — your pet
survives a reboot.

---

## Features

- Care sim: **feed, play, rest, clean, heal**, with stats that decay over time
- Grows through four stages: **egg → baby → child → adult**
- Moods and expressions: happy, hungry, sad, sick, sleeping
- Neglect leads to **sickness**, then death — with clear warnings first
- **Idle RPG adventure mode**: fights level-scaled blobs, earns XP, gold, levels
- **Boss every 10th enemy**, rotating through three foes — a crab, a spider, and a deer
- Danger tie-in: a poorly-cared pet can actually **die in battle**
- Adventure stamina is the pet's own **energy** — it fights, tires, and rests
- **Mute** (hold the side button), persisted across reboots
- Sound only where it counts: action confirmations, level-ups, and evolutions

---

## Hardware

- **M5Stack StickS3** (ESP32-S3-PICO-1-N8R8) — SKU K150
- USB-C cable

No wiring, no add-ons. Uses the built-in 1.14" display, speaker, and two buttons.

---

## Controls

| Input | Action |
|-------|--------|
| Front button — tap | Move the cursor along the action bar / dismiss a report |
| Side button — tap  | Do the selected action |
| Front button — hold | Toggle **pet mode ⇄ adventure mode** |
| Side button — hold  | **Mute / unmute** |
| Either button, held on the gravestone | Hatch a new egg |

Adventure mode is a mode you choose, not something tied to the USB cable — hold
the front button to send the pet off and hold again to bring it home for its
loot report.

---

## Install

### Option A — M5Burner (easiest, once published)

1. Open [M5Burner](https://docs.m5stack.com/en/uiflow/m5burner/intro)
2. Search **StickPet** under the StickS3 device list
3. Burn it to your Stick

### Option B — Arduino IDE (from source)

1. Install the **ESP32 boards** package (Espressif) via Boards Manager
2. Install the **M5Unified** library via Library Manager
3. Open `stickpet/stickpet.ino`
4. Set these under **Tools**:

   | Setting | Value |
   |---------|-------|
   | Board | ESP32S3 Dev Module |
   | PSRAM | **OPI PSRAM** *(required)* |
   | Flash Size | 8MB (64Mb) |
   | Partition Scheme | 8M with spiffs *(or Huge APP)* |
   | USB CDC On Boot | Enabled |

5. Upload.

---

## Building a release / publishing to M5Burner

See [`docs/publishing-to-m5burner.md`](docs/publishing-to-m5burner.md) for the
full walkthrough. Short version: set `STICKPET_LOG` to `0` at the top of the
sketch (turns off serial logging and the dev cheats), then **Sketch → Export
Compiled Binary**, and upload the resulting `*.merged.bin` through M5Burner's
USER CUSTOM → Publish flow.

---

## Developer notes

While `STICKPET_LOG` is `1`, the sketch:

- prints the **boot reset reason** (a brownout shows up here) plus a status line every 10s
- accepts single-key **serial cheats** at 115200 baud:

  | key | effect |
  |-----|--------|
  | `k` | kill instantly |
  | `s` | make sick |
  | `f` | fill all stats |
  | `x` | +1 level |
  | `e` | age up one stage |
  | `p` | add a mess |
  | `n` | hatch a new egg |
  | `b` | spawn a boss now (cycles crab → spider → deer) |
  | `?` | print the cheat list |

Set `STICKPET_LOG` to `0` for release builds to compile all of that out.

---

## License

[MIT](LICENSE) © 2026 Edward Weber

Built for the M5Stack StickS3 with [M5Unified](https://github.com/m5stack/M5Unified).
