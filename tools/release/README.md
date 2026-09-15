# Windows player release

Build MSVC Release with `RTS_BUILD_RENDER=ON`; `sanctum_launcher` is a small GUI
launcher with a static CRT. The game remains usable by command-line tests. The
portable archive has only `圣城.exe`, `游戏数据/`, and `开始游玩.txt` at its top level.

`package_windows.py` copies an explicit allowlist, not the checkout. Supply the
build directory, compiled launcher, licensed regular-weight TTF, its license,
MSVC x64 redistributable CRT directory, raylib and nlohmann/json licenses, output
directory, version and a validated attacker ONNX model (`--attacker-model`).
Run `python tools/release/package_windows.py --help` for
the required arguments. Existing package directories/ZIPs are refused. Generated
packages belong outside tracked source paths.

The launcher preserves the engineering build's default font priority: installed
SimHei, then DengXian. It does not force a different font or change UI sizes.
If neither system font loads, the renderer tries `NotoSansSC.ttf` in its working
directory; the launcher always sets this to the packaged game data directory.
Explicit `--font` still overrides this selection and fails on invalid fonts.
Microsoft fonts are not redistributed.

The bundled fallback font is Noto Sans SC from Google Fonts
(`https://github.com/google/fonts/tree/main/ofl/notosanssc`), SIL OFL 1.1.
Instantiate its `wght` axis at 400 using fontTools before packaging; the variable
font's default weight is too thin for this UI. Keep its OFL notice in the package.
The package manifest records the exact resulting font and every shipped file's
SHA256, plus the source commit. Font binaries and Microsoft redistributables are
not committed to this repository.

Verification must use a fresh extraction outside the repository, including a
Unicode/space path. Run the launcher with `--verify-assets`, then menu/battle,
guide and chronicle screenshot options. Use an isolated `LOCALAPPDATA` for tests.
Confirm the log discovers the extracted assets rather than development paths.
The launcher never needs administrator rights, Python or a compiler. It writes
its diagnostic log under `%LOCALAPPDATA%/SiegeRTS/launcher.log`.

Version 1.0.1 includes the team's selected current RL result in
`游戏数据/models/attacker.onnx`. The main menu switches between scripted and RL
attackers, saving before reloading the corresponding campaign. RL saves are
separated by model identity, including when using `--save-dir`. Each launch
initially selects script; selecting RL restores its own campaign. Unsupported
unit types and levels retain scripted control. No further training is required
to play. Check the bundled model provenance in the release notes.
Release saves use v6, combining
worker orders, Phoenix lifecycle and optional policy state. Earlier v3/v4/v5
saves are rejected with legacy backup; no migration is claimed.
