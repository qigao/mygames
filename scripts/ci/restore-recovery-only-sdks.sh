#!/usr/bin/env bash
set -euo pipefail

# Candidate-source M4 recovery integration requires only published TurboDB
# and Flowie until those repositories receive their own source qualifiers.
# NEVER download or load the old prebuilt Salts/CHttp/GmSSL in this job:
# the versioned GmSSL TLS_CONNECT ABI comes solely from rebuilt port14 SDKs.
root="${1:?absolute output directory required}"
if [[ "$root" != /* || "$root" == / ]]; then
  echo "recovery SDK destination must be an absolute non-root path" >&2
  exit 1
fi
mkdir -p -- "$root"
root="$(cd "$root" && pwd -P)"

fetch() {
  local repo="$1" tag="$2" package="$3" sha="$4" target="$5"
  local archive="$root/$package" sdk="$root/$target"
  mkdir -p -- "$sdk"
  curl --fail --location --silent --show-error --retry 3 --retry-delay 2 \
    "https://github.com/qigao/$repo/releases/download/$tag/$package" \
    -o "$archive"
  printf '%s  %s\n' "$sha" "$archive" | sha256sum --check --status
  unzip -q "$archive" -d "$sdk"
}

fetch turbodb v2.3.6 TurboDB.Native.2.3.6.nupkg \
  e3cad4bd01207ad66dc579f0990e90af6384fc6d3d9410ed3d1b01cbf1c104a5 turbodb
fetch flowie v1.0.4 Flowie.Native.1.0.4.nupkg \
  a225b4e57fc84b51a8c87a1db5a28545a0901b9b485b773d5410d36935fd24db flowie

turbodb="$root/turbodb/sdk/linux-x64"
flowie="$root/flowie/sdk/linux-x64"
for item in \
  "$turbodb/lib/cmake/TurboDB/TurboDBConfig.cmake" \
  "$turbodb/lib/turbodb/drivers/turbodb_driver_sqlite.so" \
  "$flowie/lib/cmake/Flowie/FlowieConfig.cmake"; do
  test -f "$item" || { echo "Required runtime SDK missing: $item" >&2; exit 1; }
done

if [[ -n "${GITHUB_ENV:-}" ]]; then
  {
    printf 'TURBODB_ROOT=%s\n' "$turbodb"
    printf 'FLOWIE_ROOT=%s\n' "$flowie"
  } >> "$GITHUB_ENV"
fi
echo "Recovery SDK runtime inputs verified: TurboDB 2.3.6, Flowie 1.0.4"
