#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BENCH_BASE="${BENCH_BASE:-${ROOT}/bench-results}"
OBS_BASE="${OBS_BASE:-$HOME/obs-diag-results}"
PROFILE="${PROFILE:-${ROOT}/scripts/stability-gate.env}"
OUT="${OUT:-${BENCH_BASE}/bisect-eval-$(date -u +%Y%m%d-%H%M%S).tsv}"
RUN_BENCH="${RUN_BENCH:-1}"
BENCH_DEVICE="${BENCH_DEVICE:-/dev/video0}"
BENCH_DURATION="${BENCH_DURATION:-60}"
BENCH_FPS="${BENCH_FPS:-60}"
BENCH_SIZE="${BENCH_SIZE:-1920x1080}"
BENCH_TAG="${BENCH_TAG:-bisect}"

usage() {
  cat <<'USAGE'
Usage: scripts/bisect-eval.sh [options]

Runs bench/analyzer and returns git-bisect-compatible exit codes:
  0   good
  1   bad
  125 untestable

Options:
  --no-bench         Skip bench run; evaluate existing artifacts only
  --bench-device P   Bench device (default /dev/video0)
  --bench-duration N Bench duration seconds (default 60)
  --bench-fps N      Bench fps (default 60)
  --bench-size WxH   Bench size (default 1920x1080)
  --profile PATH     Gate profile env file
  --out PATH         Analyzer TSV output path
  -h, --help         Show help
USAGE
}

while (($#)); do
  case "$1" in
    --no-bench) RUN_BENCH=0; shift ;;
    --bench-device) BENCH_DEVICE="${2:-}"; shift 2 ;;
    --bench-duration) BENCH_DURATION="${2:-}"; shift 2 ;;
    --bench-fps) BENCH_FPS="${2:-}"; shift 2 ;;
    --bench-size) BENCH_SIZE="${2:-}"; shift 2 ;;
    --profile) PROFILE="${2:-}"; shift 2 ;;
    --out) OUT="${2:-}"; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) echo "Unknown option: $1" >&2; exit 125 ;;
  esac
done

bench_summary=""
if [[ "${RUN_BENCH}" == "1" ]]; then
  bench_output="$("${ROOT}/scripts/bench.sh" -d "${BENCH_DEVICE}" -t "${BENCH_DURATION}" -f "${BENCH_FPS}" -s "${BENCH_SIZE}" -g "${BENCH_TAG}" -o "${BENCH_BASE}" || true)"
  printf '%s\n' "${bench_output}"
  bench_summary="$(printf '%s\n' "${bench_output}" | awk -F= '/^summary=/{print $2}' | tail -n1)"
  if [[ -z "${bench_summary}" ]]; then
    echo "bisect_eval=untestable reason=bench_summary_missing"
    exit 125
  fi
fi

analyze_args=(-B "${BENCH_BASE}" -O "${OBS_BASE}" -p "${PROFILE}" -o "${OUT}")
if [[ -n "${bench_summary}" ]]; then
  analyze_args+=(-S "${bench_summary}")
fi
"${ROOT}/scripts/analyze-stability-artifacts.sh" "${analyze_args[@]}" >/dev/null

last_line="$(tail -n 1 "${OUT}" || true)"
if [[ -z "${last_line}" ]]; then
  echo "bisect_eval=untestable reason=no_results out=${OUT}"
  exit 125
fi

read -r verdict reason run_id < <(
  awk -F'\t' '
    NR == 1 { for (i = 1; i <= NF; i++) col[$i] = i; next }
    { verdict = $(col["verdict"]); reason = $(col["verdict_reason"]); run_id = $(col["run_id"]) }
    END { print verdict, reason, run_id }
  ' "${OUT}"
)

echo "bisect_eval verdict=${verdict} reason=${reason} run_id=${run_id} out=${OUT}"

case "${verdict}" in
  pass)
    exit 0
    ;;
  warn|fail)
    exit 1
    ;;
  *)
    exit 125
    ;;
esac
