#!/usr/bin/env python3

import argparse
import sys
import time
from typing import Optional

import grpc

import joshua_proxy_pb2

SERVICE_FQN = "openai.fdbci.joshua_proxy.v1.JoshuaProxy"
SUBMIT_JOB_PATH = f"/{SERVICE_FQN}/SubmitJob"
GET_JOB_STATUS_PATH = f"/{SERVICE_FQN}/GetJobStatus"
SMOKE_JOB_ID = "build-preflight-smoke-test"
STATUS_POLL_INTERVAL_SECONDS = 30
STATUS_RPC_TIMEOUT_SECONDS = 15

REACHABILITY_FAILURE_CODES = {
    grpc.StatusCode.UNAVAILABLE,
    grpc.StatusCode.DEADLINE_EXCEEDED,
}


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


def _use_plaintext(addr: str) -> bool:
    target = _normalize_target(addr)
    return addr.startswith("grpc://") or target.endswith(":50051")


def _secure_channel_options(addr: str, insecure: bool) -> list[tuple[str, str]]:
    if not insecure:
        return []
    # grpcurl -insecure disables hostname verification; mirror this behavior best-effort.
    host = _normalize_target(addr).rsplit(":", 1)[0]
    return [
        ("grpc.ssl_target_name_override", host),
        ("grpc.default_authority", host),
    ]


class _JoshuaProxyClient:
    def __init__(self, *, addr: str, insecure: bool) -> None:
        target = _normalize_target(addr)
        if _use_plaintext(addr):
            self._channel = grpc.insecure_channel(target)
        else:
            self._channel = grpc.secure_channel(
                target,
                grpc.ssl_channel_credentials(),
                options=_secure_channel_options(addr, insecure),
            )
        self._submit_job_rpc = self._channel.unary_unary(
            SUBMIT_JOB_PATH,
            request_serializer=joshua_proxy_pb2.SubmitJobRequest.SerializeToString,
            response_deserializer=joshua_proxy_pb2.SubmitJobReply.FromString,
        )
        self._get_job_status_rpc = self._channel.unary_unary(
            GET_JOB_STATUS_PATH,
            request_serializer=joshua_proxy_pb2.GetJobStatusRequest.SerializeToString,
            response_deserializer=joshua_proxy_pb2.GetJobStatusReply.FromString,
        )

    def close(self) -> None:
        self._channel.close()

    def submit_job(
        self,
        *,
        correctness_package_url: str,
        runs: int,
        timeout_seconds: Optional[int] = None,
    ) -> joshua_proxy_pb2.SubmitJobReply:
        request = joshua_proxy_pb2.SubmitJobRequest(
            correctness_package_url=correctness_package_url,
            runs=runs,
        )
        return self._submit_job_rpc(request, timeout=timeout_seconds)

    def get_job_status(
        self,
        *,
        job_id: str,
        timeout_seconds: Optional[int] = None,
    ) -> joshua_proxy_pb2.GetJobStatusReply:
        request = joshua_proxy_pb2.GetJobStatusRequest(job_id=job_id)
        return self._get_job_status_rpc(request, timeout=timeout_seconds)


def _rpc_error_text(error: grpc.RpcError) -> str:
    status = error.code()
    details = error.details()
    if details:
        return f"{status.name}: {details}"
    return status.name


def _run_smoke(args: argparse.Namespace) -> int:
    client = _JoshuaProxyClient(addr=args.addr, insecure=args.insecure)
    try:
        client.get_job_status(job_id=SMOKE_JOB_ID, timeout_seconds=args.timeout_seconds)
    except grpc.RpcError as exc:
        output = _rpc_error_text(exc)
        if exc.code() in REACHABILITY_FAILURE_CODES:
            print(
                f"Joshua Proxy smoke test failed: service is not reachable at {args.addr}",
                file=sys.stderr,
            )
            if output:
                print(output, file=sys.stderr)
            return 1

        if exc.code() == grpc.StatusCode.UNIMPLEMENTED:
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
    finally:
        client.close()

    print("Joshua Proxy smoke test succeeded")
    return 0


