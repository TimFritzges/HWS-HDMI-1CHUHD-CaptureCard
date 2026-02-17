# Arch Packaging Skeleton

This folder contains local packaging skeletons for deterministic installs.

## Packages
- `hws-uhdx1capture-dkms`: installs only DKMS source into `/usr/src/HwsUHDX1Capture-<version>`.
- `hws-uhdx1capture-tools`: installs helper scripts/docs (no kernel module installation side effects).

## Deterministic Build Flow
1. Pin a source commit in `hws-uhdx1capture-dkms/PKGBUILD` via `_commit`.
2. Keep `pkgver` aligned with the source snapshot/version you are pinning.
3. Build and install with `makepkg -si` from each package directory.

## Notes
- `sha256sums=('SKIP')` is intentionally left as a local-dev default. Replace with real checksums for shared/distributed packages.
- The DKMS package relies on system DKMS hooks to build for installed kernels.
