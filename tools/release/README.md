# Windows player release

Build MSVC Release with `RTS_BUILD_RENDER=ON`; `sanctum_launcher` is a small GUI
launcher with a static CRT. The game remains usable by command-line tests. The
portable archive has only `圣城.exe`, `游戏数据/`, and `开始游玩.txt` at its top level.

`package_windows.py` copies an explicit allowlist, not the checkout. Supply the
build directory, compiled launcher, licensed regular-weight TTF, its license,
MSVC x64 redistributable CRT directory, raylib and nlohmann/json licenses, output
directory and version. Run `python tools/release/package_windows.py --help` for
the required arguments. Existing package directories/ZIPs are refused. Generated
packages belong outside tracked source paths.

The 1.0.0 font is Noto Sans SC from Google Fonts
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

The official player package defaults to the scripted attacker. Merging the RL
source does not promote an experimental model. Release saves use v6, combining
worker orders, Phoenix lifecycle and optional policy state. Earlier v3/v4/v5
saves are rejected with legacy backup; no migration is claimed.
