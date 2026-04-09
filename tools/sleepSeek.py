#!/usr/bin/env python3
"""
Report P25 RF calls whose matching network reception was not seen on every host.

The script first scans the full input set to build the complete expected-host list.
It then walks the mirrored log in file order and reports RF calls that do not have
a matching network voice transmission on every other discovered host. File order is
used on purpose because host clocks may not agree. A call window starts at
`P25 RF RF ... voice transmission` and ends at `P25 RF RF end of transmission`.
"""

from __future__ import annotations

import argparse
import re
import sys
from collections import Counter
from dataclasses import dataclass, field
from datetime import datetime
from pathlib import Path
from typing import Iterable, Sequence


TIMESTAMP_FORMAT = "%Y-%m-%d %H:%M:%S.%f"

HOST_RE = re.compile(r"^(?P<peer>\d+)\s+\((?P<host>[^()\n]+)\)\s+A:\s+")
TIMESTAMP_RE = re.compile(
    r"A:\s+(?P<ts>\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d{3})\b"
)
RF_START_RE = re.compile(
    r"\bP25 RF RF (?P<encrypted>encrypted )?voice transmission from "
    r"(?P<src>\d+) to (?:(?P<tg>TG) )?(?P<dst>\d+)\b"
)
NET_START_RE = re.compile(
    r"\bP25 Net network (?P<encrypted>encrypted )?voice transmission from "
    r"(?P<src>\d+) to (?:(?P<tg>TG) )?(?P<dst>\d+)\b"
)
NET_END_RE = re.compile(r"\bP25 Net network end of transmission\b")
RF_END_RE = re.compile(r"\bP25 RF RF end of transmission\b")
RF_LOST_RE = re.compile(r"\bP25 RF transmission lost\b")


@dataclass(frozen=True, order=True)
class HostIdentity:
    peer_id: str
    name: str

    @property
    def label(self) -> str:
        return f"{self.peer_id} {self.name}"


@dataclass(order=True)
class Event:
    timestamp: datetime
    order: int
    kind: str
    host: HostIdentity
    src_id: int | None = None
    dst_id: int | None = None
    is_tg: bool = False
    encrypted: bool = False


@dataclass
class Call:
    timestamp: datetime
    source_host: HostIdentity
    src_id: int
    dst_id: int
    is_tg: bool
    encrypted: bool
    voice_hosts: set[HostIdentity] = field(default_factory=set)
    end_hosts: set[HostIdentity] = field(default_factory=set)
    rf_end_seen: bool = False

    def matches(self, event: Event) -> bool:
        if event.src_id is None or event.dst_id is None:
            return False
        if self.source_host == event.host:
            return False
        if self.src_id != event.src_id or self.dst_id != event.dst_id:
            return False
        if self.is_tg != event.is_tg or self.encrypted != event.encrypted:
            return False
        return True


@dataclass
class SleepInterval:
    host: HostIdentity
    start_timestamp: datetime
    last_proof_timestamp: datetime | None
    missed_calls: int
    wake_timestamp: datetime | None = None


@dataclass
class ReportStats:
    total_calls: int
    host_count: int
    heard_imperfect_calls: int
    voice_imperfect_calls: int
    end_incomplete_calls: int
    end_only_incomplete_calls: int


def parse_args(argv: Sequence[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Scan mirrored DVM activity logs and report RF calls that were not "
            "received by every discovered host. Input file order is preserved so "
            "clock skew between hosts does not scramble the matching. Each call "
            "window runs from P25 RF RF voice start to P25 RF RF end."
        )
    )
    parser.add_argument("logs", nargs="+", type=Path, help="activity log file(s) to scan")
    parser.add_argument(
        "--all",
        action="store_true",
        help="accepted for compatibility; every RF call is printed",
    )
    parser.add_argument(
        "--only-missing",
        action="store_true",
        help="print only RF calls that are missing one or more hosts",
    )
    parser.add_argument(
        "--show-hosts",
        action="store_true",
        help="accepted for compatibility; hosts are always printed",
    )
    parser.add_argument(
        "--sleep-report",
        action="store_true",
        help="print sleep transition intervals for hosts that stop showing call proof",
    )
    parser.add_argument(
        "--sleep-only",
        action="store_true",
        help="print host inventory, summary, and sleep transitions without per-call proof",
    )
    return parser.parse_args(argv)


def parse_host(line: str) -> HostIdentity | None:
    match = HOST_RE.search(line)
    if match is None:
        return None
    return HostIdentity(
        peer_id=match.group("peer").strip(),
        name=match.group("host").strip(),
    )


def parse_timestamp(line: str) -> datetime | None:
    match = TIMESTAMP_RE.search(line)
    if match is None:
        return None
    return datetime.strptime(match.group("ts"), TIMESTAMP_FORMAT)


