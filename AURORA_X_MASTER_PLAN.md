# Aurora-X Master Plan

## Maximum-Scope Engineering and Research Program

Aurora-X is not being reduced to a small codec demonstration.

The long-term objective remains the construction of an adaptive transport substrate for heterogeneous, energy-constrained, disruption-prone networks: a system that accepts application-declared transport requirements, protects declared byte classes, adapts coding and link behaviour, preserves safety constraints, retains custody across interruptions, and recovers from shocks without requiring continuous central control. Aurora does not infer payload semantics.

This document defines how to reach that objective without confusing architectural ambition with already validated capability.

---

# 1. North Star

Aurora-X should eventually accept an application transport contract such as:

```text
Deliver this state update before 800 ms.
Target probability of complete delivery: 0.999.
The first 192 bytes are safety-critical.
Source energy must not fall below 18%.
EU868 duty constraints must be respected.
RF, optical and backscatter interfaces may be used.
Store-and-forward custody is allowed.
All generation metadata and delivery evidence must be authenticated.
```

The system should transform that contract into a sequence of bounded, observable decisions:

```text
Application transport requirements
    ↓
TransportContract
    ↓
Generation structure and coding policy
    ↓
Link, route, custody and timing decisions
    ↓
Observed channel and decode evidence
    ↓
Health state and safety supervision
    ↓
Constrained adaptation for the next action/generation
```

Aurora-X reaches its maximum form when this loop works across multiple nodes, real heterogeneous links, intermittent connectivity and hardware-backed security while remaining reproducible in a digital twin.

---

# 2. Foundational invariants

These rules define the project more strongly than any implementation detail.

## 2.1 One generation, one source of truth

Every encoded generation must carry or reference an immutable `GenerationDescriptor` containing at least:

- protocol version;
- generation ID;
- token/bundle ID;
- encoder family and version;
- source payload length;
- symbol size;
- number of source symbols;
- critical/bulk segment boundaries;
- redundancy policy used at spawn time;
- creation and expiry times;
- content or descriptor integrity hash;
- optional signature/provenance data.

The encoder, channel, decoder, health layer and replay system must all interpret the same descriptor.

## 2.2 Delivery state is factual

A flow is delivered only when the designated decoder reconstructs the required bytes and validates their integrity. Health logic must consume the resulting `DecodeReport`; it must not run a second incompatible decoder to estimate delivery.

## 2.3 Adaptation is bounded control

Adaptive policies may change only explicit action variables within declared bounds:

- number of repair symbols;
- protection split between segments;
- transmission attempts;
- selected link or link combination;
- pacing and scheduling;
- custody/replication policy;
- optional RIS configuration.

Every change must record its inputs, previous state, action, bounds and reason.

## 2.4 Safety dominates optimization

The optimizer may seek throughput, energy savings or exploration only inside constraints imposed by the safety supervisor. An exploratory policy cannot silently override:

- minimum energy reserve;
- regulatory duty limits;
- critical traffic protection floor;
- maximum latency or stale-data policy;
- cryptographic verification requirements;
- forbidden link or route policy.

## 2.5 Evidence levels never collapse into one another

Aurora-X distinguishes:

1. deterministic unit verification;
2. stochastic simulation;
3. network emulation with real processes and impaired links;
4. hardware-in-the-loop;
5. physical field experiments.

A result from one level is not presented as proof of another.

---

# 3. Target system architecture

