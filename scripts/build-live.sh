#!/usr/bin/env bash
set -euo pipefail
ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
cd "$ROOT"
mkdir -p build
CC="${CC:-aarch64-linux-gnu-gcc}"
"$CC" \
  -shared -fPIC -O2 -Wall -Wextra \
  -DRC_HASH_NO_DISC \
  -DRC_HASH_NO_ENCRYPTED \
  -DRC_HASH_NO_ZIP \
  -Ithird_party/zlib-headers \
  -Ithird_party/rcheevos/include \
  -Ithird_party/rcheevos/src \
  src/ra_live.c \
  src/ra_frame_hook.S \
  third_party/rcheevos/src/rc_client.c \
  third_party/rcheevos/src/rc_compat.c \
  third_party/rcheevos/src/rc_util.c \
  third_party/rcheevos/src/rc_version.c \
  third_party/rcheevos/src/rapi/*.c \
  third_party/rcheevos/src/rcheevos/*.c \
  third_party/rcheevos/src/rhash/hash.c \
  third_party/rcheevos/src/rhash/hash_rom.c \
  third_party/rcheevos/src/rhash/md5.c \
  -o build/libra_live.so \
  -ldl -pthread -lm
file build/libra_live.so
sha256sum build/libra_live.so
