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
CONCURRENT_MODE="${CONCURRENT_MODE:-auto}"
SAVE_RAW="${SAVE_RAW:-0}"
SAVE_MKV="${SAVE_MKV:-0}"
SAVE_DIR="${SAVE_DIR:-}"

rate_per_sec() {
  awk -v d="${1}" -v s="${2}" 'BEGIN { if (s <= 0) printf "0.000"; else printf "%.3f", d / s }'
}

have_cmd() {
  command -v "${1}" >/dev/null 2>&1
}

module_diag_path() {
  local found
  found="$(find /sys/module -maxdepth 3 -type f -name diag_enable 2>/dev/null | head -n1 || true)"
  if [[ -n "${found}" ]]; then
    echo "${found}"
  else
    echo "/sys/module/${MODULE}/parameters/diag_enable"
  fi
}

module_srcversion_path() {
  echo "/sys/module/${MODULE}/srcversion"
}

diag_delta_from_snapshots() {
  local before_file="${1}"
  local after_file="${2}"
  local out_file="${3}"

  if [[ ! -s "${before_file}" || ! -s "${after_file}" ]]; then
    return 0
  fi

  awk '
    FNR == 1 { next }
    NR == FNR {
      ch = $1
      b_work[ch]=$2
      b_buf_processed[ch]=$5
      b_buf_done[ch]=$6
      b_buf_error[ch]=$7
      b_copy[ch]=$8
      b_scaler[ch]=$9
      b_novideo[ch]=$10
      b_fallback[ch]=$11
      next
    }
    FNR == 1 {
      print "ch delta_work_runs delta_buf_processed delta_buf_done delta_buf_error delta_copy_frames delta_scaler_frames delta_novideo_frames delta_miss_fallbacks"
      next
    }
    {
      ch = $1
      printf "%s %d %d %d %d %d %d %d %d\n",
        ch,
        ($2 - b_work[ch]),
        ($5 - b_buf_processed[ch]),
        ($6 - b_buf_done[ch]),
        ($7 - b_buf_error[ch]),
        ($8 - b_copy[ch]),
        ($9 - b_scaler[ch]),
        ($10 - b_novideo[ch]),
        ($11 - b_fallback[ch])
    }
  ' "${before_file}" "${after_file}" > "${out_file}" || true
}

