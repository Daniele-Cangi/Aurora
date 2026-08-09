# Aurora-X

> Bio-inspired adaptive transport and cross-layer control research engine for resilient heterogeneous networks.

Aurora-X is an ambitious systems research project exploring a long-term goal:

> Can a network transport substrate observe its own condition, protect different classes of information according to intent, adapt coding and physical-link decisions under stress, preserve energy and duty-cycle constraints, and recover safely after severe disruption?

The current repository contains a substantial C++20 research implementation covering fountain-style forward error correction, adaptive redundancy, traffic classes, simulated RF/optical/backscatter links, energy and RIS models, supervisory state, cross-layer optimization, signed payloads, telemetry, UDP experiments, and an interactive dashboard.

Aurora-X is **not yet a field-deployed extreme-network stack**, a validated replacement for established FEC standards, or a production security system. The internal LT-like simulator now has a coherent descriptor-driven path for deterministic concurrent generation arrivals and decoding, but its channel, energy, safety-state, HAL, dashboard, and security layers still include simulation or prototype behaviour that must not be used for quantitative field claims.

The project is not being reduced in scope. Its engineering path and maximum target architecture are defined in [`AURORA_X_MASTER_PLAN.md`](AURORA_X_MASTER_PLAN.md).

---

## Mission

Aurora-X aims to become a contract-aware, self-observing transport layer for environments where conventional assumptions fail:

- intermittent and asymmetric connectivity;
- severe packet loss and rapidly changing channels;
- constrained batteries and harvested energy;
- regulatory duty-cycle limits;
- heterogeneous links such as RF, optical, backscatter, and future physical interfaces;
- delay-tolerant, store-and-forward, and multi-node operation;
- high-value payloads requiring integrity, provenance, and differentiated protection;
- links assisted by programmable surfaces or other environmental infrastructure.

The biological terminology is a design metaphor for control behaviour, not a claim of biological equivalence:

- **NERVE** — latency-sensitive traffic;
- **GLAND** — high-reliability state or control traffic;
- **MUSCLE** — elastic or bulk traffic;
- **panic response** — temporary fast increase in protection after failure;
- **memory** — persistent per-flow adaptation state;
- **recovery** — controlled return toward lower overhead after sustained success.

---

## Current status

**Stage:** advanced cross-layer research prototype and simulator  
**Language:** C++20, with optional Python dashboard tooling  
**Primary current task:** consolidate contract, decision, safety and runtime truthfulness before further feature work

