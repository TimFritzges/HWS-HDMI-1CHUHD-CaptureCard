#!/usr/bin/env bash
set -euo pipefail

OBS_BASE="${OBS_BASE:-./obs-diag-results}"
BENCH_BASE="${BENCH_BASE:-./bench-results}"
OUT="${OUT:-./bench-results/stability-baseline-$(date -u +%Y%m%d-%H%M%S).tsv}"
PROFILE="${PROFILE:-$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/stability-gate.env}"
BENCH_SUMMARY=""

usage() {
  cat <<'USAGE'
Usage: scripts/analyze-stability-artifacts.sh [options]

Parses historical OBS + bench artifacts and emits a baseline/regression scorecard.

Options:
  -o <path>  Output TSV path
  -O <dir>   OBS diagnostics base dir (default: ./obs-diag-results)
  -B <dir>   Bench results base dir (default: ./bench-results)
  -S <path>  Score only this bench summary
  -p <path>  Gate profile env file (default: scripts/stability-gate.env)
  -h         Help
USAGE
}

while getopts ":o:O:B:S:p:h" opt; do
  case "${opt}" in
    o) OUT="${OPTARG}" ;;
    O) OBS_BASE="${OPTARG}" ;;
    B) BENCH_BASE="${OPTARG}" ;;
    S) BENCH_SUMMARY="${OPTARG}" ;;
    p) PROFILE="${OPTARG}" ;;
    h) usage; exit 0 ;;
    :) echo "Missing argument for -${OPTARG}" >&2; exit 2 ;;
    \?) echo "Unknown option -${OPTARG}" >&2; exit 2 ;;
  esac
done

mkdir -p "$(dirname "${OUT}")"

FAIL_MEMCOPY_MIN="${FAIL_MEMCOPY_MIN:-1}"
FAIL_SOURCE_TRUTH_MISMATCH_MIN="${FAIL_SOURCE_TRUTH_MISMATCH_MIN:-1}"
FAIL_VIDEO_LONG_GAP_MIN="${FAIL_VIDEO_LONG_GAP_MIN:-1}"
FAIL_VIDEO_SEQ_GAP_MIN="${FAIL_VIDEO_SEQ_GAP_MIN:-1}"
FAIL_VIDEO_MONOTONIC_MIN="${FAIL_VIDEO_MONOTONIC_MIN:-1}"
FAIL_ACTIVE_PROBE_PLACEHOLDER_MIN="${FAIL_ACTIVE_PROBE_PLACEHOLDER_MIN:-1}"
FAIL_STALLED_NO_FRESH_MIN="${FAIL_STALLED_NO_FRESH_MIN:-1}"
FAIL_PRODUCER_NO_FREE_MIN="${FAIL_PRODUCER_NO_FREE_MIN:-1}"
FAIL_REGRESSION_SCORE_MIN="${FAIL_REGRESSION_SCORE_MIN:-100}"
WARN_REGRESSION_SCORE_MIN="${WARN_REGRESSION_SCORE_MIN:-20}"
WARN_AUDIO_DUPLICATE_HALF_MIN="${WARN_AUDIO_DUPLICATE_HALF_MIN:-1}"

if [[ -r "${PROFILE}" ]]; then
  # shellcheck disable=SC1090
  source "${PROFILE}"
fi

echo -e "run_type\trun_id\taudio_source_lost_periods\taudio_timer_silence_injects\taudio_workqueue_requeues\taudio_memcopy_failures\taudio_queue_free_slots_min\taudio_queue_free_slots_max\tvideo_long_gap_events\tvideo_seq_gap_frames\tvideo_fallback_frames\tobs_select_timeout\tkernel_timeout\tsource_truth_mismatch\tregression_score\tverdict\tverdict_reason\tsource_path\tartifact_status\tvideo_ts_non_monotonic\tvideo_seq_non_monotonic\tvideo_reused_no_fresh\tvideo_reused_backpressure\taudio_source_starved_transitions\taudio_recovery_transitions\taudio_timer_late_events\taudio_duplicate_half_seen\tvideo_no_signal_placeholders\tvideo_stalled_placeholders\tvideo_fallback_last_stable_ticks\tvideo_fallback_requested_ticks\tvideo_fallback_default_60_ticks\tpersistent_no_signal_with_active_probe\tstalled_without_fresh_frames\tv4l2_compliance_status\tvideo_producer_slot_recoveries\tvideo_producer_stale_frame_reclaims\tvideo_producer_no_free_slots" > "${OUT}"