run_preflight() {
  local preflight_file="${1}"
  local warnings_file="${2}"
  local module_name="${3}"
  local device="${4}"
  local diag_param_path="${5}"
  local diag_data_path="${6}"
  local srcversion_path="${7}"
  local holders_file="${8}"
  local mode_hint="${9}"
  local warning_count=0
  local module_loaded=0
  local device_exists=0
  local device_readable=0
  local modinfo_ok=0
  local runtime_srcversion_ok=0
  local srcversion_match=0
  local diag_param_readable=0
  local diag_data_readable=0
  local modinfo_path=""
  local modinfo_src=""
  local runtime_src=""
  local holder_lines=0

  : > "${preflight_file}"
  : > "${warnings_file}"

  if [[ -e "${device}" ]]; then
    device_exists=1
  fi
  if [[ -r "${device}" ]]; then
    device_readable=1
  fi

  if have_cmd lsmod && lsmod | awk '{print $1}' | grep -qx "${module_name}"; then
    module_loaded=1
  fi

  if have_cmd modinfo; then
    modinfo_path="$(modinfo -n "${module_name}" 2>/dev/null || true)"
    modinfo_src="$(modinfo "${module_name}" 2>/dev/null | awk -F': *' '/^srcversion/ {print $2; exit}')"
    if [[ -n "${modinfo_path}" ]]; then
      modinfo_ok=1
    fi
  fi

  if [[ -r "${srcversion_path}" ]]; then
    runtime_src="$(cat "${srcversion_path}" 2>/dev/null || true)"
    if [[ -n "${runtime_src}" ]]; then
      runtime_srcversion_ok=1
    fi
  fi

  if [[ -n "${modinfo_src}" && -n "${runtime_src}" && "${modinfo_src}" == "${runtime_src}" ]]; then
    srcversion_match=1
  fi

  if [[ -r "${diag_param_path}" ]]; then
    diag_param_readable=1
  fi
  if [[ -r "${diag_data_path}" ]]; then
    diag_data_readable=1
  fi
  if [[ -s "${holders_file}" ]]; then
    holder_lines="$(wc -l < "${holders_file}" | tr -d ' ')"
  fi

  if [[ "${module_loaded}" -ne 1 ]]; then
    echo "module_not_loaded:${module_name}" >> "${warnings_file}"
    warning_count=$((warning_count + 1))
  fi
  if [[ "${device_exists}" -ne 1 ]]; then
    echo "device_missing:${device}" >> "${warnings_file}"
    warning_count=$((warning_count + 1))
  fi
  if [[ "${device_readable}" -ne 1 ]]; then
    echo "device_not_readable:${device}" >> "${warnings_file}"
    warning_count=$((warning_count + 1))
  fi
  if [[ "${modinfo_ok}" -ne 1 || "${runtime_srcversion_ok}" -ne 1 ]]; then
    echo "module_identity_incomplete:modinfo_or_runtime_srcversion_missing" >> "${warnings_file}"
    warning_count=$((warning_count + 1))
  elif [[ "${srcversion_match}" -ne 1 ]]; then
    echo "module_identity_mismatch:modinfo_srcversion!=runtime_srcversion" >> "${warnings_file}"
    warning_count=$((warning_count + 1))
  fi
  if [[ "${diag_param_readable}" -ne 1 ]]; then
    echo "diag_param_unreadable:${diag_param_path}" >> "${warnings_file}"
    warning_count=$((warning_count + 1))
  fi
  if [[ "${diag_data_readable}" -ne 1 ]]; then
    echo "diag_debugfs_unreadable:${diag_data_path}" >> "${warnings_file}"
    warning_count=$((warning_count + 1))
  fi
  if [[ "${mode_hint}" == "solo" && "${holder_lines}" -gt 0 ]]; then
    echo "unexpected_holders_for_solo_mode:${holder_lines}" >> "${warnings_file}"
    warning_count=$((warning_count + 1))
  fi

  {
    echo "timestamp_utc=$(date -u --iso-8601=seconds)"
    echo "module=${module_name}"
    echo "module_loaded=${module_loaded}"
    echo "device=${device}"
    echo "device_exists=${device_exists}"
    echo "device_readable=${device_readable}"
    echo "modinfo_path=${modinfo_path:-unavailable}"
    echo "modinfo_srcversion=${modinfo_src:-unavailable}"
    echo "runtime_srcversion_path=${srcversion_path}"
    echo "runtime_srcversion=${runtime_src:-unavailable}"
    echo "srcversion_match=${srcversion_match}"
    echo "diag_param_path=${diag_param_path}"
    echo "diag_param_readable=${diag_param_readable}"
    echo "diag_data_path=${diag_data_path}"
    echo "diag_data_readable=${diag_data_readable}"
    echo "mode_hint=${mode_hint}"
    echo "holders_detected=${holder_lines}"
    echo "warnings_file=${warnings_file}"
    echo "warning_count=${warning_count}"
  } >> "${preflight_file}"

  echo "${warning_count}"
}

detect_device_holders() {
  local device="${1}"
  local pids_file="${2}"
  local details_file="${3}"

  : > "${pids_file}"
  : > "${details_file}"

  if command -v lsof >/dev/null 2>&1; then
    lsof -n -w "${device}" > "${details_file}" 2>&1 || true
    lsof -n -w -t "${device}" 2>/dev/null | sort -u > "${pids_file}" || true
  elif command -v fuser >/dev/null 2>&1; then
    fuser -v "${device}" > "${details_file}" 2>&1 || true
    fuser "${device}" 2>/dev/null | tr -cs '0-9\n' '\n' | sed '/^$/d' | sort -u > "${pids_file}" || true
  fi
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
  -c  Concurrent mode: auto|solo|shared (default: ${CONCURRENT_MODE})
  -R  Save tested stream as raw YUYV dump in run directory
  -M  Save tested stream as lossless MKV (FFV1) in run directory
  -O  Directory for saved media files (raw/mkv). Default: run directory

Environment:
  PRETEST_SECONDS  idle pw-top sampling duration before capture (default: DURATION)
  CONCURRENT_MODE  same as -c
  SAVE_RAW         0|1, same as -R
  SAVE_MKV         0|1, same as -M
  SAVE_DIR         media destination directory, same as -O
USAGE
}

