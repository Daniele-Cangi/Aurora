# Powered condition study v3: final result

## Status

`powered-injected-delay-condition-v3` completed its preregistered design with
four campaigns, eight complete blocks and 32 fresh-VM-pair lifecycles. No
blocks were excluded or replaced.

The immutable evidence freeze and reproducible final analysis are published in
the [`powered-condition-v3-study-v1` GitHub Release](https://github.com/Daniele-Cangi/Aurora/releases/tag/powered-condition-v3-study-v1).

## Frozen provenance

- Experimental tag: `powered-condition-v3-study-v1`
- Source commit: `0f30fab626a5cb6bcd6aab99dfc3046f9cf42930`
- Runtime binary SHA-256: `ae129eff764508cbfaf4a8147f15b15afd4ca19749dd55a6a15cd360ade94d36`
- Measurement profile: `application-controller-steady-v2`
- Authenticated process protocol: V2 with HMAC-SHA-256 through libsodium
- Primary outcome: sender-`steady_clock` `terminal_feedback_rtt_mean_us`

The study remained bound to the frozen tag after `main` advanced through
documentation-only PR #36. That change did not alter the experimental source,
workflow, benchmark plan or measurement contract. This document records the
completed result on `main` without moving or rewriting the experimental tag.

## Collection schedule

| Campaign | Blocks | GitHub Actions run | UTC start |
|---|---:|---:|---|
| `powered-v3-campaign-01` | 1–2 | [`31582307553`](https://github.com/Daniele-Cangi/Aurora/actions/runs/31582307553) | 2026-08-12 09:18:28 |
| `powered-v3-campaign-02` | 3–4 | [`31707670394`](https://github.com/Daniele-Cangi/Aurora/actions/runs/31707670394) | 2026-08-13 13:58:13 |
| `powered-v3-campaign-03` | 5–6 | [`31782066398`](https://github.com/Daniele-Cangi/Aurora/actions/runs/31782066398) | 2026-08-14 07:59:04 |
| `powered-v3-campaign-04` | 7–8 | [`31902536129`](https://github.com/Daniele-Cangi/Aurora/actions/runs/31902536129) | 2026-08-15 18:56:56 |

The campaigns occupied four distinct UTC dates and their starts were separated
by at least 18 hours. Every run used the same source commit and runtime binary.

## Preregistered analysis

The experimental unit was one fresh VM-pair lifecycle. Each analysis block
contained the four cells formed by two receiver regions and two declared
conditions. For each block, the primary contrast was the mean of the two
within-region `timed-replay-v2 − zero-delay-replay-v1` contrasts.

The registered primary test was a two-sided one-sample Student t-test of the
eight block contrasts against zero, with α = 0.05 and a two-sided 95% Student-t
confidence interval. Confirmatory success required `p < 0.05`, an interval
excluding zero and a positive point estimate.

## Final primary result

| Quantity | Result |
|---|---:|
| Complete blocks | 8 |
| Estimate, timed − zero-delay | **51.570 ms** |
| Two-sided 95% Student-t interval | **[50.778, 52.362] ms** |
| Test statistic | `t(7) = 154.024` |
| Two-sided p-value | `1.283075 × 10^-13` |
| Registered confirmatory criterion | **Met** |

The 5 ms minimum relevant effect was a prospective power alternative, not a
post-hoc decision threshold. The estimate and its complete 95% interval are
above 5 ms.

The eight block contrasts were 50.318, 50.176, 50.982, 51.849, 52.201,
52.624, 52.321 and 52.090 ms.

## Secondary and exploratory summaries

The region-specific contrasts were registered as secondary descriptive
quantities without multiplicity-adjusted claims:

| Receiver region | Mean timed − zero-delay | Descriptive 95% interval |
|---|---:|---:|
| `europe-west1-b` | 57.788 ms | [57.301, 58.275] ms |
| `us-west1-b` | 45.351 ms | [43.764, 46.938] ms |

The exploratory region-by-condition interaction, defined as the Europe
contrast minus the US West contrast, was 12.437 ms with a 95% interval of
[10.704, 14.170] ms. It is exploratory only and does not establish a causal
region effect or regional ranking.

## Technical validity and cleanup

- 32/32 lifecycle manifests passed the frozen technical gates.
- All eight complete blocks were retained; exclusions and replacements were zero.
- All samples used Measurement Contract V2 and authenticated Process Protocol V2.
- Every lifecycle had two terminal feedback samples, zero unknown feedback
  echoes, zero authentication rejections and positive replay rejection.
- Primary and independent cleanup manifests both covered 8/8 lifecycle records
  per campaign.
- Final direct GCP audits found zero matching instances, disks, addresses,
  snapshots, firewall rules, subnets and networks.

## Evidence and reproducibility

The release contains:

- `freeze-v3-20260815T211410Z.zip` — the eight original GitHub Actions artifact
  ZIPs, frozen source snapshot, run and artifact provenance, technical audit and
  per-file checksums;
- `analysis-v3-final-freeze-20260815T211410Z.zip` — the final JSON and Markdown
  reports, block-level inputs, reproducible analysis script and checksums;
- the corresponding `.sha256` sidecars;
- standalone `confirmatory-analysis.md` and `confirmatory-analysis.json` files.

Archive SHA-256 values:

- Evidence freeze: `b8ae902aa002631a60a2930d6a601e3819bdaf92a4c533d23e4c56d2c3e1c469`
- Final analysis: `be18e6a46e3e1703d355774db51c4b3ee27765920692892bab048f4af47fd32e`

## Interpretation boundary

This result supports the narrow declared emulation-condition contrast. It is
not calibrated field performance, network-only or one-way latency, a causal
regional effect, a regional ranking, hardware evidence or a general Internet
effect. Feedback RTT includes application processing, the declared impairment
profiles, network service and sender polling, all measured on the sender's
steady clock.
