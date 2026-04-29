#!/usr/bin/env python3
import sys
import os
import subprocess
import signal
import threading
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parent


def resolve_output_path(path_arg):
    path = Path(path_arg).expanduser()
    if path.is_absolute():
        return path
    if path.parts and path.parts[0] == "experiments":
        return (REPO_ROOT / path).resolve()
    return (REPO_ROOT / "experiments" / "manual_runs" / path).resolve()


def main():
    # Usage: python3 exec_esp32.py <output_file> [command...]
    
    if len(sys.argv) < 2:
        print("Usage: python3 exec_esp32.py <output_csv_path> [optional: idf.py command...]")
        print("Example: python3 exec_esp32.py experiments/manual_runs/results.csv")
        sys.exit(1)

    output_path = str(resolve_output_path(sys.argv[1]))
    output_dir = os.path.dirname(output_path)
    
    # Default command if none provided
    cmd = ["idf.py", "fullclean" , "build", "flash", "monitor"]
    if len(sys.argv) > 2:
        cmd = sys.argv[2:]

    print(f"\033[93m[WRAPPER] Running command: {' '.join(cmd)}\033[0m")
    print(f"\033[93m[WRAPPER] Output will be saved to: {output_path}\033[0m\n")

    # Start the subprocess
    process = subprocess.Popen(
        cmd, 
        stdout=subprocess.PIPE, 
        stderr=subprocess.STDOUT, 
        text=True, 
        bufsize=1,  # Line buffered
        universal_newlines=True
    )

    import re

    # Generic CSV capture state: maps section name -> list of lines
    captured_sections = {}
    current_section = None
    current_data = []

    def save_section(name, data):
        """Save a captured CSV section to disk."""
        if not data:
            return
        os.makedirs(output_dir, exist_ok=True)
        # First section (CSV) uses the output path directly, others get a suffix
        if name == "CSV":
            path = output_path
        else:
            suffix = name.lower()
            path = output_path.replace('.csv', f'_{suffix}.csv')
        with open(path, 'w') as f:
            f.writelines(data)
        print(f"\033[92m[SUCCESS] {name} extracted to {path}.\033[0m")
        captured_sections[name] = path

    def process_line(line):
        """Process a line for CSV capture."""
        nonlocal current_section, current_data

        # Detect start marker: <<<SOMETHING_START>>>
        start_match = re.search(r'<<<(\w+)_START>>>', line)
        if start_match:
            current_section = start_match.group(1)
            current_data = []
            return

        # Detect end marker: <<<SOMETHING_END>>>
        end_match = re.search(r'<<<(\w+)_END>>>', line)
        if end_match:
            section_name = end_match.group(1)
            if current_section == section_name:
                save_section(section_name, current_data)
                current_section = None
                current_data = []
            return

        # Accumulate data if inside a section
        if current_section is not None:
            current_data.append(line)

    def terminate_and_analyze():
        """Terminate the subprocess and run analysis."""
        process.terminate()
        try:
            process.wait(timeout=2)
        except subprocess.TimeoutExpired:
            process.kill()

        # --- RUN ANALYSIS ---
        # analyze_script = os.path.join(script_dir, "analyse.py")
        # if os.path.exists(analyze_script) and captured_sections:
        #     print(f"\n\033[94m[ANALYSIS] Found {analyze_script}. Running...\033[0m")
        #     print("-" * 60)
        #     subprocess.run([sys.executable, analyze_script, output_path])
        #     print("-" * 60)
        # else:
        #     print(f"\n\033[90m[INFO] No analyse.py found or no CSV captured. Skipping analysis.\033[0m")

    def save_crash_log(lines, suffix):
        """Save crash/WDT log to file, stripping ANSI escape codes."""
        ansi_escape = re.compile(r'\x1b\[[0-9;]*m')
        path = output_path.replace('.csv', suffix)
        os.makedirs(output_dir, exist_ok=True)
        with open(path, 'w') as f:
            for cl in lines:
                f.write(ansi_escape.sub('', cl))
        print(f"\033[92m[SUCCESS] Log saved to {path}\033[0m")

    # Timeout: if no termination trigger after N seconds of serial output, save what we have.
    # Disabled by default. Set EXEC_ESP32_TIMEOUT_S to a positive integer to enable it.
    serial_timeout_raw = os.environ.get("EXEC_ESP32_TIMEOUT_S", "0")
    try:
        SERIAL_TIMEOUT_S = int(serial_timeout_raw)
        if SERIAL_TIMEOUT_S < 0:
            SERIAL_TIMEOUT_S = 0
    except ValueError:
        SERIAL_TIMEOUT_S = 0
    timeout_timer = None

    def on_timeout():
        print(f"\n\033[93m[WRAPPER] Timeout ({SERIAL_TIMEOUT_S}s) — saving captured data and exiting.\033[0m")
        terminate_and_analyze()
        os._exit(0)

    wdt_saved = False  # Only capture first WDT occurrence

    try:
        for line in process.stdout:
            sys.stdout.write(line)
            process_line(line)

            # Reset timeout on each line of output when timeout is enabled.
            if SERIAL_TIMEOUT_S > 0:
                if timeout_timer:
                    timeout_timer.cancel()
                timeout_timer = threading.Timer(SERIAL_TIMEOUT_S, on_timeout)
                timeout_timer.start()

            # Normal termination: runtime stats finished
            if "<<<RUNTIME_STATS_END>>>" in line:
                if timeout_timer:
                    timeout_timer.cancel()
                process.stdout.flush()
                terminate_and_analyze()
                sys.exit(0)

            # Crash detection: ESP32 Guru Meditation Error (native fault tests)
            if "Guru Meditation Error" in line:
                print(f"\n\033[91m[CRASH] ESP32 Guru Meditation Error detected!\033[0m")
                crash_log = [line]
                for crash_line in process.stdout:
                    sys.stdout.write(crash_line)
                    process_line(crash_line)
                    crash_log.append(crash_line)
                    if "Rebooting..." in crash_line or len(crash_log) > 30:
                        break
                save_crash_log(crash_log, "_crash.log")
                if timeout_timer:
                    timeout_timer.cancel()
                terminate_and_analyze()
                sys.exit(1)

            # WDT detection: task watchdog fired
            # Save the first occurrence. Native infinite-loop fault tests should
            # stop after the log is saved; WAMR watchdog handling may recover.
            if "task_wdt: Task watchdog got triggered" in line and not wdt_saved:
                print(f"\n\033[93m[WDT] Task Watchdog triggered — capturing log, continuing...\033[0m")
                wdt_log = [line]
                for wdt_line in process.stdout:
                    sys.stdout.write(wdt_line)
                    process_line(wdt_line)
                    wdt_log.append(wdt_line)
                    # Stop after backtrace ends (blank line) or enough lines
                    if len(wdt_log) > 5 and wdt_line.strip() == '':
                        break
                    if len(wdt_log) > 30:
                        break
                save_crash_log(wdt_log, "_wdt.log")
                wdt_saved = True

                # Native fault-mode runs emit FAULT_NATIVE metadata before they
                # intentionally wedge or crash. Once the WDT log is saved,
                # terminate so the runner can continue to the next case.
                if "FAULT_NATIVE" in captured_sections:
                    print(f"\n\033[93m[WDT] Native fault run captured; stopping monitor.\033[0m")
                    if timeout_timer:
                        timeout_timer.cancel()
                    terminate_and_analyze()
                    sys.exit(1)
                # Continue monitoring — WAMR watchdog handling may recover.

    except KeyboardInterrupt:
        if timeout_timer:
            timeout_timer.cancel()
        process.send_signal(signal.SIGINT)
        process.wait()
        sys.exit(0)

    # If subprocess exits without explicit markers (e.g. build/flash error),
    # still finalize and run analysis on any captured sections.
    if timeout_timer:
        timeout_timer.cancel()
    terminate_and_analyze()
    rc = process.poll()
    sys.exit(rc if rc is not None else 0)

if __name__ == "__main__":
    main()