def parse_event(line: str, host: HostIdentity | None, order: int) -> Event | None:
    if host is None:
        return None

    timestamp = parse_timestamp(line)
    if timestamp is None:
        return None

    for kind, pattern in (
        ("rf_start", RF_START_RE),
        ("net_start", NET_START_RE),
        ("net_end", NET_END_RE),
    ):
        match = pattern.search(line)
        if match is None:
            continue

        if kind == "net_end":
            return Event(timestamp=timestamp, order=order, kind=kind, host=host)

        return Event(
            timestamp=timestamp,
            order=order,
            kind=kind,
            host=host,
            src_id=int(match.group("src")),
            dst_id=int(match.group("dst")),
            is_tg=match.group("tg") is not None,
            encrypted=match.group("encrypted") is not None,
        )

    if RF_END_RE.search(line) or RF_LOST_RE.search(line):
        return Event(timestamp=timestamp, order=order, kind="rf_end", host=host)

    return None


def scan_logs(paths: Iterable[Path]) -> tuple[list[HostIdentity], list[Event]]:
    all_hosts: set[HostIdentity] = set()
    events: list[Event] = []

    order = 0
    for path in paths:
        with path.open("r", encoding="utf-8", errors="replace") as handle:
            for line in handle:
                order += 1
                host = parse_host(line)
                if host is not None:
                    all_hosts.add(host)

                event = parse_event(line, host, order)
                if event is not None:
                    events.append(event)

    return sorted(all_hosts), events


def finalize_call(
    call: Call, all_hosts: set[HostIdentity]
) -> tuple[Call, list[HostIdentity], list[HostIdentity], list[HostIdentity]]:
    expected_hosts = all_hosts - {call.source_host}
    heard_hosts = call.voice_hosts | call.end_hosts
    missing_heard_hosts = sorted(expected_hosts - heard_hosts)
    missing_voice_hosts = sorted(expected_hosts - call.voice_hosts)
    missing_end_hosts = sorted(expected_hosts - call.end_hosts)
    return call, missing_heard_hosts, missing_voice_hosts, missing_end_hosts


def find_matching_call(current_call: Call | None, event: Event) -> Call | None:
    if current_call is None:
        return None
    if current_call.matches(event):
        return current_call
    return None


def build_report(
    events: Sequence[Event], hosts: Sequence[HostIdentity]
) -> list[tuple[Call, list[HostIdentity], list[HostIdentity], list[HostIdentity]]]:
    all_hosts = set(hosts)
    closed_calls: list[tuple[Call, list[HostIdentity], list[HostIdentity], list[HostIdentity]]] = []
    current_call: Call | None = None

    for event in events:
        if event.kind == "rf_start":
            if current_call is not None:
                closed_calls.append(finalize_call(current_call, all_hosts))
            current_call = Call(
                timestamp=event.timestamp,
                source_host=event.host,
                src_id=event.src_id or 0,
                dst_id=event.dst_id or 0,
                is_tg=event.is_tg,
                encrypted=event.encrypted,
            )
            continue

        if event.kind == "net_start":
            call = find_matching_call(current_call, event)
            if call is not None:
                call.voice_hosts.add(event.host)
            continue

        if event.kind == "net_end":
            if current_call is not None and event.host != current_call.source_host:
                current_call.end_hosts.add(event.host)
            continue

        if event.kind == "rf_end":
            if current_call is not None and event.host == current_call.source_host:
                current_call.rf_end_seen = True
            continue

    if current_call is not None:
        closed_calls.append(finalize_call(current_call, all_hosts))

    return closed_calls


def build_sleep_report(
    hosts: Sequence[HostIdentity],
    calls: Sequence[tuple[Call, list[HostIdentity], list[HostIdentity], list[HostIdentity]]],
) -> list[SleepInterval]:
    last_proof_timestamp: dict[HostIdentity, datetime | None] = {host: None for host in hosts}
    active_intervals: dict[HostIdentity, SleepInterval] = {}
    intervals: list[SleepInterval] = []

    for call, missing_heard_hosts, _, _ in calls:
        missing_set = set(missing_heard_hosts)
        for host in hosts:
            has_proof = host == call.source_host or host not in missing_set

            if has_proof:
                if host in active_intervals:
                    interval = active_intervals.pop(host)
                    interval.wake_timestamp = call.timestamp
                    intervals.append(interval)
                last_proof_timestamp[host] = call.timestamp
                continue

            if host in active_intervals:
                active_intervals[host].missed_calls += 1
                continue

            active_intervals[host] = SleepInterval(
                host=host,
                start_timestamp=call.timestamp,
                last_proof_timestamp=last_proof_timestamp[host],
                missed_calls=1,
            )

    intervals.extend(active_intervals.values())
    intervals.sort(key=lambda interval: (interval.start_timestamp, interval.host.peer_id))
    return intervals


def missing_host_display(host: HostIdentity, name_counts: Counter[str]) -> str:
    if name_counts[host.name] == 1:
        return host.name
    return host.label


