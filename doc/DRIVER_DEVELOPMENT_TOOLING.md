# Driver Development Tooling

This workflow increases evidence quality without attempting live module reloads.
PipeWire may keep the capture device open; installed driver validation therefore
remains reboot-gated.

## Scope

- `scripts/static-check.sh` rebuilds the module without privilege and runs
  installed `sparse`/`smatch` analyzers before a DKMS install is prepared.
  It cleans generated module objects before analyzer passes so checks cannot be
  silently skipped by an up-to-date build cache.
- `scripts/bench.sh` captures V4L2 compliance results, video/audio driver
  diagnostics, filtered kernel messages, synchronized PipeWire snapshots, and
  a bounded PipeWire recording of the HWS capture source for sample-integrity
  inspection.
- `scripts/postboot-diagnose.sh` runs the complete non-privileged validation
  suite after a module installation and reboot.
- `scripts/trace-session.sh` explicitly wraps one reproduction in `trace-cmd`
  collection. It is not enabled automatically because tracing changes timing.
- `tools/obs-diagnose.sh` captures snapshots, `pw-dump`, `pw-top`,
  `pw-profiler`, `perf`, and restricted helper-backed kernel tracing while OBS
  owns the device.

`vivid` or `vimc` may be used separately to verify OBS/V4L2 userspace behavior.
They do not reproduce the PCI DMA, interrupt, or HDMI signal path in this driver.

## Pre-Install Check

```bash
cd /home/thecatgoesrawr/src/HWS-HDMI-1CHUHD-CaptureCard
scripts/static-check.sh
```

Expected output includes `build_status=pass`, `sparse_status=pass`, and either
`smatch_status=pass` or `smatch_status=pass_with_style_warnings`. The latter
is accepted only when `smatch_actionable_warning_count=0` and
`smatch_error_count=0`; the current source retains an indentation-only legacy
backlog. Both analyzers are installed on this host and must be required before
a DKMS checkpoint:

```bash
scripts/static-check.sh --sparse yes --smatch yes
```

`coccinelle`/`spatch` is installed but is intentionally opt-in because the
semantic patch suite is expensive:

```bash
scripts/static-check.sh --sparse yes --smatch yes --cocci yes
```

`scripts/checkpoint-run.sh --prepare-reboot` now runs this check before writing
the DKMS command artifact. Use `--skip-static-check` only when diagnosing the
check script itself.

`static-check.sh` serializes its Kbuild output with a per-user `flock` lock.
Concurrent postboot/checkpoint/manual checks therefore wait rather than
cleaning or rebuilding the same source tree underneath another analyzer run.

## Isolated Short Bench

Do not run this while OBS is actively using the V4L2 node. The compliance check
exercises driver ioctls and the benchmark opens the capture stream.

```bash
scripts/bench.sh -d /dev/video0 -t 60 -s 1920x1080 -f 60 -g post-reboot
```

New artifacts in `bench-results/<run>/` include:

- `v4l2-compliance.log`: V4L2 API conformance, recorded as
  `v4l2_compliance_status=pass|fail|timeout|tool_missing`. Compliance is
  terminated after 90 seconds by default so a stuck driver cannot block the
  artifact run indefinitely.
- `pw-dump.before.json` and `pw-dump.after.json`: PipeWire graph and negotiated
  properties around the test.
- `pw-top.log`: PipeWire error/xrun behavior while video is being captured.
- `pw-profiler.json`: optional detailed cycle timing.
- `audio-capture.wav`, `audio-capture-metadata.txt`,
  `audio-capture-stats.log`, and `audio-capture-spectrogram.png`: post-PipeWire
  sample evidence for audible corruption. Auto-targeting refuses to record a
  non-HWS default source.
- `audio_duplicate_half_seen_delta`: a warning that the hardware half-buffer
  selector repeated during capture. This can be harmless interrupt behavior or
  repeated stale PCM; compare it with the WAV/spectrogram before altering the
  copy path.
- `video_diag` summaries include producer ring recovery, stale complete-frame
  reclamation, and no-free-slot counters. `producer_stale_frame_reclaims`
  records bounded dropped source frames when delivery is behind without
  allowing an in-flight copy to be overwritten. `producer_no_free_slots` is a
  failed gate because it means a complete source frame could not be staged.

For a focused audio timing run, enable PipeWire profiling:

```bash
PW_PROFILER_SAMPLES=600 scripts/bench.sh -t 60 -g audio-profiler
```

If `audio_capture_status=target_missing`, list the PipeWire node names and pass
the HWS input explicitly:

```bash
pactl list short sources
AUDIO_PW_TARGET=<hws-source-name> scripts/bench.sh -t 30 -g audio-sample
```

For useful spectrogram comparison, feed the capture card a controlled source
such as a constant 1 kHz stereo tone at 48 kHz/16-bit, then retain both the WAV
and generated PNG from each driver build. Random program audio can reveal
gross corruption but is weaker evidence for rate, framing, or channel-order
faults.

For a conformance run that also exercises streaming buffers:

```bash
V4L2_COMPLIANCE_STREAM_FRAMES=60 scripts/bench.sh -t 60 -g compliance-stream
```

Both the conformance stream and the capture backend are bounded. Override only
for a deliberate investigation:

```bash
V4L2_COMPLIANCE_TIMEOUT_SEC=120 CAPTURE_TIMEOUT_GRACE_SEC=30 scripts/bench.sh -t 60 -g extended-timeout
```

## OBS Session Capture

```bash
tools/obs-diagnose.sh -g obs-repro
```

The OBS wrapper records PipeWire topology before and after the session, saves a
bounded 30-second HWS-source WAV/spectrogram sample after OBS starts, and runs
bounded `pw-profiler`, `perf`, module diagnostic toggles, and the fixed-event
kernel trace during the first bounded 30 seconds of the session by default. It
does not substitute an unrelated default source if HWS auto-detection fails;
set `AUDIO_PW_TARGET=<name>` for an unusual node name or
`AUDIO_SAMPLE_AUTO=0` to disable sample recording.
Reinstall the privileged helper once after updating the repository:

```bash
sudo ./tools/install-obs-diag-priv.sh --user thecatgoesrawr
sudo -n /usr/local/sbin/hws-obs-diag-priv --trace-capable
```

Set `CAPTURE_PIPEWIRE=0`, `PW_PROFILER_ENABLE=0`, `PERF_AUTO=0`,
`AUDIO_TRACE_AUTO=0`, `AUDIO_SAMPLE_AUTO=0`, or `KERNEL_TRACE_AUTO=0` only when measuring the
diagnostic overhead itself.

## Targeted Kernel Trace

For a standalone short trace, use direct tracefs access if configured:

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

On this host tracefs currently requires privilege. The OBS workflow handles
that through `tools/obs-diag-priv.sh` rather than granting general tracefs
access or making arbitrary commands passwordless.

## Autonomous Post-Reboot Test

Close OBS, then run:

```bash
scripts/postboot-diagnose.sh -d /dev/video0 -t 60 -g installed-check
```

The command requires `sparse`, captures full pre/post state, records bounded
audio sample evidence from the identified HWS PipeWire source, runs a streaming
V4L2 compliance pass and profiled benchmark, compares on-disk/runtime module
identity, and writes a stability verdict. It exits non-zero if any required
check fails or times out.

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
scripts/postboot-diagnose.sh -t 60 -g installed-check
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
