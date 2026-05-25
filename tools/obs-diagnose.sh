#!/usr/bin/env bash
set -euo pipefail

MODULE="${MODULE:-HwsUHDX1Capture}"
OBS_BIN="${OBS_BIN:-obs}"
SAMPLE_SEC="${SAMPLE_SEC:-2}"
RUN_TAG="${RUN_TAG:-}"
OUT_BASE="${OUT_BASE:-$HOME/obs-diag-results}"
CAPTURE_FULL_SYSTEM_JOURNAL="${CAPTURE_FULL_SYSTEM_JOURNAL:-0}"
PRIV_HELPER="${PRIV_HELPER:-/usr/local/sbin/hws-obs-diag-priv}"
DIAG_AUTO_TOGGLE="${DIAG_AUTO_TOGGLE:-1}"
VIDEO_DEVICE="${VIDEO_DEVICE:-/dev/video0}"
CAPTURE_PIPEWIRE="${CAPTURE_PIPEWIRE:-1}"
PW_PROFILER_ENABLE="${PW_PROFILER_ENABLE:-1}"

usage() {
  cat <<'USAGE'
Usage: tools/obs-diagnose.sh [options] [-- OBS_ARGS...]

Run OBS and capture long-session diagnostics for driver/performance analysis.

Options:
  -g <tag>         Optional tag for run id.
  -o <dir>         Output base directory (default: ~/obs-diag-results).
  -s <seconds>     Sample interval for periodic snapshots (default: 2).
  -b <obs-bin>     OBS executable (default: obs).
  -h               Show help.

Environment:
  MODULE=HwsUHDX1Capture
  OBS_BIN=obs
  SAMPLE_SEC=2
  OUT_BASE=~/obs-diag-results
  CAPTURE_FULL_SYSTEM_JOURNAL=0|1
  PRIV_HELPER=/usr/local/sbin/hws-obs-diag-priv
  DIAG_AUTO_TOGGLE=0|1
  VIDEO_DEVICE=/dev/video0
  CAPTURE_PIPEWIRE=0|1
  PW_PROFILER_ENABLE=0|1

Examples:
  tools/obs-diagnose.sh
  tools/obs-diagnose.sh -g stream-night
  CAPTURE_FULL_SYSTEM_JOURNAL=1 tools/obs-diagnose.sh -s 1
USAGE
}

while getopts ":g:o:s:b:h" opt; do
  case "${opt}" in
    g) RUN_TAG="${OPTARG}" ;;
    o) OUT_BASE="${OPTARG}" ;;
    s) SAMPLE_SEC="${OPTARG}" ;;
    b) OBS_BIN="${OPTARG}" ;;
    h) usage; exit 0 ;;
    :) echo "Missing arg for -${OPTARG}" >&2; exit 2 ;;
    \?) echo "Unknown option: -${OPTARG}" >&2; exit 2 ;;
  esac
done
shift $((OPTIND - 1))

if ! [[ "${SAMPLE_SEC}" =~ ^[0-9]+$ ]] || [[ "${SAMPLE_SEC}" -lt 1 ]]; then
  echo "Invalid sample interval: ${SAMPLE_SEC}" >&2
  exit 2
fi
if [[ "${DIAG_AUTO_TOGGLE}" != "0" && "${DIAG_AUTO_TOGGLE}" != "1" ]]; then
  echo "Invalid DIAG_AUTO_TOGGLE: ${DIAG_AUTO_TOGGLE}" >&2
  exit 2
fi
for toggle in CAPTURE_PIPEWIRE PW_PROFILER_ENABLE; do
  if [[ "${!toggle}" != "0" && "${!toggle}" != "1" ]]; then
    echo "Invalid ${toggle}: ${!toggle}" >&2
    exit 2
  fi
done

obs_args=("$@")

stamp="$(date -u +%Y%m%d-%H%M%S)"
safe_tag=""
if [[ -n "${RUN_TAG}" ]]; then
  safe_tag="$(echo "${RUN_TAG}" | tr -cs 'A-Za-z0-9._-' '-' | sed 's/^-*//;s/-*$//')"
fi

mod_src="unknown"
if command -v modinfo >/dev/null 2>&1; then
  mod_src="$(modinfo "${MODULE}" 2>/dev/null | awk -F': *' '/^srcversion/{print $2; exit}')"
