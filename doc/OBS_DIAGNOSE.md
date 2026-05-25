# OBS Diagnostics Wrapper

This adds a launch wrapper that starts OBS and records long-session diagnostics for later driver/performance analysis.

## Files

- `tools/obs-diagnose.sh`
- `tools/collect-evidence.sh`
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
- Low-overhead OBS process counters (`perf stat`)
- Optional kernel scheduler/IRQ/hrtimer/workqueue trace via the restricted helper
- Optional coredumps since run start
- Automatic diag toggle:
  - `diag_enable` is set to `1` on wrapper start and restored to previous value on exit
  - controlled by `DIAG_AUTO_TOGGLE=0|1` (default `1`)
  - PipeWire evidence is controlled by `CAPTURE_PIPEWIRE=0|1` and
    `PW_PROFILER_ENABLE=0|1` (both default `1`)
  - detailed PipeWire profiling is bounded by `PW_PROFILER_SAMPLES=600`
  - `audio_trace_enable` is set to `1` and restored when
    `AUDIO_TRACE_AUTO=1` and the updated helper is installed
  - kernel tracing is collected automatically when `KERNEL_TRACE_AUTO=1`,
    `trace-cmd` is installed, and the updated helper passes `--trace-capable`
  - fixed-event kernel tracing is bounded by `KERNEL_TRACE_SECONDS=30` by
    default to avoid distorting a long OBS session or producing huge artifacts
  - `KERNEL_TRACE_FUNCTIONS=1` enables the higher-overhead HWS function trace
    for a short focused session only

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
wrapper can sample them without sudo. The restricted helper additionally enables
kernel journal reads, diagnostic parameter toggling, pstore listing, and a fixed
`trace-cmd` event set that stops when the OBS process exits. It cannot execute
arbitrary commands.

Install helper + tight sudoers rule:

```bash
cd /home/thecatgoesrawr/src/HWS-HDMI-1CHUHD-CaptureCard
chmod +x tools/obs-diag-priv.sh tools/install-obs-diag-priv.sh
sudo ./tools/install-obs-diag-priv.sh --user thecatgoesrawr
```

Verify:

```bash
sudo -n /usr/local/sbin/hws-obs-diag-priv --self-test
sudo -n /usr/local/sbin/hws-obs-diag-priv --trace-capable
```

`tools/obs-diagnose.sh` auto-detects this helper and uses it when available.
Live journal/PipeWire monitor processes are stopped as soon as OBS exits, and
slow post-run queries are time-bounded so a diagnostic launch cannot remain
running indefinitely after OBS has closed.

If a previous copy of the helper is installed, rerun the install command above
after updating this repository. Otherwise `meta.env` records
`kernel_trace_status=helper_update_or_trace_access_required`.

## Automatic Audio Trace Mode

`audio_trace_enable` is intentionally separate from `diag_enable`.
Diagnostic OBS launches now toggle both automatically through the helper and
restore their original values when OBS exits. To suppress trace lines for an
ordinary long stream:

```bash
AUDIO_TRACE_AUTO=0 KERNEL_TRACE_AUTO=0 tools/obs-diagnose.sh
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
