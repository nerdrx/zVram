"""Small UI checks; --gui exercises actual Tk widgets on an isolated display."""
import sys
from zvram_ui import profile_from_fields, command_text, system_status, profile_details, table_state, usage_mib


EXTERNAL = dict(name="@12345-678", display_name="vrchat.exe", external=True, source="zVram",
                mode="wrapped", priority="—", running=True, pid=12345, state="external",
                command=[], resident_mib=None, rss_mib=256, vram_mib=512, gtt_mib=64)


class FakeManager:
    def __init__(self, external=False):
        self.profiles = {"test": dict(name="test", priority="low", resident_mib=1024,
                                     command=["/bin/echo", "hello world"], mode="vulkan", env={"TEST": "1"}, min_available_mib=16384, max_swap_growth_mib=8192,
                                     vram_mib=512.4, gtt_mib=None, rss_mib=0)}
        if external:
            self.profiles[EXTERNAL["name"]] = dict(EXTERNAL)
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

    def set_live_limit(self, name, mib):
        self.calls.append(("live_limit", name, mib))

    def set_live_priority(self, name, priority):
        self.calls.append(("live_priority", name, priority))

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
    assert usage_mib({"vram_mib": 512.4}, "vram_mib") == "512"
    assert usage_mib({"rss_mib": 0}, "rss_mib") == "0"
    assert usage_mib({}, "gtt_mib") == "—"
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
    details = profile_details(EXTERNAL)
    assert "restart through updated zVram" in details and "Configured cap: unknown" in details
    assert "Next launch" not in details and "not running" not in details
    assert 'layer is NOT loaded' in details
    assert table_state(EXTERNAL) == 'Layer missing'
    assert table_state(dict(EXTERNAL, backend_loaded=True)) == 'No endpoint'
    assert table_state(dict(EXTERNAL, backend_loaded=True, control_devices=[{'capable': False}])) == 'Paging off'
    assert table_state(dict(EXTERNAL, live_capable=True)) == 'Live control'
    assert 'Apply live requires a running app' in profile_details({'running': False})
    assert "Configured cap: 1024 MiB" in profile_details(dict(EXTERNAL, active_resident_mib=1024))
    live = dict(EXTERNAL, live_capable=True, control_devices=[dict(device="2", current_limit_mib=1024, resident_mib=900, requested_mib=768, result=2, reason=7)])
    assert "GPU 1: cap 1024 MiB · resident 900 MiB · requested 768 MiB · rejected (request rejected)" in profile_details(live)
    check_external_tui()


def check_gui():
    import tkinter as tk
    from zvram_ui import ManagerWindow
    root = tk.Tk()
    manager = FakeManager(external=True)
    window = ManagerWindow(root, manager)
    root.geometry("1200x920")
    root.update()
    assert len(window.table.get_children()) == 2
    assert window.table.item("test", "values") == ("512", "—", "0", "LOW", "Stopped")
    assert tuple(window.table.cget("columns")) == ("vram", "gtt", "rss", "priority", "state")
    window.table.selection_set("test")
    root.update()
    assert window.fields["priority"].get() == "low"
    window.fields["resident_mib"].set("2048")
    window.refresh()
    root.update()
    assert window.fields["resident_mib"].get() == "2048", "Refresh overwrote unsaved edit"
    window.fields["priority"].set("high")
    window.swap_guard.set(True)
    window.save()
    root.update()
    assert manager.profiles["test"]["resident_mib"] == 2048
    assert manager.profiles["test"]["ignore_swap_guard"] is True
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
    assert window.swap_guard.get() is False
    window.fields["name"].set("second")
    window.fields["command"].set("/bin/true")
    window.fields["mode"].set("wrapped")
    window.save()
    root.update()
    assert manager.profiles["second"]["resident_mib"] is None
    assert manager.profiles["second"]["mode"] == "wrapped"
    assert manager.profiles["second"]["ignore_swap_guard"] is False
    assert "RAM available: 20000" in window.telemetry.get()
    window.update_cards(dict(gpu=[dict(card="card0", vram_used_mib=20, vram_total_mib=2048), dict(card="card1", vram_used_mib=1024, vram_total_mib=24576)]))
    assert window.cards["gpu"][0].get() == "1.0 GiB / 24.0 GiB"
    assert "card0" in window.gpu_caption.get() and "card1" in window.gpu_caption.get()
    from unittest.mock import patch
    window.table.selection_set(EXTERNAL["name"])
    root.update()
    assert window.fields["name"].get() == "vrchat.exe"
    assert window.read_only and "restart through updated zVram" in window.details.get()
    assert window.controls["Stop app"].instate(["!disabled"])
    assert all(widget.instate(["disabled"]) for label, widget in window.controls.items() if label != "Stop app")
    assert all(widget.instate(["disabled"]) for widget in window.field_widgets.values())
    from unittest.mock import DEFAULT
    with patch.multiple(manager, **{name: DEFAULT for name in ("save_profile", "start", "stop", "delete", "log_tail")}) as actions:
        window.save()
        window.act("start")
        window.delete()
        window.logs()
        for action in actions.values():
            action.assert_not_called()
    assert manager.profiles[EXTERNAL["name"]] == EXTERNAL
    window.act("stop")
    assert manager.calls[-1] == ("stop", EXTERNAL["name"])
    manager.profiles[EXTERNAL["name"]].update(live_capable=True, running=True, control_devices=[dict(device="0", capable=True, current_limit_mib=1024, resident_mib=512, requested_mib=768, result=1)])
    window.refresh()
    root.update()
    assert "pending" in window.details.get()
    assert window.controls["Apply live"].instate(["!disabled"])
    assert window.field_widgets["priority"].instate(["!disabled"])
    assert window.field_widgets["command"].instate(["disabled"])
    window.fields["resident_mib"].set("768")
    window.save()
    assert manager.calls[-1] == ("live_limit", EXTERNAL["name"], 768)
    window.fields["resident_mib"].set("")
    window.fields["priority"].set("high")
    window.apply_live()
    assert manager.calls[-1] == ("live_priority", EXTERNAL["name"], "high")
    assert manager.profiles[EXTERNAL["name"]]["priority"] == "—"
    window.new()
    assert not window.read_only
    assert all(not widget.instate(["disabled"]) for label, widget in window.controls.items() if label != "Apply live")
    assert all(not widget.instate(["disabled"]) for widget in window.field_widgets.values())
    if "--screenshot" in sys.argv:
        import subprocess
        from pathlib import Path
        path = Path("build/manager-ui-smoke.png")
        path.parent.mkdir(exist_ok=True)
        subprocess.run(["import", "-window", str(root.winfo_id()), str(path)], check=True, timeout=5)
    root.destroy()


