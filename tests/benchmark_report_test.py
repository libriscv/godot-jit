"""Performance gates must reject incomplete, corrupted and regressed results."""
import contextlib
import copy
import io
import unittest
from benchmark_report import check_gate, parse_log, WORKLOADS


class BenchmarkReportTests(unittest.TestCase):
    def setUp(self):
        self.report = {"fixture_sha256": "fixture", "results": [
            {"workload": name, "gd_samples_us": [100] * 7, "jit_samples_us": [80] * 7}
            for name in WORKLOADS["typical"]]}
        self.baseline = {"regression_gate": {"fixture_sha256": "fixture",
                         "max_jit_gd": dict.fromkeys(WORKLOADS["typical"], .9)}}

    def gate(self):
        with contextlib.redirect_stdout(io.StringIO()):
            check_gate(self.report, self.baseline)

    def test_accepts_medians_not_outliers(self):
        self.report["results"][0]["jit_samples_us"][-1] = 10000
        self.gate()

    def test_rejects_regression_even_with_forged_ratio(self):
        self.report["results"][0].update(jit_samples_us=[95] * 7, ratio=.1)
        with self.assertRaisesRegex(ValueError, "regression"):
            self.gate()

    def test_rejects_missing_duplicate_and_changed_fixture(self):
        original = copy.deepcopy(self.report)
        for mutation in (lambda: self.report["results"].pop(),
                         lambda: self.report["results"].append(self.report["results"][0]),
                         lambda: self.report.update(fixture_sha256="changed")):
            self.report = copy.deepcopy(original)
            mutation()
            with self.assertRaises(ValueError):
                self.gate()

    def test_rejects_invalid_samples(self):
        for value in (0, -1, float("nan"), float("inf")):
            self.report["results"][0]["gd_samples_us"][0] = value
            with self.assertRaisesRegex(ValueError, "Invalid timing"):
                self.gate()

    def test_complete_log_and_engine_errors(self):
        log = '\n'.join(f'{name}: GD=0.2 us/op JIT=0.1 us/op' for name in WORKLOADS["typical"])
        self.assertEqual(len(parse_log(log, "typical", 10000)), 13)
        for invalid in (log.splitlines()[0], log + '\n' + log.splitlines()[0], log + '\nSCRIPT ERROR: failure'):
            with self.assertRaises(ValueError):
                parse_log(invalid, "typical", 10000)


if __name__ == "__main__":
    unittest.main()
