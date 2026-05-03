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
AUDIO_DIAG_FILE="${AUDIO_DIAG_FILE:-/sys/kernel/debug/hwsuhdx1/audio_diag}"
CONCURRENT_MODE="${CONCURRENT_MODE:-auto}"
SAVE_RAW="${SAVE_RAW:-0}"
SAVE_MKV="${SAVE_MKV:-0}"
SAVE_DIR="${SAVE_DIR:-}"
FORCE_HEAVY_SAVE="${FORCE_HEAVY_SAVE:-0}"
AUDIO_TEST="${AUDIO_TEST:-0}"
AUDIO_PCM="${AUDIO_PCM:-auto}"
AUDIO_RATE="${AUDIO_RATE:-48000}"
AUDIO_CHANNELS="${AUDIO_CHANNELS:-2}"
AUDIO_FORMAT="${AUDIO_FORMAT:-S16_LE}"

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

audio_diag_delta_from_snapshots() {
  local before_file="${1}"
  local after_file="${2}"
  local out_file="${3}"

  if [[ ! -s "${before_file}" || ! -s "${after_file}" ]]; then
    return 0
  fi

  awk '
    function before_value(ch, name,    key) {
      key = ch SUBSEP name
      if (key in before_values)
        return before_values[key]
      return 0
    }
    function current_value(name,    idx) {
      idx = after_idx[name]
      if (idx > 0)
        return $(idx)
      return 0
    }
    FNR == 1 && NR == FNR {
      for (i = 1; i <= NF; i++)
        before_names[i] = $i
      next
    }
    NR == FNR {
      ch = $1
      for (i = 2; i <= NF; i++)
        before_values[ch SUBSEP before_names[i]] = $i
      next
    }
    FNR == 1 {
      delete after_idx
      for (i = 1; i <= NF; i++) {
        after_idx[$i] = i
        after_names[i] = $i
      }
      printf "ch"
      for (i = 2; i <= NF; i++)
        printf " delta_%s", after_names[i]
      printf "\n"
      next
    }
    {
      ch = $1
      printf "%s", ch
      for (i = 2; i <= NF; i++)
        printf " %d", (current_value(after_names[i]) - before_value(ch, after_names[i]))
      printf "\n"
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

parse_v4l2_delta_metrics() {
  local logfile="${1}"
  local fps="${2}"
  local out_kv="${3}"
  local delta_file="${out_kv}.deltas"
  local n=0
  local avg=0
  local p95=0
  local p99=0
  local max=0
  local over_2x=0
  local over_3x=0
  local target_ms=0
  local status="na"
  local idx95=0
  local idx99=0

  : > "${out_kv}"

  awk '
    {
      for (i = 1; i <= NF; i++) {
        if ($i == "delta:" && (i + 1) <= NF) {
          v = $(i + 1)
          gsub(/[^0-9.]/, "", v)
          if (v != "")
            print v
        }
      }
    }
  ' "${logfile}" > "${delta_file}" 2>/dev/null || true

  if [[ -s "${delta_file}" ]]; then
    n="$(wc -l < "${delta_file}" | tr -d ' ')"
  fi

  if [[ "${n}" -gt 0 ]]; then
    avg="$(awk '{s+=$1} END { if (NR>0) printf "%.3f", s/NR; else printf "0.000" }' "${delta_file}")"
    max="$(sort -n "${delta_file}" | tail -n1)"
    target_ms="$(awk -v f="${fps}" 'BEGIN { if (f > 0) printf "%.3f", 1000.0/f; else printf "0.000" }')"

    idx95="$(awk -v n="${n}" 'BEGIN { v = int((n*95 + 99)/100); if (v < 1) v = 1; print v }')"
    idx99="$(awk -v n="${n}" 'BEGIN { v = int((n*99 + 99)/100); if (v < 1) v = 1; print v }')"
    p95="$(sort -n "${delta_file}" | awk -v idx="${idx95}" 'NR==idx {printf "%.3f", $1; exit}')"
    p99="$(sort -n "${delta_file}" | awk -v idx="${idx99}" 'NR==idx {printf "%.3f", $1; exit}')"

    over_2x="$(awk -v t="${target_ms}" 'BEGIN{c=0} {if ($1 > (2.0*t)) c++} END{print c+0}' "${delta_file}")"
    over_3x="$(awk -v t="${target_ms}" 'BEGIN{c=0} {if ($1 > (3.0*t)) c++} END{print c+0}' "${delta_file}")"

    status="$(awk -v p95="${p95}" -v p99="${p99}" -v m="${max}" -v t="${target_ms}" '
      BEGIN {
        if (t <= 0) { print "na"; exit }
        if (p99 > (2.5*t) || m > (5.0*t)) { print "fail"; exit }
        if (p95 > (1.5*t) || p99 > (2.0*t)) { print "warn"; exit }
        print "ok"
      }')"
  fi

  {
    echo "pacing_samples=${n}"
    echo "target_frame_interval_ms=${target_ms}"
    echo "frame_delta_ms_avg=${avg}"
    echo "frame_delta_ms_p95=${p95}"
    echo "frame_delta_ms_p99=${p99}"
    echo "frame_delta_ms_max=${max}"
    echo "frame_delta_over_2x_target_events=${over_2x}"
    echo "frame_delta_over_3x_target_events=${over_3x}"
    echo "pacing_status=${status}"
  } > "${out_kv}"
}

detect_audio_pcm() {
  local pcm=""
  if ! have_cmd arecord; then
    echo ""
    return 0
  fi

  pcm="$(arecord -l 2>/dev/null | awk '
    BEGIN { IGNORECASE=1 }
    /^card [0-9]+:/ {
      card=$2
      sub(":", "", card)
      if ($0 ~ /hws|uhd|capture|huhdvideo|hwsuhdx1capture/) {
        printf "hw:%s,0\n", card
        exit
      }
    }
  ')"
  echo "${pcm}"
}

detect_hws_pw_target() {
  local id
  local name
  local target=""

  if ! have_cmd wpctl; then
    echo ""
    return 0
  fi

  while read -r id name; do
    if [[ -z "${id}" || -z "${name}" ]]; then
      continue
    fi
    if [[ "${name}" =~ alsa_input\.pci-0000_0f_00\.0\. ]]; then
      target="${name}"
      break
    fi
  done < <(wpctl status -n 2>/dev/null | sed -n 's/.* \([0-9]\+\)\. \(alsa_input\.[^ ]*\) .*/\1 \2/p')

  echo "${target}"
}

pw_format_from_alsa() {
  case "${1}" in
    S16_LE) echo "s16" ;;
    S24_LE) echo "s24" ;;
    S24_3LE) echo "s24_32" ;;
    S32_LE) echo "s32" ;;
    FLOAT_LE) echo "f32" ;;
    FLOAT64_LE) echo "f64" ;;
    *)
      # pw-record rejects unknown format tokens. Default to s16.
      echo "s16"
      ;;
  esac
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
  -O  Directory for saved media files (video/audio). Default: run directory
  -F  Force heavy save mode for long/high-res runs (disabled by default)
  -A  Enable audio probe during capture (drives audio_diag counters + saves WAV/RAW)
  -P  ALSA PCM for probe; used by arecord fallback (default: auto)
  -Q  Audio sample rate for probe (default: ${AUDIO_RATE})
  -N  Audio channels for probe (default: ${AUDIO_CHANNELS})

