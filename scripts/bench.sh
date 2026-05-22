#!/usr/bin/env bash
set -euo pipefail

DEVICE="${DEVICE:-/dev/video0}"
DURATION="${DURATION:-60}"
OUT_BASE="${OUT_BASE:-./bench-results}"
RUN_TAG="${RUN_TAG:-safe-bench}"
MODULE="${MODULE:-HwsUHDX1Capture}"
SIZE="${SIZE:-1920x1080}"
FPS="${FPS:-60}"
DIAG_ROOT="${DIAG_ROOT:-}"

usage() {
  cat <<'USAGE'
Usage: scripts/bench.sh [options]

Safe-by-default 60s capture benchmark.

Options:
  -d <device>    Video device path (default: /dev/video0)
  -t <seconds>   Capture duration (default: 60)
  -o <out_base>  Output base directory (default: ./bench-results)
  -g <tag>       Run tag (default: safe-bench)
  -s <WxH>       Capture size (default: 1920x1080)
  -f <fps>       Requested fps (default: 60)
  -h             Show help

Env overrides: DEVICE DURATION OUT_BASE RUN_TAG MODULE SIZE FPS DIAG_ROOT
USAGE
}

while getopts ":d:t:o:g:s:f:h" opt; do
  case "${opt}" in
    d) DEVICE="${OPTARG}" ;;
    t) DURATION="${OPTARG}" ;;
    o) OUT_BASE="${OPTARG}" ;;
    g) RUN_TAG="${OPTARG}" ;;
    s) SIZE="${OPTARG}" ;;
    f) FPS="${OPTARG}" ;;
    h) usage; exit 0 ;;
    :) echo "Missing argument for -${OPTARG}" >&2; exit 2 ;;
    \?) echo "Unknown option: -${OPTARG}" >&2; exit 2 ;;
  esac
done

if ! [[ "${DURATION}" =~ ^[0-9]+$ ]] || [[ "${DURATION}" -lt 1 ]]; then
  echo "Invalid duration: ${DURATION}" >&2
  exit 2
fi
if ! [[ "${FPS}" =~ ^[0-9]+$ ]] || [[ "${FPS}" -lt 1 ]]; then
  echo "Invalid fps: ${FPS}" >&2
  exit 2
fi

stamp="$(date -u +%Y%m%d-%H%M%S)"
run_id="${stamp}-$(echo "${RUN_TAG}" | tr -cs 'A-Za-z0-9._-' '-')"
outdir="${OUT_BASE}/${run_id}"
mkdir -p "${outdir}"

v4l2_all="${outdir}/v4l2-all.txt"
capture_log="${outdir}/capture.log"
journal_kern="${outdir}/journal-kernel-filtered.log"
summary="${outdir}/summary.env"
score_summary="${outdir}/${run_id}-summary.txt"
video_diag_before="${outdir}/video-diag.before.txt"
video_diag_after="${outdir}/video-diag.after.txt"
audio_diag_before="${outdir}/audio-diag.before.txt"
audio_diag_after="${outdir}/audio-diag.after.txt"
source_cadence_before="${outdir}/source-cadence.before.txt"
source_cadence_after="${outdir}/source-cadence.after.txt"

if [[ -z "${DIAG_ROOT}" ]]; then
  if [[ -r /proc/hwsuhdx1/video_diag ]]; then
    DIAG_ROOT="/proc/hwsuhdx1"
  else
    DIAG_ROOT="/sys/kernel/debug/hwsuhdx1"
  fi
fi

capture_debugfs_snapshot() {
  local node="${1}"
  local out="${2}"

  if [[ -r "${node}" ]]; then
    /usr/bin/cat "${node}" > "${out}" 2>/dev/null || true
  fi
}

diag_total_delta() {
  local before="${1}"
  local after="${2}"
  local field="${3}"

  if [[ ! -s "${before}" || ! -s "${after}" ]]; then
    echo 0
    return
  fi

  awk -v field="${field}" '
    FNR == 1 {
      delete col
      for (i = 1; i <= NF; i++)
        col[$i] = i
      next
    }
    NR == FNR {
      if (col[field] && $1 ~ /^[0-9]+$/)
        before[$1] = $(col[field])
      next
    }
    col[field] && $1 ~ /^[0-9]+$/ {
      total += $(col[field]) - before[$1]
    }
    END { print total + 0 }
  ' "${before}" "${after}"
}

