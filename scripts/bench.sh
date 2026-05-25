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
RUN_V4L2_COMPLIANCE="${RUN_V4L2_COMPLIANCE:-1}"
V4L2_COMPLIANCE_STREAM_FRAMES="${V4L2_COMPLIANCE_STREAM_FRAMES:-0}"
V4L2_COMPLIANCE_TIMEOUT_SEC="${V4L2_COMPLIANCE_TIMEOUT_SEC:-90}"
CAPTURE_TIMEOUT_GRACE_SEC="${CAPTURE_TIMEOUT_GRACE_SEC:-15}"
CAPTURE_PIPEWIRE="${CAPTURE_PIPEWIRE:-1}"
PW_PROFILER_SAMPLES="${PW_PROFILER_SAMPLES:-0}"
CAPTURE_FULL_SNAPSHOT="${CAPTURE_FULL_SNAPSHOT:-1}"

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
  RUN_V4L2_COMPLIANCE=0|1             Run isolated API conformance check (default: 1)
  V4L2_COMPLIANCE_STREAM_FRAMES=<n>   Stream frames during compliance (default: 0)
  V4L2_COMPLIANCE_TIMEOUT_SEC=<n>      Terminate stuck compliance run (default: 90)
  CAPTURE_TIMEOUT_GRACE_SEC=<n>        Grace above requested duration (default: 15)
  CAPTURE_PIPEWIRE=0|1                Capture PipeWire evidence (default: 1)
  PW_PROFILER_SAMPLES=<n>             Profiler samples during capture (default: 0/off)
  CAPTURE_FULL_SNAPSHOT=0|1           Collect complete pre/post state snapshots (default: 1)
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
for toggle in RUN_V4L2_COMPLIANCE CAPTURE_PIPEWIRE CAPTURE_FULL_SNAPSHOT; do
  if [[ "${!toggle}" != "0" && "${!toggle}" != "1" ]]; then
    echo "Invalid ${toggle}: ${!toggle} (expected 0 or 1)" >&2
    exit 2
  fi
done
for numeric in V4L2_COMPLIANCE_STREAM_FRAMES V4L2_COMPLIANCE_TIMEOUT_SEC \
  CAPTURE_TIMEOUT_GRACE_SEC PW_PROFILER_SAMPLES; do
  if ! [[ "${!numeric}" =~ ^[0-9]+$ ]]; then
    echo "Invalid ${numeric}: ${!numeric}" >&2
    exit 2
  fi
done
if [[ "${V4L2_COMPLIANCE_TIMEOUT_SEC}" -lt 1 ]]; then
  echo "Invalid V4L2_COMPLIANCE_TIMEOUT_SEC: ${V4L2_COMPLIANCE_TIMEOUT_SEC}" >&2
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
v4l2_compliance_log="${outdir}/v4l2-compliance.log"
pw_dump_before="${outdir}/pw-dump.before.json"
pw_dump_after="${outdir}/pw-dump.after.json"
pw_top_log="${outdir}/pw-top.log"
pw_profiler_log="${outdir}/pw-profiler.json"
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
snapshot_tool="${repo_root}/tools/collect-evidence.sh"

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

diag_value_after() {
  local after="${1}"
  local field="${2}"
  local channel="${3:-0}"

  if [[ ! -s "${after}" ]]; then
    echo 0
    return
  fi

  awk -v field="${field}" -v channel="${channel}" '
    FNR == 1 {
      for (i = 1; i <= NF; i++) col[$i] = i
      next
    }
    $1 == channel && col[field] {
      print $(col[field])
      found = 1
      exit
    }
    END {
      if (!found) print 0
    }
  ' "${after}"
}

active_source_channel() {
  local src_file="${1}"
  if [[ ! -s "${src_file}" ]]; then
    echo 0
    return
  fi
  awk '
    NR == 1 { next }
    $1 ~ /^[0-9]+$/ && $3 > 0 {
      if ($3 > best_fps) {
        best_fps = $3
        best_ch = $1
      }
    }
    END {
      if (best_ch == "") best_ch = 0
      print best_ch
    }
  ' "${src_file}"
}

