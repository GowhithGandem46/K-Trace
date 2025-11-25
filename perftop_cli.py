#!/usr/bin/env python3
"""
perftop_cli.py - Live dashboard for CPU profiler

Displays a live dashboard showing:
- PID | CPU | Schedules | CPU Time (rdtsc) | Function

Auto-refreshes every 500ms using curses or rich library.
"""

import sys
import time
import os
import argparse
from pathlib import Path

try:
    from rich.console import Console
    from rich.table import Table
    from rich.live import Live
    from rich.panel import Panel
    from rich.layout import Layout
    from rich.text import Text
    RICH_AVAILABLE = True
except ImportError:
    RICH_AVAILABLE = False
    try:
        import curses
        CURSES_AVAILABLE = True
    except ImportError:
        CURSES_AVAILABLE = False
        print("Error: Neither 'rich' nor 'curses' library is available.")
        print("Install with: pip install rich")
        sys.exit(1)

# Paths
PROC_PATH = "/proc/perftop"
DEBUGFS_BASE = "/sys/kernel/debug/perftop"
DEBUGFS_SUMMARY = f"{DEBUGFS_BASE}/summary"
DEBUGFS_CONFIG = f"{DEBUGFS_BASE}/config"


def read_proc_perftop():
    """Read from /proc/perftop"""
    try:
        with open(PROC_PATH, 'r') as f:
            return f.read()
    except FileNotFoundError:
        return None
    except PermissionError:
        print(f"Error: Permission denied reading {PROC_PATH}")
        print("Please run as root or with appropriate permissions.")
        return None


def read_debugfs_summary():
    """Read from debugfs summary"""
    try:
        with open(DEBUGFS_SUMMARY, 'r') as f:
            return f.read()
    except FileNotFoundError:
        return None
    except PermissionError:
        return None


def read_debugfs_cpu(cpu_id):
    """Read from debugfs cpu_N file"""
    try:
        cpu_path = f"{DEBUGFS_BASE}/cpu_{cpu_id}"
        with open(cpu_path, 'r') as f:
            return f.read()
    except FileNotFoundError:
        return None
    except PermissionError:
        return None


def parse_perftop_output(output):
    """Parse perftop output and extract task information"""
    tasks = []
    if not output:
        return tasks

    lines = output.split('\n')
    current_task = None
    current_cpu = None

    for line in lines:
        line = line.strip()
        if not line:
            continue

        # Parse CPU section
        if line.startswith('--- CPU'):
            parts = line.split()
            if len(parts) >= 2:
                try:
                    current_cpu = int(parts[1])
                except ValueError:
                    current_cpu = None
            continue

        # Parse task line: "PID: X	CPU: Y	Schedules: Z	CPU Time: W rdtsc ticks"
        if line.startswith('PID:'):
            parts = line.split('\t')
            task_info = {}
            for part in parts:
                part = part.strip()
                if part.startswith('PID:'):
                    try:
                        task_info['pid'] = int(part.split(':')[1].strip())
                    except (ValueError, IndexError):
                        task_info['pid'] = 0
                elif part.startswith('CPU:'):
                    try:
                        task_info['cpu'] = int(part.split(':')[1].strip())
                    except (ValueError, IndexError):
                        task_info['cpu'] = current_cpu if current_cpu else 0
                elif part.startswith('Schedules:'):
                    try:
                        task_info['schedules'] = int(part.split(':')[1].strip())
                    except (ValueError, IndexError):
                        task_info['schedules'] = 0
                elif 'CPU Time:' in part:
                    try:
                        time_str = part.split('CPU Time:')[1].strip().split()[0]
                        task_info['cpu_time'] = int(time_str)
                    except (ValueError, IndexError):
                        task_info['cpu_time'] = 0

            if 'pid' in task_info:
                task_info['function'] = 'N/A'
                tasks.append(task_info)
        elif line.startswith('Stack Trace:') and tasks:
            # Next lines will be function names
            continue
        elif line.startswith('    ') and tasks:
            # Function name in stack trace
            func_name = line.strip()
            if func_name and tasks:
                tasks[-1]['function'] = func_name.split('(')[0] if '(' in func_name else func_name

    return tasks