verdict_for() {
  local memcopy="${1}"
  local mismatch="${2}"
  local score="${3}"
  local long_gap="${4}"
  local seq_gap="${5}"
  local ts_nonmono="${6}"
  local seq_nonmono="${7}"
  local artifact_status="${8}"
  local active_probe_placeholder="${9:-0}"
  local stalled_no_fresh="${10:-0}"
  local compliance_status="${11:-unknown}"
  local producer_no_free="${12:-0}"
  local duplicate_half="${13:-0}"
  if [[ "${artifact_status}" != "complete" ]]; then
    echo "invalid ${artifact_status}"
    return
  fi
  if [[ "${memcopy}" -ge "${FAIL_MEMCOPY_MIN}" ]]; then
    echo "fail memcopy_failures"
    return
  fi
  if [[ "${mismatch}" -ge "${FAIL_SOURCE_TRUTH_MISMATCH_MIN}" ]]; then
    echo "fail source_truth_mismatch"
    return
  fi
  if [[ "${long_gap}" -ge "${FAIL_VIDEO_LONG_GAP_MIN}" || "${seq_gap}" -ge "${FAIL_VIDEO_SEQ_GAP_MIN}" ]]; then
    echo "fail video_gap_or_seq_gap"
    return
  fi
  if [[ "${ts_nonmono}" -ge "${FAIL_VIDEO_MONOTONIC_MIN}" || "${seq_nonmono}" -ge "${FAIL_VIDEO_MONOTONIC_MIN}" ]]; then
    echo "fail video_non_monotonic"
    return
  fi
  if [[ "${active_probe_placeholder}" -ge "${FAIL_ACTIVE_PROBE_PLACEHOLDER_MIN}" ]]; then
    echo "fail active_signal_placeholder"
    return
  fi
  if [[ "${stalled_no_fresh}" -ge "${FAIL_STALLED_NO_FRESH_MIN}" ]]; then
    echo "fail stalled_without_fresh"
    return
  fi
  if [[ "${producer_no_free}" -ge "${FAIL_PRODUCER_NO_FREE_MIN}" ]]; then
    echo "fail producer_no_free_slots"
    return
  fi
  if [[ "${compliance_status}" == "fail" || "${compliance_status}" == "timeout" ]]; then
    echo "fail v4l2_compliance"
    return
  fi
  if [[ "${compliance_status}" == "tool_missing" ]]; then
    echo "invalid missing_v4l2_compliance"
    return
  fi
  if [[ "${score}" -ge "${FAIL_REGRESSION_SCORE_MIN}" ]]; then
    echo "fail high_regression_score"
    return
  fi
  if [[ "${duplicate_half}" -ge "${WARN_AUDIO_DUPLICATE_HALF_MIN}" ]]; then
    echo "warn audio_duplicate_half_seen"
    return
  fi
  if [[ "${score}" -ge "${WARN_REGRESSION_SCORE_MIN}" ]]; then
    echo "warn elevated_regression_score"
    return
  fi
  echo "pass stable"
}

