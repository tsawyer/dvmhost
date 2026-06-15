#!/usr/bin/env python3
"""
Report P25 network-stream timing gaps inside calls.

Preferred input is a host journal exported with:

    journalctl -u dvmhost -o short-iso-precise

That format includes microsecond timestamps and P25 unit logs, allowing the
report to detect gaps between HDU/LDU/TDU records during a network call. FNE
activity logs are also accepted, but they only support call-level symptoms.
"""

from __future__ import annotations

import argparse
import os
import re
import sys
from collections import Counter
from dataclasses import dataclass, field
from datetime import datetime
from pathlib import Path
from statistics import median
from typing import Iterable, Sequence


DEFAULT_UNIT_GAP_MS = 500.0
TIMESTAMP_FORMAT = "%Y-%m-%d %H:%M:%S.%f"

ACTIVITY_HOST_RE = re.compile(r"^(?P<peer>\d+)\s+\((?P<host>[^()\n]+)\)\s+A:\s+")
ACTIVITY_TIMESTAMP_RE = re.compile(
    r"A:\s+(?P<ts>\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d{3})\b"
)
ACTIVITY_NET_START_RE = re.compile(
    r"\bP25 Net network (?P<encrypted>encrypted )?voice transmission from "
    r"(?P<src>\d+) to (?:(?P<tg>TG) )?(?P<dst>\d+)\b"
)
ACTIVITY_NET_END_RE = re.compile(r"\bP25 Net network end of transmission\b")
ACTIVITY_NET_WATCHDOG_RE = re.compile(r"\bP25 Net network watchdog has expired\b")

JOURNAL_RE = re.compile(
    r"^(?P<ts>\d{4}-\d{2}-\d{2}T\d\d:\d\d:\d\d\.\d{6}[+-]\d\d:?\d\d) "
    r"(?P<system>\S+) .*?dvmhost\[(?P<pid>\d+)\]: "
    r"(?P<level>[A-Z]): \((?P<side>NET|RF|HOST)\) (?P<msg>.*)$"
)
JOURNAL_VOICE_RE = re.compile(r"P25 Voice Call, srcId = (?P<src>\d+), dstId = (?P<dst>\d+)")
JOURNAL_UNIT_RE = re.compile(r"P25, (?P<unit>HDU|LDU1|LDU2|TDU|TDULC)\b")
IDENTITY_RE = re.compile(r"Identity:\s+(?P<identity>\S+)")
PEER_EVENT_RE = re.compile(r"\bPEER\b|master disconnect|timed out|retrying master login")


@dataclass(frozen=True, order=True)
class HostIdentity:
    peer_id: str
    name: str

    @property
    def label(self) -> str:
        if self.peer_id:
            return f"{self.peer_id} {self.name}"
        return self.name


@dataclass
class Event:
    timestamp: datetime
    order: int
    kind: str
    host: HostIdentity
    raw: str
    line_no: int
    src_id: int | None = None
    dst_id: int | None = None
    unit: str | None = None
    side: str | None = None


@dataclass
class JournalCall:
    index: int
    start: Event
    events: list[Event] = field(default_factory=list)
    gaps: list[tuple[float, Event, Event]] = field(default_factory=list)
    complete: bool = False


def parse_args(argv: Sequence[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Analyze in-call P25 network unit latency from precise dvmhost journals or "
            "mirrored FNE activity logs."
        )
    )
    parser.add_argument("logs", nargs="+", type=Path, help="journal/activity log file(s)")
    parser.add_argument(
        "--unit-gap-ms",
        type=float,
        default=DEFAULT_UNIT_GAP_MS,
        help=f"flag in-call P25 unit gaps at or above this value (default: {DEFAULT_UNIT_GAP_MS:g})",
    )
    parser.add_argument(
        "--top",
        type=int,
        default=30,
        help="maximum rows to print in each detail section (default: 30)",
    )
    return parser.parse_args(argv)


def parse_activity_event(line: str, line_no: int, order: int) -> Event | None:
    host_match = ACTIVITY_HOST_RE.search(line)
    ts_match = ACTIVITY_TIMESTAMP_RE.search(line)
    if host_match is None or ts_match is None:
        return None

    host = HostIdentity(host_match.group("peer").strip(), host_match.group("host").strip())
    timestamp = datetime.strptime(ts_match.group("ts"), TIMESTAMP_FORMAT)

    start_match = ACTIVITY_NET_START_RE.search(line)
    if start_match is not None:
        return Event(
            timestamp,
            order,
            "net_start",
            host,
            line.rstrip(),
            line_no,
            src_id=int(start_match.group("src")),
            dst_id=int(start_match.group("dst")),
            unit="VOICE",
            side="NET",
        )

    if ACTIVITY_NET_END_RE.search(line):
        return Event(timestamp, order, "net_end", host, line.rstrip(), line_no, side="NET")

    if ACTIVITY_NET_WATCHDOG_RE.search(line):
        return Event(timestamp, order, "watchdog", host, line.rstrip(), line_no, side="NET")

    return None


