#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MODULE="${MODULE:-HwsUHDX1Capture}"
DEVICE="${DEVICE:-/dev/video0}"
DURATION="${DURATION:-60}"
SIZE="${SIZE:-1920x1080}"
FPS="${FPS:-60}"
OUT_BASE="${OUT_BASE:-${ROOT}/bench-results}"
RUN_TAG="${RUN_TAG:-postboot-autonomous}"
COMPLIANCE_FRAMES="${COMPLIANCE_FRAMES:-60}"
PW_PROFILER_SAMPLES="${PW_PROFILER_SAMPLES:-600}"

usage() {
  cat <<'USAGE'
Usage: scripts/postboot-diagnose.sh [options]

Run the complete non-privileged post-reboot validation sequence:
module identity, static build/sparse, state snapshots, V4L2 compliance,
capture benchmark, PipeWire profiling, and stability verdict.

Options:
  -d <device>    V4L2 node (default: /dev/video0)
  -t <seconds>   Capture duration (default: 60)
  -s <WxH>       Requested capture resolution (default: 1920x1080)
  -f <fps>       Requested capture FPS (default: 60)
  -o <dir>       Artifact base (default: ./bench-results)
  -g <tag>       Run tag (default: postboot-autonomous)
  -h             Show help

Environment:
  MODULE COMPLIANCE_FRAMES PW_PROFILER_SAMPLES

Run only when OBS is closed; this workflow opens the V4L2 node.
USAGE
}

while getopts ":d:t:s:f:o:g:h" opt; do
  case "${opt}" in
    d) DEVICE="${OPTARG}" ;;
    t) DURATION="${OPTARG}" ;;
    s) SIZE="${OPTARG}" ;;
    f) FPS="${OPTARG}" ;;
    o) OUT_BASE="${OPTARG}" ;;
    g) RUN_TAG="${OPTARG}" ;;
    h) usage; exit 0 ;;
    :) echo "Missing argument for -${OPTARG}" >&2; exit 2 ;;
    \?) echo "Unknown option: -${OPTARG}" >&2; exit 2 ;;
  esac
done

for numeric in DURATION FPS COMPLIANCE_FRAMES PW_PROFILER_SAMPLES; do
  if ! [[ "${!numeric}" =~ ^[0-9]+$ ]] || [[ "${!numeric}" -lt 1 ]]; then
    echo "Invalid ${numeric}: ${!numeric}" >&2
    exit 2
  fi
done

stamp="$(date -u +%Y%m%d-%H%M%S)"
safe_tag="$(echo "${RUN_TAG}" | tr -cs 'A-Za-z0-9._-' '-')"
suite_dir="${OUT_BASE}/${stamp}-${safe_tag}-suite"
mkdir -p "${suite_dir}"
meta="${suite_dir}/suite.env"

{
  echo "suite_id=${stamp}-${safe_tag}"
  echo "timestamp_utc=$(date -u --iso-8601=seconds)"
  echo "module=${MODULE}"
  echo "device=${DEVICE}"
  echo "kernel=$(uname -r)"
  echo "duration=${DURATION}"
  echo "size=${SIZE}"
  echo "fps=${FPS}"
  echo "compliance_frames=${COMPLIANCE_FRAMES}"
  echo "pw_profiler_samples=${PW_PROFILER_SAMPLES}"
} > "${meta}"

"${ROOT}/tools/collect-evidence.sh" --out "${suite_dir}" --phase suite-pre \
  --module "${MODULE}" --device "${DEVICE}" > "${suite_dir}/snapshot.pre.stdout.txt" 2>&1 || true

static_status="fail"
if "${ROOT}/scripts/static-check.sh" --sparse yes --smatch yes -o "${suite_dir}" \
    > "${suite_dir}/static-check.stdout.txt" 2>&1; then
  static_status="pass"
fi
echo "static_check_status=${static_status}" >> "${meta}"

on_disk_src="$(modinfo "${MODULE}" 2>/dev/null | awk -F': *' '/^srcversion/ {print $2; exit}' || true)"
runtime_src="$(/usr/bin/cat "/sys/module/${MODULE}/srcversion" 2>/dev/null || true)"
srcversion_match=0
if [[ -n "${on_disk_src}" && "${on_disk_src}" == "${runtime_src}" ]]; then
  srcversion_match=1
fi
{
  echo "modinfo_srcversion=${on_disk_src:-unavailable}"
  echo "runtime_srcversion=${runtime_src:-unavailable}"
  echo "srcversion_match=${srcversion_match}"
} >> "${meta}"

bench_log="${suite_dir}/bench.stdout.txt"
bench_status="fail"
set +e
V4L2_COMPLIANCE_STREAM_FRAMES="${COMPLIANCE_FRAMES}" \
PW_PROFILER_SAMPLES="${PW_PROFILER_SAMPLES}" \
CAPTURE_FULL_SNAPSHOT=1 \
  "${ROOT}/scripts/bench.sh" -d "${DEVICE}" -t "${DURATION}" -s "${SIZE}" \
    -f "${FPS}" -g "${safe_tag}" -o "${suite_dir}/bench" | tee "${bench_log}"
bench_exit="${PIPESTATUS[0]}"
set -e
if [[ "${bench_exit}" == "0" ]]; then
  bench_status="pass"
fi
bench_summary="$(awk -F= '/^summary=/{print $2}' "${bench_log}" | tail -n1)"
{
  echo "bench_status=${bench_status}"
  echo "bench_exit_code=${bench_exit}"
  echo "bench_summary=${bench_summary:-unavailable}"
} >> "${meta}"

score="${suite_dir}/stability-score.tsv"
verdict="invalid"
reason="missing_bench_summary"
if [[ -n "${bench_summary}" && -f "${bench_summary}" ]]; then
  if "${ROOT}/scripts/analyze-stability-artifacts.sh" -S "${bench_summary}" -o "${score}" >/dev/null; then
    read -r verdict reason < <(
      awk -F'\t' 'NR == 1 {for (i=1; i<=NF; i++) c[$i]=i; next}
        NR == 2 {print $(c["verdict"]), $(c["verdict_reason"])}' "${score}"
    )
  else
    reason="stability_analyzer_failed"
  fi
fi
{
  echo "stability_score=${score}"
  echo "verdict=${verdict:-invalid}"
  echo "verdict_reason=${reason:-missing_score}"
} >> "${meta}"

"${ROOT}/tools/collect-evidence.sh" --out "${suite_dir}" --phase suite-post \
  --module "${MODULE}" --device "${DEVICE}" > "${suite_dir}/snapshot.post.stdout.txt" 2>&1 || true

cat "${meta}"
echo "suite_dir=${suite_dir}"

if [[ "${static_status}" != "pass" || "${srcversion_match}" != "1" ||
      "${bench_status}" != "pass" ||
      "${verdict:-invalid}" != "pass" ]]; then
  exit 1
fi
