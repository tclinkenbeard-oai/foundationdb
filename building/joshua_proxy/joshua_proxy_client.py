#!/usr/bin/env python3

import argparse
import json
import re
import subprocess
import sys
import time
from pathlib import Path
from typing import Optional


PROTO_IMPORT_PATH = Path(__file__).resolve().parent
PROTO_FILENAME = "joshua_proxy.proto"
SERVICE_FQN = "openai.fdbci.joshua_proxy.v1.JoshuaProxy"
SMOKE_JOB_ID = "build-preflight-smoke-test"
STATUS_POLL_INTERVAL_SECONDS = 30
STATUS_RPC_TIMEOUT_SECONDS = 15

REACHABILITY_FAILURE_RE = re.compile(
    r"Failed to dial target host|connection refused|context deadline exceeded|"
    r"no such host|network is unreachable|authentication handshake failed|TLS handshake timeout",
    re.IGNORECASE,
)
UNIMPLEMENTED_RE = re.compile(r"Code:\s*Unimplemented", re.IGNORECASE)


def _positive_int(value: str) -> int:
    try:
        parsed = int(value)
    except ValueError as exc:
        raise argparse.ArgumentTypeError(f"expected an integer, got: {value}") from exc
    if parsed <= 0:
        raise argparse.ArgumentTypeError(f"expected a positive integer, got: {value}")
    return parsed


def _normalize_target(addr: str) -> str:
    target = addr.removeprefix("grpc://")
    target = target.removeprefix("grpcs://")
    return target


def _grpcurl_transport_args(addr: str, insecure: bool) -> list[str]:
    target = _normalize_target(addr)
    args: list[str] = []
    if addr.startswith("grpc://") or target.endswith(":50051"):
        args.append("-plaintext")
    if insecure:
        args.append("-insecure")
    return args


def _run_grpcurl(
    *,
    addr: str,
    method: str,
    payload: dict[str, object],
    insecure: bool,
    timeout_seconds: Optional[int] = None,
) -> subprocess.CompletedProcess[str]:
    cmd = ["grpcurl"]
    cmd.extend(_grpcurl_transport_args(addr, insecure))
    if timeout_seconds is not None:
        cmd.extend(["-max-time", str(timeout_seconds)])
    cmd.extend(
        [
            "-import-path",
            str(PROTO_IMPORT_PATH),
            "-proto",
            PROTO_FILENAME,
            "-d",
            json.dumps(payload, separators=(",", ":")),
            _normalize_target(addr),
            f"{SERVICE_FQN}/{method}",
        ]
    )
    return subprocess.run(cmd, check=False, capture_output=True, text=True)


def _combined_output(result: subprocess.CompletedProcess[str]) -> str:
    return f"{result.stdout}{result.stderr}".strip()


def _run_smoke(args: argparse.Namespace) -> int:
    result = _run_grpcurl(
        addr=args.addr,
        method="GetJobStatus",
        payload={"job_id": SMOKE_JOB_ID},
        insecure=args.insecure,
        timeout_seconds=args.timeout_seconds,
    )
    output = _combined_output(result)

    if result.returncode == 0:
        print("Joshua Proxy smoke test succeeded")
        return 0

    if REACHABILITY_FAILURE_RE.search(output):
        print(
            f"Joshua Proxy smoke test failed: service is not reachable at {args.addr}",
            file=sys.stderr,
        )
        if output:
            print(output, file=sys.stderr)
        return 1

    if UNIMPLEMENTED_RE.search(output):
        print(
            f"Joshua Proxy smoke test failed: GetJobStatus is not implemented on {args.addr}",
            file=sys.stderr,
        )
        if output:
            print(output, file=sys.stderr)
        return 1

    print("Joshua Proxy responded to smoke test request (non-OK RPC status is acceptable for preflight)")
    if output:
        print(output)
    return 0


def _extract_job_id(reply: str) -> str:
    try:
        parsed = json.loads(reply)
    except json.JSONDecodeError as exc:
        raise ValueError(f"Failed to parse SubmitJob response as JSON: {reply}") from exc

    if not isinstance(parsed, dict):
        raise ValueError(f"SubmitJob response must be a JSON object: {reply}")

    job_id = parsed.get("job_id") or parsed.get("jobId")
    if not isinstance(job_id, str) or not job_id:
        raise ValueError(f"SubmitJob response did not include job_id/jobId: {reply}")

    return job_id


def _extract_status_reply(reply: str) -> tuple[str, dict[str, object]]:
    try:
        parsed = json.loads(reply)
    except json.JSONDecodeError as exc:
        raise ValueError(f"Failed to parse GetJobStatus response as JSON: {reply}") from exc

    if not isinstance(parsed, dict):
        raise ValueError(f"GetJobStatus response must be a JSON object: {reply}")

    for status in ("running", "success", "failed", "cancelled"):
        status_value = parsed.get(status)
        if isinstance(status_value, dict):
            return status, status_value

    raise ValueError(f"GetJobStatus response did not include a known status in oneof: {reply}")


