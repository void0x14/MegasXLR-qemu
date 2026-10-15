# SPDX-License-Identifier: GPL-2.0-or-later
#!/usr/bin/env python3
"""Enforce the Android task-time acceptance gate from a benchmark JSON report."""

import argparse
import json
import math
from pathlib import Path
import statistics
import sys
from datetime import datetime


LIMIT_SECONDS = 300.0
ANDROID_API_LEVELS = set(range(14, 38))


def _text(value):
    return isinstance(value, str) and bool(value.strip())


def _positive_number(value):
    return (
        isinstance(value, (int, float))
        and not isinstance(value, bool)
        and math.isfinite(value)
        and value > 0
    )


def _timestamp(value):
    if not _text(value):
        return None
    try:
        parsed = datetime.fromisoformat(value.replace("Z", "+00:00"))
    except ValueError:
        return None
    return parsed if parsed.tzinfo is not None else None


def validate_report(report):
    """Return (errors, scenario_results); each scenario result has a passed flag."""
    errors = []
    results = []
    if not isinstance(report, dict):
        return ["report root must be a JSON object"], results
    if type(report.get("schema_version")) is not int or report["schema_version"] != 1:
        errors.append("schema_version must be 1")

    matrix = report.get("support_matrix")
    if not isinstance(matrix, list) or not matrix:
        return errors + ["support_matrix must contain at least one profile"], results

    expected = {}
    profiles = set()
    for profile in matrix:
        if not isinstance(profile, dict) or not _text(profile.get("profile_id")):
            errors.append("each support_matrix entry needs a profile_id")
            continue
        profile_id = profile["profile_id"]
        if profile_id in profiles:
            errors.append("duplicate profile_id: " + profile_id)
            continue
        profiles.add(profile_id)
        scenario_ids = profile.get("scenario_ids")
        if not isinstance(scenario_ids, list) or not scenario_ids:
            errors.append(profile_id + " must list at least one scenario_id")
            continue
        for scenario_id in scenario_ids:
            if not _text(scenario_id):
                errors.append(profile_id + " contains an empty scenario_id")
            elif scenario_id in expected:
                errors.append("scenario_id appears more than once in support_matrix: " + scenario_id)
            else:
                expected[scenario_id] = profile_id

    raw_scenarios = report.get("scenarios")
    if not isinstance(raw_scenarios, list):
        return errors + ["scenarios must be a list"], results
    scenarios = {}
    for scenario in raw_scenarios:
        if not isinstance(scenario, dict) or not _text(scenario.get("scenario_id")):
            errors.append("each scenario needs a scenario_id")
            continue
        scenario_id = scenario["scenario_id"]
        if scenario_id in scenarios:
            errors.append("duplicate scenario_id in report: " + scenario_id)
        else:
            scenarios[scenario_id] = scenario

    for scenario_id in sorted(set(expected) - set(scenarios)):
        errors.append("missing required scenario: " + scenario_id)
    for scenario_id in sorted(set(scenarios) - set(expected)):
        errors.append("scenario is not in support_matrix: " + scenario_id)

    covered_apis = {
        scenario.get("android_api")
        for scenario_id, scenario in scenarios.items()
        if scenario_id in expected
        and type(scenario.get("android_api")) is int
        and 14 <= scenario["android_api"] <= 37
    }
    for api in sorted(ANDROID_API_LEVELS - covered_apis):
        errors.append(f"support_matrix does not cover Android API {api}")

    required_text = (
        "task_id", "profile_id", "image_variant", "abi", "starting_state_id",
        "task_digest", "oracle_id",
    )
    for scenario_id in sorted(set(expected) & set(scenarios)):
        scenario = scenarios[scenario_id]
        scenario_errors = []
        if scenario.get("profile_id") != expected[scenario_id]:
            scenario_errors.append("profile_id does not match support_matrix")
        for field in required_text:
            if not _text(scenario.get(field)):
                scenario_errors.append(field + " is required")
        api = scenario.get("android_api")
        if type(api) is not int or not 14 <= api <= 37:
            scenario_errors.append("android_api must be an integer from 14 through 37")
        for field in ("host", "vm"):
            if not isinstance(scenario.get(field), dict) or not scenario[field]:
                scenario_errors.append(field + " configuration must be a non-empty object")

        human = scenario.get("human_elapsed_seconds")
        if (not isinstance(human, list) or not human or
                any(not _positive_number(value) for value in human)):
            scenario_errors.append("human_elapsed_seconds must contain positive finite timings")
            human_median = None
        else:
            human_median = statistics.median(human)

        frozen = _timestamp(scenario.get("human_baseline_frozen_at_utc"))
        if frozen is None:
            scenario_errors.append("human_baseline_frozen_at_utc must be an ISO-8601 timestamp with timezone")

        agent_runs = scenario.get("agent_runs")
        if not isinstance(agent_runs, list) or not agent_runs:
            scenario_errors.append("agent_runs must contain at least one run")
            agent_runs = []

        run_results = []
        if human_median is not None:
            allowed = human_median + LIMIT_SECONDS
            for index, run in enumerate(agent_runs, 1):
                run_errors = []
                if not isinstance(run, dict):
                    run_results.append({
                        "run": index, "passed": False,
                        "errors": ["run must be an object"],
                    })
                    continue
                elapsed = run.get("elapsed_seconds")
                if not _positive_number(elapsed):
                    run_errors.append("elapsed_seconds must be positive and finite")
                started = _timestamp(run.get("started_at_utc"))
                finished = _timestamp(run.get("finished_at_utc"))
                if started is None or finished is None:
                    run_errors.append("started_at_utc and finished_at_utc must be ISO-8601 timestamps with timezone")
                elif finished < started:
                    run_errors.append("finished_at_utc precedes started_at_utc")
                elif frozen is not None and started < frozen:
                    run_errors.append("agent run started before the human baseline was frozen")
                if run.get("oracle_result") != "pass":
                    detail = run.get("failure_reason")
                    run_errors.append("independent task oracle did not pass" +
                                      (": " + detail if _text(detail) else ""))
                if _positive_number(elapsed) and elapsed > allowed:
                    run_errors.append(
                        "agent exceeded human median by "
                        f"{elapsed - human_median:.3f}s; maximum is {LIMIT_SECONDS:.0f}s"
                    )
                run_results.append({
                    "run": run.get("run_id", index),
                    "passed": not run_errors,
                    "elapsed_seconds": elapsed,
                    "delta_seconds": elapsed - human_median if _positive_number(elapsed) else None,
                    "errors": run_errors,
                })

        scenario_passed = not scenario_errors and bool(run_results) and all(
            run["passed"] for run in run_results
        )
        results.append({
            "scenario_id": scenario_id,
            "profile_id": expected[scenario_id],
            "human_median_seconds": human_median,
            "allowed_seconds": human_median + LIMIT_SECONDS if human_median is not None else None,
            "passed": scenario_passed,
            "errors": scenario_errors,
            "agent_runs": run_results,
        })

    return errors, results


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("report", type=Path, help="benchmark report JSON")
    args = parser.parse_args(argv)
    try:
        report = json.loads(args.report.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        print(f"FAIL: cannot read benchmark report: {exc}", file=sys.stderr)
        return 2

    errors, results = validate_report(report)
    for error in errors:
        print("FAIL: " + error)
    for scenario in results:
        status = "PASS" if scenario["passed"] else "FAIL"
        median = scenario["human_median_seconds"]
        allowed = scenario["allowed_seconds"]
        if median is not None:
            print(
                f"{status}: {scenario['profile_id']} / {scenario['scenario_id']} "
                f"human_median={median:.3f}s limit={allowed:.3f}s"
            )
        for error in scenario["errors"]:
            print("  " + error)
        for run in scenario["agent_runs"]:
            run_status = "PASS" if run["passed"] else "FAIL"
            delta = run["delta_seconds"]
            delta_text = f"{delta:+.3f}s vs human median" if delta is not None else "timing unavailable"
            print(f"  {run_status}: agent run {run['run']} {delta_text}")
            for error in run["errors"]:
                print("    " + error)
    return 1 if errors or any(not result["passed"] for result in results) else 0


if __name__ == "__main__":
    raise SystemExit(main())
