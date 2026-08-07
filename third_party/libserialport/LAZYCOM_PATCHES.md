# LazyCom Local Patches

The official libserialport 0.1.2 source archive has 0.1.2 in `configure.ac`
and `configure`, but its generated public `libserialport.h` still contains the
0.1.1 package and `1:0:1` ABI version macros. This conflicts with generated
`config.h`, emits macro redefinition warnings, and makes the runtime version
API report stale values.

LazyCom updates only these generated macros to the values already declared by
the 0.1.2 build metadata:

- package version `0.1.2`
- libtool ABI version `1:1:1`

The upstream archive hash remains recorded in
`include/dependencies/DEPENDENCIES.lock`. No API declarations are changed by
this patch.

On Linux, upstream 0.1.2 also rejects valid PTY paths in
`sp_get_port_by_name()` because `/dev/pts/*` has no `/sys/class/tty` metadata.
LazyCom retains the normal metadata lookup and falls back to accepting the path
only when `stat()` confirms it is a character device. This permits explicit PTY
and non-sysfs character-device paths without allowing regular files or changing
serial I/O or handle ownership.

Linux PTYs also return `ENOTTY` for modem-control ioctls. The bundled build
treats that specific result as an unavailable RTS/DTR state and tolerates it
when applying RTS/DTR levels, while preserving termios configuration and all
other ioctl errors. This makes `sp_open()` and no-flow-control configuration
usable for PTY integration tests without relaxing behavior for real adapters.

The POSIX `sp_open()` path now closes and invalidates its newly opened file
descriptor when either `flock(LOCK_EX | LOCK_NB)` or `ioctl(TIOCEXCL)` fails.
The original failure `errno` is preserved for `sp_last_error_code()` and the
descriptor is never left owned by a port for which `sp_open()` returned an
error.

`sp_set_debug_handler()` installs the replacement before tracing the setter
call. LazyCom can therefore install its errno-preserving silent handler before
any serial operation without one final message reaching stderr when
`LIBSERIALPORT_DEBUG` is set.

## LazyCom vendored-tree trimming

LazyCom keeps only the files required to build libserialport on Linux from the
checked-in `configure`/`Makefile.in`. Removed from the upstream 0.1.2 archive:

- `examples/` (only listed in `EXTRA_DIST`)
- `test_timing.c` (`make check` only)
- `Doxyfile`
- `macosx.c`, `windows.c`, `freebsd.c` (automake conditionals not active on
  Linux; the generated Linux Makefile never references them)

`serialport.c`, `timing.c`, `linux.c`, `linux_termios.c/.h`,
`libserialport_internal.h`, the autotools build files, and all license/provenance
files remain unchanged. Re-running `autoreconf` or `make dist` against this tree
is not supported.
