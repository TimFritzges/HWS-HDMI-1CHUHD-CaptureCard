#!/usr/bin/env bash
set -euo pipefail

TARGET_HELPER="${TARGET_HELPER:-/usr/local/sbin/hws-obs-diag-priv}"
SUDOERS_FILE="${SUDOERS_FILE:-/etc/sudoers.d/hws-obs-diag}"
USER_NAME="${USER_NAME:-thecatgoesrawr}"
SRC_HELPER_DEFAULT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/obs-diag-priv.sh"
SRC_HELPER="${SRC_HELPER:-$SRC_HELPER_DEFAULT}"

usage() {
  cat <<'USAGE'
Usage: sudo ./tools/install-obs-diag-priv.sh [options]

Install privileged OBS diagnostics helper and a tight sudoers rule.

Options:
  --user <name>         User allowed to run helper without password
                        (default: thecatgoesrawr)
  --target <path>       Installed helper path
                        (default: /usr/local/sbin/hws-obs-diag-priv)
  --sudoers <path>      Sudoers include file path
                        (default: /etc/sudoers.d/hws-obs-diag)
  --source <path>       Source helper script path
                        (default: tools/obs-diag-priv.sh)
  -h, --help            Show help
USAGE
}

while (($#)); do
  case "$1" in
    --user) USER_NAME="${2:-}"; shift 2 ;;
    --target) TARGET_HELPER="${2:-}"; shift 2 ;;
    --sudoers) SUDOERS_FILE="${2:-}"; shift 2 ;;
    --source) SRC_HELPER="${2:-}"; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) echo "Unknown option: $1" >&2; exit 2 ;;
  esac
done

if [[ ${EUID} -ne 0 ]]; then
  echo "Run as root (sudo)." >&2
  exit 2
fi
if [[ ! -f "${SRC_HELPER}" ]]; then
  echo "Source helper not found: ${SRC_HELPER}" >&2
  exit 2
fi

install -o root -g root -m 0755 "${SRC_HELPER}" "${TARGET_HELPER}"

tmp="$(mktemp)"
cat > "${tmp}" <<EOF
# OBS diagnostics helper (auto-generated)
${USER_NAME} ALL=(root) NOPASSWD: ${TARGET_HELPER}
EOF
chmod 0440 "${tmp}"

if ! visudo -cf "${tmp}" >/dev/null; then
  echo "Generated sudoers validation failed." >&2
  rm -f "${tmp}"
  exit 1
fi

install -o root -g root -m 0440 "${tmp}" "${SUDOERS_FILE}"
rm -f "${tmp}"

if ! visudo -cf "${SUDOERS_FILE}" >/dev/null; then
  echo "Installed sudoers file failed validation: ${SUDOERS_FILE}" >&2
  exit 1
fi

echo "installed_helper=${TARGET_HELPER}"
echo "installed_sudoers=${SUDOERS_FILE}"
echo "allowed_user=${USER_NAME}"
echo "verify_cmd=sudo -n ${TARGET_HELPER} --self-test"
echo "trace_verify_cmd=sudo -n ${TARGET_HELPER} --trace-capable"
