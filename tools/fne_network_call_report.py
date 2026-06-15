#!/usr/bin/env python3
"""Report when FNE hosts last received P25 network voice calls."""

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

VOICE_RE = re.compile(r"^network voice transmission from (?P<src>\d+) to TG (?P<dst>\d+)")
RF_VOICE_RE = re.compile(r"^RF voice transmission from (?P<src>\d+) to TG (?P<dst>\d+)")
END_RE = re.compile(
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


def format_time(value: datetime) -> str:
    return value.strftime("%H:%M:%S.%f")[:-3]


def iter_events(path: Path) -> Iterable[tuple[str, Event]]:
    with path.open("r", encoding="utf-8", errors="replace") as handle:
        for line_no, line in enumerate(handle, start=1):
            match = LOG_RE.match(line)
            if not match:
                continue

            side = match.group("side")
            message = match.group("message")
            voice_match = VOICE_RE.match(message) if side == "Net" else None
            rf_voice_match = RF_VOICE_RE.match(message) if side == "RF" else None
            end_match = END_RE.match(message) if side == "Net" else None
            rf_end_match = RF_END_RE.match(message) if side == "RF" else None
            if not voice_match and not end_match:
                if not rf_voice_match and not rf_end_match:
                    continue

            event = Event(
                line_no=line_no,
                peer=match.group("peer"),
                host=match.group("host").strip(),
                ts=parse_ts(match.group("ts")),
                message=message,
                src=(voice_match or rf_voice_match).group("src") if (voice_match or rf_voice_match) else None,
                dst=(voice_match or rf_voice_match).group("dst") if (voice_match or rf_voice_match) else None,
                duration=(end_match or rf_end_match).group("duration") if (end_match or rf_end_match) else None,
                loss=end_match.group("loss") if end_match else None,
                ber=rf_end_match.group("ber") if rf_end_match else None,
            )
            if voice_match:
                yield "voice", event
            elif rf_voice_match:
                yield "rf_voice", event
            elif rf_end_match:
                yield "rf_end", event
            else:
                yield "end", event


def markdown_table(headers: list[str], rows: list[list[str]]) -> str:
    widths = [len(header) for header in headers]
    for row in rows:
        for idx, cell in enumerate(row):
            widths[idx] = max(widths[idx], len(cell))

    def render_row(row: list[str]) -> str:
        return "| " + " | ".join(cell.ljust(widths[idx]) for idx, cell in enumerate(row)) + " |"

    separator = "| " + " | ".join("-" * width for width in widths) + " |"
    return "\n".join([render_row(headers), separator, *(render_row(row) for row in rows)])


def group_earlier_hosts(last_voice: dict[str, Event], final_start: datetime) -> list[list[str]]:
    return [
        [event.host, format_ts(event.ts)]
        for event in sorted(last_voice.values(), key=lambda item: (item.ts, item.host))
        if event.ts < final_start
    ]


def count_network_calls(events: list[Event]) -> int:
    return len(group_network_calls(events))


def group_network_calls(events: list[Event]) -> list[list[Event]]:
    calls: list[list[Event]] = []
    window = timedelta(milliseconds=FINAL_CALL_WINDOW_MS)

    for event in sorted(events, key=lambda item: item.ts):
        if calls:
            last_event = calls[-1][-1]
            if event.src == last_event.src and event.dst == last_event.dst and event.ts - last_event.ts <= window:
                calls[-1].append(event)
                continue
        calls.append([event])

    return calls


def rf_calls_without_network(rf_starts: list[Event], rf_ends: list[Event], net_starts: list[Event]) -> list[list[str]]:
    rows: list[list[str]] = []
    sorted_net_starts = sorted(net_starts, key=lambda item: item.ts)
    sorted_rf_ends = sorted(rf_ends, key=lambda item: item.ts)

    for start in sorted(rf_starts, key=lambda item: item.ts):
        end = next((event for event in sorted_rf_ends if event.host == start.host and event.ts >= start.ts), None)
        end_ts = end.ts if end else start.ts + timedelta(milliseconds=FINAL_CALL_WINDOW_MS)
        has_network = any(
            event.src == start.src and event.dst == start.dst and start.ts <= event.ts <= end_ts
            for event in sorted_net_starts
        )
        if has_network:
            continue

        rows.append([
            start.host,
            format_ts(start.ts),
            start.src or "",
            start.dst or "",
            f"{end.duration}s" if end and end.duration else "",
        ])

    return rows


def network_calls_without_rf_window(
    network_calls: list[list[Event]],
    rf_starts: list[Event],
    rf_ends: list[Event],
) -> list[list[str]]:
    rows: list[list[str]] = []
    sorted_rf_ends = sorted(rf_ends, key=lambda item: item.ts)

    for call in network_calls:
        first = call[0]
        matching_rf = []
        for start in rf_starts:
            if start.src != first.src or start.dst != first.dst:
                continue
            end = next((event for event in sorted_rf_ends if event.host == start.host and event.ts >= start.ts), None)
            end_ts = end.ts if end else start.ts + timedelta(milliseconds=FINAL_CALL_WINDOW_MS)
            if start.ts <= first.ts <= end_ts:
                matching_rf.append(start)

        if matching_rf:
            continue

        rows.append([
            format_ts(first.ts),
            first.src or "",
            first.dst or "",
            str(len({event.host for event in call})),
            ", ".join(sorted({event.host for event in call})),
        ])

    return rows


def build_report(path: Path, include_log: bool = True) -> str:
    last_voice: dict[str, Event] = {}
    voice_events: list[Event] = []
    rf_voice_events: list[Event] = []
    rf_end_events: list[Event] = []
    end_events: list[Event] = []

    for event_type, event in iter_events(path):
        if event_type == "voice":
            last_voice[event.host] = event
            voice_events.append(event)
        elif event_type == "rf_voice":
            rf_voice_events.append(event)
        elif event_type == "rf_end":
            rf_end_events.append(event)
        else:
            end_events.append(event)

    if not voice_events:
        raise SystemExit(f"No P25 Net network voice transmission entries found in {path}")

    final_voice_ts = max(event.ts for event in last_voice.values())
    final_start = final_voice_ts - timedelta(milliseconds=FINAL_CALL_WINDOW_MS)
    remaining = sorted(
        (event for event in last_voice.values() if event.ts >= final_start),
        key=lambda event: (event.ts, event.host),
    )
    final_src = remaining[0].src
    final_dst = remaining[0].dst
    final_rf_sources = [
        event
        for event in rf_voice_events
        if event.src == final_src and event.dst == final_dst and final_start <= event.ts <= final_voice_ts
    ]
    final_rf_source_hosts = {event.host for event in final_rf_sources}
    for host in final_rf_source_hosts:
        last_voice.pop(host, None)

    final_voice_max = max(event.ts for event in remaining)

    earlier_rows = group_earlier_hosts(last_voice, final_start)
    remaining_rows = [[event.host, format_ts(event.ts)] for event in remaining]
    total_hosts = len(earlier_rows) + len(remaining_rows)
    network_calls = group_network_calls(voice_events)
    no_network_rf_rows = rf_calls_without_network(rf_voice_events, rf_end_events, voice_events)
    no_rf_network_rows = network_calls_without_rf_window(network_calls, rf_voice_events, rf_end_events)

    lines: list[str] = []
    lines.append("# Network Call Report")
    lines.append("")
    lines.append("## Summary")
    lines.append("")
    lines.append(markdown_table(
        ["Metric", "Total"],
        [
            ["Hosts", str(total_hosts)],
            ["RF calls", str(len(rf_voice_events))],
            ["Network calls", str(len(network_calls))],
            ["RF calls with network fanout", str(len(rf_voice_events) - len(no_network_rf_rows))],
            ["RF calls without network fanout", str(len(no_network_rf_rows))],
            ["Network calls without matching RF window", str(len(no_rf_network_rows))],
        ],
    ))
    lines.append("")
    if no_network_rf_rows:
        lines.append("## RF calls without network fanout (kerchunks)")
        lines.append("")
        lines.append(markdown_table(["Host", "RF start", "Src", "TG", "Duration"], no_network_rf_rows))
        lines.append("")
    if no_rf_network_rows:
        lines.append("## Network calls without matching RF window")
        lines.append("")
        lines.append(markdown_table(["Network start", "Src", "TG", "Hosts", "Receiving hosts"], no_rf_network_rows))
        lines.append("")
    if earlier_rows:
        lines.append("## Hosts stopped receiving before end of report")
        lines.append("")
        lines.append(markdown_table(["Host(s)", "Last network voice seen"], earlier_rows))
        lines.append("")
    lines.append("## Hosts receiving network calls through the last observed network call in the report")
    lines.append("")
    lines.append(markdown_table(["Host", "Last network voice timestamp"], remaining_rows))

    if include_log:
        lines.append("")
        lines.append("## Log")
        lines.append("")
        lines.append("```text")
        with path.open("r", encoding="utf-8", errors="replace") as handle:
            lines.extend(line.rstrip("\n") for line in handle)
        lines.append("```")

    return "\n".join(lines)


def main() -> int:
    signal.signal(signal.SIGPIPE, signal.SIG_DFL)

    parser = argparse.ArgumentParser(
        description="Report each host's last P25 Net network voice transmission from an FNE activity log."
    )
    parser.add_argument("logfile", type=Path, help="FNE activity log to analyze")
    parser.add_argument("--no-log", action="store_true", help="Do not append the source log to the report")
    args = parser.parse_args()

    try:
        print(build_report(args.logfile, include_log=not args.no_log))
    except BrokenPipeError:
        try:
            sys.stdout.close()
        finally:
            os._exit(0)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