| Area | Current state | What is already present | Important limitation |
|---|---|---|---|
| LT-like fountain FEC | Implemented experimental codec | Deterministic systematic/repair symbols, unique source-index sampling, ideal-soliton repair degrees, incremental bounded-rank GF(2) decoding | The deterministic harness includes no-FEC, repetition and internal LT-like comparisons, but not an established external codec |
| Generation lifecycle | Implemented first vertical slice | Parsed `TransportContract`, immutable `GenerationDescriptor`, generation-indexed encoder/decoder state, independent segment expiry, bounded deterministic runtime repair emission, exact length, integrity result and authoritative per-segment `DecodeReport` evidence | Descriptor integrity uses a research checksum, not authenticated metadata; the in-memory generation store is bounded but not persistent |
| Adaptive transport policy | Modular prototype | Injected codec and policy interfaces, fixed policies, NERVE/GLAND/MUSCLE adaptive policy, bounded per-generation manager, failure response and gradual relaxation | Threshold, PID and risk-sensitive alternatives are not implemented; current evidence remains synthetic despite covering multiple channel traces |
| Cross-layer channel optimizer | Implemented prototype | RF/IR/backscatter selection, urgency, reliability target, energy and duty inputs, UCB or SNR selection, and an isolated replayable proposal RNG | Uses synthetic channel and hardware models; the benchmark still isolates coding policy rather than comparing the complete cross-layer optimizer |
| Transport health | Implemented first slice | Consumes only `DecodeReport`; progress polls update coverage without being counted as delivery failures | Aggregation/confidence and multi-flow recovery semantics still need development |
| Safety supervision | Partial | Hard envelope constrains generation/segment expiry, freshness, post-action reserve, allowed and in-contact links, simulated RF airtime, active-segment repair capacity and critical protection; timestamped health evidence expires deterministically, while the supervisory monitor preserves its bounded window and hysteresis transition state for replay | Thresholds and the default 5-second evidence lifetime remain configurable research values rather than calibrated field limits |
| Operating modes | Implemented prototype | CONSERVATIVE, NORMAL, and AGGRESSIVE policies | The default single-flow scenario provides limited transition evidence |
| Energy, channel, RIS and link models | Implemented simulation | Battery state, harvesting, contract-configured simulation-time duty accounting, LBT, fading/PER functions, geometry, RIS phases, RF/IR/backscatter costs | These are research models, not calibrated physical hardware implementations; realtime HAL duty remains a separate path |
| Hardware abstraction | Interface and mocks | Radio, IR, backscatter, RIS, SPI, I²C and GPIO facade; build provenance labels this path `field-experimental` and keeps `hardware_validated=false` | `FIELD_BUILD` currently still uses stubbed device operations and cannot produce field-evidence claims |
| Cryptographic payload integrity | Optional real path | Ed25519 through libsodium when explicitly enabled | Standalone builds without libsodium use a deterministic placeholder and must not be treated as secure |
| UDP tools | Experimental socket tools | Local and Internet-oriented UDP sequence, bidirectional, and echo experiments | Crossing an Internet path validates socket transport and RTT observation, not the complete Aurora adaptive/FEC architecture |
| Telemetry and replay | Implemented prototype | JSONL engine telemetry, V6 decision traces, V5 simulator event ledgers, canonical contact and V2 generation-arrival schedules, and benchmark channel traces; paired replay reconstructs aging/fair priority/deadline scheduling from external arrival through contact, harvesting, RIS, proposal, action and supervision | Concurrent/mobile nodes, weighted-share policies, imported emulation traces and physical hardware remain outside the current event model |
| Interactive dashboard | Visual monitoring prototype | Dash/Plotly process launcher, health plots, KPI cards, and parameter controls | The engine currently does not reload the configuration file written by the sliders |
| Automated tests | Registered with CTest and GitHub Actions | Contract semantics, deterministic repair emission, panic bounds, rolling windows, HAL/duty/contact refusal, critical scheduling, proposal/RNG/UCB replay, concurrent scheduled arrivals, simulator/contact event replay, stale-health expiry, supervisory transition replay, channel-trace integrity and benchmark determinism | Property fuzzing and calibrated hardware tests are still missing |
| Reproducible build | Dependency-light profile working | C++20 CMake build, explicit seeds, paired simulator-event/decision replay, replayable benchmark channel traces, Wilson confidence intervals, per-trial records and configure-time build/profile fingerprints | Provenance and checksum chains detect reproducibility failures and corruption but do not authenticate the producing build |

---

## What has been achieved

### 1. A real fountain-style coding experiment

Aurora-X includes an internal FEC implementation that:

1. splits a payload into source symbols;
2. emits seeded XOR combinations;
3. reconstructs the corresponding binary equations at the receiver;
4. solves the system over GF(2);
5. returns the recovered payload when the generation reaches sufficient rank.

This is a real encoder/decoder experiment. It is intentionally described as **LT-like** rather than as a standards-compliant RaptorQ replacement.

### 2. Parsed transport contracts and explicit generations

`TransportContract::parse()` now rejects unknown or invalid requirements and represents deadline, reliability, duty, allowed links, reserve floor, observation freshness, repair caps, integrity policy, experiment seed, and caller-declared byte ranges. It does not infer the meaning of payload bytes.

Each spawn produces an immutable `GenerationDescriptor` containing codec identity/version, generation and token IDs, exact payload length, symbol size/count, segment requirements, coding seeds/overhead, creation/expiry, and research integrity fingerprints. Decoder state is stored per generation ID in a bounded store, so multiple generations can be active and integrated out of order.

The current contract surface is classified explicitly in `transport_contract_semantic_audit`:

| Classification | Fields |
|---|---|
| Enforced | version, global and per-segment deadlines, duty fraction, allowed links, RIS tile count, minimum source reserve, observation freshness, maximum repair amplification, minimum critical overhead, generation/source limits, integrity requirement, experiment seed, segment ranges and segment importance |
| Policy input | global reliability, per-segment target reliability, selector choice and generation importance |
| Unsupported | payload-semantic interpretation; Aurora transports opaque bytes |

