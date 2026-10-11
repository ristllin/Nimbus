#!/usr/bin/env python3
"""Host-only checks for the USB recovery tool's setup error paths (CUM-493).

On a fresh setup `python3 tools/usb_reset.py` used to die with a traceback: pyusb was
imported unconditionally and the libusb path was hardcoded to Apple Silicon Homebrew,
so Linux, Intel macOS and Windows could never load a backend. These tests drive every
"cannot run here" path and the library discovery with fakes. None of them touches a
real USB bus: the real pyusb is either blocked or replaced by a fake whose bus is empty,
so running this suite on a bench Mac can never reset an attached board."""

from __future__ import annotations

import contextlib
import importlib.util
import io
import os
import subprocess
import sys
import tempfile
import types
import unittest
from pathlib import Path

_PATH = Path(__file__).with_name("usb_reset.py")


def _load():
    spec = importlib.util.spec_from_file_location("usb_reset_under_test", _PATH)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


USB = _load()
_USB_MODULES = ("usb", "usb.core", "usb.util", "usb.backend", "usb.backend.libusb1")
_MISSING = object()


@contextlib.contextmanager
def _usb_modules(modules: dict):
    """Install ``modules`` (name -> module, or None to make the import fail) in
    sys.modules for the duration, then restore whatever was there."""
    saved = {name: sys.modules.get(name, _MISSING) for name in _USB_MODULES}
    for name in _USB_MODULES:
        sys.modules[name] = modules.get(name)
    try:
        yield
    finally:
        for name, module in saved.items():
            if module is _MISSING:
                sys.modules.pop(name, None)
            else:
                sys.modules[name] = module


def _fake_pyusb(backend="backend"):
    """A pyusb stand-in: get_backend records the library it was pointed at and returns
    ``backend``; the bus is always empty, so nothing can ever be reset."""
    calls = {"library": [], "find": []}
    usb = types.ModuleType("usb")
    core = types.ModuleType("usb.core")
    backend_pkg = types.ModuleType("usb.backend")
    libusb1 = types.ModuleType("usb.backend.libusb1")

    def get_backend(find_library):
        calls["library"].append(find_library("usb-1.0"))
        return backend

    def find(**kwargs):
        calls["find"].append(kwargs)
        return []

    libusb1.get_backend = get_backend
    core.find = find
    usb.core, usb.backend, backend_pkg.libusb1 = core, backend_pkg, libusb1
    modules = {"usb": usb, "usb.core": core, "usb.backend": backend_pkg, "usb.backend.libusb1": libusb1}
    return modules, calls


@contextlib.contextmanager
def _patched(**values):
    saved = {name: getattr(USB, name) for name in values}
    for name, value in values.items():
        setattr(USB, name, value)
    try:
        yield
    finally:
        for name, value in saved.items():
            setattr(USB, name, value)


def _run(argv, **patches):
    out, err = io.StringIO(), io.StringIO()
    with _patched(**patches), contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        rc = USB.main(argv)
    return rc, out.getvalue(), err.getvalue()


# ---- unsupported platform: exit 2, no traceback, no pyusb import ------------------


def test_unsupported_platform_exits_2_before_importing_pyusb():
    for platform in ("win32", "cygwin", "freebsd14"):
        with _usb_modules({}):  # pyusb absent: proves the platform check comes first
            rc, out, err = _run([], current_platform=lambda p=platform: p)
        assert rc == 2, platform
        assert "macOS and Linux only" in err and platform in err
        assert "plug it back in" in err
        assert "Traceback" not in err


# ---- pyusb missing: exit 2 with the pip command and this OS's libusb package -------


def test_missing_pyusb_exits_2_with_pip_and_libusb_hint_per_os():
    expected_libusb = {"darwin": "brew install libusb", "linux": "apt install libusb-1.0-0"}
    for platform, hint in expected_libusb.items():
        with _usb_modules({}):
            rc, out, err = _run(["--all"], current_platform=lambda p=platform: p)
        assert rc == 2, platform
        assert "pip install pyusb" in err
        assert f"pipx run {_PATH.resolve()}" in err and "pipx 1.4.2+" in err
        assert hint in err, (platform, err)
        assert "Traceback" not in err


def test_missing_pyusb_from_a_real_launch_is_a_message_not_a_traceback():
    # L1's exact repro: `python3 tools/usb_reset.py` on a Python without pyusb. A blocker
    # package first on PYTHONPATH makes `import usb` fail in the child interpreter.
    if os.name == "nt":
        raise unittest.SkipTest("Windows takes the unsupported-platform path (covered above)")
    with tempfile.TemporaryDirectory() as tmp:
        blocker = Path(tmp) / "usb"
        blocker.mkdir()
        (blocker / "__init__.py").write_text('raise ImportError("pyusb blocked for this test")\n')
        env = {k: v for k, v in os.environ.items() if k != "PYTHONHOME"}
        env["PYTHONPATH"] = tmp
        # --serial names no real board: even if the blocker failed, nothing could be reset.
        command = [sys.executable, str(_PATH), "--serial", "NO-SUCH-BOARD-HOST-TEST"]
        result = subprocess.run(command, env=env, capture_output=True, text=True, timeout=60)
    assert result.returncode == 2, (result.stdout, result.stderr)
    assert "Traceback" not in result.stderr
    assert "pip install pyusb" in result.stderr


