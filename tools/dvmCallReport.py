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
import os
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
NET_WATCHDOG_RE = re.compile(r"\bP25 Net network watchdog has expired\b")
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
    watchdog_hosts: set[HostIdentity] = field(default_factory=set)
    recovery_src_id: int | None = None
    recovery_watchdog_hosts: set[HostIdentity] = field(default_factory=set)
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

    def matches_watchdog_recovery(self, event: Event) -> bool:
        if self.recovery_src_id is None:
            return False
        if event.src_id is None or event.dst_id is None:
            return False
        if event.host == self.source_host:
            return False
        if event.host not in self.recovery_watchdog_hosts:
            return False
        if event.src_id != self.recovery_src_id or event.dst_id != self.dst_id:
            return False
        if event.is_tg != self.is_tg or event.encrypted != self.encrypted:
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
    watchdog_calls: int


CallReport = tuple[
    Call,
    list[HostIdentity],
    list[HostIdentity],
    list[HostIdentity],
    list[HostIdentity],
]


def parse_args(argv: Sequence[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Scan mirrored DVM activity logs and report RF calls that were not "
            "received by every discovered host. Input file order is preserved so "
            "clock skew between hosts does not scramble the matching. Each call "
            "window runs from P25 RF RF voice start to P25 RF RF end. The selected "
            "log contents are printed after the report unless --no-logs is used."
        )
    )
    parser.add_argument("logs", nargs="+", type=Path, help="activity log file(s) to scan")
    parser.add_argument(
        "--no-logs",
        action="store_true",
        help="do not print the selected log contents after the report",
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
        ("net_watchdog", NET_WATCHDOG_RE),
    ):
        match = pattern.search(line)
        if match is None:
            continue

        if kind in {"net_end", "net_watchdog"}:
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
) -> CallReport:
    expected_hosts = all_hosts - {call.source_host}
    teardown_hosts = call.end_hosts | call.watchdog_hosts
    heard_hosts = call.voice_hosts | teardown_hosts
    missing_heard_hosts = sorted(expected_hosts - heard_hosts)
    missing_voice_hosts = sorted(expected_hosts - call.voice_hosts)
    missing_end_hosts = sorted(expected_hosts - teardown_hosts)
    watchdog_hosts = sorted(call.watchdog_hosts)
    return call, missing_heard_hosts, missing_voice_hosts, missing_end_hosts, watchdog_hosts


def find_matching_call(current_call: Call | None, event: Event) -> Call | None:
    if current_call is None:
        return None
    if current_call.matches(event):
        return current_call
    return None


def build_report(
    events: Sequence[Event], hosts: Sequence[HostIdentity]
) -> list[CallReport]:
    all_hosts = set(hosts)
    closed_calls: list[CallReport] = []
    current_call: Call | None = None

    for event in events:
        if event.kind == "rf_start":
            recovery_src_id: int | None = None
            recovery_watchdog_hosts: set[HostIdentity] = set()
            if current_call is not None:
                if (
                    current_call.watchdog_hosts
                    and current_call.dst_id == (event.dst_id or 0)
                    and current_call.is_tg == event.is_tg
                    and current_call.encrypted == event.encrypted
                ):
                    recovery_src_id = current_call.src_id
                    recovery_watchdog_hosts = set(current_call.watchdog_hosts)
                closed_calls.append(finalize_call(current_call, all_hosts))
            current_call = Call(
                timestamp=event.timestamp,
                source_host=event.host,
                src_id=event.src_id or 0,
                dst_id=event.dst_id or 0,
                is_tg=event.is_tg,
                encrypted=event.encrypted,
                recovery_src_id=recovery_src_id,
                recovery_watchdog_hosts=recovery_watchdog_hosts,
            )
            continue

        if event.kind == "net_start":
            call = find_matching_call(current_call, event)
            if call is not None:
                call.voice_hosts.add(event.host)
            elif (
                current_call is not None
                and current_call.matches_watchdog_recovery(event)
            ):
                current_call.voice_hosts.add(event.host)
            continue

        if event.kind == "net_end":
            if current_call is not None and event.host != current_call.source_host:
                current_call.end_hosts.add(event.host)
            continue

        if event.kind == "net_watchdog":
            if current_call is not None and event.host != current_call.source_host:
                current_call.watchdog_hosts.add(event.host)
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
    calls: Sequence[CallReport],
) -> list[SleepInterval]:
    last_proof_timestamp: dict[HostIdentity, datetime | None] = {host: None for host in hosts}
    active_intervals: dict[HostIdentity, SleepInterval] = {}
    intervals: list[SleepInterval] = []

    for call, missing_heard_hosts, _, _, _ in calls:
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