capture_pw_dump() {
  local out="${1}"

  if [[ "${CAPTURE_PIPEWIRE}" == "1" ]] && command -v pw-dump >/dev/null 2>&1; then
    pw-dump -N > "${out}" 2>&1 || true
  fi
}

capture_debugfs_snapshot "${DIAG_ROOT}/video_diag" "${video_diag_before}"
capture_debugfs_snapshot "${DIAG_ROOT}/audio_diag" "${audio_diag_before}"
capture_debugfs_snapshot "${DIAG_ROOT}/source_cadence" "${source_cadence_before}"
capture_pw_dump "${pw_dump_before}"
if [[ "${CAPTURE_FULL_SNAPSHOT}" == "1" && -x "${snapshot_tool}" ]]; then
  "${snapshot_tool}" --out "${outdir}" --phase pre --module "${MODULE}" --device "${DEVICE}" \
    > "${outdir}/snapshot.pre.stdout.txt" 2>&1 || true
fi

if command -v v4l2-ctl >/dev/null 2>&1; then
  v4l2-ctl --device="${DEVICE}" --all > "${v4l2_all}" 2>&1 || true
else
  echo "v4l2-ctl not found" > "${v4l2_all}"
fi

v4l2_compliance_status="disabled"
v4l2_compliance_exit_code=0
if [[ "${RUN_V4L2_COMPLIANCE}" == "1" ]]; then
  if command -v v4l2-compliance >/dev/null 2>&1; then
    compliance_args=(--device "${DEVICE}" --no-progress --color never)
    if [[ "${V4L2_COMPLIANCE_STREAM_FRAMES}" -gt 0 ]]; then
      compliance_args+=(--streaming "${V4L2_COMPLIANCE_STREAM_FRAMES}")
    fi
    set +e
    timeout --signal=TERM --kill-after=5s "${V4L2_COMPLIANCE_TIMEOUT_SEC}s" \
      v4l2-compliance "${compliance_args[@]}" > "${v4l2_compliance_log}" 2>&1
    v4l2_compliance_exit_code=$?
    set -e
    if [[ "${v4l2_compliance_exit_code}" == "0" ]]; then
      v4l2_compliance_status="pass"
    elif [[ "${v4l2_compliance_exit_code}" == "124" || "${v4l2_compliance_exit_code}" == "137" ]]; then
      v4l2_compliance_status="timeout"
      printf '\nTIMEOUT: v4l2-compliance exceeded %ss\n' "${V4L2_COMPLIANCE_TIMEOUT_SEC}" \
        >> "${v4l2_compliance_log}"
    else
      v4l2_compliance_status="fail"
    fi
  else
    v4l2_compliance_status="tool_missing"
    echo "v4l2-compliance not found" > "${v4l2_compliance_log}"
  fi
fi

declare -a monitor_pids=()
start_monitor() {
  "$@" &
  monitor_pids+=("$!")
}
stop_monitors() {
  local pid
  for pid in "${monitor_pids[@]:-}"; do
    kill "${pid}" >/dev/null 2>&1 || true
    wait "${pid}" >/dev/null 2>&1 || true
  done
}
trap stop_monitors EXIT

if [[ "${CAPTURE_PIPEWIRE}" == "1" ]] && command -v pw-top >/dev/null 2>&1; then
  start_monitor bash -c 'exec pw-top -b > "$1" 2>&1' _ "${pw_top_log}"
fi
if [[ "${CAPTURE_PIPEWIRE}" == "1" && "${PW_PROFILER_SAMPLES}" -gt 0 ]] &&
   command -v pw-profiler >/dev/null 2>&1; then
  start_monitor bash -c 'exec pw-profiler -J -n "$1" > "$2" 2>&1' _ \
    "${PW_PROFILER_SAMPLES}" "${pw_profiler_log}"
fi

backend="none"
capture_status="backend_missing"
capture_exit_code=127
capture_timeout_sec="$((DURATION + CAPTURE_TIMEOUT_GRACE_SEC))"
if command -v ffmpeg >/dev/null 2>&1; then
  backend="ffmpeg"
  set +e
  timeout --signal=TERM --kill-after=5s "${capture_timeout_sec}s" \
    ffmpeg -hide_banner -nostdin -loglevel info -stats \
    -f v4l2 -framerate "${FPS}" -video_size "${SIZE}" -i "${DEVICE}" \
    -t "${DURATION}" -an -f null - > "${capture_log}" 2>&1
  capture_exit_code=$?
  set -e
