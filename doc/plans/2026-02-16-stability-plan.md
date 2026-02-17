# Stability Plan Log (2026-02-16)

## Context
This plan captures the approved brainstorming before implementation. Scope is stability-first for HwsUHDX1Capture under active desktop usage (OBS/PipeWire), then performance.

## Primary goals
1. Eliminate crash / lockup classes and log storms.
2. Keep module selection deterministic across reboot (DKMS + dracut workflow).
3. Preserve 1080p60 reliability and improve 4k25 behavior after stability baseline is clean.

## Prioritized phases
1. Guardrails and observability (Step 0): benchmark preflight, module identity capture, run artifact integrity.
2. Single-stream ownership policy: allow multi-open, reject conflicting stream-affecting ioctls with -EBUSY.
3. Hard data-path safety checks: validate destination sizes before memcpy/scaler and fail buffer safely.
4. Logging hardening: remove/gate hot-path spam and ratelimit noisy unsupported-control messages.
5. Performance tuning after stability: worker budget/scaler-path tuning with diag deltas.

## Known risks to control
- Mixed module locations (`updates/` vs `updates/dkms`) causing wrong module after reboot.
- Stale initramfs causing old module preload.
- Concurrent client contention causing queue/format churn.
- Hot-path printk causing latency and log flood.

## Validation gates
- 3x 30s clean runs, then 3x 5m clean runs, then 60m clean run.
- Shared-attempt behavior must be controlled and predictable (`-EBUSY`, no instability).
- Post-reboot module identity must match:
  - `modinfo HwsUHDX1Capture` srcversion
  - `/sys/module/HwsUHDX1Capture/srcversion`

## Source references used during brainstorming
- V4L2 open and ownership behavior:
  - https://www.kernel.org/doc/html/latest/userspace-api/media/v4l/func-open.html
  - https://www.kernel.org/doc/html/v6.2/userspace-api/media/v4l/open.html
- Videobuf2 API:
  - https://www.kernel.org/doc/html/latest/driver-api/media/v4l2-videobuf2.html
- Kernel locking guidance:
  - https://www.kernel.org/doc/html/v6.13-rc2/kernel-hacking/locking.html
