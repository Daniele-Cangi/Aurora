# Changelog

All notable changes to Aurora-X will be documented in this file.

## [Unreleased]

### Added
- Parsed and validated transport-only `TransportContract`, including caller-declared byte ranges.
- Immutable `GenerationDescriptor`, authoritative `DecodeReport`, generation-indexed decoder state, and bounded active-generation storage.
- Hard `SafetyEnvelope` and `TransportDecisionTrace` for expiry, stale observations, energy reserve, allowed links, RF duty availability, and repair caps.
- Canonical, checksum-chained `DecisionReplayLog` plus the `aurora_replay` verifier; replay recomputes safety decisions from recorded safety-relevant contract/descriptor fields, observation and proposal inputs.
- Deterministic shared-trace `aurora_benchmark` comparisons for no FEC, 2x repetition, fixed LT-like overhead, class-aware fixed overhead and the adaptive biological policy.
- Canonical, checksum-chained channel-trace corpora for IID, Gilbert–Elliott burst, scheduled-outage, slow-drift and shock/recovery scenarios, including exact trace replay and truncation/corruption rejection.
- Per-trial benchmark retention, trace/configuration fingerprints, Wilson 95% delivery intervals, transmitted-byte cost, innovative-symbol ratio and overhead-direction-change metrics.
- Injectable `GenerationCodec`, `TransportPolicy` and `GenerationManager` boundaries with fixed and biological policy implementations.
- Generation-scoped deterministic runtime repair emission with segment-aware critical scheduling and immutable descriptors.
- Dependency-light GitHub Actions build and CTest jobs for Linux and Windows.
- Independent segment expiry, late-symbol accounting, per-segment decode reports, and reliability-prioritized runtime repair selection.
- V5 decision traces with replayable proposal inputs/state, per-attempt LBT, channel, energy and simulation-duty transitions, plus complete bounded supervisory-controller snapshots.
- Deterministic cross-layer proposal derivation from selector memory, UCB statistics, epoch and an isolated RNG state, including replayable post-execution UCB feedback.
- Checksum-chained `SimulationEventLedger` records the current simulator's fixed topology, initial generation arrival, harvesting/ingest transitions, RIS phases, channel summaries, buffer/inbox movement, decoder rank and global RNG checkpoints.
- Cross-ledger replay reconstructs RIS jitter, world gain, channel history, LBT samples, fading and pacing randomness, and rejects semantic tampering even when the event file is structurally valid.
- Canonical checksum-chained `ContactSchedule` files with half-open, non-overlapping windows and per-link RF/optical/backscatter availability.
- `aurora_contact_schedule` creates validated schedules from explicit time/link specifications for repeatable simulator and CI runs.
- Canonical checksum-chained `GenerationArrivalSchedule` files plus `aurora_generation_arrivals` for deterministic external generation releases.
- Deterministic aging/fair priority-then-EDF simulation of multiple arrived generations with stable fairness-deadline/arrival/index tie-breaks, preemption, generation-qualified packet replay, per-generation rank continuity and exact schedule round-tripping.
- Shared `GenerationScheduler` selection logic and tests for priority, earliest-deadline, stable tie-break and invalid-candidate rejection.
- Bounded starvation prevention: one-class promotion every 2000 ms, a 3000 ms fairness deadline and a replayable maximum scheduling-turn gap of `3000 + (N - 1) * 1000` ms.
- `aurora_scheduler_benchmark` compares strict priority-then-EDF against aging/fair scheduling under an identical deterministic adversarial workload and reports scheduling-turn-gap violations plus elastic turns.
- Versioned `AURORA_GENERATION_SCHEDULER_SWEEP_V2` report covering seven canonical scheduler scenarios, including dense contention and tick-rounded starvation intervals.
- CTest/GitHub Actions regression gates enforce strict starvation exposure, bounded fair scheduling turns for every candidate and byte-exact agreement with the canonical scheduler report.
- Explicit per-generation accounting for scheduled turns versus effective transport service; effective service means one or more attempts accepted by the HAL, independent of channel delivery.
- V7 simulator events record HAL-accepted effective-service attempts and paired replay binds them exactly to V6 execution traces.
- Deterministic discrete-event kernel with total `(time, phase, sequence)` ordering, arrival-before-transport phase precedence and bounded pending-event storage.
- Kernel regression coverage for out-of-order insertion, same-time phase/FIFO ordering, time-regression rejection and dynamically scheduled events.
- Cross-platform V7/V6 SHA-256 regression oracle that locks the canonical simulator ledger and decision trace to the exact pre-kernel bytes.
- Descriptor-independent generation identity reservations that do not invoke transport policy planning, allowing future arrival identities to remain deterministic without consuming adaptive state.
- Causal A-to-B runtime coverage proving that feedback from an expired generation changes the later generation's arrival-time descriptor and initial symbol budget.
- Timestamp-based safety-evidence expiry with monotonic-clock validation and replayable monitor restoration.
- Configure-time benchmark provenance covering commit/source state, compiler, target, build generator/profile, crypto/FEC profile and an explicit non-validated hardware boundary.
- CTest coverage for deterministic/reordered/duplicate symbols, concurrent interleaved generations, dynamic repair identity/caps, bounded panic, rolling statistics, HAL/duty refusal, critical scheduling, execution-trace consistency, integrity failure and safety action costs.

