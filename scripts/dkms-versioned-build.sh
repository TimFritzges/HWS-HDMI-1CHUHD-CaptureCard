#!/usr/bin/env bash
set -euo pipefail

MODULE_NAME="${MODULE_NAME:-HwsUHDX1Capture}"
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC_DIR_DEFAULT="${REPO_ROOT}/src"
LOG_ROOT_DEFAULT="${REPO_ROOT}/dkms-build-logs"

SRC_DIR="${SRC_DIR:-${SRC_DIR_DEFAULT}}"
LOG_ROOT="${LOG_ROOT:-${LOG_ROOT_DEFAULT}}"
KERNEL_MODE="current"
KERNEL_LIST_ARG=""
PRUNE_OTHERS=1
FORCE_REINSTALL=0
VERSION_OVERRIDE=""
NO_BUMP=0
DRACUT_MODE="none"
INITRD_PATH="${INITRD_PATH:-/boot/initramfs-linux-lts.img}"
DRACUT_STRICT=0
DRACUT_LAYOUT_USED="not-run"
CURRENT_KERNEL="$(uname -r)"

usage() {
  cat <<'USAGE'
Usage: scripts/dkms-versioned-build.sh [options]

Build/install a versioned DKMS source snapshot and log exactly what was built.

Options:
  --version <ver>        Use explicit DKMS version (no auto-bump).
  --no-bump              Use PACKAGE_VERSION from src/dkms.conf as-is.
  --kernels <mode>       current|all|k1,k2,... (default: current).
  --prune-others         Remove other DKMS versions of this module (activate only this version).
                         This is the default behavior.
  --keep-others          Do not remove other DKMS versions.
  --force-reinstall      Remove same module/version before add/build/install.
  --dracut-current       Run dracut for current kernel using --initrd path.
  --dracut-all           Rebuild initramfs for all selected kernels using the
                         detected host boot layout.
  --dracut-strict        Fail script if dracut step fails.
  --initrd <path>        Initramfs output path for --dracut-current
                         (default: /boot/initramfs-linux-lts.img).
  --src-dir <path>       Source directory to package (default: ./src).
  --log-root <path>      Log root directory (default: ./dkms-build-logs).
  -h, --help             Show this help.

Examples:
  sudo -E scripts/dkms-versioned-build.sh
  sudo -E scripts/dkms-versioned-build.sh --kernels all --prune-others
  sudo -E scripts/dkms-versioned-build.sh --kernels all --keep-others
  sudo -E scripts/dkms-versioned-build.sh --kernels all --prune-others --dracut-all
  sudo -E scripts/dkms-versioned-build.sh --version 1.0.0.230324.g1a2b3c4.r20260225T221500Z --kernels 6.12.73-1-lts
USAGE
}

while (($#)); do
  case "$1" in
    --version)
      VERSION_OVERRIDE="${2:-}"
      shift 2
      ;;
    --no-bump)
      NO_BUMP=1
      shift
      ;;
    --kernels)
      KERNEL_LIST_ARG="${2:-}"
      shift 2
      ;;
    --prune-others)
      PRUNE_OTHERS=1
      shift
      ;;
    --keep-others)
      PRUNE_OTHERS=0
      shift
      ;;
    --force-reinstall)
      FORCE_REINSTALL=1
      shift
      ;;
    --dracut-current)
      DRACUT_MODE="current"
      shift
      ;;
    --dracut-all)
      DRACUT_MODE="all"
      shift
      ;;
    --dracut-strict)
      DRACUT_STRICT=1
      shift
      ;;
    --initrd)
      INITRD_PATH="${2:-}"
      shift 2
      ;;
    --src-dir)
      SRC_DIR="${2:-}"
      shift 2
      ;;
    --log-root)
      LOG_ROOT="${2:-}"
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown option: $1" >&2
      usage
      exit 2
      ;;
  esac
done

if [[ ! -f "${SRC_DIR}/dkms.conf" ]]; then
  echo "Missing dkms.conf at: ${SRC_DIR}/dkms.conf" >&2
  exit 2
fi

if ! command -v dkms >/dev/null 2>&1; then
  echo "dkms not found in PATH" >&2
  exit 2