fi
if [[ -z "${mod_src}" ]]; then
  mod_src="unknown"
fi

run_id="${stamp}"
if [[ -n "${safe_tag}" ]]; then
  run_id="${run_id}-${safe_tag}"
fi
run_id="${run_id}-${MODULE}-${mod_src}"

run_dir="${OUT_BASE}/${run_id}"
mkdir -p "${run_dir}"

meta="${run_dir}/meta.env"
periodic_log="${run_dir}/periodic-samples.log"
kernel_journal_log="${run_dir}/journal-kernel-follow.log"
kernel_filtered_log="${run_dir}/journal-kernel-filtered.log"
system_journal_log="${run_dir}/journal-system-follow.log"
obs_stdout_log="${run_dir}/obs-stdout.log"
obs_profile_logs_dir="${run_dir}/obs-profile-logs"
pw_dump_before="${run_dir}/pw-dump.before.json"
pw_dump_after="${run_dir}/pw-dump.after.json"
pw_profiler_log="${run_dir}/pw-profiler.json"
mkdir -p "${obs_profile_logs_dir}"

start_iso="$(date -u --iso-8601=seconds)"
start_epoch="$(date +%s)"

{
  echo "run_id=${run_id}"
  echo "start_utc=${start_iso}"
  echo "hostname=$(hostname)"
  echo "kernel=$(uname -r)"
  echo "module=${MODULE}"
  echo "module_srcversion=${mod_src}"
  echo "obs_bin=${OBS_BIN}"
  echo "sample_sec=${SAMPLE_SEC}"
  echo "capture_full_system_journal=${CAPTURE_FULL_SYSTEM_JOURNAL}"
  echo "priv_helper=${PRIV_HELPER}"
  echo "diag_auto_toggle=${DIAG_AUTO_TOGGLE}"
  echo "video_device=${VIDEO_DEVICE}"
  echo "capture_pipewire=${CAPTURE_PIPEWIRE}"
  echo "pw_profiler_enable=${PW_PROFILER_ENABLE}"
} > "${meta}"

if command -v modinfo >/dev/null 2>&1; then
  modinfo "${MODULE}" > "${run_dir}/modinfo.txt" 2>/dev/null || true
fi
if command -v dkms >/dev/null 2>&1; then
  dkms status > "${run_dir}/dkms-status.txt" 2>/dev/null || true
fi
if command -v lsmod >/dev/null 2>&1; then
  lsmod > "${run_dir}/lsmod.txt"
fi
if command -v v4l2-ctl >/dev/null 2>&1; then
  v4l2-ctl -d "${VIDEO_DEVICE}" --all > "${run_dir}/v4l2-all.txt" 2>&1 || true
  v4l2-ctl -d "${VIDEO_DEVICE}" --list-formats-ext > "${run_dir}/v4l2-formats.txt" 2>&1 || true
fi
if [[ "${CAPTURE_PIPEWIRE}" == "1" ]] && command -v pw-dump >/dev/null 2>&1; then
  pw-dump -N > "${pw_dump_before}" 2>&1 || true
fi
{
  echo "===== /proc/cmdline ====="
  /usr/bin/cat /proc/cmdline 2>/dev/null || true
  echo
  echo "===== /proc/loadavg ====="
  /usr/bin/cat /proc/loadavg 2>/dev/null || true
} > "${run_dir}/system-snapshot.pre.txt"

priv_available=0
if command -v sudo >/dev/null 2>&1 && [[ -x "${PRIV_HELPER}" ]]; then
  if sudo -n "${PRIV_HELPER}" --self-test >/dev/null 2>&1; then
    priv_available=1
  fi
fi
echo "privileged_capture_available=${priv_available}" >> "${meta}"

diag_initial="unknown"
diag_set_on_start=0
diag_restore_on_exit=0
diag_restore_target=""

read_diag() {
  if [[ "${priv_available}" == "1" ]]; then
    sudo -n "${PRIV_HELPER}" --module "${MODULE}" --get-diag 2>/dev/null || true
  else
    /usr/bin/cat "/sys/module/${MODULE}/parameters/diag_enable" 2>/dev/null || true
  fi
}