def _status_summary(status: str, status_payload: object, total_runs: Optional[int] = None) -> str:
    def _derived_failed_count(tests_passed_value: int, tests_remaining_value: int) -> Optional[int]:
        if total_runs is None:
            return None
        return max(0, total_runs - tests_passed_value - tests_remaining_value)

    tests_passed = int(getattr(status_payload, "tests_passed", 0))
    if status == "running":
        tests_remaining = int(getattr(status_payload, "tests_remaining", 0))
        tests_failed = _derived_failed_count(tests_passed, tests_remaining)
        failed_segment = f", tests_failed={tests_failed}" if tests_failed is not None else ""
        return f"running (tests_passed={tests_passed}{failed_segment}, tests_remaining={tests_remaining})"

    if status == "success":
        tests_failed = _derived_failed_count(tests_passed, 0)
        failed_segment = f", tests_failed={tests_failed}" if tests_failed is not None else ""
        return f"success (tests_passed={tests_passed}{failed_segment})"

    if status == "cancelled":
        tests_remaining = int(getattr(status_payload, "tests_remaining", 0))
        tests_failed = _derived_failed_count(tests_passed, tests_remaining)
        failed_segment = f", tests_failed={tests_failed}" if tests_failed is not None else ""
        return f"cancelled (tests_passed={tests_passed}{failed_segment}, tests_remaining={tests_remaining})"

    failures = getattr(status_payload, "failures", [])
    failure_count = len(failures)
    if failure_count == 0:
        derived = _derived_failed_count(tests_passed, 0)
        if derived is not None:
            failure_count = derived
    return f"failed (tests_passed={tests_passed}, tests_failed={failure_count}, failures={failure_count})"


def _poll_job_to_completion(*, client: _JoshuaProxyClient, job_id: str, total_runs: Optional[int] = None) -> int:
    while True:
        try:
            reply = client.get_job_status(job_id=job_id, timeout_seconds=STATUS_RPC_TIMEOUT_SECONDS)
        except grpc.RpcError as exc:
            print(f"GetJobStatus failed for job_id={job_id}", file=sys.stderr)
            output = _rpc_error_text(exc)
            if output:
                print(output, file=sys.stderr)
            return 1
        status = reply.WhichOneof("status")
        if status is None:
            print("GetJobStatus response did not include a known status in oneof", file=sys.stderr)
            return 1
        status_payload = getattr(reply, status)

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
    client = _JoshuaProxyClient(addr=args.addr, insecure=args.insecure)
    try:
        try:
            reply = client.submit_job(
                correctness_package_url=args.correctness_package_url,
                runs=args.runs,
            )
        except grpc.RpcError as exc:
            output = _rpc_error_text(exc)
            if output:
                print(output, file=sys.stderr)
            return 1

        job_id = reply.job_id
        if not job_id:
            print("SubmitJob response did not include job_id", file=sys.stderr)
            return 1

        # Keep stdout reserved for the job ID; callers capture this value in CI scripts.
        print(job_id)
        return _poll_job_to_completion(client=client, job_id=job_id, total_runs=args.runs)
    finally:
        client.close()


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
    smoke_parser.add_argument("--insecure", action="store_true", help="Disable TLS hostname verification")
    smoke_parser.set_defaults(handler=_run_smoke)

    submit_parser = subparsers.add_parser("submit", help="Submit a Joshua correctness job")
    submit_parser.add_argument("--addr", required=True, help="Joshua Proxy address (host:port or grpc[s]://host:port)")
    submit_parser.add_argument("--correctness-package-url", required=True, help="Blob URL to correctness tarball")
    submit_parser.add_argument("--runs", type=_positive_int, required=True, help="Number of Joshua runs to execute")
    submit_parser.add_argument("--insecure", action="store_true", help="Disable TLS hostname verification")
    submit_parser.set_defaults(handler=_run_submit)

    return parser


def main() -> int:
    parser = _build_parser()
    args = parser.parse_args()
    return args.handler(args)


if __name__ == "__main__":
    raise SystemExit(main())
