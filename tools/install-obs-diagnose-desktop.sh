#!/usr/bin/env bash
set -euo pipefail

DESKTOP_FILE="${DESKTOP_FILE:-$HOME/.local/share/applications/com.obsproject.Studio.desktop}"
MODE="${MODE:-replace}"   # replace|new
WRAPPER_PATH_DEFAULT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/obs-diagnose.sh"
WRAPPER_PATH="${WRAPPER_PATH:-$WRAPPER_PATH_DEFAULT}"

usage() {
  cat <<'USAGE'
Usage: tools/install-obs-diagnose-desktop.sh [options]

Patch OBS desktop launcher to run through tools/obs-diagnose.sh
while preserving custom env/options from existing Exec line.

Options:
  --desktop <file>    Desktop file path
                      (default: ~/.local/share/applications/com.obsproject.Studio.desktop)
  --mode <mode>       replace|new (default: replace)
  --wrapper <path>    Path to obs-diagnose.sh
  -h, --help          Show help

Notes:
  - replace: edits existing desktop file in place (backup created).
  - new: writes <name>.diagnose.desktop and keeps original untouched.
USAGE
}

while (($#)); do
  case "$1" in
    --desktop) DESKTOP_FILE="${2:-}"; shift 2 ;;
    --mode) MODE="${2:-}"; shift 2 ;;
    --wrapper) WRAPPER_PATH="${2:-}"; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) echo "Unknown option: $1" >&2; exit 2 ;;
  esac
done

if [[ ! -f "${DESKTOP_FILE}" ]]; then
  echo "Desktop file not found: ${DESKTOP_FILE}" >&2
  exit 2
fi
if [[ ! -x "${WRAPPER_PATH}" ]]; then
  echo "Wrapper script not executable: ${WRAPPER_PATH}" >&2
  exit 2
fi
if [[ "${MODE}" != "replace" && "${MODE}" != "new" ]]; then
  echo "Invalid mode: ${MODE}" >&2
  exit 2
fi

orig_exec="$(awk -F= '/^Exec=/{print substr($0,6); exit}' "${DESKTOP_FILE}")"
if [[ -z "${orig_exec}" ]]; then
  echo "Could not find Exec= line in ${DESKTOP_FILE}" >&2
  exit 2
fi

if [[ "${orig_exec}" == *"obs-diagnose.sh"* ]]; then
  echo "Desktop already points to obs-diagnose.sh"
  exit 0
fi

# Preserve custom env flags in front of obs binary:
# Example input: env __NV_DISABLE_EXPLICIT_SYNC=1 obs %U
# Output:        env __NV_DISABLE_EXPLICIT_SYNC=1 /path/obs-diagnose.sh %U
new_exec="${orig_exec/ obs / ${WRAPPER_PATH} }"
if [[ "${new_exec}" == "${orig_exec}" ]]; then
  # Fallback for unusual Exec strings without " obs " token.
  new_exec="${WRAPPER_PATH} %U"
fi

target="${DESKTOP_FILE}"
if [[ "${MODE}" == "new" ]]; then
  target="${DESKTOP_FILE%.desktop}.diagnose.desktop"
  cp -a "${DESKTOP_FILE}" "${target}"
else
  backup="${DESKTOP_FILE}.bak.$(date -u +%Y%m%dT%H%M%SZ)"
  cp -a "${DESKTOP_FILE}" "${backup}"
  echo "backup=${backup}"
fi

tmp="${target}.tmp.$$"
awk -v new_exec="${new_exec}" '
  BEGIN { done=0 }
  /^Exec=/ && done==0 {
    print "Exec=" new_exec
    done=1
    next
  }
  { print }
' "${target}" > "${tmp}"
mv "${tmp}" "${target}"

update-desktop-database "${HOME}/.local/share/applications" >/dev/null 2>&1 || true

echo "patched=${target}"
echo "old_exec=${orig_exec}"
echo "new_exec=${new_exec}"
