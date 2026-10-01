#!/bin/bash

SHDIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
BASE="/mnt/vendor/deep/nnddss"
LOG="$SHDIR/RGDSPlus-RA-uninstall.log"

exec >"$LOG" 2>&1

echo "========================================"
echo "RGDSPlus-RA v0.1.1 uninstaller"
echo "========================================"
date
echo

fail()
{
    echo
    echo "ERROR: $*"
    echo "Uninstall aborted."
    echo "Log: $LOG"
    sync
    exit 1
}

[ "$(id -u)" = "0" ] || fail "uninstaller must run as root"

echo "Removing RGDSPlus-RA..."

if [ -f "$BASE/nnddss.real" ]; then
    rm -f "$BASE/nnddss" || fail "could not remove launcher wrapper"
    mv "$BASE/nnddss.real" "$BASE/nnddss" || fail "could not restore original NNDDSS"
    chmod 755 "$BASE/nnddss"
fi

rm -rf "$BASE/rgdsplus-ra"
rm -f "$BASE/lib/libra_settings_stage7d2.so"

rm -f \
    /tmp/libra_stage7d2_perf.so \
    /tmp/libra_popup_stage7d4b.so

echo
echo "RGDSPlus-RA removed successfully."
echo "RetroAchievements account and settings were preserved."
echo
echo "Done. Returning to Applications."

sync
exit 0