Environment:
  PRETEST_SECONDS  idle pw-top sampling duration before capture (default: DURATION)
  CONCURRENT_MODE  same as -c
  SAVE_RAW         0|1, same as -R
  SAVE_MKV         0|1, same as -M
  SAVE_DIR         media destination directory, same as -O
  FORCE_HEAVY_SAVE 0|1, same as -F
  AUDIO_TEST       0|1, same as -A
  AUDIO_PCM        ALSA PCM string or "auto", same as -P
  AUDIO_RATE       sample rate for audio probe, same as -Q
  AUDIO_CHANNELS   channel count for audio probe, same as -N
  AUDIO_FORMAT     format hint (ALSA style, default: S16_LE)
USAGE
}

while getopts ":d:t:s:r:o:g:c:RMO:FAP:Q:N:h" opt; do
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
    F) FORCE_HEAVY_SAVE=1 ;;
    A) AUDIO_TEST=1 ;;
    P) AUDIO_PCM="${OPTARG}" ;;
    Q) AUDIO_RATE="${OPTARG}" ;;
    N) AUDIO_CHANNELS="${OPTARG}" ;;
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
if [[ "${FORCE_HEAVY_SAVE}" != "0" && "${FORCE_HEAVY_SAVE}" != "1" ]]; then
  echo "Invalid FORCE_HEAVY_SAVE=${FORCE_HEAVY_SAVE} (expected 0 or 1)" >&2
  exit 2