elif command -v v4l2-ctl >/dev/null 2>&1; then
  backend="v4l2-ctl"
  set +e
  timeout --signal=TERM --kill-after=5s "${capture_timeout_sec}s" \
    v4l2-ctl --verbose --device="${DEVICE}" \
    --set-fmt-video=width="${SIZE%x*}",height="${SIZE#*x}",pixelformat=YUYV \
    --set-parm="${FPS}" --stream-mmap=3 --stream-poll \
    --stream-count="$((DURATION * FPS))" --stream-to=/dev/null > "${capture_log}" 2>&1
  capture_exit_code=$?
  set -e
else
  echo "No capture backend found (ffmpeg or v4l2-ctl)" > "${capture_log}"
fi
if [[ "${capture_exit_code}" == "0" ]]; then
  capture_status="pass"
elif [[ "${capture_exit_code}" == "124" || "${capture_exit_code}" == "137" ]]; then
  capture_status="timeout"
  printf '\nTIMEOUT: capture exceeded %ss\n' "${capture_timeout_sec}" >> "${capture_log}"
elif [[ "${backend}" != "none" ]]; then
  capture_status="fail"
fi
stop_monitors
trap - EXIT

if command -v journalctl >/dev/null 2>&1; then
  journalctl -k -b --no-pager | rg -i "${MODULE}|videobuf2|dma|timeout|reset|error" > "${journal_kern}" || true
else
  echo "journalctl not found" > "${journal_kern}"
fi

capture_debugfs_snapshot "${DIAG_ROOT}/video_diag" "${video_diag_after}"
capture_debugfs_snapshot "${DIAG_ROOT}/audio_diag" "${audio_diag_after}"
capture_debugfs_snapshot "${DIAG_ROOT}/source_cadence" "${source_cadence_after}"
capture_pw_dump "${pw_dump_after}"
if [[ "${CAPTURE_FULL_SNAPSHOT}" == "1" && -x "${snapshot_tool}" ]]; then
  "${snapshot_tool}" --out "${outdir}" --phase post --module "${MODULE}" --device "${DEVICE}" \
    > "${outdir}/snapshot.post.stdout.txt" 2>&1 || true
fi

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
video_signal_active_debounce_confirms_delta="$(diag_total_delta "${video_diag_before}" "${video_diag_after}" signal_active_debounce_confirms)"
video_signal_inactive_debounce_confirms_delta="$(diag_total_delta "${video_diag_before}" "${video_diag_after}" signal_inactive_debounce_confirms)"
video_signal_stall_timeout_events_delta="$(diag_total_delta "${video_diag_before}" "${video_diag_after}" signal_stall_timeout_events)"
video_producer_slot_recoveries_delta="$(diag_total_delta "${video_diag_before}" "${video_diag_after}" producer_slot_recoveries)"
video_producer_stale_frame_reclaims_delta="$(diag_total_delta "${video_diag_before}" "${video_diag_after}" producer_stale_frame_reclaims)"
video_producer_no_free_slots_delta="$(diag_total_delta "${video_diag_before}" "${video_diag_after}" producer_no_free_slots)"
video_active_channel="$(active_source_channel "${source_cadence_after}")"
video_signal_active_bit_ch0="$(diag_value_after "${video_diag_after}" signal_active_bit "${video_active_channel}")"
video_signal_detected_width_ch0="$(diag_value_after "${video_diag_after}" signal_detected_width "${video_active_channel}")"
video_signal_detected_height_ch0="$(diag_value_after "${video_diag_after}" signal_detected_height "${video_active_channel}")"
video_signal_transition_reason_ch0="$(diag_value_after "${video_diag_after}" signal_transition_reason "${video_active_channel}")"
persistent_no_signal_with_active_probe=0
stalled_without_fresh_frames=0
if [[ "${video_signal_active_bit_ch0}" -gt 0 && "${video_no_signal_placeholder_frames_delta}" -gt 0 ]]; then
  persistent_no_signal_with_active_probe=1