def summarize_calls(
    hosts: Sequence[HostIdentity],
    calls: Sequence[tuple[Call, list[HostIdentity], list[HostIdentity], list[HostIdentity]]],
) -> ReportStats:
    heard_imperfect_calls = 0
    voice_imperfect_calls = 0
    end_incomplete_calls = 0
    end_only_incomplete_calls = 0

    for call, missing_heard_hosts, missing_voice_hosts, missing_end_hosts in calls:
        heard_imperfect = bool(missing_heard_hosts)
        end_incomplete = bool(missing_end_hosts) or not call.rf_end_seen

        if heard_imperfect:
            heard_imperfect_calls += 1
        if missing_voice_hosts:
            voice_imperfect_calls += 1
        if end_incomplete:
            end_incomplete_calls += 1
        if end_incomplete and not heard_imperfect:
            end_only_incomplete_calls += 1

    return ReportStats(
        total_calls=len(calls),
        host_count=len(hosts),
        heard_imperfect_calls=heard_imperfect_calls,
        voice_imperfect_calls=voice_imperfect_calls,
        end_incomplete_calls=end_incomplete_calls,
        end_only_incomplete_calls=end_only_incomplete_calls,
    )


def print_report(
    hosts: Sequence[HostIdentity],
    calls: Sequence[tuple[Call, list[HostIdentity], list[HostIdentity], list[HostIdentity]]],
    only_missing: bool,
    show_call_details: bool,
) -> ReportStats:
    stats = summarize_calls(hosts, calls)
    name_counts = Counter(host.name for host in hosts)

    for host in hosts:
        print(host.label)
    print(f"Host count: {len(hosts)}\n")

    if show_call_details:
        for call, missing_heard_hosts, missing_voice_hosts, missing_end_hosts in calls:
            heard_imperfect = bool(missing_heard_hosts)
            if only_missing and not heard_imperfect:
                continue

            heard_count = len(call.voice_hosts | call.end_hosts) + 1
            voice_count = len(call.voice_hosts) + 1
            end_count = len(call.end_hosts) + (1 if call.rf_end_seen else 0)
            print(
                f"{call.timestamp.strftime(TIMESTAMP_FORMAT)[:-3]} "
                f"{call.source_host.peer_id} {call.source_host.name} "
                f"heard_records {heard_count}/{len(hosts)} "
                f"voice_records {voice_count}/{len(hosts)} "
                f"end_records {end_count}/{len(hosts)}"
            )
            if missing_heard_hosts:
                for host in missing_heard_hosts:
                    print(f"  missing {missing_host_display(host, name_counts)}")
            else:
                print("  missing -")

            if missing_voice_hosts:
                for host in missing_voice_hosts:
                    print(f"  voice-missing {missing_host_display(host, name_counts)}")
            else:
                print("  voice-missing -")

            if not call.rf_end_seen:
                print(f"  end-missing {missing_host_display(call.source_host, name_counts)}")
            if missing_end_hosts:
                for host in missing_end_hosts:
                    print(f"  end-missing {missing_host_display(host, name_counts)}")
            elif call.rf_end_seen:
                print("  end-missing -")

    if show_call_details:
        print()

    if stats.heard_imperfect_calls == 0:
        print(
            f"No calls were missing host proof across {stats.total_calls} RF calls "
            f"({stats.host_count} hosts)."
        )
    else:
        print(
            f"Scanned {stats.total_calls} RF calls across {stats.host_count} hosts; "
            f"{stats.heard_imperfect_calls} were missing host proof; "
            f"{stats.voice_imperfect_calls} were missing voice evidence; "
            f"{stats.end_incomplete_calls} had incomplete end evidence; "
            f"{stats.end_only_incomplete_calls} had end-only gaps."
        )

    return stats


def print_sleep_report(intervals: Sequence[SleepInterval]) -> None:
    print("\nSleep transitions")

    if not intervals:
        print("No sleep transitions detected.")
        return

    for interval in intervals:
        last_proof = (
            interval.last_proof_timestamp.strftime(TIMESTAMP_FORMAT)[:-3]
            if interval.last_proof_timestamp is not None
            else "none"
        )
        wake = (
            interval.wake_timestamp.strftime(TIMESTAMP_FORMAT)[:-3]
            if interval.wake_timestamp is not None
            else "log-end"
        )
        print(
            f"{interval.start_timestamp.strftime(TIMESTAMP_FORMAT)[:-3]} "
            f"{interval.host.label} sleep-start "
            f"after {last_proof} "
            f"missed_calls {interval.missed_calls} "
            f"wake {wake}"
        )


def main(argv: Sequence[str]) -> int:
    args = parse_args(argv)
    hosts, events = scan_logs(args.logs)

    if not hosts:
        print(
            "No host identifiers were found. Expected mirrored activity lines like "
            "'310658804 (AWP-HOME) A: 2026-04-08 12:00:00.000 ...'.",
            file=sys.stderr,
        )
        return 1

    calls = build_report(events, hosts)
    show_call_details = not args.sleep_only
    print_report(hosts, calls, args.only_missing, show_call_details)
    if args.sleep_report:
        print_sleep_report(build_sleep_report(hosts, calls))
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
