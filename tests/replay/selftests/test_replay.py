"""Capture integrity, regression detection, and isolated runner failure paths."""
import copy
import gzip
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / 'tests'))
from replay.records import collect, compare, inspect
from replay.runner import ReplayCase, ReplayRunner, IsolatedAdapter, collector_digest, load_suite, lua_value
from replay.run import main
from replay.legacy import OriginalChunks, compare_legacy, inspect_legacy


def data():
    resource = dict(stored=10.0, ratio=0.5, income=1.0, requested=2.0, usage=1.0)
    unit = dict(army=1, blueprint='testunit', dead=False, position=dict(x=0, y=0, z=0),
                velocity=dict(x=0, y=0, z=0), orientation=dict(x=0, y=0, z=0, w=1),
                health=100.0, max_health=100.0, fraction_complete=1, work_progress=0, fuel=1, shield=0,
                layer='Land', states={}, orders={'1': dict(type=2, x=10, y=0, z=0, target_id='42')}, queue_length=1)
    return dict(armies={'1': dict(name='ARMY_1', defeated=False, economy=dict(MASS=copy.deepcopy(resource), ENERGY=resource))},
                units={'7': unit})


def records(role='original', identity='capture-id', name='full-game'):
    common = dict(version=1, capture=identity)
    header = dict(common, event='header', case=name, role=role, collector=collector_digest(),
                  start_tick=1, end_tick=3, interval=1, session=dict(map='/maps/test/test.scmap', options={}, mods={}))
    ticks = [dict(common, event='tick', sequence=n, tick=n, data=data()) for n in range(1, 4)]
    return [header, *ticks, dict(common, event='complete', samples=3, last_tick=3)]


def write(path, rows):
    path.write_text(''.join(json.dumps(row) + '\n' for row in rows), encoding='utf-8')


def legacy_records():
    rows = [dict(kind='start', schema_version=1, run_id='original-run', requested_ticks=[1, 2, 3])]
    for tick in range(1, 4):
        source = data()
        units = [dict(entity_id=int(identity), blueprint_id=unit['blueprint'], army=unit['army'],
                      position=[unit['position'][key] for key in ('x', 'y', 'z')], health=unit['health'], max_health=unit['max_health'])
                 for identity, unit in source['units'].items()]
        armies = [dict(army=int(identity), mass_stored=army['economy']['MASS']['stored'],
                       energy_stored=army['economy']['ENERGY']['stored']) for identity, army in source['armies'].items()]
        rows.append(dict(kind='snapshot', schema_version=1, run_id='original-run', tick=tick, units=units, armies=armies))
    rows.append(dict(kind='complete', schema_version=1, run_id='original-run', snapshot_count=3))
    return rows


class CaptureTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='fa-replay-tests-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.oracle, self.actual = self.root / 'oracle.jsonl', self.root / 'actual.jsonl'
        write(self.oracle, records())
        write(self.actual, records('candidate', 'new-id'))

    def test_equal_captures_and_order_independent_objects(self):
        result = compare(self.oracle, self.actual)
        self.assertTrue(result.passed)
        self.assertEqual(result.ticks, 3)
        self.assertGreater(result.checks, 100)
        rows = records('candidate')
        rows[1]['data'] = dict(reversed(list(rows[1]['data'].items())))
        write(self.actual, rows)
        self.assertTrue(compare(self.oracle, self.actual).passed)

    def test_first_divergence_for_movement_combat_economy_and_orders(self):
        changes = [('position', lambda d: d['units']['7']['position'].update(x=4)),
                   ('health', lambda d: d['units']['7'].update(health=99)),
                   ('economy', lambda d: d['armies']['1']['economy']['MASS'].update(stored=9)),
                   ('orders', lambda d: d['units']['7']['orders']['1'].update(target_id='43')),
                   ('unexpected', lambda d: d['units'].update({'8': copy.deepcopy(d['units']['7'])})),
                   ('missing', lambda d: d['units'].clear())]
        for label, change in changes:
            rows = records('candidate')
            change(rows[2]['data'])
            write(self.actual, rows)
            result = compare(self.oracle, self.actual)
            with self.subTest(label=label):
                self.assertFalse(result.passed)
                self.assertEqual(result.first_tick, 2)
                self.assertIn(label, result.reason)

    def test_incomplete_duplicate_foreign_and_malformed_captures_fail(self):
        mutations = [lambda r: r.pop(), lambda r: r.pop(2), lambda r: r.insert(2, copy.deepcopy(r[1])),
                     lambda r: r.append(r[-1]), lambda r: r[2].update(capture='foreign'),
                     lambda r: r[1].update(sequence=True), lambda r: r[-1].update(samples=True),
                     lambda r: r[1]['data']['units']['7'].pop('health'),
                     lambda r: r[1]['data'].update(armies={}),
                     lambda r: r[1]['data']['units']['7'].update(position=[])]
        for mutation in mutations:
            rows = records('candidate')
            mutation(rows)
            write(self.actual, rows)
            with self.subTest(mutation=mutation), self.assertRaises(ValueError):
                inspect(self.actual)
        for value in ('NaN', 'Infinity', '1e999'):
            self.actual.write_text(json.dumps(records('candidate')[0]).replace('"interval": 1', '"interval": ' + value) + '\n')
            with self.assertRaises(ValueError): inspect(self.actual)
        self.actual.write_text('{"version":1,"version":1}\n')
        with self.assertRaises(ValueError): inspect(self.actual)

    def test_tolerances_do_not_mask_discrete_changes(self):
        rows = records('candidate')
        rows[2]['data']['units']['7']['position']['x'] = 1
        write(self.actual, rows)
        self.assertTrue(compare(self.oracle, self.actual, abs_tol=1).passed)
        rows[2]['data']['units']['7']['orders']['1']['type'] = 3
        write(self.actual, rows)
        self.assertFalse(compare(self.oracle, self.actual, abs_tol=1).passed)
        for value in (-1, float('nan'), float('inf'), True):
            with self.assertRaises(ValueError): compare(self.oracle, self.actual, abs_tol=value)

    def test_metadata_changes_and_candidate_baselines_fail(self):
        for key, value in [('collector', 'a' * 64), ('case', 'another'), ('session', dict(map='/other', options={}, mods={}))]:
            rows = records('candidate')
            rows[0][key] = value
            write(self.actual, rows)
            self.assertFalse(compare(self.oracle, self.actual).passed)
        write(self.oracle, records('candidate'))
        with self.assertRaises(ValueError): compare(self.oracle, self.actual)

    def test_collect_validates_completion_and_never_overwrites(self):
        log, output = self.root / 'original.log', self.root / 'collected.jsonl'
        log.write_text('warning: diagnostic\n' + ''.join('info: SESSION_REPLAY ' + json.dumps(row) + '\n' for row in records()))
        self.assertEqual(collect(log, output)['end_tick'], 3)
        old = output.read_bytes()
        with self.assertRaises(FileExistsError): collect(log, output)
        self.assertEqual(output.read_bytes(), old)
        output.unlink()
        log.write_text('SESSION_REPLAY ' + json.dumps(records()[0]) + '\n')
        with self.assertRaises(ValueError): collect(log, output)
        self.assertFalse(output.exists())

    def test_compressed_captures_stream_and_preserve_existing_files(self):
        compressed = self.root / 'oracle.jsonl.gz'
        with gzip.open(compressed, 'wt', encoding='utf-8') as target:
            target.write(self.oracle.read_text())
        self.assertTrue(compare(compressed, self.actual).passed)
        log, collected = self.root / 'log', self.root / 'collected.jsonl.gz'
        log.write_text(''.join('SESSION_REPLAY ' + json.dumps(row) + '\n' for row in records()))
        self.assertEqual(collect(log, collected)['end_tick'], 3)
        before = collected.read_bytes()
        with self.assertRaises(FileExistsError): collect(log, collected)
        self.assertEqual(collected.read_bytes(), before)


