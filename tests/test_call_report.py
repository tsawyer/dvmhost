import importlib.util
import sys
import tempfile
import unittest
from pathlib import Path


SCRIPT_PATH = Path(__file__).parents[1] / "tools" / "call_report.py"
SPEC = importlib.util.spec_from_file_location("call_report", SCRIPT_PATH)
assert SPEC is not None and SPEC.loader is not None
call_report = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = call_report
SPEC.loader.exec_module(call_report)


class CallReportTests(unittest.TestCase):
    def test_final_network_end_counts_as_host_activity(self) -> None:
        log = """\
310658807 ( AWP-SYP) A: 2026-06-14 19:23:47.893 P25 RF RF voice transmission from 3106588 to TG 10253
310658803 ( AWP-OAT) A: 2026-06-14 19:23:48.085 P25 Net network voice transmission from 3106588 to TG 10253
310658807 ( AWP-SYP) A: 2026-06-14 19:23:48.497 P25 RF RF end of transmission, 0.7 seconds, BER: 0.0%
310658802 ( AWP-STGO) A: 2026-06-14 19:23:48.500 P25 Net network end of transmission, 0.7 seconds, 0% packet loss
310658803 ( AWP-OAT) A: 2026-06-14 19:23:48.505 P25 Net network end of transmission, 0.7 seconds, 0% packet loss
"""
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "activity.log"
            path.write_text(log, encoding="utf-8")
            report = call_report.build_report(path, include_log=False)

        self.assertRegex(report, r"\| Early Last Call\s+\| 0\s+\|")
        self.assertNotIn("Hosts stopped receiving before end of report", report)


if __name__ == "__main__":
    unittest.main()
