# Aurora-X

> Bio-inspired adaptive transport and cross-layer control research engine for resilient heterogeneous networks.

Aurora-X is an ambitious systems research project exploring a long-term goal:

> Can a network transport substrate observe its own condition, protect different classes of information according to intent, adapt coding and physical-link decisions under stress, preserve energy and duty-cycle constraints, and recover safely after severe disruption?

The current repository contains a substantial C++20 research implementation covering fountain-style forward error correction, adaptive redundancy, traffic classes, simulated RF/optical/backscatter links, energy and RIS models, supervisory state, cross-layer optimization, signed payloads, telemetry, UDP experiments, and an interactive dashboard.

Aurora-X is **not yet a field-deployed extreme-network stack**, a validated replacement for established FEC standards, or a production security system. The internal LT-like simulator now has one coherent generation/decode path, but its channel, energy, safety-state, HAL, dashboard, and security layers still include simulation or prototype behaviour that must not be used for quantitative field claims.

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
**Primary current task:** turn the coherent internal generation lifecycle into a controlled comparison and replay platform

| Area | Current state | What is already present | Important limitation |
|---|---|---|---|
| LT-like fountain FEC | Implemented experimental codec | Deterministic systematic/repair symbols, unique source-index sampling, ideal-soliton repair degrees, incremental bounded-rank GF(2) decoding | Not yet benchmarked against robust LT, RaptorQ, Reed–Solomon, repetition, or no-FEC baselines |
| Generation lifecycle | Implemented first vertical slice | Parsed `TransportContract`, immutable `GenerationDescriptor`, generation-indexed decoder state, expiry, exact length, integrity result and authoritative `DecodeReport` | Descriptor integrity uses a research checksum, not authenticated metadata; the in-memory generation store is bounded but not persistent |
| Adaptive FEC organism | Substantial prototype | NERVE/GLAND/MUSCLE policy profiles, class-aware overhead, failure response, panic boost and gradual relaxation | Biological policy remains coupled to the first generation manager implementation and needs an interchangeable controller interface |
| Cross-layer channel optimizer | Implemented prototype | RF/IR/backscatter selection, urgency, reliability target, energy and duty inputs, UCB or SNR selection | Uses synthetic channel and hardware models; controller benefits are not yet isolated by controlled benchmarks |
| Transport health | Implemented first slice | Consumes only `DecodeReport`; progress polls update coverage without being counted as delivery failures | Aggregation/confidence and multi-flow recovery semantics still need development |
| Safety supervision | Partial | Hard envelope enforces expiry, observation freshness, reserve floor, allowed links, RF duty availability and repair-amplification caps; legacy HEALTHY/DEGRADED/CRITICAL monitor remains | The legacy state monitor still needs replacement by a hysteretic, evidence-aware supervisor |
| Operating modes | Implemented prototype | CONSERVATIVE, NORMAL, and AGGRESSIVE policies | Some transitions are unreachable in the default single-flow scenario because inactive classes retain empty health state |
| Energy, channel, RIS and link models | Implemented simulation | Battery state, harvesting, duty limiter, LBT, fading/PER functions, geometry, RIS phases, RF/IR/backscatter costs | These are research models, not calibrated physical hardware implementations |
| Hardware abstraction | Interface and mocks | Radio, IR, backscatter, RIS, SPI, I²C and GPIO facade | `FIELD_BUILD` currently still uses stubbed device operations |
| Cryptographic payload integrity | Optional real path | Ed25519 through libsodium when explicitly enabled | Standalone builds without libsodium use a deterministic placeholder and must not be treated as secure |
| UDP tools | Experimental socket tools | Local and Internet-oriented UDP sequence, bidirectional, and echo experiments | Crossing an Internet path validates socket transport and RTT observation, not the complete Aurora adaptive/FEC architecture |
| Telemetry | Implemented prototype | JSONL engine telemetry and JSON health events | Schema and provenance need consolidation; some advertised CSV fields are placeholders |
| Interactive dashboard | Visual monitoring prototype | Dash/Plotly process launcher, health plots, KPI cards, and parameter controls | The engine currently does not reload the configuration file written by the sliders |
| Automated tests | Registered with CTest | Contract parsing, deterministic codec behaviour, concurrent/interleaved generations, expiry, malformed input, integrity, exact length, bounded state, health and safety invariants | Property fuzzing, CI, cross-platform runs and benchmark statistics are still missing |
| Reproducible build | Dependency-light profile working | C++20 CMake build, explicit seed for coding and simulator RNG, optional insecure/secure dependency profiles | Linux CI and full event-trace replay are not implemented |

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

### 3. Adaptive protection with memory

Per flow class and priority, the organism tracks:

- critical and bulk overhead;
- genetic baseline overhead;
- average coverage;
- success and failure counts;
- good and bad streaks;
- temporary panic state;
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

The main simulator now applies the optimizer through a hard `SafetyEnvelope`, transmits the organism's spawned packets, and feeds the same authoritative `DecodeReport` to delivery and transport health. The older three-state safety monitor remains as a supervisory prototype and is not presented as a complete safety system.

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
GenerationDescriptor
   ↓
AlienFountainOrganism::spawn()
   ↓
Channel / nodes / custody
   ↓
AlienFountainOrganism::integrate()
   ↓
DecodeReport
   ↓
FlowHealth → SafetySupervisor → Optimizer
   ↓
policy for the next generation
```

The same report determines delivery, coverage/rank, critical completion, integrity outcome, and health feedback. The former monolithic delivery decoder and parallel organism health decoder were removed from the active simulator path.

The current boundary is intentionally narrow:

- symbol count;
- original payload length;
- critical and bulk boundaries;
- symbol size;
- encoder identity and version;
- generation identifier;
- adaptation parameters used at spawn time;
- bounded in-memory state for concurrent generations.

The optional external RaptorQ adapter is not part of this lifecycle and remains a disabled legacy experiment until it can implement the same descriptor/report contract.

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
aurora_organism.hpp          Generation manager and biological adaptive policy
aurora_intention.hpp         Transitional include for TransportContract
aurora_hal.hpp               Hardware abstraction and current mock backends
include/aurora/
  fec/LtLikeCodec.hpp        Deterministic experimental LT-like codec
  transport/                 Contract, descriptor, report and health types
  safety/SafetyEnvelope.hpp  Hard transport-decision constraints and trace
src/core/
  AuroraSafetyMonitor.hpp    Legacy safety-state prototype
tests/
  test_aurora_organism.cpp   Generation/FEC lifecycle invariants
  test_transport_contract.cpp
  test_safety_envelope.cpp
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

The first decisive benchmark will compare, under identical seeds and constraints:

- no FEC;
- fixed repetition;
- fixed LT-like overhead;
- adaptive Aurora policy;
- optional established-code baseline when available.

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

Aurora-X should be judged neither as a finished product nor as a small disposable prototype. It is a high-ambition research system with several implemented foundations and one immediate obligation: make the adaptive coding loop internally coherent, measurable, and reproducible before extending it further.