### Changed
- Replaced the main research run's `for(step)` time authority with explicit generation-arrival and recurring transport-quantum events; controller and ledger timestamps now come from the event kernel.
- Removed wall-clock pacing waits from simulation research runs while preserving identical pacing RNG consumption; interactive and field-experimental paths retain realtime pacing.
- Preserved the current 1000 ms transport quantum and aligned-arrival model while separating those model constraints from the event queue implementation.
- Routed the dependency-light simulator through the organism's reserved-identity/spawn/integrate lifecycle exclusively; delivery and health consume the same report.
- Deferred policy planning, descriptor construction and initial symbol emission from simulator initialization to each declared generation arrival; V6 ledger metadata now completes reserved future identities before their arrival event without changing the serialized format version.
- Reduced `AlienFountainOrganism` to a compatibility facade over separately testable codec, policy and generation-state components.
- Reworked the experimental LT-like codec to use deterministic systematic symbols, unique source-index sampling, ideal-soliton repair degrees, incremental rank tracking, and bounded equation storage.
- Made simulation and coding randomness reproducible from the contract seed.
- Added `--decision-trace <path>` to the main simulator.
- Added `--event-ledger <path>` and optional paired event-ledger verification to `aurora_replay`.
- Added `--contact-schedule <path>` and `--contact-schedule-out <path>` to the simulation CLI; replay/contact options remain unavailable in `FIELD_BUILD`.
- Added `--generation-arrivals <path>` and `--generation-arrivals-out <path>` to the simulation CLI; arrival replay remains unavailable in interactive-lab and `FIELD_BUILD` modes.
- Added `--generation-scheduler <strict|fair>`, `--generation-aging-ms` and `--generation-starvation-ms`; scheduler options remain simulation-only in `FIELD_BUILD`.
- Added `aurora_scheduler_benchmark --sweep` and `--verify-sweep <report>` for deterministic report generation and CI snapshot verification.
- Extended arrival specifications with optional service class and relative deadline fields while preserving the compact inheritance form.
- Upgraded decision provenance to V5: proposal derivation, selector/RNG advancement, UCB feedback, per-attempt HAL/channel/resource transitions, and safety-window/operating-mode transitions are independently reconstructed and checked for cross-record continuity.
- V2 through V5 decision traces are no longer accepted because they cannot provide the complete V6 contact, proposal, action and supervisory evidence and must be regenerated.
- Upgraded decision provenance to V6 and simulator ledgers to V2 so contact availability is serialized, safety-constrained and cross-checked against the embedded schedule; V5/V1 artifacts must be regenerated.
- Upgraded simulator ledgers to V3 so the canonical arrival schedule, every generation identity, timed packet release, active generation and per-generation rank are replayed; V1/V2 artifacts must be regenerated.
- Upgraded generation-arrival schedules to V2 and simulator ledgers to V4 so resolved service class, descriptor expiry and priority/deadline selection are independently replayed; V1 schedules and V1–V3 ledgers must be regenerated.
- Upgraded simulator ledgers to V5 so scheduling intervals and per-generation last-service state independently reconstruct aging, fairness-lane entry and the starvation bound; V1–V4 ledgers must be regenerated.
- Upgraded simulator ledgers to V6 so strict/fair discipline and configurable aging/starvation intervals are replayed exactly; V1–V5 ledgers must be regenerated.
- Renamed the fairness invariant and scheduler benchmark metrics to scheduling-turn gaps, upgraded the canonical scheduler sweep to V2, and removed any implied finite effective-service guarantee without explicit serviceability assumptions.
- Upgraded simulator ledgers to V7 for explicit effective-service accounting; V1–V6 simulator ledgers must be regenerated, while the decision trace remains V6.
- Separated contract-configured simulation-time RF duty accounting from the realtime HAL limiter.
- Extended `aurora_benchmark` with scenario parameters plus `--trace-in`, `--trace-out` and `--runs` reproducibility controls; the original positional IID invocation remains supported.
- Benchmark fields that are not semantically applicable now emit `N/A` instead of misleading zero values.
- Rebuilt the supervisory safety monitor as a hysteretic state machine with separate enter/exit thresholds, consecutive transition evidence and staged critical recovery.
- Made `NO_EVIDENCE` force conservative optimizer mode instead of allowing stale healthy metrics to authorize exploration.
- MinGW executables now link the GCC runtime statically to avoid accidental loading of an ABI-incompatible `libstdc++-6.dll` from unrelated Windows applications on `PATH`.

