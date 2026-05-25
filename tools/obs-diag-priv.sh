#!/usr/bin/env bash
set -euo pipefail

MODULE="HwsUHDX1Capture"
MODE=""
SINCE=""
DIAG_VALUE=""
TRACE_VALUE=""
TRACE_TOKEN=""
TRACE_PID=""
TRACE_FUNCTIONS=0
TRACE_SECONDS="30"

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
  --get-audio-trace           Print current audio_trace_enable value.
  --set-audio-trace <0|1>     Set audio_trace_enable value.
  --trace-capable             Verify that kernel trace recording is available.
  --trace-until-pid <token> <pid> <seconds>
                               Record fixed HWS timing events until PID exits
                               or the bounded duration expires (1-600 sec).
  --trace-report <token>      Print a text rendering of a captured trace.
  --trace-export <token>      Export the binary trace.dat artifact.
  --trace-remove <token>      Delete a completed helper trace.

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
    --get-audio-trace) MODE="get-audio-trace"; shift ;;
    --set-audio-trace) MODE="set-audio-trace"; TRACE_VALUE="${2:-}"; shift 2 ;;
    --trace-capable) MODE="trace-capable"; shift ;;
    --trace-until-pid)
      MODE="trace-until-pid"; TRACE_TOKEN="${2:-}"; TRACE_PID="${3:-}"
      TRACE_SECONDS="${4:-}"; shift 4
      ;;
    --trace-report) MODE="trace-report"; TRACE_TOKEN="${2:-}"; shift 2 ;;
    --trace-export) MODE="trace-export"; TRACE_TOKEN="${2:-}"; shift 2 ;;
    --trace-remove) MODE="trace-remove"; TRACE_TOKEN="${2:-}"; shift 2 ;;
    --trace-functions) TRACE_FUNCTIONS=1; shift ;;
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

if [[ "${MODE}" == "get-audio-trace" ]]; then
  cat "/sys/module/${MODULE}/parameters/audio_trace_enable"
  exit 0
fi

if [[ "${MODE}" == "set-audio-trace" ]]; then
  if [[ "${TRACE_VALUE}" != "0" && "${TRACE_VALUE}" != "1" ]]; then
    echo "--set-audio-trace requires 0 or 1" >&2
    exit 2
  fi
  echo "${TRACE_VALUE}" > "/sys/module/${MODULE}/parameters/audio_trace_enable"
  exit 0
fi

valid_trace_token() {
  [[ "${TRACE_TOKEN}" =~ ^[A-Za-z0-9._-]+$ ]] && [[ "${#TRACE_TOKEN}" -le 160 ]]
}

trace_dir="/var/tmp/hws-obs-diag-${SUDO_UID:-0}"
trace_file=""
if [[ "${MODE}" == "trace-until-pid" || "${MODE}" == "trace-report" ||
      "${MODE}" == "trace-export" || "${MODE}" == "trace-remove" ]]; then
  if ! valid_trace_token; then
    echo "Invalid trace token." >&2
    exit 2
  fi
  trace_file="${trace_dir}/${TRACE_TOKEN}.dat"
fi

if [[ "${MODE}" == "trace-capable" ]]; then
  if ! command -v trace-cmd >/dev/null 2>&1; then
    echo "trace-cmd is not installed" >&2
    exit 1
  fi
  trace-cmd list -e >/dev/null
  echo "ok"
  exit 0
fi

if [[ "${MODE}" == "trace-until-pid" ]]; then
  if ! [[ "${TRACE_PID}" =~ ^[0-9]+$ ]] || ! kill -0 "${TRACE_PID}" 2>/dev/null; then
    echo "Invalid or inactive trace PID." >&2
    exit 2
  fi
  if ! [[ "${TRACE_SECONDS}" =~ ^[0-9]+$ ]] || [[ "${TRACE_SECONDS}" -lt 1 ]] ||
     [[ "${TRACE_SECONDS}" -gt 600 ]]; then
    echo "Invalid trace duration (expected 1-600 seconds)." >&2
    exit 2
  fi
  install -d -m 0700 "${trace_dir}"
  trace_args=(
    record -o "${trace_file}"
    -e irq:irq_handler_entry -e irq:irq_handler_exit
    -e workqueue:workqueue_queue_work
    -e workqueue:workqueue_execute_start -e workqueue:workqueue_execute_end
    -e timer:hrtimer_start -e timer:hrtimer_expire_entry -e timer:hrtimer_expire_exit
    -e sched:sched_wakeup -e sched:sched_switch
  )
  if [[ "${TRACE_FUNCTIONS}" == "1" ]]; then
    trace_args+=(
      -p function -l video_data_process -l audio_data_process
      -l hws_audio_publish_timer_fn -l MemCopyVideoToSteam
      -l MemCopyAudioToSteam -l SetQuene -l SetAudioQuene
    )
  fi
  trace-cmd "${trace_args[@]}" -- /usr/bin/bash -c \
    'pid="$1"; seconds="$2"; end=$((SECONDS + seconds)); while kill -0 "$pid" 2>/dev/null && ((SECONDS < end)); do sleep 1; done' \
    _ "${TRACE_PID}" "${TRACE_SECONDS}"
  exit $?
fi

if [[ "${MODE}" == "trace-report" ]]; then
  [[ -r "${trace_file}" ]] || exit 1
  exec trace-cmd report "${trace_file}"
fi

if [[ "${MODE}" == "trace-export" ]]; then
  [[ -r "${trace_file}" ]] || exit 1
  exec cat "${trace_file}"
fi

if [[ "${MODE}" == "trace-remove" ]]; then
  rm -f "${trace_file}"
  exit 0
fi

if [[ "${MODE}" == "sample" ]]; then
  echo "-- privileged sample_utc=$(date -u --iso-8601=seconds) --"
  echo "module=${MODULE}"
  if [[ -r "/sys/module/${MODULE}/srcversion" ]]; then
    echo "module_srcversion=$(cat "/sys/module/${MODULE}/srcversion" 2>/dev/null || true)"
  fi

  echo "-- hws diag video_diag --"
  cat /proc/hwsuhdx1/video_diag 2>/dev/null ||
    cat /sys/kernel/debug/hwsuhdx1/video_diag 2>/dev/null || true
  echo "-- hws diag audio_diag --"
  cat /proc/hwsuhdx1/audio_diag 2>/dev/null ||
    cat /sys/kernel/debug/hwsuhdx1/audio_diag 2>/dev/null || true
  echo "-- hws diag source_cadence --"
  cat /proc/hwsuhdx1/source_cadence 2>/dev/null ||
    cat /sys/kernel/debug/hwsuhdx1/source_cadence 2>/dev/null || true

  echo "-- pstore listing --"
  ls -la /sys/fs/pstore 2>/dev/null || true

  echo "-- dmesg filtered tail (driver/perf) --"
  dmesg --ctime 2>/dev/null | rg -i 'HwsUHDX1Capture|videobuf2|uvcvideo|retire_capture_urb|callbacks suppressed|timeout|reset|error|BUG:|Oops|soft lockup' | tail -n 80 || true

  echo "-- interrupts (hws/uvc/xhci/nvidia) --"
  cat /proc/interrupts 2>/dev/null | rg -i 'hws|uvc|xhci|nvidia' || true
  exit 0
fi

echo "Invalid mode: ${MODE}" >&2
exit 2