```text
┌─────────────────────────────────────────────────────────────┐
│                         Application                         │
└─────────────────────────────┬───────────────────────────────┘
                              │ TransportContract
┌─────────────────────────────▼───────────────────────────────┐
│ Contract Parser / Validator                                  │
│ deadline, reliability, critical ranges, energy, policy      │
└─────────────────────────────┬───────────────────────────────┘
                              │ validated requirements
┌─────────────────────────────▼───────────────────────────────┐
│ Generation Manager                                          │
│ segmentation, codec, source/repair schedule, provenance     │
└─────────────────────────────┬───────────────────────────────┘
                              │ EncodedSymbol
┌─────────────────────────────▼───────────────────────────────┐
│ Custody and Disruption Layer                                │
│ storage, expiry, deduplication, forwarding, acknowledgments │
└─────────────────────────────┬───────────────────────────────┘
                              │ Candidate actions
┌─────────────────────────────▼───────────────────────────────┐
│ Safety Supervisor                                           │
│ invariants, state machine, emergency and recovery policy    │
└─────────────────────────────┬───────────────────────────────┘
                              │ Safe action envelope
┌─────────────────────────────▼───────────────────────────────┐
│ Cross-Layer Optimizer                                       │
│ coding, link, pacing, routing, energy and RIS decisions      │
└─────────────────────────────┬───────────────────────────────┘
                              │ LinkAction
┌─────────────────────────────▼───────────────────────────────┐
│ Link Abstraction                                            │
│ RF | optical | backscatter | wired | satellite | future     │
└─────────────────────────────┬───────────────────────────────┘
                              │ observations
┌─────────────────────────────▼───────────────────────────────┐
│ Estimation and Health                                       │
│ channel state, decode progress, energy, failures, confidence│
└─────────────────────────────┬───────────────────────────────┘
                              │ state/evidence
┌─────────────────────────────▼───────────────────────────────┐
│ Telemetry, Replay and Digital Twin                          │
│ event log, deterministic replay, benchmark and verification │
└─────────────────────────────────────────────────────────────┘
```

---

# 4. Immediate stabilization — start here

This phase does not reduce the vision. It removes contradictions that prevent the existing architecture from being trusted.

## Phase 0A — Repository integrity

### Work

- [x] Rewrite the README around achieved foundations and maximum vision.
- [x] Add this master plan.
- [ ] Remove unresolved merge markers and obsolete instructions.
- [x] Establish one supported C++ standard: C++20.
- [x] Make the dependency-light internal-FEC build the default developer profile.
- [x] Make real crypto and external RaptorQ explicit opt-in profiles.
- [x] Register executable tests with CTest.
- [x] Preserve assertions or replace them with test-framework checks in optimized test builds.
- [ ] Add CI for Windows and Linux using the dependency-light profile.
- [ ] Move `Orginal/` into `docs/history/` or a tagged historical branch.
- [ ] Add a machine-readable version and build-feature report.

### Exit gate

A clean clone can configure, build, test and run the core simulator using documented commands on at least Windows and Linux.

## Phase 0B — Freeze the current behaviour

Before changing the loop:

- [ ] create fixed-seed golden scenarios;
- [ ] capture stdout, JSONL and delivery outcomes;
- [ ] record current success/failure behaviour;
- [ ] store representative payloads including non-symbol-aligned sizes;
- [ ] preserve current dashboard screenshots and concept art;
- [ ] label the baseline as historical, not correct-by-definition.

### Exit gate

Future refactors can show exactly what behaviour changed and why.

---

# 5. Coherent adaptive FEC loop

This is the first decisive engineering transformation.

## Phase 1A — GenerationContext

Introduce immutable structures:

```cpp
struct SegmentDescriptor {
    SegmentKind kind;
    std::size_t offset;
    std::size_t length;
    std::uint32_t source_symbols;
    double requested_overhead;
};

struct GenerationDescriptor {
    ProtocolVersion protocol;
    GenerationId generation_id;
    TokenId token_id;
    CodecId codec;
    std::size_t payload_length;
    std::size_t symbol_size;
    std::vector<SegmentDescriptor> segments;
    FlowClass flow_class;
    ProtectionPolicy policy;
    Hash descriptor_hash;
};
```

`AlienFountainOrganism` must no longer store `_K_critical`, `_K_bulk` or payload sizes as global mutable fields. State must be either:

- carried by `GenerationDescriptor`; or
- stored in a bounded map indexed by `generation_id`, with expiry and memory limits.

### Required tests

- [x] two tokens spawned before either is integrated;
- [x] interleaved symbols from multiple generations;
- [x] delayed integration of an older token;
- [x] duplicate symbols;
- [x] reordered symbols;
- [x] generation expiry;
- [x] non-aligned payload lengths;
- [x] empty bulk or empty critical segment;
- [x] descriptor mismatch and corruption.

