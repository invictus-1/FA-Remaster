"""Configuration, supervision and execution-record contracts; standard library only."""
import copy
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / 'tests'))
from framework import Runner, TestCase as Case, TestStage as Stage
from framework.adapters import LinuxAdapter, MacOSAdapter, PlatformAdapter, WindowsAdapter
from framework.cli import load_suite
from framework.protocol import evaluate, read_events
from framework.runner import lua_string


def ledger(case, payloads=None):
    payloads = payloads or [
        ('host_start', 0, {'kind': 'lua'}),
        ('stage_start', 1, {'name': 'execute', 'terminal': True}),
        ('suite_start', 1, {'suite': 1, 'name': 'contract'}),
        ('assertion', 1, {'suite': 1, 'label': 'a real assertion', 'ok': True}),
        ('cleanup', 1, {'suite': 1, 'label': 'resource', 'ok': True, 'reason': ''}),
        ('suite_end', 1, {'suite': 1, 'status': 'passed', 'reason': ''}),
        ('stage_end', 1, {'status': 'completed'}),
        ('case_complete', 1, {'status': 'passed'}),
        ('host_exit', 0, {'returncode': 0, 'pending': 0}),
    ]
    return [dict(version=1, run='fixture-run', case=case.name, sequence=i, event=event, stage=stage, data=data)
            for i, (event, stage, data) in enumerate(payloads, 1)]


