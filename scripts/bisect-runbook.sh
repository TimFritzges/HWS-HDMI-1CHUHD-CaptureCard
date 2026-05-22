#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MODULE="${MODULE:-HwsUHDX1Capture}"
VERSION="${VERSION:-1.0.0.230324}"
KVER="${KVER:-$(uname -r)}"
PROFILE="${PROFILE:-${ROOT}/scripts/stability-gate.env}"

cat <<RUNBOOK
Bisect runbook (reboot-gated, no unload/reload assumption)

1) Start bisect
   git bisect start
   git bisect bad <known_bad>
   git bisect good <known_good>

2) For each commit checkout, run the versioned install/reboot flow as root:
   DKMS_SRC=/usr/src/${MODULE}-${VERSION}
   rm -rf "\${DKMS_SRC}"
   install -d "\${DKMS_SRC}"
   rsync -a --delete ${ROOT}/src/ "\${DKMS_SRC}/"
   dkms remove -m ${MODULE} -v ${VERSION} --all || true
   dkms add -m ${MODULE} -v ${VERSION}
   dkms build -m ${MODULE} -v ${VERSION} -k ${KVER}
   dkms install -m ${MODULE} -v ${VERSION} -k ${KVER} --force
   depmod -a
   dracut --force /boot/initramfs-linux-lts.img ${KVER}
   reboot

3) After reboot, evaluate commit:
   ${ROOT}/scripts/bisect-eval.sh --profile ${PROFILE}

4) Mark commit based on exit code:
   0   -> git bisect good
   1   -> git bisect bad
   125 -> git bisect skip

5) Finish:
   git bisect reset

Artifacts:
- Bench outputs: ${ROOT}/bench-results/
- Analyzer TSV: ${ROOT}/bench-results/bisect-eval-*.tsv
RUNBOOK
