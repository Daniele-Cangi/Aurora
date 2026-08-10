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
**Primary current task:** add an established external FEC baseline to the complete deterministic transport comparison

| Area | Current state | What is already present | Important limitation |
|---|---|---|---|
| LT-like fountain FEC | Implemented experimental codec | Deterministic systematic/repair symbols, unique source-index sampling, ideal-soliton repair degrees, incremental bounded-rank GF(2) decoding | The deterministic harness includes no-FEC, repetition and internal LT-like comparisons, but not an established external codec |
| Generation lifecycle | Implemented first vertical slice | Parsed `TransportContract`, immutable `GenerationDescriptor`, generation-indexed encoder/decoder state, independent segment expiry, bounded deterministic runtime repair emission, exact length, integrity result and authoritative per-segment `DecodeReport` evidence | Descriptor integrity uses a research checksum, not authenticated metadata; the in-memory generation store is bounded but not persistent |
| Adaptive transport policy | Modular prototype | Injected codec and policy interfaces, fixed policies, NERVE/GLAND/MUSCLE adaptive policy, bounded per-generation manager, failure response and gradual relaxation | Threshold, PID and risk-sensitive alternatives are not implemented; current evidence remains synthetic despite covering multiple channel traces |
| Cross-layer channel optimizer | Implemented prototype | RF/IR/backscatter selection, urgency, reliability target, energy and duty inputs, UCB or SNR selection, an isolated replayable proposal RNG, and a complete contact/deadline-aware benchmark path | Uses synthetic channel and hardware models; comparative results remain simulation evidence rather than calibrated performance claims |
| Transport health | Implemented first slice | Consumes only `DecodeReport`; progress polls update coverage without being counted as delivery failures | Aggregation/confidence and multi-flow recovery semantics still need development |
| Safety supervision | Partial | Hard envelope constrains generation/segment expiry, freshness, post-action reserve, allowed and in-contact links, simulated RF airtime, active-segment repair capacity and critical protection; timestamped health evidence expires deterministically, while the supervisory monitor preserves its bounded window and hysteresis transition state for replay | Thresholds and the default 5-second evidence lifetime remain configurable research values rather than calibrated field limits |
| Operating modes | Implemented prototype | CONSERVATIVE, NORMAL, and AGGRESSIVE policies | The default single-flow scenario provides limited transition evidence |
| Energy, channel, RIS and link models | Implemented simulation | Battery state, harvesting, contract-configured simulation-time duty accounting, LBT, fading/PER functions, geometry, RIS phases, RF/IR/backscatter costs | These are research models, not calibrated physical hardware implementations; realtime HAL duty remains a separate path |
| Hardware abstraction | Interface and mocks | Radio, IR, backscatter, RIS, SPI, I²C and GPIO facade; build provenance labels this path `field-experimental` and keeps `hardware_validated=false` | `FIELD_BUILD` currently still uses stubbed device operations and cannot produce field-evidence claims |
| Cryptographic payload integrity | Optional real path | Ed25519 through libsodium when explicitly enabled | Standalone builds without libsodium use a deterministic placeholder and must not be treated as secure |
| UDP tools | Experimental socket tools | Local and Internet-oriented UDP sequence, bidirectional, and echo experiments | Crossing an Internet path validates socket transport and RTT observation, not the complete Aurora adaptive/FEC architecture |
| Telemetry and replay | Implemented prototype | Deterministic `(time, phase, sequence)` event kernel, JSONL telemetry, V6 decision traces, V7 simulator event ledgers, canonical contact and V2 generation-arrival schedules, benchmark channel traces, and a strict-vs-fair scheduler harness; paired replay reconstructs causal planning, turns and effective service | The current transport model still schedules a periodic 1000 ms quantum and requires arrivals on that boundary; the fairness bound applies to turns, not effective service; concurrent/mobile nodes and imported emulation traces remain outside the model |
| Interactive dashboard | Visual monitoring prototype | Dash/Plotly process launcher, health plots, KPI cards, and parameter controls | The engine currently does not reload the configuration file written by the sliders |
| Automated tests | Registered with CTest and GitHub Actions | Contract semantics, deterministic repair emission, panic bounds, rolling windows, HAL/duty/contact refusal, critical scheduling, proposal/RNG/UCB replay, concurrent scheduled arrivals, simulator/contact event replay, stale-health expiry, supervisory transition replay, channel-trace integrity, isolated baselines and end-to-end benchmark determinism | Property fuzzing and calibrated hardware tests are still missing |
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

