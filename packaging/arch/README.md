# Arch Packaging Skeleton

This folder contains local packaging skeletons for deterministic installs.

## Packages
- `hws-uhdx1capture-dkms`: installs only DKMS source into `/usr/src/HwsUHDX1Capture-<version>`.
- `hws-uhdx1capture-tools`: installs helper scripts/docs (no kernel module installation side effects).

## Deterministic Build Flow
1. Pin a source commit in `hws-uhdx1capture-dkms/PKGBUILD` via `_commit`.
2. Keep `pkgver` aligned with the source snapshot/version you are pinning.
3. Build and install with `makepkg -si` from each package directory.
4. Rebuild DKMS for all installed kernels (no source drift):
   - `dkms status | rg HwsUHDX1Capture`
   - `sudo dkms autoinstall -m HwsUHDX1Capture -v <pkgver>`
5. Rebuild initramfs using host policy:
   - Preferred on this host: `sudo dracut --force /boot/initramfs-linux-lts.img \"$(uname -r)\"`
   - mkinitcpio hosts: `sudo mkinitcpio -P`
6. Verify loaded module identity after reboot:
   - `modinfo HwsUHDX1Capture | rg -n \"srcversion|filename\"`
   - `/usr/bin/cat /sys/module/HwsUHDX1Capture/srcversion`
   - Values must match.

## Rollback
1. Install previous pinned package version (`pkgver`/`_commit` pair).
2. Reinstall DKMS snapshot:
   - `sudo dkms remove -m HwsUHDX1Capture -v <bad-version> --all || true`
   - `sudo dkms add -m HwsUHDX1Capture -v <good-version>`
   - `sudo dkms build -m HwsUHDX1Capture -v <good-version> -k \"$(uname -r)\"`
   - `sudo dkms install -m HwsUHDX1Capture -v <good-version> -k \"$(uname -r)\" --force`
3. Rebuild initramfs and reboot.

## Reboot-Gated Checkpoints (No-Unload Environments)
- Use `scripts/checkpoint-run.sh` when userspace (for example PipeWire) prevents safe module unload/reload.
- The script:
  - runs a short validation bench (`scripts/bench.sh`) with tagged artifacts,
  - emits privileged DKMS/install/reboot commands into `dkms-commands.txt`,
  - records module identity and stability score output for checkpoint comparisons.
- For closed-loop boot flow:
  - pre-reboot: `scripts/checkpoint-run.sh --prepare-reboot`
  - post-reboot: `scripts/checkpoint-postboot.sh`

## Regression Gate + Bisect Helpers
- Gate profile: `scripts/stability-gate.env`
- Analyzer honors profile via `scripts/analyze-stability-artifacts.sh -p <profile>`
- Bisect evaluation helper:
  - `scripts/bisect-eval.sh` returns `0=good`, `1=bad`, `125=untestable`
  - `scripts/bisect-runbook.sh` prints deterministic reboot-gated bisect steps.

## Notes
- `sha256sums=('SKIP')` is intentionally left as a local-dev default. Replace with real checksums for shared/distributed packages.
- The DKMS package relies on system DKMS hooks to build for installed kernels.
