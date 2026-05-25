#!/usr/bin/env bash
set -euo pipefail

# Reboot-gated checkpoint runner for environments where module unload/reload
# is not reliable (e.g., PipeWire holds the capture device).

MODULE="${MODULE:-HwsUHDX1Capture}"
VERSION="${VERSION:-1.0.0.230324}"
KVER="${KVER:-$(uname -r)}"
SRC_DIR="${SRC_DIR:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/src}"
RESULTS_BASE="${RESULTS_BASE:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/bench-results}"
RUN_TAG="${RUN_TAG:-checkpoint}"
INITRD="${INITRD:-detected-by-dkms-versioned-build}"
DEVICE="${DEVICE:-/dev/video0}"
DURATION="${DURATION:-60}"
FPS="${FPS:-60}"
SIZE="${SIZE:-1920x1080}"
SKIP_DKMS="${SKIP_DKMS:-0}"
SKIP_BENCH="${SKIP_BENCH:-0}"
SKIP_STATIC_CHECK="${SKIP_STATIC_CHECK:-0}"
ALLOW_LTS_TARGET="${ALLOW_LTS_TARGET:-0}"
PENDING_FILE="${PENDING_FILE:-/var/tmp/hws-checkpoint-pending.env}"
STALE_HOURS="${STALE_HOURS:-24}"
PREPARE_REBOOT=0
POSTBOOT_RUN=0
FORCE_STALE=0

usage() {
  cat <<'USAGE'
Usage: scripts/checkpoint-run.sh [options]

Runs a checkpointed validation flow without module unload/reload.
It prints the privileged commands to run as root when needed.

Options:
  --run-tag <tag>      Artifact tag (default: checkpoint)
  --device <path>      Video device (default: /dev/video0)
  --duration <sec>     Bench duration (default: 60)
  --fps <n>            Bench target fps (default: 60)
  --size <WxH>         Bench frame size (default: 1920x1080)
  --skip-dkms          Skip DKMS command emission
  --skip-bench         Skip bench execution
  --skip-static-check  Skip the non-privileged source build before DKMS emission
  --prepare-reboot     Create pending marker and emit reboot-resume commands
  --postboot-run       Resume from pending marker and finish checkpoint
  --pending-file PATH  Override pending marker path
  --force-stale        Ignore stale pending marker age check
  -h, --help           Show help

Environment:
  MODULE VERSION KVER SRC_DIR RESULTS_BASE RUN_TAG INITRD ALLOW_LTS_TARGET
  ALLOW_LTS_TARGET=1 explicitly permits preparing an LTS checkpoint.
USAGE
}

while (($#)); do
  case "$1" in
    --run-tag) RUN_TAG="${2:-}"; shift 2 ;;
    --device) DEVICE="${2:-}"; shift 2 ;;
    --duration) DURATION="${2:-}"; shift 2 ;;
    --fps) FPS="${2:-}"; shift 2 ;;
    --size) SIZE="${2:-}"; shift 2 ;;
    --skip-dkms) SKIP_DKMS=1; shift ;;
    --skip-bench) SKIP_BENCH=1; shift ;;
    --skip-static-check) SKIP_STATIC_CHECK=1; shift ;;
    --prepare-reboot) PREPARE_REBOOT=1; shift ;;
    --postboot-run) POSTBOOT_RUN=1; shift ;;
    --pending-file) PENDING_FILE="${2:-}"; shift 2 ;;
    --force-stale) FORCE_STALE=1; shift ;;
    -h|--help) usage; exit 0 ;;
    *) echo "Unknown option: $1" >&2; exit 2 ;;
  esac
done

if ! [[ "${DURATION}" =~ ^[0-9]+$ ]] || [[ "${DURATION}" -lt 1 ]]; then
  echo "Invalid duration: ${DURATION}" >&2
  exit 2
fi
if [[ "${ALLOW_LTS_TARGET}" != "0" && "${ALLOW_LTS_TARGET}" != "1" ]]; then
  echo "Invalid ALLOW_LTS_TARGET: ${ALLOW_LTS_TARGET}" >&2
  exit 2
fi

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

write_pending_marker() {
  {
    printf 'CHECKPOINT_ID=%q\n' "${checkpoint_id}"
    printf 'OUTDIR=%q\n' "${outdir}"
    printf 'RESULTS_BASE=%q\n' "${RESULTS_BASE}"
    printf 'RUN_TAG=%q\n' "${RUN_TAG}"
    printf 'MODULE=%q\n' "${MODULE}"
    printf 'VERSION=%q\n' "${VERSION}"
    printf 'KVER=%q\n' "${KVER}"
    printf 'SRC_DIR=%q\n' "${SRC_DIR}"
    printf 'DEVICE=%q\n' "${DEVICE}"
    printf 'DURATION=%q\n' "${DURATION}"
    printf 'FPS=%q\n' "${FPS}"
    printf 'SIZE=%q\n' "${SIZE}"
    printf 'INITRD=%q\n' "${INITRD}"
    printf 'CREATED_EPOCH=%q\n' "$(date +%s)"
  } > "${PENDING_FILE}"
}

consume_pending_marker() {
  if [[ ! -r "${PENDING_FILE}" ]]; then
    echo "Missing pending marker: ${PENDING_FILE}" >&2
    exit 2
  fi
  # shellcheck disable=SC1090
  source "${PENDING_FILE}"
  if [[ -z "${OUTDIR:-}" || -z "${CHECKPOINT_ID:-}" ]]; then
    echo "Invalid pending marker: ${PENDING_FILE}" >&2
    exit 2
  fi
  local now age max_age
  now="$(date +%s)"
  max_age="$((STALE_HOURS * 3600))"
  age="$((now - ${CREATED_EPOCH:-0}))"
  if [[ "${FORCE_STALE}" != "1" && "${age}" -gt "${max_age}" ]]; then
    echo "Pending marker is stale (${age}s > ${max_age}s): ${PENDING_FILE}" >&2
    exit 3
  fi
}

