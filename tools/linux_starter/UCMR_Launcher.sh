#!/bin/sh
# UCMR_Launcher - what players start on Linux (the Windows package's UCMR_Launcher.exe, tools/windows_starter). It runs
# the launcher window (launcher-qt/UCMRLauncher, beside it) from the package's folder with the same arguments - --play
# u1 | u2 | u3 included - and becomes that window (exec), so its exit code is the window's. The window finds its Qt
# libraries through its own run path ($ORIGIN/lib) and its plugins and QML through launcher-qt/qt.conf: no environment
# variable is set, so nothing the launcher opens (a file manager, a browser) inherits the package's Qt.
# Shipped as UCMR_Launcher by tools/package_linux.sh.
here=$(dirname -- "$(readlink -f -- "$0")")
window=$here/launcher-qt/UCMRLauncher
if [ ! -x "$window" ]; then
    echo "The launcher window (launcher-qt/UCMRLauncher) is not next to this program. Unpack the whole package into" \
         "one folder and start the launcher from there." >&2
    exit 1
fi
cd -- "$here" || exit 1
exec "$window" "$@"