while getopts ":d:t:s:r:o:g:c:RMO:h" opt; do
  case "${opt}" in
    d) DEVICE="${OPTARG}" ;;
    t) DURATION="${OPTARG}" ;;
    s) SIZE="${OPTARG}" ;;
    r) FPS="${OPTARG}" ;;
    o) OUTDIR_BASE="${OPTARG}" ;;
    g) RUN_TAG="${OPTARG}" ;;
    c) CONCURRENT_MODE="${OPTARG}" ;;
    R) SAVE_RAW=1 ;;
    M) SAVE_MKV=1 ;;
    O) SAVE_DIR="${OPTARG}" ;;
    h) usage; exit 0 ;;
    :) echo "Missing arg for -${OPTARG}" >&2; usage; exit 2 ;;
    \?) echo "Unknown option: -${OPTARG}" >&2; usage; exit 2 ;;
  esac
done

if [[ "${CONCURRENT_MODE}" != "auto" && "${CONCURRENT_MODE}" != "solo" && "${CONCURRENT_MODE}" != "shared" ]]; then
  echo "Invalid concurrent mode: ${CONCURRENT_MODE} (expected auto|solo|shared)" >&2
  exit 2
fi

if [[ -z "${PRETEST_SECONDS}" ]]; then
  PRETEST_SECONDS="${DURATION}"
fi

if [[ "${SAVE_RAW}" != "0" && "${SAVE_RAW}" != "1" ]]; then
  echo "Invalid SAVE_RAW=${SAVE_RAW} (expected 0 or 1)" >&2
  exit 2
fi
if [[ "${SAVE_MKV}" != "0" && "${SAVE_MKV}" != "1" ]]; then
  echo "Invalid SAVE_MKV=${SAVE_MKV} (expected 0 or 1)" >&2
  exit 2
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
save_dir_run="${outdir}"
raw_capture_file="${save_dir_run}/${run_id}-capture.yuyv"
mkv_capture_file="${save_dir_run}/${run_id}-capture.mkv"
history_file="${OUTDIR_BASE}/history-v4.csv"
diag_before_file="${outdir}/${run_id}-diag.before.txt"
diag_after_file="${outdir}/${run_id}-diag.after.txt"
diag_delta_file="${outdir}/${run_id}-diag.delta.txt"
diag_status_file="${outdir}/${run_id}-diag.status.txt"
preflight_file="${outdir}/${run_id}-preflight.txt"
preflight_warnings_file="${outdir}/${run_id}-preflight.warnings.txt"
holders_pre_pids_file="${outdir}/${run_id}-holders.pre.pids.txt"
holders_pre_details_file="${outdir}/${run_id}-holders.pre.txt"
diag_runtime_path="$(module_diag_path)"
srcversion_runtime_path="$(module_srcversion_path)"
diag_param_value=""
runtime_srcversion=""
modinfo_srcversion=""
diag_file_readable=0
diag_before_captured=0
diag_after_captured=0
detected_holder_count=0
detected_holder_pids=""
concurrent_client_mode="${CONCURRENT_MODE}"
preflight_warning_count=0
preflight_status="ok"

if [[ -n "${SAVE_DIR}" ]]; then
  save_dir_run="${SAVE_DIR}/${run_id}"
  mkdir -p "${save_dir_run}"
  raw_capture_file="${save_dir_run}/${run_id}-capture.yuyv"
  mkv_capture_file="${save_dir_run}/${run_id}-capture.mkv"
fi

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
  echo "save_raw=${SAVE_RAW}"
  echo "save_mkv=${SAVE_MKV}"
  echo "save_dir=${SAVE_DIR:-run_dir_default}"
  echo "save_dir_run=${save_dir_run}"
} > "${info_file}"

if command -v modinfo >/dev/null 2>&1; then
  modinfo "${MODULE}" > "${outdir}/${run_id}-modinfo.txt" 2>/dev/null || true
  modinfo_srcversion="$(modinfo "${MODULE}" 2>/dev/null | awk -F': *' '/^srcversion/ {print $2; exit}')"
  modpath="$(modinfo -n "${MODULE}" 2>/dev/null || true)"
  if [[ -n "${modpath}" && -f "${modpath}" ]]; then
    sha256sum "${modpath}" > "${outdir}/${run_id}-module.sha256"
  fi
