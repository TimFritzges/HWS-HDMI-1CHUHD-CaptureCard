# AGENTS.md — HwsUHDX1Capture maintenance rules

## Mission
Maintain the `HwsUHDX1Capture` Linux kernel module with a focus on:
1) Deterministic, stable DKMS + Arch/Garuda packaging (no moving `-git` target unless explicitly requested).
2) Diagnosing and improving performance consistency vs. prior manual installs.

## Non-negotiable constraints
- Do NOT change the upstream revision / source version unless explicitly told.
- Do NOT run `sudo` (or scripts that auto-escalate) unless explicitly approved by the user.
  - You may *suggest* sudo commands, but do not execute them.
- Every change must include:
  - Build steps (commands)
  - Test steps (commands + expected outputs)
  - Rollback steps
- Preserve the LTS kernel as a fallback environment. Do not build/install this
  driver into, or regenerate initramfs for, LTS unless the user explicitly
  requests that specific LTS operation.
- Treat live unload/reload as unavailable on this host because PipeWire holds
  the device. Installed-driver validation is reboot-gated.

## Repo map (quick)
- `src/` — kernel module source + `Makefile` + `dkms.conf`
- `scripts/` — installer/uninstaller helpers (may use sudo; treat as privileged)
- `scripts/postboot-diagnose.sh` — autonomous post-reboot validation suite
- `tools/collect-evidence.sh` — non-privileged runtime evidence snapshot helper
- `tools/obs-diagnose.sh` — OBS launch wrapper with automatic evidence capture
- `play/` — smoke tests (GStreamer/xawtv helpers)
- top-level `install.sh`, `dkms-install.sh`, `uninstall.sh` — wrappers into `scripts/`

## Required deliverables (in order)
A) `scripts/bench.sh`
- Runs a 60s capture benchmark and writes an artifact directory with:
  - `v4l2-ctl --all` output
  - capture stats via `ffmpeg` (or `v4l2-ctl --stream-*`) for a fixed duration
  - `journalctl -k -b` filtered for: `HwsUHDX1Capture|videobuf2|dma|timeout|reset|error`
- Must be safe-by-default (no installs, no unloads, no destructive actions).
- Accept configuration via env vars/flags (device path, duration, output dir).
- Must collect `v4l2-compliance`, pre/post HWS/PipeWire state snapshots, and
  profiling artifacts when the respective installed tools are available.
- Streaming probes must be time-bounded and record explicit timeout status;
  never let a stalled driver block completion of the diagnostic artifact.

B) Fix `src/dkms.conf`
- Remove deprecated features such as `CLEAN` / `REMAKE_INITRD`.
- Ensure DKMS names align (`PACKAGE_NAME`, built module name).
- Install destination must be `/updates/dkms` so DKMS wins over stray manual modules.

C) Provide a non-git PKGBUILD
- Package a pinned source (specific commit or local snapshot tarball).
- Document how to rebuild for multiple kernels and how to rollback.

D) If performance differs across versions
- Propose a bisect plan using `scripts/bench.sh` as pass/fail.
- Only run bisect steps after user approval (can be time-consuming).

## PR/commit hygiene
- Small, scoped commits; imperative subjects.
- PR description must include: what/why, tested kernel(s), bench output, rollback.

## Known-good DKMS update flow (updated 2026-05-25)
Use the versioned script while booted into the intended non-LTS test kernel.
It detects the appropriate current initramfs target, logs the source identity,
and avoids the prior hard-coded LTS path hazard.

- Pre-install command:
  - `scripts/static-check.sh --sparse yes --smatch yes`
- Installation commands for the user to run:
  - `uname -r` and confirm this is the non-LTS test kernel.
  - `sudo -E ./scripts/dkms-versioned-build.sh --kernels current --prune-others --dracut-current`
  - `sudo reboot`
- Post-reboot checks:
  - `modinfo HwsUHDX1Capture | rg -n "filename|srcversion|diag_enable"`
  - `/usr/bin/cat /sys/module/HwsUHDX1Capture/srcversion`
  - `/usr/bin/cat /sys/module/HwsUHDX1Capture/parameters/diag_enable`
  - `scripts/postboot-diagnose.sh -d /dev/video0 -t 60 -g installed-check`
  - Expect `modinfo` and `/sys/module/.../srcversion` to match.

Notes:
- If shell `cat` is aliased to `bat`, use `/usr/bin/cat`.

## Initramfs policy (important)
- Do not assume `mkinitcpio` or `dracut`; verify the host first.
- On this machine, prefer `dracut` as the default workflow.
- For routine testing, use `scripts/dkms-versioned-build.sh --kernels current
  --dracut-current` only while booted into the non-LTS test kernel.
- If a host is explicitly mkinitcpio-managed, use:
  - `sudo mkinitcpio -P`
