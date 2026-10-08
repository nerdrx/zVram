"""Small UI checks; --gui exercises actual Tk widgets on an isolated display."""
import sys
from zvram_ui import profile_from_fields, command_text, system_status, profile_details


class FakeManager:
    def __init__(self):
        self.profiles = {"test": dict(name="test", priority="low", resident_mib=1024,
                                     command=["/bin/echo", "hello world"], mode="vulkan", env={"TEST": "1"}, min_available_mib=16384, max_swap_growth_mib=8192)}
        self.calls = []

    def list_profiles(self):
        return list(self.profiles.values())

    def status(self):
        return dict(mem_available_mib=20000, swap_used_mib=500,
                    gpu=[dict(card="card1", vram_used_mib=100, vram_total_mib=24000, gtt_used_mib=50)])

    def save_profile(self, profile):
        self.profiles[profile["name"]] = profile

    def start(self, name):
        self.calls.append(("start", name))
        self.profiles[name].update(running=True, pid=12345)

    def stop(self, name):
        self.calls.append(("stop", name))
        self.profiles[name].update(running=False)

    def set_priority(self, name, priority):
        self.profiles[name].update(priority=priority, resident_mib=None)

    def log_tail(self, name, lines=30):
        return "Test log: no workload launched."

    def delete(self, name):
        del self.profiles[name]


def check_fields():
    profile = profile_from_fields("tiny", "low", "", '/bin/echo "hello world"')
    assert profile["command"] == ["/bin/echo", "hello world"]
    assert profile["resident_mib"] is None
    assert profile_from_fields("a", "high", None, "echo hi")["resident_mib"] is None
    assert profile_from_fields("a", "low", "", "zvram -- /bin/true", mode="wrapped")["mode"] == "wrapped"
    assert "Native: no zVram residency cap" in profile_details({"mode": "native"})
    assert command_text(profile) == "/bin/echo 'hello world'"
    for fields in (("", "low", "1", "echo hi"), ("a", "bad", "1", "echo hi"),
                   ("a", "high", "0", "echo hi"), ("a", "high", "-1", "echo hi"),
                   ("a", "high", "1", ""), ("a", "high", "1", 'echo "')):
        try:
            profile_from_fields(*fields)
        except ValueError:
            pass
        else:
            raise AssertionError("Accepted invalid profile: %r" % (fields,))
    assert "VRAM 100/24000 MiB" in system_status(FakeManager().status())
    assert "not physical residency" in profile_details({})


def check_gui():
    import tkinter as tk
    from zvram_ui import ManagerWindow
    root = tk.Tk()
    manager = FakeManager()
    window = ManagerWindow(root, manager)
    root.geometry("1200x920")
    root.update()
    window.table.selection_set("test")
    root.update()
    assert window.fields["priority"].get() == "low"
    window.fields["resident_mib"].set("2048")
    window.refresh()
    root.update()
    assert window.fields["resident_mib"].get() == "2048", "Refresh overwrote unsaved edit"
    window.fields["priority"].set("high")
    window.save()
    root.update()
    assert manager.profiles["test"]["resident_mib"] == 2048
    assert manager.profiles["test"]["env"] == {"TEST": "1"}
    assert manager.profiles["test"]["min_available_mib"] == 16384
    assert manager.profiles["test"]["max_swap_growth_mib"] == 8192
    window.act("start")
    window.act("stop")
    assert manager.calls == [("start", "test"), ("stop", "test")]
    window.logs()
    root.update()
    for child in root.winfo_children():
        if isinstance(child, tk.Toplevel):
            child.destroy()
    window.new()
    window.fields["name"].set("second")
    window.fields["command"].set("/bin/true")
    window.fields["mode"].set("wrapped")
    window.save()
    root.update()
    assert manager.profiles["second"]["resident_mib"] is None
    assert manager.profiles["second"]["mode"] == "wrapped"
    assert "RAM available: 20000" in window.telemetry.get()
    window.update_cards(dict(gpu=[dict(card="card0", vram_used_mib=20, vram_total_mib=2048), dict(card="card1", vram_used_mib=1024, vram_total_mib=24576)]))
    assert window.cards["gpu"][0].get() == "1.0 GiB / 24.0 GiB"
    assert "card0" in window.gpu_caption.get() and "card1" in window.gpu_caption.get()
    if "--screenshot" in sys.argv:
        import subprocess
        from pathlib import Path
        path = Path("build/manager-ui-smoke.png")
        path.parent.mkdir(exist_ok=True)
        subprocess.run(["import", "-window", str(root.winfo_id()), str(path)], check=True, timeout=5)
    root.destroy()


def check_tui():
    import json
    import os
    import pty
    import select
    import subprocess
    import tempfile
    import time
    with tempfile.TemporaryDirectory() as directory:
        result = os.path.join(directory, "result.json")
        master, slave = pty.openpty()
        script = ("import curses,json; from test_zvram_ui import FakeManager; "
                  "from zvram_ui import _tui; m=FakeManager(); curses.wrapper(_tui,m); "
                  "json.dump([m.profiles,m.calls],open(%r,'w'))" % result)
        child = subprocess.Popen([sys.executable, "-c", script], stdin=slave, stdout=slave, stderr=slave,
                                 env=dict(os.environ, TERM="xterm-256color"))
        os.close(slave)
        output = bytearray()
        def feed(keys):
            os.write(master, keys)
            end = time.monotonic() + 0.15
            while time.monotonic() < end:
                if select.select([master], [], [], 0.03)[0]:
                    try:
                        output.extend(os.read(master, 65536))
                    except OSError:
                        break
        try:
            feed(b"")
            feed(b"n")
            for value in (b"tiny", b"low", b"", b"vulkan", b"/bin/true"):
                feed(b"\x15" + value + b"\n")
            feed(b"s")
            feed(b"x")
            feed(b"p")
            feed(b"l")
            feed(b" ")
            feed(b"q")
            child.wait(timeout=5)
            assert child.returncode == 0, output.decode(errors="replace")
            profiles, calls = json.load(open(result))
            assert profiles["tiny"]["command"] == ["/bin/true"]
            assert profiles["tiny"]["resident_mib"] is None
            assert calls == [["start", "test"], ["stop", "test"]]
            assert profiles["test"]["priority"] == "high"
            assert profiles["test"]["resident_mib"] is None
        finally:
            if child.poll() is None:
                child.terminate()
                child.wait(timeout=5)
            os.close(master)


if __name__ == "__main__":
    check_fields()
    if "--gui" in sys.argv:
        check_gui()
    if "--tui" in sys.argv:
        check_tui()
    print("zVram UI checks passed" + (" (Tk widgets exercised)" if "--gui" in sys.argv else ""))