def format_timestamp(timestamp: datetime) -> str:
    return timestamp.strftime(TIMESTAMP_FORMAT)[:-3]


def format_duration_seconds(seconds: float) -> str:
    if seconds < 10:
        return f"{seconds:.1f}s"

    rounded = int(round(seconds))
    minutes, secs = divmod(rounded, 60)
    hours, minutes = divmod(minutes, 60)

    if hours:
        return f"{hours}h {minutes}m"
    if minutes:
        return f"{minutes}m {secs}s"
    return f"{secs}s"


def call_target_display(call: Call) -> str:
    if call.is_tg:
        return f"TG {call.dst_id}"
    return str(call.dst_id)


def call_label(call: Call) -> str:
    encryption = " enc" if call.encrypted else ""
    return (
        f"{format_timestamp(call.timestamp)} {call.source_host.label} "
        f"{call.src_id}->{call_target_display(call)}{encryption}"
    )


def compact_host_list(
    hosts: Sequence[HostIdentity], name_counts: Counter[str], limit: int = 4
) -> str:
    labels = [missing_host_display(host, name_counts) for host in hosts]
    if not labels:
        return "-"
    if len(labels) <= limit:
        return ", ".join(labels)
    return f"{', '.join(labels[:limit])}, +{len(labels) - limit} more"


def call_is_complete(
    call_info: CallReport
) -> bool:
    call, missing_heard_hosts, missing_voice_hosts, missing_end_hosts, _ = call_info
    return (
        not missing_heard_hosts
        and not missing_voice_hosts
        and not missing_end_hosts
        and call.rf_end_seen
    )


def describe_pattern_summary(
    hosts: Sequence[HostIdentity],
    calls: Sequence[CallReport],
    name_counts: Counter[str],
) -> str:
    imperfect_calls = [
        (call, missing_heard_hosts)
        for call, missing_heard_hosts, _, _, _ in calls
        if missing_heard_hosts
    ]
    if not imperfect_calls:
        return (
            "No evidence of sleeping hosts or mirrored-call gaps appears in this log slice."
        )

    expected_peer_count = max(len(hosts) - 1, 0)
    missing_sizes = [len(missing_heard_hosts) for _, missing_heard_hosts in imperfect_calls]
    if expected_peer_count and all(size == expected_peer_count for size in missing_sizes):
        return (
            "Every missing-peer event hit all peer hosts at once, which points away "
            "from a single sleeping host and toward a source-side or log-matching "
            "anomaly."
        )

    intervals = build_sleep_report(hosts, calls)
    repeated_intervals = sorted(
        (interval for interval in intervals if interval.missed_calls >= 2),
        key=lambda interval: (-interval.missed_calls, interval.host.peer_id),
    )
    if repeated_intervals:
        top_hosts = ", ".join(
            f"{missing_host_display(interval.host, name_counts)} "
            f"({interval.missed_calls} calls)"
            for interval in repeated_intervals[:3]
        )
        return (
            "Repeated host-specific misses suggest sleep or disconnect behavior on "
            f"{top_hosts}."
        )

    if all(size == 1 for size in missing_sizes):
        host_counts = Counter(
            missing_heard_hosts[0] for _, missing_heard_hosts in imperfect_calls
        )
        top_hosts = ", ".join(
            f"{missing_host_display(host, name_counts)} ({count})"
            for host, count in host_counts.most_common(3)
        )
        return (
            "Each missing-peer event affected only one host at a time, which looks "
            f"more like host-specific receive/sleep issues: {top_hosts}."
        )

    top_hosts = Counter(
        host for _, missing_heard_hosts in imperfect_calls for host in missing_heard_hosts
    )
    if top_hosts:
        most_affected = ", ".join(
            f"{missing_host_display(host, name_counts)} ({count})"
            for host, count in top_hosts.most_common(3)
        )
        return (
            "Missing-peer events affected subsets of hosts rather than everyone at "
            f"once. Most affected: {most_affected}."
        )

    return (
        "Missing-peer events affected subsets of hosts, so this looks more selective "
        "than a whole-network outage."
    )


