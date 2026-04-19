# DFSI Improvements

## Summary

The DFSI work in `codex/legacy-dfsi-clean` adds an opt-in, sequence-based receive path for P25 DFSI traffic while preserving the existing behavior as the default. The branch introduces a new `legacyDFSI` configuration flag, wires it through host startup, and updates `ModemV24` so both Motorola V.24 DFSI and TIA-102 DFSI receive handling can assemble LDUs from actual voice-frame sequencing instead of relying only on a running frame count.

The main practical benefit is safer LDU reconstruction when DFSI traffic is incomplete, out of order, or contains bad metadata. In the new path, LDU emission depends on seeing the expected contiguous voice-frame sequence, corrected LDU metadata is validated before use, and logging now shows which DFSI handling mode is active. This setting is independent of `dfsiTIAMode`: `dfsiTIAMode` chooses the DFSI framing style, while `legacyDFSI` chooses the receive-side reassembly behavior. Because `legacyDFSI` defaults to `true`, existing systems keep the current behavior until an operator explicitly enables the new handling.

## Scope

This document intentionally covers only the DFSI-related changes in `codex/legacy-dfsi-clean`.

The DFSI-specific changes are concentrated in these files:

| File | DFSI-related change |
| --- | --- |
| `configs/config.example.yml` | Adds the new `dfsi.legacyDFSI` option and documents its meaning. |
| `src/host/Host.Config.cpp` | Reads `legacyDFSI`, logs the selected mode, and passes it into `ModemV24`. |
| `src/host/modem/ModemV24.h` | Adds state for non-legacy sequence tracking plus new helper declarations. |
| `src/host/modem/ModemV24.cpp` | Implements the new non-legacy DFSI receive handling for both Motorola/V.24 and TIA-102 DFSI. |

## Detailed Changes

### 1. New DFSI configuration switch

The example configuration now exposes:

```yaml
system:
  modem:
    dfsi:
      legacyDFSI: true
```

This flag determines which receive-handling model `ModemV24` uses:

- `true`: keeps the legacy behavior.
- `false`: enables the newer sequence-based DFSI receive handling.

This setting is separate from `dfsiTIAMode`. `dfsiTIAMode` chooses the DFSI wire format, while `legacyDFSI` chooses how received DFSI voice frames are sequenced and emitted as LDUs.

The comment added to the example config makes the intent explicit: legacy handling remains the default, and the newer receive path is opt-in.

### 2. Host startup now propagates the DFSI mode explicitly

`Host.Config.cpp` now does three things with the new setting:

1. Reads `dfsi.legacyDFSI` from the YAML configuration, defaulting to `true`.
2. Logs `DFSI Legacy Handling: yes/no` during modem initialization so the active behavior is visible at startup.
3. Calls `ModemV24::setLegacyDFSI()` after constructing the DFSI modem instance.

This is important because it makes the change operationally safe: the new behavior is available without silently changing existing deployments.

### 3. `ModemV24` now supports two DFSI receive models

The core branch change is inside `ModemV24`.

New class state was added to support the non-legacy receive path:

- `m_legacyDFSI` controls whether legacy or non-legacy handling is active.
- `ldu1Seq` and `ldu2Seq` were added to `DFSICallData` so LDU1 and LDU2 progress can be tracked independently.
- `setLegacyDFSI()` was added as the public setter used by host startup.

Before the non-legacy path was added, receive-side LDU assembly was primarily driven by the running counter `m_rxCall->n`:

- emit LDU1 when `n == 9`
- emit LDU2 when `n == 18`

That model assumes voice frames arrive in the expected order and that counting arrivals is good enough to know when an LDU is complete.

In `codex/legacy-dfsi-clean`, that legacy counter-based behavior still exists when `legacyDFSI` is `true`, but a second path is available when it is `false`.

### 4. Sequence-based handling uses actual DFSI frame types

The new non-legacy path adds explicit sequence tracking for:

- `LDU1_VOICE1` through `LDU1_VOICE9`
- `LDU2_VOICE10` through `LDU2_VOICE18`

