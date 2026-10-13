#!/usr/bin/env python3
"""Graph orchestration only. All Submit/Replace calls use the real C++ adapter."""
from __future__ import annotations
import argparse
import json
import os
from pathlib import Path
import shlex
import signal
import subprocess
import threading
import time
import traceback
import rclpy
from jtc_characterization import Probe


def stop_process(p: subprocess.Popen | None) -> None:
    if p is None or p.poll() is not None:
        return
    os.killpg(p.pid, signal.SIGTERM)
    try:
        p.wait(timeout=5)
    except subprocess.TimeoutExpired:
        os.killpg(p.pid, signal.SIGKILL)
        p.wait(timeout=5)


class CompileLoad:
    """Two explicit compiler workers; finite, reaped and no ccache wrapper."""
    def __init__(self, command: list[str], directory: Path):
        self.command, self.directory = command, directory
        self.stop = threading.Event()
        self.ready = [threading.Event(), threading.Event()]
        self.records: list[dict] = []
        self.lock = threading.Lock()
        self.threads: list[threading.Thread] = []

    def worker(self, index: int) -> None:
        deadline = time.monotonic() + 90  # Fixture-only workload limit.
        for iteration in range(30):
            if self.stop.is_set() or time.monotonic() >= deadline:
                break
            command = self.command.copy()
            command[command.index('-o') + 1] = str(self.directory / f'load-{index}.o')
            begin = time.monotonic_ns()
            p = None
            try:
                with (self.directory / f'load-{index}-{iteration}.log').open('w') as log:
                    p = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT,
                                         env={**os.environ, 'CCACHE_DISABLE': '1'},
                                         start_new_session=True)
                    self.ready[index].set()
                    while p.poll() is None:
                        if self.stop.wait(0.02) or time.monotonic() >= deadline:
                            stop_process(p)
                            break
                    code = p.wait(timeout=5)
                record = dict(worker=index, iteration=iteration, begin_ns=begin,
                              end_ns=time.monotonic_ns(), returncode=code, command=command)
                with self.lock:
                    self.records.append(record)
                if code != 0 and not self.stop.is_set():
                    raise RuntimeError(f'load compiler failed {index}/{iteration}: {code}')
            except BaseException as error:
                with self.lock:
                    self.records.append(dict(worker=index, error=repr(error),
                                             begin_ns=begin, end_ns=time.monotonic_ns()))
                self.stop.set()
                break
            finally:
                stop_process(p)

    def start(self) -> None:
        self.threads = [threading.Thread(target=self.worker, args=(i,)) for i in range(2)]
        for thread in self.threads:
            thread.start()
        if not all(event.wait(5) for event in self.ready):
            raise RuntimeError('both load compilers did not start')

    def close(self) -> None:
        self.stop.set()
        for thread in self.threads:
            thread.join(timeout=12)
            if thread.is_alive():
                raise RuntimeError('load worker failed finite reap')
        (self.directory / 'compiler-load.json').write_text(json.dumps(self.records, indent=2) + '\n')


def resource_facts() -> dict:
    result = {}
    for name in ('memory.peak', 'memory.current', 'memory.swap.current', 'memory.swap.peak', 'cpu.stat', 'memory.events'):
        path = Path('/sys/fs/cgroup') / name
        result[name] = path.read_text() if path.exists() else None
    return result


def main() -> int:
    args = argparse.ArgumentParser()
    args.add_argument('--out', type=Path, required=True)
    args.add_argument('--binary', type=Path, required=True)
    args.add_argument('--compile-commands', type=Path, required=True)
    opts = args.parse_args()
    commands = json.loads(opts.compile_commands.read_text())
    item = next(c for c in commands if c['file'].endswith('ros2_control_execution_backend.cpp'))
    command = shlex.split(item['command'])
    if Path(command[0]).name not in ('c++', 'g++') or not command[0].startswith('/usr/bin/'):
        raise RuntimeError('expected direct system C++ compiler, no compiler cache')
    results = []
    rclpy.init()
    try:
        for rate in (100, 150):
            for loaded in (False, True):
                case = f'rate{rate}_' + ('compile_load' if loaded else 'idle')
                output = opts.out / case
                probe = Probe(output, case)
                yaml = (output / 'controllers.yaml').read_text()
                (output / 'controllers.yaml').write_text(yaml.replace('update_rate: 100', f'update_rate: {rate}'))
                load = CompileLoad(command, output) if loaded else None
                child = None
                outcome = dict(case=case, rate_hz=rate, loaded=loaded,
                               measured_budget=50, warmup_budget=5,
                               resource_before=resource_facts())
                try:
                    probe.start()  # Direct fixture activation, never CCLC proof.
                    if load:
                        load.start()
                    with (output / 'adapter-probe.log').open('w') as log:
                        child = subprocess.Popen([str(opts.binary), str(rate), '50', '5', str(output)],
                                                 stdout=log, stderr=subprocess.STDOUT,
                                                 start_new_session=True)
                        deadline = time.monotonic() + 35
                        while child.poll() is None and time.monotonic() < deadline:
                            probe.spin(0.01)
                        if child.poll() is None:
                            stop_process(child)
                            raise TimeoutError('bounded real-adapter case deadline')
                        outcome['returncode'] = child.wait(timeout=5)
                    if outcome['returncode'] != 0:
                        raise RuntimeError(f'real adapter probe failed {outcome["returncode"]}')
                    outcome['status'] = 'observed'
                except BaseException:
                    outcome['status'] = 'failed'
                    outcome['traceback'] = traceback.format_exc()
                finally:
                    stop_process(child)
                    if load:
                        try:
                            load.close()
                            if any('error' in r for r in load.records):
                                outcome['status'] = 'failed'
                                outcome['load_error'] = 'managed compiler workload failed; retained observations'
                        except BaseException:
                            outcome['status'] = 'failed'
                            outcome['load_cleanup_error'] = traceback.format_exc()
                    try:
                        probe.close()
                    except BaseException:
                        outcome['status'] = 'failed'
                        outcome['manager_cleanup_error'] = traceback.format_exc()
                    outcome['resource_after'] = resource_facts()
                    (output / 'outcome.json').write_text(json.dumps(outcome, indent=2) + '\n')
                    results.append(outcome)
        (opts.out / 'outcomes.json').write_text(json.dumps(results, indent=2) + '\n')
    finally:
        rclpy.shutdown()
    return int(any(r['status'] != 'observed' for r in results))


if __name__ == '__main__':
    raise SystemExit(main())