## Phase 1B — One encoder and one decoder path

The main engine must use:

```text
organism.spawn()
   → node buffers/channel
   → organism.integrate()
   → DecodeReport
```

Remove the parallel health decoder. The same `DecodeReport` determines:

- full delivery;
- partial critical delivery;
- rank/coverage;
- symbols received and innovative symbols;
- decode cost;
- integrity state;
- reason for failure.

### Proposed report

```cpp
struct DecodeReport {
    GenerationId generation_id;
    DecodeStatus status;
    bool critical_complete;
    bool payload_complete;
    std::size_t original_bytes;
    std::size_t recovered_bytes;
    std::uint32_t symbols_seen;
    std::uint32_t innovative_symbols;
    std::uint32_t rank;
    double coverage;
    Duration decode_time;
    IntegrityStatus integrity;
    FailureReason failure_reason;
};
```

### Exit gate

There is no path where the transport decoder declares success while FlowHealth observes a decode failure for the same generation.

**Status:** achieved for the dependency-light internal LT-like simulator. The disabled legacy RaptorQ experiment has not yet been adapted to the descriptor/report interface.

## Phase 1C — Correct the LT-like implementation

- [x] sample source indices without replacement inside each encoded symbol;
- [x] record effective degree explicitly;
- [x] add deterministic seeding scoped to generation and symbol ID;
- [x] implement a documented degree distribution;
- [ ] add robust soliton as an experimental policy;
- [x] track innovative versus dependent packets;
- [x] preserve exact original payload length;
- [x] define maximum generation size;
- [ ] add sparse/peeling decode path before dense elimination;
- [x] bound retained decoder equations and active generation state;
- [ ] fuzz malformed seeds, degree values and symbol lengths.

### Exit gate

For declared generation sizes, the codec has measured decode probability, overhead and complexity curves across many seeds.

---

# 6. Control system reconstruction

## Phase 2A — Replace metaphor-only variables with control semantics

The biological names remain available in presentation, but implementation types should state what they control.

Examples:

```text
panic_boost        → transient_redundancy_boost_generations
crit_overhead      → critical_repair_ratio
bulk_overhead      → bulk_repair_ratio
good_streak        → consecutive_success_generations
bad_streak         → consecutive_failed_generations
avg_coverage       → coverage_ewma
```

## Phase 2B — Flow health model

Each active flow class needs an explicit state with:

- observation timestamp;
- sample count and confidence;
- complete-delivery rate;
- critical-segment delivery rate;
- coverage/rank progression;
- innovative-symbol ratio;
- latency and deadline outcomes;
- transmitted overhead;
- energy cost;
- consecutive success/failure history;
- age of last valid observation.

Inactive classes must be marked `NO_EVIDENCE`, not represented as healthy zero-failure classes.

## Phase 2C — Safety supervisor as a true state machine

Implement class-aware transitions:

```text
HEALTHY
  ├─ sustained warning evidence → DEGRADED
  └─ severe shock invariant     → CRITICAL

DEGRADED
  ├─ worsening evidence         → CRITICAL
  └─ sustained recovery         → HEALTHY

CRITICAL
  └─ minimum recovery evidence  → DEGRADED
```

The supervisor must use separate enter and exit thresholds, minimum dwell times, observation confidence and explicit recovery conditions.

### Safety invariants

- NERVE protection cannot fall below its contract floor.
- GLAND integrity failure cannot be hidden by MUSCLE success.
- No optimization occurs on stale health evidence.
- Energy reserve and duty constraints cannot be violated by panic response.
- CRITICAL mode cannot transition directly to aggressive exploration.

