"""
PS1 DEV CART GUI: runs ps1transfer, one task at a time (a cartridge can only be opened by one program).

The output is read by a thread and handed to the GUI thread with a queue: text for the log, "progress=done/total"
lines (ps1transfer -progress) for the progress bar, "pcdrv=..." lines (ps1transfer -pcdrvlog) for the file server
activity. A task can also take commands on its input (ps1transfer -synth).

This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
If a copy of the MPL was not distributed with this file,
You can obtain one at https://mozilla.org/MPL/2.0/.
"""
import os
import queue
import re
import subprocess
import threading

from devices import NO_WINDOW

PROGRESS = re.compile(r"^progress=(\d+)/(\d+)$")
BANNER = "PS1 DEV CART Transfer tool"
PCDRV = "pcdrv="          # File server activity lines (not shown in the log)


class Task:
    def __init__(self, name, args, long_running, quiet, on_line, on_done, console=False, log_filter=None):
        self.name = name
        self.log_filter = log_filter        # Called with each line: False = not shown in the log
        self.console = console              # Output also shown in the console page
        self.args = args
        self.long_running = long_running    # Runs until stopped (console, watch, GDB server)
        self.quiet = quiet                  # Output not shown in the log (status queries)
        self.on_line = on_line              # Called with each output line
        self.on_done = on_done              # Called with the exit code
        self.proc = None
        self.output = []                    # Lines, for the on_done callback
        self.stopped = False


class Runner:
    def __init__(self, root, on_text, on_progress, on_state, on_pcdrv=None):
        self.root = root
        self.on_pcdrv = on_pcdrv            # (task, line): file server activity line
        self.on_text = on_text              # (task, text): output text for the log/console
        self.on_progress = on_progress      # (done, total)
        self.on_state = on_state            # (task or None): a task started or ended
        self.task = None
        self.queue = queue.Queue()
        self.partial = ""
        self.shown = 0
        self.send_lock = threading.Lock()   # send() may be called from another thread (MIDI input)
        self.root.after(50, self._poll)

    @property
    def busy(self):
        return self.task is not None

    def start(self, args, name, long_running=False, quiet=False, cwd=None, on_line=None, on_done=None, console=False,
              stdin=False, log_filter=None):
        """Starts ps1transfer (args[0] is its path). stdin: commands sent with send(). Returns False if a task
        already runs."""
        if self.task:
            return False
        task = Task(name, args, long_running, quiet, on_line, on_done, console, log_filter)
        try:
            task.proc = subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                         stdin=subprocess.PIPE if stdin else subprocess.DEVNULL, cwd=cwd,
                                         creationflags=NO_WINDOW, bufsize=0)
        except OSError as e:
            self.on_text(task, "Can't start %s: %s\n" % (args[0], e))
            if on_done:
                on_done(-1, [])
            return True
        self.task = task
        self.partial = ""
        self.shown = 0
        threading.Thread(target=self._read, args=(task,), daemon=True).start()
        self.on_state(task)
        return True

    def send(self, line):
        """Sends a command line to the task's input. Returns False if no task takes commands."""
        task = self.task
        if not task or not task.proc or not task.proc.stdin:
            return False
        with self.send_lock:
            try:
                task.proc.stdin.write((line + "\n").encode())
                task.proc.stdin.flush()
            except (OSError, ValueError):
                return False
        return True

    def stop(self):
        if self.task and self.task.proc:
            self.task.stopped = True
            proc = self.task.proc
            if proc.stdin:
                # A task taking commands (ps1transfer -synth, -relay) is asked to quit: it ends cleanly, never in
                # the middle of a USB transfer (which would leave the PS1 reading the next command as data)
                try:
                    with self.send_lock:
                        proc.stdin.write(b"quit\n")
                        proc.stdin.flush()
                        proc.stdin.close()
                except (OSError, ValueError):
                    pass
                self.root.after(2000, lambda: self._terminate(proc))
                return
            self._terminate(proc)

    @staticmethod
    def _terminate(proc):
        if proc.poll() is None:
            try:
                proc.terminate()
            except OSError:
                pass

    def _read(self, task):
        fd = task.proc.stdout.fileno()
        while True:
            try:
                data = os.read(fd, 4096)
            except OSError:
                break
            if not data:
                break
            self.queue.put((task, data.decode(errors="replace")))
        task.proc.wait()
        self.queue.put((task, None))

    def _poll(self):
        try:
            while True:
                task, text = self.queue.get_nowait()
                if text is None:
                    self._finish(task)
                else:
                    self._text(task, text)
        except queue.Empty:
            pass
        self.root.after(50, self._poll)

    def _text(self, task, text):
        # Whole lines are checked for progress lines and the banner, and kept in the task output. A partial line
        # is shown at once (console): self.shown characters of it are already in the log.
        # Line ends are \n, or \r\n / \n\r from the PS1 tty: \r is dropped (progress uses -progress lines)
        text = (self.partial + text).replace("\r", "")
        lines = text.split("\n")
        self.partial = lines.pop()
        out = []
        for line in lines:
            shown, self.shown = self.shown, 0
            m = PROGRESS.match(line.strip())
            if m:
                self.on_progress(int(m.group(1)), int(m.group(2)))
                continue
            if line.startswith(PCDRV):
                if self.on_pcdrv:
                    self.on_pcdrv(task, line)
                continue
            if line.startswith(BANNER) or (not line.strip() and not out and not task.output):
                continue
            if task.on_line:
                task.on_line(line)
            if task.log_filter and not task.log_filter(line):
                continue
            task.output.append(line)
            out.append(line[shown:] + "\n")
        # A partial line is shown at once (console), unless lines are filtered
        maybe_hidden = ("progress=".startswith(self.partial[:9]) or self.partial.startswith(PCDRV) or
                        PCDRV.startswith(self.partial))
        if (len(self.partial) > self.shown) and not task.log_filter and not maybe_hidden:
            out.append(self.partial[self.shown:])
            self.shown = len(self.partial)
        if out and not task.quiet:
            self.on_text(task, "".join(out))

    def _finish(self, task):
        if self.partial:
            self._text(task, "\n")
        code = task.proc.returncode
        self.task = None
        self.on_state(None)
        if task.on_done:
            task.on_done(code, task.output)
