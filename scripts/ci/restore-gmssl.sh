#!/usr/bin/env bash
set -euo pipefail

# Exact GmSSL port 12 from the Salts 2.3.0-rc.16 pinned registry.
# Use source-build vcpkg without binary NuGet cache / Mono.
root="$1"
if [[ -z "$root" ]]; then echo "missing GmSSL restore root" >&2; exit 1; fi
mkdir -p -- "$root"
root="$(cd "$root" && pwd -P)"
installed="$root/installed/x64-linux"

if [[ ! -f "$installed/share/gmssl/GmSSLConfig.cmake" ]]; then
  vcpkg="$root/vcpkg"
  registry="$root/vcpkg-cache"
  mkdir -p "$vcpkg" "$registry"
  git -C "$vcpkg" init -q
  git -C "$vcpkg" remote add origin https://github.com/microsoft/vcpkg.git
  git -C "$vcpkg" fetch -q --depth=1 origin b1b19307e2d2ec1eefbdb7ea069de7d4bcd31f01
  git -C "$vcpkg" checkout -q --detach FETCH_HEAD

  git -C "$registry" init -q
  git -C "$registry" remote add origin https://github.com/qigao/vcpkg-cache.git
  git -C "$registry" fetch -q --depth=1 origin e678e8c1d0510e89b79e53b1828bfd2cab18ab0e
  git -C "$registry" checkout -q --detach FETCH_HEAD
  grep -q '"port-version": 12' "$registry/ports/gmssl/vcpkg.json"

  (
    cd "$vcpkg"
    ./bootstrap-vcpkg.sh -disableMetrics
    VCPKG_BINARY_SOURCES=clear ./vcpkg install gmssl:x64-linux \
      --overlay-ports="$registry/ports" --x-install-root="$root/installed" --disable-metrics
  )
fi

test -f "$installed/share/gmssl/GmSSLConfig.cmake"
test -f "$installed/include/gmssl/tls.h"
test -f "$installed/lib/libgmssl.a"
if [[ -n "$GITHUB_ENV" ]]; then
  printf 'GMSSL_ROOT=%s\n' "$installed" >> "$GITHUB_ENV"
  printf 'SDK_PREFIX_PATH=%s;%s\n' "$SDK_PREFIX_PATH" "$installed" >> "$GITHUB_ENV"
fi
