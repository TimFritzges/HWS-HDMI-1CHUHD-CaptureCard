#!/usr/bin/env bash
set -euo pipefail

OUT_DIR=""
PHASE="snapshot"
MODULE="${MODULE:-HwsUHDX1Capture}"
DEVICE="${DEVICE:-/dev/video0}"

usage() {
  cat <<'USAGE'
Usage: tools/collect-evidence.sh --out <dir> [options]

Collect a non-privileged HWS/PipeWire/system state snapshot without opening a
capture stream or modifying module state.

Options:
  --out <dir>       Artifact output directory (required)
  --phase <label>   Filename prefix (default: snapshot)
  --module <name>   Module name (default: HwsUHDX1Capture)
  --device <path>   V4L2 node for query-only status (default: /dev/video0)
  -h, --help        Show help
USAGE
}

while (($#)); do
  case "$1" in
    --out) OUT_DIR="${2:-}"; shift 2 ;;
    --phase) PHASE="${2:-}"; shift 2 ;;
    --module) MODULE="${2:-}"; shift 2 ;;
    --device) DEVICE="${2:-}"; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) echo "Unknown option: $1" >&2; exit 2 ;;
  esac
done

if [[ -z "${OUT_DIR}" ]]; then
  echo "--out is required" >&2
  exit 2
fi
if [[ ! "${PHASE}" =~ ^[A-Za-z0-9._-]+$ ]]; then
  echo "Invalid phase label: ${PHASE}" >&2
  exit 2
fi

mkdir -p "${OUT_DIR}"
prefix="${OUT_DIR}/${PHASE}"
diag_root="/proc/hwsuhdx1"
if [[ ! -r "${diag_root}/video_diag" ]]; then
  diag_root="/sys/kernel/debug/hwsuhdx1"
fi

capture_command() {
  local output="$1"
  shift
  "$@" > "${output}" 2>&1 || true
}

{
  echo "timestamp_utc=$(date -u --iso-8601=seconds)"
  echo "hostname=$(hostname)"
  echo "kernel=$(uname -r)"
  echo "module=${MODULE}"
  echo "device=${DEVICE}"
  echo "diag_root=${diag_root}"
  echo "device_exists=$([[ -e "${DEVICE}" ]] && echo 1 || echo 0)"
  echo "device_readable=$([[ -r "${DEVICE}" ]] && echo 1 || echo 0)"
  for cmd in v4l2-ctl v4l2-compliance ffmpeg pw-dump pw-top pw-profiler \
    wpctl pactl arecord trace-cmd kernelshark sparse smatch spatch bpftrace \
    perf makedumpfile kexec; do
    if command -v "${cmd}" >/dev/null 2>&1; then
      echo "tool_${cmd//-/_}=present"
    else
      echo "tool_${cmd//-/_}=missing"
    fi
  done
} > "${prefix}-inventory.env"

{
  echo "===== uname ====="
  uname -a 2>&1 || true
  echo
  echo "===== cmdline ====="
  /usr/bin/cat /proc/cmdline 2>&1 || true
  echo
  echo "===== loadavg ====="
  /usr/bin/cat /proc/loadavg 2>&1 || true
  echo
  echo "===== devices ====="
  ls -l /dev/video* /dev/snd/* 2>&1 || true
  echo
  echo "===== pstore listing ====="
  ls -la /sys/fs/pstore 2>&1 || true
} > "${prefix}-system.txt"

if command -v lspci >/dev/null 2>&1; then
  capture_command "${prefix}-pci.txt" lspci -nnk
fi
if command -v lsmod >/dev/null 2>&1; then
  capture_command "${prefix}-lsmod.txt" lsmod
fi
if command -v modinfo >/dev/null 2>&1; then
  capture_command "${prefix}-modinfo.txt" modinfo "${MODULE}"
fi

{
  echo "===== runtime srcversion ====="
  /usr/bin/cat "/sys/module/${MODULE}/srcversion" 2>&1 || true
  echo
  echo "===== module parameters ====="
  for param in /sys/module/"${MODULE}"/parameters/*; do
    [[ -e "${param}" ]] || continue
    printf '%s=' "$(basename "${param}")"
    /usr/bin/cat "${param}" 2>&1 || true
  done
} > "${prefix}-module-runtime.txt"

for node in video_diag audio_diag source_cadence; do
  if [[ -r "${diag_root}/${node}" ]]; then
    /usr/bin/cat "${diag_root}/${node}" > "${prefix}-${node}.txt" 2>&1 || true
  fi
done

if command -v v4l2-ctl >/dev/null 2>&1; then
  capture_command "${prefix}-v4l2-devices.txt" v4l2-ctl --list-devices
  capture_command "${prefix}-v4l2-all.txt" v4l2-ctl -d "${DEVICE}" --all
  capture_command "${prefix}-v4l2-formats.txt" v4l2-ctl -d "${DEVICE}" --list-formats-ext
fi
if command -v arecord >/dev/null 2>&1; then
  capture_command "${prefix}-alsa-capture-devices.txt" arecord -l
fi

if command -v pw-dump >/dev/null 2>&1; then
  capture_command "${prefix}-pw-dump.json" pw-dump -N
fi
if command -v wpctl >/dev/null 2>&1; then
  capture_command "${prefix}-wpctl-status.txt" wpctl status -n
fi
if command -v pactl >/dev/null 2>&1; then
  capture_command "${prefix}-pactl-sources.txt" pactl list sources
fi

if command -v journalctl >/dev/null 2>&1; then
  journalctl -k -b --no-pager 2>/dev/null |
    rg -i "${MODULE}|hws:|videobuf2|dma|timeout|reset|error|BUG:|Oops|soft lockup" \
      > "${prefix}-journal-kernel-filtered.log" || true
fi

echo "snapshot_prefix=${prefix}"
