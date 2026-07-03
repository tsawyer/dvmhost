# Quantar Call Handling Improvements

AI WARNING: This document was in part generated using AI assistance. As such, there is the possibility of some error or inconsistency.

## Summary

This work represents roughly six months of DVMHost development and real-world testing on our network of nearly 20 Quantars. The main operational result is greatly improved Quantar call handling: fewer malformed or wedged calls, elimination of incorrect srcId and dstId, better recovery from missing or damaged LDU structure, cleaner teardown behavior, and less stale network state carrying into the next call.

One of the central fixes is moving away from the old assumption that counting received voice frames is enough to reconstruct a valid P25 LDU. Counting frames can tell us that nine voice frames arrived, but it cannot prove they were the correct frame types for the current LDU. That is a dangerous assumption for Quantar DFSI operation because a dropped, repeated, late, or misordered DFSI voice frame can still make the count look complete while leaving the resulting LDU structurally wrong. The improved receive path uses the DFSI frame identifiers/tags documented in the TIA-102 DFSI material to track LDU1 and LDU2 progress explicitly. In practice, DVMHost can recognize LDU1 voice 1 through 9 and LDU2 voice 10 through 18 by their DFSI frame identifiers instead of relying only on a running counter.

These changes and the others mentioned below improve field conditions by preserving valid audio slots, filling short structural gaps with null audio, repairing recoverable LC metadata, and preventing stale network state from wedging subsequent calls. The improvements include DFSI receive cleanup, P25 link-control hardening, network-to-DFSI recovery, stale stream cleanup, call teardown fixes, and follow-up recovery changes. The result is a more defensive P25 call path that can recover from realistic field damage while staying stricter about invalid call identity. The call-handling improvements are noticeable in day-to-day DVM network usage. Incorrect user IDs and talkgroups no longer appear during calls, and marginal signals are handled more gracefully instead of corrupting call state or leaving later calls wedged.

## Developer Map

The map below points to the main implementation areas behind the call-handling improvements. Each area lists the files and functions most useful for review.

### DFSI modem receive and conversion

- Files: `src/host/modem/ModemV24.cpp`, `src/host/modem/ModemV24.h`
- Key code: `DFSICallData`, `convertToAirV24()`, `convertToAirTIA()`, `updateRxVoiceSequence()`, `emitRxCallLDU1()`, `emitRxCallLDU2()`

### DFSI call identity and metadata recovery

- Files: `src/host/modem/ModemV24.cpp`, `src/host/modem/ModemV24.h`
- Key code: `decodeRxCallLDU1Metadata()`, `decodeRxCallLDU2Metadata()`, `recordRxVoiceErrors()`, `setP25SilenceThreshold()`

### Host P25 voice call handling

- Files: `src/host/p25/packet/Voice.cpp`, `src/host/p25/packet/Voice.h`
- Key code: `process()`, `processNetwork()`, `decodeNetLDU1Payload()`, `decodeNetLDU2Payload()`, `checkNetTrafficCollision()`, `isSameCallVoiceOverlap()`, `resolveRFTerminatorLC()`

### Host P25 control and diagnostics

- Files: `src/host/p25/Control.cpp`, `src/host/p25/Control.h`
- Key code: `processNetwork()`, `rememberNetworkFrame()`, `startNetworkWatchdog()`, `setNetGateBlocked()`, `logCallEndSummary()`, `writeRF_TDU()`

### FNE stale stream and teardown handling

- Files: `src/fne/network/callhandler/TagP25Data.cpp`, `src/fne/network/callhandler/TagP25Data.h`
- Key code: `resetMatchingCallStream()`, `suppressCallStream()`, `isSuppressedCallStream()`, `processFrame()`

### Supporting validation and network receive

- Files: `src/common/network/Network.cpp`, `src/common/network/BaseNetwork.cpp`, `src/common/edac/Golay24128.cpp`, `tests/edac/Golay24128_Tests.cpp`, `src/common/p25/acl/AccessControl.cpp`
- Key code: `readP25()`, `decode24128()`, `validateTGId()`