**Current status:** the supervisory monitor now preserves `NO_EVIDENCE`, excludes inactive flow classes, validates telemetry fractions, expires timestamped evidence against a monotonic simulation clock, uses separate enter/exit thresholds and consecutive transition evidence, requires `CRITICAL -> DEGRADED -> HEALTHY` recovery, and forces conservative operation when evidence is missing. Its complete bounded evidence window, clock, pending hysteresis transition and operating-mode transition are replayed before/after every recorded action. The hard `SafetyEnvelope` remains authoritative for per-action constraints. Confidence calibration and field-derived freshness limits remain pending.

## Phase 2D — Policy comparison

Implement interchangeable policies:

- fixed overhead;
- threshold adaptation;
- current Aurora biological policy;
- PID-style bounded controller;
- risk-sensitive controller;
- optional contextual bandit;
- future model-predictive controller.

The biological policy earns its place by outperforming or stabilizing better than explicit baselines, not by terminology.

**Current status:** the codec, generation manager and transport policy are now separate interfaces. Fixed and biological policies are injectable and deterministic. Threshold, PID-style and risk-sensitive controllers remain pending. The V6 decision trace reconstructs contact-aware proposal, action and supervisory transitions. A paired V6 simulator ledger reconstructs the current fixed-node stream, including canonical external generation arrivals, configurable strict or aging/fair priority-then-EDF selection, fairness deadlines, the bounded service-gap invariant, declared contact windows, harvesting, ingest, RIS, channel history, global RNG use and generation-qualified packet arrivals. Weighted-share policies and concurrent/mobile nodes remain pending.

### Exit gate

Each controller is deterministic under a fixed event stream, independently testable and produces a complete decision trace.

---

# 7. Scientific benchmark program

Aurora-X requires a benchmark framework, not isolated successful runs.

## Phase 3A — Baselines

At minimum compare:

1. no FEC;
2. fixed repetition;
3. fixed LT-like overhead;
4. class-aware fixed overhead;
5. adaptive Aurora policy;
6. Reed–Solomon or another block-code baseline where appropriate;
7. RaptorQ baseline when a verified implementation is available.

**Current status:** items 1–5 use explicit replayable traces with shared transmission-slot outcomes. A separate deterministic adversarial scheduler harness now compares strict priority-then-EDF with configurable aging/fair scheduling on identical candidates and reports service-gap bound violations. Items 6–7 and externally valid comparative conclusions remain pending.

## Phase 3B — Channel scenarios

- IID random loss;
- Gilbert–Elliott burst loss;
- scheduled outages;
- slowly drifting channel;
- abrupt shock and recovery;
- asymmetric forward/reverse links;
- reordering and duplication;
- intermittent contact windows;
- deadline-limited transmissions;
- energy depletion and harvesting;
- multi-link disagreement;
- stale channel estimates.

**Current status:** IID, Gilbert–Elliott burst loss, scheduled outages, slow drift and abrupt shock/recovery are implemented as canonical deterministic benchmark traces. The main orchestrator now accepts canonical per-link `ContactSchedule` windows and replays them end-to-end. Contact-aware comparative baselines, asymmetric links, reordering/duplication, explicit deadline scheduling, benchmark energy/harvesting, multi-link disagreement and stale estimates remain pending.

## Phase 3C — Experimental discipline

For every scenario:

- shared channel seeds across policies;
- hundreds or thousands of repetitions where needed;
- declared hardware/compiler/build;
- confidence intervals;
- complete failed-run retention;
- configuration and commit hash in every output;
- deterministic replay of individual runs.

**Current status:** shared trace slots, exact trace import/export, checksum-chain integrity, per-trial failed/successful records, configuration/trace fingerprints, Wilson 95% delivery intervals, and configure-time commit/compiler/target/build provenance are implemented. Outputs explicitly distinguish the simulation evidence level from the `field-experimental` build profile and never mark current hardware as validated. Authenticated provenance, larger prescribed repetition counts and distributional latency/energy statistics remain pending.

## Primary metrics

- payload delivery probability;
- critical-segment delivery probability;
- deadline success probability;
- useful goodput;
- bytes and symbols transmitted per delivered byte;
- innovative-symbol ratio;
- median, p95 and p99 decode latency;
- energy per delivered byte;
- energy reserve violations;
- duty-cycle consumption;
- panic frequency and duration;
- adaptation settling time;
- controller overshoot and oscillation;
- recovery delay;
- false panic rate;
- unresolved/aborted generation rate;
- CPU and memory cost.

