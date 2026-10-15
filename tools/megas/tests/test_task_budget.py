# SPDX-License-Identifier: GPL-2.0-or-later
#!/usr/bin/env python3
"""Contract tests for the human-relative Android task-time gate."""

import copy
import unittest

from check_task_budget import validate_report


def valid_report(agent_seconds=410.0, oracle_result="pass"):
    report = {
        "schema_version": 1,
        "support_matrix": [],
        "scenarios": [],
    }
    for api in range(14, 38):
        profile_id = f"android-api{api}-x86-aosp"
        scenario_id = f"settings-toggle-api{api}"
        report["support_matrix"].append({
            "profile_id": profile_id,
            "scenario_ids": [scenario_id],
        })
        report["scenarios"].append({
            "scenario_id": scenario_id,
            "profile_id": profile_id,
            "task_id": "wifi-toggle",
            "android_api": api,
            "image_variant": "aosp",
            "abi": "x86",
            "host": {"cpu": "test-host", "memory_mib": 2048},
            "vm": {"machine": "pc", "memory_mib": 1024},
            "starting_state_id": "sha256:starting-state",
            "task_digest": "sha256:task",
            "oracle_id": "wifi-enabled",
            "human_elapsed_seconds": [100, 110, 120, 105, 115],
            "human_baseline_frozen_at_utc": "2026-10-15T11:59:00Z",
            "agent_runs": [{
                "run_id": f"agent-api{api}",
                "started_at_utc": "2026-10-15T12:00:00Z",
                "finished_at_utc": "2026-10-15T12:06:50Z",
                "elapsed_seconds": agent_seconds,
                "oracle_result": oracle_result,
                "failure_reason": "target did not verify" if oracle_result != "pass" else "",
            }],
        })
    return report


class TaskBudgetTests(unittest.TestCase):
    def test_exactly_five_minutes_over_human_median_passes(self):
        errors, results = validate_report(valid_report())
        self.assertEqual(errors, [])
        self.assertTrue(results[0]["passed"])
        self.assertEqual(results[0]["human_median_seconds"], 110)

    def test_one_millisecond_over_five_minute_budget_fails(self):
        errors, results = validate_report(valid_report(agent_seconds=410.001))
        self.assertEqual(errors, [])
        self.assertTrue(any(not result["passed"] for result in results))

    def test_fast_but_unverified_result_fails(self):
        errors, results = validate_report(valid_report(agent_seconds=90, oracle_result="fail"))
        self.assertEqual(errors, [])
        self.assertTrue(any(not result["passed"] for result in results))

    def test_missing_advertised_scenario_fails(self):
        report = valid_report()
        report["support_matrix"][0]["scenario_ids"].append("app-install")
        errors, _ = validate_report(report)
        self.assertIn("missing required scenario: app-install", errors)

    def test_agent_cannot_start_before_baseline_is_frozen(self):
        report = valid_report()
        report["scenarios"][0]["human_baseline_frozen_at_utc"] = "2026-10-15T12:01:00Z"
        errors, results = validate_report(report)
        self.assertEqual(errors, [])
        first_api_result = next(result for result in results if result["scenario_id"] == "settings-toggle-api14")
        self.assertFalse(first_api_result["passed"])
        self.assertIn(
            "agent run started before the human baseline was frozen",
            first_api_result["agent_runs"][0]["errors"],
        )

    def test_android_api_range_is_checked(self):
        report = copy.deepcopy(valid_report())
        report["scenarios"][0]["android_api"] = 38
        errors, results = validate_report(report)
        self.assertTrue(any(
            "android_api must be an integer" in error
            for result in results for error in result["errors"]
        ))
        self.assertTrue(any(not result["passed"] for result in results))

    def test_every_android_api_14_through_37_is_required(self):
        report = valid_report()
        report["support_matrix"] = [
            entry for entry in report["support_matrix"]
            if entry["profile_id"] != "android-api37-x86-aosp"
        ]
        report["scenarios"] = [
            scenario for scenario in report["scenarios"]
            if scenario["scenario_id"] != "settings-toggle-api37"
        ]
        errors, _ = validate_report(report)
        self.assertIn("support_matrix does not cover Android API 37", errors)


if __name__ == "__main__":
    unittest.main()