parse_obs_run() {
  local run_dir="$1"
  local run_id
  run_id="$(basename "${run_dir}")"
  local periodic="${run_dir}/periodic-samples.log"
  local journal="${run_dir}/journal-kernel-follow.log"
  local stdout_log="${run_dir}/obs-stdout.log"

  local src_lost=0 timer_silence=0 requeues=0 memcopy=0 qmin=0 qmax=0
  local long_gap=0 seq_gap=0 fallback=0 obs_timeout=0 kern_timeout=0 mismatch=0
  local ts_nonmono=0 seq_nonmono=0 reused_no_fresh=0 reused_backpressure=0
  local no_signal_placeholders=0 stalled_placeholders=0 fallback_last=0 fallback_requested=0 fallback_default=0
  local starved_transitions=0 recovery_transitions=0 timer_late=0 duplicate_half=0 artifact_status="missing_audio_diag"

  if [[ -s "${periodic}" ]]; then
    read -r src_lost timer_silence requeues memcopy qmin qmax fallback starved_transitions recovery_transitions timer_late duplicate_half artifact_status < <(
      awk '
        /^-- ((hws )?debugfs|hws diag) audio_diag --/ {in_a=1; hdr=1; next}
        in_a && /^-- / {in_a=0}
        in_a && hdr {for(i=1;i<=NF;i++) h[$i]=i; hdr=0; next}
        in_a && NF>5 && $1 ~ /^[0-9]+$/ {
          ch=$1
          if (!(ch in fs)) {
            fs[ch,$1]=1
            first_src[ch]=$(h["source_lost_periods"])
            first_sil[ch]=$(h["timer_silence_injects"])
            first_req[ch]=$(h["workqueue_requeues"])
            first_mem[ch]=$(h["memcopy_failures"])
            first_qmin[ch]=$(h["queue_free_slots_min"])
            first_qmax[ch]=$(h["queue_free_slots_max"])
            first_fb[ch]=$(h["fallback_silence_injects"])
            first_starved[ch]=$(h["source_starved_transitions"])
            first_recovery[ch]=$(h["recovery_transitions"])
            first_late[ch]=$(h["timer_late_events"])
            first_duplicate[ch]=$(h["duplicate_half_seen"])
          }
          last_src[ch]=$(h["source_lost_periods"])
          last_sil[ch]=$(h["timer_silence_injects"])
          last_req[ch]=$(h["workqueue_requeues"])
          last_mem[ch]=$(h["memcopy_failures"])
          last_qmin[ch]=$(h["queue_free_slots_min"])
          last_qmax[ch]=$(h["queue_free_slots_max"])
          last_fb[ch]=$(h["fallback_silence_injects"])
          last_starved[ch]=$(h["source_starved_transitions"])
          last_recovery[ch]=$(h["recovery_transitions"])
          last_late[ch]=$(h["timer_late_events"])
          last_duplicate[ch]=$(h["duplicate_half_seen"])
        }
        END {
          min_seen=""
          max_seen=0
          for (k in last_src) {
            src += (last_src[k]-first_src[k])
            sil += (last_sil[k]-first_sil[k])
            req += (last_req[k]-first_req[k])
            mem += (last_mem[k]-first_mem[k])
            fb += (last_fb[k]-first_fb[k])
            starved += (last_starved[k]-first_starved[k])
            recovery += (last_recovery[k]-first_recovery[k])
            late += (last_late[k]-first_late[k])
            duplicate += (last_duplicate[k]-first_duplicate[k])
            if (min_seen=="" || last_qmin[k] < min_seen) min_seen=last_qmin[k]
            if (last_qmax[k] > max_seen) max_seen=last_qmax[k]
          }
          if (min_seen=="") min_seen=0
          status = length(last_src) ? "complete" : "missing_audio_diag"
          printf "%d %d %d %d %d %d %d %d %d %d %d %s\n", src+0, sil+0, req+0, mem+0, min_seen+0, max_seen+0, fb+0, starved+0, recovery+0, late+0, duplicate+0, status
        }
      ' "${periodic}"
    )

    read -r ts_nonmono seq_nonmono reused_no_fresh reused_backpressure no_signal_placeholders stalled_placeholders fallback_last fallback_requested fallback_default < <(
      awk '
        /^-- ((hws )?debugfs|hws diag) video_diag --/ {in_v=1; hdr=1; next}
        in_v && /^-- / {in_v=0}
        in_v && hdr {for(i=1;i<=NF;i++) h[$i]=i; hdr=0; next}
        in_v && NF>5 && $1 ~ /^[0-9]+$/ {
          ch=$1
          if (!(ch in first)) {
            first[ch]=1
            first_ts[ch]=$(h["ts_non_monotonic_events"])
            first_seq[ch]=$(h["seq_non_monotonic_events"])
            first_reuse[ch]=$(h["reused_no_fresh_runs"])
            first_bp[ch]=$(h["reused_backpressure_runs"])
            first_no_signal[ch]=$(h["no_signal_placeholder_frames"])
            first_stalled[ch]=$(h["stalled_placeholder_frames"])
            first_last[ch]=$(h["fallback_last_stable_ticks"])
            first_requested[ch]=$(h["fallback_requested_ticks"])
            first_default[ch]=$(h["fallback_default_60_ticks"])
          }
          last_ts[ch]=$(h["ts_non_monotonic_events"])
          last_seq[ch]=$(h["seq_non_monotonic_events"])
          last_reuse[ch]=$(h["reused_no_fresh_runs"])
          last_bp[ch]=$(h["reused_backpressure_runs"])
          last_no_signal[ch]=$(h["no_signal_placeholder_frames"])
          last_stalled[ch]=$(h["stalled_placeholder_frames"])
          last_last[ch]=$(h["fallback_last_stable_ticks"])
          last_requested[ch]=$(h["fallback_requested_ticks"])
          last_default[ch]=$(h["fallback_default_60_ticks"])
        }
        END {
          for (ch in first) {
            ts += last_ts[ch] - first_ts[ch]
            seq += last_seq[ch] - first_seq[ch]
            reuse += last_reuse[ch] - first_reuse[ch]
            bp += last_bp[ch] - first_bp[ch]
            no_signal += last_no_signal[ch] - first_no_signal[ch]
            stalled += last_stalled[ch] - first_stalled[ch]
            last_fb += last_last[ch] - first_last[ch]
            requested_fb += last_requested[ch] - first_requested[ch]
            default_fb += last_default[ch] - first_default[ch]
          }
          printf "%d %d %d %d %d %d %d %d %d\n", ts+0, seq+0, reuse+0, bp+0, no_signal+0, stalled+0, last_fb+0, requested_fb+0, default_fb+0
        }
      ' "${periodic}"
    )

    long_gap="$(rg -c "frame_delta_over_3x_target_events=[1-9]" "${periodic}" 2>/dev/null || echo 0)"
  fi

  if [[ -s "${stdout_log}" ]]; then
    obs_timeout="$(rg -ci "v4l2-input: .*select timed out" "${stdout_log}" 2>/dev/null || echo 0)"
  fi

  if [[ -s "${journal}" ]]; then
    kern_timeout="$(rg -ci "(${MODULE:-HwsUHDX1Capture}|hws:|videobuf2|dma).*(timeout|reset|error)|(timeout|reset|error).*(${MODULE:-HwsUHDX1Capture}|hws:|videobuf2|dma)" "${journal}" 2>/dev/null || echo 0)"
  fi

  mismatch="$(awk -v t="${obs_timeout}" -v l="${src_lost}" 'BEGIN{print (t>0 || l>0)?1:0}')"
  local score
  score="$(awk -v a="${src_lost}" -v b="${timer_silence}" -v c="${requeues}" -v d="${memcopy}" -v e="${kern_timeout}" -v f="${obs_timeout}" -v g="${long_gap}" -v h="${fallback}" -v i="${ts_nonmono}" -v j="${seq_nonmono}" -v k="${timer_late}" -v m="${mismatch}" 'BEGIN{print a*5 + b*3 + c + d*4 + e*2 + f*2 + g*4 + h + i*10 + j*10 + k + m*10}')"

  read -r verdict verdict_reason < <(verdict_for "${memcopy}" "${mismatch}" "${score}" "${long_gap}" "${seq_gap}" "${ts_nonmono}" "${seq_nonmono}" "${artifact_status}" 0 0 not_run 0 "${duplicate_half}")
  echo -e "obs\t${run_id}\t${src_lost}\t${timer_silence}\t${requeues}\t${memcopy}\t${qmin}\t${qmax}\t${long_gap}\t${seq_gap}\t${fallback}\t${obs_timeout}\t${kern_timeout}\t${mismatch}\t${score}\t${verdict}\t${verdict_reason}\t${run_dir}\t${artifact_status}\t${ts_nonmono}\t${seq_nonmono}\t${reused_no_fresh}\t${reused_backpressure}\t${starved_transitions}\t${recovery_transitions}\t${timer_late}\t${duplicate_half}\t${no_signal_placeholders}\t${stalled_placeholders}\t${fallback_last}\t${fallback_requested}\t${fallback_default}\t0\t0\tnot_run\t0\t0\t0"
}