fi
if [[ "${AUDIO_TEST}" != "0" && "${AUDIO_TEST}" != "1" ]]; then
  echo "Invalid AUDIO_TEST=${AUDIO_TEST} (expected 0 or 1)" >&2
  exit 2
fi
if ! [[ "${AUDIO_RATE}" =~ ^[0-9]+$ ]] || [[ "${AUDIO_RATE}" -le 0 ]]; then
  echo "Invalid AUDIO_RATE=${AUDIO_RATE} (expected positive integer)" >&2
  exit 2
fi
if ! [[ "${AUDIO_CHANNELS}" =~ ^[0-9]+$ ]] || [[ "${AUDIO_CHANNELS}" -le 0 ]]; then
  echo "Invalid AUDIO_CHANNELS=${AUDIO_CHANNELS} (expected positive integer)" >&2
  exit 2
fi

save_raw_requested="${SAVE_RAW}"
save_mkv_requested="${SAVE_MKV}"
save_guard_applied=0
save_guard_reason=""
size_w="${SIZE%x*}"
size_h="${SIZE#*x}"
if [[ "${size_w}" =~ ^[0-9]+$ && "${size_h}" =~ ^[0-9]+$ ]]; then
  pixels=$((size_w * size_h))
else
  pixels=0
fi
if [[ "${FORCE_HEAVY_SAVE}" == "0" && ( "${SAVE_RAW}" == "1" || "${SAVE_MKV}" == "1" ) ]]; then
  # Prevent accidental benchmark invalidation from heavy write/encode paths.
  if (( DURATION > 90 )); then
    SAVE_RAW=0
    SAVE_MKV=0
    save_guard_applied=1
    save_guard_reason="duration_gt_90"
  elif (( pixels >= 3840 * 2160 && DURATION > 30 )); then
    SAVE_RAW=0
    SAVE_MKV=0
    save_guard_applied=1
    save_guard_reason="uhd_duration_gt_30"
  fi
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
av_mkv_capture_file="${save_dir_run}/${run_id}-capture-av.mkv"
audio_wav_file="${save_dir_run}/${run_id}-audio.wav"
audio_raw_file="${save_dir_run}/${run_id}-audio.raw"
history_file="${OUTDIR_BASE}/history-v4.csv"
diag_before_file="${outdir}/${run_id}-diag.before.txt"
diag_after_file="${outdir}/${run_id}-diag.after.txt"
diag_delta_file="${outdir}/${run_id}-diag.delta.txt"
audio_diag_before_file="${outdir}/${run_id}-audio-diag.before.txt"
audio_diag_after_file="${outdir}/${run_id}-audio-diag.after.txt"
audio_diag_delta_file="${outdir}/${run_id}-audio-diag.delta.txt"
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
audio_diag_file_readable=0
audio_diag_before_captured=0
audio_diag_after_captured=0
detected_holder_count=0
detected_holder_pids=""
concurrent_client_mode="${CONCURRENT_MODE}"
preflight_warning_count=0
preflight_status="ok"
audio_log="${outdir}/${run_id}-audio.log"
audio_test_started=0
audio_backend="none"
audio_pcm_resolved=""
audio_pid=""
audio_exit_status=""
audio_wav_saved=0
audio_raw_saved=0
av_mkv_saved=0

if [[ -n "${SAVE_DIR}" ]]; then
  save_dir_run="${SAVE_DIR}/${run_id}"
  mkdir -p "${save_dir_run}"
  raw_capture_file="${save_dir_run}/${run_id}-capture.yuyv"
  mkv_capture_file="${save_dir_run}/${run_id}-capture.mkv"
  av_mkv_capture_file="${save_dir_run}/${run_id}-capture-av.mkv"
  audio_wav_file="${save_dir_run}/${run_id}-audio.wav"
  audio_raw_file="${save_dir_run}/${run_id}-audio.raw"
fi

if [[ -f "${history_file}" ]]; then
  if ! head -n1 "${history_file}" | grep -q "pacing_status"; then
    history_file="${OUTDIR_BASE}/history-v5.csv"
  fi
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
  echo "save_raw_requested=${save_raw_requested}"
  echo "save_mkv_requested=${save_mkv_requested}"
  echo "save_raw=${SAVE_RAW}"
  echo "save_mkv=${SAVE_MKV}"
  echo "force_heavy_save=${FORCE_HEAVY_SAVE}"
  echo "save_guard_applied=${save_guard_applied}"
  echo "save_guard_reason=${save_guard_reason:-none}"
  echo "save_dir=${SAVE_DIR:-run_dir_default}"
  echo "save_dir_run=${save_dir_run}"
  echo "audio_test_requested=${AUDIO_TEST}"
  echo "audio_pcm_requested=${AUDIO_PCM}"
  echo "audio_rate=${AUDIO_RATE}"
  echo "audio_channels=${AUDIO_CHANNELS}"
  echo "audio_format=${AUDIO_FORMAT}"
} > "${info_file}"

