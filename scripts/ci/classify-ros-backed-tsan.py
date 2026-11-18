#!/usr/bin/env python3
"""Classify unsuppressed ROS TSan reports using the OD-47 frame boundary."""

from __future__ import annotations

import re
import sys
from pathlib import Path

WARNING = re.compile(r"^WARNING: ThreadSanitizer:.*$", re.MULTILINE)
FATAL = re.compile(r"(?:FATAL: ThreadSanitizer|ThreadSanitizer: CHECK failed)")
FRAME = re.compile(r"^\s*#\d+\s+(.*)$")
LOCK_ORDER = re.compile(r"ThreadSanitizer: lock-order-inversion")
ACCESS = re.compile(
    r"^(?:previous\s+)?(?:atomic\s+)?(?:read|write)\s+(?:of|at)\b", re.I
)
LOCK_ACCESS = re.compile(r"^mutex\s+\S+\s+acquired\s+here\b", re.I)
PROJECT_PATH = re.compile(
    r"(?:^|[\s/(])(?:[^\s:()]*/)?manyforge_[a-zA-Z0-9_]+/"
    r"(?:src|include|test|tests|tools|python)/[^\s:()]+"
)
PROJECT_NAMESPACE = re.compile(r"(?<![A-Za-z0-9_:])manyforge::|\b_ZN9manyforge")
RUNTIME_SYMBOL = re.compile(
    r"^(?:(?:void|bool|int|unsigned|long|char|auto)\s+)?"
    r"(?:std::(?:allocator|allocator_traits|__new_allocator|__alloc_traits|"
    r"_Sp_|__shared_|shared_ptr|weak_ptr|make_shared|allocate_shared|"
    r"_Construct|_Destroy|construct_at|destroy_at|vector|deque|list|map|"
    r"set|unordered_|_Rb_tree|_Hashtable|_Vector_base|_Deque_base|"
    r"basic_string|__cxx11::basic_string)|__gnu_cxx::|"
    r"__interceptor_|__tsan_|__sanitizer::|"
    r"operator\s+(?:new|delete)|malloc\b|calloc\b|realloc\b|free\b|"
    r"memcpy\b|memmove\b|memset\b|pthread_\w+\b|__libc_\w+\b)"
)


def without_template_arguments(text: str) -> str:
    """Keep namespaces in the called symbol, not template parameter types."""
    result: list[str] = []
    depth = 0
    for character in text:
        if character == "<":
            depth += 1
        elif character == ">" and depth:
            depth -= 1
        elif not depth:
            result.append(character)
    return "".join(result)


def is_project_frame(frame: str) -> bool:
    if PROJECT_PATH.search(frame):
        return True
    symbol = without_template_arguments(frame).split("(", 1)[0]
    return bool(PROJECT_NAMESPACE.search(symbol))


def is_runtime_frame(frame: str) -> bool:
    # Symbol matching precedes the project test: std:: containers/control
    # blocks may contain ManyForge template parameters but allocate for SDKs.
    return bool(RUNTIME_SYMBOL.search(frame)) or "/libsanitizer/" in frame


def relevant_stack_is_project(frames: list[str]) -> bool:
    for frame in frames:
        if is_runtime_frame(frame):
            continue
        return is_project_frame(frame)
    return False


def report_is_project(report: str) -> bool:
    section: str | None = None
    frames: list[str] = []

    def project_stack() -> bool:
        return bool(section and relevant_stack_is_project(frames))

    for line in report.splitlines():
        frame = FRAME.match(line)
        if frame:
            if section:
                frames.append(frame.group(1))
            continue
        stripped = line.strip()
        if not stripped:
            if project_stack():
                return True
            section, frames = None, []
            continue
        if ACCESS.match(stripped) or LOCK_ACCESS.match(stripped):
            if project_stack():
                return True
            section, frames = "access", []
        elif stripped.lower().startswith("location is "):
            if project_stack():
                return True
            section, frames = "location", []
            if "global" in stripped.lower() and is_project_frame(stripped):
                return True
        elif stripped.startswith(("Thread ", "Mutex ", "SUMMARY:", "WARNING:")):
            if project_stack():
                return True
            section, frames = None, []
    return project_stack()


# Lock-order inversions: the acquiring frames are pthread and libstdc++ lock
# wrappers. Skip those wrappers unless they were compiled into an SDK library
# (then the acquisition is the SDK's); the first other frame decides.
LOCK_ACQUIRED = re.compile(r"^mutex\s+\S+\s+(?:previously\s+)?acquired\b", re.I)
LOCK_WRAPPER = re.compile(
    r"^(?:void\s+)?(?:__gthread_\w+|std::(?:unique_lock|lock_guard|scoped_lock|"
    r"shared_lock|(?:recursive_|timed_|recursive_timed_|shared_|shared_timed_)?mutex)\b)"
)
MODULE = re.compile(r"\(([^()\s]+)\+0x[0-9a-fA-F]+\)")
PROJECT_MODULE = re.compile(r"^(?:lib)?manyforge|^test_")
NEUTRAL_MODULE = re.compile(r"^(?:libstdc\+\+|libtsan|libc|libpthread|ld-linux)")


def frame_module(frame: str) -> str | None:
    found = MODULE.search(frame)
    return found.group(1) if found else None


def acquisition_is_project(frames: list[str]) -> bool:
    for frame in frames:
        if is_runtime_frame(frame) and not LOCK_WRAPPER.search(frame):
            continue
        module = frame_module(frame)
        if LOCK_WRAPPER.search(frame):
            if module and not PROJECT_MODULE.search(module) and not NEUTRAL_MODULE.search(module):
                return False  # A lock wrapper inside an SDK library.
            continue
        return is_project_frame(frame) or bool(module and PROJECT_MODULE.search(module))
    return False


def lock_order_is_project(report: str) -> bool:
    stacks: list[list[str]] = []
    current: list[str] | None = None
    for line in report.splitlines():
        frame = FRAME.match(line)
        if frame:
            if current is not None:
                current.append(frame.group(1))
            continue
        stripped = line.strip()
        if LOCK_ACQUIRED.match(stripped):
            current = []
            stacks.append(current)
        elif stripped:
            current = None
    return any(acquisition_is_project(stack) for stack in stacks)


def report_fails(report: str) -> bool:
    # A lock-order inversion fails only when at least one of its mutex
    # acquisitions is ManyForge's own.
    if LOCK_ORDER.search(report.splitlines()[0]):
        return lock_order_is_project(report)
    return report_is_project(report)


def classify(path: Path) -> bool:
    text = path.read_text(encoding="utf-8", errors="replace")
    failed = False
    if FATAL.search(text):
        print(f"::error::ThreadSanitizer runtime failure: {path}")
        failed = True
    matches = list(WARNING.finditer(text))
    for index, match in enumerate(matches):
        end = matches[index + 1].start() if index + 1 < len(matches) else len(text)
        report = text[match.start():end]
        if report_fails(report):
            print(f"::error::ManyForge ThreadSanitizer report {index + 1}: {path}")
            failed = True
        else:
            print(f"::warning::SDK race, to classify (report {index + 1}): {path}")
    return failed


def main() -> int:
    if len(sys.argv) < 2:
        print("usage: classify-ros-backed-tsan.py LOG [LOG ...]", file=sys.stderr)
        return 2
    failed = False
    for name in sys.argv[1:]:
        try:
            failed = classify(Path(name)) or failed
        except OSError as error:
            print(f"::error::Cannot read TSan log {name}: {error}", file=sys.stderr)
            failed = True
    return int(failed)


if __name__ == "__main__":
    raise SystemExit(main())
