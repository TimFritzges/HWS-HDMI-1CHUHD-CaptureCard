# OBS Diagnostics Wrapper

This adds a launch wrapper that starts OBS and records long-session diagnostics for later driver/performance analysis.

## Files

- `tools/obs-diagnose.sh`
- `tools/install-obs-diagnose-desktop.sh`
- `tools/obs-diag-priv.sh`
- `tools/install-obs-diag-priv.sh`

## What gets captured per run

- Module identity: `modinfo`, `dkms status`, `lsmod`
- Driver/runtime: HWS module params, `/proc/hwsuhdx1` diag snapshots (debugfs fallback)
- System/runtime: periodic `ps`, `loadavg`, filtered `/proc/interrupts`
- Journals:
  - live kernel follow log
  - live kernel filtered log (`HwsUHDX1Capture`, `videobuf2`, `uvcvideo`, timeout/reset/error patterns)
  - kernel since run-start (+ filtered copy)
- OBS logs generated during run (`~/.config/obs-studio/logs`)
- PipeWire graph and timing evidence (`pw-dump`, `pw-top`, `pw-profiler`)
- Optional coredumps since run start
- Automatic diag toggle:
  - `diag_enable` is set to `1` on wrapper start and restored to previous value on exit
  - controlled by `DIAG_AUTO_TOGGLE=0|1` (default `1`)
  - PipeWire evidence is controlled by `CAPTURE_PIPEWIRE=0|1` and
    `PW_PROFILER_ENABLE=0|1` (both default `1`)

Output is versioned by UTC timestamp + module srcversion:
- `~/obs-diag-results/<timestamp>-<tag>-HwsUHDX1Capture-<srcversion>/`

## Attach to OBS desktop launcher

This preserves your current custom Exec style (for example `env __NV_DISABLE_EXPLICIT_SYNC=1 obs %U`) by replacing only the `obs` binary with the wrapper.

```bash
cd /home/thecatgoesrawr/src/HWS-HDMI-1CHUHD-CaptureCard
chmod +x tools/obs-diagnose.sh tools/install-obs-diagnose-desktop.sh
tools/install-obs-diagnose-desktop.sh --mode replace
```

Use `--mode new` if you want a second launcher instead of replacing the existing one.

## Manual run

```bash
cd /home/thecatgoesrawr/src/HWS-HDMI-1CHUHD-CaptureCard
tools/obs-diagnose.sh -g stream-session
```

## Optional privileged diagnostics (passwordless sudo)

New driver builds expose read-only driver diagnostics at `/proc/hwsuhdx1` so the
wrapper can sample them without sudo. This helper still enables unrestricted
kernel journal reads and debugfs fallback on older driver builds.

Install helper + tight sudoers rule:

```bash
cd /home/thecatgoesrawr/src/HWS-HDMI-1CHUHD-CaptureCard
chmod +x tools/obs-diag-priv.sh tools/install-obs-diag-priv.sh
sudo ./tools/install-obs-diag-priv.sh --user thecatgoesrawr
```

Verify:

```bash
sudo -n /usr/local/sbin/hws-obs-diag-priv --self-test
```

`tools/obs-diagnose.sh` auto-detects this helper and uses it when available.

## Deep audio trace mode (optional)

`audio_trace_enable` is intentionally separate from `diag_enable`.
Use this only for focused debugging because it can generate high trace volume.

```bash
echo 1 | sudo tee /sys/module/HwsUHDX1Capture/parameters/audio_trace_enable
# run OBS diagnostic session
echo 0 | sudo tee /sys/module/HwsUHDX1Capture/parameters/audio_trace_enable
```

Current `audio_diag` includes root-cause counters for:
- queue starvation and copy failures (`no_free_queue_slots`, `memcopy_failures`)
- silence injection paths (`no_video_silence_injects`, `fallback_silence_injects`, `timer_silence_injects`)
- timing and pressure (`irq_to_copy_*`, `copy_to_deliver_*`, `irq_to_deliver_*`, `queue_free_slots_*`)
- timer activity and source-loss behavior (`timer_runs`, `real_periods`, `silence_periods`,
  `underrun_periods`, `source_lost_periods`, `timer_late_*`, `pcm_running`,
  `staged_fill_bytes_*`)

The ALSA capture clock is timer-driven while PCM is running. If HDMI audio or the
video source stops producing packets, the driver should continue advancing ALSA
periods with silence instead of letting PipeWire accumulate xruns.

## Rollback

If you used `--mode replace`, restore the latest backup:

```bash
ls -1t ~/.local/share/applications/com.obsproject.Studio.desktop.bak.* | head -n1
cp -a <that-backup-file> ~/.local/share/applications/com.obsproject.Studio.desktop
update-desktop-database ~/.local/share/applications || true
```

Remove privileged helper integration:

```bash
sudo rm -f /etc/sudoers.d/hws-obs-diag
sudo rm -f /usr/local/sbin/hws-obs-diag-priv
```
