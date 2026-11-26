# minsysmon

A small Linux system monitor for the terminal, built with ncurses. It shows
CPU, memory, swap and optional GPU memory usage, a list of the busiest user
processes, and — optionally — the tail of a log file or the output of a shell
command, all refreshed together.

It reads system counters from `/proc`, so it is Linux-specific.

## Why?

I wanted a minimal system monitor that could be used in tmux cleanly. I use 6
tmux panels to monitor various hosts/logs while I'm making changes, upgrading,
experimenting, etc.

## Features

* Centered hostname and uptime (days/hours/minutes/seconds from `/proc/uptime`).
* CPU usage bar calculated from successive `/proc/stat` snapshots.
* RAM usage bar from `MemTotal`/`MemAvailable` in `/proc/meminfo`.
* Swap usage bar from `SwapTotal`/`SwapFree`.
* Optional VRAM usage bar. NVIDIA GPUs are queried through `nvidia-smi` first;
  if that is unavailable, Vulkan device-local heaps are read via `vulkaninfo`.
  The source is probed once at startup and the VRAM line is omitted entirely
  when no usable source is found.
* Top CPU processes (kernel threads excluded), with usage measured over the
  interval between refreshes rather than over the process lifetime.
* Optional log-file tail or shell-command tail under the process list.
* Press `q` to quit.

## Requirements

* Linux with `/proc`.
* A C compiler and ncurses development files (`ncurses.h`, `libncurses`).
* On some distributions `libtinfo` must be linked separately; the commands
  below include `-ltinfo`. If your system folds tinfo into ncurses, remove it.
* Optional: `nvidia-smi` or `vulkaninfo` in `PATH` for the VRAM line.
* Optional: a recent `tmux` (with `select-layout -E` and pane format variables)
  for the example scripts in `tmux-examples/`.

## Build

```bash
make
```

Or compile directly:

```bash
gcc -O2 -Wall -Wextra minsysmon.c -o minsysmon -lncurses -ltinfo
```

Use `make clean` to remove the compiled binary.

## Install

```bash
make
sudo make install                    # installs to /usr/local/bin/minsysmon
make install PREFIX=$HOME/.local     # or install to a user prefix
```

`DESTDIR` is honored for staged installs.

## Tests

```bash
make test
```

The suite compiles into a temporary directory and runs from there, so it also
works on mounts that disallow execution. A strict `-std=c11 -Werror` build, CLI
validation, and a staged install are always checked. If `tmux` is available,
the suite also verifies TUI rendering, prompt `q` handling, and clean `SIGINT`
exit; otherwise those checks are skipped.

Set `CC`, `LDLIBS`, or `TMPDIR` to override the compiler, linker flags, or
temporary directory used by the tests. If you override `TMPDIR`, point it at a
directory that allows execution.

## Usage

```bash
./minsysmon  # defaults: 5 s delay, 3 processes
./minsysmon -d 1 -n 10  # refresh every second, show 10 processes
./minsysmon -l /var/log/syslog -t 20  # show the last 20 lines of a log
./minsysmon -s 'journalctl -n 10 --no-pager 2>&1'
./minsysmon -h  # full help
```

| Option | Meaning |
| --- | --- |
| `-d`, `--delay <seconds>` | Refresh delay, positive whole seconds (default: 5) |
| `-n`, `--processes <count>` | Number of top processes to show, 1–20 (default: 3) |
| `-l`, `--log <logfile>` | Show the last `-t` lines of a log file |
| `-s`, `--shell <command>` | Run a command on each refresh and show its last `-t` lines |
| `-t`, `--taillines <count>` | Tail length for `-l`/`-s`, 1–100 (default: 10) |
| `-h`, `--help` | Show help and exit |
| `-V`, `--version` | Show version and exit |

`-l` and `-s` are mutually exclusive because they share the same display
area. A shell command is run via `/bin/sh -c` on every refresh, so it must be
non-interactive and exit on its own or the display will stall. Quote it as a
single argument and redirect its stderr (`2>&1` or `2>/dev/null`) so child
output does not overwrite the screen.

## tmux examples

The scripts in `tmux-examples/` are small helpers for setting up a multi-pane
monitoring window. They create the session on first use and afterwards replace
the command in individual panes in place.

```bash
# Three columns: minsysmon on the left on pve host, minsysmon in the middle on current host, minsysmon on steamdeck on the right
tmux-examples/tmux3 1 'ssh -t pve "bash -ic '\''minsysmon -n 10 -s \"journalctl -n 40 --no-pager 2>&1\" -t 40'\''"' \
2 "minsysmon -n 5 -l /var/logs/comfyui.txt -t 44" \
3 'ssh -t steamdeck "bash -ic '\''minsysmon -n 10 -s \"journalctl -n 40 --no-pager 2>&1\" -t 40'\''"'

# Three columns by two rows, numbered left-to-right, top-to-bottom (1-6)
tmux-examples/tmux6 -N -s monitors \
1 ~/.local/bin/minsysmon -n 20 -d 3 \
2 'ssh -t bazzite "bash -ic '\''minsysmon -n 10 -s \"journalctl -n 40 --no-pager 2>&1\" -t 40'\''"'
3 'ssh -t steamdeck "bash -ic '\''minsysmon -n 10 -s \"journalctl -n 40 --no-pager 2>&1\" -t 40'\''"'
4 "journalctl -f" \
5 iostat --pretty \
6 ifstat -p
```

Both scripts accept `-s SESSION` to choose a session name (defaults: `three`
and `six`) and `-N` to skip attaching on first creation. Copy them into your
`PATH` (or add their directory to it) to invoke them as `tmux3`/`tmux6`.

## License

MIT. See [LICENSE](LICENSE).