fi

detect_device_holders "${DEVICE}" "${holders_pre_pids_file}" "${holders_pre_details_file}"
if [[ -s "${holders_pre_pids_file}" ]]; then
  detected_holder_count="$(wc -l < "${holders_pre_pids_file}" | tr -d ' ')"
  detected_holder_pids="$(paste -sd, "${holders_pre_pids_file}")"
else
  detected_holder_count=0
  detected_holder_pids=""
fi
if [[ "${CONCURRENT_MODE}" == "auto" ]]; then
  if [[ "${detected_holder_count}" -gt 0 ]]; then
    concurrent_client_mode="shared"
  else
    concurrent_client_mode="solo"
  fi
fi

if [[ -r "${srcversion_runtime_path}" ]]; then
  runtime_srcversion="$(cat "${srcversion_runtime_path}" 2>/dev/null || true)"
fi
if [[ -r "${diag_runtime_path}" ]]; then
  diag_param_value="$(cat "${diag_runtime_path}" 2>/dev/null || true)"
fi

if [[ -r "${DIAG_FILE}" ]]; then
  diag_file_readable=1
fi
if [[ -r "${DIAG_FILE}" ]]; then
  cat "${DIAG_FILE}" > "${diag_before_file}" 2>/dev/null || true
  if [[ -s "${diag_before_file}" ]]; then
    diag_before_captured=1
  fi
fi

preflight_warning_count="$(run_preflight "${preflight_file}" "${preflight_warnings_file}" "${MODULE}" "${DEVICE}" "${diag_runtime_path}" "${DIAG_FILE}" "${srcversion_runtime_path}" "${holders_pre_pids_file}" "${concurrent_client_mode}")"
if [[ "${preflight_warning_count}" -gt 0 ]]; then
  preflight_status="warn"
fi

{
  echo "diag_file=${DIAG_FILE}"
  echo "diag_file_readable=${diag_file_readable}"
  echo "concurrent_client_mode=${concurrent_client_mode}"
  echo "detected_holder_count=${detected_holder_count}"
  echo "detected_holder_pids=${detected_holder_pids:-none}"
  echo "holders_pre_file=${holders_pre_details_file}"
  echo "diag_param_path=${diag_runtime_path}"
  echo "diag_param_value=${diag_param_value:-unavailable}"
  echo "runtime_srcversion_path=${srcversion_runtime_path}"
  echo "runtime_srcversion=${runtime_srcversion:-unavailable}"
  echo "modinfo_srcversion=${modinfo_srcversion:-unavailable}"
  echo "diag_before_captured=${diag_before_captured}"
  echo "preflight_file=${preflight_file}"
  echo "preflight_warnings_file=${preflight_warnings_file}"
  echo "preflight_status=${preflight_status}"
  echo "preflight_warning_count=${preflight_warning_count}"
} > "${diag_status_file}"

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

if [[ "${SAVE_RAW}" == "1" || "${SAVE_MKV}" == "1" ]]; then
  if command -v ffmpeg >/dev/null 2>&1; then
    backend="ffmpeg"
    ffmpeg_args=(
      -hide_banner -nostdin -loglevel info -stats
      -f v4l2
      -input_format yuyv422
      -framerate "${FPS}"
      -video_size "${SIZE}"
      -t "${DURATION}"
      -i "${DEVICE}"
      -an
    )
    if [[ "${SAVE_RAW}" == "1" ]]; then
      ffmpeg_args+=( -map 0:v:0 -c:v rawvideo -pix_fmt yuyv422 -f rawvideo "${raw_capture_file}" )
    fi
    if [[ "${SAVE_MKV}" == "1" ]]; then
      ffmpeg_args+=( -map 0:v:0 -c:v ffv1 -level 3 -g 1 -threads 0 -f matroska "${mkv_capture_file}" )
    fi
    # Keep a sink equivalent to /dev/null benchmarking.
    ffmpeg_args+=( -map 0:v:0 -c:v rawvideo -pix_fmt yuyv422 -f null - )
    ffmpeg "${ffmpeg_args[@]}" > "${run_log}" 2>&1 || true
  else
    backend="none"
    echo "SAVE_RAW/SAVE_MKV requested but ffmpeg not available." > "${run_log}"
  fi
