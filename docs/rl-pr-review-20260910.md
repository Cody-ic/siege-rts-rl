# PR #163 review follow-up (2026-09-10)

The candidate is not approved for adoption. The latest published three-profile A/B evaluation failed its adoption gate; earlier partial evaluations and missing historical checkpoints do not constitute a passing result. Keep the scripted policy as the default. Do not substitute a new checkpoint and label it as completion of the missing historical cases.

## Preparation and sampling

Use `--defender-prepare-ticks 900 --curriculum 1.0` together. Preparation builds real defenses at the original spawn distance. Shortened-distance interpolation can place attackers inside the prepared city, so both configuration validation and the environment factory reject this combination before constructing worlds.

Preparation currently runs serially across environments and during resets. Its cost grows with environment count and preparation ticks; `--threads` does not make that preparation loop parallel. Record startup and reset wall time separately from rollout throughput before increasing the batch size. No parallel-speedup claim is made here.

Map selection cycles over the configured pool. Each map advances levels after visiting its own spawn points; maps with three and five spawns therefore change level on different rounds. Coverage is balanced over complete per-map cycles, not necessarily within a short rollout. The regression checks both maps over a common complete cycle. Preserve map order, seed, levels, and episode indices when comparing or resuming published runs.

## Evidence retention

Keep published preregistrations, configuration and checkpoint hashes, per-case results, and summaries in version control. Preserve existing published evidence, including failed runs. Store new raw logs, TensorBoard output, and checkpoint binaries outside Git; record their artifact location, hash, and retention owner in the run summary. A missing artifact must be marked unavailable. Each decision summary must identify the exact candidate, baseline, case set, and gate outcome.

Validation: `python tests/training_math_test.py` passes 11 tests without torch/native bindings, including preparation guards and mixed-map spawn/level coverage. These tests do not revalidate policy quality or training throughput.
