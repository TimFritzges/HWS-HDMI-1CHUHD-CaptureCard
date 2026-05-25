#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT_BASE="${OUT_BASE:-${ROOT}/bench-results}"
RUN_TAG="${RUN_TAG:-trace}"
TRACE_FUNCTIONS="${TRACE_FUNCTIONS:-0}"

usage() {
  cat <<'USAGE'
Usage: scripts/trace-session.sh [options] -- <reproduction command> [args...]

Record scheduler/timer/workqueue/IRQ activity around one reproduction command.
This script never uses sudo and does not load or unload the driver.

Options:
  -o <out-base>       Artifact base directory (default: ./bench-results)
  -g <tag>            Run tag (default: trace)
  --functions         Also trace selected HWS functions (higher overhead)
  -h                  Show help

Examples:
  scripts/trace-session.sh -- scripts/bench.sh -t 10 -g trace-video
  TRACE_FUNCTIONS=1 scripts/trace-session.sh -- scripts/bench.sh -t 10 -g trace-audio

Requirements:
  `trace-cmd` installed and tracefs access granted for this user.
USAGE
}

while (($#)); do
  case "$1" in
    -o) OUT_BASE="${2:-}"; shift 2 ;;
    -g) RUN_TAG="${2:-}"; shift 2 ;;
    --functions) TRACE_FUNCTIONS=1; shift ;;
    --) shift; break ;;
    -h|--help) usage; exit 0 ;;
    *) echo "Unknown option before --: $1" >&2; exit 2 ;;
  esac
done

if (($# == 0)); then
  echo "Missing reproduction command after --" >&2
  usage >&2
  exit 2
fi
if [[ "${TRACE_FUNCTIONS}" != "0" && "${TRACE_FUNCTIONS}" != "1" ]]; then
  echo "Invalid TRACE_FUNCTIONS: ${TRACE_FUNCTIONS}" >&2
  exit 2
fi
if ! command -v trace-cmd >/dev/null 2>&1; then
  echo "trace-cmd is not installed; install it before running trace capture." >&2
  exit 127
fi

stamp="$(date -u +%Y%m%d-%H%M%S)"
safe_tag="$(echo "${RUN_TAG}" | tr -cs 'A-Za-z0-9._-' '-')"
outdir="${OUT_BASE}/${stamp}-${safe_tag}"
mkdir -p "${outdir}"
trace_dat="${outdir}/trace.dat"
trace_report="${outdir}/trace.report.txt"
summary="${outdir}/summary.env"

trace_args=(
  record -o "${trace_dat}"
  -e irq:irq_handler_entry -e irq:irq_handler_exit
  -e workqueue:workqueue_queue_work -e workqueue:workqueue_execute_start -e workqueue:workqueue_execute_end
  -e timer:hrtimer_start -e timer:hrtimer_expire_entry -e timer:hrtimer_expire_exit
  -e sched:sched_wakeup -e sched:sched_switch
)
if [[ "${TRACE_FUNCTIONS}" == "1" ]]; then
  trace_args+=(
    -p function
    -l video_data_process
    -l audio_data_process
    -l hws_audio_publish_timer_fn
    -l MemCopyVideoToSteam
    -l MemCopyAudioToSteam
    -l SetQuene
    -l SetAudioQuene
  )
fi

set +e
trace-cmd "${trace_args[@]}" -- "$@" > "${outdir}/trace-cmd.stdout.log" 2> "${outdir}/trace-cmd.stderr.log"
trace_exit=$?
set -e
if [[ "${trace_exit}" -eq 0 ]]; then
  trace-cmd report "${trace_dat}" > "${trace_report}" 2>&1 || true
fi

{
  echo "timestamp_utc=$(date -u --iso-8601=seconds)"
  echo "trace_functions=${TRACE_FUNCTIONS}"
  echo "trace_exit_code=${trace_exit}"
  printf 'reproduction_command='
  printf '%q ' "$@"
  echo
  echo "trace_dat=${trace_dat}"
  echo "trace_report=${trace_report}"
  echo "outdir=${outdir}"
} > "${summary}"
cat "${summary}"
exit "${trace_exit}"
