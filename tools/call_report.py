#!/usr/bin/env python3
"""Report host network-call activity from a DVM FNE activity log."""

from __future__ import annotations

import argparse
import os
import re
import signal
import sys
from dataclasses import dataclass
from datetime import datetime, timedelta
from pathlib import Path
from typing import Iterable


LOG_RE = re.compile(
    r"^\s*(?P<peer>\d+)\s+\(\s*(?P<host>[^)]+?)\s*\)\s+A:\s+"
    r"(?P<ts>\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d{3})\s+"
    r"P25\s+(?P<side>Net|RF)\s+(?P<message>.+)$"
)
NET_VOICE_RE = re.compile(r"^network voice transmission from (?P<src>\d+) to TG (?P<dst>\d+)")
RF_VOICE_RE = re.compile(r"^RF voice transmission from (?P<src>\d+) to TG (?P<dst>\d+)")
NET_END_RE = re.compile(
    r"^network end of transmission, (?P<duration>[0-9.]+) seconds, "
    r"(?P<loss>\d+)% packet loss"
)
RF_END_RE = re.compile(r"^RF end of transmission, (?P<duration>[0-9.]+) seconds, BER: (?P<ber>[0-9.]+)%")

FINAL_CALL_WINDOW_MS = 1000


@dataclass(frozen=True)
class Event:
    line_no: int
    peer: str
    host: str
    ts: datetime
    message: str
    src: str | None = None
    dst: str | None = None
    duration: str | None = None
    loss: str | None = None
    ber: str | None = None


def parse_ts(value: str) -> datetime:
    return datetime.strptime(value, "%Y-%m-%d %H:%M:%S.%f")


def format_ts(value: datetime) -> str:
    return value.strftime("%Y-%m-%d %H:%M:%S.%f")[:-3]


def format_duration(start: datetime, end: datetime) -> str:
    return f"{(end - start).total_seconds():.3f}s"


def record_latest(events: dict[str, Event], event: Event) -> None:
    current = events.get(event.host)
    if current is None or event.ts > current.ts:
        events[event.host] = event


def markdown_table(headers: list[str], rows: list[list[str]]) -> str:
    widths = [len(header) for header in headers]
    for row in rows:
        for idx, cell in enumerate(row):
            widths[idx] = max(widths[idx], len(cell))

    def render_row(row: list[str]) -> str:
        return "| " + " | ".join(cell.ljust(widths[idx]) for idx, cell in enumerate(row)) + " |"

    separator = "| " + " | ".join("-" * width for width in widths) + " |"
    return "\n".join([render_row(headers), separator, *(render_row(row) for row in rows)])


def iter_events(path: Path) -> Iterable[tuple[str, Event]]:
    with path.open("r", encoding="utf-8", errors="replace") as handle:
        for line_no, line in enumerate(handle, start=1):
            match = LOG_RE.match(line)
            if not match:
                continue

            side = match.group("side")
            message = match.group("message")
            net_voice = NET_VOICE_RE.match(message) if side == "Net" else None
            rf_voice = RF_VOICE_RE.match(message) if side == "RF" else None
            net_end = NET_END_RE.match(message) if side == "Net" else None
            rf_end = RF_END_RE.match(message) if side == "RF" else None
            if not (net_voice or rf_voice or net_end or rf_end):
                continue

            voice = net_voice or rf_voice
            end = net_end or rf_end
            event = Event(
                line_no=line_no,
                peer=match.group("peer"),
                host=match.group("host").strip(),
                ts=parse_ts(match.group("ts")),
                message=message,
                src=voice.group("src") if voice else None,
                dst=voice.group("dst") if voice else None,
                duration=end.group("duration") if end else None,
                loss=net_end.group("loss") if net_end else None,
                ber=rf_end.group("ber") if rf_end else None,
            )

            if net_voice:
                yield "net_voice", event
            elif rf_voice:
                yield "rf_voice", event
            elif net_end:
                yield "net_end", event
            else:
                yield "rf_end", event