write_diag() {
  local v="$1"
  if [[ "${priv_available}" == "1" ]]; then
    sudo -n "${PRIV_HELPER}" --module "${MODULE}" --set-diag "${v}" >/dev/null 2>&1
  else
    return 1
  fi
}

restore_diag_once() {
  if [[ "${diag_set_on_start}" == "1" && "${diag_restore_on_exit}" == "0" && -n "${diag_restore_target}" ]]; then
    if write_diag "${diag_restore_target}"; then
      diag_restore_on_exit=1
    fi
  fi
}

diag_initial="$(read_diag | tr -d '[:space:]')"
if [[ -z "${diag_initial}" ]]; then
  diag_initial="unknown"
fi
echo "diag_initial=${diag_initial}" >> "${meta}"

if [[ "${DIAG_AUTO_TOGGLE}" == "1" ]]; then
  if [[ "${diag_initial}" == "0" || "${diag_initial}" == "1" ]]; then
    diag_restore_target="${diag_initial}"
    if write_diag "1"; then
      diag_set_on_start=1
    fi
  fi
fi
echo "diag_set_on_start=${diag_set_on_start}" >> "${meta}"

declare -a bg_pids=()

start_bg() {
  "$@" &
  bg_pids+=("$!")
}

cleanup() {
  restore_diag_once
  local p
  for p in "${bg_pids[@]:-}"; do
    kill "${p}" >/dev/null 2>&1 || true
  done
}
trap cleanup EXIT

if command -v journalctl >/dev/null 2>&1; then
  if [[ "${priv_available}" == "1" ]]; then
    start_bg bash -lc "sudo -n \"${PRIV_HELPER}\" --module \"${MODULE}\" --follow-kernel > \"${kernel_journal_log}\" 2>&1"
    start_bg bash -lc "sudo -n \"${PRIV_HELPER}\" --module \"${MODULE}\" --follow-kernel 2>&1 | rg --line-buffered -i '${MODULE}|videobuf2|uvcvideo|retire_capture_urb|callbacks suppressed|timeout|reset|error|BUG:|Oops|soft lockup' > \"${kernel_filtered_log}\""
    if [[ "${CAPTURE_FULL_SYSTEM_JOURNAL}" == "1" ]]; then
      start_bg bash -lc "sudo -n \"${PRIV_HELPER}\" --module \"${MODULE}\" --follow-system > \"${system_journal_log}\" 2>&1"
    fi
  else
    start_bg bash -lc "journalctl -k -f -o short-iso > \"${kernel_journal_log}\" 2>&1"
    start_bg bash -lc "journalctl -k -f -o short-iso 2>&1 | rg --line-buffered -i '${MODULE}|videobuf2|uvcvideo|retire_capture_urb|callbacks suppressed|timeout|reset|error|BUG:|Oops|soft lockup' > \"${kernel_filtered_log}\""
    if [[ "${CAPTURE_FULL_SYSTEM_JOURNAL}" == "1" ]]; then
      start_bg bash -lc "journalctl -f -o short-iso > \"${system_journal_log}\" 2>&1"
    fi
  fi
fi

if [[ "${CAPTURE_PIPEWIRE}" == "1" ]] && command -v pw-top >/dev/null 2>&1; then
  start_bg bash -lc "pw-top -b > \"${run_dir}/pw-top-live.log\" 2>&1"
fi
if [[ "${CAPTURE_PIPEWIRE}" == "1" && "${PW_PROFILER_ENABLE}" == "1" ]] &&
   command -v pw-profiler >/dev/null 2>&1; then
  start_bg bash -lc "pw-profiler -J > \"${pw_profiler_log}\" 2>&1"
fi

