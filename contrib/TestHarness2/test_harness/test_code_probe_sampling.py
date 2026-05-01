import unittest

from test_harness.summarize import should_report_code_probes


class CodeProbeSamplingTest(unittest.TestCase):
    def test_boundary_rates(self):
        self.assertTrue(should_report_code_probes("test-run", 1.0))
        self.assertFalse(should_report_code_probes("test-run", 0.0))

    def test_invalid_rates(self):
        with self.assertRaises(ValueError):
            should_report_code_probes("test-run", -0.1)
        with self.assertRaises(ValueError):
            should_report_code_probes("test-run", 1.1)

    def test_stable_subset(self):
        sampled = [
            should_report_code_probes("test-run-{}".format(i), 0.5)
            for i in range(100)
        ]
        self.assertEqual(
            should_report_code_probes("test-run-42", 0.5),
            should_report_code_probes("test-run-42", 0.5),
        )
        self.assertGreater(sum(sampled), 0)
        self.assertLess(sum(sampled), len(sampled))


if __name__ == "__main__":
    unittest.main()
