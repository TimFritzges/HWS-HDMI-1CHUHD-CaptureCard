# DKMS Update Runbook (Deterministic)

This runbook updates `HwsUHDX1Capture` from local source to DKMS in a reproducible way.

## Canonical method (variables first)
```bash
KVER="$(uname -r)"
SRC="/home/thecatgoesrawr/src/HWS-HDMI-1CHUHD-CaptureCard/src"
DKMS_SRC="/usr/src/HwsUHDX1Capture-1.0.0.230324"
INITRD="/boot/initramfs-linux-lts.img"

sudo rm -rf "$DKMS_SRC"
sudo install -d "$DKMS_SRC"
sudo rsync -a --delete "$SRC/" "$DKMS_SRC/"

sudo dkms remove -m HwsUHDX1Capture -v 1.0.0.230324 --all || true
sudo dkms add -m HwsUHDX1Capture -v 1.0.0.230324
sudo dkms build -m HwsUHDX1Capture -v 1.0.0.230324 -k "$KVER"
sudo dkms install -m HwsUHDX1Capture -v 1.0.0.230324 -k "$KVER" --force
sudo depmod -a

echo 'options HwsUHDX1Capture diag_enable=0' | sudo tee /etc/modprobe.d/hwsuhdx1capture.conf
sudo dracut --force "$INITRD" "$KVER"
sudo reboot
```

## Verify
```bash
modinfo HwsUHDX1Capture | rg -n "filename|srcversion|parm|diag_enable"
/usr/bin/cat /sys/module/HwsUHDX1Capture/srcversion
/usr/bin/cat /sys/module/HwsUHDX1Capture/parameters/diag_enable
find /lib/modules/$(uname -r) -type f -name 'HwsUHDX1Capture.ko*' -print | sort
```

Expected:
- `filename` points to `/lib/modules/<kernel>/updates/dkms/HwsUHDX1Capture.ko.zst`
- `modinfo` `srcversion` matches `/sys/module/.../srcversion`
- only one module path for this kernel (no duplicate `/updates/` copy)

## Runtime log check
```bash
journalctl -k -b --since "2 min ago" --no-pager | rg "START STATUS|END STATUS" | wc -l
```

Expected: count stays near `0` in normal mode (`diag_enable=0`).

## Rollback
```bash
cd /home/thecatgoesrawr/src/HWS-HDMI-1CHUHD-CaptureCard
git restore src/hws_video.c src/hws.h
cd src
make clean || true
make
sudo rsync -a --delete \
  /home/thecatgoesrawr/src/HWS-HDMI-1CHUHD-CaptureCard/src/ \
  /usr/src/HwsUHDX1Capture-1.0.0.230324/
sudo dkms build -m HwsUHDX1Capture -v 1.0.0.230324 -k "$(uname -r)" --force
sudo dkms install -m HwsUHDX1Capture -v 1.0.0.230324 -k "$(uname -r)" --force
sudo depmod -a
```

## Known pitfalls
- Do not split the `rsync` destination onto a new line. Keep `"$DKMS_SRC/"` in the same command.
- If shell `cat` is aliased to `bat`, use `/usr/bin/cat` for sysfs reads.
