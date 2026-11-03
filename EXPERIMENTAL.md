# BlueRetro fork - Experimental builds

Fork of [darthcloud/BlueRetro](https://github.com/darthcloud/BlueRetro) (archived upstream on 2025-12-14) with experimental fixes and features, currently targeting the **PlayStation (HW1 / external adapter)** build.

All changes live on branch [`fix/ps-rumble-per-poll`](https://github.com/rpf16rj/BlueRetro/tree/fix/ps-rumble-per-poll). Prebuilt firmware is published on the [Releases](https://github.com/rpf16rj/BlueRetro/releases) page. Builds are experimental: test carefully before deploying.

## Changes vs upstream master

### 1. PlayStation rumble regression fix (`main/wired/ps_spi.c`)

Upstream commit `4beea29` ("[BT] Make rumble more responsive", merged for the Switch 2 beta) made wired feedback conditional on *changes* in the rumble motor bytes, caching `rumble_r_val`/`rumble_l_val`. This broke rumble on PlayStation:

- games that never issue the `0x4D` motor-mapping command only drive motor byte 0, so index-based change detection misses transitions;
- change detection is fragile whenever the configured indices don't match the actual byte layout a game uses.

This fork restores the v24.10 behavior of queueing feedback on **every** `0x42` poll, while keeping two improvements added after v24.10:

- `acc_mode & ACC_RUMBLE` bitmask check, so rumble works with both **Rumble** and **Memory+Rumble** accessory modes;
- the `req[0] == 0x73` fallback for the original PS analog controller.

Result: PS1 rumble works again on the post-Switch-2-beta codebase, including simultaneously with the virtual memory card.

### 2. DualSense vibration v2 / accurate haptics (`main/bluetooth/hidp/ps.c`)

The DualSense output report now selects the newer "v2" haptics mode instead of DS4-compatible rumble emulation:

- `valid_flag0 = HAPTICS_SELECT` (0x02) instead of `COMPATIBLE_VIBRATION | HAPTICS_SELECT` (0x03);
- `valid_flag2 = COMPATIBLE_VIBRATION2` (0x04).

This mirrors the recipe used by the Linux `hid-playstation` driver: the two actuator bytes drive the DualSense voice-coil haptics directly, giving finer and more precise rumble. Requires DualSense firmware >= 2.21 (bundled with PS5 updates since 2021; DualSense Edge always supports it).

### 3. WiFi configuration web UI (experimental, WIP)

Work in progress: the adapter exposes a WiFi access point (`BlueRetro-XXXX`) with an embedded web page at `http://192.168.4.1` so configuration works even without a paired Bluetooth client or internet access:

- view firmware info, connected/paired controllers;
- read/write global, output and input (mapping) configuration;
- memory card browser: list, download and upload save files;
- system commands: reset, deep sleep, factory reset, OTA update.

## Building

Requires ESP-IDF **v5.5.x**.

```sh
# Windows CMD (clear MSYSTEM if running from Git Bash)
set BR_HW=_hw1
set BR_SYS=_playstation
copy configs\hw1\playstation sdkconfig
call %USERPROFILE%\esp-idf\export.bat
idf.py build
```

Output: `build/BlueRetro_hw1_playstation.bin`

`build_br.bat` at the repo root wraps this for the HW1 PlayStation target.

## Flashing

- Serial: `idf.py -p COMx flash`, or from `build/`: `python -m esptool --chip esp32 -b 460800 --before default_reset --after hard_reset write_flash "@flash_args"`
- OTA: upload `BlueRetro_hw1_playstation.bin` through the web config update page.

## Recommended settings (PlayStation)

- Output Mode: **GamePad** (not GamePadAlt, which presents a FlightStick ID)
- Accessory: **Memory+Rumble** for virtual memory card + rumble

## Tested

- DS4 + DualSense on PS1: pairing, all buttons, rumble, virtual memory card simultaneously.
