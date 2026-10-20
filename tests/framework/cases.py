"""Validated, platform independent test specifications."""
from dataclasses import dataclass, field, replace
import math
from pathlib import Path
import re


def identifier(value, label):
    if type(value) is not str or not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9_.-]*', value):
        raise ValueError(f'{label} must be a simple filename')
    reserved = {'CON', 'PRN', 'AUX', 'NUL'} | {f'{prefix}{n}' for prefix in ('COM', 'LPT') for n in range(1, 10)}
    if value.endswith('.') or value.split('.', 1)[0].upper() in reserved:
        raise ValueError(f'{label} must be portable across filesystems')


def strings(value, label):
    if type(value) is not list or any(type(item) is not str or '\0' in item for item in value):
        raise ValueError(f'{label} must be a list of strings without NUL')


def source_file(root, name):
    path = Path(name)
    if not path.is_absolute():
        path = Path(root) / path
    if not path.is_file():
        raise ValueError(f'test input file does not exist: {path}')
    return path.resolve()


@dataclass
class TestStage:
    name: str
    script: str
    context: str = 'frontend'
    terminal: bool = field(default=True, init=False, repr=False)

    def __post_init__(self):
        identifier(self.name, 'stage name')
        if type(self.script) is not str or not self.script or '\0' in self.script:
            raise ValueError('stage script must be a nonempty path')
        if type(self.context) is not str or self.context not in ('frontend', 'game'):
            raise ValueError('stage context must be frontend or game')


@dataclass
class TestCase:
    name: str
    script: str = ''
    host: str = 'ui'
    args: list = field(default_factory=list)
    required: list = field(default_factory=list)
    timeout: float = 180
    frames: int = 120
    stage: str = 'setup'
    width: int = 1920
    height: int = 1080
    session: str = ''
    game_script: bool = False
    world: bool = False
    window: bool = True
    setup: str = ''
    stages: list = field(default_factory=list)
    fixtures: dict = field(default_factory=dict)
    operation: str = ''
    screenshot: bool = False
    capture_window: bool = False
    environment: dict = field(default_factory=dict)
    completion: bool = True
    capabilities: list = field(default_factory=list)

    def __post_init__(self):
        identifier(self.name, 'test name')
        for name in ('script', 'host', 'stage', 'session', 'setup', 'operation'):
            value = getattr(self, name)
            if type(value) is not str or '\0' in value:
                raise ValueError(f'{name} must be a string without NUL')
        for name in ('game_script', 'world', 'window', 'screenshot', 'capture_window', 'completion'):
            if type(getattr(self, name)) is not bool:
                raise ValueError(f'{name} must be a boolean')
        for name in ('frames', 'width', 'height'):
            value = getattr(self, name)
            if type(value) is not int or value <= 0 or value > 2147483647:
                raise ValueError(f'{name} must be a positive integer')
        if self.width > 32767 or self.height > 32767:
            raise ValueError('dimensions exceed the portable input range')
        if type(self.timeout) not in (int, float) or not math.isfinite(self.timeout) or self.timeout <= 0:
            raise ValueError('timeout must be a finite positive number')
        for name in ('args', 'required', 'capabilities'):
            strings(getattr(self, name), name)
        for name in ('environment', 'fixtures'):
            value = getattr(self, name)
            if type(value) is not dict or any(type(k) is not str or type(v) is not str or not k or
                                              '\0' in k or '\0' in v for k, v in value.items()):
                raise ValueError(f'{name} must map nonempty strings to strings without NUL')
        if any('=' in key for key in self.environment):
            raise ValueError('environment names cannot contain =')
        if self.host not in ('ui', 'sim', 'viewer'):
            raise ValueError('test host must be ui, sim, or viewer')
        if self.stage not in ('userinit', 'setup', 'frontend'):
            raise ValueError('startup stage must be userinit, setup, or frontend')
        if type(self.stages) is not list:
            raise ValueError('stages must be a list')
        self.stages = [TestStage(**entry) if type(entry) is dict else entry for entry in self.stages]
        if any(type(entry) is not TestStage for entry in self.stages):
            raise ValueError('stages must contain stage specifications')
        if self.kind == 'lua':
            if self.host != 'ui' or not (self.script or self.stages) or not self.completion:
                raise ValueError('Lua tests require the UI host, a script or stages, and completion')
        elif self.script or self.setup or self.stages or self.game_script or self.completion or not self.required:
            raise ValueError('command tests require log checks and completion=false, without Lua scripts')
        if self.game_script and not self.session:
            raise ValueError('game_script requires a session; use explicit stages for frontend-launched games')
        if self.stages and (self.script or self.setup or self.game_script):
            raise ValueError('explicit stages cannot be combined with script, setup, or game_script')
        if self.setup and not self.script:
            raise ValueError('setup requires a test script')
        plan = self.plan()
        if plan:
            if len({entry.name for entry in plan}) != len(plan):
                raise ValueError('stage names must be unique')
            contexts = [entry.context for entry in plan]
            if contexts != sorted(contexts, key=lambda item: item == 'game'):
                raise ValueError('frontend stages must precede game stages')
            if contexts[0] == 'game' and not self.session:
                raise ValueError('a game stage needs a session or a preceding frontend stage')
        operations = {'rules': 'sim', 'graphics_clear': 'viewer', 'graphics_pipelines': 'viewer'}
        if self.operation and operations.get(self.operation) != self.host:
            raise ValueError('operation is unknown or incompatible with the selected host')
        if self.capture_window and self.host != 'ui':
            raise ValueError('window capture requires the UI host')
        if self.capture_window and (not self.world or not self.window):
            raise ValueError('window capture requires world rendering and a window')
        if self.screenshot and self.host == 'sim':
            raise ValueError('screenshots require the UI or viewer host')
        if self.host == 'viewer' and not self.window:
            raise ValueError('the viewer requires a window')
        known = {'input', 'clipboard', 'resize', 'simulation', 'rendering', 'window_input'}
        if set(self.capabilities) - known:
            raise ValueError('unknown capabilities: ' + ', '.join(sorted(set(self.capabilities) - known)))
        reserved = {'--init', '--prefs', '--import-prefs', '--own', '--size', '--stage', '--frames', '--lua',
                    '--game-lua', '--session', '--window', '--no-world', '--no-backdrop', '--no-sound',
                    '--test-events', '--test-run', '--test-case', '--log', '--quiet',
                    '--screenshot', '--window-capture'}
        if any(arg.split('=', 1)[0] in reserved for arg in self.args):
            raise ValueError('args cannot override runner-owned options; use the corresponding case fields')
        for pattern in self.required:
            re.compile(pattern)

    @property
    def kind(self):
        return 'lua' if self.script or self.stages else 'command'

    def plan(self):
        if self.stages:
            stages = [replace(stage) for stage in self.stages]
        elif self.script:
            stages = [TestStage('prepare', self.setup)] if self.setup else []
            stages += [TestStage('execute', self.script, 'game' if self.game_script else 'frontend')]
        else:
            return []
        for position, stage in enumerate(stages):
            stage.terminal = position == len(stages) - 1
        return stages