def display_with_rich(refresh_interval=0.5):
    """Display using rich library"""
    console = Console()

    def generate_table():
        output = read_proc_perftop()
        if not output:
            table = Table(title="CPU Profiler - No Data Available")
            table.add_column("Status", style="red")
            table.add_row("Module not loaded or no data collected yet")
            return Panel(table)

        tasks = parse_perftop_output(output)
        if not tasks:
            table = Table(title="CPU Profiler - Waiting for Data")
            table.add_column("Status", style="yellow")
            table.add_row("Collecting profiling data...")
            return Panel(table)

        # Sort by CPU time (descending)
        tasks.sort(key=lambda x: x.get('cpu_time', 0), reverse=True)
        top_tasks = tasks[:20]  # Top 20

        table = Table(title="CPU Profiler - Top Tasks", show_header=True, header_style="bold magenta")
        table.add_column("PID", justify="right", style="cyan", no_wrap=True)
        table.add_column("CPU", justify="center", style="green")
        table.add_column("Schedules", justify="right", style="yellow")
        table.add_column("CPU Time", justify="right", style="blue")
        table.add_column("Function", style="white")

        for task in top_tasks:
            pid = task.get('pid', 0)
            cpu = task.get('cpu', '?')
            schedules = task.get('schedules', 0)
            cpu_time = task.get('cpu_time', 0)
            function = task.get('function', 'N/A')[:50]  # Truncate long names

            table.add_row(
                str(pid),
                str(cpu),
                str(schedules),
                f"{cpu_time:,}",
                function
            )

        # Add summary info
        summary = read_debugfs_summary()
        summary_text = Text()
        if summary:
            summary_lines = summary.split('\n')[:5]
            summary_text.append("\n".join(summary_lines), style="dim")
        else:
            summary_text.append("Summary not available", style="dim")

        layout = Layout()
        layout.split_column(
            Layout(table, name="main", ratio=3),
            Layout(Panel(summary_text, title="Summary"), name="summary", ratio=1)
        )

        return layout

    try:
        with Live(generate_table(), refresh_per_second=int(1/refresh_interval), screen=True) as live:
            while True:
                live.update(generate_table())
                time.sleep(refresh_interval)
    except KeyboardInterrupt:
        console.print("\n[bold red]Exiting...[/bold red]")


def display_with_curses(refresh_interval=0.5):
    """Display using curses library"""
    stdscr = curses.initscr()
    curses.noecho()
    curses.cbreak()
    stdscr.keypad(True)
    curses.curs_set(0)

    try:
        while True:
            stdscr.clear()
            stdscr.addstr(0, 0, "CPU Profiler - Top Tasks (Press Ctrl+C to exit)")
            stdscr.addstr(1, 0, "=" * 80)

            output = read_proc_perftop()
            if not output:
                stdscr.addstr(3, 0, "Module not loaded or no data collected yet")
                stdscr.refresh()
                time.sleep(refresh_interval)
                continue

            tasks = parse_perftop_output(output)
            if not tasks:
                stdscr.addstr(3, 0, "Collecting profiling data...")
                stdscr.refresh()
                time.sleep(refresh_interval)
                continue

            # Sort by CPU time
            tasks.sort(key=lambda x: x.get('cpu_time', 0), reverse=True)
            top_tasks = tasks[:20]

            # Header
            y = 3
            stdscr.addstr(y, 0, f"{'PID':<8} {'CPU':<6} {'Schedules':<12} {'CPU Time':<15} {'Function':<30}")
            y += 1
            stdscr.addstr(y, 0, "-" * 80)
            y += 1

            # Data rows
            for task in top_tasks:
                pid = task.get('pid', 0)
                cpu = task.get('cpu', '?')
                schedules = task.get('schedules', 0)
                cpu_time = task.get('cpu_time', 0)
                function = task.get('function', 'N/A')[:28]

                stdscr.addstr(y, 0, f"{pid:<8} {cpu:<6} {schedules:<12} {cpu_time:<15,} {function:<30}")
                y += 1
                if y >= curses.LINES - 2:
                    break

            stdscr.refresh()
            time.sleep(refresh_interval)

    except KeyboardInterrupt:
        pass
    finally:
        curses.nocbreak()
        stdscr.keypad(False)
        curses.echo()
        curses.endwin()


def main():
    parser = argparse.ArgumentParser(description='CPU Profiler Live Dashboard')
    parser.add_argument('--interval', type=float, default=0.5,
                        help='Refresh interval in seconds (default: 0.5)')
    parser.add_argument('--source', choices=['proc', 'debugfs'], default='proc',
                        help='Data source: proc or debugfs (default: proc)')
    args = parser.parse_args()

    # Check if module is loaded
    if not os.path.exists(PROC_PATH) and not os.path.exists(DEBUGFS_BASE):
        print("Error: perftop module is not loaded.")
        print("Please load it with: sudo insmod perftop.ko")
        sys.exit(1)

    if RICH_AVAILABLE:
        display_with_rich(args.interval)
    elif CURSES_AVAILABLE:
        display_with_curses(args.interval)
    else:
        print("Error: No suitable display library available")
        sys.exit(1)


if __name__ == '__main__':
    main()

