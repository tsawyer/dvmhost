# P25 NET-to-DFSI Recovery

## Goal

This branch makes the host more tolerant when P25 voice arrives from the FNE/network and is being fed to a Quantar or other V.24 DFSI device.

The working field failure is that UDP traffic from the FNE can continue reaching DVMHost while DVMHost stops processing/logging inbound P25 audio until a local RF key-up or service restart. The target is the NET-to-DFSI side of the host path, not FNE fan-out and not DFSI received from the Quantar.

## Working Theory

Quantar-era DFSI links were normally carried over deterministic microwave or wired channel-bank transport. They likely tolerate imperfect audio better than malformed or discontinuous DFSI voice structure.

The host should therefore smooth small network irregularities before they reach the DFSI side:

- preserve expected DFSI voice-frame order
- fill short missing voice-frame slots with null IMBE
- keep LDU1/LDU2 output structurally complete
- log every recovery so real field traffic can confirm or disprove the theory
- reset at clean stream boundaries rather than letting one malformed network payload poison the call path

## Initial Implementation

The first implementation keeps behavior narrow and observable:

- `Network::clock()` now preserves the full payload when queueing P25 network packets longer than 254 bytes. The ring buffer used a two-byte length encoding for these packets, but only appended the `length - 254` remainder bytes. `BaseNetwork::readP25()` then expected the full decoded length, causing a P25 net-ring underflow and a zero-filled/corrupt frame. This can produce the field symptom where tcpdump shows inbound UDP but DVMHost stops processing useful P25.
- `Voice::processNetwork()` no longer silently ignores an LDU just because one embedded DFSI voice-frame slot is malformed or missing.
- Each NET LDU is decoded by expected DFSI slot.
- Valid slots are decoded normally.
- Missing or unexpected slots are left as null IMBE.
- The host logs `P25, NET-to-DFSI Recovery` with DUID, srcId, dstId, and recovered slot count.

This is intentionally conservative. It does not invent long stretches of audio and it does not change FNE collision ownership. It only prevents a short malformed NET LDU payload from starving the downstream DFSI path.

## Receive Ring Bug

While reviewing the network receive path, we found a concrete DVMHost-side bug that can explain "tcpdump still sees UDP, but DVMHost stops processing P25."

Incoming P25 network packets are queued in `m_rxP25Data` with a compact length prefix. For packets up to 254 bytes, the code writes one length byte followed by the packet payload. For packets longer than 254 bytes, the code writes `254` followed by a second byte containing `length - 254`; `BaseNetwork::readP25()` then reconstructs the total length as `254 + remainder`.

The bug was that `Network::clock()` reused the shortened remainder byte as the payload length. So a 300-byte packet was queued as:

- length prefix: `254`, `46`
- payload bytes written: `46`
- payload bytes later read: `300`

That leaves the P25 network ring misaligned or underflowed. Later P25 packets can still arrive from UDP, but the host-side P25 frame reader is consuming corrupt ring-buffer state instead of valid P25 frames.

The fix is intentionally small: keep the two-byte length prefix, but always append the full original packet length to `m_rxP25Data`.

## Follow-up Isolated Fixes

During the post-deployment audit we found a few unrelated but obvious P25/DFSI cleanup defects. These are kept small so they can be reviewed without changing the wider sleep-test theory:

- `ModemV24::create_TDU()` generated a TDU and then overwrote the modem tag bytes with the generated air frame copy. The helper now copies the generated frame first and then sets `TAG_EOT`.
- The TIA DFSI end-of-stream path queued a generated TDU using `P25_TDU_FRAME_LENGTH_BITS` as a byte count. It now uses `P25_TDU_FRAME_LENGTH_BYTES`.
- The TIA DFSI LDU1 voice 4 and voice 5 handlers cleared the whole LC FEC buffer after earlier LDU1 LC bytes had already been captured. They now append their LC bytes without erasing the earlier pieces.
- Host RF-to-network TDU forwarding now refuses to send a network TDU when the current RF call identity has already been cleared to `srcId = 0` or `dstId = 0`. The FNE already rejects those invalid TDUs, so suppressing them at the host keeps the journal cleaner without changing a valid call path.
