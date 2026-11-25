# RecompLauncher

[RecompLauncher](https://recomplauncher.com) installs and updates UCMR from this repository's GitHub releases. It
reads [`recomplauncher.json`](../recomplauncher.json) (schema: <https://recomplauncher.com/schema/recomplauncher.json>),
which `tools/package_windows.sh` puts at the top of `UCMR_windows.zip`.

What the file tells RecompLauncher:

- **start:** `UCMR_Launcher.exe`, the same program players start by hand. The player picks their game copies and
  runs the imports in UCMR's own launcher, as without RecompLauncher.
- **saves:** `analysis/uncharted-user/savedata` (all save profiles), for RecompLauncher's save backups.
- **logs:** `analysis` (each game's `boot.log`).
- **system:** 64-bit Windows 10 or newer with Vulkan 1.3, as in `tools/README-Windows.txt`.

When RecompLauncher starts the launcher it sets `RECOMPLAUNCHER=1`. The launcher then skips its own update check
(`launcher/native/updater.cpp`, `updates.cpp`), because RecompLauncher installs new releases itself. Started any
other way, nothing changes.

Releases keep the asset name `UCMR_windows.zip`, because launchers already installed look for that name when they
update.
