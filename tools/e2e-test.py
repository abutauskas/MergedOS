#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""End-to-end test of setup, restart, login and desktop persistence in QEMU.

  tools/e2e-test.py <kernel.elf32>

Boot 1: blank SATA disk. Drives the graphical wizard over the serial line
        (the kernel accepts keys from it): name, password, storage, Wi-Fi
        skip, desktop "Retro". The kernel saves the setup and resets.
Boot 2: same disk. The wizard must not appear. A wrong password must be
        rejected, the right one accepted, and the saved desktop started.
Boot 3: same disk, text mode (gfx=off): the password is still required.

The test waits for kernel log markers before each key press, so it does not
depend on timing. The password must never show up in the serial log.
"""
import os
import re
import selectors
import socket
import subprocess
import sys
import tempfile
import time

QEMU = os.environ.get("QEMU", "qemu-system-x86_64")
PASSWORD = "s3cret-pw"
DOWN, ENTER, TAB = "\x1b[B", "\r", "\t"


class Machine:
    def __init__(self, kernel, disk, append):
        self.log = b""
        self.proc = subprocess.Popen(
            [QEMU, "-m", "64", "-no-reboot", "-display", "none", "-serial", "stdio",
             "-device", "isa-debug-exit,iobase=0xf4,iosize=0x04",
             "-device", "ahci,id=ahci", "-drive", f"id=d0,file={disk},format=raw,if=none",
             "-device", "ide-hd,drive=d0,bus=ahci.0", "-kernel", kernel, "-append", append,
             "-monitor", f"unix:{disk}.mon,server,nowait"],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        os.set_blocking(self.proc.stdout.fileno(), False)
        self.sel = selectors.DefaultSelector()
        self.sel.register(self.proc.stdout, selectors.EVENT_READ)
        self.seen = 0
        self.mon_path = disk + ".mon"
        self.mon = None

    def monitor(self, command):
        """Send one command to the QEMU monitor (used to move and click the mouse)."""
        if self.mon is None:
            for _ in range(50):
                try:
                    self.mon = socket.socket(socket.AF_UNIX)
                    self.mon.connect(self.mon_path)
                    break
                except OSError:
                    self.mon = None
                    time.sleep(0.1)
        self.mon.sendall((command + "\n").encode())
        time.sleep(0.25)

    def click_at(self, pointer, target):
        self.monitor("mouse_move %d %d" % (target[0] - pointer[0], target[1] - pointer[1]))
        self.monitor("mouse_button 1")
        self.monitor("mouse_button 0")

    def wait(self, marker, timeout=60):
        """Block until `marker` appears in output not yet consumed."""
        deadline = time.time() + timeout
        needle = marker.encode()
        while True:
            pos = self.log.find(needle, self.seen)
            if pos >= 0:
                self.seen = pos + len(needle)
                return
            if time.time() > deadline or (self.proc.poll() is not None and not self.sel.select(0)):
                raise AssertionError(f"timed out waiting for {marker!r}; tail of log:\n"
                                     + self.log[-600:].decode(errors="replace"))
            if self.sel.select(0.2):
                self.log += self.proc.stdout.read() or b""

    def send(self, text):
        self.proc.stdin.write(text.encode())
        self.proc.stdin.flush()

    def finish(self, timeout=60):
        try:
            self.proc.wait(timeout)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            raise AssertionError("machine did not stop; tail of log:\n"
                                 + self.log[-600:].decode(errors="replace"))
        self.log += self.proc.stdout.read() or b""
        return self.proc.returncode, self.log.decode(errors="replace")


def main():
    kernel = sys.argv[1]
    with tempfile.TemporaryDirectory() as tmp:
        disk = os.path.join(tmp, "disk.img")
        with open(disk, "wb") as f:
            f.truncate(64 * 1024 * 1024)                # blank disk

        # ---- boot 1: first-run wizard --------------------------------------
        m = Machine(kernel, disk, "")
        m.wait("[setup] screen: language"); m.send(ENTER)
        m.wait("[setup] screen: name");     m.send("Ana" + ENTER)
        m.wait("[setup] screen: password"); m.send(PASSWORD + TAB + PASSWORD + ENTER)
        m.wait("[setup] screen: storage");  m.send(ENTER)       # default: the blank disk
        m.wait("[setup] screen: wifi");     m.send(ENTER)       # skip
        m.wait("[setup] screen: desktop");  m.send(DOWN * 4 + ENTER)   # Retro, not the default
        m.wait("[setup] screen: summary");  m.send(ENTER)
        m.wait("written (0)")
        m.wait("[setup] setup saved: restarting")
        rc, log1 = m.finish()
        assert PASSWORD not in log1, "password leaked to the log during setup"
        assert "[login]" not in log1
        print("e2e: boot 1: wizard completed, setup written, machine reset (qemu exit %d)" % rc)

        # ---- boot 2: login, wrong then right password -----------------------
        m = Machine(kernel, disk, "")
        m.wait('saved setup found on disk: user "Ana"')
        m.wait("[login] password required");            m.send("not-the-password" + ENTER)
        m.wait("[login] wrong password (attempt 1)");   m.send(PASSWORD[:-1] + ENTER)
        m.wait("[login] wrong password (attempt 2)");   m.send(PASSWORD + ENTER)
        m.wait("[login] ok")
        m.wait("[de] desktop from saved setup: Retro 640x480x16")
        m.wait("[de] session: Retro 640x480x16")
        m.wait("msh:/>")
        # The graphical shell: click the "files" launcher with the emulated mouse, then
        # drag the window it opened by its title bar. Positions come from the kernel log.
        text = m.log.decode(errors="replace")
        files = tuple(map(int, re.search(r"\[desk\] icon files (\d+) (\d+)", text).groups()))
        tx, ty, px, py = map(int, re.search(r"\[desk\] title (\d+) (\d+) pointer (\d+) (\d+)",
                                            text).groups())
        m.click_at((px, py), files)
        m.wait("[desk] launcher: files")
        m.wait("[desk] window open files")              # the click opened the Files window
        m.wait("ls /")                                  # typed into the shell by the click
        m.wait("<DIR>")
        m.wait("msh:/>")
        # Click the first file the Files window lists (a sample PNG): the Viewer opens it.
        text = m.log.decode(errors="replace")
        rx, ry = map(int, re.search(r"\[desk\] window files row0 (\d+) (\d+)", text).groups())
        m.click_at(files, (rx, ry))
        m.wait("[desk] files: home/samples/sample.png")
        m.wait("[desk] viewer: /home/samples/sample.png: PNG image (image/png), image decoded")
        files = (rx, ry)                                # where the pointer now is
        # The new window may cover the terminal's title bar: drag the Files window instead.
        text = m.log.decode(errors="replace")
        tx, ty = map(int, re.search(r"\[desk\] window files title (\d+) (\d+)", text).groups())
        m.monitor("mouse_move %d %d" % (tx - files[0], ty - files[1]))
        m.monitor("mouse_button 1")
        m.monitor("mouse_move 40 30")
        m.monitor("mouse_button 0")
        m.wait("[desk] window moved")
        m.send("poweroff" + ENTER)
        rc, log2 = m.finish()
        assert rc == 33, rc
        assert "[setup] screen" not in log2, "wizard ran again although a setup was saved"
        assert PASSWORD not in log2 and "not-the-password" not in log2, "password leaked"
        assert log2.index("[login] ok") < log2.index("[de] session"), "desktop before login"
        print("e2e: boot 2: wizard skipped, 2 wrong passwords rejected, right one accepted, "
              "saved desktop (Retro) started;\n     mouse click on a launcher opened its window and ran its command, a click on a file "
              "opened it in the Viewer, title-bar drag moved the window")

        # ---- boot 3: text mode still asks ------------------------------------
        m = Machine(kernel, disk, "gfx=off")
        m.wait("Password for Ana:");                    m.send("wrong" + ENTER)
        m.wait("[login] wrong password (attempt 1)")
        m.wait("Password for Ana:");                    m.send(PASSWORD + ENTER)
        m.wait("[login] ok")
        m.wait("msh:/>");                               m.send("poweroff" + ENTER)
        rc, log3 = m.finish()
        assert rc == 33 and PASSWORD not in log3
        print("e2e: boot 3: text mode (gfx=off) also requires the password")
    print("e2e: PASS")


if __name__ == "__main__":
    try:
        main()
    except AssertionError as e:
        print("e2e: FAIL:", e)
        sys.exit(1)
