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
- `--dracut-all` now detects the host boot layout:
  - classic GRUB-style `/boot/initramfs-*.img` hosts: rebuilds each selected kernel explicitly via
    `dracut --force /boot/initramfs-<pkgbase>.img <kver>`
  - kernel-install style hosts: falls back to `dracut --regenerate-all --force`
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
