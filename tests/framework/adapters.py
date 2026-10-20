"""Compatibility layer for executable discovery, loaders, and process execution."""
import os
import signal
from pathlib import Path
import subprocess
import sys


class PlatformAdapter:
    suffix = ''
    loader_variable = None
    path_separator = ':'

    def operation_args(self, operation):
        operations = {'rules': ['--rules'], 'graphics_clear': ['--vulkan-clear'],
                      'graphics_pipelines': ['--gfx-check']}
        if operation not in operations:
            raise ValueError(f'unknown test operation: {operation}')
        return operations[operation]

    @classmethod
    def detect(cls, platform='auto'):
        if platform == 'auto':
            if sys.platform == 'win32':
                platform = 'windows'
            elif sys.platform.startswith('linux'):
                platform = 'linux'
            elif sys.platform == 'darwin':
                platform = 'macos'
            else:
                raise RuntimeError(f'No test process adapter for {sys.platform}')
        if platform == 'windows':
            return WindowsAdapter()
        if platform == 'linux':
            return LinuxAdapter()
        if platform == 'macos':
            return MacOSAdapter()
        raise ValueError(f'Unknown test platform: {platform}')

    def executable(self, build_dir, name):
        build_dir = Path(build_dir).resolve()
        filename = name + self.suffix
        executable = build_dir / filename
        if executable.is_file():
            return executable
        raise RuntimeError(f'{filename} not found in {build_dir}; build the requested host first')

    def environment(self, executable, extra=None):
        env = dict(os.environ)
        if extra:
            env.update(extra)
        if self.loader_variable:
            folder = str(Path(executable).parent)
            old = env.get(self.loader_variable, '')
            env[self.loader_variable] = folder + (self.path_separator + old if old else '')
        return env

    def run(self, command, cwd, log, timeout, env=None):
        # argv is passed directly. Paths containing spaces never become shell commands.
        with Path(log).open('w', encoding='utf-8') as output:
            job = WindowsJob() if os.name == 'nt' else None
            process = None
            try:
                process = subprocess.Popen([str(arg) for arg in command], cwd=cwd, stdin=subprocess.DEVNULL,
                                           stdout=output, stderr=subprocess.STDOUT,
                                           start_new_session=os.name != 'nt',
                                           env=self.environment(command[0], env))
                if job:
                    job.assign(process)
                return process.wait(timeout=timeout)
            finally:
                if job:
                    job.close()
                if process:
                    if os.name != 'nt':
                        try:
                            os.killpg(process.pid, signal.SIGKILL)
                        except ProcessLookupError:
                            pass
                    elif process.poll() is None:
                        process.kill()
                    process.wait()


class WindowsJob:
    """Closing the job releases all assigned processes and their descendants."""
    def __init__(self):
        import ctypes
        from ctypes import wintypes

        class Limits(ctypes.Structure):
            _fields_ = [('process_time', ctypes.c_int64), ('job_time', ctypes.c_int64),
                        ('flags', wintypes.DWORD), ('minimum_working_set', ctypes.c_size_t),
                        ('maximum_working_set', ctypes.c_size_t), ('active_process_limit', wintypes.DWORD),
                        ('affinity', ctypes.c_size_t), ('priority', wintypes.DWORD), ('scheduling', wintypes.DWORD)]

        class Counters(ctypes.Structure):
            _fields_ = [(name, ctypes.c_uint64) for name in
                        ('read_operations', 'write_operations', 'other_operations', 'read_bytes', 'write_bytes', 'other_bytes')]

        class ExtendedLimits(ctypes.Structure):
            _fields_ = [('basic', Limits), ('io', Counters), ('process_memory', ctypes.c_size_t),
                        ('job_memory', ctypes.c_size_t), ('peak_process_memory', ctypes.c_size_t), ('peak_job_memory', ctypes.c_size_t)]

        self.kernel = ctypes.WinDLL('kernel32', use_last_error=True)
        self.kernel.CreateJobObjectW.argtypes = [ctypes.c_void_p, wintypes.LPCWSTR]
        self.kernel.CreateJobObjectW.restype = wintypes.HANDLE
        self.kernel.SetInformationJobObject.argtypes = [wintypes.HANDLE, ctypes.c_int, ctypes.c_void_p, wintypes.DWORD]
        self.kernel.AssignProcessToJobObject.argtypes = [wintypes.HANDLE, wintypes.HANDLE]
        self.kernel.CloseHandle.argtypes = [wintypes.HANDLE]
        self.handle = self.kernel.CreateJobObjectW(None, None)
        if not self.handle:
            raise ctypes.WinError(ctypes.get_last_error())
        limits = ExtendedLimits()
        limits.basic.flags = 0x2000  # JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
        if not self.kernel.SetInformationJobObject(self.handle, 9, ctypes.byref(limits), ctypes.sizeof(limits)):
            error = ctypes.WinError(ctypes.get_last_error())
            self.close()
            raise error

    def assign(self, process):
        import ctypes
        if not self.kernel.AssignProcessToJobObject(self.handle, int(process._handle)):
            raise ctypes.WinError(ctypes.get_last_error())

    def close(self):
        if self.handle:
            self.kernel.CloseHandle(self.handle)
            self.handle = None


class WindowsAdapter(PlatformAdapter):
    suffix = '.exe'
    loader_variable = 'PATH'
    path_separator = ';'


class LinuxAdapter(PlatformAdapter):
    loader_variable = 'LD_LIBRARY_PATH'


class MacOSAdapter(PlatformAdapter):
    loader_variable = 'DYLD_LIBRARY_PATH'
