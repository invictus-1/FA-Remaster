#!/usr/bin/env python3
"""Capture original-game Lua state once, then replay it in isolated headless engine runs."""
import argparse
import contextlib
import json
from pathlib import Path
import sys
import tempfile
import uuid

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tests'))
from replay.records import collect, compare, integer
from replay.runner import ReplayRunner, bundle, collector_digest, load_suite
from framework.cases import identifier


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='operation', required=True)
    prepare = commands.add_parser('prepare', help='write a Lua capture overlay for the original game')
    prepare.add_argument('--name', default='full-game')
    prepare.add_argument('--start-tick', type=int, default=1)
    prepare.add_argument('--end-tick', type=int, required=True)
    prepare.add_argument('--interval', type=int, default=1)
    prepare.add_argument('--output', type=Path, required=True)
    extract = commands.add_parser('collect', help='extract and validate a complete original capture from a game log')
    extract.add_argument('--log', type=Path, required=True)
    extract.add_argument('--output', type=Path, required=True)
    diff = commands.add_parser('compare', help='compare existing original and candidate captures')
    diff.add_argument('--oracle', type=Path, required=True)
    diff.add_argument('--actual', type=Path, required=True)
    diff.add_argument('--abs-tol', type=float, default=0)
    diff.add_argument('--rel-tol', type=float, default=0)
    run = commands.add_parser('run', help='run every selected case in a fresh simulation process')
    run.add_argument('--suite', type=Path, default=ROOT / 'tests/suites/session_replay.json')
    run.add_argument('--fixtures', type=Path, help='fixture path root; default: the repository')
    run.add_argument('--build-dir', type=Path, default=ROOT / 'build-native')
    run.add_argument('--init', type=Path)
    run.add_argument('--output', type=Path, help='keep logs and JSON/JUnit reports here; otherwise temporary outputs are removed')
    run.add_argument('--test', action='append')
    run.add_argument('--list', action='store_true')
    args = parser.parse_args(argv)
    try:
        if args.operation == 'prepare':
            identifier(args.name, 'capture name')
            for key in ('start_tick', 'end_tick', 'interval'):
                integer(getattr(args, key), key, 1 if key == 'interval' else 0)
            if args.end_tick < args.start_tick or (args.end_tick - args.start_tick) % args.interval:
                raise ValueError('end tick must be on the sampling interval')
            args.output.mkdir(parents=True)
            bundle(args.output, dict(case=args.name, start_tick=args.start_tick, end_tick=args.end_tick,
                                    interval=args.interval, role='original', capture=uuid.uuid4().hex,
                                    collector=collector_digest()))
            print(f'Capture overlay: {args.output.resolve()} (mount at /; append /session-replay-hook to hook)')
            return 0
        if args.operation == 'collect':
            header = collect(args.log, args.output)
            if header['role'] != 'original':
                args.output.unlink()
                raise ValueError('collect requires an original-game capture; candidate data is not a baseline')
            print(f'Original capture: {args.output}, ticks {header["start_tick"]}..{header["end_tick"]}')
            return 0
        if args.operation == 'compare':
            result = compare(args.oracle, args.actual, args.abs_tol, args.rel_tol)
            print(f'{"PASS" if result.passed else "FAIL"}: {result.ticks} matching ticks, {result.checks} checks')
            if result.reason:
                print(result.reason)
            return 0 if result.passed else 1
        cases = load_suite(args.suite)
        if args.test:
            missing = set(args.test) - {case.name for case in cases}
            if missing:
                raise ValueError('unknown tests: ' + ', '.join(sorted(missing)))
            cases = [case for case in cases if case.name in args.test]
        if args.list:
            for case in cases:
                print(f'{case.name}: {case.replay or case.session or case.session_info} against {case.oracle}')
            return 0
        if args.init is None:
            raise ValueError('--init is required; provide the same game scripts, mods and maps as the original')
        with contextlib.ExitStack() as stack:
            output = args.output or Path(stack.enter_context(tempfile.TemporaryDirectory(prefix='fa-session-replay-')))
            runner = ReplayRunner(args.build_dir, args.init, output, args.fixtures or ROOT)
            if args.output:
                print(f'Results: {runner.output}', flush=True)
            passed = True
            for case in cases:
                result = runner.run(case)
                print(f'{result.status.upper()} {result.name}: {result.checks} checks, {result.duration:.2f}s', flush=True)
                if result.reason:
                    print(result.reason, flush=True)
                passed = passed and result.status == 'passed'
            if args.output:
                print(f'Reports: {runner.output / "results.json"}, {runner.output / "junit.xml"}')
            return 0 if passed else 1
    except (OSError, RuntimeError, ValueError, TypeError, KeyError) as error:
        print(f'replay: {error}', file=sys.stderr)
        return 2


if __name__ == '__main__':
    sys.exit(main())