parse_bench_run() {
  local summary="$1"
  local run_dir
  run_dir="$(dirname "${summary}")"
  local run_id
  run_id="$(basename "${run_dir}")"

  local src_lost timer_silence requeues memcopy qmin qmax seq_gap fallback mismatch long_gap obs_timeout kern_timeout
  local ts_nonmono seq_nonmono reused_no_fresh reused_backpressure starved_transitions recovery_transitions timer_late duplicate_half
  local no_signal_placeholders stalled_placeholders fallback_last fallback_requested fallback_default
  local active_probe_placeholder stalled_no_fresh compliance_status capture_status producer_recoveries producer_stale_reclaims producer_no_free
  local diag_available audio_diag_available artifact_status
  src_lost="$(awk -F= '/^audio_source_lost_periods_delta=/{print $2}' "${summary}" | tail -n1)"
  timer_silence="$(awk -F= '/^audio_timer_silence_injects_delta=/{print $2}' "${summary}" | tail -n1)"
  requeues="$(awk -F= '/^audio_workqueue_requeues_delta=/{print $2}' "${summary}" | tail -n1)"
  memcopy="$(awk -F= '/^audio_memcopy_failures_delta=/{print $2}' "${summary}" | tail -n1)"
  qmin="$(awk -F= '/^audio_queue_free_slots_min_observed=/{print $2}' "${summary}" | tail -n1)"
  qmax="$(awk -F= '/^audio_queue_free_slots_max_observed=/{print $2}' "${summary}" | tail -n1)"
  seq_gap="$(awk -F= '/^source_seq_gap_frames=/{print $2}' "${summary}" | tail -n1)"
  mismatch="$(awk -F= '/^source_truth_mismatch_flag=/{print $2}' "${summary}" | tail -n1)"
  long_gap="$(awk -F= '/^frame_delta_over_3x_target_events=/{print $2}' "${summary}" | tail -n1)"
  fallback="$(awk -F= '/^audio_starvation_intervals=/{print $2}' "${summary}" | tail -n1)"
  ts_nonmono="$(awk -F= '/^video_ts_non_monotonic_delta=/{print $2}' "${summary}" | tail -n1)"
  seq_nonmono="$(awk -F= '/^video_seq_non_monotonic_delta=/{print $2}' "${summary}" | tail -n1)"
  reused_no_fresh="$(awk -F= '/^video_reused_no_fresh_delta=/{print $2}' "${summary}" | tail -n1)"
  reused_backpressure="$(awk -F= '/^video_reused_backpressure_delta=/{print $2}' "${summary}" | tail -n1)"
  starved_transitions="$(awk -F= '/^audio_source_starved_transitions_delta=/{print $2}' "${summary}" | tail -n1)"
  recovery_transitions="$(awk -F= '/^audio_recovery_transitions_delta=/{print $2}' "${summary}" | tail -n1)"
  timer_late="$(awk -F= '/^audio_timer_late_events_delta=/{print $2}' "${summary}" | tail -n1)"
  duplicate_half="$(awk -F= '/^audio_duplicate_half_seen_delta=/{print $2}' "${summary}" | tail -n1)"
  no_signal_placeholders="$(awk -F= '/^video_no_signal_placeholder_frames_delta=/{print $2}' "${summary}" | tail -n1)"
  stalled_placeholders="$(awk -F= '/^video_stalled_placeholder_frames_delta=/{print $2}' "${summary}" | tail -n1)"
  fallback_last="$(awk -F= '/^video_fallback_last_stable_ticks_delta=/{print $2}' "${summary}" | tail -n1)"
  fallback_requested="$(awk -F= '/^video_fallback_requested_ticks_delta=/{print $2}' "${summary}" | tail -n1)"
  fallback_default="$(awk -F= '/^video_fallback_default_60_ticks_delta=/{print $2}' "${summary}" | tail -n1)"
  active_probe_placeholder="$(awk -F= '/^persistent_no_signal_with_active_probe=/{print $2}' "${summary}" | tail -n1)"
  stalled_no_fresh="$(awk -F= '/^stalled_without_fresh_frames=/{print $2}' "${summary}" | tail -n1)"
  compliance_status="$(awk -F= '/^v4l2_compliance_status=/{print $2}' "${summary}" | tail -n1)"
  producer_recoveries="$(awk -F= '/^video_producer_slot_recoveries_delta=/{print $2}' "${summary}" | tail -n1)"
  producer_stale_reclaims="$(awk -F= '/^video_producer_stale_frame_reclaims_delta=/{print $2}' "${summary}" | tail -n1)"
  producer_no_free="$(awk -F= '/^video_producer_no_free_slots_delta=/{print $2}' "${summary}" | tail -n1)"
  capture_status="$(awk -F= '/^capture_status=/{print $2}' "${summary}" | tail -n1)"
  diag_available="$(awk -F= '/^diag_(after_captured|available)=/{print $2}' "${summary}" | tail -n1)"
  audio_diag_available="$(awk -F= '/^(audio_diag_after_captured|audio_diag_available)=/{print $2}' "${summary}" | tail -n1)"

  src_lost="${src_lost:-0}"; timer_silence="${timer_silence:-0}"; requeues="${requeues:-0}"; memcopy="${memcopy:-0}"
  qmin="${qmin:-0}"; qmax="${qmax:-0}"; seq_gap="${seq_gap:-0}"; mismatch="${mismatch:-0}"
  long_gap="${long_gap:-0}"; fallback="${fallback:-0}"; obs_timeout=0; kern_timeout=0
  ts_nonmono="${ts_nonmono:-0}"; seq_nonmono="${seq_nonmono:-0}"
  reused_no_fresh="${reused_no_fresh:-0}"; reused_backpressure="${reused_backpressure:-0}"
  starved_transitions="${starved_transitions:-0}"; recovery_transitions="${recovery_transitions:-0}"; timer_late="${timer_late:-0}"; duplicate_half="${duplicate_half:-0}"
  no_signal_placeholders="${no_signal_placeholders:-0}"; stalled_placeholders="${stalled_placeholders:-0}"
  fallback_last="${fallback_last:-0}"; fallback_requested="${fallback_requested:-0}"; fallback_default="${fallback_default:-0}"
  active_probe_placeholder="${active_probe_placeholder:-0}"; stalled_no_fresh="${stalled_no_fresh:-0}"
  compliance_status="${compliance_status:-not_recorded}"
  producer_recoveries="${producer_recoveries:-0}"; producer_stale_reclaims="${producer_stale_reclaims:-0}"; producer_no_free="${producer_no_free:-0}"
  capture_status="${capture_status:-not_recorded}"
  artifact_status="complete"
  if [[ "${diag_available:-0}" != "1" ]]; then
    artifact_status="missing_video_diag"
  elif [[ "${audio_diag_available:-0}" != "1" ]]; then
    artifact_status="missing_audio_diag"
  elif [[ "${capture_status}" != "pass" ]]; then
    artifact_status="capture_${capture_status}"
  fi

  local score
  score="$(awk -v a="${src_lost}" -v b="${timer_silence}" -v c="${requeues}" -v d="${memcopy}" -v e="${seq_gap}" -v f="${long_gap}" -v g="${fallback}" -v h="${ts_nonmono}" -v i="${seq_nonmono}" -v j="${timer_late}" -v m="${mismatch}" -v p="${active_probe_placeholder}" -v s="${stalled_no_fresh}" -v n="${producer_no_free}" 'BEGIN{print a*5 + b*3 + c + d*4 + e*2 + f*4 + g + h*10 + i*10 + j + m*10 + p*10 + s*10 + n*4}')"

  read -r verdict verdict_reason < <(verdict_for "${memcopy}" "${mismatch}" "${score}" "${long_gap}" "${seq_gap}" "${ts_nonmono}" "${seq_nonmono}" "${artifact_status}" "${active_probe_placeholder}" "${stalled_no_fresh}" "${compliance_status}" "${producer_no_free}" "${duplicate_half}")
  echo -e "bench\t${run_id}\t${src_lost}\t${timer_silence}\t${requeues}\t${memcopy}\t${qmin}\t${qmax}\t${long_gap}\t${seq_gap}\t${fallback}\t${obs_timeout}\t${kern_timeout}\t${mismatch}\t${score}\t${verdict}\t${verdict_reason}\t${run_dir}\t${artifact_status}\t${ts_nonmono}\t${seq_nonmono}\t${reused_no_fresh}\t${reused_backpressure}\t${starved_transitions}\t${recovery_transitions}\t${timer_late}\t${duplicate_half}\t${no_signal_placeholders}\t${stalled_placeholders}\t${fallback_last}\t${fallback_requested}\t${fallback_default}\t${active_probe_placeholder}\t${stalled_no_fresh}\t${compliance_status}\t${producer_recoveries}\t${producer_stale_reclaims}\t${producer_no_free}"
}

if [[ -z "${BENCH_SUMMARY}" && -d "${OBS_BASE}" ]]; then
  while IFS= read -r d; do
    parse_obs_run "${d}" >> "${OUT}"
  done < <(find "${OBS_BASE}" -mindepth 1 -maxdepth 1 -type d | sort)
fi

if [[ -n "${BENCH_SUMMARY}" ]]; then
  if [[ ! -s "${BENCH_SUMMARY}" ]]; then
    echo "Bench summary is missing: ${BENCH_SUMMARY}" >&2
    exit 2
  fi
  parse_bench_run "${BENCH_SUMMARY}" >> "${OUT}"
elif [[ -d "${BENCH_BASE}" ]]; then
  while IFS= read -r s; do
    parse_bench_run "${s}" >> "${OUT}"
  done < <(find "${BENCH_BASE}" -type f \( -name "*-summary.txt" -o -name "summary.env" \) | sort)
fi

echo "wrote=${OUT}"