def _status_summary(status: str, status_payload: dict[str, object], total_runs: Optional[int] = None) -> str:
    def _counter(snake_case_name: str, camel_case_name: str) -> object:
        value = status_payload.get(snake_case_name)
        if value is None:
            value = status_payload.get(camel_case_name)
        return 0 if value is None else value

    def _to_int(value: object) -> Optional[int]:
        if isinstance(value, int):
            return value
        if isinstance(value, str):
            try:
                return int(value)
            except ValueError:
                return None
        return None

    def _derived_failed_count(tests_passed_value: object, tests_remaining_value: object) -> Optional[int]:
        if total_runs is None:
            return None
        passed_int = _to_int(tests_passed_value)
        remaining_int = _to_int(tests_remaining_value)
        if passed_int is None or remaining_int is None:
            return None
        return max(0, total_runs - passed_int - remaining_int)

    tests_passed = _counter("tests_passed", "testsPassed")

    if status == "running":
        tests_remaining = _counter("tests_remaining", "testsRemaining")
        tests_failed = _derived_failed_count(tests_passed, tests_remaining)
        failed_segment = f", tests_failed={tests_failed}" if tests_failed is not None else ""
        return f"running (tests_passed={tests_passed}{failed_segment}, tests_remaining={tests_remaining})"

    if status == "success":
        tests_failed = _derived_failed_count(tests_passed, 0)
        failed_segment = f", tests_failed={tests_failed}" if tests_failed is not None else ""
        return f"success (tests_passed={tests_passed}{failed_segment})"

    if status == "cancelled":
        tests_remaining = _counter("tests_remaining", "testsRemaining")
        tests_failed = _derived_failed_count(tests_passed, tests_remaining)
        failed_segment = f", tests_failed={tests_failed}" if tests_failed is not None else ""
        return f"cancelled (tests_passed={tests_passed}{failed_segment}, tests_remaining={tests_remaining})"

    failures = status_payload.get("failures")
    failure_count = len(failures) if isinstance(failures, list) else None
    if failure_count is None:
        failure_count = _derived_failed_count(tests_passed, 0)
    if failure_count is None:
        failure_count = 0
    return f"failed (tests_passed={tests_passed}, tests_failed={failure_count}, failures={failure_count})"


def _poll_job_to_completion(*, addr: str, job_id: str, insecure: bool, total_runs: Optional[int] = None) -> int:
    while True:
        result = _run_grpcurl(
            addr=addr,
            method="GetJobStatus",
            payload={"job_id": job_id},
            insecure=insecure,
            timeout_seconds=STATUS_RPC_TIMEOUT_SECONDS,
        )

        if result.returncode != 0:
            output = _combined_output(result)
            print(f"GetJobStatus failed for job_id={job_id}", file=sys.stderr)
            if output:
                print(output, file=sys.stderr)
            return result.returncode

        raw_reply = result.stdout.strip()
        if not raw_reply:
            raw_reply = _combined_output(result)

        try:
            status, status_payload = _extract_status_reply(raw_reply)
        except ValueError as exc:
            print(exc, file=sys.stderr)
            return 1

        print(
            f"Joshua job_id={job_id} status: {_status_summary(status, status_payload, total_runs=total_runs)}",
            file=sys.stderr,
        )

        if status == "running":
            time.sleep(STATUS_POLL_INTERVAL_SECONDS)
            continue

        if status == "success":
            return 0

        return 1


def _run_submit(args: argparse.Namespace) -> int:
    result = _run_grpcurl(
        addr=args.addr,
        method="SubmitJob",
        payload={"correctness_package_url": args.correctness_package_url, "runs": args.runs},
        insecure=args.insecure,
    )

    if result.returncode != 0:
        output = _combined_output(result)
        if output:
            print(output, file=sys.stderr)
        return result.returncode

    raw_reply = result.stdout.strip()
    if not raw_reply:
        raw_reply = _combined_output(result)

    try:
        job_id = _extract_job_id(raw_reply)
    except ValueError as exc:
        print(exc, file=sys.stderr)
        return 1

    # Keep stdout reserved for the job ID; callers capture this value in CI scripts.
    print(job_id)
    return _poll_job_to_completion(addr=args.addr, job_id=job_id, insecure=args.insecure, total_runs=args.runs)


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Joshua Proxy client for build entrypoint integration")
    subparsers = parser.add_subparsers(dest="command", required=True)

    smoke_parser = subparsers.add_parser("smoke", help="Run Joshua Proxy reachability smoke test")
    smoke_parser.add_argument("--addr", required=True, help="Joshua Proxy address (host:port or grpc[s]://host:port)")
    smoke_parser.add_argument(
        "--timeout-seconds",
        type=_positive_int,
        required=True,
        help="gRPC smoke test timeout in seconds",
    )
    smoke_parser.add_argument("--insecure", action="store_true", help="Pass -insecure to grpcurl")
    smoke_parser.set_defaults(handler=_run_smoke)

    submit_parser = subparsers.add_parser("submit", help="Submit a Joshua correctness job")
    submit_parser.add_argument("--addr", required=True, help="Joshua Proxy address (host:port or grpc[s]://host:port)")
    submit_parser.add_argument("--correctness-package-url", required=True, help="Blob URL to correctness tarball")
    submit_parser.add_argument("--runs", type=_positive_int, required=True, help="Number of Joshua runs to execute")
    submit_parser.add_argument("--insecure", action="store_true", help="Pass -insecure to grpcurl")
    submit_parser.set_defaults(handler=_run_submit)

    return parser


def main() -> int:
    parser = _build_parser()
    args = parser.parse_args()
    return args.handler(args)


if __name__ == "__main__":
    raise SystemExit(main())
