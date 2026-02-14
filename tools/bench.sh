#!/usr/bin/env bash
set -euo pipefail

DEVICE="${DEVICE:-/dev/video0}"
DURATION="${DURATION:-30}"
SIZE="${SIZE:-1920x1080}"
FPS="${FPS:-60}"
OUTDIR_BASE="${OUTDIR_BASE:-./bench-results}"
MODULE="${MODULE:-HwsUHDX1Capture}"
RUN_TAG="${RUN_TAG:-}"
PRETEST_SECONDS="${PRETEST_SECONDS:-}"
DIAG_FILE="${DIAG_FILE:-/sys/kernel/debug/hwsuhdx1/video_diag}"

rate_per_sec() {
  awk -v d="${1}" -v s="${2}" 'BEGIN { if (s <= 0) printf "0.000"; else printf "%.3f", d / s }'
}

parse_pw_top_log() {
  local logfile="${1}"
  local prefix="${2}"
  local out_kv="${3}"

  awk -v pref="${prefix}" '
    BEGIN {
      start=0; end=0; delta=0;
      cstart=0; cend=0; cdelta=0;
      nodes=0; cnodes=0;
    }
    /^[RSI][[:space:]]+[0-9]+/ {
      id=$2;
      err=$9 + 0;
      name="";
      for (i=10; i<=NF; i++) name = name (i==10 ? "" : " ") $i;
      sub(/^[+[:space:]]+/, "", name);

      if (!(id in first)) first[id]=err;
      last[id]=err;

      if (name ~ /Capture_Card|v4l2_input\.pci-0000_0f_00\.0/) {
        if (!(id in cfirst)) cfirst[id]=err;
        clast[id]=err;
      }
    }
    END {
      for (k in last) {
        nodes++;
        s=first[k]; e=last[k]; d=e-s;
        start += s; end += e;
        if (d > 0) delta += d;
      }
      for (k in clast) {
        cnodes++;
        s=cfirst[k]; e=clast[k]; d=e-s;
        cstart += s; cend += e;
        if (d > 0) cdelta += d;
      }
      printf "%s_nodes=%d\n", pref, nodes;
      printf "%s_err_start_total=%d\n", pref, start;
      printf "%s_err_end_total=%d\n", pref, end;
      printf "%s_err_delta_total=%d\n", pref, delta;
      printf "%s_capture_nodes=%d\n", pref, cnodes;
      printf "%s_capture_err_start_total=%d\n", pref, cstart;
      printf "%s_capture_err_end_total=%d\n", pref, cend;
      printf "%s_capture_err_delta_total=%d\n", pref, cdelta;
    }
  ' "${logfile}" > "${out_kv}"
}

usage() {
  cat <<USAGE
Usage: $(basename "$0") [-d device] [-t seconds] [-s WxH] [-r fps] [-o outdir]

Options:
  -d  Video device (default: ${DEVICE})
  -t  Duration in seconds (default: ${DURATION})
  -s  Resolution (default: ${SIZE})
  -r  Target FPS (default: ${FPS})
  -o  Output base directory (default: ${OUTDIR_BASE})
  -g  Optional run tag (example: baseline, post-dkms-fix)

Environment:
  PRETEST_SECONDS  idle pw-top sampling duration before capture (default: DURATION)
USAGE
}

while getopts ":d:t:s:r:o:g:h" opt; do
  case "${opt}" in
    d) DEVICE="${OPTARG}" ;;
    t) DURATION="${OPTARG}" ;;
    s) SIZE="${OPTARG}" ;;
    r) FPS="${OPTARG}" ;;
    o) OUTDIR_BASE="${OPTARG}" ;;
    g) RUN_TAG="${OPTARG}" ;;
    h) usage; exit 0 ;;
    :) echo "Missing arg for -${OPTARG}" >&2; usage; exit 2 ;;
    \?) echo "Unknown option: -${OPTARG}" >&2; usage; exit 2 ;;
  esac
done

if [[ -z "${PRETEST_SECONDS}" ]]; then
  PRETEST_SECONDS="${DURATION}"
fi

stamp="$(date -u +%Y%m%d-%H%M%S)"
run_id="${stamp}"
if [[ -n "${RUN_TAG}" ]]; then
  # Keep run IDs filename-safe for easy sorting/grepping.
  safe_tag="$(echo "${RUN_TAG}" | tr -cs 'A-Za-z0-9._-' '-' | sed 's/^-*//;s/-*$//')"
  run_id="${stamp}-${safe_tag}"
