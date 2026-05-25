#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC_DIR="${SRC_DIR:-${ROOT}/src}"
KERNELDIR="${KERNELDIR:-/lib/modules/$(uname -r)/build}"
OUT_BASE="${OUT_BASE:-${ROOT}/bench-results}"
RUN_SPARSE="${RUN_SPARSE:-auto}"
JOBS="${JOBS:-$(nproc)}"

usage() {
  cat <<'USAGE'
Usage: scripts/static-check.sh [options]

Non-privileged pre-install build validation for HwsUHDX1Capture.

Options:
  -k <kernel-build-dir>  Kernel build tree (default: running kernel)
  -o <out-base>          Artifact base directory (default: ./bench-results)
  -j <jobs>              Parallel build jobs (default: nproc)
  --sparse <auto|yes|no> Run sparse when installed or require/skip it
  -h                      Show help

This script does not install, unload, or load modules.
USAGE
}

while (($#)); do
  case "$1" in
    -k) KERNELDIR="${2:-}"; shift 2 ;;
    -o) OUT_BASE="${2:-}"; shift 2 ;;
    -j) JOBS="${2:-}"; shift 2 ;;
    --sparse) RUN_SPARSE="${2:-}"; shift 2 ;;
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
if [[ "${RUN_SPARSE}" != "auto" && "${RUN_SPARSE}" != "yes" && "${RUN_SPARSE}" != "no" ]]; then
  echo "Invalid --sparse value: ${RUN_SPARSE} (expected auto, yes, or no)" >&2
  exit 2
fi

stamp="$(date -u +%Y%m%d-%H%M%S)"
outdir="${OUT_BASE}/${stamp}-static-check"
mkdir -p "${outdir}"
build_log="${outdir}/module-build.log"
sparse_log="${outdir}/sparse.log"
summary="${outdir}/summary.env"

build_status="fail"
if make -C "${SRC_DIR}" KERNELDIR="${KERNELDIR}" -j"${JOBS}" > "${build_log}" 2>&1; then
  build_status="pass"
fi

sparse_status="skipped"
if command -v sparse >/dev/null 2>&1; then
  if [[ "${RUN_SPARSE}" != "no" ]]; then
    if make -C "${KERNELDIR}" M="${SRC_DIR}" C=1 CHECK=sparse modules > "${sparse_log}" 2>&1; then
      sparse_status="pass"
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

{
  echo "timestamp_utc=$(date -u --iso-8601=seconds)"
  echo "kernel=$(uname -r)"
  echo "kernel_build_dir=${KERNELDIR}"
  echo "source_dir=${SRC_DIR}"
  echo "build_status=${build_status}"
  echo "sparse_requested=${RUN_SPARSE}"
  echo "sparse_status=${sparse_status}"
  echo "build_log=${build_log}"
  echo "sparse_log=${sparse_log}"
  echo "outdir=${outdir}"
} > "${summary}"

cat "${summary}"
if [[ "${build_status}" != "pass" || "${sparse_status}" == "fail" || "${sparse_status}" == "tool_missing" ]]; then
  exit 1
fi