if [[ "${save_guard_applied}" -eq 1 ]]; then
  echo "bench: disabled -R/-M due to safeguard (${save_guard_reason}); use -F to force heavy save mode" | tee -a "${run_log}" >&2
fi

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
if [[ -r "${AUDIO_DIAG_FILE}" ]]; then
  audio_diag_file_readable=1
fi
if [[ -r "${DIAG_FILE}" ]]; then
  cat "${DIAG_FILE}" > "${diag_before_file}" 2>/dev/null || true
  if [[ -s "${diag_before_file}" ]]; then
    diag_before_captured=1
  fi
fi
if [[ -r "${AUDIO_DIAG_FILE}" ]]; then
  cat "${AUDIO_DIAG_FILE}" > "${audio_diag_before_file}" 2>/dev/null || true
  if [[ -s "${audio_diag_before_file}" ]]; then
    audio_diag_before_captured=1
  fi
fi

preflight_warning_count="$(run_preflight "${preflight_file}" "${preflight_warnings_file}" "${MODULE}" "${DEVICE}" "${diag_runtime_path}" "${DIAG_FILE}" "${srcversion_runtime_path}" "${holders_pre_pids_file}" "${concurrent_client_mode}")"
if [[ "${preflight_warning_count}" -gt 0 ]]; then
  preflight_status="warn"
fi

