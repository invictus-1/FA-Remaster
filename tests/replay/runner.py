"""Reuse portable test supervision for isolated simulation captures."""
from dataclasses import asdict, dataclass, field
import hashlib
import json
import math
from pathlib import Path
import subprocess
import time
import uuid

from framework import Runner, TestCase
from framework.adapters import PlatformAdapter
from framework.cases import identifier, source_file, strings
from framework.protocol import evaluate, unique_object, read_events
from framework.runner import TestResult, lua_string
from .records import compare, inspect

ROOT = Path(__file__).resolve().parents[2]
COLLECTOR = Path(__file__).with_name('capture.lua')
HOOK_PATH = '/session-replay-hook'


def collector_digest():
    return hashlib.sha256(COLLECTOR.read_bytes()).hexdigest()


def lua_value(value):
    if value is None:
        return 'nil'
    if type(value) is bool:
        return 'true' if value else 'false'
    if type(value) is str:
        return lua_string(value)
    if type(value) in (int, float) and math.isfinite(value):
        return str(value)
    if type(value) is list:
        return '{' + ', '.join(lua_value(item) for item in value) + '}'
    if type(value) is dict and all(type(key) is str for key in value):
        return '{' + ', '.join('[' + lua_string(key) + '] = ' + lua_value(value[key]) for key in sorted(value)) + '}'
    raise ValueError('simulation setup input must contain finite JSON values')


def bundle(folder, config):
    folder = Path(folder)
    folder.mkdir(parents=True, exist_ok=True)
    script = folder / 'testing/session_capture.lua'
    hook = folder / 'session-replay-hook/lua/simInit.lua'
    script.parent.mkdir(parents=True, exist_ok=True)
    hook.parent.mkdir(parents=True, exist_ok=True)
    script.write_bytes(COLLECTOR.read_bytes())
    values = ', '.join(key + ' = ' + lua_value(value)
                       for key, value in sorted(config.items()))
    hook.write_text("local begin = BeginSession\nfunction BeginSession()\n"
                    "    begin()\n    import('/testing/session_capture.lua').start({" + values + "})\nend\n",
                    encoding='utf-8')


def isolated_init(init, folder, capture_folder):
    init, folder = Path(init).resolve(), Path(folder)
    path = folder / 'init.lua'
    path.write_text('InitFileDir = ' + lua_string(init.parent.as_posix()) + '\ndo\n' +
                    init.read_text(encoding='utf-8') + '\nend\n' +
                    'table.insert(path, 1, {dir = ' + lua_string(Path(capture_folder).resolve().as_posix()) +
                    ", mountpoint = '/'})\nhook = hook or {}\ntable.insert(hook, " + lua_string(HOOK_PATH) + ')\n',
                    encoding='utf-8')
    return path


class IsolatedAdapter(PlatformAdapter):
    def __init__(self, native=None):
        native = native or PlatformAdapter.detect()
        self.native = native
        self.suffix = native.suffix
        self.loader_variable = native.loader_variable
        self.path_separator = native.path_separator

    def environment(self, executable, extra=None):
        env = self.native.environment(executable)
        env = {key: value for key, value in env.items() if not key.startswith('MOHO64_')}
        env['MOHO64_FIXES'] = '0'
        if extra:
            env.update(extra)
        return env


@dataclass
class ReplayCase:
    name: str
    oracle: str
    replay: str = ''
    session: str = ''
    session_info: str = ''
    sim_setup: str = ''
    setup_input: dict = field(default_factory=dict)
    oracle_format: str = 'session-replay'
    seed: object = None
    stream: str = ''
    mods: str = ''
    maps: list = field(default_factory=list)
    timeout: float = 300
    abs_tol: float = 0
    rel_tol: float = 0

    def __post_init__(self):
        identifier(self.name, 'replay test name')
        for key in ('oracle', 'replay', 'session', 'session_info', 'sim_setup', 'stream', 'mods'):
            if type(getattr(self, key)) is not str or '\0' in getattr(self, key):
                raise ValueError(f'{key} must be a path string')
        if not self.oracle or sum(bool(value) for value in (self.replay, self.session, self.session_info)) != 1:
            raise ValueError('each case needs an oracle and exactly one session input')
        if self.oracle_format not in ('session-replay', 'fa-oracle'):
            raise ValueError('unknown original capture format')
        if self.session or self.session_info:
            if type(self.seed) is not int or not 0 <= self.seed <= 0xffffffff:
                raise ValueError('Lua sessions require an explicit uint32 seed')
        elif self.seed is not None or self.stream:
            raise ValueError('seed and stream belong to Lua session cases')
        if self.stream and not self.session:
            raise ValueError('stream requires a GameConfig session')
        if type(self.setup_input) is not dict or (self.setup_input and not self.sim_setup):
            raise ValueError('setup_input requires a simulation setup script')
        lua_value(self.setup_input)
        strings(self.maps, 'maps')
        for key in ('timeout', 'abs_tol', 'rel_tol'):
            value = getattr(self, key)
            if type(value) not in (int, float) or not math.isfinite(value) or value < 0 or (key == 'timeout' and not value):
                raise ValueError(f'{key} must be a finite {"positive" if key == "timeout" else "nonnegative"} number')


def load_suite(path):
    raw = json.loads(Path(path).read_text(encoding='utf-8'), object_pairs_hook=unique_object)
    if type(raw) is not dict or set(raw) != {'tests'} or type(raw['tests']) is not list or not raw['tests']:
        raise ValueError('replay suite must contain a nonempty tests list')
    cases = [ReplayCase(**entry) for entry in raw['tests']]
    if len({case.name for case in cases}) != len(cases):
        raise ValueError('duplicate replay test names')
    return cases