fi
outdir="${OUTDIR_BASE}/${run_id}"
mkdir -p "${outdir}"

info_file="${outdir}/${run_id}-system.txt"
summary_file="${outdir}/${run_id}-summary.txt"
run_log="${outdir}/${run_id}-capture.log"
history_file="${OUTDIR_BASE}/history-v4.csv"
diag_before_file="${outdir}/${run_id}-diag.before.txt"
diag_after_file="${outdir}/${run_id}-diag.after.txt"

{
  echo "timestamp_utc=$(date -u --iso-8601=seconds)"
  echo "run_id=${run_id}"
  echo "run_tag=${RUN_TAG:-none}"
  echo "device=${DEVICE}"
  echo "duration=${DURATION}"
  echo "size=${SIZE}"
  echo "fps=${FPS}"
  echo "kernel=$(uname -r)"
  echo "arch=$(uname -m)"
  echo "hostname=$(hostname)"
} > "${info_file}"

if command -v modinfo >/dev/null 2>&1; then
  modinfo "${MODULE}" > "${outdir}/${run_id}-modinfo.txt" 2>/dev/null || true
  modpath="$(modinfo -n "${MODULE}" 2>/dev/null || true)"
  if [[ -n "${modpath}" && -f "${modpath}" ]]; then
    sha256sum "${modpath}" > "${outdir}/${run_id}-module.sha256"
  fi
fi

if [[ -r "${DIAG_FILE}" ]]; then
  cat "${DIAG_FILE}" > "${diag_before_file}" 2>/dev/null || true
fi

if command -v lsmod >/dev/null 2>&1; then
  lsmod > "${outdir}/${run_id}-lsmod.txt"
fi

if command -v v4l2-ctl >/dev/null 2>&1; then
  v4l2-ctl --device="${DEVICE}" --all > "${outdir}/${run_id}-v4l2-all.txt" 2>&1 || true
  v4l2-ctl --device="${DEVICE}" --list-formats-ext > "${outdir}/${run_id}-v4l2-formats.txt" 2>&1 || true
fi

dmesg --ctime > "${outdir}/${run_id}-dmesg.before.txt" 2>/dev/null || true

backend="none"
pw_top_log="${outdir}/${run_id}-pw-top.log"
pw_top_idle_log="${outdir}/${run_id}-pw-top-idle.log"
pw_top_pid=""

idle_elapsed_seconds=0
idle_nodes=0
idle_err_start_total=0
idle_err_end_total=0
idle_err_delta_total=0
idle_capture_nodes=0
idle_capture_err_start_total=0
idle_capture_err_end_total=0
idle_capture_err_delta_total=0
idle_err_rate_per_s="0.000"
idle_capture_err_rate_per_s="0.000"

if command -v pw-top >/dev/null 2>&1; then
  idle_start_epoch="$(date +%s)"
  # Baseline idle sampling before capture to account for persistent ERR counters.
  pw-top -b -n "$((PRETEST_SECONDS + 2))" > "${pw_top_idle_log}" 2>&1 || true
  idle_end_epoch="$(date +%s)"
  idle_elapsed_seconds="$((idle_end_epoch - idle_start_epoch))"
  [[ "${idle_elapsed_seconds}" -lt 1 ]] && idle_elapsed_seconds=1

  parse_pw_top_log "${pw_top_idle_log}" "idle" "${outdir}/${run_id}-pw-top-idle-metrics.txt"
  # shellcheck disable=SC1090
  source "${outdir}/${run_id}-pw-top-idle-metrics.txt"
  idle_err_rate_per_s="$(rate_per_sec "${idle_err_delta_total}" "${idle_elapsed_seconds}")"
  idle_capture_err_rate_per_s="$(rate_per_sec "${idle_capture_err_delta_total}" "${idle_elapsed_seconds}")"

  # Sample PipeWire status while capture workload runs.
  pw-top -b -n "$((DURATION + 2))" > "${pw_top_log}" 2>&1 &
  pw_top_pid=$!
fi

start_local="$(date '+%Y-%m-%d %H:%M:%S')"
start_epoch="$(date +%s)"

