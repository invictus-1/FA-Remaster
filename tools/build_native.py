"""Shared CMake entry point for native builds from the existing Python build scripts."""
import os
import subprocess

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def build(out, target, jobs, no_debug=False, headless=False, fetch_sdl3=False, sdl_lib=None):
    command = ['cmake', '-S', ROOT, '-B', out,
               '-DCMAKE_BUILD_TYPE=' + ('Release' if no_debug else 'RelWithDebInfo'),
               '-DMOHO64_BUILD_UI=' + ('ON' if target in ('moho64ui', 'moho64ui_test') else 'OFF'),
               '-DMOHO64_BUILD_VIEWER=' + ('ON' if target == 'moho64view' else 'OFF'),
               '-DMOHO64_HEADLESS_UI=' + ('ON' if headless else 'OFF'),
               '-DMOHO64_FETCH_SDL3=' + ('ON' if fetch_sdl3 else 'OFF')]
    if sdl_lib:
        command.append('-DCMAKE_PREFIX_PATH=' + os.path.dirname(os.path.abspath(sdl_lib)))
    if target == 'moho64ui_test':
        command.append('-DBUILD_TESTING=ON')
    subprocess.run(command, check=True)
    subprocess.run(['cmake', '--build', out, '--target', target, '--parallel', str(jobs)], check=True)