fi
if [[ "${DRACUT_MODE}" != "none" ]] && ! command -v dracut >/dev/null 2>&1; then
  echo "dracut not found in PATH (requested mode=${DRACUT_MODE})" >&2
  exit 2
fi

if [[ ${EUID} -ne 0 ]]; then
  echo "Run as root (recommended: sudo -E scripts/dkms-versioned-build.sh ...)" >&2
  exit 2
fi

run() {
  echo "+ $*"
  "$@"
}

have_legacy_boot_layout() {
  [[ -d /boot/grub ]] && compgen -G "/boot/initramfs-*.img" >/dev/null
}

kernel_pkgbase_for() {
  local kver="${1}"
  local pkgbase=""
  local pkgbase_file="/lib/modules/${kver}/pkgbase"
  local localversion_pkg="/lib/modules/${kver}/build/localversion.20-pkgname"

  if [[ -r "${pkgbase_file}" ]]; then
    pkgbase="$(tr -d '[:space:]' < "${pkgbase_file}")"
    if [[ -n "${pkgbase}" ]]; then
      echo "${pkgbase}"
      return 0
    fi
  fi

  if [[ -r "${localversion_pkg}" ]]; then
    pkgbase="$(tr -d '\r\n' < "${localversion_pkg}")"
    pkgbase="${pkgbase#-}"
    if [[ -n "${pkgbase}" ]]; then
      echo "${pkgbase}"
      return 0
    fi
  fi

  case "${kver}" in
    *-arch*-znver3|*-znver3)
      pkgbase="linux-znver3"
      ;;
    *-zen*-zen|*-zen)
      pkgbase="linux-zen"
      ;;
    *-cachyos-lts)
      pkgbase="linux-cachyos-lts"
      ;;
    *-cachyos)
      pkgbase="linux-cachyos"
      ;;
    *-rt-lts)
      pkgbase="linux-rt-lts"
      ;;
    *-rt)
      pkgbase="linux-rt"
      ;;
    *-lts)
      pkgbase="linux-lts"
      ;;
    *-arch*)
      pkgbase="linux"
      ;;
  esac

  if [[ -n "${pkgbase}" ]]; then
    echo "${pkgbase}"
    return 0
  fi

  return 1
}

kernel_initrd_path_for() {
  local kver="${1}"
  local pkgbase=""

  pkgbase="$(kernel_pkgbase_for "${kver}" 2>/dev/null || true)"
  [[ -n "${pkgbase}" ]] || return 1
  echo "/boot/initramfs-${pkgbase}.img"
}

run_dracut_all_for_selected_kernels() {
  local rc=0
  local kver=""
  local initrd=""
  local dracut_layout="unknown"

  if have_legacy_boot_layout; then
    dracut_layout="legacy-boot"
    DRACUT_LAYOUT_USED="${dracut_layout}"
    echo "dracut_layout=${dracut_layout}"
    for kver in "${TARGET_KERNELS[@]}"; do
      initrd="$(kernel_initrd_path_for "${kver}" || true)"
      if [[ -z "${initrd}" ]]; then
        echo "warning: could not determine initramfs path for kernel ${kver}"
        rc=7
        continue
      fi
      echo "+ dracut --force ${initrd} ${kver}"
      dracut --force "${initrd}" "${kver}" || rc=$?
    done
    return "${rc}"
  fi

  dracut_layout="kernel-install"
  DRACUT_LAYOUT_USED="${dracut_layout}"
  echo "dracut_layout=${dracut_layout}"
  dracut --regenerate-all --force || rc=$?
  return "${rc}"
}

read_base_version() {
  awk -F'"' '/^PACKAGE_VERSION=/{print $2; exit}' "${SRC_DIR}/dkms.conf"
}

BASE_VERSION="$(read_base_version)"
if [[ -z "${BASE_VERSION}" ]]; then
  echo "Could not parse PACKAGE_VERSION from ${SRC_DIR}/dkms.conf" >&2
  exit 2
fi

GIT_HEAD="unknown"
GIT_DIRTY="0"
if git -C "${REPO_ROOT}" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
  GIT_HEAD="$(git -C "${REPO_ROOT}" rev-parse --short HEAD 2>/dev/null || echo "unknown")"
  GIT_DIRTY="$(git -C "${REPO_ROOT}" status --porcelain 2>/dev/null | wc -l | tr -d ' ')"
