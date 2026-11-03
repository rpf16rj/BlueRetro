# BlueRetro fork (rpf16rj) - agent notes

## Workflow rules

- **NEVER commit, push or publish releases until the user confirms the change works on hardware and explicitly asks.** Local edits and builds are fine; publication requires a green light.
- Version every generated binary in `firmware/` using `BlueRetro_<hw>_<system>_<YYYY-MM-DD>_<features>.bin` (new file each build, never overwrite).
- Do not commit the `firmware/` folder or `sdkconfig` (generated). Commit sources only.
- Do not use `GamePadAlt` output mode for PlayStation (it presents a FlightStick ID) — always `GamePad`. For MC+rumble use Accessory = `Memory+Rumble`.

## Build (Windows, ESP-IDF v5.5.1 at C:\Users\rpf16\esp-idf)

- Git Bash cannot run `export.bat` (MSYSTEM leak) — always run builds via `MSYS_NO_PATHCONV=1 cmd /C build_br.bat` from repo root.
- Correct config selection is `copy configs\hw1\playstation sdkconfig` (BR_HW/BR_SYS env vars only affect the binary name). `build_br.bat` already does this.
- Verify `sdkconfig`: `# CONFIG_BLUERETRO_HW2 is not set`, `CONFIG_BLUERETRO_SYSTEM_PSX_PS2=y`, `CONFIG_BLUERETRO_WEBUI=y`.
- Output: `build\BlueRetro_hw1_playstation.bin`. Flash serial: `idf.py -p COMx flash` or `python -m esptool ... write_flash "@flash_args"` from `build\`.

## Fork changes vs upstream (branch fix/ps-rumble-per-poll)

1. `main/wired/ps_spi.c`: rumble feedback queued on every 0x42 poll (v24.10 behavior) — reverts the change-detection from 4beea29 that broke PS rumble. Keeps `acc_mode & ACC_RUMBLE` and the `req[0]==0x73` OG analog fallback.
2. `main/bluetooth/hidp/ps.c`: DualSense vibration v2 — flag0 `cmd`=0x02 (HAPTICS_SELECT only), flag2 `use_accurate_rumble`=0x04 (COMPATIBLE_VIBRATION2). Requires DS5 fw >= 2.21.
3. `main/net/webui.c` + `main/net/index.html` (`CONFIG_BLUERETRO_WEBUI`): always-on WiFi AP `BlueRetro-XXXX` / pass `blueretro` → `http://192.168.4.1`. WebSocket JSON API `/ws` exposing: info, glbl/out/in cfg get+set, dev_list/dev_disc/pair, mc_read/mc_write (128KiB), dir/del (/fs), sys reset|sleep|factory. OTA via `POST /ota`. MC browser parses PS1 dir frames client-side.
4. `partitions.csv`: 2×1.3MB app slots (factory + ota_0); storage kept at 0x310000 so configs/pairings/MC survive. First flash after the layout change must be serial.

## Gotchas learned

- `CONFIG_HTTPD_WS_SUPPORT=y` is required for httpd WS API (set in sdkconfig).
- `configs/hw1/playstation` file must end with newline when appending options.
- Green LED on DS4/DS5 = out_idx 2 (ports sensed occupied → wrong mapping = dead buttons). On HW1 `sense_port_is_empty` always returns 1, so out_idx should be 0 (blue).