Helper logic maps each DFSI frame type to its expected sequence position and advances `ldu1Seq` or `ldu2Seq` only when the next contiguous frame arrives. If a frame arrives out of order, the relevant sequence tracker is reset instead of forcing an LDU to be emitted.

This changes the receive behavior in an important way:

- Legacy mode treats any nine or eighteen received voice frames as enough to build an LDU.
- Non-legacy mode only emits an LDU after the complete expected frame sequence has been observed in order.

That makes the non-legacy path more defensive when the DFSI stream has dropped, repeated, or reordered voice frames.

### 5. The new sequencing is applied to both DFSI input formats

The non-legacy handling is not limited to one wire format. The branch updates both:

- `convertToAirV24()` for Motorola serial/V.24 DFSI
- `convertToAirTIA()` for TIA-102 DFSI

In both functions:

- voice-frame processing still fills the same working buffers (`netLDU1`, `netLDU2`, `LDULC`, LSD fields, encryption fields, and error counts)
- legacy mode continues to increment `m_rxCall->n`
- non-legacy mode marks the arrival of a voice frame and runs the new sequence tracker to determine whether LDU1 or LDU2 is ready to emit

This means the behavioral improvement applies consistently regardless of whether the DFSI source is the Motorola-style serial path or the TIA-format path.

### 6. LDU1 metadata handling is stricter in the new path

The branch adds `decodeRxCallLDU1Metadata()` and `emitRxCallLDU1()` to centralize LDU1 emission for the non-legacy mode.

The new LDU1 flow:

- attempts Reed-Solomon correction on the LDU1 LC bytes
- rejects the LDU if RS correction fails
- rejects the LDU if the corrected LC bytes still do not decode into valid link-control metadata
- updates cached call metadata from the corrected LC when decoding succeeds

This is stricter than the legacy flow, which logged RS problems but continued assembling output from previously collected fields. The new behavior reduces the chance of emitting an LDU1 built from invalid corrected metadata.

### 7. LDU2 metadata handling is more fault-tolerant but still explicit

The branch also adds `decodeRxCallLDU2Metadata()` and `emitRxCallLDU2()`.

For LDU2, the new path:

- attempts RS correction on the LDU2 LC bytes
- refreshes encryption metadata (`MI`, `algId`, `kId`) when correction succeeds
- falls back to the last known encryption metadata when correction fails
- logs a warning when that fallback is used

So the new path is stricter for LDU1 identity/control metadata, but more tolerant for LDU2 encryption metadata. That is a sensible split: bad LDU1 control information is discarded, while LDU2 can still be emitted with the last usable crypto metadata if the audio block sequence itself is otherwise complete.

### 8. Error accounting and emission are now more intentionally scoped

The new helper-based emit path also localizes when sequence counters and error counters are cleared:

- `ldu1Seq` and `ldu2Seq` are reset after their respective emission attempts
- accumulated voice-frame error counts are logged and cleared as part of LDU emission

This makes the receive path easier to reason about because the sequencing, metadata validation, and emission steps are now grouped into dedicated helpers rather than being spread only across the legacy `n == 9` and `n == 18` branches.

### 9. Backward compatibility is preserved by default

The branch does not force the new behavior on existing systems.

Backward-compatibility safeguards include:

- `legacyDFSI` defaults to `true`
- host startup logs the selected handling mode
- the original counter-based path remains in place for both V.24 and TIA DFSI receive handling

As a result, the branch can be introduced without changing runtime behavior until an operator deliberately sets:

```yaml
system:
  modem:
    dfsi:
      legacyDFSI: false
```

### 10. Net effect of the DFSI work

Taken together, the DFSI improvements in `codex/legacy-dfsi-clean` do the following:

- keep the current DFSI behavior as the safe default
- add an opt-in sequence-based receive path
- require contiguous, correctly ordered voice-frame sequences before emitting LDUs in the new mode
- validate corrected LDU1 metadata before use
- preserve usable LDU2 output by reusing the last known encryption metadata when RS correction fails
- apply the same receive-side sequencing model to both Motorola/V.24 DFSI and TIA-102 DFSI processing

In short, the branch is less about changing the DFSI wire format and more about making DFSI receive reconstruction more explicit, configurable, and resilient without breaking existing deployments.