class RunnerTests(unittest.TestCase):
    def setUp(self):
        CaptureTests.setUp(self)
        self.init = self.root / 'game mounts.lua'
        self.init.write_text("path = {}\nhook = {}\n")
        self.replay = self.root / 'game.SCFAReplay'
        self.replay.touch()
        executable = self.root / ('moho64_test' + IsolatedAdapter().suffix)
        executable.touch()
        self.case = ReplayCase('full-game', str(self.oracle), replay=str(self.replay))
        self.runner = ReplayRunner(self.root, self.init, self.root / 'reports', self.root)

    def fake_host(self, command, cwd, log, timeout):
        cwd = Path(cwd)
        hook = (cwd / 'capture/session-replay-hook/lua/simInit.lua').read_text()
        identity = re.search(r'capture = "([a-f0-9]+)"', hook)[1]
        rows = records('candidate', identity)
        Path(log).write_text(''.join('info: SESSION_REPLAY ' + json.dumps(row) + '\n' for row in rows) + 'info: moho64: ran 3 ticks\n')
        events = [dict(version=1, run=self.runner.runtime.run_id, case='full-game', sequence=1, event='host_start', stage=0,
                       data=dict(kind='command')),
                  dict(version=1, run=self.runner.runtime.run_id, case='full-game', sequence=2, event='host_exit', stage=0,
                       data=dict(returncode=0, pending=0))]
        write(cwd / 'events.jsonl', events)
        return 0

    def test_runner_uses_private_process_paths_and_reports(self):
        before = self.oracle.read_bytes()
        with patch.object(self.runner.runtime.adapter, 'run', side_effect=self.fake_host) as host:
            result = self.runner.run(self.case)
        self.assertEqual(result.status, 'passed', result.reason)
        command, cwd, _, _ = host.call_args.args
        self.assertEqual(Path(cwd), self.runner.output / 'full-game')
        self.assertIn('--sim', command)
        self.assertNotEqual(command[command.index('--init') + 1], self.init)
        self.assertEqual(self.oracle.read_bytes(), before)
        self.assertGreater(result.checks, 100)
        self.assertEqual(ET.parse(self.runner.output / 'junit.xml').getroot().get('failures'), '0')
        other = ReplayRunner(self.root, self.init, self.root / 'reports', self.root)
        self.assertNotEqual(self.runner.output, other.output)

    def test_engine_errors_override_complete_matching_capture(self):
        def broken(*args):
            self.fake_host(*args)
            file = Path(args[1]) / 'events.jsonl'
            events = [json.loads(line) for line in file.read_text().splitlines()]
            events.insert(1, dict(events[0], event='error', data=dict(message='Lua callback failed')))
            for index, event in enumerate(events, 1): event['sequence'] = index
            write(file, events)
            return 0
        with patch.object(self.runner.runtime.adapter, 'run', side_effect=broken):
            result = self.runner.run(self.case)
        self.assertEqual(result.status, 'failed')
        self.assertIn('Lua callback failed', result.reason)

    def test_timeout_and_missing_baseline_fail(self):
        with patch.object(self.runner.runtime.adapter, 'run', side_effect=subprocess.TimeoutExpired('host', 1)):
            result = self.runner.run(self.case)
        self.assertEqual(result.status, 'failed')
        self.assertIn('timed out', result.reason)
        self.oracle.unlink()
        other = ReplayRunner(self.root, self.init, self.root / 'reports', self.root)
        self.assertEqual(other.run(self.case).status, 'failed')

    def test_debug_environment_does_not_leak(self):
        with patch.dict(os.environ, {'MOHO64_NO_STREAM': '1', 'MOHO64_START_LUA': '/foreign.lua', 'MOHO64_FIXES': '1'}):
            env = self.runner.runtime.adapter.environment(sys.executable)
        self.assertEqual({k: v for k, v in env.items() if k.startswith('MOHO64_')}, {'MOHO64_FIXES': '0'})

    def test_replay_isolation_uses_each_native_platform_loader(self):
        from framework.adapters import LinuxAdapter, MacOSAdapter, WindowsAdapter
        for native in (LinuxAdapter(), MacOSAdapter(), WindowsAdapter()):
            adapter = IsolatedAdapter(native)
            executable = self.root / ('moho64_test' + native.suffix)
            with patch.dict(os.environ, {'MOHO64_NO_STREAM': '1'}):
                env = adapter.environment(executable)
            self.assertEqual(adapter.suffix, native.suffix)
            self.assertNotIn('MOHO64_NO_STREAM', env)
            self.assertTrue(env[native.loader_variable].startswith(str(self.root)))

    def test_existing_original_hook_capture_runs_against_exact_launch_table(self):
        write(self.oracle, legacy_records())
        session = self.root / 'session-info.lua'
        session.write_text('return {RandomSeed=1}\n')
        case = ReplayCase('full-game', str(self.oracle), session_info=str(session), seed=1, oracle_format='fa-oracle')
        with patch.object(self.runner.runtime.adapter, 'run', side_effect=self.fake_host) as host:
            result = self.runner.run(case)
        self.assertEqual(result.status, 'passed', result.reason)
        self.assertIn('--session-info', host.call_args.args[0])


