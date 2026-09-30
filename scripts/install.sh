#!/bin/sh
set -eu

BASE="/mnt/vendor/deep/nnddss"
RA_DIR="$BASE/rgdsplus-ra"

EXPECTED_NNDDSS="336caf2fd154e5ab2ceefe0eb60cefe8b28d4fbdf4790c834b83eaa9b38b93cd"
EXPECTED_CORE="5d58c35bbf269fa4dedbac76f99d1614c2f55b2e5e1dd3a08eaecf9737453fe3"
EXPECTED_LIVE="7b8540cdf77981a4f32aa962fc61d8c88bf0c1a6fe1df91331e219d6428a6f3a"
EXPECTED_POPUP="b50524d38635616aec1067650fe45f8e2db0f2cf4feb3f47d8b1c5cd4cc02481"
EXPECTED_SETTINGS="1a83c6421b7bd3edef7fe519d3a3f32317be950a435160f61cbc57e7d7cc8d7f"

SELF_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
PKG_DIR="$(CDPATH= cd -- "$SELF_DIR/.." && pwd)"

fail() {
    echo "ERROR: $*" >&2
    exit 1
}

hash_file() {
    sha256sum "$1" | awk '{print $1}'
}

[ "$(id -u)" = "0" ] || fail "installer must run as root"
[ -f "$PKG_DIR/bin/libra_live.so" ] || fail "bin/libra_live.so missing"
[ -f "$PKG_DIR/bin/libra_popup.so" ] || fail "bin/libra_popup.so missing"
[ -f "$PKG_DIR/bin/libra_settings.so" ] || fail "bin/libra_settings.so missing"
[ -f "$BASE/lib/libnnddss.so" ] || fail "libnnddss.so not found"

if [ -f "$BASE/nnddss.real" ]; then
    ORIGINAL="$BASE/nnddss.real"
elif [ -f "$BASE/nnddss" ]; then
    ORIGINAL="$BASE/nnddss"
else
    fail "NNDDSS executable not found"
fi

[ "$(hash_file "$ORIGINAL")" = "$EXPECTED_NNDDSS" ] || \
    fail "unsupported nnddss build: $(hash_file "$ORIGINAL")"

[ "$(hash_file "$BASE/lib/libnnddss.so")" = "$EXPECTED_CORE" ] || \
    fail "unsupported libnnddss.so build: $(hash_file "$BASE/lib/libnnddss.so")"

[ "$(hash_file "$PKG_DIR/bin/libra_live.so")" = "$EXPECTED_LIVE" ] || \
    fail "release live binary hash mismatch"
[ "$(hash_file "$PKG_DIR/bin/libra_popup.so")" = "$EXPECTED_POPUP" ] || \
    fail "release popup binary hash mismatch"
[ "$(hash_file "$PKG_DIR/bin/libra_settings.so")" = "$EXPECTED_SETTINGS" ] || \
    fail "release settings binary hash mismatch"

if [ ! -f "$BASE/nnddss.real" ]; then
    mv "$BASE/nnddss" "$BASE/nnddss.real"
fi

mkdir -p "$RA_DIR"
cp "$PKG_DIR/bin/libra_live.so" "$RA_DIR/libra_live.so"
cp "$PKG_DIR/bin/libra_popup.so" "$RA_DIR/libra_popup.so"
cp "$PKG_DIR/bin/libra_settings.so" "$RA_DIR/libra_settings.so"
cp "$PKG_DIR/bin/libra_settings.so" "$BASE/lib/libra_settings_stage7d2.so"
chmod 755 "$RA_DIR"/*.so "$BASE/lib/libra_settings_stage7d2.so"

RA_USERNAME=""
if [ -f "$BASE/.ra_user" ]; then
    RA_USERNAME="$(cat "$BASE/.ra_user" 2>/dev/null || true)"
fi

if [ -z "$RA_USERNAME" ]; then
    printf "RetroAchievements username: "
    read RA_USERNAME
fi

[ -n "$RA_USERNAME" ] || fail "RetroAchievements username cannot be empty"
printf '%s\n' "$RA_USERNAME" > "$BASE/.ra_user"
chmod 600 "$BASE/.ra_user"

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
echo "RGDSPlus-RA v0.1.0 installed."
echo "Username: $RA_USERNAME"
echo "Client:   RGDSPlus-RA/0.1.0 rcheevos/12.5"
echo
echo "IMPORTANT: v0.1.0 is a spectator-mode public preview."
echo "Achievement and leaderboard submissions are disabled."
echo "Launch the DS emulator normally, then use the RA menu to sign in."
