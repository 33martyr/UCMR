# sourced by run.sh for the progression test: time() is the test's (fake_time.h), and the C runtime functions the
# sources call get an _old name in the old build (deps_progression.cpp defines both).
OLD_FLAGS="$OLD_FLAGS -include $here/fake_time.h -Druntime_file_user_dir=runtime_file_user_dir_old -Druntime_file_translate=runtime_file_translate_old"
NEW_FLAGS="$NEW_FLAGS -include $here/fake_time.h"
# Both builds link the current progression_store.cpp (deps_progression.cpp), so the old progression.cpp must see the
# current progression_store.h too: since 62fc26b the store's types grew (MatchPlayer / Store headshots, 475fba8;
# PlayerStats unlisted, f11e480), and with the 62fc26b copy run.sh extracts the old build passed and kept smaller
# objects than the store code reads. A clang VFS overlay shows the current header at that copy's path.
pg_path() { if command -v cygpath > /dev/null; then cygpath -m "$1"; else printf '%s' "$1"; fi; }
pg_overlay=$root/out/u2mp_rewrite/progression-store.yaml
mkdir -p "$root/out/u2mp_rewrite"
printf "{ 'version': 0, 'roots': [ { 'type': 'file', 'name': '%s', 'external-contents': '%s' } ] }\n" \
    "$(pg_path "$root/out/u2mp_rewrite/$t/old/progression_store.h")" "$(pg_path "$root/src/u2mp/progression_store.h")" \
    > "$pg_overlay"
OLD_FLAGS="$OLD_FLAGS -ivfsoverlay $(pg_path "$pg_overlay")"
