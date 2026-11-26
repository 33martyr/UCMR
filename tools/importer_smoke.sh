#!/usr/bin/env bash
# The importers' checks that need no game files (docs/WINDOWS-DEPENDENCIES.md §13 phase 2), on Linux after
# tools/build_importers_linux.sh and on Windows after tools/package_windows.sh (OUT=dist/UCMR_windows). Run from the
# repository root; each line names what it ran, a failed check is an annotation and fails the step.
set -uo pipefail
b=${OUT:-out/importers}
x=
case "${OSTYPE:-}" in msys* | cygwin* | win*) x=.exe ;; esac
export U3MP_PATCHER=$PWD/u3mp/patcher   # u3import finds the recipe here (the package has it next to u3mp/bin)
failed=0
check() {   # <title> <expected exit code> <grep pattern for the output> <command...>
    local title=$1 want=$2 pattern=$3 got out
    shift 3
    out=$("$@" 2>&1); got=$?
    if [[ $got != "$want" ]] || ! grep -Eq -- "$pattern" <<< "$out"; then
        echo "FAIL $title (exit $got, wanted $want): $(head -c 400 <<< "$out")"
        echo "::error title=importer smoke: $title::exit $got, wanted $want: $(head -c 300 <<< "$out" | tr '\n' ' ')"
        failed=$((failed + 1))
    else
        echo "ok   $title: $(head -1 <<< "$out" | cut -c1-120)"
    fi
}
check "u3fix without a step: its usage" 2 '^usage: u3fix' "$b/u3mp/bin/u3fix$x"
check "u3fix levels without arguments: names what is missing" 1 'missing|unknown' "$b/u3mp/bin/u3fix$x" levels
check "u3fix carries its code ids" 0 '.' grep -ac 'U3FIX-CODE-IDS 1' "$b/u3mp/bin/u3fix$x"
check "u3import without arguments: names what is required" 2 'required: --ps4, --data, --port' "$b/u3mp/bin/u3import$x"
check "u3import --shipped-id" 0 '^[0-9a-f]{64}' "$b/u3mp/bin/u3import$x" --shipped-id
check "u3import --recipe-fingerprint" 0 '^[0-9a-f]{64}' "$b/u3mp/bin/u3import$x" --recipe-fingerprint
check "u3import --recipe --list: every recipe step" 0 '^copy-ps3-' "$b/u3mp/bin/u3import$x" --recipe --list
check "u2import without arguments: its usage" 2 '^usage: u2import' "$b/u2mp/bin/u2import$x"
check "u2import --shipped-id" 0 '^[0-9a-f]{64}' "$b/u2mp/bin/u2import$x" --shipped-id
echo "steps listed: $("$b/u3mp/bin/u3import$x" --recipe --list | wc -l)," \
    "recipe fingerprint: $("$b/u3mp/bin/u3import$x" --recipe-fingerprint)"
(( failed == 0 ))
