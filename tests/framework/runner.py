"""Platform independent cases, isolated runs, and strict completion reporting."""
from dataclasses import asdict, dataclass
import json
from pathlib import Path
import subprocess
import time
import uuid
import xml.etree.ElementTree as ET

from .adapters import PlatformAdapter
from .cases import TestCase, source_file
from .protocol import VERSION, evaluate, read_events


@dataclass
class TestResult:
    name: str
    status: str
    duration: float
    log: str
    reason: str = ''
    checks: int = 0
    returncode: object = None
    events: str = ''


def lua_string(value):
    # Lua 5.0 has no JSON \u escapes. UTF-8 strings need only quote, slash and control escaping.
    return '"' + ''.join('\\%03d' % ord(c) if ord(c) < 32 else
                         '\\' + c if c in ('"', '\\') else c for c in str(value)) + '"'


def xml_text(value):
    return ''.join(c if ord(c) in (9, 10, 13) or 0x20 <= ord(c) <= 0xd7ff or
                   0xe000 <= ord(c) <= 0xfffd or 0x10000 <= ord(c) <= 0x10ffff else '\ufffd' for c in value)


class Runner:
    def __init__(self, root, build_dir, init, output, mode='headless', adapter=None, fixtures=None, fixture_root=None):
        if mode not in ('headless', 'windowed'):
            raise ValueError('mode must be headless or windowed')
        self.root = Path(root).resolve()
        self.fixture_root = Path(fixture_root or self.root).resolve()
        self.build_dir = Path(build_dir).resolve()
        self.init = Path(init).resolve()
        if not self.init.is_file():
            raise ValueError(f'game init file does not exist: {self.init}')
        self.mode = mode
        self.adapter = adapter or PlatformAdapter.detect()
        self.fixtures = fixtures or {}
        # Unique directory per invocation; repeated/concurrent runs share no preferences or results.
        self.run_id = uuid.uuid4().hex
        self.output = Path(output).resolve() / (time.strftime('%Y%m%d-%H%M%S') + '-' + self.run_id[:8])
        self.output.mkdir(parents=True)
        self.results = []

    def command(self, case, folder):
        exe = self.adapter.executable(self.build_dir, {'ui': 'moho64ui_test', 'sim': 'moho64_test', 'viewer': 'moho64view_test'}[case.host])
        command = [exe, '--init', self.init, '--test-events', folder / 'events.jsonl', '--test-run', self.run_id,
                   '--test-case', case.name]
        if case.host == 'ui':
            command += ['--prefs', folder / 'test.prefs', '--import-prefs', 'none', '--own', self.root / 'ui/data',
                        '--no-backdrop', '--no-sound', '--size', f'{case.width}x{case.height}',
                        '--stage', case.stage, '--frames', str(case.frames)]
            if not case.world:
                command.append('--no-world')
            if self.mode == 'windowed' and case.window:
                command.append('--window')
            if case.kind == 'lua':
                fixtures = dict(self.fixtures, **case.fixtures)
                entries = ', '.join(f'[{lua_string(key)}] = {lua_string(value)}' for key, value in fixtures.items())
                for context, option in (('frontend', '--lua'), ('game', '--game-lua')):
                    stages = [stage for stage in case.plan() if stage.context == context]
                    if not stages:
                        continue
                    wrapper = folder / (context + '.lua')
                    bodies = []
                    for stage in stages:
                        source = source_file(self.root, stage.script).read_text(encoding='utf-8')
                        bodies.append('{name = ' + lua_string(stage.name) + ', terminal = ' +
                                      ('true' if stage.terminal else 'false') + ', run = function()\n' + source + '\nend}')
                    ready = ', moho64ui_TestHost.prepared' if context == 'frontend' and any(
                        stage.context == 'game' for stage in case.plan()) else ''
                    wrapper.write_text(
                        "local framework = import('/testing/framework.lua')\n"
                        f'framework.configure({lua_string(self.mode if case.window else "headless")})\n' +
                        f'TEST_FIXTURES = {{{entries}}}\n' +
                        ("ConExecute('remaster_frame_rate 60')\n" if self.mode == 'windowed' and case.window else '') +
                        'framework.execute({\n' + ',\n'.join(bodies) + '\n}' + ready + ')\n', encoding='utf-8')
                    command += [option, wrapper]
        if case.operation:
            command += self.adapter.operation_args(case.operation)
        if case.host == 'viewer':
            command += ['--size', f'{case.width}x{case.height}', '--frames', str(case.frames)]
        if case.screenshot:
            command += ['--screenshot', folder / 'screenshot.png']
        if case.capture_window:
            command += ['--window-capture', folder / 'window']
        if case.session:
            command += ['--session', source_file(self.root, case.session)]
        return command + case.args

    def run(self, case):
        from replay.runner import ReplayCase, ReplayRunner
        if isinstance(case, ReplayCase):
            return ReplayRunner.using(self).run(case)
        folder = self.output / case.name
        folder.mkdir()
        log = folder / 'host.log'
        events_path = folder / 'events.jsonl'
        start = time.monotonic()
        try:
            # Capabilities describe the test's needs, never the OS. Unsupported requirements
            # are reported explicitly and do not count as passed.
            available = {'input', 'clipboard', 'resize'} if case.host == 'ui' else set()
            if case.host == 'sim' or case.session:
                available.add('simulation')
            if self.mode == 'windowed' and ((case.world and case.window) or case.host == 'viewer'):
                available.add('rendering')
            if self.mode == 'windowed' and case.host == 'ui' and case.window:
                available.add('window_input')
            missing = set(case.capabilities) - available
            if missing:
                result = TestResult(case.name, 'skipped', 0, str(log), 'requires: ' + ', '.join(sorted(missing)))
            else:
                command = self.command(case, folder)
                (folder / 'command.json').write_text(json.dumps([str(arg) for arg in command], indent=2) + '\n', encoding='utf-8')
                returncode = self.adapter.run(command, self.root, log, case.timeout, case.environment)
                text = log.read_text(encoding='utf-8', errors='replace')
                events = read_events(events_path)
                status, reason, checks = evaluate(text, returncode, case, events, self.run_id)
                if status == 'passed' and case.screenshot and not (folder / 'screenshot.png').is_file():
                    status, reason = 'failed', 'requested screenshot was not produced'
                if status == 'passed' and case.capture_window and not (folder / 'window_game.png').is_file():
                    status, reason = 'failed', 'requested game-window capture was not produced'
                result = TestResult(case.name, status, time.monotonic() - start, str(log), reason, checks, returncode,
                                    str(events_path))
        except subprocess.TimeoutExpired:
            result = TestResult(case.name, 'failed', time.monotonic() - start, str(log), f'timed out after {case.timeout}s',
                                events=str(events_path))
        except (OSError, RuntimeError, ValueError) as error:
            result = TestResult(case.name, 'failed', time.monotonic() - start, str(log), str(error),
                                events=str(events_path))
        self.results.append(result)
        self.report()
        return result

    def report(self):
        payload = {'version': VERSION, 'run': self.run_id, 'mode': self.mode,
                   'results': [asdict(result) for result in self.results]}
        (self.output / 'results.json').write_text(json.dumps(payload, indent=2) + '\n', encoding='utf-8')
        suite = ET.Element('testsuite', name='moho64', tests=str(len(self.results)),
                           failures=str(sum(r.status == 'failed' for r in self.results)),
                           skipped=str(sum(r.status == 'skipped' for r in self.results)))
        for result in self.results:
            node = ET.SubElement(suite, 'testcase', name=result.name, time=f'{result.duration:.3f}')
            if result.status == 'failed':
                reason = xml_text(result.reason)
                ET.SubElement(node, 'failure', message=reason).text = reason
            elif result.status == 'skipped':
                ET.SubElement(node, 'skipped', message=xml_text(result.reason))
            ET.SubElement(node, 'system-out').text = xml_text('Log: ' + result.log)
        ET.ElementTree(suite).write(self.output / 'junit.xml', encoding='utf-8', xml_declaration=True)
