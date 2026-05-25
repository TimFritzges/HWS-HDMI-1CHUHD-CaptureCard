#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC_DIR="${SRC_DIR:-${ROOT}/src}"
KERNELDIR="${KERNELDIR:-/lib/modules/$(uname -r)/build}"
OUT_BASE="${OUT_BASE:-${ROOT}/bench-results}"
RUN_SPARSE="${RUN_SPARSE:-auto}"
RUN_SMATCH="${RUN_SMATCH:-auto}"
RUN_COCCI="${RUN_COCCI:-no}"
JOBS="${JOBS:-$(nproc)}"
LOCK_WAIT_SEC="${LOCK_WAIT_SEC:-600}"
LOCK_FILE="${LOCK_FILE:-${TMPDIR:-/tmp}/hwsuhdx1capture-static-check.${UID}.lock}"

usage() {
  cat <<'USAGE'
Usage: scripts/static-check.sh [options]

Non-privileged pre-install build validation for HwsUHDX1Capture.

Options:
  -k <kernel-build-dir>  Kernel build tree (default: running kernel)
  -o <out-base>          Artifact base directory (default: ./bench-results)
  -j <jobs>              Parallel build jobs (default: nproc)
  --sparse <auto|yes|no> Run sparse when installed or require/skip it
  --smatch <auto|yes|no> Run smatch when installed or require/skip it
  --cocci <auto|yes|no>  Run Kbuild coccicheck (default: no; expensive)
  -h                      Show help

Environment:
  LOCK_WAIT_SEC=<n>       Wait for concurrent source-tree check (default: 600)
  LOCK_FILE=<path>        Override per-user validation lock path

This script does not install, unload, or load modules.
USAGE
}

while (($#)); do
  case "$1" in
    -k) KERNELDIR="${2:-}"; shift 2 ;;
    -o) OUT_BASE="${2:-}"; shift 2 ;;
    -j) JOBS="${2:-}"; shift 2 ;;
    --sparse) RUN_SPARSE="${2:-}"; shift 2 ;;
    --smatch) RUN_SMATCH="${2:-}"; shift 2 ;;
    --cocci) RUN_COCCI="${2:-}"; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) echo "Unknown option: $1" >&2; exit 2 ;;
  esac
done

if [[ ! -d "${KERNELDIR}" ]]; then
  echo "Kernel build tree not found: ${KERNELDIR}" >&2
  exit 2
fi
if ! [[ "${JOBS}" =~ ^[0-9]+$ ]] || [[ "${JOBS}" -lt 1 ]]; then
  echo "Invalid job count: ${JOBS}" >&2
  exit 2
fi
if ! [[ "${LOCK_WAIT_SEC}" =~ ^[0-9]+$ ]]; then
  echo "Invalid LOCK_WAIT_SEC: ${LOCK_WAIT_SEC}" >&2
  exit 2
fi
for analyzer in RUN_SPARSE RUN_SMATCH RUN_COCCI; do
  if [[ "${!analyzer}" != "auto" && "${!analyzer}" != "yes" && "${!analyzer}" != "no" ]]; then
    echo "Invalid analyzer value for ${analyzer}: ${!analyzer} (expected auto, yes, or no)" >&2
    exit 2
  fi
done

if ! command -v flock >/dev/null 2>&1; then
  echo "flock is required to serialize Kbuild validation." >&2
  exit 2
fi
exec {lock_fd}> "${LOCK_FILE}"
if ! flock -w "${LOCK_WAIT_SEC}" "${lock_fd}"; then
  echo "Timed out waiting for validation lock: ${LOCK_FILE}" >&2
  exit 2
fi

stamp="$(date -u +%Y%m%d-%H%M%S)"
outdir="${OUT_BASE}/${stamp}-static-check"
mkdir -p "${outdir}"
build_log="${outdir}/module-build.log"
sparse_log="${outdir}/sparse.log"
smatch_log="${outdir}/smatch.log"
cocci_log="${outdir}/coccinelle.log"
clean_log="${outdir}/clean.log"
summary="${outdir}/summary.env"

clean_module() {
  make -C "${SRC_DIR}" KERNELDIR="${KERNELDIR}" clean >> "${clean_log}" 2>&1 || true
}

count_matches() {
  local pattern="$1"
  local file="$2"
  local count

  count="$(rg -c -- "${pattern}" "${file}" 2>/dev/null || true)"
  printf '%s\n' "${count:-0}"
}

: > "${clean_log}"
clean_module
build_status="fail"
if make -C "${SRC_DIR}" KERNELDIR="${KERNELDIR}" -j"${JOBS}" > "${build_log}" 2>&1; then
  build_status="pass"
fi