“Policy input” does not mean an achieved SLA. Global reliability selects protection behaviour; per-segment target reliability orders eligible runtime repairs after importance and before deadline. `SegmentDecodeReport::observed_target_met` records exact success in one trial, not an ensemble guarantee.

### 3. Adaptive protection with memory

Per flow class and priority, the organism tracks:

- critical and bulk overhead;
- genetic baseline overhead;
- average coverage;
- success and failure counts;
- good and bad streaks;
- temporary panic state consumed by exactly the next configured number of generated generations;
- a configurable genotype policy.

Failures can increase future redundancy rapidly, while sustained successful operation can reduce it gradually toward the baseline.

### 4. A cross-layer simulation environment

The engine combines coding decisions with models of:

- RF, optical, and backscatter transmission;
- energy expenditure and harvesting;
- duty-cycle availability;
- listen-before-talk behaviour;
- synthetic SNR, fading, and packet error probability;
- obstacles and programmable-surface paths;
- deadline pressure and remaining decode work.

### 5. Supervisory and optimization layers

Aurora-X includes experimental layers for:

- aggregated flow health;
- safety state classification;
- conservative, normal, and aggressive operation;
- SNR-based or UCB-based physical-link selection;
- telemetry-driven feedback.

The main simulator now applies the optimizer through a hard `SafetyEnvelope`, transmits the organism's spawned packets, and feeds the same authoritative `DecodeReport` to delivery and transport health. The supervisory monitor reports `NO_EVIDENCE` until active-flow observations exist, excludes inactive classes, applies Schmitt-style enter/exit thresholds, and requires consecutive evidence before changing an established state. `NO_EVIDENCE` forces conservative operation, and recovery from `CRITICAL` must pass through `DEGRADED`. Proposal, action and supervisory transition state are replayed per action; the configured thresholds remain uncalibrated research values and the surrounding simulator event stream remains outside that proof.

### 6. Security and provenance experiments

The repository includes:

- token serialization;
- signatures;
- optional real Ed25519 through libsodium;
- Merkle-style proof-of-delivery experiments.

Secure claims apply only to builds explicitly using the real cryptographic backend.

### 7. Real socket experiments

The UDP tools demonstrate that Aurora-related payloads and sequence data can be serialized and moved through actual operating-system sockets across local or Internet paths. They are useful transport experiments, but they do not by themselves validate Aurora's simulated RF, RIS, energy, or adaptive-FEC models.

---

## Current internal generation path

The dependency-light simulator now follows one path:

```text
TransportContract
   ↓
TransportPolicy + GenerationCodec
   ↓
GenerationManager::spawn() → GenerationDescriptor
   ↓
SafetyEnvelope → deterministic repair emission → exact recorded execution
   ↓
HAL acceptance → simulated channel → receiver
   ↓
GenerationManager::integrate()
   ↓
authoritative DecodeReport
   ↓
FlowHealth → replayable proposal controller → SafetyEnvelope
   ↓
DecisionReplayLog V6 + action and supervisory feedback
```

The same report determines delivery, coverage/rank, critical completion, integrity outcome, and health feedback. A decision trace is saved only after runtime execution counts have been attached and checked against the constrained decision. The former monolithic delivery decoder and parallel organism health decoder were removed from the active simulator path.

The current boundary is intentionally narrow:

- symbol count;
- original payload length;
- critical and bulk boundaries;
- symbol size;
- encoder identity and version;
- generation identifier;
- adaptation parameters used at spawn time;
- bounded in-memory state for concurrent generations.

`AlienFountainOrganism` remains as a compatibility facade that composes the biological policy, the experimental LT-like codec and the generation manager. Fixed policies and tagged codecs can be injected independently in tests and benchmark runs. The optional external RaptorQ adapter is not part of this lifecycle and remains a disabled legacy experiment until it can implement the same descriptor/report contract.

---

## Target architecture

Aurora-X is intended to evolve toward the following layered system:

```text
Application intent and mission constraints
                  ↓
        Intent compiler / policy contract
                  ↓
     Adaptive coding and generation manager
                  ↓
  Custody, fragmentation, provenance and security
                  ↓
 Cross-layer optimizer and safety supervisor
                  ↓
Link abstraction: RF | optical | backscatter | future
                  ↓
 Network, hardware-in-the-loop, or physical hardware
                  ↓
      Telemetry, digital twin and verification
```