The main simulator now runs from a deterministic priority queue ordered by timestamp, event phase and insertion sequence. Generation arrivals precede transport work at the same timestamp; each transport quantum schedules its successor explicitly. Simulation research runs no longer wait on wall-clock pacing, although they consume the same pacing RNG so canonical traces remain unchanged; interactive and field-experimental paths retain realtime waits. The optimizer then passes through the hard `SafetyEnvelope`, transmits the organism's spawned packets, and feeds the same authoritative `DecodeReport` to delivery and health. V6 decision traces replay proposal, action and supervisory transitions, while the paired V7 simulator ledger reconstructs the fixed-node event stream. Future generation IDs are reserved without calling the policy; arrival-time planning consumes all earlier feedback. Each quantum identifies the granted scheduler turn and separately records HAL-accepted effective service. The committed regression gate fixes the canonical V7/V6 SHA-256 outputs, so the kernel migration preserves the prior observable trace byte for byte. This proves the implemented simulation path and causal ordering, not calibrated thresholds or hardware behaviour.

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
reserve deterministic generation identity (no policy planning)
   ↓
declared arrival → TransportPolicy + GenerationCodec
   ↓
GenerationManager::spawn_reserved() → GenerationDescriptor
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
  simulation/DeterministicEventKernel.hpp Deterministic event queue/order
  simulation/ContactSchedule.hpp       Canonical link-contact windows
  simulation/GenerationArrivalSchedule.hpp Canonical external arrivals
  simulation/GenerationScheduler.hpp       Strict or aging/fair EDF selector
  simulation/GenerationSchedulerBenchmark.hpp Adversarial scheduler comparison
  simulation/ChannelTrace.hpp      Canonical channel traces and generators
  simulation/BaselineBenchmark.hpp Replayable statistical comparison harness
apps/
  aurora_replay.cpp          Independent proposal/action/safety replay tool
  aurora_contact_schedule.cpp Canonical contact-schedule creator
  aurora_generation_arrivals.cpp Canonical generation-arrival creator
  aurora_scheduler_benchmark.cpp Strict-vs-fair starvation benchmark
  aurora_transport_benchmark.cpp Complete deterministic transport comparison
  aurora_benchmark.cpp       CSV baseline runner
benchmarks/
  generation_scheduler_sweep_v2.csv Canonical scheduling-turn regression report
  end_to_end_transport_sweep_v1.csv Canonical complete-stack regression report
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
  test_deterministic_event_kernel.cpp
  test_simulation_event_ledger.cpp
  test_contact_schedule.cpp
  test_generation_arrival_schedule.cpp
  test_generation_scheduler.cpp
  test_generation_scheduler_benchmark.cpp
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
  --generation-scheduler fair \
  --generation-aging-ms 2000 \
  --generation-starvation-ms 3000 \
  --contact-schedule contacts.trace \
  --decision-trace decision-trace.log \
  --event-ledger simulation-events.log
```

The compact entry form is `<time-ms>:<tag>` and inherits importance/deadline from the base contract. The extended form is `<time-ms>:<tag>:<class>:<deadline-ms|inherit>`, where class is `critical`, `important`, `elastic` or `inherit`. The first arrival must be at 0 ms; later times are unique, strictly increasing and aligned to the currently modeled 1000 ms transport quantum. This is a model constraint, not a limitation of the event queue. Tags are unique stable identifiers containing only letters, digits, `.`, `_` or `-`.

The kernel orders events by `(time_ms, phase, sequence)`. Arrival phase is earlier than transport-quantum phase, so a generation arriving at a quantum boundary is planned and made eligible before scheduler selection at that same timestamp; sequence preserves deterministic FIFO order within a phase. The engine reserves each identity without invoking policy planning, then calls the current policy, constructs the immutable descriptor and spawns initial symbols at the arrival event. Feedback from A can therefore change B while B retains its precomputable identity. `--generation-scheduler fair` selects by effective class, descriptor expiry, arrival and stable index. A generation waiting without a scheduled turn is promoted every `--generation-aging-ms`; after `--generation-starvation-ms` it enters the fairness lane. Every selected quantum resets only its scheduling-turn clock.

For `N` continuously eligible non-terminal generations, the deterministic maximum gap between scheduled turns is `ceil(starvation_ms / 1000) * 1000 + (N - 1) * 1000` ms; rounding accounts for the fixed scheduler tick. With defaults and the schedule capacity of 128 arrivals this is 130000 ms. `--generation-scheduler strict` disables aging and the anti-starvation lane, intentionally leaving the scheduling-turn gap unbounded for baseline comparison. The simulator prints this bound under `maximum_scheduling_turn_gap_ms`, and the V7 event ledger embeds the discipline and reconstructs the last scheduled turn independently.

Effective transport service is narrower: it occurs only when at least one attempted packet is accepted by the HAL. A packet accepted by the HAL but lost on the channel still consumed transport capacity and therefore counts; a turn with no contact, a safety/duty/HAL refusal, or zero attempts does not. The scheduler does not claim a finite effective-service bound because contacts, energy, duty budget and HAL availability can all remain unavailable indefinitely. Such a bound would require explicit serviceability assumptions. The V7 event ledger records HAL-accepted attempts separately, and paired verification binds them to the V6 decision execution trace. Use `--generation-arrivals-out <path>` to persist the exact arrival schedule used.

The paired verifier first reconstructs the declared generation releases, last-scheduled-turn clocks, aging/fair priority/deadline selection, scheduled link availability, each harvesting/ingest transition, deterministic RIS perturbation, illumination, world gain and SNR summary from the session topology and global RNG checkpoints. It binds every step to the scheduled generation identity and resolved per-generation contract, then derives the exact V6 proposal, recomputes the `SafetyEnvelope` decision—including link replacement or rejection outside a contact window—and validates that V7 effective-service attempts equal the HAL-accepted attempts, independently of channel delivery. Finally it validates LBT, fading, energy/duty, pacing randomness, inbox arrivals, UCB and supervisory feedback while enforcing per-generation decoder-rank continuity.

This covers scheduled link contacts, configurable bounded starvation prevention and multiple preemptible external arrivals in the current fixed-node simulator. Concurrent/mobile nodes, weighted-share fairness and imported field traces are not yet implemented. Arrival replay and scheduler options are unavailable in `FIELD_BUILD`; arrival replay remains unavailable in interactive-lab mode. All samples remain simulation evidence rather than calibrated measurements, and real field hardware is not reconstructed.

V2 through V5 decision traces do not contain the complete V6 contact, proposal, action and supervisory evidence and are intentionally rejected. V1 generation-arrival schedules lack service/deadline fields, and V1–V6 simulator event ledgers lack explicit effective-service accounting; these artifacts are rejected and must be regenerated.

Run the deterministic adversarial scheduler comparison (`steps`, `critical contenders`, `aging ms`, `starvation ms`):

```bash
./build/bin/aurora_scheduler_benchmark 20 2 2000 3000
```

The default scenario keeps one elastic and two critical generations continuously eligible. Both disciplines receive the same candidates and expiries. CSV reports the comparison bound, observed maximum gap, bound violations and elastic selections; the command succeeds only when strict priority violates the comparison bound while fair scheduling respects it. This is scheduler-level simulation evidence, not a latency or throughput claim for the complete transport stack.

Run the canonical seven-scenario parameter sweep:

```bash
./build/bin/aurora_scheduler_benchmark --sweep
```

Verify the generated bytes and semantic thresholds against the versioned report:

```bash
./build/bin/aurora_scheduler_benchmark \
  --verify-sweep benchmarks/generation_scheduler_sweep_v2.csv
