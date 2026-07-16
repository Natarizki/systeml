# SystemL

A dinit-inspired service manager with 66-style tree grouping, written in C.

SystemL can run as PID 1 (init) or as a standalone service supervisor. Services are grouped into **trees** — independent collections of services, each running as its own isolated process. A tree crashing or misbehaving never affects services in another tree.

## Concepts

- **Tree**: a named group of services (e.g. `boot`, `default`). Each tree runs as its own `systeml <tree-name>` process, spawned and supervised by PID 1.
- **Service**: a single process definition inside a tree, with dependencies, restart behavior, and one of three types.
- **PID 1 mode**: when `systeml` detects it's running as PID 1, it mounts essential filesystems, spawns one `systeml <tree>` child per tree listed in `pid1.conf`, and supervises them.

## Service types

| Type        | Behavior                                                              |
|-------------|-------------------------------------------------------------------------|
| `process`   | Long-running daemon. SystemL forks and tracks it directly.              |
| `bgprocess` | Self-daemonizing process. SystemL reads its PID from a `pidfile=` after start. |
| `scripted`  | Oneshot script. Runs to completion; exit 0 means started, nonzero means failed. |

## Service file format

Plain `key=value`, one per line, no extension, placed inside a tree directory (e.g. `trees/default/myservice`):
```bash
type=process
command=/usr/bin/myserver
stop-command=/usr/bin/myserver --stop
depends-on=network
auto-restart=yes
```
For `bgprocess`, add `pidfile=/path/to/file.pid`.

## Building

# make 

Produces two binaries: `systeml` (the daemon) and `systemlctl` (the control client).

## Usage

Run a tree as a standalone supervisor:
```bash
./systeml
```
**Control a running tree:**
```bash
./systemlctl  status
./systemlctl  start 
./systemlctl  stop 
./systemlctl  restart 
```
**Manage which trees PID 1 spawns at boot:**
```bash
./systemlctl tree list
./systemlctl tree enable 
./systemlctl tree disable 
./systemlctl tree switch 
```
## Restart behavior

Services with `auto-restart=yes` are restarted automatically on exit, up to 5 times within a 60-second window. Beyond that, the service is marked `failed` and left stopped to avoid restart storms.

## License

[![License: GPL v3](https://img.shields.io/badge/License-GPLv3-blue.svg)](https://www.gnu.org/licenses/gpl-3.0)

or

[![License: GPL v3](https://img.shields.io/badge/License-GPLv3-blue.svg)](LICENSE)