def rf_starts_without_end(rf_starts: list[Event], rf_ends: list[Event]) -> list[list[str]]:
    rows: list[list[str]] = []
    sorted_rf_ends = sorted(rf_ends, key=lambda item: item.ts)

    for start in sorted(rf_starts, key=lambda item: item.ts):
        end = next((event for event in sorted_rf_ends if event.host == start.host and event.ts >= start.ts), None)
        if end:
            continue
        rows.append([start.host, format_ts(start.ts), start.src or "", start.dst or ""])

    return rows


def matching_rf_end(start: Event, rf_ends: list[Event]) -> Event | None:
    return next(
        (
            event
            for event in sorted(rf_ends, key=lambda item: item.ts)
            if event.host == start.host and event.ts >= start.ts
        ),
        None,
    )


def network_voice_hosts_for_call(start: Event, end: Event, net_voice_events: list[Event]) -> set[str]:
    return {
        event.host
        for event in net_voice_events
        if event.src == start.src
        and event.dst == start.dst
        and start.line_no < event.line_no < end.line_no
    }


def network_end_hosts_for_call(start: Event, end: Event, net_end_events: list[Event]) -> set[str]:
    latest_end = end.ts + timedelta(milliseconds=FINAL_CALL_WINDOW_MS)
    return {
        event.host
        for event in net_end_events
        if event.line_no > start.line_no and start.ts <= event.ts <= latest_end
    }


def last_networked_rf_call(
    rf_voice_events: list[Event],
    rf_end_events: list[Event],
    net_voice_events: list[Event],
    net_end_events: list[Event],
) -> tuple[Event, Event, set[str]] | None:
    for start in sorted(rf_voice_events, key=lambda item: item.ts, reverse=True):
        end = matching_rf_end(start, rf_end_events)
        if not end:
            continue
        network_hosts = (
            network_voice_hosts_for_call(start, end, net_voice_events)
            | network_end_hosts_for_call(start, end, net_end_events)
        )
        if network_hosts:
            return start, end, network_hosts
    return None


def no_fanout_calls(
    rf_voice_events: list[Event],
    rf_end_events: list[Event],
    net_voice_events: list[Event],
) -> list[list[str]]:
    rows: list[list[str]] = []

    for start in sorted(rf_voice_events, key=lambda item: item.ts):
        end = matching_rf_end(start, rf_end_events)
        if not end or network_voice_hosts_for_call(start, end, net_voice_events):
            continue

        rows.append([
            start.host,
            format_ts(start.ts),
            format_ts(end.ts),
            start.src or "",
            start.dst or "",
            format_duration(start.ts, end.ts),
        ])

    return rows