The complete target includes:

- concurrent, independently decodable generations;
- class-aware and deadline-aware coding;
- online channel estimation;
- constrained adaptation rather than unconstrained heuristic tuning;
- multi-hop custody and delay-tolerant forwarding;
- authenticated generation metadata;
- hardware-backed keys where available;
- real link adapters;
- digital-twin replay;
- reproducible benchmark scenarios;
- formal safety invariants for adaptation boundaries;
- comparison against established baselines.

See [`AURORA_X_MASTER_PLAN.md`](AURORA_X_MASTER_PLAN.md) for the complete path.

---

## Repository structure

```text
aurora_x.cpp                 Main simulation/orchestration harness
aurora_extreme.hpp           Channel, energy, optimizer and shared models
aurora_organism.hpp          Compatibility facade for the biological policy
aurora_intention.hpp         Transitional include for TransportContract
aurora_hal.hpp               Hardware abstraction and current mock backends
include/aurora/
  control/                   Proposal replay and transport policy interfaces
  fec/                       Codec interface and deterministic LT-like codec
  transport/                 Contract, descriptor, manager, report and health
  safety/SafetyEnvelope.hpp  Hard transport-decision constraints and trace
  telemetry/DecisionReplayLog.hpp  Canonical decision log and verifier
  telemetry/SimulationEventLedger.hpp  Inter-step simulator event ledger
  simulation/ContactSchedule.hpp       Canonical link-contact windows
  simulation/GenerationArrivalSchedule.hpp Canonical external arrivals
  simulation/GenerationScheduler.hpp       Priority-then-EDF selector
  simulation/ChannelTrace.hpp      Canonical channel traces and generators
  simulation/BaselineBenchmark.hpp Replayable statistical comparison harness
apps/
  aurora_replay.cpp          Independent proposal/action/safety replay tool
  aurora_contact_schedule.cpp Canonical contact-schedule creator
  aurora_generation_arrivals.cpp Canonical generation-arrival creator
  aurora_benchmark.cpp       CSV baseline runner
src/core/
  AuroraSafetyMonitor.hpp    Legacy safety-state prototype
tests/
  test_aurora_organism.cpp   Generation/FEC lifecycle invariants
  test_transport_contract.cpp
  test_safety_envelope.cpp
  test_decision_replay.cpp
  test_modular_transport.cpp
  test_baseline_benchmark.cpp
  test_channel_trace.cpp
  test_simulation_event_ledger.cpp
  test_contact_schedule.cpp
  test_generation_arrival_schedule.cpp
  test_generation_scheduler.cpp
aurora_batch_test.cpp        Experimental parameter sweep harness
aurora_udp_*.cpp             UDP transport experiments
aurora_dash_lab.py           Live monitoring dashboard
Orginal/                     Historical source snapshot; not the active implementation
```

---

## Build

### Core research build

The dependency-light build uses the internal LT-like FEC and placeholder signatures:

```bash
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Debug \
  -DUSE_SODIUM=OFF \
  -DUSE_RAPTORQ=OFF \
  -DBUILD_NET_TOOLS=ON \
  -DBUILD_TESTING=ON

cmake --build build --config Debug
ctest --test-dir build --output-on-failure
```

Run:

```bash
./build/bin/aurora_x
```

On multi-configuration Windows generators the executable may be under `build/bin/Debug/`.

Record and independently replay safety decisions:

```bash
./build/bin/aurora_x --decision-trace decision-trace.log
./build/bin/aurora_replay decision-trace.log
```

Record and replay the complete event stream implemented by the current simulator:

```bash
./build/bin/aurora_x \
  --decision-trace decision-trace.log \
  --event-ledger simulation-events.log
./build/bin/aurora_replay decision-trace.log simulation-events.log
```

Create and apply deterministic contact windows before recording:

```bash
./build/bin/aurora_contact_schedule contacts.trace \
  0:1000:rf \
  1000:18446744073709551615:all

./build/bin/aurora_x \
  --contact-schedule contacts.trace \
  --decision-trace decision-trace.log \
  --event-ledger simulation-events.log
```