{
  echo "diag_file=${DIAG_FILE}"
  echo "diag_file_readable=${diag_file_readable}"
  echo "audio_diag_file=${AUDIO_DIAG_FILE}"
  echo "audio_diag_file_readable=${audio_diag_file_readable}"
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
  echo "audio_diag_before_captured=${audio_diag_before_captured}"
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

if [[ "${AUDIO_TEST}" == "1" ]]; then
  audio_probe_file="${audio_wav_file}"
  if have_cmd pw-record; then
    local_pw_target=""
    local_pw_format="$(pw_format_from_alsa "${AUDIO_FORMAT}")"
    if [[ "${AUDIO_PCM}" == "auto" ]]; then
      local_pw_target="$(detect_hws_pw_target)"
      audio_pcm_resolved="${local_pw_target:-auto-default}"
    else
      # Keep old meaning: -P is ALSA PCM. With pw-record we can only target nodes,
      # so user-provided -P is treated as a best-effort hint and ignored here.
      local_pw_target="$(detect_hws_pw_target)"
      audio_pcm_resolved="${AUDIO_PCM}"
    fi
    audio_backend="pw-record"
    if have_cmd timeout; then
      if [[ -n "${local_pw_target}" ]]; then
        timeout --signal=INT "${DURATION}s" pw-record --target "${local_pw_target}" --rate "${AUDIO_RATE}" --channels "${AUDIO_CHANNELS}" --format "${local_pw_format}" "${audio_probe_file}" > "${audio_log}" 2>&1 &
      else
        timeout --signal=INT "${DURATION}s" pw-record --rate "${AUDIO_RATE}" --channels "${AUDIO_CHANNELS}" --format "${local_pw_format}" "${audio_probe_file}" > "${audio_log}" 2>&1 &
      fi
      audio_pid=$!
      audio_test_started=1
    else
      echo "audio_probe: timeout command not available; skipping pw-record probe" > "${audio_log}"
    fi
  elif have_cmd arecord; then
    if [[ "${AUDIO_PCM}" == "auto" ]]; then
      audio_pcm_resolved="$(detect_audio_pcm)"
    else
      audio_pcm_resolved="${AUDIO_PCM}"
    fi
    if [[ -n "${audio_pcm_resolved}" ]]; then
      audio_backend="arecord"
      arecord -D "${audio_pcm_resolved}" -f "${AUDIO_FORMAT}" -r "${AUDIO_RATE}" -c "${AUDIO_CHANNELS}" -d "${DURATION}" -t wav "${audio_probe_file}" > "${audio_log}" 2>&1 &
      audio_pid=$!
      audio_test_started=1
    else
      echo "audio_probe: no matching ALSA PCM found" > "${audio_log}"
    fi
  else
    echo "audio_probe: arecord not available" > "${audio_log}"
  fi
fi

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
if [[ -n "${audio_pid}" ]]; then
  set +e
  wait "${audio_pid}" 2>/dev/null
  audio_exit_status="$?"
  set -e
  # timeout exits with 124 when it ends the probe at requested duration.
  if [[ "${audio_backend}" == "pw-record" && "${audio_exit_status}" == "124" ]]; then
    audio_exit_status="0"
  fi
fi
if [[ -s "${audio_wav_file}" ]]; then
  audio_wav_saved=1
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
pacing_samples=0
target_frame_interval_ms="0.000"
frame_delta_ms_avg="0.000"
frame_delta_ms_p95="0.000"
frame_delta_ms_p99="0.000"
frame_delta_ms_max="0.000"
frame_delta_over_2x_target_events=0
frame_delta_over_3x_target_events=0
pacing_status="na"

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

if [[ "${backend}" == "v4l2-ctl-seq" && -s "${run_log}" ]]; then
  parse_v4l2_delta_metrics "${run_log}" "${FPS}" "${outdir}/${run_id}-pacing-metrics.txt"
  if [[ -s "${outdir}/${run_id}-pacing-metrics.txt" ]]; then
    # shellcheck disable=SC1090
    source "${outdir}/${run_id}-pacing-metrics.txt"
  fi
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
if [[ -r "${AUDIO_DIAG_FILE}" ]]; then
  cat "${AUDIO_DIAG_FILE}" > "${audio_diag_after_file}" 2>/dev/null || true
  if [[ -s "${audio_diag_after_file}" ]]; then
    audio_diag_after_captured=1
  fi
fi
if [[ "${diag_before_captured}" -eq 1 && "${diag_after_captured}" -eq 1 ]]; then
  diag_delta_from_snapshots "${diag_before_file}" "${diag_after_file}" "${diag_delta_file}"
fi
if [[ "${audio_diag_before_captured}" -eq 1 && "${audio_diag_after_captured}" -eq 1 ]]; then
  audio_diag_delta_from_snapshots "${audio_diag_before_file}" "${audio_diag_after_file}" "${audio_diag_delta_file}"
fi
{
  echo "diag_after_captured=${diag_after_captured}"
  echo "audio_diag_after_captured=${audio_diag_after_captured}"
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

# Derive additional audio artifacts from the probe WAV when available.
if [[ "${audio_wav_saved}" == "1" ]] && command -v ffmpeg >/dev/null 2>&1; then
  ffmpeg -hide_banner -nostdin -loglevel error -y \
    -i "${audio_wav_file}" -f s16le -acodec pcm_s16le "${audio_raw_file}" \
    > /dev/null 2>&1 || true
  if [[ -s "${audio_raw_file}" ]]; then
    audio_raw_saved=1
  fi
fi

# If both video MKV and audio WAV exist, generate a muxed A/V MKV.
if [[ "${SAVE_MKV}" == "1" && "${audio_wav_saved}" == "1" && -s "${mkv_capture_file}" ]] && command -v ffmpeg >/dev/null 2>&1; then
  ffmpeg -hide_banner -nostdin -loglevel error -y \
    -i "${mkv_capture_file}" -i "${audio_wav_file}" \
    -map 0:v:0 -map 1:a:0 -c:v copy -c:a pcm_s16le -shortest \
    "${av_mkv_capture_file}" > /dev/null 2>&1 || true
  if [[ -s "${av_mkv_capture_file}" ]]; then
    av_mkv_saved=1
  fi
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
  echo "pacing_samples=${pacing_samples}"
  echo "target_frame_interval_ms=${target_frame_interval_ms}"
  echo "frame_delta_ms_avg=${frame_delta_ms_avg}"
  echo "frame_delta_ms_p95=${frame_delta_ms_p95}"
  echo "frame_delta_ms_p99=${frame_delta_ms_p99}"
  echo "frame_delta_ms_max=${frame_delta_ms_max}"
  echo "frame_delta_over_2x_target_events=${frame_delta_over_2x_target_events}"
  echo "frame_delta_over_3x_target_events=${frame_delta_over_3x_target_events}"
  echo "pacing_status=${pacing_status}"
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
  echo "audio_diag_file=${AUDIO_DIAG_FILE}"
  echo "audio_diag_file_readable=${audio_diag_file_readable}"
  echo "diag_param_path=${diag_runtime_path}"
  echo "diag_param_value=${diag_param_value:-unavailable}"
  echo "runtime_srcversion=${runtime_srcversion:-unavailable}"
  echo "modinfo_srcversion=${modinfo_srcversion:-unavailable}"
  echo "diag_before_captured=${diag_before_captured}"
  echo "diag_after_captured=${diag_after_captured}"
  echo "audio_diag_before_captured=${audio_diag_before_captured}"
  echo "audio_diag_after_captured=${audio_diag_after_captured}"
  echo "save_raw=${SAVE_RAW}"
  echo "save_mkv=${SAVE_MKV}"
  echo "save_raw_requested=${save_raw_requested}"
  echo "save_mkv_requested=${save_mkv_requested}"
  echo "force_heavy_save=${FORCE_HEAVY_SAVE}"
  echo "save_guard_applied=${save_guard_applied}"
  echo "save_guard_reason=${save_guard_reason:-none}"
  echo "save_dir=${SAVE_DIR:-run_dir_default}"
  echo "save_dir_run=${save_dir_run}"
  echo "audio_test_requested=${AUDIO_TEST}"
  echo "audio_test_started=${audio_test_started}"
  echo "audio_backend=${audio_backend}"
  echo "audio_pcm_requested=${AUDIO_PCM}"
  echo "audio_pcm_resolved=${audio_pcm_resolved:-none}"
  echo "audio_rate=${AUDIO_RATE}"
  echo "audio_channels=${AUDIO_CHANNELS}"
  echo "audio_format=${AUDIO_FORMAT}"
  echo "audio_log=${audio_log}"
  echo "audio_exit_status=${audio_exit_status:-na}"
  echo "audio_wav_file=${audio_wav_file}"
  echo "audio_wav_saved=${audio_wav_saved}"
  echo "audio_raw_file=${audio_raw_file}"
  echo "audio_raw_saved=${audio_raw_saved}"
  echo "raw_capture_file=${raw_capture_file}"
  echo "mkv_capture_file=${mkv_capture_file}"
  echo "av_mkv_capture_file=${av_mkv_capture_file}"
  echo "av_mkv_saved=${av_mkv_saved}"
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
  echo "run_id,timestamp_utc,run_tag,backend,device,size,fps,duration,expected_frames,actual_frames,estimated_drop_vs_target_frames,source_seq_span_frames,source_seq_gap_frames,ffmpeg_counter_frames,pacing_samples,target_frame_interval_ms,frame_delta_ms_avg,frame_delta_ms_p95,frame_delta_ms_p99,frame_delta_ms_max,frame_delta_over_2x_target_events,frame_delta_over_3x_target_events,pacing_status,retire_capture_urb_events,callbacks_suppressed_events,callbacks_suppressed_total,uvcvideo_events,module_events,preflight_status,preflight_warning_count,idle_elapsed_seconds,idle_err_delta_total,idle_err_rate_per_s,idle_capture_err_delta_total,idle_capture_err_rate_per_s,test_err_delta_total,test_err_rate_per_s,test_capture_err_delta_total,test_capture_err_rate_per_s,net_err_rate_over_idle_per_s,net_capture_err_rate_over_idle_per_s,net_err_delta_over_idle,net_capture_err_delta_over_idle,elapsed_seconds,results_dir" > "${history_file}"
fi
echo "${run_id},$(date -u --iso-8601=seconds),${RUN_TAG:-none},${backend},${DEVICE},${SIZE},${FPS},${DURATION},${expected_frames},${actual_frames},${drop_estimate},${source_seq_span_frames:-},${source_seq_gap_frames:-},${ffmpeg_counter_frames:-},${pacing_samples},${target_frame_interval_ms},${frame_delta_ms_avg},${frame_delta_ms_p95},${frame_delta_ms_p99},${frame_delta_ms_max},${frame_delta_over_2x_target_events},${frame_delta_over_3x_target_events},${pacing_status},${retire_capture_urb_events},${callbacks_suppressed_events},${callbacks_suppressed_total},${uvcvideo_events},${module_events},${preflight_status},${preflight_warning_count},${idle_elapsed_seconds},${idle_err_delta_total},${idle_err_rate_per_s},${idle_capture_err_delta_total},${idle_capture_err_rate_per_s},${test_err_delta_total},${test_err_rate_per_s},${test_capture_err_delta_total},${test_capture_err_rate_per_s},${net_err_rate_over_idle_per_s},${net_capture_err_rate_over_idle_per_s},${net_err_delta_over_idle},${net_capture_err_delta_over_idle},${actual_seconds},${outdir}" >> "${history_file}"

cat "${summary_file}"
echo "logs=${run_log}"