if command -v v4l2-ctl >/dev/null 2>&1; then
  backend="v4l2-ctl-seq"
  v4l2-ctl --verbose --device="${DEVICE}" \
    --set-fmt-video=width="${SIZE%x*}",height="${SIZE#*x}",pixelformat=YUYV \
    --set-parm="${FPS}" --stream-mmap=3 --stream-to=/dev/null \
    --stream-count="$((DURATION * FPS))" --stream-poll > "${run_log}" 2>&1 || true
elif command -v gst-launch-1.0 >/dev/null 2>&1; then
  backend="gstreamer"
  gst-launch-1.0 -e v4l2src device="${DEVICE}" num-buffers="$((DURATION * FPS))" \
    ! "video/x-raw,format=YUY2,width=${SIZE%x*},height=${SIZE#*x},framerate=${FPS}/1" \
    ! queue ! fpsdisplaysink text-overlay=false video-sink=fakesink sync=false \
    > "${run_log}" 2>&1 || true
elif command -v ffmpeg >/dev/null 2>&1; then
  backend="ffmpeg"
  ffmpeg -hide_banner -nostdin -loglevel info -stats \
    -f v4l2 -framerate "${FPS}" -video_size "${SIZE}" -i "${DEVICE}" \
    -t "${DURATION}" -an -f null - > "${run_log}" 2>&1 || true
elif command -v v4l2-ctl >/dev/null 2>&1; then
  backend="v4l2-ctl"
  v4l2-ctl --device="${DEVICE}" --set-fmt-video=width="${SIZE%x*}",height="${SIZE#*x}",pixelformat=YUYV \
    --set-parm="${FPS}" --stream-mmap=3 --stream-to=/dev/null \
    --stream-count="$((DURATION * FPS))" > "${run_log}" 2>&1 || true
else
  echo "No capture backend found (ffmpeg, gst-launch-1.0, or v4l2-ctl)." > "${run_log}"
fi

if [[ -n "${pw_top_pid}" ]]; then
  wait "${pw_top_pid}" 2>/dev/null || true
fi

end_epoch="$(date +%s)"

actual_seconds="$((end_epoch - start_epoch))"
[[ "${actual_seconds}" -lt 1 ]] && actual_seconds=1

expected_frames="$((DURATION * FPS))"
actual_frames=""
source_seq_span_frames=""
source_seq_gap_frames=""
ffmpeg_counter_frames=""

if [[ "${backend}" == "v4l2-ctl-seq" ]]; then
  seq_file="${outdir}/${run_id}-seq.txt"
  grep -Eo 'seq:[[:space:]]*[0-9]+' "${run_log}" | awk '{print $2}' > "${seq_file}" || true
  seq_count="$(wc -l < "${seq_file}" | tr -d ' ')"
  if [[ "${seq_count}" -gt 0 ]]; then
    first_seq="$(head -n1 "${seq_file}")"
    last_seq="$(tail -n1 "${seq_file}")"
    actual_frames="${seq_count}"
    source_seq_span_frames=$(( last_seq - first_seq + 1 ))
    source_seq_gap_frames=$(( source_seq_span_frames - seq_count ))
    if [[ "${source_seq_gap_frames}" -lt 0 ]]; then
      source_seq_gap_frames=0
    fi
  else
    actual_frames=0
    source_seq_span_frames=0
    source_seq_gap_frames=0
  fi
elif [[ "${backend}" == "gstreamer" ]]; then
  actual_frames="$(grep -Eo 'rendered: [0-9]+' "${run_log}" | tail -n1 | tr -dc '0-9' || true)"
elif [[ "${backend}" == "ffmpeg" ]]; then
  ffmpeg_counter_frames="$(grep -Eo 'frame=\s*[0-9]+' "${run_log}" | tail -n1 | tr -dc '0-9' || true)"
  actual_frames="${ffmpeg_counter_frames}"
elif [[ "${backend}" == "v4l2-ctl" ]]; then
  actual_frames="$(grep -Eo '[0-9]+ frames' "${run_log}" | tail -n1 | tr -dc '0-9' || true)"
fi

if [[ -z "${actual_frames}" ]]; then
  actual_frames=0
fi

drop_estimate=$(( expected_frames - actual_frames ))
if [[ "${drop_estimate}" -lt 0 ]]; then
  drop_estimate=0
fi