sparse_status="skipped"
sparse_warning_count=0
sparse_error_count=0
if command -v sparse >/dev/null 2>&1; then
  if [[ "${RUN_SPARSE}" != "no" ]]; then
    clean_module
    if make -C "${KERNELDIR}" M="${SRC_DIR}" C=1 CHECK=sparse modules > "${sparse_log}" 2>&1; then
      sparse_warning_count="$(count_matches 'warning:' "${sparse_log}")"
      sparse_error_count="$(count_matches 'error:' "${sparse_log}")"
      if ((sparse_warning_count > 0 || sparse_error_count > 0)); then
        sparse_status="findings"
      else
        sparse_status="pass"
      fi
    else
      sparse_status="fail"
    fi
  fi
elif [[ "${RUN_SPARSE}" == "yes" ]]; then
  sparse_status="tool_missing"
  echo "sparse is required but not installed" > "${sparse_log}"
elif [[ "${RUN_SPARSE}" == "auto" ]]; then
  sparse_status="tool_missing_optional"
  echo "sparse not installed; compiler build was still executed" > "${sparse_log}"
fi

smatch_status="skipped"
smatch_warning_count=0
smatch_indent_warning_count=0
smatch_actionable_warning_count=0
smatch_error_count=0
if command -v smatch >/dev/null 2>&1; then
  if [[ "${RUN_SMATCH}" != "no" ]]; then
    clean_module
    if make -C "${KERNELDIR}" M="${SRC_DIR}" C=1 CHECK="smatch -p=kernel" modules > "${smatch_log}" 2>&1; then
      smatch_warning_count="$(count_matches ' warn:' "${smatch_log}")"
      smatch_indent_warning_count="$(count_matches 'warn: inconsistent indenting' "${smatch_log}")"
      smatch_error_count="$(count_matches ' error:' "${smatch_log}")"
      smatch_actionable_warning_count=$((smatch_warning_count - smatch_indent_warning_count))
      if ((smatch_actionable_warning_count > 0 || smatch_error_count > 0)); then
        smatch_status="findings"
      elif ((smatch_indent_warning_count > 0)); then
        smatch_status="pass_with_style_warnings"
      else
        smatch_status="pass"
      fi
    else
      smatch_status="fail"
    fi
  fi
elif [[ "${RUN_SMATCH}" == "yes" ]]; then
  smatch_status="tool_missing"
  echo "smatch is required but not installed" > "${smatch_log}"
elif [[ "${RUN_SMATCH}" == "auto" ]]; then
  smatch_status="tool_missing_optional"
  echo "smatch not installed; compiler and requested analyzers were still executed" > "${smatch_log}"
fi

cocci_status="skipped"
if command -v spatch >/dev/null 2>&1; then
  if [[ "${RUN_COCCI}" != "no" ]]; then
    if make -C "${KERNELDIR}" M="${SRC_DIR}" coccicheck MODE=report > "${cocci_log}" 2>&1; then
      cocci_status="pass"
    else
      cocci_status="fail"
    fi
  fi
elif [[ "${RUN_COCCI}" == "yes" ]]; then
  cocci_status="tool_missing"
  echo "spatch/coccinelle is required but not installed" > "${cocci_log}"
elif [[ "${RUN_COCCI}" == "auto" ]]; then
  cocci_status="tool_missing_optional"
  echo "spatch/coccinelle not installed" > "${cocci_log}"
fi

{
  echo "timestamp_utc=$(date -u --iso-8601=seconds)"
  echo "kernel=$(uname -r)"
  echo "kernel_build_dir=${KERNELDIR}"
  echo "source_dir=${SRC_DIR}"
  echo "lock_file=${LOCK_FILE}"
  echo "build_status=${build_status}"
  echo "sparse_requested=${RUN_SPARSE}"
  echo "sparse_status=${sparse_status}"
  echo "sparse_warning_count=${sparse_warning_count}"
  echo "sparse_error_count=${sparse_error_count}"
  echo "smatch_requested=${RUN_SMATCH}"
  echo "smatch_status=${smatch_status}"
  echo "smatch_warning_count=${smatch_warning_count}"
  echo "smatch_indent_warning_count=${smatch_indent_warning_count}"
  echo "smatch_actionable_warning_count=${smatch_actionable_warning_count}"
  echo "smatch_error_count=${smatch_error_count}"
  echo "cocci_requested=${RUN_COCCI}"
  echo "cocci_status=${cocci_status}"
  echo "clean_log=${clean_log}"
  echo "build_log=${build_log}"
  echo "sparse_log=${sparse_log}"
  echo "smatch_log=${smatch_log}"
  echo "cocci_log=${cocci_log}"
  echo "outdir=${outdir}"
} > "${summary}"

cat "${summary}"
if [[ "${build_status}" != "pass" || "${sparse_status}" == "fail" || "${sparse_status}" == "findings" || "${sparse_status}" == "tool_missing" ||
      "${smatch_status}" == "fail" || "${smatch_status}" == "findings" || "${smatch_status}" == "tool_missing" ||
      "${cocci_status}" == "fail" || "${cocci_status}" == "tool_missing" ]]; then
  exit 1
fi