## Modem DFSI Receive Path

Improvements start in `ModemV24`, because this is where DFSI frames from V.24 serial or TIA DFSI UDP are reconstructed into P25 air-interface frames.

- **`DFSICallData` now carries recovery state.**  
  `src/host/modem/ModemV24.h` adds call-local fields for `maxVoiceFrameErrors`, `lastLDU1LC`, `lastLDU1LCValid`, `ldu1Seq`, and `ldu2Seq`. These fields let the modem distinguish a clean decode, a recoverable metadata failure, and an LDU that should be discarded.

- **The old counter-based DFSI receive trigger was replaced.**  
  The modem no longer treats a raw count of received voice frames as enough to prove that an LDU is structurally valid. DFSI voice frame identifiers now drive LDU1/LDU2 progress, which directly addresses the Quantar failure mode described in the Summary.

- **`convertToAirV24()` and `convertToAirTIA()` share the same recovery model.**  
  Both receive paths now feed errors through `recordRxVoiceErrors()`, store voice slots into `netLDU1` or `netLDU2`, and use `updateRxVoiceSequence()` plus `emitRxCallLDU1()` or `emitRxCallLDU2()`. This keeps Motorola/V.24 DFSI and TIA DFSI behavior aligned.

- **`updateRxVoiceSequence()` tracks LDU progress by DFSI frame identifier.**  
  The helper maps DFSI frame identifiers to LDU1 voice positions 1 through 9 and LDU2 voice positions 10 through 18, following the sequence model described by the TIA-102 DFSI documentation. This lets the receive path recognize the LDU boundary from the actual DFSI voice-frame tag. The current logic tolerates skipped subframes by advancing on later valid frame identifiers, rather than depending only on a raw count of received voice frames.

- **Old inline LDU emission was moved into shared helpers.**  
  The previous `n == 9` and `n == 18` emission blocks were replaced by `emitRxCallLDU1()` and `emitRxCallLDU2()`. Those helpers now own metadata decode, error reporting, sequence reset, P25 sync/NID generation, LC/LSD encoding, audio encoding, status bits, and converted-frame storage.

- **`create_TDU()` preserves modem end-of-transmission tags.**  
  The generated TDU frame is copied first, then `TAG_EOT` and the tag metadata are written. This prevents the generated air frame from overwriting the modem tag bytes needed by the DFSI call termination path.

## DFSI LC and Metadata Recovery

This area is the heart of the DFSI recovery improvement: keep recoverable calls alive, but do not trust bad identity.

- **`decodeRxCallLDU1Metadata()` validates corrected LDU1 LC before use.**  
  The modem now Reed-Solomon decodes LDU1 LC bytes and then calls `control.decodeLC()` before using source, destination, manufacturer, LCO, or service options. Bad corrected LC is treated as metadata failure.

- **Trusted LDU1 LC is cached only after non-zero identity.**  
  `decodeRxCallLDU1Metadata()` updates `lastLDU1LC` only when both `control.getSrcId()` and `control.getDstId()` are non-zero. That prevents a damaged or empty LC from becoming the basis for later recovery.

- **`emitRxCallLDU1()` can recover voice with trusted LC.**  
  If LDU1 metadata fails but the current call has trusted cached LC and voice errors are within `m_p25SilenceThreshold`, the helper emits the LDU using the cached LC. If no trusted LC exists, or the voice errors exceed the threshold, the LDU is discarded.

- **`recordRxVoiceErrors()` adds better error diagnostics.**  
  The modem now tracks total LDU errors and the highest single-frame error count. Logs include `maxFrameErrs`, which helps field debugging by showing whether a problem was spread across the whole LDU or concentrated in one voice frame.