dmesg --ctime > "${outdir}/${run_id}-dmesg.after.txt" 2>/dev/null || true
dmesg --ctime --since "${start_local}" > "${outdir}/${run_id}-dmesg.since.txt" 2>/dev/null || true
if [[ -r "${DIAG_FILE}" ]]; then
  cat "${DIAG_FILE}" > "${diag_after_file}" 2>/dev/null || true
fi
if [[ -s "${outdir}/${run_id}-dmesg.after.txt" ]]; then
  grep -Ei "${MODULE}|v4l2|vb2|dma|timeout|drop|overrun|underrun|error|warn" \
    "${outdir}/${run_id}-dmesg.after.txt" > "${outdir}/${run_id}-dmesg.filtered.txt" || true
fi

retire_capture_urb_events=0
callbacks_suppressed_events=0
callbacks_suppressed_total=0
uvcvideo_events=0
module_events=0
if [[ -s "${outdir}/${run_id}-dmesg.since.txt" ]]; then
  retire_capture_urb_events="$(grep -c 'retire_capture_urb' "${outdir}/${run_id}-dmesg.since.txt" || true)"
  callbacks_suppressed_events="$(grep -c 'callbacks suppressed' "${outdir}/${run_id}-dmesg.since.txt" || true)"
  callbacks_suppressed_total="$(grep -Eo 'retire_capture_urb: [0-9]+ callbacks suppressed' "${outdir}/${run_id}-dmesg.since.txt" | awk '{sum+=$2} END {print sum+0}')"
  uvcvideo_events="$(grep -ci 'uvcvideo' "${outdir}/${run_id}-dmesg.since.txt" || true)"
  module_events="$(grep -ci "${MODULE}" "${outdir}/${run_id}-dmesg.since.txt" || true)"
fi

test_nodes=0
test_err_start_total=0
test_err_end_total=0
test_err_delta_total=0
test_capture_nodes=0
test_capture_err_start_total=0
test_capture_err_end_total=0
test_capture_err_delta_total=0
test_err_rate_per_s="0.000"
test_capture_err_rate_per_s="0.000"
net_err_rate_over_idle_per_s="0.000"
net_capture_err_rate_over_idle_per_s="0.000"
net_err_delta_over_idle=0
net_capture_err_delta_over_idle=0
if [[ -s "${pw_top_log}" ]]; then
  parse_pw_top_log "${pw_top_log}" "test" "${outdir}/${run_id}-pw-top-test-metrics.txt"
  # shellcheck disable=SC1090
  source "${outdir}/${run_id}-pw-top-test-metrics.txt"
  test_err_rate_per_s="$(rate_per_sec "${test_err_delta_total}" "${actual_seconds}")"
  test_capture_err_rate_per_s="$(rate_per_sec "${test_capture_err_delta_total}" "${actual_seconds}")"
  net_err_rate_over_idle_per_s="$(awk -v t="${test_err_rate_per_s}" -v i="${idle_err_rate_per_s}" 'BEGIN { v=t-i; if (v<0) v=0; printf "%.3f", v }')"
  net_capture_err_rate_over_idle_per_s="$(awk -v t="${test_capture_err_rate_per_s}" -v i="${idle_capture_err_rate_per_s}" 'BEGIN { v=t-i; if (v<0) v=0; printf "%.3f", v }')"
  net_err_delta_over_idle="$(awk -v td="${test_err_delta_total}" -v ir="${idle_err_rate_per_s}" -v s="${actual_seconds}" 'BEGIN {v = td - (ir*s); if (v < 0) v = 0; printf "%.0f", v}')"
  net_capture_err_delta_over_idle="$(awk -v td="${test_capture_err_delta_total}" -v ir="${idle_capture_err_rate_per_s}" -v s="${actual_seconds}" 'BEGIN {v = td - (ir*s); if (v < 0) v = 0; printf "%.0f", v}')"
fi

