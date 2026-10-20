"""Fault injection against the real VM, Maui callbacks, scheduler and result writer."""
import json
from pathlib import Path
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[3]
HOST = Path(sys.argv.pop(1)).resolve()
sys.path.insert(0, str(ROOT / 'tests'))
from framework import Runner, TestCase as Case, TestStage as Stage
from framework.adapters import PlatformAdapter


class HostAdapter(PlatformAdapter):
    def executable(self, *args): return HOST
    def run(self, command, cwd, log, timeout, env=None):
        def arg(name): return command[command.index(name) + 1]
        return super().run([HOST, ROOT / 'tests/framework/lua/framework.lua', arg('--test-events'),
                            arg('--test-run'), arg('--test-case'), arg('--lua'), arg('--frames')], cwd, log, timeout, env)


class HostTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix='framework host ')
        self.addCleanup(temporary.cleanup)
        self.folder = Path(temporary.name)
        self.init = self.folder / 'init.lua'
        self.init.touch()
        self.runner = Runner(ROOT, HOST.parent, self.init, self.folder / 'output', adapter=HostAdapter())
        self.count = 0

    def run_lua(self, source, **kwargs):
        self.count += 1
        name = 'case' + str(self.count)
        script = self.folder / (name + '.lua')
        script.write_text(source, encoding='utf-8')
        result = self.runner.run(Case(name, script=str(script), frames=30, **kwargs))
        self.records = [json.loads(line) for line in Path(result.events).read_text().splitlines()]
        self.log = Path(result.log).read_text()
        return result

    def test_success_and_cleanup_have_native_evidence(self):
        result = self.run_lua("""
            local t = framework.suite('success')
            t:cleanup(function() LOG('RESOURCE RELEASED') end)
            t:equal('value', 2, 2)
            t:finish()
        """)
        self.assertEqual(result.status, 'passed', result.reason)
        self.assertIn('RESOURCE RELEASED', self.log)
        self.assertTrue(any(r['event'] == 'cleanup' and r['data']['ok'] for r in self.records))

    def test_callback_exception_cannot_pass(self):
        result = self.run_lua("""
            local t = framework.suite('callback')
            local frame = GetFrame(0)
            frame:AcquireKeyboardFocus(true)
            frame.HandleEvent = function(self, event)
                if event.Type == 'KeyDown' then error('callback exploded') end
                return false
            end
            framework.input():press('A')
            t:check('body continued', true)
            t:finish()
        """)
        self.assertEqual(result.status, 'failed')
        self.assertTrue(any(r['event'] == 'error' and 'callback exploded' in r['data']['message'] for r in self.records))

    def test_background_exception_cannot_pass(self):
        result = self.run_lua("""
            local t = framework.suite('background')
            t:fork(function() WaitFrames(2) error('background exploded') end)
            t:check('body passed', true)
            t:finish()
        """)
        self.assertEqual(result.status, 'failed')
        self.assertIn('background exploded', self.log)

    def test_layout_callback_exception_cannot_pass(self):
        result = self.run_lua("""
            local t = framework.suite('layout')
            GetFrame(0).Width.SetValue = function() error('layout exploded') end
            framework.input():resize(800, 600)
            t:check('body continued', true)
            t:finish()
        """)
        self.assertEqual(result.status, 'failed')
        self.assertTrue(any(r['event'] == 'error' and 'layout exploded' in r['data']['message'] for r in self.records))

    def test_native_preference_callbacks_cannot_hide_errors(self):
        for callback in ('SetPreference', 'SavePreferences'):
            with self.subTest(callback=callback):
                result = self.run_lua("""
                    local t = framework.suite('preference callback')
                    local called = false
                    """ + callback + """ = function() called = true error('preference callback exploded') end
                    ConExecute('UI_RenderUnitBars')
                    t:equal('callback was invoked', called, true)
                    t:finish()
                """)
                self.assertEqual(result.status, 'failed')
                self.assertTrue(any(r['event'] == 'error' and 'preference callback exploded' in r['data']['message'] for r in self.records))

    def test_errors_after_completion_override_the_result(self):
        result = self.run_lua("""
            local t = framework.suite('late error')
            ForkThread(function() WaitFrames(4) error('late application error') end)
            t:check('body passed', true)
            t:finish()
        """)
        self.assertEqual(result.status, 'failed')
        kinds = [r['event'] for r in self.records]
        self.assertLess(kinds.index('case_complete'), kinds.index('error'))

    def test_abort_runs_cleanup_in_reverse_order(self):
        result = self.run_lua("""
            local t = framework.suite('abort')
            t:cleanup(function() LOG('CLEANUP FIRST') end)
            t:cleanup(function() LOG('CLEANUP SECOND') end)
            t:assert('stops the test', false)
        """)
        self.assertEqual(result.status, 'failed')
        self.assertLess(self.log.index('CLEANUP SECOND'), self.log.index('CLEANUP FIRST'))

    def test_cleanup_failure_and_yield_are_failures(self):
        for body in ("error('cleanup exploded')", 'WaitFrames(2)'):
            with self.subTest(body=body):
                result = self.run_lua("local t = framework.suite('cleanup'); t:cleanup(function() " + body +
                                      " end); t:check('body passed', true); t:finish()")
                self.assertEqual(result.status, 'failed')
                self.assertTrue(any(r['event'] == 'cleanup' and not r['data']['ok'] for r in self.records))

    def test_skip_runs_cleanup_and_remains_skipped(self):
        result = self.run_lua("""
            local t = framework.suite('skip')
            t:cleanup(function() LOG('SKIP CLEANUP') end)
            t:skip('fixture unavailable')
        """)
        self.assertEqual(result.status, 'skipped', result.reason)
        self.assertIn('fixture unavailable', result.reason)
        self.assertIn('SKIP CLEANUP', self.log)

    def test_skip_cannot_hide_cleanup_failure(self):
        result = self.run_lua("""
            local t = framework.suite('skip cleanup')
            t:cleanup(function() error('skip cleanup exploded') end)
            t:skip('fixture unavailable')
        """)
        self.assertEqual(result.status, 'failed')

    def test_missing_and_duplicate_completion_fail(self):
        for end in ('', 't:finish(); t:finish()'):
            with self.subTest(end=end):
                result = self.run_lua("local t = framework.suite('completion'); t:check('body passed', true); " + end)
                self.assertEqual(result.status, 'failed')

    def test_unfinished_owned_threads_cannot_pass(self):
        result = self.run_lua("""
            local t = framework.suite('unfinished')
            t:fork(function() WaitFrames(1000) end)
            t:check('body passed', true)
            t:finish()
        """)
        self.assertEqual(result.status, 'failed')
        self.assertTrue(any(r['event'] == 'thread_end' and r['data']['status'] == 'cancelled' for r in self.records))

    def test_fake_log_markers_have_no_authority(self):
        self.assertEqual(self.run_lua("LOG('TEST ok pretend'); LOG('TEST done: 0 failed')").status, 'failed')

    def test_quoted_markers_do_not_corrupt_real_results(self):
        result = self.run_lua("""
            local t = framework.suite('quoted markers')
            t:equal('message', 'TEST skipped: literal TEST FAIL', 'TEST skipped: literal TEST FAIL')
            t:finish()
        """)
        self.assertEqual(result.status, 'passed', result.reason)

    def test_setup_and_body_both_execute_in_order(self):
        setup = self.folder / 'prepare.lua'
        setup.write_text('PREPARED = true')
        result = self.run_lua("local t = framework.suite('prepared'); t:equal('setup ran', PREPARED, true); t:finish()",
                              setup=str(setup))
        self.assertEqual(result.status, 'passed', result.reason)
        self.assertEqual([r['data']['name'] for r in self.records if r['event'] == 'stage_start'], ['prepare', 'execute'])
        argv = json.loads((Path(result.log).parent / 'command.json').read_text())
        self.assertEqual(argv.count('--lua'), 1)

    def test_missing_game_stage_fails(self):
        source = self.folder / 'stage.lua'
        source.write_text("local t = framework.suite('stage'); t:check('stage ran', true); t:finish()")
        result = self.runner.run(Case('case', stages=[Stage('prepare', str(source)),
                                                    Stage('game', str(source), context='game')], frames=10))
        self.assertEqual(result.status, 'failed')

    def test_typing_generates_shift_and_punctuation_key_events(self):
        result = self.run_lua("""
            local t = framework.suite('typing')
            local seen, frame = {}, GetFrame(0)
            frame:AcquireKeyboardFocus(true)
            frame.HandleEvent = function(self, event)
                table.insert(seen, event.Type .. event.KeyCode .. (event.Modifiers.Shift and 's' or ''))
                return false
            end
            framework.input():type_keys('A!')
            t:equal('US key sequence', table.concat(seen, ' '),
                    'KeyDown65s Char65s KeyUp65s KeyDown49s Char33s KeyUp49s')
            t:finish()
        """)
        self.assertEqual(result.status, 'passed', result.reason)

    def test_process_exit_cannot_leave_a_passing_partial_record(self):
        result = self.run_lua("local t = framework.suite('exit'); t:check('body passed', true); os.exit(0)")
        self.assertEqual(result.status, 'failed')

    def test_control_characters_in_errors_preserve_valid_reports(self):
        result = self.run_lua("error('control character: ' .. string.char(1))")
        self.assertEqual(result.status, 'failed')
        suite = ET.parse(self.runner.output / 'junit.xml').getroot()
        self.assertEqual(suite.get('failures'), '1')


if __name__ == '__main__': unittest.main()
