#!/usr/bin/env python3
"""Run platform independent Lua, simulation and renderer test cases (standard library only)."""
import argparse
import json
from pathlib import Path
import sys

from . import Runner, TestCase
from .adapters import PlatformAdapter
from .protocol import unique_object
from .cases import source_file

ROOT = Path(__file__).resolve().parents[2]


def load_suite(path):
    from replay.runner import ReplayCase
    raw = json.loads(Path(path).read_text(encoding='utf-8'), object_pairs_hook=unique_object)
    if type(raw) is not dict or set(raw) != {'tests'} or type(raw['tests']) is not list:
        raise ValueError('suite must contain a tests list')
    cases = [(ReplayCase if 'oracle' in entry else TestCase)(**entry) for entry in raw['tests']]
    if len({case.name for case in cases}) != len(cases):
        raise ValueError('suite contains duplicate test names')
    return cases


def main():
    from replay.runner import ReplayCase
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--platform', choices=('auto', 'linux', 'windows', 'macos'), default='auto',
                        help='native platform adapter; default: detect the current operating system')
    parser.add_argument('--build-dir', type=Path, default=ROOT / 'build-native', help='directory containing the built executables')
    parser.add_argument('--init', type=Path, help='game mount/init file; required to run runtime tests')
    parser.add_argument('--suite', type=Path, default=ROOT / 'tests/suites/default.json')
    parser.add_argument('--test', action='append', help='test name to run (repeatable)')
    parser.add_argument('--mode', choices=('headless', 'windowed'), default='headless')
    parser.add_argument('--output', type=Path, default=ROOT / '.fa-remaster/tests')
    parser.add_argument('--list', action='store_true', help='list cases without launching anything')
    parser.add_argument('--fixture', action='append', default=[], metavar='NAME=FILE', help='provide a test fixture (repeatable)')
    parser.add_argument('--fixture-root', type=Path, default=ROOT, help='path root for original-session inputs')
    args = parser.parse_args()
    try:
        cases = load_suite(args.suite)
        if args.test:
            missing = set(args.test) - {case.name for case in cases}
            if missing:
                raise ValueError('unknown tests: ' + ', '.join(sorted(missing)))
            cases = [case for case in cases if case.name in args.test]
        if not cases:
            raise ValueError('suite contains no selected tests')
        if args.list:
            for case in cases:
                if isinstance(case, ReplayCase):
                    print(f'{case.name}: sim, {case.replay or case.session or case.session_info} against {case.oracle}')
                else:
                    print(f'{case.name}: {case.host}, {case.script or "command"}')
            return 0
        if args.init is None:
            raise ValueError('--init is required; provide your game mount/init file')
        for case in cases:
            if isinstance(case, ReplayCase):
                for source in (case.oracle, case.replay, case.session, case.session_info, case.sim_setup, case.mods):
                    if source:
                        source_file(args.fixture_root, source)
                continue
            for source in [entry.script for entry in case.plan()] + ([case.session] if case.session else []):
                source_file(ROOT, source)
        adapter = PlatformAdapter.detect(args.platform)
        if type(adapter) is not type(PlatformAdapter.detect()):
            raise ValueError(f'--platform {args.platform} requires running on that operating system')
        fixtures = {}
        for assignment in args.fixture:
            name, separator, value = assignment.partition('=')
            if not separator or not name or not Path(value).is_file():
                raise ValueError('--fixture must be NAME=FILE pointing to an existing file')
            fixtures[name] = str(Path(value).resolve())
        runner = Runner(ROOT, args.build_dir, args.init, args.output, args.mode, adapter=adapter, fixtures=fixtures,
                        fixture_root=args.fixture_root)
        print(f'Results: {runner.output}', flush=True)
        for case in cases:
            result = runner.run(case)
            print(f'{result.status.upper()} {result.name}: {result.checks} checks, {result.duration:.2f}s', flush=True)
            if result.reason:
                print(result.reason, flush=True)
        print(f'Reports: {runner.output / "results.json"}, {runner.output / "junit.xml"}')
        return 0 if all(result.status == 'passed' for result in runner.results) else 1
    except (OSError, RuntimeError, ValueError, KeyError, TypeError) as error:
        print(f'test: {error}', file=sys.stderr)
        return 2


if __name__ == '__main__':
    sys.exit(main())
