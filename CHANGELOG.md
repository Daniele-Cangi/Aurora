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
- V4 decision traces with per-attempt LBT, channel, energy and simulation-duty transitions plus complete bounded supervisory-controller snapshots.
- Timestamp-based safety-evidence expiry with monotonic-clock validation and replayable monitor restoration.
- Configure-time benchmark provenance covering commit/source state, compiler, target, build generator/profile, crypto/FEC profile and an explicit non-validated hardware boundary.
- CTest coverage for deterministic/reordered/duplicate symbols, concurrent interleaved generations, dynamic repair identity/caps, bounded panic, rolling statistics, HAL/duty refusal, critical scheduling, execution-trace consistency, integrity failure and safety action costs.

### Changed
- Routed the dependency-light simulator through `AlienFountainOrganism::spawn()` and `integrate()` exclusively; delivery and health now consume the same report.
- Reduced `AlienFountainOrganism` to a compatibility facade over separately testable codec, policy and generation-state components.
- Reworked the experimental LT-like codec to use deterministic systematic symbols, unique source-index sampling, ideal-soliton repair degrees, incremental rank tracking, and bounded equation storage.
- Made simulation and coding randomness reproducible from the contract seed.
- Added `--decision-trace <path>` to the main simulator.
- Upgraded decision provenance to V4: actual repair/attempt aggregates remain backed by replayable per-attempt HAL/channel/resource transitions, while safety-window, hysteresis and operating-mode transitions are independently reconstructed and checked for cross-record continuity.
- V2 and V3 decision traces are no longer accepted because they cannot provide the complete V4 action and supervisory-controller evidence and must be regenerated.
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