class LegacyTests(unittest.TestCase):
    def setUp(self):
        CaptureTests.setUp(self)
        write(self.oracle, legacy_records())

    def test_existing_hook_values_match_candidate_projection(self):
        self.assertTrue(compare_legacy(self.oracle, self.actual).passed)
        rows = legacy_records()
        rows[2]['units'][0]['position'][0] = 0.01
        write(self.oracle, rows)
        result = compare_legacy(self.oracle, self.actual)
        self.assertFalse(result.passed)
        self.assertEqual(result.first_tick, 2)
        self.assertIn('position', result.reason)
        self.assertTrue(compare_legacy(self.oracle, self.actual, abs_tol=0.01).passed)

    def test_existing_hook_incomplete_and_malformed_data_fail(self):
        mutations = [lambda r: r.pop(), lambda r: r.pop(2), lambda r: r[1].update(tick=True),
                     lambda r: r[1]['units'].append(r[1]['units'][0]),
                     lambda r: r[1]['units'][0].update(health=float('inf')),
                     lambda r: r[0].update(schema_version=True), lambda r: r[-1].update(snapshot_count=True)]
        for change in mutations:
            rows = legacy_records()
            change(rows)
            write(self.oracle, rows)
            with self.assertRaises(ValueError): inspect_legacy(self.oracle)

    def test_original_log_chunks_detect_loss_duplicates_and_foreign_runs(self):
        text = json.dumps(legacy_records()[0])
        chunks = [dict(run_id='original-run', sequence=1, part=n + 1, parts=2, data=piece)
                  for n, piece in enumerate((text[:20], text[20:]))]
        assembler = OriginalChunks('original-run')
        self.assertIsNone(assembler.append(chunks[0]))
        self.assertEqual(assembler.append(chunks[1]), legacy_records()[0])
        for mutation in (dict(part=2), dict(run_id='foreign'), dict(sequence=True), dict(parts=1)):
            assembler = OriginalChunks('original-run')
            assembler.append(chunks[0])
            with self.assertRaises(ValueError): assembler.append(dict(chunks[0], **mutation))


class SuiteTests(unittest.TestCase):
    def test_shared_runner_loads_mixed_cases_and_uses_one_report(self):
        from framework.cli import load_suite as shared_suite
        from framework import Runner
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            suite = root / 'suite.json'
            suite.write_text(json.dumps({'tests': [
                {'name': 'ui-case', 'script': 'case.lua'},
                {'name': 'replay-case', 'replay': 'game', 'oracle': 'missing'}
            ]}))
            cases = shared_suite(suite)
            self.assertIsInstance(cases[1], ReplayCase)
            init = root / 'init.lua'
            init.write_text('path = {}')
            runtime = Runner(root, root, init, root / 'reports')
            result = runtime.run(cases[1])
            self.assertEqual(result.status, 'failed')
            self.assertEqual(len(runtime.results), 1)
            self.assertTrue((runtime.output / 'results.json').is_file())
            self.assertTrue((runtime.output / 'junit.xml').is_file())

    def test_committed_original_fixture_hashes_and_completion(self):
        directory = ROOT / 'tests/fixtures/session_replay/original_battle'
        provenance = json.loads((directory / 'provenance.json').read_text())
        for filename, expected in provenance['files'].items():
            self.assertEqual(hashlib.sha256((directory / filename).read_bytes()).hexdigest(), expected)
        for filename, end in (('startup.jsonl.gz', 8), ('prefix.jsonl.gz', 128), ('full.jsonl.gz', 6000)):
            header = inspect_legacy(directory / filename)
            self.assertEqual(header['requested_ticks'], list(range(1, end + 1)))

    def test_lua_sessions_require_seed_and_strict_suite(self):
        for fields in (dict(session='game.lua'), dict(session='game.lua', seed=True), dict(replay='game', seed=1),
                       dict(replay='game', session='game.lua'), dict(replay='game', timeout=float('nan'))):
            with self.assertRaises(ValueError): ReplayCase('case', 'oracle', **fields)
        self.assertEqual(ReplayCase('case', 'oracle', session='game.lua', seed=0).seed, 0)
        with self.assertRaises(ValueError): ReplayCase('case', 'oracle', replay='game', setup_input={'tick': 5})
        with self.assertRaises(ValueError): lua_value({'tick': float('nan')})
        self.assertEqual(lua_value({'when': 5, 'enabled': True}), '{["enabled"] = true, ["when"] = 5}')
        with tempfile.TemporaryDirectory() as temporary:
            suite = Path(temporary) / 'suite.json'
            for value in ({'tests': []}, {'tests': [{'name': 'case', 'oracle': 'a', 'replay': 'b', 'typo': 1}]}):
                suite.write_text(json.dumps(value))
                with self.assertRaises((ValueError, TypeError)): load_suite(suite)

    def test_prepare_and_list_need_no_game_installation(self):
        with tempfile.TemporaryDirectory() as temporary:
            overlay = Path(temporary) / 'overlay'
            self.assertEqual(main(['prepare', '--end-tick', '3', '--output', str(overlay)]), 0)
            self.assertTrue((overlay / 'session-replay-hook/lua/simInit.lua').is_file())
            self.assertEqual(main(['run', '--list']), 0)


if __name__ == '__main__':
    unittest.main()
