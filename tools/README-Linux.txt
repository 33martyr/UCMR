UCMR - unofficial PC port, Linux preview
=================================================================
Source: https://github.com/UC-Resurrected/UCMR

THE GAMES DO NOT RUN ON LINUX YET.
This package is a preview of the Linux port: the launcher window and the multiplayer imports.
The game runtime itself has not been ported to Linux yet, so PLAY cannot start Uncharted 1, 2
or 3 from this package. To play today, use the Windows package (UCMR_windows.zip), on Windows or
through Proton/Wine. The Linux port goes on phase by phase (docs/WINDOWS-DEPENDENCIES.md in the
source); later packages will run the games.

What you need
- A 64-bit Linux with glibc 2.35 or newer (for example Ubuntu 22.04 or newer), X11 or Wayland.
- The usual desktop libraries: fontconfig, freetype, libxkbcommon, libGL/libEGL, libX11/libxcb
  or libwayland-client. The less common Qt helpers (xcb-cursor, xcb-icccm, ...) ship in
  launcher-qt/lib.
- For the Uncharted 3 multiplayer import's Havok tables: GNU objdump (the binutils package, for
  example "sudo apt install binutils"). Without it the import skips that step and says so.
- Controller navigation in the launcher window uses the system's SDL3 (libSDL3.so.0) when it is
  installed; keyboard and mouse always work.

Getting started
1. Extract the whole folder wherever you like and keep its contents together:
     tar -xzf UCMR_linux.tar.gz
2. Run "UCMR_Launcher" in the UCMR_linux folder (double-click it, or ./UCMR_Launcher in a
   terminal).
3. Optional, a menu entry with the launcher's icon (run in the UCMR_linux folder):
     sed -e "s|^Exec=.*|Exec=\"$PWD/UCMR_Launcher\"|" -e "s|^Icon=.*|Icon=$PWD/launcher/assets/icon.png|" \
         UCMR_Launcher.desktop > ~/.local/share/applications/UCMR_Launcher.desktop

What works in this preview
- The launcher window with all its pages, settings and the game-folder choice.
- The Uncharted 2 and Uncharted 3 multiplayer imports (u2mp/bin/u2import, u3mp/bin/u3import and
  u3fix), from your own game files. They build and pass their checks on Linux, but a whole import
  has not been tested on Linux yet.
- Not yet on Linux: starting a game, Add to Steam, desktop shortcuts, the online sign-in, the
  launcher's own update (download the new tarball and extract it over this folder instead).

Good to know
- Settings, saves and logs live in the analysis/ folder beside the launcher, as on Windows.
- What changed in each version: the release notes on the project's GitHub Releases page.

License
GNU GPL v2 or later (LICENSE.txt); the source is at the address at the top. Not covered by the
GPL and licensed separately: the multiplayer helper u3mp/patcher/build/<version>/profile-helper.bin,
a separate work by this fork's author. The bundled libraries' licenses are in licenses/ (Qt in
licenses/qt, FFmpeg in licenses/ffmpeg, BearSSL in licenses/bearssl, the system libraries that ship
in launcher-qt/lib in licenses/distro).
This is an unofficial fan project, not affiliated with, endorsed or supported by Sony Interactive
Entertainment, Naughty Dog or Bluepoint Games.
