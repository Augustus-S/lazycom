#!/usr/bin/env python3
"""Check that a packaged TUI renders and exits as an unprivileged user."""

import errno
import fcntl
import os
import pty
import select
import signal
import struct
import sys
import tempfile
import termios
import time


def main():
    command = sys.argv[1:]
    if not command:
        raise SystemExit("Usage: smoke-test.py PROGRAM [ARGS...]")
    if os.getuid() == 0:
        os.setgroups([])
        os.setgid(65534)
        os.setuid(65534)

    with tempfile.TemporaryDirectory(prefix="lazycom-smoke-") as directory:
        env = os.environ.copy()
        env.update(TERM="xterm-256color", XDG_CONFIG_HOME=directory + "/config",
                   XDG_STATE_HOME=directory + "/state")
        pid, terminal = pty.fork()
        if pid == 0:
            os.chdir(directory)
            os.execvpe(command[0], command, env)

        fcntl.ioctl(terminal, termios.TIOCSWINSZ, struct.pack("HHHH", 36, 120, 0, 0))
        started = time.monotonic()
        transcript = bytearray()
        quit_sent = False
        reaped = False
        try:
            while time.monotonic() - started < 20:
                if select.select([terminal], [], [], 0.05)[0]:
                    try:
                        transcript.extend(os.read(terminal, 65536))
                    except OSError as error:
                        if error.errno != errno.EIO:
                            raise
                if len(transcript) > 4 * 1024 * 1024:
                    raise RuntimeError("TUI output exceeded smoke-test limit")
                if not quit_sent and b"LazyCom" in transcript and time.monotonic() - started > 1:
                    os.write(terminal, b"q")
                    quit_sent = True
                exited, status = os.waitpid(pid, os.WNOHANG)
                if exited:
                    reaped = True
                    if not quit_sent or os.waitstatus_to_exitcode(status) != 0:
                        raise RuntimeError(f"TUI failed to render or quit: {bytes(transcript[-4096:])!r}")
                    print("TUI rendered and quit successfully as an unprivileged user")
                    return
            raise RuntimeError("TUI did not exit within 20 seconds")
        finally:
            if not reaped:
                os.killpg(pid, signal.SIGKILL)
                os.waitpid(pid, 0)
            os.close(terminal)


if __name__ == "__main__":
    main()
