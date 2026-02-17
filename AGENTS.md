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

## Repo map (quick)
- `src/` — kernel module source + `Makefile` + `dkms.conf`
- `scripts/` — installer/uninstaller helpers (may use sudo; treat as privileged)
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

## Known-good DKMS update flow (validated 2026-02-15)
Use this method exactly to avoid path split mistakes and stale initramfs loads.

- Variables:
  - `KVER="$(uname -r)"`
  - `SRC="/home/thecatgoesrawr/src/HWS-HDMI-1CHUHD-CaptureCard/src"`
  - `DKMS_SRC="/usr/src/HwsUHDX1Capture-1.0.0.230324"`
  - `INITRD="/boot/initramfs-linux-lts.img"`
- Commands:
  - `sudo rm -rf "$DKMS_SRC"`
  - `sudo install -d "$DKMS_SRC"`
  - `sudo rsync -a --delete "$SRC/" "$DKMS_SRC/"`
  - `sudo dkms remove -m HwsUHDX1Capture -v 1.0.0.230324 --all || true`
  - `sudo dkms add -m HwsUHDX1Capture -v 1.0.0.230324`
  - `sudo dkms build -m HwsUHDX1Capture -v 1.0.0.230324 -k "$KVER"`
  - `sudo dkms install -m HwsUHDX1Capture -v 1.0.0.230324 -k "$KVER" --force`
  - `sudo depmod -a`
  - `echo 'options HwsUHDX1Capture diag_enable=0' | sudo tee /etc/modprobe.d/hwsuhdx1capture.conf`
  - `sudo dracut --force "$INITRD" "$KVER"`
  - `sudo reboot`
- Post-reboot checks:
  - `modinfo HwsUHDX1Capture | rg -n "filename|srcversion|diag_enable"`
  - `/usr/bin/cat /sys/module/HwsUHDX1Capture/srcversion`
  - `/usr/bin/cat /sys/module/HwsUHDX1Capture/parameters/diag_enable`
  - Expect `modinfo` and `/sys/module/.../srcversion` to match.

Notes:
- Keep `rsync` destination on the same command line: `"$DKMS_SRC/"`.
- If shell `cat` is aliased to `bat`, use `/usr/bin/cat`.

## Initramfs policy (important)
- Do not assume `mkinitcpio` or `dracut`; verify the host first.
- On this machine, prefer `dracut` as the default workflow.
- After DKMS install, rebuild initramfs for the running kernel:
  - `sudo dracut --force "/boot/initramfs-linux-lts.img" "$(uname -r)"`
- If a host is explicitly mkinitcpio-managed, use:
  - `sudo mkinitcpio -P`
- Always verify post-reboot that loaded module matches on-disk module:
  - `modinfo HwsUHDX1Capture | rg -n "srcversion|filename"`
  - `/usr/bin/cat /sys/module/HwsUHDX1Capture/srcversion`
  - These must match.
