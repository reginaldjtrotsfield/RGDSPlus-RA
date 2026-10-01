#!/bin/bash

SHDIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
PKG_DIR="$SHDIR/RGDSPlus-RA"
BASE="/mnt/vendor/deep/nnddss"
RA_DIR="$BASE/rgdsplus-ra"
LOG="$SHDIR/RGDSPlus-RA-install.log"

EXPECTED_NNDDSS="336caf2fd154e5ab2ceefe0eb60cefe8b28d4fbdf4790c834b83eaa9b38b93cd"
EXPECTED_CORE="5d58c35bbf269fa4dedbac76f99d1614c2f55b2e5e1dd3a08eaecf9737453fe3"
EXPECTED_LIVE="c36119580ac6e0732901a665badbf024d94f0756e36df34192fddb1695974332"
EXPECTED_POPUP="b50524d38635616aec1067650fe45f8e2db0f2cf4feb3f47d8b1c5cd4cc02481"
EXPECTED_SETTINGS="8f1cea8180fb45e1e2f44c26b003920fe0c740d21ec8c7bcd72257d0deef161d"

exec >"$LOG" 2>&1

echo "========================================"
echo "RGDSPlus-RA v0.1.1 installer"
echo "========================================"
date
echo

fail()
{
    echo
    echo "ERROR: $*"
    echo "Installation aborted."
    echo "Log: $LOG"
    sync
    exit 1
}

hash_file()
{
    sha256sum "$1" | awk '{print $1}'
}

[ "$(id -u)" = "0" ] || fail "installer must run as root"

[ -f "$PKG_DIR/libra_live.so" ] || fail "libra_live.so missing"
[ -f "$PKG_DIR/libra_popup.so" ] || fail "libra_popup.so missing"
[ -f "$PKG_DIR/libra_settings.so" ] || fail "libra_settings.so missing"
[ -f "$BASE/lib/libnnddss.so" ] || fail "libnnddss.so not found"

if [ -f "$BASE/nnddss.real" ]; then
    ORIGINAL="$BASE/nnddss.real"
elif [ -f "$BASE/nnddss" ]; then
    ORIGINAL="$BASE/nnddss"
else
    fail "NNDDSS executable not found"
fi

echo "Verifying supported NNDDSS build..."

[ "$(hash_file "$ORIGINAL")" = "$EXPECTED_NNDDSS" ] || \
    fail "unsupported nnddss build"

[ "$(hash_file "$BASE/lib/libnnddss.so")" = "$EXPECTED_CORE" ] || \
    fail "unsupported libnnddss.so build"

echo "Verifying RGDSPlus-RA files..."

[ "$(hash_file "$PKG_DIR/libra_live.so")" = "$EXPECTED_LIVE" ] || \
    fail "libra_live.so hash mismatch"

[ "$(hash_file "$PKG_DIR/libra_popup.so")" = "$EXPECTED_POPUP" ] || \
    fail "libra_popup.so hash mismatch"

[ "$(hash_file "$PKG_DIR/libra_settings.so")" = "$EXPECTED_SETTINGS" ] || \
    fail "libra_settings.so hash mismatch"

echo "Installing RGDSPlus-RA..."

if [ ! -f "$BASE/nnddss.real" ]; then
    mv "$BASE/nnddss" "$BASE/nnddss.real" || \
        fail "could not preserve original NNDDSS executable"
fi

mkdir -p "$RA_DIR" || fail "could not create RGDSPlus-RA directory"

cp "$PKG_DIR/libra_live.so" "$RA_DIR/libra_live.so" || fail "could not install live library"
cp "$PKG_DIR/libra_popup.so" "$RA_DIR/libra_popup.so" || fail "could not install popup library"
cp "$PKG_DIR/libra_settings.so" "$RA_DIR/libra_settings.so" || fail "could not install settings library"
cp "$PKG_DIR/libra_settings.so" "$BASE/lib/libra_settings_stage7d2.so" || fail "could not install settings preload"

chmod 755 \
    "$RA_DIR/libra_live.so" \
    "$RA_DIR/libra_popup.so" \
    "$RA_DIR/libra_settings.so" \
    "$BASE/lib/libra_settings_stage7d2.so"

cat > "$BASE/nnddss" <<'WRAPPER'
#!/bin/sh
BASE="/mnt/vendor/deep/nnddss"
RA_DIR="$BASE/rgdsplus-ra"

RA_USER="$(cat "$BASE/.ra_user" 2>/dev/null || true)"
RA_TOKEN="$(cat "$BASE/.ra_token" 2>/dev/null || true)"

export RA_USER
export RA_TOKEN

cp "$RA_DIR/libra_live.so" /tmp/libra_stage7d2_perf.so || exit 1
cp "$RA_DIR/libra_popup.so" /tmp/libra_popup_stage7d4b.so || exit 1

export LD_PRELOAD="/tmp/libra_stage7d2_perf.so:/tmp/libra_popup_stage7d4b.so:$BASE/lib/libra_settings_stage7d2.so"

exec "$BASE/nnddss.real" "$@" > /tmp/ra_frontend.log 2>&1
WRAPPER

chmod 755 "$BASE/nnddss"

echo
echo "Installation successful."
echo "RGDSPlus-RA v0.1.1 is installed."
echo
echo "Launch a DS game normally and use the"
echo "RetroAchievements menu to sign in."
echo
echo "Existing RetroAchievements credentials were preserved."
echo
echo "Done. Returning to Applications."

sync
exit 0