- **`decodeRxCallLDU2Metadata()` updates encryption metadata only after correction.**  
  LDU2 MI, algorithm ID, and key ID are copied from the corrected LC buffer, not piecemeal from raw voice-frame additional data. If RS correction fails, `emitRxCallLDU2()` falls back to the last known encryption metadata and logs that recovery.

- **TIA DFSI LDU1 LC assembly no longer wipes earlier bytes.**  
  The TIA receive path now appends the voice 4 and voice 5 LC bytes without clearing the whole LDU1 LC FEC buffer. This protects earlier LDU1 metadata that was already captured from previous voice slots.

## Host Voice Processing and NET-to-DFSI Recovery

The host voice packet layer is where network P25 calls are admitted, repaired, converted into RF/DFSI output, or rejected.

- **`processNetwork()` repairs short network LDU payload damage.**  
  Network LDU1 and LDU2 handling now calls `decodeNetLDU1Payload()` and `decodeNetLDU2Payload()`. Those helpers decode payloads by expected DFSI voice slot, keep valid slots, and fill short missing or malformed slots with null IMBE. This keeps downstream DFSI structure complete without fabricating speech.

- **Missing LDU alternation is repaired with null audio.**  
  If another LDU1 arrives before the expected LDU2, `processNetwork()` logs the missing LDU2 and writes a null-audio LDU2. The reverse path does the same for missing LDU1. This protects DFSI devices from repeated LDU halves that would otherwise disrupt superframe structure.

- **`rememberNetworkFrame()` records admission and rejection reasons.**  
  The voice path now records outcomes such as `NET_LDU1_ADMITTED`, `NET_LDU2_ADMITTED`, `NET_LDU1_SHAPE_INVALID`, and `NET_LDU2_SHAPE_INVALID`. These records are later useful for sleep-gate and stale-stream diagnostics.

- **`startNetworkWatchdog()` is called only after admitted traffic.**  
  The watchdog starts when LDU traffic is actually accepted into the Net-to-RF/DFSI path, instead of starting from traffic that might later be dropped. This avoids false watchdog expiry on rejected or blocked frames.

- **`checkNetTrafficCollision()` now recognizes same-call overlap.**  
  `isSameCallVoiceOverlap()` identifies network voice that overlaps the active RF call for the same source/destination rather than treating it as a different collision. This policy belongs in `Voice.cpp`, where the DUID and active voice state are available.

- **TGID 0 is rejected in DFSI group LDU1 handling.**  
  `Voice::process()` rejects DFSI group LDU1 frames with destination TGID 0. The old `forceAllowTG0` configuration path was removed, and TGID 0 filtering now relies on `AccessControl::validateTGId()` plus explicit DFSI LDU1 rejection. This prevents blackhole talkgroup traffic from becoming valid call state.

- **Late-entry DFSI calls can start from valid LDU1.**  
  For DFSI/V.24, HDU destination data can be unreliable or absent. The voice path avoids treating a bad HDU destination as authoritative and lets the first valid LDU1 establish the talkgroup when needed.

- **RF undecodable LC falls back to prior call context.**  
  RF LDU1 processing can reuse the last known LDU1 LC when a current LDU1 is undecodable, repairing source and destination IDs where possible. RF LDU2 processing can reuse encryption context and regenerate MI continuity. These are separate from modem cached-LC recovery, but they solve the same operational issue: do not drop a valid ongoing call only because one LC decode failed.

- **`resolveRFTerminatorLC()` improves teardown identity.**  
  RF TDU handling now tries to repair missing source/destination context before writing network call termination. If no active context exists, the host avoids sending a malformed network termination with zero IDs.

## Host Control, Sleep, and Call Lifetime Diagnostics

`Control.cpp` ties network read, gate decisions, watchdogs, and call-end reporting together.

- **`Control::processNetwork()` records blocked and invalid network frames.**  
  The control path records conditions such as null reads, blocked gates, invalid frame length, and later voice admission results. This makes it easier to understand why UDP packets may still be arriving while no useful P25/DFSI audio progresses.

