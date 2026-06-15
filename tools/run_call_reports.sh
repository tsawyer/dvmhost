#!/usr/bin/env bash
set -euo pipefail

LOG_GLOB="/var/log/dvm/dvmfne*activity.log"
REPORT_DIR="/var/www/dvm-activity/reports"
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
CALL_REPORT="${SCRIPT_DIR}/call_report.py"

usage() {
    cat <<EOF
Usage: $(basename -- "$0") [--today|--yesterday]

Options:
  --today      Generate only today's report
  --yesterday  Generate only yesterday's report
  -h, --help   Show this help

With no option, reports are generated for all matching logs.
EOF
}

target_date=""
case "${1:-}" in
    "")
        ;;
    --today)
        target_date="$(date +%F)"
        ;;
    --yesterday)
        target_date="$(date -v-1d +%F 2>/dev/null || date -d yesterday +%F)"
        ;;
    -h|--help)
        usage
        exit 0
        ;;
    *)
        usage >&2
        exit 2
        ;;
esac

mkdir -p "${REPORT_DIR}"

shopt -s nullglob
if [[ -n "${target_date}" ]]; then
    logs=( /var/log/dvm/dvmfne*"${target_date}"*activity.log )
else
    logs=( ${LOG_GLOB} )
fi

if (( ${#logs[@]} == 0 )); then
    if [[ -n "${target_date}" ]]; then
        echo "No logs matched date: ${target_date}" >&2
    else
        echo "No logs matched: ${LOG_GLOB}" >&2
    fi
    exit 1
fi

for log in "${logs[@]}"; do
    filename="$(basename -- "${log}")"

    if [[ "${filename}" =~ ([0-9]{4}-[0-9]{2}-[0-9]{2}) ]]; then
        report_date="${BASH_REMATCH[1]}"
    else
        echo "Skipping ${log}: could not find YYYY-MM-DD in filename" >&2
        continue
    fi

    output="${REPORT_DIR}/call_report_${report_date}.txt"
    "${CALL_REPORT}" "${log}" > "${output}"
    echo "Wrote ${output}"
done