sampler() {
  while kill -0 "${obs_pid}" >/dev/null 2>&1; do
    {
      echo "===== sample_utc=$(date -u --iso-8601=seconds) ====="
      /usr/bin/cat /proc/loadavg 2>/dev/null || true
      echo "-- ps (obs/pipewire/wireplumber) --"
      ps -eo pid,ppid,ni,pri,psr,stat,%cpu,%mem,rss,cmd | rg -i '(^ *PID|obs|pipewire|wireplumber)' || true
      echo "-- interrupts (hws/uvc/xhci/nvidia) --"
      /usr/bin/cat /proc/interrupts 2>/dev/null | rg -i 'hws|uvc|xhci|nvidia' || true
      echo "-- hws params --"
      /usr/bin/cat /sys/module/${MODULE}/parameters/diag_enable 2>/dev/null || true
      /usr/bin/cat /sys/module/${MODULE}/parameters/video_work_budget 2>/dev/null || true
      /usr/bin/cat /sys/module/${MODULE}/parameters/audio_work_budget 2>/dev/null || true
      /usr/bin/cat /sys/module/${MODULE}/parameters/audio_period_bytes 2>/dev/null || true
      /usr/bin/cat /sys/module/${MODULE}/parameters/audio_periods 2>/dev/null || true
      /usr/bin/cat /sys/module/${MODULE}/parameters/fps_policy_mode 2>/dev/null || true
      echo "-- hws diag video_diag --"
      /usr/bin/cat /proc/hwsuhdx1/video_diag 2>/dev/null ||
        /usr/bin/cat /sys/kernel/debug/hwsuhdx1/video_diag 2>/dev/null || true
      echo "-- hws diag audio_diag --"
      /usr/bin/cat /proc/hwsuhdx1/audio_diag 2>/dev/null ||
        /usr/bin/cat /sys/kernel/debug/hwsuhdx1/audio_diag 2>/dev/null || true
      echo "-- hws diag source_cadence --"
      /usr/bin/cat /proc/hwsuhdx1/source_cadence 2>/dev/null ||
        /usr/bin/cat /sys/kernel/debug/hwsuhdx1/source_cadence 2>/dev/null || true
      if [[ "${priv_available}" == "1" ]]; then
        echo "-- privileged helper sample --"
        sudo -n "${PRIV_HELPER}" --module "${MODULE}" --sample 2>/dev/null || true
      fi
      echo
    } >> "${periodic_log}"
    sleep "${SAMPLE_SEC}"
  done
}

obs_log_dir="${HOME}/.config/obs-studio/logs"
before_obs_logs="${run_dir}/obs-logs.before.txt"
after_obs_logs="${run_dir}/obs-logs.after.txt"
if [[ -d "${obs_log_dir}" ]]; then
  find "${obs_log_dir}" -maxdepth 1 -type f -name '*.txt' -printf '%f\n' | sort > "${before_obs_logs}" || true
fi

"${OBS_BIN}" "${obs_args[@]}" > "${obs_stdout_log}" 2>&1 &
obs_pid=$!
sampler &
bg_pids+=("$!")

set +e
wait "${obs_pid}"
obs_exit=$?
set -e

restore_diag_once

end_iso="$(date -u --iso-8601=seconds)"
end_epoch="$(date +%s)"
runtime_sec="$((end_epoch - start_epoch))"

if [[ -d "${obs_log_dir}" ]]; then
  find "${obs_log_dir}" -maxdepth 1 -type f -name '*.txt' -printf '%f\n' | sort > "${after_obs_logs}" || true
  if [[ -f "${before_obs_logs}" && -f "${after_obs_logs}" ]]; then
    comm -13 "${before_obs_logs}" "${after_obs_logs}" | while read -r f; do
      [[ -n "${f}" ]] || continue
      cp -a "${obs_log_dir}/${f}" "${obs_profile_logs_dir}/" 2>/dev/null || true
    done
  fi
  # Also copy files updated during this run window.
  find "${obs_log_dir}" -maxdepth 1 -type f -name '*.txt' -newermt "${start_iso}" -exec cp -a {} "${obs_profile_logs_dir}/" \; 2>/dev/null || true
fi

if command -v coredumpctl >/dev/null 2>&1; then
  coredumpctl --since "${start_iso}" --no-pager > "${run_dir}/coredumps-since-start.txt" 2>&1 || true
fi
if [[ "${CAPTURE_PIPEWIRE}" == "1" ]] && command -v pw-dump >/dev/null 2>&1; then
  pw-dump -N > "${pw_dump_after}" 2>&1 || true
fi

{
  echo "===== /proc/cmdline ====="
  /usr/bin/cat /proc/cmdline 2>/dev/null || true
  echo
  echo "===== /proc/loadavg ====="
  /usr/bin/cat /proc/loadavg 2>/dev/null || true
} > "${run_dir}/system-snapshot.post.txt"