```

The V2 sweep varies contention, aging and starvation intervals, including a starvation interval that is not aligned to the fixed 1000 ms scheduler tick. Every strict row must starve the elastic candidate and exceed the fair scheduling-turn comparison bound. Every fair row must schedule every candidate, remain within the turn-gap bound and report zero violations. It deliberately makes no effective-service claim. CTest performs both the semantic checks and byte-for-byte snapshot verification, so the Linux and Windows GitHub Actions jobs reject unexplained metric drift.

Run the complete transport-stack sweep:

```bash
./build/bin/aurora_transport_benchmark --sweep
```

Verify its semantic gates and byte-for-byte canonical report:

```bash
./build/bin/aurora_transport_benchmark \
  --verify-sweep benchmarks/end_to_end_transport_sweep_v1.csv
```

This runner uses the main `Engine`, not the isolated codec harness. Every row traverses scheduled arrivals, causal arrival-time planning, the aging/fair scheduler, contact windows, the cross-layer proposal, `SafetyEnvelope`, HAL acceptance, channel delivery and authoritative generation decode. It compares the existing fixed-minimum, fixed-class-aware and biological-adaptive policies across four declared seeds in a causal feedback-wave scenario and a deadline-contention scenario. Reported metrics include initial protection, scheduler turns, HAL-accepted service, attempts and delivery, wire bytes, useful bytes, energy, terminal latency and deadline outcomes. Every trial must pass paired V7/V6 ledger replay before it contributes to the report.

Controllers receive identical exogenous inputs: intention, topology/model, contact schedule, arrival schedule, scheduler configuration and seed set. This is deliberately labelled `simulation-common-inputs-action-dependent-rng`, not “identical trace”: once controllers choose different actions, they can consume the deterministic RNG differently. The world fingerprint proves common declared inputs, while each row's compiler-portable structural trace fingerprint covers RNG checkpoints, scheduling, discrete descriptor protection, attempts and outcomes for its actual causal trajectory. The snapshot is a regression oracle, not a claim that one policy is universally superior.

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

An established block or fountain codec, broader asymmetric scenarios, authenticated provenance and calibrated resource/energy costs remain required before comparative claims can be made. The complete-stack sweep now measures contact/deadline-aware execution under declared simulation inputs; it does not turn those inputs into field evidence.

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

Aurora-X should be judged neither as a finished product nor as a small disposable prototype. Its generation loop is causal, scheduling opportunity is distinct from HAL-accepted effective service, and the main research run is driven by a deterministic discrete-event kernel whose canonical V7/V6 output is byte-locked to the pre-migration oracle. The complete contact/deadline-aware sweep now exercises that stack end to end under common declared inputs. The next obligation is an established external FEC baseline, followed by process-separated emulation with an explicit reverse-feedback channel and finally calibrated hardware evidence.