def print_assessment(
    stats: ReportStats,
    hosts: Sequence[HostIdentity],
    calls: Sequence[CallReport],
    name_counts: Counter[str],
) -> None:
    print("Assessment")
    if (
        stats.heard_imperfect_calls == 0
        and stats.voice_imperfect_calls == 0
        and stats.end_incomplete_calls == 0
    ):
        print(f"  All {stats.total_calls} RF calls had full voice and teardown coverage on all {stats.host_count} hosts.")
        if stats.watchdog_calls:
            print(
                f"  {stats.watchdog_calls} call(s) used one or more peer watchdog timeouts "
                "instead of explicit end logs."
            )
        print(
            "  No evidence of sleeping hosts, mirrored-call gaps, or teardown issues "
            "appears in this log slice."
        )
        return

    if stats.heard_imperfect_calls == 0:
        print(f"  All {stats.total_calls} RF calls were heard on every host.")
        details: list[str] = []
        if stats.voice_imperfect_calls:
            details.append(
                f"{stats.voice_imperfect_calls} call(s) were missing one or more peer voice-start logs"
            )
        if stats.end_incomplete_calls:
            details.append(
                f"{stats.end_incomplete_calls} were missing one or more teardown logs"
            )
        if stats.watchdog_calls:
            details.append(
                f"{stats.watchdog_calls} had one or more peer watchdog timeouts"
            )
        if details:
            print(f"  {'; '.join(details)}.")
        print(
            "  This looks like partial start/end bookkeeping gaps rather than hosts "
            "sleeping through calls."
        )
        return

    print(f"  {stats.total_calls} RF calls across {stats.host_count} hosts.")
    details = [
        f"{stats.heard_imperfect_calls} call(s) were not seen on all peers",
    ]
    if stats.voice_imperfect_calls:
        details.append(
            f"{stats.voice_imperfect_calls} were missing one or more peer voice-start logs"
        )
    if stats.end_incomplete_calls:
        details.append(
            f"{stats.end_incomplete_calls} were missing one or more teardown logs"
        )
    if stats.watchdog_calls:
        details.append(
            f"{stats.watchdog_calls} had one or more peer watchdog timeouts"
        )
    print(f"  {'; '.join(details)}.")
    print(f"  {describe_pattern_summary(hosts, calls, name_counts)}")