capture_debugfs_snapshot "${DIAG_ROOT}/video_diag" "${video_diag_before}"
capture_debugfs_snapshot "${DIAG_ROOT}/audio_diag" "${audio_diag_before}"
capture_debugfs_snapshot "${DIAG_ROOT}/source_cadence" "${source_cadence_before}"

if command -v v4l2-ctl >/dev/null 2>&1; then
  v4l2-ctl --device="${DEVICE}" --all > "${v4l2_all}" 2>&1 || true
else
  echo "v4l2-ctl not found" > "${v4l2_all}"
fi

backend="none"
if command -v ffmpeg >/dev/null 2>&1; then
  backend="ffmpeg"
  ffmpeg -hide_banner -nostdin -loglevel info -stats \
    -f v4l2 -framerate "${FPS}" -video_size "${SIZE}" -i "${DEVICE}" \
    -t "${DURATION}" -an -f null - > "${capture_log}" 2>&1 || true
elif command -v v4l2-ctl >/dev/null 2>&1; then
  backend="v4l2-ctl"
  v4l2-ctl --verbose --device="${DEVICE}" \
    --set-fmt-video=width="${SIZE%x*}",height="${SIZE#*x}",pixelformat=YUYV \
    --set-parm="${FPS}" --stream-mmap=3 --stream-poll \
    --stream-count="$((DURATION * FPS))" --stream-to=/dev/null > "${capture_log}" 2>&1 || true
else
  echo "No capture backend found (ffmpeg or v4l2-ctl)" > "${capture_log}"
fi

if command -v journalctl >/dev/null 2>&1; then
  journalctl -k -b --no-pager | rg -i "${MODULE}|videobuf2|dma|timeout|reset|error" > "${journal_kern}" || true
else
  echo "journalctl not found" > "${journal_kern}"
fi

capture_debugfs_snapshot "${DIAG_ROOT}/video_diag" "${video_diag_after}"
capture_debugfs_snapshot "${DIAG_ROOT}/audio_diag" "${audio_diag_after}"
capture_debugfs_snapshot "${DIAG_ROOT}/source_cadence" "${source_cadence_after}"

actual_frames=0
if [[ "${backend}" == "ffmpeg" ]]; then
  actual_frames="$(grep -Eo 'frame=\s*[0-9]+' "${capture_log}" | tail -n1 | tr -dc '0-9' || echo 0)"
elif [[ "${backend}" == "v4l2-ctl" ]]; then
  actual_frames="$(grep -Eo '[0-9]+ frames' "${capture_log}" | tail -n1 | tr -dc '0-9' || echo 0)"
fi
actual_frames="${actual_frames:-0}"

diag_available=0
audio_diag_available=0
source_cadence_available=0
if [[ -s "${video_diag_before}" && -s "${video_diag_after}" ]]; then
  diag_available=1
fi
if [[ -s "${audio_diag_before}" && -s "${audio_diag_after}" ]]; then
  audio_diag_available=1
fi
if [[ -s "${source_cadence_after}" ]]; then
  source_cadence_available=1
fi

