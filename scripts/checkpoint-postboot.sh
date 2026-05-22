#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PENDING_FILE="${PENDING_FILE:-/var/tmp/hws-checkpoint-pending.env}"
FORCE_STALE="${FORCE_STALE:-0}"

while (($#)); do
  case "$1" in
    --pending-file) PENDING_FILE="${2:-}"; shift 2 ;;
    --force-stale) FORCE_STALE=1; shift ;;
    -h|--help)
      cat <<'USAGE'
Usage: scripts/checkpoint-postboot.sh [options]
  --pending-file PATH  Pending marker path (default /var/tmp/hws-checkpoint-pending.env)
  --force-stale        Ignore stale marker age
USAGE
      exit 0
      ;;
    *) echo "Unknown option: $1" >&2; exit 2 ;;
  esac
done

args=(--postboot-run --pending-file "${PENDING_FILE}")
if [[ "${FORCE_STALE}" == "1" ]]; then
  args+=(--force-stale)
fi
"${ROOT}/scripts/checkpoint-run.sh" "${args[@]}"