Windows are half-open `[start_ms, end_ms)`. Gaps mean complete loss of contact; link sets are `none`, `all`, or `+`-joined subsets of `rf`, `optical`, and `backscatter`.
Use `--contact-schedule-out <path>` to persist the exact canonical schedule selected for a run.

Create a deterministic external-generation arrival schedule and replay it with the same run:

```bash
./build/bin/aurora_generation_arrivals arrivals.trace \
  0:alpha:elastic:10000 \
  1000:beta:critical:5000 \
  5000:gamma:inherit:inherit

./build/bin/aurora_x \
  --generation-arrivals arrivals.trace \
  --generation-arrivals-out arrivals-used.trace \
  --contact-schedule contacts.trace \
  --decision-trace decision-trace.log \
  --event-ledger simulation-events.log
```

The compact entry form is `<time-ms>:<tag>` and inherits importance/deadline from the base contract. The extended form is `<time-ms>:<tag>:<class>:<deadline-ms|inherit>`, where class is `critical`, `important`, `elastic` or `inherit`. The first arrival must be at 0 ms; later times are unique, strictly increasing and aligned to the simulator's 1000 ms step. Tags are unique stable identifiers containing only letters, digits, `.`, `_` or `-`.

The engine deterministically pre-constructs every immutable descriptor and releases its initial symbols only at the declared time. Ordinarily it selects by effective class (`critical`, then `important`, then `elastic`), earliest absolute descriptor expiry, arrival time and stable schedule index. A generation waiting without service is promoted by one class every 2000 ms. After 3000 ms it enters the anti-starvation lane, which is ordered by earliest fairness deadline before EDF and the stable tie-breaks. Every selected 1000 ms quantum resets that generation's fairness clock. A later critical or tighter-deadline generation can therefore preempt an earlier generation, while an elastic generation still receives a bounded turn and remains independently decodable across preemption and resume.

For `N` continuously eligible non-terminal generations, the deterministic maximum gap between service turns is `3000 + (N - 1) * 1000` ms; at the schedule capacity of 128 arrivals this is 130000 ms. The simulator prints the active policy and bound at startup, and the V5 event ledger embeds all three policy intervals plus the independently reconstructed last-service state. Use `--generation-arrivals-out <path>` to persist the exact arrival schedule used.

The paired verifier first reconstructs the declared generation releases, last-service clocks, aging/fair priority/deadline selection, scheduled link availability, each harvesting/ingest transition, deterministic RIS perturbation, illumination, world gain and SNR summary from the session topology and global RNG checkpoints. It binds every step to the active generation identity and resolved per-generation contract, then derives the exact V6 proposal, recomputes the `SafetyEnvelope` decision—including link replacement or rejection outside a contact window—and validates LBT samples, fading, delivery, energy/duty, pacing randomness and generation-qualified destination inbox arrivals. Finally it applies UCB and supervisory feedback while enforcing schedule, contact, RNG, energy, buffer, inbox and per-generation decoder-rank continuity across steps.

This covers scheduled link contacts, bounded starvation prevention and multiple preemptible external arrivals in the current fixed-node simulator. Concurrent/mobile nodes, configurable or weighted-share fairness and imported field traces are not yet implemented. Arrival replay is unavailable in interactive-lab and `FIELD_BUILD` modes. All samples remain simulation evidence rather than calibrated measurements, and real field hardware is not reconstructed.

V2 through V5 decision traces do not contain the complete V6 contact, proposal, action and supervisory evidence and are intentionally rejected. V1 generation-arrival schedules lack service/deadline fields, and V1–V4 simulator event ledgers lack the complete aging/fairness evidence; these artifacts are rejected and must be regenerated.

Run the deterministic IID-loss comparison (`loss`, `trials`, `seed`):

```bash
./build/bin/aurora_benchmark 0.25 200 0xA607A
```

Generate and replay a burst-loss campaign while retaining its raw trials:

```bash
./build/bin/aurora_benchmark \
  --scenario burst --trials 200 --seed 0xA607A \
  --trace-out burst.trace --runs burst-runs.csv

./build/bin/aurora_benchmark --trace-in burst.trace
```