fi

STAMP_UTC="$(date -u +%Y%m%dT%H%M%SZ)"
if [[ -n "${VERSION_OVERRIDE}" ]]; then
  DKMS_VERSION="${VERSION_OVERRIDE}"
elif [[ "${NO_BUMP}" -eq 1 ]]; then
  DKMS_VERSION="${BASE_VERSION}"
else
  source_tag="nogit"
  if [[ "${GIT_HEAD}" != "unknown" ]]; then
    source_tag="g${GIT_HEAD}"
    if [[ "${GIT_DIRTY}" != "0" ]]; then
      source_tag="${source_tag}.dirty${GIT_DIRTY}"
    fi
  fi
  DKMS_VERSION="${BASE_VERSION}.${source_tag}.r${STAMP_UTC}"
fi

if [[ -n "${KERNEL_LIST_ARG}" ]]; then
  KERNEL_MODE="${KERNEL_LIST_ARG}"
fi

select_kernels() {
  local mode="${1}"
  if [[ "${mode}" == "current" ]]; then
    uname -r
    return 0
  fi
  if [[ "${mode}" == "all" ]]; then
    local k
    for k in /lib/modules/*; do
      [[ -d "${k}" ]] || continue
      [[ -d "${k}/build" ]] || continue
      basename "${k}"
    done
    return 0
  fi
  # comma-separated explicit list
  echo "${mode}" | tr ',' '\n' | sed '/^$/d'
}

mapfile -t TARGET_KERNELS < <(select_kernels "${KERNEL_MODE}")
if [[ ${#TARGET_KERNELS[@]} -eq 0 ]]; then
  echo "No kernels selected (mode=${KERNEL_MODE})" >&2
  exit 2
fi

RUN_ID="$(date -u +%Y%m%d-%H%M%S)-${DKMS_VERSION//[^A-Za-z0-9._-]/_}"
RUN_DIR="${LOG_ROOT}/${RUN_ID}"
mkdir -p "${RUN_DIR}"
RUN_LOG="${RUN_DIR}/run.log"
SUMMARY="${RUN_DIR}/summary.env"
HISTORY="${LOG_ROOT}/history.tsv"
exec > >(tee -a "${RUN_LOG}") 2>&1

echo "run_id=${RUN_ID}"
echo "repo_root=${REPO_ROOT}"
echo "src_dir=${SRC_DIR}"
echo "dkms_version=${DKMS_VERSION}"
echo "kernel_mode=${KERNEL_MODE}"
echo "target_kernels=${TARGET_KERNELS[*]}"
echo "dracut_mode=${DRACUT_MODE}"
echo "dracut_strict=${DRACUT_STRICT}"
echo "initrd_path=${INITRD_PATH}"
echo "current_kernel=${CURRENT_KERNEL}"

run dkms status | tee "${RUN_DIR}/dkms-status.before.txt" >/dev/null

DKMS_SRC="/usr/src/${MODULE_NAME}-${DKMS_VERSION}"
EXISTING_VERSION=0
if dkms status | grep -q "^${MODULE_NAME}/${DKMS_VERSION},"; then
  EXISTING_VERSION=1
fi

if [[ "${FORCE_REINSTALL}" -eq 1 || "${EXISTING_VERSION}" -eq 1 ]]; then
  run dkms remove -m "${MODULE_NAME}" -v "${DKMS_VERSION}" --all || true
fi

run install -d "${DKMS_SRC}"
run rsync -a --delete \
  --exclude '*.o' \
  --exclude '*.ko' \
  --exclude '*.mod' \
  --exclude '*.mod.c' \
  --exclude '*.cmd' \
  --exclude 'Module.symvers' \
  --exclude 'modules.order' \
  "${SRC_DIR}/" "${DKMS_SRC}/"

run sed -i "s/^PACKAGE_VERSION=.*/PACKAGE_VERSION=\"${DKMS_VERSION}\"/" "${DKMS_SRC}/dkms.conf"
run sed -i "s/^AUTOINSTALL=.*/AUTOINSTALL=\"yes\"/" "${DKMS_SRC}/dkms.conf"

{
  echo "module_name=${MODULE_NAME}"
  echo "dkms_version=${DKMS_VERSION}"
  echo "created_utc=$(date -u --iso-8601=seconds)"
  echo "repo_root=${REPO_ROOT}"
  echo "git_head=${GIT_HEAD}"
  echo "git_dirty_file_count=${GIT_DIRTY}"
} > "${DKMS_SRC}/.hws_build_meta"

run dkms add -m "${MODULE_NAME}" -v "${DKMS_VERSION}"

list_module_versions() {
  dkms status | awk -v mod="${MODULE_NAME}" '
    {
      split($1, a, "/");
      if (a[1] != mod) next;
      v = a[2];
      sub(/[:,].*$/, "", v);
      if (v != "") print v;
    }' | sort -u
}

if [[ "${PRUNE_OTHERS}" -eq 1 ]]; then
  mapfile -t OTHER_VERSIONS < <(
    list_module_versions | awk -v keep="${DKMS_VERSION}" '
      $1 != keep { print $1 }' | sort -u
  )
  for oldv in "${OTHER_VERSIONS[@]:-}"; do
    [[ -n "${oldv}" ]] || continue
    run dkms remove -m "${MODULE_NAME}" -v "${oldv}" --all || true
  done
fi

BUILD_RC=0
CURRENT_KERNEL_INSTALLED=0
declare -a SUCCEEDED_KERNELS=()
declare -a FAILED_KERNELS=()

for kver in "${TARGET_KERNELS[@]}"; do
  if run dkms build -m "${MODULE_NAME}" -v "${DKMS_VERSION}" -k "${kver}" &&
     run dkms install -m "${MODULE_NAME}" -v "${DKMS_VERSION}" -k "${kver}" --force; then
    SUCCEEDED_KERNELS+=("${kver}")
    if [[ "${kver}" == "${CURRENT_KERNEL}" ]]; then
      CURRENT_KERNEL_INSTALLED=1
    fi
  else
    echo "warning: dkms build/install failed for kernel ${kver}"
    FAILED_KERNELS+=("${kver}")
    BUILD_RC=1
  fi
done

if [[ ${#SUCCEEDED_KERNELS[@]} -eq 0 ]]; then
  echo "error: dkms build/install failed for all selected kernels"
  exit 1
fi

run depmod -a

if [[ "${DRACUT_MODE}" == "all" ]]; then
  run_dracut_all_for_selected_kernels || dracut_rc=$?
elif [[ "${DRACUT_MODE}" == "current" ]]; then
  if [[ "${CURRENT_KERNEL_INSTALLED}" == "1" ]]; then
    dracut --force "${INITRD_PATH}" "${CURRENT_KERNEL}" || dracut_rc=$?
  else
    echo "warning: skipping dracut for current kernel ${CURRENT_KERNEL} because its dkms build/install failed"
    dracut_rc=8
  fi
fi
dracut_rc="${dracut_rc:-0}"
if [[ "${dracut_rc}" != "0" ]]; then
  echo "warning: dracut step failed with exit code ${dracut_rc}"
fi

run dkms status | tee "${RUN_DIR}/dkms-status.after.txt" >/dev/null

MODINFO_FILE="${RUN_DIR}/modinfo.current-kernel.txt"
if modinfo "${MODULE_NAME}" >/dev/null 2>&1; then
  run modinfo "${MODULE_NAME}" | tee "${MODINFO_FILE}" >/dev/null
fi

RUNTIME_SRCVERSION="unavailable"
if [[ -r "/sys/module/${MODULE_NAME}/srcversion" ]]; then
  RUNTIME_SRCVERSION="$(cat "/sys/module/${MODULE_NAME}/srcversion" 2>/dev/null || echo "unavailable")"
fi

MODINFO_SRCVERSION="$(modinfo "${MODULE_NAME}" 2>/dev/null | awk -F': *' '/^srcversion/{print $2; exit}')"
MODINFO_FILENAME="$(modinfo "${MODULE_NAME}" 2>/dev/null | awk -F': *' '/^filename/{print $2; exit}')"

{
  echo "run_id=${RUN_ID}"
  echo "timestamp_utc=$(date -u --iso-8601=seconds)"
  echo "module_name=${MODULE_NAME}"
  echo "dkms_version=${DKMS_VERSION}"
  echo "kernel_mode=${KERNEL_MODE}"
  echo "target_kernels=${TARGET_KERNELS[*]}"
  echo "current_kernel=${CURRENT_KERNEL}"
  echo "succeeded_kernels=${SUCCEEDED_KERNELS[*]}"
  echo "failed_kernels=${FAILED_KERNELS[*]}"
  echo "build_exit_code=${BUILD_RC}"
  echo "current_kernel_installed=${CURRENT_KERNEL_INSTALLED}"
  echo "dracut_mode=${DRACUT_MODE}"
  echo "dracut_layout=${DRACUT_LAYOUT_USED}"
  echo "dracut_strict=${DRACUT_STRICT}"
  echo "dracut_exit_code=${dracut_rc}"
  echo "initrd_path=${INITRD_PATH}"
  echo "prune_others=${PRUNE_OTHERS}"
  echo "force_reinstall=${FORCE_REINSTALL}"
  echo "existing_version_before=${EXISTING_VERSION}"
  echo "repo_root=${REPO_ROOT}"
  echo "src_dir=${SRC_DIR}"
  echo "dkms_src=${DKMS_SRC}"
  echo "git_head=${GIT_HEAD}"
  echo "git_dirty_file_count=${GIT_DIRTY}"
  echo "modinfo_filename=${MODINFO_FILENAME:-unavailable}"
  echo "modinfo_srcversion=${MODINFO_SRCVERSION:-unavailable}"
  echo "runtime_srcversion=${RUNTIME_SRCVERSION}"
  echo "run_log=${RUN_LOG}"
  echo "dkms_status_before=${RUN_DIR}/dkms-status.before.txt"
  echo "dkms_status_after=${RUN_DIR}/dkms-status.after.txt"
} | tee "${SUMMARY}"

if [[ ! -f "${HISTORY}" ]]; then
  echo -e "timestamp_utc\trun_id\tmodule\tversion\tkernel_mode\ttarget_kernels\tcurrent_kernel\tsucceeded_kernels\tfailed_kernels\tbuild_exit_code\tdracut_mode\tdracut_layout\tdracut_strict\tdracut_exit_code\tinitrd_path\tprune_others\tforce_reinstall\texisting_before\tgit_head\tgit_dirty\tmodinfo_srcversion\truntime_srcversion\trun_dir" > "${HISTORY}"
fi
echo -e "$(date -u --iso-8601=seconds)\t${RUN_ID}\t${MODULE_NAME}\t${DKMS_VERSION}\t${KERNEL_MODE}\t${TARGET_KERNELS[*]}\t${CURRENT_KERNEL}\t${SUCCEEDED_KERNELS[*]}\t${FAILED_KERNELS[*]}\t${BUILD_RC}\t${DRACUT_MODE}\t${DRACUT_LAYOUT_USED}\t${DRACUT_STRICT}\t${dracut_rc}\t${INITRD_PATH}\t${PRUNE_OTHERS}\t${FORCE_REINSTALL}\t${EXISTING_VERSION}\t${GIT_HEAD}\t${GIT_DIRTY}\t${MODINFO_SRCVERSION:-unavailable}\t${RUNTIME_SRCVERSION}\t${RUN_DIR}" >> "${HISTORY}"

OVERALL_RC=0
if [[ "${BUILD_RC}" != "0" ]]; then
  OVERALL_RC="${BUILD_RC}"
fi
if [[ "${dracut_rc}" != "0" && "${DRACUT_STRICT}" == "1" ]]; then
  OVERALL_RC="${dracut_rc}"
fi

if [[ "${OVERALL_RC}" == "0" ]]; then
  echo "done=1"
else
  echo "done=0"
fi
if [[ "${DRACUT_MODE}" == "none" ]]; then
  echo "next=Rebuild initramfs with dracut for the running kernel, then reboot."
elif [[ "${CURRENT_KERNEL_INSTALLED}" == "1" ]]; then
  echo "next=current-kernel dracut step executed; reboot when ready."
else
  echo "next=current kernel was not installed successfully; fix build failures before reboot."
fi

if [[ "${OVERALL_RC}" != "0" ]]; then
  exit "${OVERALL_RC}"
fi