def print_incident_report(
    hosts: Sequence[HostIdentity],
    calls: Sequence[CallReport],
    name_counts: Counter[str],
) -> None:
    incidents = [
        (index, call_info)
        for index, call_info in enumerate(calls)
        if not call_is_complete(call_info)
    ]
    if not incidents:
        return

    expected_peer_count = max(len(hosts) - 1, 0)
    print("\nNotable calls")

    for index, (call, missing_heard_hosts, missing_voice_hosts, missing_end_hosts, watchdog_hosts) in incidents:
        missing_heard_count = len(missing_heard_hosts)
        missing_voice_count = len(missing_voice_hosts)
        watchdog_count = len(watchdog_hosts)
        source_end_missing = not call.rf_end_seen
        total_end_missing = len(missing_end_hosts) + (1 if source_end_missing else 0)

        if expected_peer_count and missing_heard_count == expected_peer_count:
            headline = "source-only anomaly"
            detail = (
                f"All {expected_peer_count} peer hosts missed both voice and teardown."
            )
            interpretation = (
                "This points away from sleeping peers and toward a source-local or "
                "log-matching issue."
            )
        elif missing_heard_count:
            headline = "selective peer miss"
            detail = (
                f"{missing_heard_count} peer host(s) missed both voice and teardown: "
                f"{compact_host_list(missing_heard_hosts, name_counts)}."
            )
            interpretation = (
                "This is selective, so start by checking those hosts rather than "
                "assuming a network-wide problem."
            )
        elif missing_voice_count and total_end_missing == 0 and watchdog_count == 0:
            headline = "voice-start-only gap"
            detail = (
                f"{missing_voice_count} host(s) missed voice start but still logged "
                f"the end: {compact_host_list(missing_voice_hosts, name_counts)}."
            )
            interpretation = (
                "This looks more like late start logging than a fully missed call."
            )
        elif total_end_missing and missing_voice_count == 0 and watchdog_count == 0:
            headline = "teardown-only gap"
            end_hosts = list(missing_end_hosts)
            if source_end_missing:
                end_hosts = [call.source_host, *end_hosts]
            detail = (
                f"Teardown logs were missing on {total_end_missing} host(s): "
                f"{compact_host_list(end_hosts, name_counts)}."
            )
            interpretation = (
                "Voice was present everywhere, so this looks like teardown bookkeeping "
                "rather than a sleep event."
            )
        else:
            headline = "mixed log gap"
            detail_parts: list[str] = []
            if missing_voice_count:
                detail_parts.append(
                    f"voice-start missing on {missing_voice_count} host(s) "
                    f"({compact_host_list(missing_voice_hosts, name_counts)})"
                )
            if watchdog_count:
                detail_parts.append(
                    f"watchdog timeout on {watchdog_count} host(s) "
                    f"({compact_host_list(watchdog_hosts, name_counts)})"
                )
            if total_end_missing:
                end_hosts = list(missing_end_hosts)
                if source_end_missing:
                    end_hosts = [call.source_host, *end_hosts]
                detail_parts.append(
                    f"teardown logs missing on {total_end_missing} host(s) "
                    f"({compact_host_list(end_hosts, name_counts)})"
                )
            detail = f"{'; '.join(detail_parts)}."
            interpretation = (
                "This call has partial logging on some hosts, so it is worth checking "
                "whether the pattern repeats."
            )

        context_parts: list[str] = []
        if index > 0 and call_is_complete(calls[index - 1]):
            previous_call = calls[index - 1][0]
            context_parts.append(
                f"previous RF call was complete "
                f"{format_duration_seconds((call.timestamp - previous_call.timestamp).total_seconds())} "
                "earlier"
            )
        if index + 1 < len(calls) and call_is_complete(calls[index + 1]):
            next_call = calls[index + 1][0]
            context_parts.append(
                f"next RF call was complete "
                f"{format_duration_seconds((next_call.timestamp - call.timestamp).total_seconds())} "
                "later"
            )

        print(f"  {call_label(call)} {headline}")
        print(f"    {detail}")
        print(f"    {interpretation}")
        if context_parts:
            print(f"    Context: {'; '.join(context_parts)}.")


def summarize_calls(
    hosts: Sequence[HostIdentity],
    calls: Sequence[CallReport],
) -> ReportStats:
    heard_imperfect_calls = 0
    voice_imperfect_calls = 0
    end_incomplete_calls = 0
    watchdog_calls = 0

    for call, missing_heard_hosts, missing_voice_hosts, missing_end_hosts, watchdog_hosts in calls:
        heard_imperfect = bool(missing_heard_hosts)
        end_incomplete = bool(missing_end_hosts) or not call.rf_end_seen

        if heard_imperfect:
            heard_imperfect_calls += 1
        if missing_voice_hosts:
            voice_imperfect_calls += 1
        if end_incomplete:
            end_incomplete_calls += 1
        if watchdog_hosts:
            watchdog_calls += 1

    return ReportStats(
        total_calls=len(calls),
        host_count=len(hosts),
        heard_imperfect_calls=heard_imperfect_calls,
        voice_imperfect_calls=voice_imperfect_calls,
        end_incomplete_calls=end_incomplete_calls,
        watchdog_calls=watchdog_calls,
    )


def print_report(
    hosts: Sequence[HostIdentity],
    calls: Sequence[CallReport],
) -> ReportStats:
    stats = summarize_calls(hosts, calls)
    name_counts = Counter(host.name for host in hosts)

    for host in hosts:
        print(host.label)
    print(f"Host count: {len(hosts)}")
    print(f"Call count: {stats.total_calls}\n")

    print_assessment(stats, hosts, calls, name_counts)
    print_incident_report(hosts, calls, name_counts)

    return stats


def print_selected_logs(paths: Sequence[Path]) -> None:
    print("\nSelected logs")
    for index, path in enumerate(paths):
        if index:
            print()
        if len(paths) > 1:
            print(f"==> {path} <==")
        with path.open("r", encoding="utf-8", errors="replace") as handle:
            for line in handle:
                print(line, end="")


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
    print_report(hosts, calls)
    if not args.no_logs:
        print_selected_logs(args.logs)
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
