# Distillery Notes

The Distillery pipeline produces optimized debugger trajectories and training data from raw debug runs.

## Debugger DPO Generation

The script [`Scripts/generate_dbg_dpo.py`](Scripts/generate_dbg_dpo.py) builds step-level debugger DPO pairs from distilled `step_*.json` datasets.

It now uses both:

- `optimized_fix_<start>_<fix>.json`
- `original_fix_<start>_<fix>.json`

### Why `original_fix_*.json` matters

`original_fix_*.json` is preserved as structured evidence about the pre-optimization fix track. It is not treated as an automatically valid positive path, but it is still useful when mining synthetic rejects.

Current policy in the DPO generator:

- exact info-action matches from the original fix track are not used as rejects
- exact mapped original steps are tagged in output metadata
- `fix_function` matches against original fix subjects are tagged in output metadata only
- original fix subjects do not automatically shield a sampled reject

This is intentional:

- original trajectories may contain historically successful but now suboptimal or context-dependent actions
- optimized trajectories remain the preferred policy target
- the generator should avoid obvious false negatives without treating all original steps as correct

### Reject Kinds

Kept rejects are now tagged so training can filter them by policy:

- `hard_negative`
  - malformed output
  - schema violations
  - duplicate/invalid requests
  - grounded shortcuts such as jumping straight to the final fix too early
- `efficiency_negative`
  - structured, still-plausible debugger actions that differ from the optimized step
  - accepted to preserve optimized-trajectory preference data without pretending they are malformed or obviously invalid

This is a deliberate separation:

- valid-but-less-efficient alternatives should not be mixed blindly with malformed or clearly wrong negatives
- downstream training can keep only `hard_negative`, or include `efficiency_negative` as a softer preference signal
- when selecting a single reject for a step, the generator prefers structured non-`run_test` negatives over `run_test`, and `run_test` over malformed fallback outputs

### Output metadata

Generated DPO records may include:

- `reject_kind`
- `original_track_present`
- `matches_original_step_exact`
- `matches_original_fix_subject`

These fields are for auditing reject quality and understanding when original-track evidence influenced pair selection.

## Local Validation

Run:

```bash
python3 -m py_compile Environment/Distillery/Scripts/generate_dbg_dpo.py Environment/Distillery/Scripts/test_generate_dbg_dpo.py
python3 -m unittest discover -s Environment/Distillery/Scripts -p 'test_generate_dbg_dpo.py'
```

The deterministic tests cover:

- original-track metadata loading
- `hard_negative` vs `efficiency_negative` tagging
- suppression of exact original info-action rejects
- metadata tagging for original `fix_function` subjects
- propagation of the new metadata and trace files into emitted outputs
