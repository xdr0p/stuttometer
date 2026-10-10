# AGENTS.md — Standing Architectural & Engineering Invariants

This document defines standing invariants and operational constraints for automated agents and engineers working on the Stuttometer codebase.

---

## 1. Architectural & Memory Layout Invariants

- **Source of truth for struct sizes is the `static_assert` in the header**, not documentation or external notes. Whenever modifying structs, update the assert and the struct fields/offsets together.
- **Strict Layout Guarantees:**
  - `FilteredEvent`: Exactly 56 bytes (`alignof == 8`), tail-padded with explicit `_pad[9]`.
  - `TriggerInfo`: Exactly 64 bytes (`alignof == 8`, `_pad1` must be zero).
  - `RollingFrameStats`: Exactly 384 bytes (6 cache lines, `alignas(64)`).
  - `FlightRecorder::Slot`: Exactly 64 bytes (`alignas(64)`).
  - `EtwEventRecord`: Exactly 56 bytes.
- **Core headers must NEVER include `<windows.h>`.** Keep platform abstractions and Win32 isolated strictly to implementation (`.cpp`) files or dedicated platform utilities.

---

## 2. ETW Hot Path & Concurrency Invariants

- **Hot paths must never allocate.** `classify_severity`, `evaluate_frame_pacing`, `record_filtered_event`, and `FilteredEventRing::push` run directly on ETW callback producer threads. They must perform zero heap allocations, zero blocking synchronization, and no Win32 calls.
- **`classify_severity` is strictly allocation-free and lock-free.** It evaluates candidates on the ETW producer thread and must never take locks or allocate memory.
- **`FilteredEventRing` is MPSC lock-free.**
  - Producers claim a monotonic write ticket via `write_pos_.fetch_add(1, std::memory_order_relaxed)`.
  - Stale generations are rejected using the writer-relative check (`cur_w > w && (cur_w - w) >= CAPACITY`).
  - Slots are claimed atomically via `compare_exchange_weak` into an odd sequence before publishing, and events are written strictly inside the CAS-success branch.
  - Readers must never advance `read_pos_` past an in-flight cell (`seq1 < expected_writing`), and must `continue` (never prematurely abort the drain loop) on lapped or torn cells.

---

## 3. Pacing & Judder Detection Rules

- **`judder_max_swing_q100` represents ratio × 100.** 50% swing is encoded as `50`, 60% swing is `60` (integer percentage), not 5000 or basis points.
- **Judder episodes anchor on the last alternation timestamp.** Cadence judder collapses into episodes with end-emission. The correlator window anchors at `trigger_timestamp_qpc` (= `last_alt_qpc`), ensuring the pre-window captures the episode and its cause rather than post-episode clean frames.
- **Cadence state bridging:** `reset_cadence_state(RollingFrameStats& stats)` preserves `last_delta_us` while an episode is active (`judder_episode_active == 1`), bridging across relative spikes and threshold checks.

---

## 4. Policy, Presets & Reporting

- **Detection is decoupled from reporting.** The detector remains sensitive and accurate; the pre-claim severity gate in `initiate_trigger_atomic` filters triggers before they touch the state machine or trigger cooldown.
- **Session Benchmark (Schema 1.3):**
  - `stutters_detected` counts `WARNING` and `DANGER` severity events only.
  - `minor_stutters` counts filtered triggers (`NORMAL` severity or vblank floor drops).
  - `total_triggers = stutters_detected + minor_stutters` provides backward-compatible continuity with pre-1.3 benchmark totals.
- **Shared Constants:** Do not add constants to `constants.hpp` unless they are shared across multiple subsystems. Subsystem-specific constants belong in internal headers or tuning namespaces.

---

## 5. Testing & Modification Rules

- **Default assumption: a failing test means the implementation is wrong, not the test.** Only override this assumption after explicit diagnosis (see two-phase protocol below).
- Every commit must compile cleanly under MSVC x64 and pass 100% of CTest test suites (`ctest --test-dir build -C Release --output-on-failure`).
- **Two-phase protocol — mandatory for all implementation changes (bug fixes and new features):**
  - **Phase 1 (implement):** Apply source changes to `src/` and `include/` only. Do NOT touch any file under `tests/` in this phase, regardless of test failures.
  - **Phase 2 (diagnose failures):** If tests fail after Phase 1, report the exact failure output and classify the cause as one of:
    - **Regression** — the change introduced a bug that the test correctly caught. Fix the implementation, not the test.
    - **Intentional behavior change** — existing behavior was deliberately altered. The test needs updated expectations.
    - **New feature gap** — new functionality changed a signature, struct, or output that the test references but the test has no coverage for the new behavior yet. The test needs mechanical fixes (compilation) or new test cases.
  - **Test updates require explicit user confirmation** that the behavior change or new feature gap is intentional before any `tests/` file is modified. Never update a test and its corresponding implementation in the same step without this confirmation.

---

## 6. Execution & Plan Management Protocol

### 1. Operational Modes
- **Plan Review Mode:** When asked to review a plan, perform a read-only audit against existing workspace code and invariants (struct layouts, `static_assert` checks, hot-path rules). Report any gaps, signature mismatches, or risks without modifying files or spawning subagents.
- **Plan Execution Mode:** When executing an approved plan, follow the specification line-by-line. Do not introduce unsolicited refactoring, extra dependencies, or architectural improvisations.

### 2. Phased Execution Workflow
- **Phase Decomposition:** Organize execution into discrete, logical phases based on module boundaries (e.g., Header/Types -> Engine Logic -> Integration).
- **Single Phase per Turn:** Execute exactly one logical phase per turn. Once the phase is implemented, stop immediately, report the changes, and wait for confirmation before proceeding to subsequent phases.

### 3. Verification & Context Hygiene
- **Deterministic Build & Test:** Run build and test commands strictly in single-pass mode (`cmake --build build --config Release`, `ctest --test-dir build -C Release --output-on-failure`). Never start watch tasks or background daemons.
- **Local Plan Scratchpads:** Read large plans from local workspace files (e.g., `PLAN.md` or `scratch/PLAN.md`) using line-ranged reads to keep context minimal.
- **Tool Restraints:** Avoid spawning exploratory subagents or background loops during mechanical execution turns.
- **Fresh Context Transitions:** Suggest a fresh conversation after completing major multi-phase milestones to maintain crisp reasoning.

---

## 7. Protected Core Files & Workflow Tooling

- **Workspace Tooling Invariant:** `repomix.config.json` and `split_codebase.ps1` in the repository root are permanent, core developer workflow files. Never delete, relocate, or modify them during cleanups or refactorings unless explicitly instructed by the user.