def test_help_needs_no_pyusb():
    with _usb_modules({}), contextlib.redirect_stdout(io.StringIO()) as out:
        try:
            USB.main(["--help"])
        except SystemExit as exc:
            assert exc.code == 0
    assert "--skip" in out.getvalue()


# ---- libusb discovery: system search first, Homebrew (both prefixes) as fallback ----


def _not_found(name):
    return None


def test_find_libusb_on_linux_uses_the_system_search():
    found = USB.find_libusb("linux", find_library=lambda name: "libusb-1.0.so.0", exists=lambda p: False)
    assert found == "libusb-1.0.so.0"
    # The Homebrew prefixes are macOS only.
    assert USB.find_libusb("linux", find_library=_not_found, exists=lambda p: True) is None


def test_find_libusb_on_macos_prefers_homebrew_apple_silicon_then_intel():
    apple, intel = USB.HOMEBREW_LIBUSB
    assert USB.find_libusb("darwin", find_library=_not_found, exists=lambda p: p == apple) == apple
    assert USB.find_libusb("darwin", find_library=_not_found, exists=lambda p: p == intel) == intel
    # Both present (an old Intel copy left on an Apple Silicon Mac): the native one wins.
    assert USB.find_libusb("darwin", find_library=_not_found, exists=lambda p: p in (apple, intel)) == apple
    assert USB.find_libusb("darwin", find_library=_not_found, exists=lambda p: False) is None
    # Another absolute install the system search finds (e.g. MacPorts) still works.
    macports = "/opt/local/lib/libusb-1.0.dylib"
    assert USB.find_libusb("darwin", find_library=lambda n: macports, exists=lambda p: p == macports) == macports


def test_find_libusb_on_macos_never_loads_a_library_from_the_current_directory():
    # find_library returns a bare name for a file in the current directory; loading it
    # would run whatever was planted there. It is ignored, and Homebrew's copy is used.
    planted = "libusb-1.0.dylib"
    apple = USB.HOMEBREW_LIBUSB[0]
    assert USB.find_libusb("darwin", find_library=lambda n: planted, exists=lambda p: True) == apple
    assert USB.find_libusb("darwin", find_library=lambda n: planted, exists=lambda p: p == planted) is None


def test_reset_failure_on_linux_permissions_names_the_fix():
    denied = OSError(13, "Access denied (insufficient permissions)")
    assert "udev rule" in USB._reset_failed_hint(denied, "linux")
    assert USB._reset_failed_hint(denied, "darwin") == ""
    assert USB._reset_failed_hint(OSError(5, "I/O error"), "linux") == ""


def test_missing_libusb_exits_2_with_the_os_package():
    for platform, hint in {"darwin": "brew install libusb", "linux": "sudo dnf install libusb1"}.items():
        modules, calls = _fake_pyusb()
        with _usb_modules(modules):
            rc, out, err = _run([], current_platform=lambda p=platform: p, find_libusb=lambda p: None)
        assert rc == 2, platform
        assert "libusb 1.0 library was not found" in err and hint in err
        assert calls["find"] == []  # never reached the bus


def test_unloadable_libusb_exits_2():
    modules, calls = _fake_pyusb(backend=None)  # pyusb could not open the library
    with _usb_modules(modules):
        rc, out, err = _run([], current_platform=lambda: "linux", find_libusb=lambda p: "/opt/broken/libusb.so")
    assert rc == 2
    assert "could not be loaded" in err and "/opt/broken/libusb.so" in err
    assert calls["find"] == []


def test_found_library_is_the_one_handed_to_pyusb_and_the_bus_is_scanned():
    # The happy path up to the bus: the discovered library is what the backend loads,
    # then the (fake, empty) bus is scanned and the usual "no device" result returns 1.
    modules, calls = _fake_pyusb()
    with _usb_modules(modules):
        rc, out, err = _run([], current_platform=lambda: "darwin", find_libusb=lambda p: "/opt/homebrew/lib/x.dylib")
    assert calls["library"] == ["/opt/homebrew/lib/x.dylib"]
    assert calls["find"] == [{"find_all": True, "idVendor": USB.VID, "backend": "backend"}]
    assert rc == 1 and "no device 303a:* on the bus" in out


if __name__ == "__main__":
    for name, fn in sorted(globals().items()):
        if name.startswith("test_") and callable(fn):
            try:
                fn()
            except unittest.SkipTest as skip:
                print(f"skipped {name}: {skip}")
    print("usb_reset: platform gate, pyusb/libusb error paths, Homebrew fallback passed")
