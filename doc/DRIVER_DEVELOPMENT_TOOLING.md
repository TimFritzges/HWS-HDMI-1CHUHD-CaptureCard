# Driver Development Tooling

This workflow increases evidence quality without attempting live module reloads.
PipeWire may keep the capture device open; installed driver validation therefore
remains reboot-gated.

## Scope

- `scripts/static-check.sh` builds the module without privilege and optionally
  runs `sparse` before a DKMS install is prepared.
- `scripts/bench.sh` captures V4L2 compliance results, video/audio driver
  diagnostics, filtered kernel messages, and synchronized PipeWire snapshots.
- `scripts/trace-session.sh` explicitly wraps one reproduction in `trace-cmd`
  collection. It is not enabled automatically because tracing changes timing.
- `tools/obs-diagnose.sh` captures `pw-dump`, `pw-top`, and `pw-profiler`
  evidence while OBS owns the device.

`vivid` or `vimc` may be used separately to verify OBS/V4L2 userspace behavior.
They do not reproduce the PCI DMA, interrupt, or HDMI signal path in this driver.

## Pre-Install Check

```bash
cd /home/thecatgoesrawr/src/HWS-HDMI-1CHUHD-CaptureCard
scripts/static-check.sh
```

Expected output includes `build_status=pass`. If `sparse` is not installed,
the default result is `sparse_status=tool_missing_optional`; compilation still
acts as the required gate. After installing `sparse`, require it with:

```bash
scripts/static-check.sh --sparse yes
```

`scripts/checkpoint-run.sh --prepare-reboot` now runs this check before writing
the DKMS command artifact. Use `--skip-static-check` only when diagnosing the
check script itself.

## Isolated Short Bench

Do not run this while OBS is actively using the V4L2 node. The compliance check
exercises driver ioctls and the benchmark opens the capture stream.

```bash
scripts/bench.sh -d /dev/video0 -t 60 -s 1920x1080 -f 60 -g post-reboot
```

New artifacts in `bench-results/<run>/` include:

- `v4l2-compliance.log`: V4L2 API conformance, recorded as
  `v4l2_compliance_status=pass|fail|tool_missing`.
- `pw-dump.before.json` and `pw-dump.after.json`: PipeWire graph and negotiated
  properties around the test.
- `pw-top.log`: PipeWire error/xrun behavior while video is being captured.
- `pw-profiler.json`: optional detailed cycle timing.

For a focused audio timing run, enable PipeWire profiling:

```bash
PW_PROFILER_SAMPLES=600 scripts/bench.sh -t 60 -g audio-profiler
```

For a conformance run that also exercises streaming buffers:

```bash
V4L2_COMPLIANCE_STREAM_FRAMES=60 scripts/bench.sh -t 60 -g compliance-stream
```

## OBS Session Capture

```bash
tools/obs-diagnose.sh -g obs-repro
```

The OBS wrapper records PipeWire topology before and after the session and runs
`pw-profiler` during the session by default. Set `CAPTURE_PIPEWIRE=0` or
`PW_PROFILER_ENABLE=0` only when measuring profiling overhead.

## Targeted Kernel Trace

Install `trace-cmd` and grant tracefs access according to host policy, then run:

```bash
scripts/trace-session.sh -- scripts/bench.sh -t 10 -g trace-video
```

This records IRQ, hrtimer, workqueue, and scheduler events. Function tracing has
higher overhead and is reserved for a short reproduction:

```bash
TRACE_FUNCTIONS=1 scripts/trace-session.sh -- scripts/bench.sh -t 10 -g trace-audio
```

The trace artifact is `bench-results/<run>/trace.dat`; renderable text is
written to `trace.report.txt`.

## Build And Install Checkpoint

Keep the LTS kernel as the fallback environment. Build/install the driver only
while booted into the intended non-LTS test kernel; this ensures
`--dracut-current` cannot rewrite the LTS initramfs:

```bash
KVER="$(uname -r)" # verify this is the non-LTS test kernel first
sudo -E ./scripts/dkms-versioned-build.sh \
  --kernels current \
  --prune-others \
  --dracut-current
sudo reboot
```

After reboot:

```bash
modinfo HwsUHDX1Capture | rg -n "filename|srcversion|version"
/usr/bin/cat /sys/module/HwsUHDX1Capture/srcversion
scripts/bench.sh -t 60 -g installed-check
```

Expected result: the on-disk and runtime `srcversion` values match, and the
bench summary records compliance, diagnostics, and PipeWire artifacts.

## Rollback

Do not change the LTS installation while testing the non-LTS driver. Reinstall
a prior known-good version while booted into only the affected test kernel:

```bash
KVER="$(uname -r)" # verify this is the non-LTS test kernel first
sudo -E ./scripts/dkms-versioned-build.sh \
  --version <known-good-version> \
  --kernels current \
  --prune-others \
  --force-reinstall \
  --dracut-current
sudo reboot
```

After reboot, verify `srcversion` identity again before starting OBS.