class ReplayRunner:
    def __init__(self, build_dir, init, output, fixture_dir):
        self.runtime = Runner(ROOT, build_dir, init, output, adapter=IsolatedAdapter())
        self.adapter = self.runtime.adapter
        self.fixtures = Path(fixture_dir).resolve()

    @classmethod
    def using(cls, runtime):
        runner = cls.__new__(cls)
        runner.runtime = runtime
        runner.adapter = IsolatedAdapter(runtime.adapter)
        runner.fixtures = runtime.fixture_root
        return runner

    @property
    def output(self):
        return self.runtime.output

    def input(self, value):
        return source_file(self.fixtures, value)

    def capture(self, case, header, folder):
        """Run one candidate. The original baseline is never opened for writing."""
        config = {key: header[key] for key in ('case', 'start_tick', 'end_tick', 'interval', 'collector')}
        config.update(role='candidate', capture=uuid.uuid4().hex)
        capture_folder = folder / 'capture'
        if case.sim_setup:
            config.update(setup_script='/testing/session_setup.lua', setup_input=case.setup_input)
        bundle(capture_folder, config)
        if case.sim_setup:
            (capture_folder / 'testing/session_setup.lua').write_bytes(self.input(case.sim_setup).read_bytes())
        init = isolated_init(self.runtime.init, folder, capture_folder)
        events = folder / 'events.jsonl'
        log = folder / 'host.log'
        command = [self.adapter.executable(self.runtime.build_dir, 'moho64_test'),
                   '--test-events', events, '--test-run', self.runtime.run_id, '--test-case', case.name,
                   '--init', init, '--ticks', str(header['end_tick'])]
        if case.replay:
            command += ['--sim', self.input(case.replay)]
        elif case.session_info:
            command += ['--session-info', self.input(case.session_info), '--seed', str(case.seed)]
        else:
            command += ['--session', self.input(case.session), '--seed', str(case.seed)]
            if case.stream:
                command += ['--stream', self.input(case.stream)]
        if case.mods:
            command += ['--mods', self.input(case.mods)]
        for maps in case.maps:
            path = (self.fixtures / maps).resolve()
            if not path.is_dir():
                raise ValueError(f'map folder does not exist: {path}')
            command += ['--maps', path]
        for name in ('LOCAL_APPDATA', 'APPDATA', 'PERSONAL', 'COMMON_APPDATA', 'COMMON_DOCUMENTS'):
            path = folder / 'user' / name
            path.mkdir(parents=True)
            command += ['--folder', name + '=' + str(path)]
        (folder / 'command.json').write_text(json.dumps([str(arg) for arg in command], indent=2) + '\n', encoding='utf-8')
        returncode = self.adapter.run(command, folder, log, case.timeout)
        contract = TestCase(case.name, host='sim', completion=False,
                            required=[f'moho64: ran {header["end_tick"]} ticks'] if header['end_tick'] else ['sim start-up ok'])
        with log.open(encoding='utf-8') as source:
            completion = ''.join(line for line in source if line.startswith(('info: moho64: ran ', 'info: moho64: sim start-up ')))
        status, reason, _ = evaluate(completion, returncode, contract, read_events(events), self.runtime.run_id)
        if status != 'passed':
            raise ValueError('engine run failed: ' + reason)
        from .records import collect
        actual = folder / 'actual.jsonl'
        collect(log, actual)
        captured = inspect(actual, 'candidate')
        if captured['capture'] != config['capture']:
            raise ValueError('candidate capture belongs to another run')
        return actual, returncode

    def run(self, case):
        folder = self.output / case.name
        folder.mkdir()
        log = folder / 'host.log'
        start = time.monotonic()
        returncode = None
        try:
            oracle = self.input(case.oracle)
            if case.oracle_format == 'fa-oracle':
                from .legacy import inspect_legacy
                legacy = inspect_legacy(oracle)
                header = dict(case=case.name, collector=collector_digest(), start_tick=legacy['requested_ticks'][0],
                              end_tick=legacy['requested_ticks'][-1], interval=1)
            else:
                header = inspect(oracle, 'original')
            if header['case'] != case.name:
                raise ValueError('baseline belongs to another case')
            if header['collector'] != collector_digest():
                raise ValueError('collector changed; capture a new original baseline')
            actual, returncode = self.capture(case, header, folder)
            if case.oracle_format == 'fa-oracle':
                from .legacy import compare_legacy
                comparison = compare_legacy(oracle, actual, case.abs_tol, case.rel_tol)
            else:
                comparison = compare(oracle, actual, case.abs_tol, case.rel_tol)
            (folder / 'comparison.json').write_text(json.dumps(asdict(comparison), indent=2) + '\n', encoding='utf-8')
            status, reason, checks = ('passed' if comparison.passed else 'failed'), comparison.reason, comparison.checks
        except subprocess.TimeoutExpired:
            status, reason, checks = 'failed', f'timed out after {case.timeout}s', 0
        except (OSError, RuntimeError, ValueError) as error:
            status, reason, checks = 'failed', str(error), 0
        result = TestResult(case.name, status, time.monotonic() - start, str(log), reason, checks, returncode,
                            str(folder / 'events.jsonl'))
        self.runtime.results.append(result)
        self.runtime.report()
        return result