### Exit gate

Aurora-X can state, with plots and uncertainty, where adaptation helps, where it does not and what it costs.

---

# 8. Simulation fidelity and digital twin

## Phase 4A — Modular models

Replace hard-coded formulas with versioned interfaces:

```cpp
class ChannelModel;
class EnergyModel;
class DutyCycleModel;
class MobilityModel;
class RISModel;
class LinkModel;
class ContactPlan;
```

Every model declares:

- units;
- assumptions;
- parameter bounds;
- stochastic state;
- source/reference;
- calibration status;
- version.

## Phase 4B — Event-driven simulation

Move from wall-clock sleeps toward a deterministic discrete-event core for research runs.

Advantages:

- faster-than-real-time sweeps;
- exact replay;
- no scheduler noise in simulated latency;
- concurrent nodes and contact windows;
- clean separation between simulated and physical time.

A wall-clock mode remains for dashboard, emulation and hardware integration.

## Phase 4C — Digital-twin replay

Telemetry from emulation or hardware should be importable as a channel/contact trace. Aurora-X can then replay the same observed conditions against different policies.

The current V6 `DecisionReplayLog` derives each proposal from recorded channel summaries, UCB/channel-selector state, epoch and isolated RNG state; verifies contact-aware safety decisions; replays each admitted action; applies UCB feedback; and reconstructs the bounded supervisory transition. The paired V6 `SimulationEventLedger` embeds canonical `ContactSchedule` and V2 `GenerationArrivalSchedule` inputs plus strict/fair discipline, the fixed 1000 ms service quantum and configurable aging/fairness intervals. It binds records to fixed node topology, immutable generation identities, resolved service class and descriptor expiry, then independently reconstructs timed generation release, last-service clocks, configured priority-then-EDF selection, the tick-rounded `ceil(starvation_ms / 1000) * 1000 + (N - 1) * 1000` ms fair service-gap bound, contact availability, inter-step harvesting/ingest, RIS phases, world gain, channel history, LBT/fading/pacing randomness, generation-qualified inbox arrivals and per-generation state continuity. This covers preemptible external arrivals with configurable bounded starvation prevention in the implemented fixed-node simulator. Concurrent/mobile nodes, weighted-share schedulers and imported field/emulation traces remain pending.

### Exit gate

A field or emulation trace can be replayed deterministically and compared across controller versions.

---

# 9. Real network emulation

## Phase 5A — Process-separated nodes

Create independent sender, relay and receiver executables or processes using the same protocol structures as the simulator.

```text
sender process → impaired network namespace → relay → receiver
```

## Phase 5B — Controlled impairment

Linux `tc/netem`, container networks or an equivalent test harness should control:

- loss;
- burst loss;
- delay;
- jitter;
- duplication;
- reorder;
- rate limit;
- outage windows.

## Phase 5C — Real feedback channel

Health and acknowledgments must travel through an explicit reverse channel with its own delay/loss model. The optimizer cannot read receiver state by shared memory in a distributed test.

## Phase 5D — Transport protocol

Define:

- packet header and versioning;
- generation metadata exchange;
- repair request/ack strategy;
- deduplication;
- expiry;
- custody transfer;
- replay protection;
- congestion and pacing behaviour;
- maximum packet and generation sizes.

### Exit gate

Aurora adaptation outperforms or meaningfully differs from fixed policies in a repeatable process-separated emulation.

---

# 10. Security, integrity and provenance

## Phase 6A — No silent crypto fallback

Introduce explicit build/runtime security modes:

```text
INSECURE_TEST
AUTHENTICATED_SOFTWARE_KEY
AUTHENTICATED_HARDWARE_KEY
```

A build without libsodium must display and emit `INSECURE_TEST`; it must never label placeholder signatures as Ed25519.

## Phase 6B — Authenticated descriptors

Sign or authenticate:

- generation descriptor;
- payload hash;
- expiry;
- critical ranges;
- flow contract;
- custody transitions;
- delivery evidence.

Encoded symbols may use efficient per-generation authentication rather than independent public-key signatures per symbol.

## Phase 6C — Threat model

Document resistance and non-resistance to:

- symbol corruption;
- forged generations;
- replay;
- downgrade;
- descriptor substitution;
- malicious repair flooding;
- custody repudiation;
- compromised relay;
- traffic analysis;
- jamming and denial of service.

### Exit gate

Security properties are explicit, tested and tied to concrete key management rather than compilation macros alone.

---

# 11. Delay-tolerant custody and multi-node operation

## Phase 7A — Custody state

Every bundle requires a state machine:

```text
CREATED
ENCODING
IN_CUSTODY
PARTIALLY_FORWARDED
TRANSFER_PENDING
TRANSFERRED
DELIVERED
EXPIRED
ABORTED
```

## Phase 7B — Multi-hop forwarding

- store-and-forward generations;
- partial innovative-symbol forwarding;
- recoding policy where mathematically safe;
- generation deduplication;
- contact-aware scheduling;
- custody acknowledgments;
- expiry and garbage collection;
- bounded storage pressure policy.

## Phase 7C — Distributed adaptation

Avoid every node independently amplifying redundancy. Explore:

- source-only coding control;
- relay feedback aggregation;
- local repair under bounded authority;
- distributed rate allocation;
- consensus or leader-based emergency state;
- prevention of positive-feedback storms.

### Exit gate

Aurora can operate across at least three independent nodes with interruption, delayed custody and no shared internal state.

---

# 12. Heterogeneous physical links

## Phase 8A — Real HAL contracts

Define stable interfaces for:

- radio configuration and transmit/receive;
- optical link;
- backscatter interface;
- RIS controller;
- clock and timestamping;
- energy measurement;
- hardware RNG and key storage.

Mocks and physical adapters must be separate targets.

## Phase 8B — First physical adapter

Choose one constrained, measurable path rather than pretending to implement all links simultaneously. Candidate first target:

- two Linux or microcontroller nodes with a documented LoRa/SX126x adapter;
- host-side Aurora controller;
- measured packet outcomes and current draw;
- optional relay.

## Phase 8C — Hardware-in-the-loop

Connect physical packet and energy measurements back into the simulator/digital twin.

## Phase 8D — RIS and alternative links

RIS, optical and backscatter work only proceeds with:

- an actual adapter or calibrated external model;
- explicit measurement methodology;
- baseline without the component;
- no inference from simulated illumination alone.

### Exit gate

At least one real link has a reproducible end-to-end experiment with measured energy, latency, packet outcomes and decode evidence.

---

# 13. Maximum frontier research

These are not immediate deliverables, but they remain part of Aurora-X's top-level direction.

## 13.1 Online system identification

Estimate channel transition dynamics, contact duration and energy cost online rather than using fixed formulas.

## 13.2 Risk-sensitive and predictive control

Move beyond threshold rules toward policies optimizing expected utility under hard failure constraints:

- constrained model-predictive control;
- chance-constrained optimization;
- distributionally robust control;
- risk-sensitive bandits;
- safe reinforcement learning only after strong baselines exist.

## 13.3 Formal supervision

Specify invariants and verify state transitions using model checking or property-based exploration.

Potential properties:

- critical protection never below contract minimum;
- no AGGRESSIVE transition without fresh healthy evidence;
- bounded adaptation action per generation;
- eventual recovery under sustained good conditions;
- no unbounded redundancy escalation;
- no duty/energy violation caused by panic mode.

## 13.4 Application-declared unequal error protection

Critical/important/elastic segmentation can evolve through application-declared byte ranges and progressive byte layers. Aurora consumes those declarations as transport requirements and does not infer what the ranges mean.

## 13.5 Network coding and recoding

Investigate safe relay recoding, rank-aware forwarding and generation mixing under authenticated metadata.

## 13.6 Contact-plan and orbital integration