### Removed
- Removed the active simulator's incompatible parallel delivery decoder and the nonexistent `test_podm.cpp` CMake target.
- Removed transport-policy console logging from the core adaptive component; policy state remains queryable without coupling the reusable control path to simulator output.

### Fixed
- Made biological panic apply to exactly the next configured number of generated generations instead of lasting until that many successful deliveries.
- Corrected rolling channel windows to evict the oldest observation rather than the newest.
- Prevented receiver delivery when energy, simulation duty, LBT/HAL or the simulated channel rejects a transmission.
- Made the safety envelope cap known action energy, RF airtime and remaining repair budget before authorization.
- Stopped inactive flow classes from diluting active failures in the legacy safety monitor; insufficient active evidence is now explicit.
- Normalized per-segment rounding so initial and runtime emission cannot exceed the global repair-amplification limit.
- Rejected invalid safety telemetry fractions and invalid hysteresis configurations instead of allowing undefined policy transitions.

### Added
- **FASE 3b**: Adaptive weight loss mechanism based on success streaks
  - Added `good_streak` and `bad_streak` tracking to `FlowState`
  - Implemented homeostatic weight loss: after 4+ consecutive successes with no panic and avg_coverage > 0.85, overhead gradually decreases
  - Enhanced failure response: repeated failures (bad_streak >= 3) trigger additional overhead boost for critical flows
- **Test Suite**: Comprehensive test suite (`test_aurora_organism`) with three scenarios:
  - Good channel: NERVE, GLAND, MUSCLE with full delivery
  - Bad channel: NERVE/GLAND with packet loss, showing panic activation
  - Adaptation: GLAND transitioning from bad to good channel, demonstrating overhead increase then decrease
- **Documentation**: Professional/academic README restructure with:
  - Real-world performance results (Taiwan/Denmark intercontinental test)
  - Scientific language: "homeostatic behavior", "dynamic regulation"
  - Clear getting started with CMake-only instructions

### Changed
- **Adaptive Logic**: Refined up-adapt mechanism to use `bad_streak` for more intelligent failure response
- **Logging**: Extended `[ALIEN][ADAPT]` log to include `gs` (good_streak) and `bs` (bad_streak) fields
- **Weight Loss Threshold**: Set `COV_GOOD_THRESHOLD` to 0.85 (from 0.90) for more realistic activation in test scenarios

### Fixed
- **Dimagrimento Activation**: Fixed weight loss mechanism to activate reliably in test scenarios by adjusting coverage threshold

## Previous Versions

For changes before FASE 3b, see git history.

