# Aurora

> Bio-inspired adaptive transport and cross-layer control research engine for resilient heterogeneous networks.

<p align="center">
  <img src="docs/images/aurora-architecture-overview.png" alt="Aurora architecture overview showing adaptive FEC, cross-layer control, and heterogeneous links." width="100%">
</p>

[![Transport CI](https://github.com/Daniele-Cangi/Aurora/actions/workflows/ci.yml/badge.svg)](https://github.com/Daniele-Cangi/Aurora/actions/workflows/ci.yml)
[![License: Apache 2.0](https://img.shields.io/badge/License-Apache_2.0-blue.svg)](LICENSE)

Aurora is a C++20 systems-research project asking a focused question:

> Can a transport system observe failure, protect information according to
> importance and deadlines, adapt its redundancy, and recover without violating
> explicit safety and resource constraints?

The repository contains working generation-based FEC transport, fixed and
adaptive policies, deterministic simulation, authenticated process-separated
UDP emulation, cross-layer models, safety supervision, replayable telemetry and
reproducible experiment tooling.

Aurora is **not** a production network stack or calibrated field system. Current
claims stop at controlled simulation and emulation evidence.

[Technical reference](AURORA_TECHNICAL_REFERENCE.md) ·
[Pilot result](docs/raw-host-policy-pilot-v1-final-results.md) ·
[Evidence explorer](docs/raw-host-policy-pilot-v1-explorer.html) ·
[Post-shock study](docs/raw-host-post-shock-efficiency-study-v1.md) ·
[Research frontier](docs/OPEN_RESEARCH_FRONTIER.md) ·
[Contributing](CONTRIBUTING.md)

## The feedback loop

Aurora separates policy from transport execution. A generation is planned,
encoded, transmitted and evaluated; authenticated terminal feedback is applied
before the next plan.

```text
intent + deadline + traffic class
              ↓
      runtime-selected policy
              ↓
 generation plan + FEC protection
              ↓
 authenticated sender → receiver
              ↓
 receiver-local delivery outcome
              ↓
 terminal feedback → next plan
```

Fixed policies provide stable comparators. `biological-adaptive` carries state
across generations and may increase protection after failure, then relax after
successful recovery. All treatments use the same binary; policy selection is a
runtime argument.

## What exists today

| Area | Implemented capability | Evidence boundary |
|---|---|---|
| Generation transport | Parsed contracts, immutable descriptors, segmented deadlines, bounded repair and exact decode reports | Implemented and regression tested |
| Runtime policies | `fixed-minimum`, `fixed-class-aware`, `biological-adaptive` | Same binary across treatments |
| Process emulation | Separate sender/receiver processes, authenticated UDP frames, replay rejection and terminal ACK | Loopback, Docker and independent-host CI |
| Deterministic research engine | Channel traces, event kernel, arrivals, contact schedules, schedulers and decision replay | Synthetic simulation evidence |
| Cross-layer control | RF/optical/backscatter models, energy state, RIS, operating modes and safety envelope | Research models, not calibrated hardware |
| Provenance | Seeds, source/binary identities, trace hashes, evidence archives and checksum manifests | Detects drift and corruption; does not authenticate the producing build |
| Dashboard | Process launch, telemetry and health visualization | Monitoring works; live slider reload remains disabled |

For subsystem details, build profiles, protocol contracts and retained historical
evidence, see the [long-form technical reference](AURORA_TECHNICAL_REFERENCE.md).

## The current experiment

The completed pilot exposed all policies to the same deterministic generation-2
terminal failure. Generations 0–1 establish the pre-shock state; generation 2 is
the imposed perturbation; generations 3–7 measure the response.

<p align="center">
  <img src="docs/images/raw-host-post-shock-study-design-v1.svg" alt="Diagram of the frozen post-shock study: one binary, two runtime policies, a common generation-two failure, five post-shock generations and 23 paired randomized blocks." width="100%">
</p>

The proposed confirmatory follow-up compares `biological-adaptive` with
`fixed-class-aware` in 23 randomized complete blocks, for 46 fresh VM-pair
lifecycles. Its primary outcome is the post-shock sum of initial plus emitted
repair symbols over the five **scheduled** generations. Critical delivery before
receiver-local deadlines is a guardrail.

The design is pending review. It does not authorize GCP execution, and no result
from generation 2 may be interpreted as a policy win or loss.

## What the pilot showed

All policies delivered 2/2 critical generations before the shock and 5/5 after
it in both blocks. The imposed generation-2 failure was identical for every
treatment.

The biological policy changed its first post-failure plan; both fixed policies
remained invariant:

<p align="center">
  <img src="docs/images/raw-host-policy-pilot-v1-causal-response.svg" alt="Three plots showing fixed policy protection factors remaining constant while biological-adaptive raises critical and important protection after the imposed generation-two failure." width="100%">
</p>

Biological adaptation used more initial protection and fewer subsequent repair
symbols. Against `fixed-class-aware`, its post-shock wire total was 732 versus
800 in one block and 800 versus 800 in the other:

<p align="center">
  <img src="docs/images/raw-host-policy-pilot-v1-efficiency-signal.svg" alt="Stacked initial and repair symbol counts for fixed-class-aware and biological-adaptive in both descriptive pilot blocks." width="100%">
</p>

This is a **descriptive efficiency signal**, not a delivery advantage or a
policy-superiority claim. The frozen evidence, analysis and checksums are
published in the
[`raw-host-policy-pilot-v1-study-v4` Release](https://github.com/Daniele-Cangi/Aurora/releases/tag/raw-host-policy-pilot-v1-study-v4).

The standalone [evidence explorer](docs/raw-host-policy-pilot-v1-explorer.html)
lets readers step through the causal response, switch pilot blocks and compare
transport outcomes. It embeds the checksum-linked frozen input and performs no
network request; download or clone the repository and open the file locally.

## Quick start

Configure the dependency-light research profile:

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

Run the simulator:

```bash
./build/bin/aurora_x
```

Run the two-process loopback regression:

```bash
python tests/run_process_emulation.py \
  ./build/bin/aurora_process_emulation \
  benchmarks/process_timed_v2.trace \
  benchmarks/process_feedback_v2.trace \
  benchmarks/process_auth_test.key \
  0123456789abcdef
```

`USE_SODIUM=OFF` is an insecure deterministic test profile. Authenticated
transport claims require the libsodium build described in the
[technical reference](AURORA_TECHNICAL_REFERENCE.md#real-ed25519-build).

## Evidence discipline

Aurora distinguishes four evidence levels:

1. **Unit** — codec, parser, controller and state-machine behavior.
2. **Simulation** — declared synthetic channel, energy and topology models.
3. **Emulation** — real processes and sockets under controlled impairment.
4. **Field** — physical hardware, measured channels and calibrated resources.

Evidence from one level is never treated as proof of another. Experiments freeze
workloads, traces, policy definitions, randomization, measurement schemas and
analysis rules before dispatch. Final figures are generated from frozen analysis
artifacts and include checksum-linked provenance.

Key records:

- [Completed descriptive policy pilot](docs/raw-host-policy-pilot-v1-final-results.md)
- [Interactive frozen-evidence explorer](docs/raw-host-policy-pilot-v1-explorer.html)
- [Proposed confirmatory efficiency study](docs/raw-host-post-shock-efficiency-study-v1.md)
- [Machine-readable study design](benchmarks/gcp_raw_post_shock_efficiency_study_v1.json)
- [Frozen visualization contract](benchmarks/raw_host_post_shock_efficiency_visualization_v1.json)
- [Master research plan](AURORA_X_MASTER_PLAN.md)

## Repository map

```text
apps/                 Executables and benchmark front ends
include/aurora/       Transport, policy, safety, simulation and telemetry APIs
src/                  Compiled implementations and external-codec adapters
benchmarks/           Frozen traces, designs, measurement and analysis contracts
tools/                Planning, validation, evidence and visualization tooling
tests/                Unit, process, Docker and regression harnesses
docs/                 Study protocols, amendments, results and research notes
```

The full file-by-file guide and all advanced commands remain in
[AURORA_TECHNICAL_REFERENCE.md](AURORA_TECHNICAL_REFERENCE.md).

## Boundaries

- No current result establishes general policy superiority.
- Feedback RTT is sender-steady application RTT, not one-way or network-only latency.
- Energy, link and RIS models are synthetic research models.
- `FIELD_BUILD` still uses stubbed device operations and is not field evidence.
- The dashboard visualizes telemetry, but its controls do not yet reconfigure the live engine.
- The internal LT-like codec is experimental; optional external FEC integrations have separate profiles.

These limitations are part of the evidence contract, not footnotes.

## Contributing and license

Research contributions should state the question, fixed and changed factors,
comparison baseline, evidence level and outcome that would count against the
proposal. See [CONTRIBUTING.md](CONTRIBUTING.md).

Aurora is licensed under the [Apache License 2.0](LICENSE). Third-party notices
are recorded in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
