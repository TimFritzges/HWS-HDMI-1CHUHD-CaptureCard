# Phase 2 Validation Checkpoints

This file tracks Phase 2 validation targets for the `videoslock` lock-scope refactor in `video_data_process()`.

## Success Criteria

1. Checkpoint 1 (Short):
- No new errors over `30s` test window, consistently for `3` consecutive runs.

2. Checkpoint 2 (Medium):
- No new errors over a `5 minute` window.

3. Checkpoint 3 (Long):
- No new errors over a `60 minute` window.

## Error Signals To Watch

- `net_err_rate_over_idle_per_s == 0.000`
- `net_capture_err_rate_over_idle_per_s == 0.000`
- `net_err_delta_over_idle == 0`
- `net_capture_err_delta_over_idle == 0`
- `retire_capture_urb_events == 0`
- `callbacks_suppressed_events == 0`
- `callbacks_suppressed_total == 0`
- `source_seq_gap_frames == 0`
- `estimated_drop_vs_target_frames == 0`

## Recommended Commands

30s run (repeat 3x with different tags):

```bash
./tools/bench.sh -t 30 -g phase2-30s-a
./tools/bench.sh -t 30 -g phase2-30s-b
./tools/bench.sh -t 30 -g phase2-30s-c
```

5 minute run:

```bash
./tools/bench.sh -t 300 -g phase2-5m
```

60 minute run:

```bash
./tools/bench.sh -t 3600 -g phase2-60m
```

## Notes

- Keep `PRETEST_SECONDS` equal to `-t` (default behavior) for fair idle-vs-test subtraction.
- Use `bench-results/history-v4.csv` for run-over-run comparison.
- Ignore known pre-fix benchmark rows before `run_id=20260214-012833-math-fix-check`.