def check_external_tui():
    from unittest.mock import DEFAULT, patch
    from zvram_ui import _tui
    class Screen:
        def __init__(self):
            self.keys = iter(map(ord, "epsldq"))
            self.lines = []
        def getmaxyx(self): return (30, 180)
        def timeout(self, value): pass
        def erase(self): pass
        def refresh(self): pass
        def getch(self): return next(self.keys)
        def addnstr(self, row, col, text, width, flags): self.lines.append(text)
    manager = FakeManager(external=True)
    manager.profiles.pop("test")
    screen = Screen()
    with patch("curses.curs_set"), patch("zvram_ui._edit", side_effect=AssertionError("external edit")), \
         patch("zvram_ui._prompt", side_effect=AssertionError("external prompt")), \
         patch.multiple(manager, **{name: DEFAULT for name in ("save_profile", "set_priority", "start", "stop", "delete", "log_tail")}) as actions:
        _tui(screen, manager)
        for action in actions.values():
            action.assert_not_called()
    assert manager.profiles == {EXTERNAL["name"]: EXTERNAL}
    assert manager.calls == []
    assert any("vrchat.exe" in line and "External" in line for line in screen.lines)
    assert any("Configured cap: unknown" in line for line in screen.lines)


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
        output_cursor = 0
        def wait_for(text):
            nonlocal output_cursor
            expected = text.encode()
            deadline = time.monotonic() + 5
            while expected not in output[output_cursor:] and time.monotonic() < deadline:
                if child.poll() is not None:
                    break
                if select.select([master], [], [], max(0, min(0.1, deadline-time.monotonic())))[0]:
                    try:
                        output.extend(os.read(master, 65536))
                    except OSError:
                        break
            found = output.find(expected, output_cursor)
            assert found >= 0, "TUI did not reach %r: %s" % (text, output.decode(errors="replace"))
            output_cursor = found + len(expected)

        def send(keys):
            os.write(master, keys)

        try:
            wait_for("zVram Manager")
            send(b"n")
            for prompt, value in (
                    ("Name:", b"tiny\n"),
                    ("Priority [high/normal/low]:", b"\x15low\n"),
                    ("Resident MiB [blank=default]:", b"\n"),
                    ("Mode [native/vulkan/wrapped]:", b"\x15vulkan\n"),
                    ("Command:", b"/bin/true\n")):
                wait_for(prompt)
                send(value)
            wait_for("Saved; residency applies next launch")
            send(b"s")
            wait_for("test: start requested")
            send(b"x")
            wait_for("Stopped")
            send(b"p")
            wait_for("Priority saved")
            send(b"l")
            wait_for("Any key returns")
            send(b" ")
            wait_for("zVram Manager")
            send(b"q")
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
