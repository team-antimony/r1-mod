"""Keep Frida's temp files from piling up in %TEMP%.

Every Frida run on Windows unpacks its helpers (and, on older builds, a 25-35 MB agent DLL per
attach) into fresh %TEMP%\\frida-<hex> folders and leaves some of them behind, even after a clean
frida.shutdown(). The polling loop in native_test.ps1 turned that into 61 GB in a week.

Import this module before `import frida`. It gives the process its own temp folder,
%TEMP%\\r1-mod-frida\\<pid>, removes it on exit, and on start removes the folders of earlier runs
that were killed before they could clean up.

A process started through Frida inherits the redirected TEMP. Pass env=fridatmp.ENV to
device.spawn() so it gets the normal one.
"""
import atexit, os, shutil, sys, tempfile, time
from pathlib import Path

ENV = dict(os.environ)                      # the environment before the redirect
ROOT = Path(tempfile.gettempdir()).resolve() / 'r1-mod-frida'
OWN = ROOT / str(os.getpid())


def _end_helpers():
    """End the helper processes Frida started from OWN.

    One of them (idund.exe in Frida 17) outlives frida.shutdown() and even this process by about
    30 seconds, keeping its exe locked and holding our stdout open, which stalls `... | Select`.
    """
    import ctypes
    from ctypes import wintypes as w
    k32 = ctypes.WinDLL('kernel32')
    k32.OpenProcess.restype = w.HANDLE
    k32.OpenProcess.argtypes = (w.DWORD, w.BOOL, w.DWORD)
    k32.QueryFullProcessImageNameW.argtypes = (w.HANDLE, w.DWORD, w.LPWSTR, ctypes.POINTER(w.DWORD))
    k32.TerminateProcess.argtypes = (w.HANDLE, w.UINT)
    k32.CloseHandle.argtypes = (w.HANDLE,)
    pids = (w.DWORD * 16384)()
    size = w.DWORD()
    k32.K32EnumProcesses(pids, ctypes.sizeof(pids), ctypes.byref(size))
    prefix = str(OWN).lower() + os.sep
    for pid in pids[:size.value // ctypes.sizeof(w.DWORD)]:
        handle = k32.OpenProcess(0x1001, False, pid)    # QUERY_LIMITED_INFORMATION | TERMINATE
        if not handle:
            continue
        path = ctypes.create_unicode_buffer(1024)
        length = w.DWORD(1024)
        if k32.QueryFullProcessImageNameW(handle, 0, path, ctypes.byref(length)) and path.value.lower().startswith(prefix):
            k32.TerminateProcess(handle, 0)
        k32.CloseHandle(handle)


def _remove(folder):
    for _ in range(30):                     # the helper takes a moment to exit and release its exe
        shutil.rmtree(folder, ignore_errors=True)
        if not folder.exists():
            return
        time.sleep(0.1)


def _sweep():
    """Remove the folders of runs that are gone. A live run holds its `alive` file open."""
    if not ROOT.is_dir():
        return
    for folder in ROOT.iterdir():
        if folder == OWN:
            continue
        try:
            (folder / 'alive').unlink(missing_ok=True)
        except OSError:
            continue
        shutil.rmtree(folder, ignore_errors=True)


def _claim():
    for _ in range(5):                      # a concurrent sweep can remove the folder before `alive` exists
        try:
            OWN.mkdir(parents=True, exist_ok=True)
            return open(OWN / 'alive', 'w')
        except OSError:
            time.sleep(0.05)
    raise RuntimeError(f'cannot create {OWN}')


def _cleanup():
    frida = sys.modules.get('frida')
    if frida is not None:
        try:
            frida.shutdown()                # stops most helpers, which otherwise keep their exes locked
        except Exception:
            pass
    try:
        _end_helpers()
    except Exception:
        pass                                # whatever stays locked is swept by the next run
    _alive.close()
    _remove(OWN)


_sweep()
_alive = _claim()
os.environ['TMP'] = os.environ['TEMP'] = str(OWN)
atexit.register(_cleanup)