def parse_journal_event(
    line: str,
    line_no: int,
    order: int,
    identity: str | None,
) -> tuple[Event | None, str | None]:
    match = JOURNAL_RE.search(line)
    if match is None:
        return None, identity

    msg = match.group("msg").strip()
    identity_match = IDENTITY_RE.search(msg)
    if identity_match is not None:
        identity = identity_match.group("identity")

    host = HostIdentity("", identity or match.group("system"))
    timestamp = datetime.strptime(match.group("ts"), "%Y-%m-%dT%H:%M:%S.%f%z")
    side = match.group("side")

    voice_match = JOURNAL_VOICE_RE.search(msg)
    if voice_match is not None and side in {"NET", "RF"}:
        kind = "net_start" if side == "NET" else "rf_start"
        return (
            Event(
                timestamp,
                order,
                kind,
                host,
                line.rstrip(),
                line_no,
                src_id=int(voice_match.group("src")),
                dst_id=int(voice_match.group("dst")),
                unit="VOICE",
                side=side,
            ),
            identity,
        )

    unit_match = JOURNAL_UNIT_RE.search(msg)
    if unit_match is not None and side in {"NET", "RF"}:
        unit = unit_match.group("unit")
        kind = "net_unit" if side == "NET" else "rf_unit"
        if unit in {"TDU", "TDULC"}:
            kind = "net_end" if side == "NET" else "rf_end"
        return (
            Event(
                timestamp,
                order,
                kind,
                host,
                line.rstrip(),
                line_no,
                unit=unit,
                side=side,
            ),
            identity,
        )

    if side == "NET" and "network watchdog has expired" in msg:
        return Event(timestamp, order, "watchdog", host, line.rstrip(), line_no, side=side), identity

    if side == "NET" and PEER_EVENT_RE.search(msg):
        return Event(timestamp, order, "peer_event", host, line.rstrip(), line_no, side=side), identity

    return None, identity


def scan_logs(paths: Iterable[Path]) -> tuple[list[Event], bool, bool]:
    events: list[Event] = []
    saw_journal = False
    saw_activity = False
    order = 0
    identity: str | None = None

    for path in paths:
        with path.open("r", encoding="utf-8", errors="replace") as handle:
            for line_no, line in enumerate(handle, 1):
                order += 1
                journal_event, identity = parse_journal_event(line, line_no, order, identity)
                if journal_event is not None:
                    events.append(journal_event)
                    saw_journal = True
                    continue

                activity_event = parse_activity_event(line, line_no, order)
                if activity_event is not None:
                    events.append(activity_event)
                    saw_activity = True

    return events, saw_journal, saw_activity


def delta_ms(later: datetime, earlier: datetime) -> float:
    return (later - earlier).total_seconds() * 1000.0


def build_journal_calls(events: Sequence[Event]) -> list[JournalCall]:
    calls: list[JournalCall] = []
    active: JournalCall | None = None

    for event in events:
        if event.kind == "net_start":
            if active is not None:
                calls.append(active)
            active = JournalCall(len(calls) + 1, event, [event])
            continue

        if active is None:
            continue

        if event.side == "NET" and event.kind in {"net_unit", "net_end"}:
            previous_unit = next(
                (
                    prior
                    for prior in reversed(active.events)
                    if prior.side == "NET" and prior.kind in {"net_unit", "net_end"}
                ),
                None,
            )
            if previous_unit is not None:
                gap = delta_ms(event.timestamp, previous_unit.timestamp)
                if gap >= 0:
                    active.gaps.append((gap, previous_unit, event))
            active.events.append(event)
            if event.kind == "net_end":
                active.complete = True
                calls.append(active)
                active = None

    if active is not None:
        calls.append(active)
    return calls


def format_timestamp(timestamp: datetime) -> str:
    if timestamp.tzinfo is not None:
        return timestamp.isoformat(timespec="microseconds")
    return timestamp.strftime(TIMESTAMP_FORMAT)


def format_ms(value: float) -> str:
    if value >= 1000:
        return f"{value / 1000.0:.3f}s"
    return f"{value:.1f}ms"


def print_source_summary(paths: Sequence[Path], events: Sequence[Event]) -> None:
    print("Source")
    for path in paths:
        print(f"  File: {path}")
    calls = sorted(
        [event for event in events if event.kind == "net_start"],
        key=lambda event: (event.timestamp, event.order),
    )
    if calls:
        print(f"  First call: {format_timestamp(calls[0].timestamp)}")
        print(f"  Last call:  {format_timestamp(calls[-1].timestamp)}")
    print()


