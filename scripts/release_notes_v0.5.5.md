## Stuttometer v0.5.5 — FileTime Timestamp Fix (Critical)

### Critical Bug Fix: Tool Was Producing Zero Reports

This release resolves a critical defect in which no stutter reports were generated during live capture sessions. The tool would enter a stalled state and suppress every trigger indefinitely without surfacing an error.

**Root cause:** ETW delivers event timestamps in the FileTime domain (100 ns ticks since 1601-01-01, approximately 1.34×10¹⁷) rather than the QPC tick domain (approximately 3.7×10¹⁰ on a freshly booted system). Phase 2 of `TriggerEngine::initiate_trigger_atomic()` stored `post_target_qpc_` using the ETW FileTime value, anchoring the post-window deadline roughly 1.34×10¹⁷ ticks in the future. Because `poll_state()` compares this deadline against the real wall-clock value returned by `get_current_qpc()`, the condition `current_qpc >= post_target_qpc_` would not become true for approximately 425 years of continuous uptime. As a result, the state machine entered `COLLECTING_POST` on the first triggered frame and never advanced.

**Fix:** Phase 2 now uses `get_current_qpc()` (real wall-clock QPC) for both `post_target_qpc_` and `claimed_timestamp_qpc_`. The original ETW-domain timestamp is retained in `active_trigger_.trigger_timestamp_qpc` (Phase 3) to preserve correct flight-recorder snapshot windowing.

**Verification:** Validated against a live Spider-Man 2 session, producing 26 reports in 15 seconds post-fix, with the first report generated within approximately 2 seconds.

### Warmup Starvation Fix (High-Refresh Displays)

The `DYNAMIC_ONLY` and `HYBRID` warmup branches were discarding frames above the static threshold without recording them in the rolling statistics. On a 200 Hz display (5 ms threshold, ~8 ms actual frame time), this caused every warmup frame to be dropped, leaving `sample_count` permanently at 0 and preventing dynamic detection from ever activating.

This has been corrected by pushing a clamped sample before returning. The trigger continues to be suppressed correctly during warmup, but the baseline now accumulates as intended, allowing warmup to complete.

### Regression Testing

A new test, `test_filetime_domain_timestamp_does_not_block_poll_state()`, feeds a genuine FileTime-domain ETW timestamp (`134345709646471285ULL`, captured from an actual run) into `on_dxgi_present()` and confirms that `poll_state()` fires within the post-window using real QPC time. This test fails against pre-fix code and passes against the corrected implementation.

A `STUTTO_ASSERT_MSG(expr, msg)` macro was also added to support descriptive assertion messages on test failure.

### Test Suite Status

All 16 unit test suites pass following this fix. Timestamp-domain anchors in `test_correlator.cpp`, `test_etw_session.cpp`, `test_frame_pacing.cpp`, and `test_kernel_present_tracking.cpp` were updated to reflect the corrected behavior.

### Files Changed

- `src/trigger_engine.cpp` — Phase 2 timestamp domain fix; Phase 4 `frozen_timestamp_qpc_` also updated to use `now_qpc`
- `include/stuttometer/frame_pacing_tracker.hpp` — warmup clamped-sample push for `DYNAMIC_ONLY` and `HYBRID`
- `tests/test_frame_pacing.cpp` — regression test and warmup test updates
- `tests/test_kernel_present_tracking.cpp` — QPC-domain timestamp anchors
- `tests/test_etw_session.cpp` — QPC-domain timestamp anchors
- `tests/test_correlator.cpp` — QPC-domain timestamp anchors