class RecordTests(unittest.TestCase):
    def setUp(self): self.case = Case('contract', script='contract.lua')
    def status(self, records, text='', returncode=0):
        return evaluate(text, returncode, self.case, records, 'fixture-run')[0]

    def test_pass_requires_structured_evidence(self):
        self.assertEqual(self.status(ledger(self.case)), 'passed')
        self.assertEqual(self.status(None, 'TEST ok a\nTEST done: 0 failed'), 'failed')
        records = ledger(self.case)
        records.pop(3)
        for i, record in enumerate(records, 1): record['sequence'] = i
        self.assertEqual(self.status(records), 'failed')

    def test_log_text_cannot_create_outcomes(self):
        text = 'TEST FAIL quoted text\nTEST skipped: quoted text\nTEST done: 9 failed'
        self.assertEqual(self.status(ledger(self.case), text), 'passed')

    def test_typed_errors_override_completion(self):
        records = ledger(self.case)
        records.insert(-1, dict(version=1, run='fixture-run', case=self.case.name, sequence=9,
                                event='error', stage=0, data={'message': 'callback exploded'}))
        records[-1]['sequence'] = 10
        self.assertEqual(self.status(records, 'warning: unusual callback format'), 'failed')

    def test_partial_duplicate_and_foreign_records_fail(self):
        for mutation in ('partial', 'sequence', 'case', 'run', 'version', 'duplicate_completion', 'bool_id', 'data'):
            records = copy.deepcopy(ledger(self.case))
            if mutation == 'partial': records.pop()
            elif mutation == 'sequence': records[3]['sequence'] = 3
            elif mutation == 'case': records[3]['case'] = 'another-case'
            elif mutation == 'run': records[3]['run'] = 'old-run'
            elif mutation == 'version': records[3]['version'] = 2
            elif mutation == 'bool_id': records[3]['stage'] = True
            elif mutation == 'data': records[3]['data']['ok'] = 'true'
            else:
                records.insert(-1, copy.deepcopy(records[-2]))
                for i, record in enumerate(records, 1): record['sequence'] = i
            with self.subTest(mutation=mutation): self.assertEqual(self.status(records), 'failed')

    def test_unfinished_threads_and_stages_fail(self):
        records = ledger(self.case)
        records.insert(3, dict(version=1, run='fixture-run', case=self.case.name, sequence=4,
                               event='thread_start', stage=1, data={'thread': 1}))
        for i, record in enumerate(records, 1): record['sequence'] = i
        self.assertEqual(self.status(records), 'failed')
        self.case = Case('contract', stages=[Stage('prepare', 'prepare.lua'), Stage('execute', 'contract.lua')])
        self.assertEqual(self.status(ledger(self.case)), 'failed')

    def test_nonzero_exit_overrides_checks(self):
        records = ledger(self.case)
        records[-1]['data']['returncode'] = 7
        self.assertEqual(self.status(records, returncode=7), 'failed')

    def test_command_cases_require_declared_checks(self):
        case = Case('simulation', host='sim', completion=False, required=['ran 20 ticks'])
        payloads = [('host_start', 0, {'kind': 'command'}), ('host_exit', 0, {'returncode': 0, 'pending': 0})]
        for text, expected in (('ran 20 ticks', 'passed'), ('ran 19 ticks', 'failed')):
            self.assertEqual(evaluate(text, 0, case, ledger(case, payloads), 'fixture-run')[0], expected)
        payloads.insert(1, ('error', 0, {'message': 'sim callback exploded'}))
        self.assertEqual(evaluate('ran 20 ticks', 0, case, ledger(case, payloads), 'fixture-run')[0], 'failed')

    def test_json_rejects_duplicate_keys_and_nonfinite_numbers(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / 'events.jsonl'
            for text in ('{"sequence":1,"sequence":2}', '{"sequence":NaN}'):
                path.write_text(text)
                with self.assertRaises(ValueError): read_events(path)


class ConfigurationTests(unittest.TestCase):
    def test_values_are_validated_without_coercion(self):
        invalid = [{'frames': True}, {'frames': 1.5}, {'width': '800'}, {'height': 0}, {'window': 'false'},
                   {'timeout': float('nan')}, {'timeout': float('inf')}, {'timeout': True}, {'args': '--seed 7'},
                   {'args': ['--prefs', '/tmp/shared']}, {'required': [1]}, {'environment': {'A': 1}},
                   {'fixtures': {'replay': None}}, {'capabilities': ['made-up']}, {'stage': 'made-up'},
                   {'host': 'sim'}, {'operation': 'rules'}, {'completion': False}]
        for kwargs in invalid:
            with self.subTest(kwargs=kwargs), self.assertRaises(ValueError): Case('case', script='case.lua', **kwargs)

    def test_case_and_stage_combinations(self):
        with self.assertRaises(ValueError): Case('empty', completion=False)
        with self.assertRaises(ValueError): Case('game', script='game.lua', game_script=True)
        with self.assertRaises(ValueError): Case('mixed', script='case.lua', stages=[Stage('execute', 'case.lua')])
        with self.assertRaises(ValueError): Case('stages', stages=[Stage('game', 'g.lua', 'game'), Stage('frontend', 'f.lua')])
        for name in ('../escape', '/tmp/test', '', 'a/b'):
            with self.assertRaises(ValueError): Case(name, script='case.lua')
        case = Case('prepared', script='case.lua', setup='setup.lua')
        self.assertEqual([(s.name, s.terminal) for s in case.plan()], [('prepare', False), ('execute', True)])

    def test_suite_structure_and_duplicates(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / 'suite.json'
            for source in ('{}', '{"tests":"case"}', '{"tests":[],"typo":1}',
                           '{"tests":[{"name":"case","script":"a"},{"name":"case","script":"b"}]}',
                           '{"tests":[{"name":"case","script":"a","frames":10,"frames":20}]}'):
                path.write_text(source)
                with self.assertRaises(ValueError): load_suite(path)

    def test_lua_quoting_preserves_paths(self):
        self.assertEqual(lua_string('a"b\\c\n'), '"a\\"b\\\\c\\010"')
        self.assertEqual(lua_string('é'), '"é"')


class AdapterTests(unittest.TestCase):
    def test_executable_and_loader_paths(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for adapter in (LinuxAdapter(), MacOSAdapter(), WindowsAdapter()):
                path = root / ('moho64ui_test' + adapter.suffix)
                path.touch()
                self.assertEqual(adapter.executable(root, 'moho64ui_test'), path)
                env = adapter.environment(path, {adapter.loader_variable: 'existing'})
                self.assertTrue(env[adapter.loader_variable].endswith(adapter.path_separator + 'existing'))
                path.unlink()
                with self.assertRaises(RuntimeError): adapter.executable(root, 'moho64ui_test')

    def test_process_arguments_and_timeout(self):
        with tempfile.TemporaryDirectory(prefix='portable tests ') as temporary:
            log = Path(temporary) / 'output.log'
            literal = 'path with spaces;$(do-not-run)'
            adapter = PlatformAdapter.detect()
            self.assertEqual(adapter.run([sys.executable, '-c', 'import sys; print(sys.argv[1])', literal], temporary, log, 10), 0)
            self.assertEqual(log.read_text().strip(), literal)
            with self.assertRaises(subprocess.TimeoutExpired):
                adapter.run([sys.executable, '-c', 'import time; time.sleep(10)'], temporary, log, 0.1)

    @unittest.skipUnless(os.name == 'posix', 'POSIX process groups')
    def test_descendants_are_stopped_on_exit_and_timeout(self):
        with tempfile.TemporaryDirectory() as temporary:
            folder = Path(temporary)
            marker = folder / 'leaked-child'
            child = 'import pathlib,time,sys; time.sleep(0.8); pathlib.Path(sys.argv[1]).touch()'
            parent = ('import subprocess,sys,time; subprocess.Popen([sys.executable,"-c",sys.argv[1],sys.argv[2]]); '
                      'time.sleep(float(sys.argv[3]))')
            for delay, timeout in (('0', 5), ('10', 0.1)):
                try:
                    PlatformAdapter.detect().run([sys.executable, '-c', parent, child, str(marker), delay], folder, folder / 'log', timeout)
                except subprocess.TimeoutExpired: self.assertEqual(delay, '10')
                time.sleep(1)
                self.assertFalse(marker.exists())


class RunnerTests(unittest.TestCase):
    def test_ui_requires_a_dedicated_host(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            init = root / 'init.lua'
            init.touch()
            result = Runner(root, root, init, root / 'output').run(Case('ui', script='unused.lua'))
            self.assertEqual(result.status, 'failed')
            self.assertIn('moho64ui_test', result.reason)

    def test_platform_mismatch_creates_no_run(self):
        platform = 'linux' if sys.platform == 'win32' else 'windows'
        with tempfile.TemporaryDirectory() as temporary:
            folder = Path(temporary)
            init = folder / 'init.lua'
            init.touch()
            output = folder / 'output'
            result = subprocess.run([sys.executable, str(ROOT / 'tests/run.py'), '--platform', platform,
                                     '--init', str(init), '--output', str(output)], cwd=ROOT, capture_output=True, text=True,
                                    env=dict(os.environ, PYTHONDONTWRITEBYTECODE='1'))
            self.assertEqual(result.returncode, 2)
            self.assertFalse(output.exists())

    def test_isolation_reports_and_skips(self):
        class FixtureAdapter(LinuxAdapter):
            def executable(self, *args): return Path(sys.executable)
            def run(self, command, cwd, log, timeout, env=None):
                def arg(name): return command[command.index(name) + 1]
                prefs = Path(arg('--prefs'))
                self.was_isolated = not prefs.exists()
                prefs.write_text('isolated')
                records = ledger(Case(str(arg('--test-case')), script='fixture.lua'))
                for record in records: record['run'] = str(arg('--test-run'))
                Path(arg('--test-events')).write_text(''.join(json.dumps(record) + '\n' for record in records))
                Path(log).write_text('diagnostic text')
                return 0
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            init = root / 'init.lua'
            init.touch()
            script = root / 'case.lua'
            script.write_text('-- fixture')
            runners = [Runner(root, root, init, root / 'out', adapter=FixtureAdapter()) for _ in range(2)]
            for runner in runners:
                self.assertEqual(runner.run(Case('fixture', script=str(script))).status, 'passed')
                self.assertTrue(runner.adapter.was_isolated)
                self.assertEqual(runner.run(Case('desktop', script=str(script), capabilities=['window_input'])).status, 'skipped')
                with self.assertRaises(FileExistsError): runner.run(Case('fixture', script=str(script)))
                payload = json.loads((runner.output / 'results.json').read_text())
                self.assertEqual([r['status'] for r in payload['results']], ['passed', 'skipped'])
                self.assertEqual(ET.parse(runner.output / 'junit.xml').getroot().get('skipped'), '1')
                self.assertTrue((runner.output / 'fixture/test.prefs').is_file())
            self.assertNotEqual(runners[0].output, runners[1].output)


if __name__ == '__main__': unittest.main()