def print_journal_report(
    paths: Sequence[Path],
    events: Sequence[Event],
    unit_gap_ms: float,
    top: int,
) -> None:
    calls = build_journal_calls(events)
    net_p25_events = [
        event
        for event in events
        if event.side == "NET" and event.kind in {"net_start", "net_unit", "net_end"}
    ]
    watchdogs = [event for event in events if event.kind == "watchdog"]
    internal_gaps = [(gap, call, prev, event) for call in calls for gap, prev, event in call.gaps]
    flagged = [item for item in internal_gaps if item[0] >= unit_gap_ms]
    flagged.sort(key=lambda item: item[0], reverse=True)

    values = sorted(gap for gap, _, _, _ in internal_gaps)
    print("P25 In-Call Latency Report")
    print_source_summary(paths, events)
    print(f"  NET P25 calls: {len(calls)}")
    print(f"  NET P25 unit events: {len(net_p25_events)}")
    print(f"  Watchdog expiries: {len(watchdogs)}")
    print(f"  Unit gap threshold: {unit_gap_ms:g} ms")
    if values:
        p95 = values[int(0.95 * (len(values) - 1))]
        p99 = values[int(0.99 * (len(values) - 1))]
        print(
            "  In-call gap stats: "
            f"median {format_ms(median(values))}, p95 {format_ms(p95)}, "
            f"p99 {format_ms(p99)}, max {format_ms(values[-1])}"
        )

    thresholds = (250.0, 500.0, 750.0, 1000.0, 1500.0)
    print("\nThreshold counts")
    for threshold in thresholds:
        print(f"  >= {threshold:g} ms: {sum(1 for gap, _, _, _ in internal_gaps if gap >= threshold)}")

    if flagged:
        print("\nIn-call NET P25 unit gaps")
        for gap, call, previous, event in flagged[:top]:
            print(
                f"  {format_ms(gap):>9} call #{call.index} "
                f"{format_timestamp(call.start.timestamp)} "
                f"{call.start.src_id}->{call.start.dst_id} "
                f"lines {previous.line_no}->{event.line_no} "
                f"{previous.unit}->{event.unit}"
            )
            print(f"    next: {event.raw}")
        remaining = len(flagged) - top
        if remaining > 0:
            print(f"  ... {remaining} more not shown")
    else:
        print("\nIn-call NET P25 unit gaps")
        print("  No in-call gaps crossed the selected threshold.")

    if watchdogs:
        print("\nWatchdog events")
        for event in watchdogs[:top]:
            print(f"  line {event.line_no} {format_timestamp(event.timestamp)} {event.raw}")


def print_activity_report(paths: Sequence[Path], events: Sequence[Event], top: int) -> None:
    starts = [event for event in events if event.kind == "net_start"]
    watchdogs = [event for event in events if event.kind == "watchdog"]
    endings = [event for event in events if event.kind == "net_end"]
    per_host = Counter(event.host.label for event in starts)

    print("FNE Activity Call-Gap Report")
    print_source_summary(paths, events)
    print("  Note: activity logs do not contain individual P25 unit/frame timestamps.")
    print(f"  Network voice starts: {len(starts)}")
    print(f"  Network endings: {len(endings)}")
    print(f"  Watchdog expiries: {len(watchdogs)}")

    if per_host:
        print("\nPeer start counts")
        for host, count in per_host.most_common():
            print(f"  {host}: {count}")

    if watchdogs:
        print("\nWatchdog events")
        for event in watchdogs[:top]:
            print(f"  line {event.line_no} {format_timestamp(event.timestamp)} {event.raw}")


def main(argv: Sequence[str]) -> int:
    args = parse_args(argv)
    if args.unit_gap_ms < 0:
        print("gap thresholds must be non-negative", file=sys.stderr)
        return 2
    if args.top < 1:
        print("--top must be at least 1", file=sys.stderr)
        return 2
    events, saw_journal, saw_activity = scan_logs(args.logs)
    if not events:
        print("No recognizable journal or activity events found.", file=sys.stderr)
        return 1

    if saw_journal:
        print_journal_report(
            args.logs,
            events,
            args.unit_gap_ms,
            args.top,
        )
    elif saw_activity:
        print_activity_report(args.logs, events, args.top)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main(sys.argv[1:]))
    except BrokenPipeError:
        try:
            devnull = os.open(os.devnull, os.O_WRONLY)
            os.dup2(devnull, sys.stdout.fileno())
        except OSError:
            pass
        raise SystemExit(0)
