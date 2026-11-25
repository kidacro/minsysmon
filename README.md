# sysmon

A small Linux system monitor for the terminal, built with ncurses. It shows
CPU, memory, swap and optional GPU memory usage, a list of the busiest user
processes, and — optionally — the tail of a log file or the output of a shell
command, all refreshed together.

It reads system counters from `/proc`, so it is Linux-specific.

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
* Optional: `tmux` for the example scripts in `tmux-examples/`.

## Build

```bash
make
```

Or compile directly:

```bash
gcc -O2 -Wall -Wextra sysmon.c -o sysmon -lncurses -ltinfo
```

Use `make clean` to remove the compiled binary.

## Usage

```bash
./sysmon                                   # defaults: 5 s delay, 3 processes
./sysmon -d 1 -n 10                        # refresh every second, show 10 processes
./sysmon -l /var/log/syslog -t 20           # show the last 20 lines of a log
./sysmon -s 'journalctl -n 10 --no-pager 2>&1'
./sysmon -h                                # full help
```

| Option | Meaning |
| --- | --- |
| `-d`, `--delay <seconds>` | Refresh delay, positive whole seconds (default: 5) |
| `-n`, `--processes <count>` | Number of top processes to show, 1–20 (default: 3) |
| `-l`, `--log <logfile>` | Show the last `-t` lines of a log file |
| `-s`, `--shell <command>` | Run a command on each refresh and show its last `-t` lines |
| `-t`, `--taillines <count>` | Tail length for `-l`/`-s`, 1–100 (default: 10) |
| `-h`, `--help` | Show help and exit |

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
# Three columns: sysmon on the left, a log in the middle, htop on the right
tmux-examples/tmux3 1 ./sysmon 2 "tail -f /var/log/syslog" 3 htop

# Three columns by two rows, numbered left-to-right, top-to-bottom (1-6)
tmux-examples/tmux6 -N -s monitors 1 ./sysmon 4 "journalctl -f" 6 htop
```

Both scripts accept `-s SESSION` to choose a session name (defaults: `three`
and `six`) and `-N` to skip attaching on first creation. Copy them into your
`PATH` (or add their directory to it) to invoke them as `tmux3`/`tmux6`.

## License

MIT. See [LICENSE](LICENSE).
