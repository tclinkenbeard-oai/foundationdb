#!/usr/bin/env python3
import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path


def dir_has_files(path: str) -> bool:
    p = Path(path)
    if not p.is_dir():
        return False
    try:
        next(p.iterdir())
        return True
    except StopIteration:
        return False


def run_cmd(cmd, **kwargs) -> int:
    try:
        return subprocess.run(cmd, **kwargs).returncode
    except FileNotFoundError:
        print(f"{cmd[0]} not found", file=sys.stderr)
        return 127


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cache-file", required=True)
    parser.add_argument("--cache-dest", required=True)
    args = parser.parse_args()

    if not dir_has_files(".ci-cache/buildx-new"):
        print("No buildx cache output found under .ci-cache/buildx-new; skipping upload")
        return 0

    shutil.rmtree(".ci-cache/buildx", ignore_errors=True)
    os.makedirs(".ci-cache", exist_ok=True)
    try:
        shutil.move(".ci-cache/buildx-new", ".ci-cache/buildx")
    except Exception as exc:
        print(str(exc), file=sys.stderr)
        return 1

    cache_file_path = Path(".ci-cache") / args.cache_file
    if cache_file_path.exists():
        cache_file_path.unlink()

    try:
        if args.cache_file.endswith(".tar.zst"):
            tar_rc = run_cmd(
                [
                    "tar",
                    "--zstd",
                    "-cf",
                    args.cache_file,
                    "buildx",
                ],
                cwd=".ci-cache",
            )
        elif args.cache_file.endswith(".tar.gz"):
            tar_rc = run_cmd(
                [
                    "tar",
                    "-czf",
                    args.cache_file,
                    "buildx",
                ],
                cwd=".ci-cache",
            )
        else:
            print(f"Unsupported cache archive format: {args.cache_file}", file=sys.stderr)
            return 1
        if tar_rc != 0:
            raise RuntimeError("tar failed")
    except Exception:
        if args.cache_file.endswith(".tar.zst"):
            print("Failed to package buildx cache with zstd; skipping upload", file=sys.stderr)
        else:
            print("Failed to package buildx cache with gzip; skipping upload", file=sys.stderr)
        return 0

    print("--- Build cache upload")
    print("~~~ Upload destination")
    print(f"Uploading buildx cache to {args.cache_dest}")

    env = os.environ.copy()
    env["BUILDKITE_ARTIFACT_UPLOAD_DESTINATION"] = args.cache_dest
    if run_cmd(
        ["buildkite-agent", "artifact", "upload", args.cache_file],
        cwd=".ci-cache",
        env=env,
    ) != 0:
        print("Failed to upload buildx cache; continuing without failing the build", file=sys.stderr)

    return 0


if __name__ == "__main__":
    sys.exit(main())