def build_report(path: Path, include_log: bool = True) -> str:
    last_net_activity: dict[str, Event] = {}
    last_rf_end: dict[str, Event] = {}
    host_call_counts: dict[str, int] = {}
    net_voice_events: list[Event] = []
    net_end_events: list[Event] = []
    rf_voice_events: list[Event] = []
    rf_end_events: list[Event] = []

    for event_type, event in iter_events(path):
        if event_type == "net_voice":
            record_latest(last_net_activity, event)
            net_voice_events.append(event)
        elif event_type == "net_end":
            record_latest(last_net_activity, event)
            net_end_events.append(event)
        elif event_type == "rf_voice":
            host_call_counts[event.host] = host_call_counts.get(event.host, 0) + 1
            rf_voice_events.append(event)
        elif event_type == "rf_end":
            record_latest(last_rf_end, event)
            rf_end_events.append(event)

    if not net_voice_events:
        raise SystemExit(f"No P25 Net network voice transmission entries found in {path}")

    final_networked_call = last_networked_rf_call(
        rf_voice_events, rf_end_events, net_voice_events, net_end_events
    )
    if final_networked_call:
        final_rf_start, final_rf_end, network_hosts = final_networked_call
        active_hosts = {final_rf_start.host} | network_hosts
    else:
        final_rf_end = max(rf_end_events, key=lambda event: event.ts) if rf_end_events else None
        final_voice_ts = max(event.ts for event in last_net_activity.values())
        active_hosts = {
            event.host
            for event in last_net_activity.values()
            if event.ts >= final_voice_ts - timedelta(milliseconds=FINAL_CALL_WINDOW_MS)
        }

    earlier_rows = [
        [event.host, format_ts(event.ts)]
        for event in sorted(last_net_activity.values(), key=lambda item: (item.ts, item.host))
        if event.host not in active_hosts
    ]
    hosts_heard = sorted({row[0] for row in earlier_rows} | active_hosts)
    hosts_heard_rows = [
        [
            host,
            format_ts(last_rf_end[host].ts) if host in last_rf_end else "",
            str(host_call_counts[host]) if host in host_call_counts else "",
        ]
        for host in hosts_heard
    ]
    last_rf_end_rows = [[final_rf_end.host, format_ts(final_rf_end.ts)]] if final_rf_end else []

    no_call_end_rows = rf_starts_without_end(rf_voice_events, rf_end_events)
    no_fanout_rows = no_fanout_calls(rf_voice_events, rf_end_events, net_voice_events)

    summary_rows = [
        ["Hosts", str(len(hosts_heard))],
        ["Calls", str(len(rf_voice_events))],
        ["No Fan-out Calls", str(len(no_fanout_rows))],
        ["Early Last Call", str(len(earlier_rows))],
    ]
    if no_call_end_rows:
        summary_rows.append(["No Call end", str(len(no_call_end_rows))])

    lines: list[str] = [
        "# Network Call Report",
        "",
        f"Log: {path.name}",
        f"Run: {format_ts(datetime.now())}",
        "",
        "## Summary",
        "",
        markdown_table(["Metric", "Total"], summary_rows),
        "",
    ]

    lines.extend([
        "## Hosts Heard",
        "",
        markdown_table(["Host", "Last Heard", "Calls"], hosts_heard_rows),
        "",
    ])

    if no_call_end_rows:
        lines.extend([
            "## No Call end",
            "",
            markdown_table(["Host", "RF start", "Src", "TG"], no_call_end_rows),
            "",
        ])

    if no_fanout_rows:
        lines.extend([
            "## No Fan-out Calls",
            "",
            markdown_table(["Host", "RF start", "RF end", "Src", "TG", "Duration"], no_fanout_rows),
            "",
        ])

    if earlier_rows:
        lines.extend([
            "## Hosts stopped receiving before end of report",
            "",
            markdown_table(["Host(s)", "Early last call"], earlier_rows),
            "",
        ])

    lines.extend([
        "## Last Networked RF End of Transmission",
        "",
        markdown_table(["Host", "Timestamp"], last_rf_end_rows),
    ])

    if include_log:
        lines.extend(["", "## Log", "", "```text"])
        with path.open("r", encoding="utf-8", errors="replace") as handle:
            lines.extend(line.rstrip("\n") for line in handle)
        lines.append("```")

    return "\n".join(lines)


def main() -> int:
    signal.signal(signal.SIGPIPE, signal.SIG_DFL)

    parser = argparse.ArgumentParser(
        description="Report P25 network-call activity from an FNE activity log."
    )
    parser.add_argument("logfile", type=Path, help="FNE activity log to analyze")
    parser.add_argument("--no-log", action="store_true", help="Do not append the source log to the report")
    args = parser.parse_args()

    try:
        print(build_report(args.logfile, include_log=not args.no_log))
    except FileNotFoundError:
        print(f"Log file not found: {args.logfile}", file=sys.stderr)
        return 1
    except BrokenPipeError:
        try:
            sys.stdout.close()
        finally:
            os._exit(0)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
