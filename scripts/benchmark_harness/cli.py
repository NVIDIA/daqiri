# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

"""Command-line interface for planning, running, and resuming benchmark suites."""

from __future__ import annotations

import argparse
import datetime as dt
import json
import sys
import uuid
from pathlib import Path

from .executor import ExecutorError, build_executors
from .lifecycle import Harness, HarnessFailure
from .manifest import ManifestError, load_inputs, load_resolved, resolve_plan
from .results import initialize_run, render_summary, verify_run_identity


def _default_run_id(suite_name: str) -> str:
    timestamp = dt.datetime.now(dt.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    return f"{suite_name}-{timestamp}-{uuid.uuid4().hex[:8]}"


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Plan and run reproducible DAQIRI benchmark suites"
    )
    subparsers = parser.add_subparsers(dest="command", required=True)
    for command in ("plan", "run"):
        subparser = subparsers.add_parser(command)
        subparser.add_argument("--suite", required=True, type=Path)
        subparser.add_argument("--site", required=True, type=Path)
        subparser.add_argument("--run-id")
        if command == "run":
            subparser.add_argument("--output", required=True, type=Path)
            subparser.add_argument(
                "--dry-run",
                action="store_true",
                help="print the complete plan without creating artifacts or contacting hosts",
            )
    resume = subparsers.add_parser("resume")
    resume.add_argument("run_dir", type=Path)
    report = subparsers.add_parser("report")
    report.add_argument("run_dir", type=Path)
    return parser


def _worker_path() -> Path:
    return Path(__file__).with_name("remote_worker.py")


def _repository() -> Path:
    return Path(__file__).resolve().parents[2]


def _print_plan(plan: dict) -> None:
    print(json.dumps(plan, indent=2, sort_keys=True))


def main(argv: list[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    try:
        if args.command in {"plan", "run"}:
            suite, site = load_inputs(args.suite.resolve(), args.site.resolve())
            run_id = args.run_id or _default_run_id(suite["name"])
            plan = resolve_plan(suite, site, run_id)
            if args.command == "plan" or args.dry_run:
                _print_plan(plan)
                return 0
            run_dir = args.output.resolve()
            initialize_run(run_dir, plan)
            harness = Harness(
                plan,
                run_dir,
                build_executors(plan, _worker_path()),
                _repository(),
            )
            try:
                harness.initialize()
            except KeyboardInterrupt:
                harness.record_initialization_failure(
                    "benchmark initialization interrupted"
                )
                raise
            except (ExecutorError, HarnessFailure, OSError, ValueError) as exc:
                harness.record_initialization_failure(
                    f"initialization failed: {type(exc).__name__}: {exc}"
                )
                print(f"benchmark initialization failed: {exc}", file=sys.stderr)
                return 1
            return harness.run_all()

        plan = load_resolved(args.run_dir / "resolved-manifest.yaml")
        verify_run_identity(args.run_dir, plan)
        if args.command == "report":
            render_summary(args.run_dir, plan)
            return 0
        harness = Harness(
            plan,
            args.run_dir.resolve(),
            build_executors(plan, _worker_path()),
            _repository(),
        )
        harness.recover_orphans()
        try:
            harness.initialize_resume()
        except KeyboardInterrupt:
            harness.record_initialization_failure(
                "benchmark resume initialization interrupted"
            )
            raise
        except (ExecutorError, HarnessFailure, OSError, ValueError) as exc:
            harness.record_initialization_failure(
                f"resume initialization failed: {type(exc).__name__}: {exc}"
            )
            print(f"benchmark resume failed: {exc}", file=sys.stderr)
            return 1
        return harness.run_all()
    except (ManifestError, OSError, ValueError) as exc:
        print(f"benchmark harness error: {exc}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print("benchmark harness interrupted after bounded cleanup", file=sys.stderr)
        return 130


if __name__ == "__main__":
    raise SystemExit(main())
