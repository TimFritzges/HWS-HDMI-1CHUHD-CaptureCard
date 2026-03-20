# DKMS Versioned Build Workflow

This workflow creates a unique DKMS module version per test run, logs what was built, and can make one version the active auto-rebuild target.

## Script

`scripts/dkms-versioned-build.sh`

## What it does

- Creates DKMS version from source identity + build stamp:
  - `PACKAGE_VERSION.g<git-short-hash>.rYYYYMMDDTHHMMSSZ`
  - dirty working tree: `PACKAGE_VERSION.g<hash>.dirty<filecount>.rYYYYMMDDTHHMMSSZ`
  - unless overridden via `--version`.
- Copies `src/` into `/usr/src/HwsUHDX1Capture-<version>`.
- Rewrites copied `dkms.conf` to that exact `PACKAGE_VERSION`.
- Runs `dkms add/build/install` for selected kernels.
- Stores full logs and summary in `dkms-build-logs/<run-id>/`.
- Appends history row in `dkms-build-logs/history.tsv`.
- By default, old DKMS versions are pruned so kernel updates rebuild only one active version.
  Use `--keep-others` to opt out.
- `--prune-others` now prunes by scanning all three places that matter for future pacman hooks:
  - `dkms status`
  - `/var/lib/dkms/HwsUHDX1Capture/`
  - `/usr/src/HwsUHDX1Capture-*`
  This removes stale versions that were no longer visible enough through `dkms status` alone
  but still got picked up later by Garuda's DKMS post-transaction hook.
- `--dracut-all` now detects the host boot layout:
  - classic GRUB-style `/boot/initramfs-*.img` hosts: rebuilds each selected kernel explicitly via
    `dracut --force /boot/initramfs-<pkgbase>.img <kver>`
  - kernel-install style hosts: falls back to `dracut --regenerate-all --force`
- `--kernels all --dracut-current` now still rebuilds the running kernel's initramfs even if a
  different installed kernel fails its DKMS build later in the run.
- Per-run logs now record which kernels succeeded and which failed. The script exits non-zero on
  partial DKMS failure, but only after logging the result and attempting the current-kernel dracut
  step when possible.
- dracut failures are still logged as warnings by default so DKMS install can succeed;
  use `--dracut-strict` to make dracut failure abort the script.

## Build steps

```bash
cd /home/thecatgoesrawr/src/HWS-HDMI-1CHUHD-CaptureCard
sudo -E scripts/dkms-versioned-build.sh --kernels all
```

One-shot build + install + dracut for all kernels:

```bash
sudo -E scripts/dkms-versioned-build.sh --kernels all --prune-others --dracut-all

# rebuild only the running kernel's initramfs, but still build/install for all kernels
sudo -E scripts/dkms-versioned-build.sh --kernels all --prune-others --dracut-current

# keep all DKMS versions (not recommended for pacman hook stability)
sudo -E scripts/dkms-versioned-build.sh --kernels all --keep-others
```

Strict dracut mode (fail fast):

```bash
sudo -E scripts/dkms-versioned-build.sh --kernels all --prune-others --dracut-all --dracut-strict
```

If you need a specific version string:

```bash
sudo -E scripts/dkms-versioned-build.sh --version 1.0.0.230324.g1a2b3c4.r20260225T223000Z --kernels all --prune-others --dracut-all
```

If you skipped `--dracut-all`, rebuild initramfs and reboot:

```bash
sudo dracut --force "/boot/initramfs-linux-lts.img" "$(uname -r)"
sudo reboot
```

## Test steps

```bash
modinfo HwsUHDX1Capture | rg -n "filename|srcversion|version"
/usr/bin/cat /sys/module/HwsUHDX1Capture/srcversion
dkms status | rg HwsUHDX1Capture
```

Expected:
- `modinfo srcversion` matches `/sys/module/.../srcversion`.
- `dkms status` shows the intended test version installed for your kernels.
- If the script exited non-zero, inspect `dkms-build-logs/<run-id>/summary.env`:
  - `succeeded_kernels=...`
  - `failed_kernels=...`
  - `current_kernel_installed=1` means the running kernel was still refreshed and is safe to reboot into.

## Rollback steps

1. Find a prior known-good version:

```bash
dkms status | rg HwsUHDX1Capture
```

2. Re-activate it:

```bash
sudo -E scripts/dkms-versioned-build.sh --version <known-good-version> --kernels all --prune-others --force-reinstall
sudo dracut --force "/boot/initramfs-linux-lts.img" "$(uname -r)"
sudo reboot
```