- **`setNetGateBlocked()` logs transitions instead of repeated noise.**  
  Gate-block diagnostics are stateful, so logs show when the Net-to-RF path becomes blocked and when it clears. That helps diagnose sleep-test behavior without flooding logs on every clock cycle.

- **`logCallEndSummary()` provides consistent end-of-call reporting.**  
  RF terminators, network terminators, watchdog expiry, and RF frame loss now share a clearer summary path. Developers investigating stuck calls should check this function and the call sites in `Voice.cpp` and `Control.cpp`.

- **`writeRF_TDU()` is used more carefully.**  
  Teardown code distinguishes local RF cleanup from network-facing call termination. That prevents invalid TDUs from being forwarded after source or destination identity has already been cleared.

## FNE Stale Stream and Teardown Handling

FNE changes prevent stale or malformed peer traffic from keeping calls stuck active after the host has decided the stream is no longer valid.

- **`resetMatchingCallStream()` clears stale peer stream state.**  
  Given peer ID, SSRC, and stream ID, the FNE can now find the active call, clear group and private-call status, decrement active-call accounting where appropriate, and erase stream packet sequence state.

- **Invalid TDUs reset matching call streams.**  
  In `TagP25Data::processFrame()`, all-zero TDUs and no-destination TDUs are rejected. When they match an active stream, `resetMatchingCallStream()` is called so malformed teardown traffic does not leave the FNE stuck with an active call.

- **`suppressCallStream()` and `isSuppressedCallStream()` block stale collision traffic.**  
  After collision timeout, the FNE can suppress the old stream by peer, SSRC, source, destination, and stream ID. Matching stale frames are dropped until a TDU/TDULC arrives or the bounded suppression age expires.

- **Call termination authority is checked.**  
  `processFrame()` validates that `LC_CALL_TERM` comes from the peer that owns the active call. This prevents unrelated peers from ending or corrupting another stream's call state.

## Network Transport and Decode Guardrails

These changes protect call handling before frames reach the higher-level voice and DFSI recovery paths. They are not Quantar-specific by themselves, but they matter to Quantar operation because bad transport buffering or bad protected metadata can produce the same field symptoms as a DFSI sequencing problem: malformed voice frames, incorrect call identity, stuck calls, or audio that stops even though network packets are still arriving.

- **The P25 network receive ring preserves long payloads.**  
  Longer P25 network packets were being announced to the reader as one size but only partly stored in the receive ring. That could make DVMHost read missing or shifted bytes as if they were real P25 voice data, corrupting later call frames even while UDP packets were still arriving normally. `src/common/network/Network.cpp` now writes the full packet payload after the compact two-byte length prefix for P25 frames longer than 254 bytes. `BaseNetwork::readP25()` expects the reconstructed full length, so preserving the full payload prevents ring underflow and corrupt frame reads.

- **Golay(24,12,8) decode validity was tightened and tested.**  
  Golay(24,12,8) protects compact P25 signaling fields by carrying 12 bits of data in a 24-bit codeword capable of correcting up to three bit errors. The bug fixed here was in decode-validity reporting: some bad protected data could appear usable. `Golay24128::decode24128()` now reports decode validity more accurately, and focused coverage was added in `tests/edac/Golay24128_Tests.cpp` for zero data, all-ones data, normal patterns, correctable errors, uncorrectable errors, and byte-array behavior. This supports DFSI/P25 paths that depend on Golay-protected voice header and signaling fields. Test coverage was checked with the P25 test set; all P25 tests pass except `./build/tests/dvmtests "[p25][kmm_cmac]"` and `./build/tests/dvmtests "[aes][mac_cmac]"`, and those same two tests also fail on master.

## AI Assistance

This document and the related code were developed with assistance from Codex. This document was mostly human-written and has been reviewed carefully for accuracy. The code has received human review within the author’s expertise and available equipment. Given the size and complexity of this work, developer peer review and continued field validation are recommended. Of course, this work is for non-commercial amateur radio use only.