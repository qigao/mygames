#!/usr/bin/env bash
set -euo pipefail

# Coherent CHttp 2.1.0-rc.6 published ABI family.
# Public GitHub release assets restored with curl + SHA256. No mono or dotnet.
root="$1"
if [[ -z "$root" ]]; then echo "output directory required" >&2; exit 1; fi
mkdir -p -- "$root"
root="$(cd "$root" && pwd -P)"

download() {
  local repo="$1" tag="$2" name="$3" digest="$4" key="$5"
  local dir="$root/$key" target="$root/$key.nupkg"
  mkdir -p -- "$dir"
  curl --fail --location --silent --show-error --retry 3 --retry-delay 2 \
    "https://github.com/qigao/$repo/releases/download/$tag/$name" -o "$target"
  printf '%s  %s\n' "$digest" "$target" | sha256sum --check --status
  unzip -q "$target" -d "$dir"
}

download salts v2.3.0-rc.16 Salts.Native.2.3.0-rc.16.nupkg \
  0b8592cb0b27e91fb5a3ee2c5a888ca53c7593cf9eb8cd5adfbc38fac43659b7 salts
download salts-utils v4.3.0-rc.11 SaltsUtils.Native.4.3.0-rc.11.nupkg \
  0d15b3dba534b55a42069dc665070cf177062d0bd029b31639dfb2bb2d4bdf64 saltsutils
download chttp v2.1.0-rc.6 CHttp.Native.2.1.0-rc.6.nupkg \
  d0e6e8c57c83e714443fd56bde9f0a203d44005656cf25fa6be2fe59cb9f03a2 chttp

salts="$root/salts/sdk/linux-x64"
utils="$root/saltsutils/sdk/linux-x64"
chttp="$root/chttp/sdk/linux-x64"
for file in "$salts/lib/cmake/Salts/SaltsConfig.cmake" \
  "$utils/lib/cmake/SaltsUtils/SaltsUtilsConfig.cmake" \
  "$chttp/lib/cmake/Chttp/ChttpConfig.cmake"; do
  test -f "$file" || { echo "Missing SDK file: $file" >&2; exit 1; }
done

if [[ -n "$GITHUB_ENV" ]]; then
  {
    printf 'SALTS_ROOT=%s\n' "$salts"
    printf 'SALTS_UTILS_ROOT=%s\n' "$utils"
    printf 'CHTTP_ROOT=%s\n' "$chttp"
    printf 'SDK_PREFIX_PATH=%s;%s;%s\n' "$chttp" "$salts" "$utils"
    printf 'LD_LIBRARY_PATH=%s/lib:%s/lib:%s/lib:%s/bin:%s/bin:%s/bin\n' \
      "$chttp" "$utils" "$salts" "$chttp" "$utils" "$salts"
  } >> "$GITHUB_ENV"
else
  printf 'SALTS_ROOT=%s\nSALTS_UTILS_ROOT=%s\nCHTTP_ROOT=%s\n' \
    "$salts" "$utils" "$chttp"
fi