{
  echo "run_id=${run_id}"
  echo "run_tag=${RUN_TAG:-none}"
  echo "backend=${backend}"
  echo "expected_frames=${expected_frames}"
  echo "actual_frames=${actual_frames}"
  echo "estimated_drop_vs_target_frames=${drop_estimate}"
  if [[ -n "${source_seq_span_frames}" ]]; then
    echo "source_seq_span_frames=${source_seq_span_frames}"
    echo "source_seq_gap_frames=${source_seq_gap_frames}"
  fi
  if [[ -n "${ffmpeg_counter_frames}" ]]; then
    echo "ffmpeg_counter_frames=${ffmpeg_counter_frames}"
  fi
  echo "retire_capture_urb_events=${retire_capture_urb_events}"
  echo "callbacks_suppressed_events=${callbacks_suppressed_events}"
  echo "callbacks_suppressed_total=${callbacks_suppressed_total}"
  echo "uvcvideo_events=${uvcvideo_events}"
  echo "module_events=${module_events}"
  echo "idle_elapsed_seconds=${idle_elapsed_seconds}"
  echo "idle_nodes=${idle_nodes}"
  echo "idle_err_start_total=${idle_err_start_total}"
  echo "idle_err_end_total=${idle_err_end_total}"
  echo "idle_err_delta_total=${idle_err_delta_total}"
  echo "idle_err_rate_per_s=${idle_err_rate_per_s}"
  echo "idle_capture_nodes=${idle_capture_nodes}"
  echo "idle_capture_err_start_total=${idle_capture_err_start_total}"
  echo "idle_capture_err_end_total=${idle_capture_err_end_total}"
  echo "idle_capture_err_delta_total=${idle_capture_err_delta_total}"
  echo "idle_capture_err_rate_per_s=${idle_capture_err_rate_per_s}"
  echo "test_nodes=${test_nodes}"
  echo "test_err_start_total=${test_err_start_total}"
  echo "test_err_end_total=${test_err_end_total}"
  echo "test_err_delta_total=${test_err_delta_total}"
  echo "test_err_rate_per_s=${test_err_rate_per_s}"
  echo "test_capture_nodes=${test_capture_nodes}"
  echo "test_capture_err_start_total=${test_capture_err_start_total}"
  echo "test_capture_err_end_total=${test_capture_err_end_total}"
  echo "test_capture_err_delta_total=${test_capture_err_delta_total}"
  echo "test_capture_err_rate_per_s=${test_capture_err_rate_per_s}"
  echo "net_err_rate_over_idle_per_s=${net_err_rate_over_idle_per_s}"
  echo "net_capture_err_rate_over_idle_per_s=${net_capture_err_rate_over_idle_per_s}"
  echo "net_err_delta_over_idle=${net_err_delta_over_idle}"
  echo "net_capture_err_delta_over_idle=${net_capture_err_delta_over_idle}"
  echo "elapsed_seconds=${actual_seconds}"
  echo "results_dir=${outdir}"
} > "${summary_file}"

if [[ ! -f "${history_file}" ]]; then
  echo "run_id,timestamp_utc,run_tag,backend,device,size,fps,duration,expected_frames,actual_frames,estimated_drop_vs_target_frames,source_seq_span_frames,source_seq_gap_frames,ffmpeg_counter_frames,retire_capture_urb_events,callbacks_suppressed_events,callbacks_suppressed_total,uvcvideo_events,module_events,idle_elapsed_seconds,idle_err_delta_total,idle_err_rate_per_s,idle_capture_err_delta_total,idle_capture_err_rate_per_s,test_err_delta_total,test_err_rate_per_s,test_capture_err_delta_total,test_capture_err_rate_per_s,net_err_rate_over_idle_per_s,net_capture_err_rate_over_idle_per_s,net_err_delta_over_idle,net_capture_err_delta_over_idle,elapsed_seconds,results_dir" > "${history_file}"
fi
echo "${run_id},$(date -u --iso-8601=seconds),${RUN_TAG:-none},${backend},${DEVICE},${SIZE},${FPS},${DURATION},${expected_frames},${actual_frames},${drop_estimate},${source_seq_span_frames:-},${source_seq_gap_frames:-},${ffmpeg_counter_frames:-},${retire_capture_urb_events},${callbacks_suppressed_events},${callbacks_suppressed_total},${uvcvideo_events},${module_events},${idle_elapsed_seconds},${idle_err_delta_total},${idle_err_rate_per_s},${idle_capture_err_delta_total},${idle_capture_err_rate_per_s},${test_err_delta_total},${test_err_rate_per_s},${test_capture_err_delta_total},${test_capture_err_rate_per_s},${net_err_rate_over_idle_per_s},${net_capture_err_rate_over_idle_per_s},${net_err_delta_over_idle},${net_capture_err_delta_over_idle},${actual_seconds},${outdir}" >> "${history_file}"

cat "${summary_file}"
echo "logs=${run_log}"