fi
if [[ "${video_signal_stalled_transitions_delta}" -gt 0 && "${video_reused_no_fresh_delta}" -gt 0 ]]; then
  stalled_without_fresh_frames=1
fi
source_truth_mismatch_flag=0
source_truth_active_fps=0
if [[ -s "${source_cadence_after}" ]]; then
  source_truth_active_fps="$(awk -v ch="${video_active_channel}" 'NR > 1 && $1 == ch { print $3; exit }' "${source_cadence_after}")"
  source_truth_active_fps="${source_truth_active_fps:-0}"
  source_truth_mismatch_flag="$(awk -v req="${FPS}" -v src="${source_truth_active_fps}" 'BEGIN {
    if (req <= 0 || src <= 0) { print 0; exit }
    diff = req - src
    if (diff < 0) diff = -diff
    print (diff > (src * 0.10)) ? 1 : 0
  }')"
fi

{
  echo "artifact_schema=safe-bench-v3"
  echo "run_id=${run_id}"
  echo "timestamp_utc=$(date -u --iso-8601=seconds)"
  echo "module=${MODULE}"
  echo "device=${DEVICE}"
  echo "duration=${DURATION}"
  echo "size=${SIZE}"
  echo "fps=${FPS}"
  echo "backend=${backend}"
  echo "capture_status=${capture_status}"
  echo "capture_exit_code=${capture_exit_code}"
  echo "capture_timeout_sec=${capture_timeout_sec}"
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
  echo "video_signal_active_debounce_confirms_delta=${video_signal_active_debounce_confirms_delta}"
  echo "video_signal_inactive_debounce_confirms_delta=${video_signal_inactive_debounce_confirms_delta}"
  echo "video_signal_stall_timeout_events_delta=${video_signal_stall_timeout_events_delta}"
  echo "video_producer_slot_recoveries_delta=${video_producer_slot_recoveries_delta}"
  echo "video_producer_stale_frame_reclaims_delta=${video_producer_stale_frame_reclaims_delta}"
  echo "video_producer_no_free_slots_delta=${video_producer_no_free_slots_delta}"
  echo "video_active_channel=${video_active_channel}"
  echo "video_signal_active_bit_ch0=${video_signal_active_bit_ch0}"
  echo "video_signal_detected_width_ch0=${video_signal_detected_width_ch0}"
  echo "video_signal_detected_height_ch0=${video_signal_detected_height_ch0}"
  echo "video_signal_transition_reason_ch0=${video_signal_transition_reason_ch0}"
  echo "persistent_no_signal_with_active_probe=${persistent_no_signal_with_active_probe}"
  echo "stalled_without_fresh_frames=${stalled_without_fresh_frames}"
  echo "v4l2_compliance_status=${v4l2_compliance_status}"
  echo "v4l2_compliance_exit_code=${v4l2_compliance_exit_code}"
  echo "v4l2_compliance_stream_frames=${V4L2_COMPLIANCE_STREAM_FRAMES}"
  echo "v4l2_compliance_timeout_sec=${V4L2_COMPLIANCE_TIMEOUT_SEC}"
  echo "capture_pipewire=${CAPTURE_PIPEWIRE}"
  echo "pw_profiler_samples=${PW_PROFILER_SAMPLES}"
  echo "capture_full_snapshot=${CAPTURE_FULL_SNAPSHOT}"
  echo "v4l2_all=${v4l2_all}"
  echo "v4l2_compliance_log=${v4l2_compliance_log}"
  echo "pw_dump_before=${pw_dump_before}"
  echo "pw_dump_after=${pw_dump_after}"
  echo "pw_top_log=${pw_top_log}"
  echo "pw_profiler_log=${pw_profiler_log}"
  echo "capture_log=${capture_log}"
  echo "journal_kernel_filtered=${journal_kern}"
  echo "outdir=${outdir}"
} > "${summary}"

cp "${summary}" "${score_summary}"
cat "${summary}"
echo "summary=${score_summary}"
