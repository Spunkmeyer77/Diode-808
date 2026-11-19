# DIODE 808 for Max for Live

The DIODE 808 device: the x0x sequencer panel with the **Diode 808 circuit engine** (every voice of the TR-808 simulated at transistor level, the same engine as the Diode 808 AU/VST3).

- `device/DIODE 808.amxd` — the device (unfrozen)
- `code/diode808_seq.js`, `code/diode808_engine.js` — panel and pattern data
- `source/diode808_tilde` — the `diode808~` external (15 signal trigger inlets, live controls, flam)
- `../engine/` — the tr808 voice board (Lemus), GNU AGPLv3

Build: `build-macos.sh` (universal, macOS 12+) or `build-windows.bat` (x64). CI builds only on a `[release]` commit or a manual run.

Transistor Labs · ticcoross@gmail.com · https://ticcoross.gumroad.com/
