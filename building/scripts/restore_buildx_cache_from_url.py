#!/usr/bin/env python3
import os
import shutil
import subprocess
import sys
import tempfile
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
    if len(sys.argv) != 2:
        print("usage: restore_buildx_cache_from_url.py <cache_url>", file=sys.stderr)
        return 1

    cache_url = sys.argv[1]
    fd, archive_path = tempfile.mkstemp(prefix="buildx-cache.", dir="/tmp")
    os.close(fd)

    curl_rc = run_cmd(
        [
            "curl",
            "-fsSL",
            "--retry",
            "3",
            "--retry-all-errors",
            "-o",
            archive_path,
            cache_url,
        ]
    )
    if curl_rc != 0:
        try:
            os.remove(archive_path)
        except FileNotFoundError:
            pass
        return 1

    shutil.rmtree(".ci-cache/buildx", ignore_errors=True)
    os.makedirs(".ci-cache", exist_ok=True)

    try:
        if cache_url.endswith(".tar.zst"):
            tar_rc = run_cmd(["tar", "--zstd", "-xf", archive_path, "-C", ".ci-cache"])
        elif cache_url.endswith(".tar.gz"):
            tar_rc = run_cmd(["tar", "-xzf", archive_path, "-C", ".ci-cache"])
        else:
            raise RuntimeError(f"unsupported cache archive format in URL: {cache_url}")
        if tar_rc != 0:
            raise RuntimeError("extract failed")
    except Exception as exc:
        print(str(exc), file=sys.stderr)
        try:
            os.remove(archive_path)
        except FileNotFoundError:
            pass
        shutil.rmtree(".ci-cache/buildx", ignore_errors=True)
        return 1

    try:
        os.remove(archive_path)
    except FileNotFoundError:
        pass

    return 0 if dir_has_files(".ci-cache/buildx") else 1


if __name__ == "__main__":
    sys.exit(main())