Available synthetic scenarios are `iid`, `burst` (Gilbert–Elliott), `outage`, `drift`, and `shock`. Every policy consumes the same delivered/lost result for a given transmission slot. The trace contains canonical scenario parameters, seed, exact outcomes, a checksum chain and an end marker. Summary CSV includes Wilson 95% intervals; the optional run CSV retains successes and failures per baseline/trial. The biological policy updates protection between generations/trials, so a shock trace is not evidence of intra-generation adaptation.

These outputs are simulation evidence. Repeated generation or replay of the same trace is byte-for-byte deterministic.

### Real Ed25519 build

Install libsodium and configure:

```bash
cmake -S . -B build-secure \
  -DCMAKE_BUILD_TYPE=Release \
  -DUSE_SODIUM=ON \
  -DUSE_RAPTORQ=OFF \
  -DBUILD_NET_TOOLS=ON \
  -DBUILD_TESTING=ON

cmake --build build-secure --config Release
```

Only this path may be described as using real Ed25519.

### Optional RaptorQ path

The RaptorQ adapter is legacy/experimental and requires an explicitly supplied compatible implementation. It is not required for the current internal-organism research path.

```bash
cmake -S . -B build-rq \
  -DUSE_RAPTORQ=ON \
  -DRAPTORQ_ROOT=/path/to/libRaptorQ \
  -DUSE_SODIUM=ON
```

---

## Interactive dashboard

The dashboard can launch the engine in interactive-lab mode and visualize streamed health events:

```bash
python -m pip install dash plotly
python aurora_dash_lab.py
```

Current status:

- monitoring works as an experimental process bridge;
- slider values are written to `aurora_interactive_config.json`;
- the C++ reload path is currently disabled, so controls do not yet change the live engine.

---

## Testing and validation policy

Aurora-X will distinguish four levels of evidence:

1. **Unit evidence** — exact behaviour of codecs, parsers, controllers, and state machines.
2. **Simulation evidence** — results under declared synthetic channel and energy models.
3. **Emulation evidence** — real processes and sockets under controlled network impairment.
4. **Field evidence** — physical hardware, measured channels, and calibrated energy use.

Results from one level must not be presented as proof of another.

The current baseline harness compares, under identical seeds and constraints:

- no FEC;
- fixed repetition;
- fixed LT-like overhead;
- class-aware fixed overhead;
- adaptive Aurora policy;

The harness now supports IID loss, Gilbert–Elliott bursts, scheduled outages, slow drift and shock/recovery traces, per-trial retention, Wilson confidence intervals, goodput, transmitted-byte cost, innovative-symbol ratio and overhead-direction changes. Every summary and retained trial also declares its configure-time commit, clean/dirty source state, compiler, target, build type/generator, execution profile, crypto/FEC profile, and a canonical build fingerprint. Benchmark evidence is always labelled `simulation`; even a `BUILD_FIELD=ON` executable remains `field-experimental` with `hardware_validated=false`. Innovative-symbol ratio is reported only for coded policies; overhead-direction changes only for the adaptive policy; and cost per delivered byte is `N/A` when no payload is delivered.

An established block or fountain codec, asymmetric/contact/deadline scenarios, authenticated provenance and calibrated resource/energy costs remain required before comparative claims can be made.

Primary metrics:

- delivery probability;
- useful goodput;
- transmitted bytes per delivered byte;
- decode latency distribution;
- energy per delivered byte;
- deadline success rate;
- adaptation settling time;
- overshoot and oscillation;
- false panic and delayed recovery rates.

---

## Visual concept

<img width="2816" height="1536" alt="Aurora-X conceptual visualization" src="https://github.com/user-attachments/assets/f7726fd8-7921-44e1-9533-207cf81e6458" />

*Conceptual visualization of the Aurora-X research direction. It represents the intended autonomous and resilient network substrate; it is not a diagram of currently deployed hardware.*

---

## License

Aurora-X is source-available under the repository's custom evaluation, research, and demonstration license. Commercial, industrial, production, redistribution, and modification rights are restricted unless separately authorized.

See [`LICENSE`](LICENSE).

---

Aurora-X should be judged neither as a finished product nor as a small disposable prototype. Its generation loop and first synthetic channel campaigns are coherent, modular and replayable. The next obligation is to add established-code and deadline/contact comparisons, then calibrate resource evidence, before extending into more links, custody or hardware claims.
