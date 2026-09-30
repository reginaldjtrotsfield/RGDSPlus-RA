#!/bin/sh
set -eu

BASE="/mnt/vendor/deep/nnddss"

[ "$(id -u)" = "0" ] || {
    echo "ERROR: uninstaller must run as root" >&2
    exit 1
}

if [ -f "$BASE/nnddss.real" ]; then
    rm -f "$BASE/nnddss"
    mv "$BASE/nnddss.real" "$BASE/nnddss"
    chmod 755 "$BASE/nnddss"
fi

rm -rf "$BASE/rgdsplus-ra"
rm -f "$BASE/lib/libra_settings_stage7d2.so"
rm -f /tmp/libra_stage7d2_perf.so /tmp/libra_popup_stage7d4b.so

if [ "${1:-}" = "--purge-data" ]; then
    rm -f "$BASE/.ra_token" "$BASE/.ra_user" "$BASE/ra.cfg" "$BASE/ra.cfg.tmp"
    echo "RGDSPlus-RA removed; RetroAchievements user data purged."
else
    echo "RGDSPlus-RA removed."
    echo "RetroAchievements account/config files were preserved."
fi