if [[ "${POSTBOOT_RUN}" == "1" ]]; then
  consume_pending_marker
  checkpoint_id="${CHECKPOINT_ID}"
  outdir="${OUTDIR}"
  SKIP_DKMS=1
  SKIP_BENCH=0
else
  stamp="$(date -u +%Y%m%d-%H%M%S)"
  checkpoint_id="${stamp}-${RUN_TAG}"
  outdir="${RESULTS_BASE}/${checkpoint_id}"
fi
mkdir -p "${outdir}"

{
  echo "checkpoint_id=${checkpoint_id}"
  echo "timestamp_utc=$(date -u --iso-8601=seconds)"
  echo "module=${MODULE}"
  echo "version=${VERSION}"
  echo "kernel=${KVER}"
  echo "src_dir=${SRC_DIR}"
  echo "device=${DEVICE}"
  echo "duration=${DURATION}"
  echo "fps=${FPS}"
  echo "size=${SIZE}"
  echo "initrd=${INITRD}"
} > "${outdir}/checkpoint.env"

if [[ "${POSTBOOT_RUN}" == "1" ]]; then
  {
    echo "postboot_resume_utc=$(date -u --iso-8601=seconds)"
    echo "pending_file=${PENDING_FILE}"
  } >> "${outdir}/checkpoint.env"
fi

if [[ "${SKIP_DKMS}" == "0" ]]; then
  if [[ "${KVER}" == *lts* && "${ALLOW_LTS_TARGET}" != "1" ]]; then
    echo "Refusing to prepare a DKMS checkpoint for LTS fallback kernel ${KVER}." >&2
    echo "Boot the non-LTS test kernel first, or set ALLOW_LTS_TARGET=1 deliberately." >&2
    exit 2
  fi
  if [[ "${KVER}" != "$(uname -r)" ]]; then
    echo "Refusing DKMS checkpoint preparation for non-running kernel ${KVER}." >&2
    echo "The versioned workflow rebuilds only the running kernel initramfs safely." >&2
    exit 2
  fi
  if [[ "${SKIP_STATIC_CHECK}" == "0" ]]; then
    "${script_dir}/static-check.sh" -o "${RESULTS_BASE}" |
      tee "${outdir}/static-check.stdout.txt"
  fi
  cat > "${outdir}/dkms-commands.txt" <<CMDS
# Run as root while booted into the intended non-LTS test kernel.
# The versioned builder records source identity and detects the current dracut target.
cd "$(cd "${script_dir}/.." && pwd)"
sudo -E ./scripts/dkms-versioned-build.sh --kernels current --prune-others --dracut-current
# reboot required before validation bench
CMDS
  if [[ "${PREPARE_REBOOT}" == "1" ]]; then
    write_pending_marker
    cat >> "${outdir}/dkms-commands.txt" <<CMDS
# post-reboot resume (as root or user with module access):
#   ${script_dir}/checkpoint-postboot.sh --pending-file "${PENDING_FILE}"
CMDS
  fi
fi

bench_summary=""
if [[ "${SKIP_BENCH}" == "0" ]]; then
  bench_log="${outdir}/bench.stdout.txt"
  "${script_dir}/bench.sh" \
    -d "${DEVICE}" -t "${DURATION}" -f "${FPS}" -s "${SIZE}" -g "${checkpoint_id}" -o "${RESULTS_BASE}" |
    tee "${bench_log}"
  bench_summary="$(awk -F= '/^summary=/{print $2}' "${bench_log}" | tail -n1)"
  if [[ -n "${bench_summary}" ]]; then
    printf 'bench_summary=%s\n' "${bench_summary}" >> "${outdir}/checkpoint.env"
  fi
fi

if command -v modinfo >/dev/null 2>&1; then
  modinfo "${MODULE}" > "${outdir}/modinfo.txt" 2>&1 || true
fi
if [[ -r "/sys/module/${MODULE}/srcversion" ]]; then
  /usr/bin/cat "/sys/module/${MODULE}/srcversion" > "${outdir}/runtime.srcversion.txt" 2>/dev/null || true
fi

if [[ -x "${script_dir}/analyze-stability-artifacts.sh" && -n "${bench_summary}" ]]; then
  "${script_dir}/analyze-stability-artifacts.sh" \
    -B "${RESULTS_BASE}" -O "$HOME/obs-diag-results" -S "${bench_summary}" -o "${outdir}/stability-score.tsv" || true
fi

if [[ -s "${outdir}/stability-score.tsv" ]]; then
  awk -F'\t' '
    NR == 1 { for (i = 1; i <= NF; i++) col[$i] = i; next }
    { verdict = $(col["verdict"]); reason = $(col["verdict_reason"]) }
    END { printf "checkpoint_verdict=%s\ncheckpoint_reason=%s\n", verdict, reason }
  ' "${outdir}/stability-score.tsv" > "${outdir}/checkpoint.verdict.env"
fi

if [[ "${POSTBOOT_RUN}" == "1" ]]; then
  {
    echo "completed_utc=$(date -u --iso-8601=seconds)"
    echo "completed_epoch=$(date +%s)"
  } > "${outdir}/checkpoint.completed.env"
  rm -f "${PENDING_FILE}"
fi

echo "checkpoint_dir=${outdir}"
