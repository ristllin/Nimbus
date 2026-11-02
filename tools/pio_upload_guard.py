"""platformio extra_scripts - refuse `pio run -t upload` on a forbidden board (tools/bench_guard.py).

With no --upload-port PlatformIO picks whatever port it finds, so an upload with no port is refused while a
forbidden board is on the bus. Nothing here opens a port.
"""

import importlib.util
import os

Import("env")  # noqa: F821  (platformio construction environment)


def _load_guard():
    path = os.path.join(env["PROJECT_DIR"], "tools", "bench_guard.py")  # noqa: F821
    spec = importlib.util.spec_from_file_location("bench_guard", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def refuse_forbidden_upload(target, source, env):  # noqa: ARG001 - the SCons action signature
    """A SCons pre-action: a non-zero return stops the upload before anything touches the port."""
    guard = _load_guard()
    port = env.subst("$UPLOAD_PORT")
    try:
        if port:
            guard.check_port(port, "pio upload")
        elif guard.forbidden_on_bus():
            raise guard.ForbiddenDevice(
                "pio upload: refused, a forbidden board is on the bus and no --upload-port was given."
            )
    except guard.ForbiddenDevice as exc:
        print(str(exc))
        return 1
    return 0


env.AddPreAction("upload", refuse_forbidden_upload)  # noqa: F821