For satellite or highly scheduled networks, integrate predicted contact windows, link budgets and custody planning while keeping the core independent of any single domain.

## 13.7 Distributed transport-controller coordination — deferred

Only after the single-node generation and custody lifecycle is trustworthy, investigate how transport controllers exchange bounded link/custody observations without creating instability, central dependency or security exposure. This is not a generic agent architecture.

## 13.8 Verified autonomous recovery

The final goal is not merely higher delivery probability. It is an observable system that can:

- recognize a shock;
- enter a safe constrained regime;
- preserve critical traffic;
- avoid resource collapse;
- gather sufficient new evidence;
- recover gradually;
- explain every transition afterward.

---

# 14. Proposed repository architecture

```text
CMakeLists.txt
cmake/
  presets and dependency discovery

include/aurora/
  protocol/
  fec/
  control/
  safety/
  custody/
  link/
  telemetry/
  crypto/

src/
  protocol/
  fec/
  control/
  safety/
  custody/
  link/
  telemetry/
  crypto/

apps/
  aurora_sim/
  aurora_node/
  aurora_replay/
  aurora_lab_bridge/

tests/
  unit/
  integration/
  property/
  fuzz/

benchmarks/
  scenarios/
  baselines/
  reports/

python/
  aurora_lab/
  analysis/

hardware/
  mocks/
  adapters/

docs/
  architecture/
  protocols/
  models/
  experiments/
  history/
```

---

# 15. Release gates

## Research Core 0.2 — Coherent generation

- single spawn/integrate path;
- per-generation descriptor;
- concurrent token support;
- exact payload-length recovery;
- CTest and CI;
- no incompatible health decoder.

## Research Core 0.3 — Measured adaptation

- fixed-policy baselines;
- shared-seed benchmark harness;
- confidence intervals;
- controller trace and safety state machine;
- documented regions where adaptation helps or hurts.

## Research Core 0.4 — Deterministic simulator

- event-driven execution;
- modular models;
- replayable scenarios;
- telemetry schema and provenance.

## Research Core 0.5 — Network emulation

- process-separated nodes;
- controlled impairment;
- real feedback channel;
- protocol versioning;
- end-to-end adaptive comparison.

## Research Core 0.6 — Secure custody

- authenticated descriptors;
- explicit security modes;
- custody state machine;
- replay and corruption tests.

## Research Core 0.7 — Multi-node disruption tolerance

- sender, relay, receiver;
- interrupted contacts;
- bounded storage;
- distributed repair/custody policy.

## Research Core 0.8 — Hardware-in-the-loop

- one real link adapter;
- measured energy and packet outcomes;
- trace replay into digital twin.

## Research Core 0.9 — Advanced control

- predictive or risk-sensitive policy;
- formal safety invariants;
- comparison with simpler controllers.

## Aurora-X 1.0 Research System

A reproducible, multi-node, secure, heterogeneous adaptive transport research platform with:

- coherent coding and delivery evidence;
- validated safety supervision;
- real and simulated link adapters;
- delay-tolerant custody;
- benchmarked adaptation;
- hardware measurements;
- complete decision provenance;
- published limits and failure cases.

Version 1.0 is not defined by a quantity of features. It is defined by the ability to demonstrate that the complete loop is coherent, bounded, measurable and reproducible.

---

# 16. Immediate execution order

Work should begin in this exact order:

1. repair repository/build/test integrity;
2. freeze fixed-seed baseline behaviour;
3. introduce `GenerationDescriptor` and per-generation state;
4. route the main engine through `organism.spawn()`;
5. make `DecodeReport` the only health truth;
6. add concurrent/interleaved-generation tests;
7. correct LT index selection and payload-length handling;
8. rebuild SafetySupervisor with `NO_EVIDENCE` and hysteresis;
9. implement fixed-policy baselines;
10. produce the first statistically valid adaptive-versus-fixed report.

Only after step 10 should new biological mechanisms, additional links, learning policies or hardware claims be added.

This is not a retreat from Aurora-X's maximum vision. It is the shortest path that makes the maximum vision technically possible.