elif command -v v4l2-ctl >/dev/null 2>&1; then
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

# Keep post-processing resilient: optional collectors/parsers should not
# prevent summary/history output.
set +e

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
  if [[ -s "${diag_after_file}" ]]; then
    diag_after_captured=1
  fi
fi
if [[ "${diag_before_captured}" -eq 1 && "${diag_after_captured}" -eq 1 ]]; then
  diag_delta_from_snapshots "${diag_before_file}" "${diag_after_file}" "${diag_delta_file}"
fi
{
  echo "diag_after_captured=${diag_after_captured}"
} >> "${diag_status_file}"
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
  if [[ -s "${outdir}/${run_id}-pw-top-test-metrics.txt" ]]; then
    # shellcheck disable=SC1090
    source "${outdir}/${run_id}-pw-top-test-metrics.txt"
  fi
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
  echo "concurrent_client_mode=${concurrent_client_mode}"
  echo "detected_holder_count=${detected_holder_count}"
  echo "detected_holder_pids=${detected_holder_pids:-none}"
  echo "diag_file=${DIAG_FILE}"
  echo "diag_file_readable=${diag_file_readable}"
  echo "diag_param_path=${diag_runtime_path}"
  echo "diag_param_value=${diag_param_value:-unavailable}"
  echo "runtime_srcversion=${runtime_srcversion:-unavailable}"
  echo "modinfo_srcversion=${modinfo_srcversion:-unavailable}"
  echo "diag_before_captured=${diag_before_captured}"
  echo "diag_after_captured=${diag_after_captured}"
  echo "save_raw=${SAVE_RAW}"
  echo "save_mkv=${SAVE_MKV}"
  echo "save_dir=${SAVE_DIR:-run_dir_default}"
  echo "save_dir_run=${save_dir_run}"
  echo "raw_capture_file=${raw_capture_file}"
  echo "mkv_capture_file=${mkv_capture_file}"
  echo "preflight_status=${preflight_status}"
  echo "preflight_warning_count=${preflight_warning_count}"
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
  echo "run_id,timestamp_utc,run_tag,backend,device,size,fps,duration,expected_frames,actual_frames,estimated_drop_vs_target_frames,source_seq_span_frames,source_seq_gap_frames,ffmpeg_counter_frames,retire_capture_urb_events,callbacks_suppressed_events,callbacks_suppressed_total,uvcvideo_events,module_events,preflight_status,preflight_warning_count,idle_elapsed_seconds,idle_err_delta_total,idle_err_rate_per_s,idle_capture_err_delta_total,idle_capture_err_rate_per_s,test_err_delta_total,test_err_rate_per_s,test_capture_err_delta_total,test_capture_err_rate_per_s,net_err_rate_over_idle_per_s,net_capture_err_rate_over_idle_per_s,net_err_delta_over_idle,net_capture_err_delta_over_idle,elapsed_seconds,results_dir" > "${history_file}"
fi
echo "${run_id},$(date -u --iso-8601=seconds),${RUN_TAG:-none},${backend},${DEVICE},${SIZE},${FPS},${DURATION},${expected_frames},${actual_frames},${drop_estimate},${source_seq_span_frames:-},${source_seq_gap_frames:-},${ffmpeg_counter_frames:-},${retire_capture_urb_events},${callbacks_suppressed_events},${callbacks_suppressed_total},${uvcvideo_events},${module_events},${preflight_status},${preflight_warning_count},${idle_elapsed_seconds},${idle_err_delta_total},${idle_err_rate_per_s},${idle_capture_err_delta_total},${idle_capture_err_rate_per_s},${test_err_delta_total},${test_err_rate_per_s},${test_capture_err_delta_total},${test_capture_err_rate_per_s},${net_err_rate_over_idle_per_s},${net_capture_err_rate_over_idle_per_s},${net_err_delta_over_idle},${net_capture_err_delta_over_idle},${actual_seconds},${outdir}" >> "${history_file}"

cat "${summary_file}"
echo "logs=${run_log}"
