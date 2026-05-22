#!/usr/bin/env bash
set -euo pipefail

MODULE="HwsUHDX1Capture"
MODE=""
SINCE=""
DIAG_VALUE=""

usage() {
  cat <<'USAGE'
Usage: obs-diag-priv.sh [options]

Privileged helper for OBS diagnostics. Intended to run via sudoers NOPASSWD.

Modes:
  --self-test                 Validate helper availability.
  --sample                    Emit one privileged diagnostics snapshot.
  --follow-kernel             Follow kernel journal (stdout stream).
  --follow-system             Follow system journal (stdout stream).
  --kernel-since <timestamp>  Print kernel journal since timestamp.
  --get-diag                  Print current diag_enable value.
  --set-diag <0|1>            Set diag_enable value.

Options:
  --module <name>             Module name (default: HwsUHDX1Capture)
  -h, --help                  Show help
USAGE
}

while (($#)); do
  case "$1" in
    --self-test) MODE="self-test"; shift ;;
    --sample) MODE="sample"; shift ;;
    --follow-kernel) MODE="follow-kernel"; shift ;;
    --follow-system) MODE="follow-system"; shift ;;
    --kernel-since) MODE="kernel-since"; SINCE="${2:-}"; shift 2 ;;
    --get-diag) MODE="get-diag"; shift ;;
    --set-diag) MODE="set-diag"; DIAG_VALUE="${2:-}"; shift 2 ;;
    --module) MODULE="${2:-}"; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) echo "Unknown argument: $1" >&2; exit 2 ;;
  esac
done

if [[ -z "${MODE}" ]]; then
  usage
  exit 2
fi

if [[ "${MODE}" == "self-test" ]]; then
  echo "ok"
  exit 0
fi

if [[ "${MODE}" == "follow-kernel" ]]; then
  exec journalctl -k -f -o short-iso
fi

if [[ "${MODE}" == "follow-system" ]]; then
  exec journalctl -f -o short-iso
fi

if [[ "${MODE}" == "kernel-since" ]]; then
  if [[ -z "${SINCE}" ]]; then
    echo "--kernel-since requires timestamp argument" >&2
    exit 2
  fi
  exec journalctl -k -b --since "${SINCE}" --no-pager
fi

if [[ "${MODE}" == "get-diag" ]]; then
  cat "/sys/module/${MODULE}/parameters/diag_enable"
  exit 0
fi

if [[ "${MODE}" == "set-diag" ]]; then
  if [[ "${DIAG_VALUE}" != "0" && "${DIAG_VALUE}" != "1" ]]; then
    echo "--set-diag requires 0 or 1" >&2
    exit 2
  fi
  echo "${DIAG_VALUE}" > "/sys/module/${MODULE}/parameters/diag_enable"
  exit 0
fi

if [[ "${MODE}" == "sample" ]]; then
  echo "-- privileged sample_utc=$(date -u --iso-8601=seconds) --"
  echo "module=${MODULE}"
  if [[ -r "/sys/module/${MODULE}/srcversion" ]]; then
    echo "module_srcversion=$(cat "/sys/module/${MODULE}/srcversion" 2>/dev/null || true)"
  fi

  echo "-- hws debugfs video_diag --"
  cat /sys/kernel/debug/hwsuhdx1/video_diag 2>/dev/null || true
  echo "-- hws debugfs audio_diag --"
  cat /sys/kernel/debug/hwsuhdx1/audio_diag 2>/dev/null || true

  echo "-- dmesg filtered tail (driver/perf) --"
  dmesg --ctime 2>/dev/null | rg -i 'HwsUHDX1Capture|videobuf2|uvcvideo|retire_capture_urb|callbacks suppressed|timeout|reset|error|BUG:|Oops|soft lockup' | tail -n 80 || true

  echo "-- interrupts (hws/uvc/xhci/nvidia) --"
  cat /proc/interrupts 2>/dev/null | rg -i 'hws|uvc|xhci|nvidia' || true
  exit 0
fi

echo "Invalid mode: ${MODE}" >&2
exit 2