- Always verify post-reboot that loaded module matches on-disk module:
  - `modinfo HwsUHDX1Capture | rg -n "srcversion|filename"`
  - `/usr/bin/cat /sys/module/HwsUHDX1Capture/srcversion`
  - These must match.

## New diagnostics abilities (2026-03-07)
- New module param:
  - `audio_trace_enable` (0/1) enables trace hooks for audio drop/silence paths.
- Runtime toggle commands:
  - `echo 1 | sudo tee /sys/module/HwsUHDX1Capture/parameters/diag_enable`
  - `echo 0 | sudo tee /sys/module/HwsUHDX1Capture/parameters/diag_enable`
  - `echo 1 | sudo tee /sys/module/HwsUHDX1Capture/parameters/audio_trace_enable`
  - `echo 0 | sudo tee /sys/module/HwsUHDX1Capture/parameters/audio_trace_enable`
- Audio diagnostics now expose root-cause counters in debugfs:
  - `sudo /usr/bin/cat /sys/kernel/debug/hwsuhdx1/audio_diag`
  - Includes `no_video_silence_injects`, `fallback_silence_injects`, `no_free_queue_slots`,
    `memcopy_failures`, `bad_packet_sizes`, `workqueue_requeues`, `stream_not_running`,
    `timer_silence_injects`, `timer_runs`.
- Audio diagnostics now expose timing and queue pressure metrics:
  - Latency chains: `irq_to_copy_*`, `copy_to_deliver_*`, `irq_to_deliver_*`.
  - Queue pressure: `queue_free_slots_min/max/total/samples`.
- Audio clock behavior added after Linux 7.0 compatibility work:
  - ALSA capture timing is driven by the PCM period timer while `pcm_running=1`.
  - Hardware audio packets are staged; the timer publishes one ALSA period per tick.
  - Missing or late hardware/source packets are converted to silence periods instead
    of stopping ALSA pointer advancement.
  - New counters include `real_periods`, `silence_periods`, `underrun_periods`,
    `source_lost_periods`, `staged_overruns`, `staged_bytes_dropped`,
    `timer_late_events`, `timer_late_ns_max`, `pcm_running`,
    `staged_fill_bytes_min`, and `staged_fill_bytes_max`.
- Trace lines when `audio_trace_enable=1`:
  - `hws_audio_drop ... reason=bad_packet|no_free_queue|memcopy_fail|stream_not_running`
  - `hws_audio_silence ... reason=no_video|fallback|timer|source_lost|underrun`
- Video producer diagnostics include `producer_slot_recoveries`,
  `producer_stale_frame_reclaims`, and `producer_no_free_slots`. A recovery
  indicates the DMA writer avoided a retained-frame slot; a stale-frame
  reclaim indicates source cadence outran delivery without violating frame
  immutability; any no-free event fails the stability gate.

## Autonomous evidence workflow (2026-05-25)
- Installed tools used automatically: `v4l-utils`, `ffmpeg`, `pipewire`,
  `wireplumber`, `alsa-utils`, `sparse`, `trace-cmd`, and `perf`.
- Installed tools available for focused manual investigation: `kernelshark`,
  `bpftrace`, `makedumpfile`, and `kexec-tools`. Do not enable crash-dump or
  tracing changes that affect boot without explicit user approval.
- `scripts/postboot-diagnose.sh` is the default isolated post-reboot gate. Run
  it only while OBS is closed because it opens the V4L2 node.
- `scripts/static-check.sh --sparse yes --smatch yes` must report
  `sparse_status=pass`, `smatch_actionable_warning_count=0`, and
  `smatch_error_count=0`; `smatch_status=pass_with_style_warnings` is allowed
  only for the existing indentation-only backlog.
- Do not bypass the `static-check.sh` `flock` lock: compiler/sparse/smatch/
  coccinelle passes share Kbuild outputs and concurrent runs invalidate results.
- `tools/obs-diagnose.sh` is already used by the user's OBS desktop launcher.
  It captures complete snapshots, PipeWire timing, `perf` statistics, driver
  counters, automatic audio trace lines, and fixed-event `trace-cmd` output.
  It must stop collector process groups immediately after OBS exits and bound
  post-run probes so it cannot leave diagnostic monitors running.
- Tracefs is not user-readable on this host. The source helper
  `tools/obs-diag-priv.sh` exposes only fixed diagnostic modes and must be
  reinstalled by the user after helper changes:
  - `sudo ./tools/install-obs-diag-priv.sh --user thecatgoesrawr`
  - `sudo -n /usr/local/sbin/hws-obs-diag-priv --trace-capable`
- Higher-overhead OBS function tracing is opt-in via
  `KERNEL_TRACE_FUNCTIONS=1`; default automatic trace collection records
  scheduler, IRQ, timer, and workqueue events only, bounded by
  `KERNEL_TRACE_SECONDS=30` unless a focused run overrides it.