if command -v journalctl >/dev/null 2>&1; then
  if [[ "${priv_available}" == "1" ]]; then
    sudo -n "${PRIV_HELPER}" --module "${MODULE}" --kernel-since "${start_iso}" > "${run_dir}/journal-kernel-since-start.log" 2>&1 || true
  else
    journalctl -k -b --since "${start_iso}" --no-pager > "${run_dir}/journal-kernel-since-start.log" 2>&1 || true
  fi
  cat "${run_dir}/journal-kernel-since-start.log" | rg -i "${MODULE}|videobuf2|uvcvideo|retire_capture_urb|callbacks suppressed|timeout|reset|error|BUG:|Oops|soft lockup" > "${run_dir}/journal-kernel-since-start.filtered.log" 2>&1 || true
fi

count_matches() {
  local f="$1"
  local p="$2"
  if [[ -f "${f}" ]]; then
    rg -i -c "${p}" "${f}" 2>/dev/null || echo 0
  else
    echo 0
  fi
}

obs_v4l2_select_timeout_count="$(count_matches "${obs_stdout_log}" "v4l2-input: .*select timed out")"
obs_v4l2_failed_status_count="$(count_matches "${obs_stdout_log}" "v4l2-input: .*failed to log status")"
obs_render_lag_line="$(rg -i "lagged frames due to rendering lag/stalls" "${obs_stdout_log}" 2>/dev/null | tail -n 1 || true)"
obs_encode_lag_line="$(rg -i "skipped frames due to encoding lag" "${obs_stdout_log}" 2>/dev/null | tail -n 1 || true)"
obs_net_drop_line="$(rg -i "dropped frames due to insufficient bandwidth/connection stalls" "${obs_stdout_log}" 2>/dev/null | tail -n 1 || true)"
kernel_retire_suppressed_count="$(count_matches "${run_dir}/journal-kernel-since-start.filtered.log" "retire_capture_urb: .*callbacks suppressed")"
kernel_hw_timeout_count="$(count_matches "${run_dir}/journal-kernel-since-start.filtered.log" "Timeout waiting for hardware access")"
kernel_nvidia_gem_error_count="$(count_matches "${run_dir}/journal-kernel-since-start.filtered.log" "Failed to allocate NVKMS memory for GEM object")"

{
  echo "end_utc=${end_iso}"
  echo "runtime_seconds=${runtime_sec}"
  echo "obs_exit_code=${obs_exit}"
  echo "run_dir=${run_dir}"
  echo "diag_restore_on_exit=${diag_restore_on_exit}"
  echo "diag_restore_target=${diag_restore_target:-unknown}"
  diag_final="$(read_diag | tr -d '[:space:]')"
  if [[ -z "${diag_final}" ]]; then
    diag_final="unknown"
  fi
  echo "diag_final=${diag_final}"
  echo "obs_v4l2_select_timeout_count=${obs_v4l2_select_timeout_count}"
  echo "obs_v4l2_failed_status_count=${obs_v4l2_failed_status_count}"
  echo "kernel_retire_suppressed_count=${kernel_retire_suppressed_count}"
  echo "kernel_hw_timeout_count=${kernel_hw_timeout_count}"
  echo "kernel_nvidia_gem_error_count=${kernel_nvidia_gem_error_count}"
  echo "pw_dump_before=${pw_dump_before}"
  echo "pw_dump_after=${pw_dump_after}"
  echo "pw_profiler_log=${pw_profiler_log}"
  echo "obs_render_lag_line=${obs_render_lag_line}"
  echo "obs_encode_lag_line=${obs_encode_lag_line}"
  echo "obs_net_drop_line=${obs_net_drop_line}"
} >> "${meta}"

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
baseline_parser="${repo_root}/scripts/analyze-stability-artifacts.sh"
if [[ -x "${baseline_parser}" ]]; then
  "${baseline_parser}" -O "${OUT_BASE}" -B "${repo_root}/bench-results" -o "${run_dir}/stability-score.tsv" >/dev/null 2>&1 || true
fi

echo "run_id=${run_id}"
echo "run_dir=${run_dir}"
echo "obs_exit_code=${obs_exit}"
echo "runtime_seconds=${runtime_sec}"

exit "${obs_exit}"