audio_source_lost_periods_delta="$(diag_total_delta "${audio_diag_before}" "${audio_diag_after}" source_lost_periods)"
audio_timer_silence_injects_delta="$(diag_total_delta "${audio_diag_before}" "${audio_diag_after}" timer_silence_injects)"
audio_workqueue_requeues_delta="$(diag_total_delta "${audio_diag_before}" "${audio_diag_after}" workqueue_requeues)"
audio_memcopy_failures_delta="$(diag_total_delta "${audio_diag_before}" "${audio_diag_after}" memcopy_failures)"
audio_source_starved_transitions_delta="$(diag_total_delta "${audio_diag_before}" "${audio_diag_after}" source_starved_transitions)"
audio_recovery_transitions_delta="$(diag_total_delta "${audio_diag_before}" "${audio_diag_after}" recovery_transitions)"
audio_timer_late_events_delta="$(diag_total_delta "${audio_diag_before}" "${audio_diag_after}" timer_late_events)"
video_reused_no_fresh_delta="$(diag_total_delta "${video_diag_before}" "${video_diag_after}" reused_no_fresh_runs)"
video_reused_backpressure_delta="$(diag_total_delta "${video_diag_before}" "${video_diag_after}" reused_backpressure_runs)"
video_ts_non_monotonic_delta="$(diag_total_delta "${video_diag_before}" "${video_diag_after}" ts_non_monotonic_events)"
video_seq_non_monotonic_delta="$(diag_total_delta "${video_diag_before}" "${video_diag_after}" seq_non_monotonic_events)"
video_signal_stalled_transitions_delta="$(diag_total_delta "${video_diag_before}" "${video_diag_after}" signal_stalled_transitions)"
video_signal_no_signal_transitions_delta="$(diag_total_delta "${video_diag_before}" "${video_diag_after}" signal_no_signal_transitions)"
video_no_signal_placeholder_frames_delta="$(diag_total_delta "${video_diag_before}" "${video_diag_after}" no_signal_placeholder_frames)"
video_stalled_placeholder_frames_delta="$(diag_total_delta "${video_diag_before}" "${video_diag_after}" stalled_placeholder_frames)"
video_fallback_last_stable_ticks_delta="$(diag_total_delta "${video_diag_before}" "${video_diag_after}" fallback_last_stable_ticks)"
video_fallback_requested_ticks_delta="$(diag_total_delta "${video_diag_before}" "${video_diag_after}" fallback_requested_ticks)"
video_fallback_default_60_ticks_delta="$(diag_total_delta "${video_diag_before}" "${video_diag_after}" fallback_default_60_ticks)"
source_truth_mismatch_flag=0
source_truth_active_fps=0
if [[ -s "${source_cadence_after}" ]]; then
  source_truth_active_fps="$(awk 'NR > 1 && $1 == 0 { print $3; exit }' "${source_cadence_after}")"
  source_truth_active_fps="${source_truth_active_fps:-0}"
  source_truth_mismatch_flag="$(awk -v req="${FPS}" -v src="${source_truth_active_fps}" 'BEGIN {
    if (req <= 0 || src <= 0) { print 0; exit }
    diff = req - src
    if (diff < 0) diff = -diff
    print diff > (src * 0.10) ? 1 : 0
  }')"
fi

{
  echo "artifact_schema=safe-bench-v2"
  echo "run_id=${run_id}"
  echo "timestamp_utc=$(date -u --iso-8601=seconds)"
  echo "module=${MODULE}"
  echo "device=${DEVICE}"
  echo "duration=${DURATION}"
  echo "size=${SIZE}"
  echo "fps=${FPS}"
  echo "backend=${backend}"
  echo "actual_frames=${actual_frames}"
  echo "diag_available=${diag_available}"
  echo "audio_diag_available=${audio_diag_available}"
  echo "source_cadence_available=${source_cadence_available}"
  echo "diag_root=${DIAG_ROOT}"
  echo "source_truth_active_fps=${source_truth_active_fps}"
  echo "source_truth_mismatch_flag=${source_truth_mismatch_flag}"
  echo "audio_source_lost_periods_delta=${audio_source_lost_periods_delta}"
  echo "audio_timer_silence_injects_delta=${audio_timer_silence_injects_delta}"
  echo "audio_workqueue_requeues_delta=${audio_workqueue_requeues_delta}"
  echo "audio_memcopy_failures_delta=${audio_memcopy_failures_delta}"
  echo "audio_source_starved_transitions_delta=${audio_source_starved_transitions_delta}"
  echo "audio_recovery_transitions_delta=${audio_recovery_transitions_delta}"
  echo "audio_timer_late_events_delta=${audio_timer_late_events_delta}"
  echo "video_reused_no_fresh_delta=${video_reused_no_fresh_delta}"
  echo "video_reused_backpressure_delta=${video_reused_backpressure_delta}"
  echo "video_ts_non_monotonic_delta=${video_ts_non_monotonic_delta}"
  echo "video_seq_non_monotonic_delta=${video_seq_non_monotonic_delta}"
  echo "video_signal_stalled_transitions_delta=${video_signal_stalled_transitions_delta}"
  echo "video_signal_no_signal_transitions_delta=${video_signal_no_signal_transitions_delta}"
  echo "video_no_signal_placeholder_frames_delta=${video_no_signal_placeholder_frames_delta}"
  echo "video_stalled_placeholder_frames_delta=${video_stalled_placeholder_frames_delta}"
  echo "video_fallback_last_stable_ticks_delta=${video_fallback_last_stable_ticks_delta}"
  echo "video_fallback_requested_ticks_delta=${video_fallback_requested_ticks_delta}"
  echo "video_fallback_default_60_ticks_delta=${video_fallback_default_60_ticks_delta}"
  echo "v4l2_all=${v4l2_all}"
  echo "capture_log=${capture_log}"
  echo "journal_kernel_filtered=${journal_kern}"
  echo "outdir=${outdir}"
} > "${summary}"

cp "${summary}" "${score_summary}"
cat "${summary}"
echo "summary=${score_summary}"
