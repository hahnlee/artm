"""Run a subprocess with deterministic process-group timeout cleanup."""

from __future__ import annotations

import os
import signal
import subprocess
from typing import Any, Sequence


# A timed-out ART invocation gets a short, graceful shutdown window before the
# whole process group is force-killed.  Keeping this at the runner boundary is
# important: subprocess.run() only waits for its direct child and can leave a
# darwin-art-host descendant behind when that child times out.
TERMINATE_GRACE_SECONDS = 1.0


def _signal_group(process: subprocess.Popen[Any], signum: int) -> None:
    """Signal the session/process group, tolerating an already-gone group."""
    if os.name == "posix":
        try:
            os.killpg(process.pid, signum)
            return
        except ProcessLookupError:
            return
    # The ART runner currently targets Darwin, but retain a useful fallback
    # for callers importing this helper on platforms without process groups.
    if process.poll() is None:
        process.send_signal(signum)


def run_process_group(
    arguments: Sequence[str],
    *,
    check: bool = False,
    env: dict[str, str] | None = None,
    stdout: Any = None,
    stderr: Any = None,
    timeout: float | None = None,
    cwd: os.PathLike[str] | str | None = None,
) -> subprocess.CompletedProcess[Any]:
    """Run *arguments* in a new session and reap every timed-out descendant.

    A timeout first sends SIGTERM to the complete process group, waits briefly
    for normal cleanup, then sends SIGKILL and waits without a timeout.  The
    final wait is unconditional, so this helper never returns with a direct
    child (or an inherited pipe held by one of its descendants) unreaped.
    """
    process = subprocess.Popen(
        list(arguments),
        env=env,
        stdout=stdout,
        stderr=stderr,
        cwd=cwd,
        start_new_session=True,
    )
    try:
        output, error = process.communicate(timeout=timeout)
    except subprocess.TimeoutExpired as original_timeout:
        _signal_group(process, signal.SIGTERM)
        try:
            output, error = process.communicate(
                timeout=TERMINATE_GRACE_SECONDS)
        except subprocess.TimeoutExpired:
            _signal_group(process, signal.SIGKILL)
            output, error = process.communicate()
        raise subprocess.TimeoutExpired(
            list(arguments), timeout, output=output, stderr=error) \
            from original_timeout

    completed = subprocess.CompletedProcess(
        list(arguments), process.returncode, output, error)
    if check and completed.returncode:
        raise subprocess.CalledProcessError(
            completed.returncode,
            completed.args,
            output=completed.stdout,
            stderr=completed.stderr,
        )
    return completed
