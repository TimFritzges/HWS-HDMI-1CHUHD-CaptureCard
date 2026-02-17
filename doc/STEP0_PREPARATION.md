# Step 0 Preparation: Guardrails and Observability

This step is non-invasive: it does not unload modules, install packages, or change runtime state.

## What changed
- `tools/bench.sh` now writes preflight artifacts before running capture:
  - `<run>-preflight.txt`
  - `<run>-preflight.warnings.txt`
- Preflight checks include:
  - module loaded
  - device exists/readable
  - `modinfo` vs runtime `/sys/module/.../srcversion` match
  - diag parameter path readability
  - debugfs diag file readability
  - holder sanity vs selected concurrent mode
- `history-v4.csv` now includes:
  - `preflight_status`
  - `preflight_warning_count`

## Run
```bash
cd /home/thecatgoesrawr/src/HWS-HDMI-1CHUHD-CaptureCard
sudo -E ./tools/bench.sh -t 30 -g step0-check -c auto
```

## Verify expected output
- Summary contains:
  - `preflight_status=ok` (or `warn` with reason file)
  - `preflight_warning_count=0` for a clean run
- Artifacts exist:
  - `bench-results/<run>/<run>-preflight.txt`
  - `bench-results/<run>/<run>-preflight.warnings.txt`

## If preflight warns
- Inspect warning file first.
- Common fixes:
  - srcversion mismatch: verify DKMS + initramfs flow.
  - diag unreadable: check debugfs mount and permissions.
  - unexpected holders in solo mode: rerun with `-c shared`.

## Rollback
- Restore previous benchmark script:
```bash
cd /home/thecatgoesrawr/src/HWS-HDMI-1CHUHD-CaptureCard
git checkout -- tools/bench.sh packaging/arch/hws-uhdx1capture-tools/bench.sh doc/STEP0_PREPARATION.md
```
