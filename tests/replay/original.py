#!/usr/bin/env python3
"""Capture a long original-game session using an existing approved Steam/Wine lab."""
import argparse
import fcntl
import gzip
import json
import math
from pathlib import Path
import shutil
import sys
import time
import uuid

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tests'))
from framework.runner import lua_string
from framework.cases import identifier
from replay.legacy import OriginalChunks, inspect_legacy


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--lab', type=Path, required=True)
    parser.add_argument('--name', default='original-battle')
    parser.add_argument('--ticks', type=int, default=6000)
    parser.add_argument('--timeout', type=float, default=900)
    parser.add_argument('--map', default='SCMP_001')
    parser.add_argument('--seed', type=int, default=1)
    parser.add_argument('--through-steam', action='store_true', help='launch with the installed Steam client')
    args = parser.parse_args()
    lab = args.lab.resolve()
    sys.path.insert(0, str(lab / 'tools/fa-loop'))
    import runtimes
    from common import runner
    import zipfile
    identifier(args.name, 'original case name')
    if not 1 <= args.ticks <= 2147483647 or not 0 <= args.seed <= 0xffffffff or not math.isfinite(args.timeout) or args.timeout <= 0:
        parser.error('ticks, timeout and seed are out of range')
    task = runtimes.windows.STATE / ('session-regression-' + uuid.uuid4().hex[:12])
    task.mkdir()
    output = task / 'original'
    output.mkdir()
    case = dict(id=args.name, map=args.map, seed=args.seed, factions=[1], ticks=list(range(1, args.ticks + 1)),
                options=dict(Victory='sandbox', FogOfWar='explored', UnitCap='500'), input=dict(action='battle', end_tick=args.ticks))
    request = None
    print(f'Original capture: {task}', flush=True)
    try:
        with (runtimes.windows.STATE / 'game-execution.lock').open('a') as lock:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            game = lab / '.fa-runner/game'
            mounted = runtimes.prepare_overlay(game, output, case, args.name, 'bin/SupComDataPath.lua', 'gamedata/mohodata.scd')
            shutil.copyfile(ROOT / 'tests/replay/fixtures/battle.lua', output / 'overlay/fa-oracle/setup.lua')
            config = dict(run_id=args.name, ticks=case['ticks'], setup_script='/fa-oracle/setup.lua', input=case['input'])
            (output / 'overlay/fa-oracle/config.lua').write_text('Config = ' + runner.lua_value(config) + '\n')
            exporter = output / 'overlay/fa-oracle/oracle.lua'
            source = exporter.read_text()
            source = source.replace('local function Emit(record)', 'local transport_sequence = 0\nlocal function Emit(record)')
            source = source.replace("    LOG('FA_ORACLE ' .. Encode(record))", """
    transport_sequence = transport_sequence + 1
    local text = Encode(record)
    local count = math.ceil(string.len(text) / 128)
    for part = 1, count do
        LOG('FA_ORACLE_CHUNK ' .. Encode({run_id = config.run_id, sequence = transport_sequence,
            part = part, parts = count, data = string.sub(text, (part - 1) * 128 + 1, part * 128)}))
    end
""")
            exporter.write_text(source)
            launch = output / 'overlay/lua/SinglePlayerLaunch.lua'
            source = launch.read_text()
            export = """
    LOG('FA_SESSION_INFO_BEGIN')
    local text = repr(info)
    for line in string.gfind(text, '[^\\r\\n]+') do LOG('FA_SESSION_INFO ' .. line) end
    LOG('FA_SESSION_INFO_END')
"""
            source = source.replace('    FaLoopOriginalLaunch(info)', export + '    FaLoopOriginalLaunch(info)')
            launch.write_text(source)
            with zipfile.ZipFile(game / 'gamedata/mohodata.scd') as archive:
                user = archive.read('lua/userInit.lua')
            (output / 'overlay/lua/userInit.lua').write_bytes(user + b"\nConExecute('WLD_GameSpeed 10')\n")
            with zipfile.ZipFile(game / 'gamedata/mohodata.scd') as archive:
                sync = archive.read('lua/UserSync.lua')
            (output / 'overlay/lua/UserSync.lua').write_bytes(sync + b"\nlocal original = OnSync\nlocal accelerated = false\nfunction OnSync() original(); if not accelerated then accelerated = true; ConExecute('WLD_GameSpeed 10') end end\n")
            exe = r'C:\Program Files (x86)\Steam\steamapps\common\Supreme Commander Forged Alliance\bin\SupremeCommander.exe'
            installed = runtimes.windows.STATE / 'prefix/drive_c/Program Files (x86)/Steam/steamapps/common/Supreme Commander Forged Alliance/bin/SupremeCommander.exe'
            if runner.sha256(installed) != runner.sha256(game / 'bin/SupremeCommander.exe'):
                raise ValueError('installed original binary differs from the supplied game')
            request = dict(action='launch', exe=exe, init=mounted + '/init_oracle.lua',
                cwd='/state/prefix/drive_c/Program Files (x86)/Steam/steamapps/common/Supreme Commander Forged Alliance/bin',
                wine_log='/state/' + (output / 'wine.log').relative_to(runtimes.windows.STATE).as_posix(),
                argv=['wine', exe, '/nosound', '/init', mounted + '/init_oracle.lua', '/log', mounted + '/game.log',
                      '/map', args.map, '/faction', '1', '/seed', str(args.seed)], wine_debug='-all')
            if args.through_steam:
                request['argv'] = ['wine', r'C:\Program Files (x86)\Steam\steam.exe', '-applaunch', '9420', *request['argv'][2:]]
            (output / 'runtime-request.json').write_text(json.dumps(request, indent=2))
            manifest = dict(origin='original_game', executable_sha256=runner.sha256(installed), case=case,
                            hook_sha256=runner.sha256(lab / 'tools/fa-runner/hook.lua'),
                            exporter_sha256=runner.sha256(exporter),
                            source_exporter_sha256=runner.sha256(lab / 'tools/fa-runner/oracle.lua'))
            (task / 'manifest.json').write_text(json.dumps(manifest, indent=2))
            runtimes.helper(request)
            deadline = time.monotonic() + args.timeout
            last_sequence = 0
            next_status = time.monotonic() + 5
            with (output / 'game.log').open('a'):
                pass
            with (output / 'game.log').open(encoding='latin-1') as log, gzip.open(task / 'oracle.jsonl.gz', 'wt', encoding='utf-8') as oracle:
                session_lines, session_started, complete = [], False, False
                chunks = OriginalChunks(args.name)
                while not complete:
                    offset = log.tell()
                    line = log.readline()
                    if not line or not line.endswith('\n'):
                        log.seek(offset)
                        if time.monotonic() >= deadline:
                            raise TimeoutError('original capture timed out')
                        if time.monotonic() >= next_status:
                            if not runtimes.helper(dict(request, action='status'))['pids']:
                                raise RuntimeError('original game exited before capture completion')
                            next_status = time.monotonic() + 5
                        time.sleep(.1)
                        continue
                    if 'FA_SESSION_INFO_BEGIN' in line:
                        if session_started:
                            raise ValueError('duplicate original launch table')
                        session_started = True
                    elif 'FA_SESSION_INFO_END' in line:
                        (task / 'session-info.lua').write_text('return ' + '\n'.join(session_lines) + '\n', encoding='utf-8')
                    elif 'FA_SESSION_INFO ' in line:
                        session_lines.append(line.partition('FA_SESSION_INFO ')[2].rstrip('\r\n'))
                    elif 'FA_ORACLE_CHUNK ' in line:
                        chunk = json.loads(line.partition('FA_ORACLE_CHUNK ')[2])
                        row = chunks.append(chunk)
                        if row is None:
                            continue
                        if row.get('kind') == 'error':
                            raise ValueError('original Lua exporter failed: ' + row.get('message', ''))
                        oracle.write(json.dumps(row, ensure_ascii=False, allow_nan=False) + '\n')
                        if row.get('kind') == 'snapshot':
                            last_sequence = row['tick']
                            if last_sequence % 600 == 0:
                                print(f'Original: tick {last_sequence}/{args.ticks}', flush=True)
                        complete = row.get('kind') == 'complete'
            inspect_legacy(task / 'oracle.jsonl.gz')
            actions = []
            with (output / 'game.log').open(encoding='latin-1') as source:
                for line in source:
                    if 'FA_REGRESSION_ACTION tick=' in line:
                        actions.append(int(line.partition('FA_REGRESSION_ACTION tick=')[2]))
            if actions != list(range(5, args.ticks - 100 + 1, 600)):
                raise ValueError('original did not execute every configured construction/combat round')
            if not (task / 'session-info.lua').is_file():
                raise ValueError('missing original launch table')
            init = task / 'candidate-init.lua'
            init.write_text('path = {\n' +
                "{dir = " + lua_string(str(game / 'gamedata/*.scd')) + ", mountpoint = '/'},\n" +
                "{dir = " + lua_string(str(game)) + ", mountpoint = '/'},\n}\nhook = {'/schook'}\n")
            suite = dict(tests=[dict(name=args.name, oracle='oracle.jsonl.gz', oracle_format='fa-oracle',
                         session_info='session-info.lua', seed=args.seed, timeout=600,
                         sim_setup='setup.lua', setup_input=case['input'])])
            (task / 'suite.json').write_text(json.dumps(suite, indent=2))
            (task / 'setup.lua').write_bytes((output / 'overlay/fa-oracle/setup.lua').read_bytes())
            print(f'Original completed {last_sequence} ticks. Suite: {task / "suite.json"}', flush=True)
            return 0
    finally:
        if request:
            runtimes.helper(dict(request, action='stop'))


if __name__ == '__main__':
    sys.exit(main())
