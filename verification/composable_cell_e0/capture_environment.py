from __future__ import annotations

import argparse
import importlib.metadata
import json
import os
import platform
import subprocess
import sys


def first_line(*command: str) -> str:
    return subprocess.check_output(command, text=True).splitlines()[0]


def package_version(name: str) -> str:
    return importlib.metadata.version(name)


def pkg_config_version(name: str) -> str:
    return subprocess.check_output(
        ["pkg-config", "--modversion", name], text=True
    ).strip()


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--image-reference", required=True)
    parser.add_argument("--image-id", required=True)
    args = parser.parse_args()

    print(
        json.dumps(
            {
                "architecture": platform.machine(),
                "cmake": first_line("cmake", "--version"),
                "compiler": first_line(
                    os.environ.get("CXX", "c++"), "--version"
                ),
                "image_id": args.image_id,
                "image_reference": args.image_reference,
                "libutf8proc": pkg_config_version("libutf8proc"),
                "nanobind": package_version("nanobind"),
                "ninja": first_line("ninja", "--version"),
                "platform": platform.platform(),
                "pybind11": package_version("pybind11"),
                "python": sys.version,
                "yaml": pkg_config_version("yaml-0.1"),
            },
            indent=2,
            sort_keys=True,
        )
    )


if __name__ == "__main__":
    main()
