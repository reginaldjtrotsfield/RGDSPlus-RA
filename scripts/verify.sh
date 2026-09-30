#!/bin/sh
set -eu
BASE="/mnt/vendor/deep/nnddss"

echo "=== RGDSPlus-RA FILES ==="
sha256sum \
  "$BASE/rgdsplus-ra/libra_live.so" \
  "$BASE/rgdsplus-ra/libra_popup.so" \
  "$BASE/rgdsplus-ra/libra_settings.so"

echo
echo "=== EMULATOR BUILD ==="
sha256sum "$BASE/nnddss.real" "$BASE/lib/libnnddss.so"

echo
echo "=== ACCOUNT CONFIG ==="
[ -f "$BASE/.ra_user" ] && echo "Username configured: yes" || echo "Username configured: no"
[ -s "$BASE/.ra_token" ] && echo "Token configured: yes" || echo "Token configured: no"

echo
echo "=== RECENT RA LOG ==="
tail -n 60 /tmp/ra_frontend.log 2>/dev/null || true
