"""Small stdlib interfaces for zVram managed launch profiles.

Profiles affect wrapped launches only. Changed residency takes effect on restart.
"""
import shlex

PRIORITIES = ("high", "normal", "low")
NOTICE = "zVram launches only. External launches are read-only. Priority presets apply on next launch; an explicit MiB cap overrides them. Native mode has no cap."


def profile_from_fields(name, priority, resident_mib, command, mode="vulkan"):
    """Parse form text without invoking a shell."""
    name = name.strip()
    if not name:
        raise ValueError("Profile name is required")
    if priority not in PRIORITIES:
        raise ValueError("Priority must be high, normal or low")
    resident = int(resident_mib) if resident_mib is not None and str(resident_mib).strip() else None
    if resident is not None and resident <= 0:
        raise ValueError("Resident MiB must be positive, or blank for the priority default")
    if mode not in ("native", "vulkan", "wrapped"):
        raise ValueError("Mode must be native, vulkan or wrapped")
    arguments = shlex.split(command)
    if not arguments:
        raise ValueError("Command is required")
    return dict(name=name, priority=priority, resident_mib=resident, command=arguments, mode=mode)


def command_text(profile):
    command = profile.get("command", [])
    return shlex.join(command) if isinstance(command, list) else command


def status_text(profile):
    if profile.get("running"):
        return ("External · read-only (PID %s)" if profile.get("external") else "Running (PID %s)") % profile.get("pid", "?")
    return str(profile.get("state", "stopped")).capitalize()


def system_status(status):
    ram = "RAM available: %s MiB | Swap used: %s MiB" % (status.get("mem_available_mib", "?"), status.get("swap_used_mib", "?"))
    gpu = ["%s VRAM %s/%s MiB, GTT %s MiB" % (item.get("card", "GPU"), item.get("vram_used_mib", "?"), item.get("vram_total_mib", "?"), item.get("gtt_used_mib", "?")) for item in status.get("gpu", [])]
    return ram + (" | " + " | ".join(gpu) if gpu else "")


def profile_details(profile):
    value = lambda key: "—" if profile.get(key) is None else str(profile[key])
    text = "RSS %s MiB · DRM VRAM %s MiB · GTT %s MiB (logical; not physical residency)" % (value("rss_mib"), value("vram_mib"), value("gtt_mib"))
    if profile.get("external"):
        cap = "%s MiB" % profile["active_resident_mib"] if profile.get("active_resident_mib") is not None else "unknown"
        text += " | External zVram launch · read-only | " + profile.get("state", "Launch configured") + " | Configured cap: " + cap
    elif profile.get("mode") == "native":
        text += " | Native: no zVram residency cap"
    else:
        active = "%s MiB" % profile["active_resident_mib"] if profile.get("running") and profile.get("active_resident_mib") else "not running"
        pending = "%s MiB" % profile["resident_mib"] if profile.get("resident_mib") else profile.get("priority", "normal") + " preset"
        text += " | Active cap: %s · Next launch: %s" % (active, pending)
        if profile.get("mode") == "wrapped":
            text += " · existing zVram command"
    if profile.get("pid"):
        text += " | PID %s" % profile["pid"]
    if profile.get("lastlog"):
        text += " | " + profile["lastlog"]
    return text


# Novum Xenium static/style.css core palette; keep this stdlib app visually aligned.
NX = dict(bg="#100f14", fg="#e9e6ef", panel="#19171e", border="#37323e",
          accent="#a88bd6", muted="#b2aabc", input="#121016", selected="#3b2b51")


def gib(value):
    return "—" if value is None else "%.1f GiB" % (value / 1024)


class ManagerWindow:
    def __init__(self, root, manager):
        import tkinter as tk
        from tkinter import ttk
        self.root, self.manager = root, manager
        self.profiles, self.editing = {}, None
        self.read_only = False
        self.controls, self.field_widgets = [], {}
        root.title("zVram Manager")
        root.minsize(1080, 900)
        root.geometry("1200x920")
        root.configure(background=NX["bg"])
        root.option_add("*TCombobox*Listbox.background", NX["input"])
        root.option_add("*TCombobox*Listbox.foreground", NX["fg"])
        root.option_add("*TCombobox*Listbox.selectBackground", NX["selected"])
        style = ttk.Style(root)
        style.theme_use("clam")
        style.configure(".", background=NX["bg"], foreground=NX["fg"], font=("DejaVu Sans", 10))
        style.configure("TFrame", background=NX["bg"])
        style.configure("Card.TFrame", background=NX["panel"], bordercolor=NX["border"], lightcolor=NX["border"], darkcolor=NX["border"], relief="solid", borderwidth=1)
        style.configure("Panel.TFrame", background=NX["panel"])
        style.configure("TLabel", background=NX["bg"], foreground=NX["fg"])
        style.configure("Muted.TLabel", foreground=NX["muted"])
        style.configure("Card.TLabel", background=NX["panel"], foreground=NX["fg"])
        style.configure("CardMuted.TLabel", background=NX["panel"], foreground=NX["muted"])
        style.configure("CardValue.TLabel", background=NX["panel"], foreground=NX["fg"], font=("DejaVu Sans", 24, "bold"))
        style.configure("Title.TLabel", font=("DejaVu Sans", 25, "bold"))
        style.configure("Section.TLabel", background=NX["panel"], foreground=NX["fg"], font=("DejaVu Sans", 13, "bold"))
        style.configure("Badge.TLabel", background=NX["selected"], foreground=NX["accent"], padding=(10, 6), font=("DejaVu Sans", 9, "bold"))
        style.configure("TButton", background=NX["panel"], foreground=NX["fg"], bordercolor=NX["border"], lightcolor=NX["border"], darkcolor=NX["border"], padding=(12, 9), focusthickness=2, focuscolor=NX["accent"])
        style.map("TButton", background=[("active", NX["selected"])], bordercolor=[("focus", NX["accent"])])
        style.configure("Accent.TButton", background=NX["accent"], foreground=NX["bg"], font=("DejaVu Sans", 10, "bold"))
        style.map("Accent.TButton", background=[("active", "#c1a9e4")], foreground=[("active", NX["bg"])])
        style.configure("TEntry", fieldbackground=NX["input"], foreground=NX["fg"], insertcolor=NX["fg"], bordercolor=NX["border"], lightcolor=NX["border"], darkcolor=NX["border"], padding=9)
        style.map("TEntry", bordercolor=[("focus", NX["accent"])])
        style.configure("TCombobox", fieldbackground=NX["input"], foreground=NX["fg"], background=NX["panel"], arrowcolor=NX["accent"], bordercolor=NX["border"], lightcolor=NX["border"], darkcolor=NX["border"], padding=8)
        style.map("TCombobox", fieldbackground=[("readonly", NX["input"])], foreground=[("readonly", NX["fg"])], selectbackground=[("readonly", NX["selected"])], selectforeground=[("readonly", NX["fg"])])
        style.configure("Treeview", background=NX["panel"], fieldbackground=NX["panel"], foreground=NX["fg"], borderwidth=0, rowheight=40)
        style.layout("Treeview", [("Treeview.treearea", {"sticky": "nswe"})])
        style.configure("Treeview.Heading", background=NX["panel"], foreground=NX["muted"], borderwidth=0, padding=(8, 10), font=("DejaVu Sans", 9, "bold"))
        style.map("Treeview", background=[("selected", NX["selected"])], foreground=[("selected", NX["fg"])])
        style.map("Treeview.Heading", background=[("active", NX["selected"])])
        style.configure("Horizontal.TProgressbar", background=NX["accent"], troughcolor=NX["border"], bordercolor=NX["border"], lightcolor=NX["accent"], darkcolor=NX["accent"], borderwidth=0, thickness=5)
        root.columnconfigure(0, weight=1)
        root.rowconfigure(0, weight=1)
        body = ttk.Frame(root, padding=24)
        body.grid(sticky="nsew")
        body.columnconfigure(0, weight=1)
        body.rowconfigure(2, weight=1)
        header = ttk.Frame(body)
        header.grid(row=0, column=0, sticky="ew", pady=(0, 22))
        header.columnconfigure(0, weight=1)
        ttk.Label(header, text="zVram", style="Title.TLabel").grid(row=0, column=0, sticky="w")
        ttk.Label(header, text="Memory, on your terms.", style="Muted.TLabel").grid(row=1, column=0, sticky="w", pady=(4, 0))
        ttk.Label(header, text="NX  /  MEMORY CONTROL", style="Badge.TLabel").grid(row=0, column=1, rowspan=2, sticky="e")
        overview = ttk.Frame(body)
        overview.grid(row=1, column=0, sticky="ew", pady=(0, 20))
        self.cards = {}
        for index, (key, title, caption) in enumerate((("gpu", "PHYSICAL GPU VRAM", "Device-wide usage · not per-process accounting"),
                                                      ("ram", "SYSTEM RAM AVAILABLE", "Headroom for model spill and your desktop"),
                                                      ("swap", "SYSTEM SWAP USED", "System-wide · includes other applications"))):
            overview.columnconfigure(index, weight=1, uniform="cards")
            card = ttk.Frame(overview, padding=18, style="Card.TFrame")
            card.grid(row=0, column=index, sticky="nsew", padx=(0 if index == 0 else 8, 0 if index == 2 else 8))
            ttk.Label(card, text=title, style="CardMuted.TLabel", font=("DejaVu Sans", 9, "bold")).pack(anchor="w")
            value = tk.StringVar(value="—")
            ttk.Label(card, textvariable=value, style="CardValue.TLabel").pack(anchor="w", pady=(9, 8))
            bar = ttk.Progressbar(card, maximum=100, value=0)
            if key == "gpu":
                bar.pack(fill="x", pady=(0, 8))
            if key == "gpu":
                self.gpu_caption = tk.StringVar(value=caption)
                ttk.Label(card, textvariable=self.gpu_caption, style="CardMuted.TLabel", wraplength=315, font=("DejaVu Sans", 9)).pack(anchor="w")
            else:
                ttk.Label(card, text=caption, style="CardMuted.TLabel", wraplength=315, font=("DejaVu Sans", 9)).pack(anchor="w")
            self.cards[key] = (value, bar)
        workspace = ttk.Frame(body)
        workspace.grid(row=2, column=0, sticky="nsew")
        workspace.columnconfigure(0, weight=4)
        workspace.columnconfigure(1, weight=6)
        workspace.rowconfigure(0, weight=1)
        left = ttk.Frame(workspace, style="Card.TFrame", padding=16)
        left.grid(row=0, column=0, sticky="nsew", padx=(0, 16))
        left.columnconfigure(0, weight=1)
        left.rowconfigure(2, weight=1)
        titlebar = ttk.Frame(left, style="Panel.TFrame")
        titlebar.grid(row=0, column=0, sticky="ew")
        titlebar.columnconfigure(0, weight=1)
        ttk.Label(titlebar, text="Profiles & zVram apps", style="Section.TLabel").grid(row=0, column=0, sticky="w")
        ttk.Button(titlebar, text="+ New", command=self.new).grid(row=0, column=1, sticky="e")
        ttk.Label(left, text="External zVram launches appear read-only.", style="CardMuted.TLabel").grid(row=1, column=0, sticky="w", pady=(10, 12))
        self.table = ttk.Treeview(left, columns=("priority", "state"), height=7)
        self.table.heading("#0", text="APP / PROFILE", anchor="w")
        self.table.column("#0", width=175, minwidth=120, stretch=True)
        for key, label, width in (("priority", "PRIORITY", 76), ("state", "STATUS", 120)):
            self.table.heading(key, text=label, anchor="w")
            self.table.column(key, width=width, stretch=False)
        self.table.grid(row=2, column=0, sticky="nsew")
        self.table.bind("<<TreeviewSelect>>", self.select)
        leftactions = ttk.Frame(left, style="Panel.TFrame")
        leftactions.grid(row=3, column=0, sticky="ew", pady=(12, 0))
        for label, callback in (("Refresh", self.refresh), ("View logs", self.logs), ("Delete", self.delete)):
            button = ttk.Button(leftactions, text=label, command=callback)
            button.pack(side="left", padx=(0, 7))
            if label != "Refresh":
                self.controls.append(button)
        right = ttk.Frame(workspace, style="Card.TFrame", padding=20)
        right.grid(row=0, column=1, sticky="nsew")
        right.columnconfigure(0, weight=1)
        right.columnconfigure(1, weight=1)
        ttk.Label(right, text="Profile settings", style="Section.TLabel").grid(row=0, column=0, columnspan=2, sticky="w")
        ttk.Label(right, text="Save preferences, then launch when you are ready.", style="CardMuted.TLabel").grid(row=1, column=0, columnspan=2, sticky="w", pady=(5, 14))
        self.fields = {}
        fields = (("name", "Profile name", 2, 0, 2), ("priority", "VRAM priority", 4, 0, 1),
                  ("mode", "Launch mode", 4, 1, 1), ("resident_mib", "Resident MiB · blank uses priority preset", 6, 0, 2),
                  ("command", "Command · shell quoting supported; no shell invoked", 8, 0, 2))
        for key, label, row, column, span in fields:
            ttk.Label(right, text=label, style="CardMuted.TLabel", font=("DejaVu Sans", 9)).grid(row=row, column=column, columnspan=span, sticky="w", pady=(0, 5), padx=(0, 10 if span == 1 else 0))
            variable = tk.StringVar(value="normal" if key == "priority" else "vulkan" if key == "mode" else "")
            self.fields[key] = variable
            widget = ttk.Combobox(right, textvariable=variable, values=PRIORITIES if key == "priority" else ("native", "vulkan", "wrapped"), state="readonly") if key in ("priority", "mode") else ttk.Entry(right, textvariable=variable)
            self.field_widgets[key] = widget
            widget.grid(row=row + 1, column=column, columnspan=span, sticky="ew", pady=(0, 12), padx=(0, 10 if column == 0 and span == 1 else 0))
        actions = ttk.Frame(right, style="Panel.TFrame")
        actions.grid(row=10, column=0, columnspan=2, sticky="ew", pady=(2, 16))
        for label, callback, kind in (("Save settings", self.save, "TButton"), ("Start app", lambda: self.act("start"), "Accent.TButton"),
                                     ("Stop app", lambda: self.act("stop"), "TButton")):
            button = ttk.Button(actions, text=label, command=callback, style=kind)
            button.pack(side="left", padx=(0, 10))
            self.controls.append(button)
        ttk.Label(right, text="PROCESS ACCOUNTING", style="CardMuted.TLabel", font=("DejaVu Sans", 9, "bold")).grid(row=11, column=0, columnspan=2, sticky="w")
        self.details = tk.StringVar(value="Select a profile to inspect its process.")
        ttk.Label(right, textvariable=self.details, style="CardMuted.TLabel", wraplength=545, justify="left").grid(row=12, column=0, columnspan=2, sticky="nw", pady=(8, 0))
        self.telemetry = tk.StringVar(value="")
        footer = ttk.Frame(body)
        footer.grid(row=3, column=0, sticky="ew", pady=(16, 0))
        ttk.Label(footer, text=NOTICE, style="Muted.TLabel", wraplength=1120, font=("DejaVu Sans", 9)).pack(anchor="w")
        self.message = tk.StringVar(value="Ready · Ctrl+S saves settings · F5 refreshes")
        ttk.Label(footer, textvariable=self.message, wraplength=1120, font=("DejaVu Sans", 9)).pack(anchor="w", pady=(7, 0))
        root.bind("<Control-s>", lambda event: self.save())
        root.bind("<F5>", lambda event: self.refresh())
        self.refresh()
        self.root.after(2000, self.tick)

    def update_cards(self, status):
        gpus = status.get("gpu", [])
        primary = max(gpus, key=lambda gpu: gpu.get("vram_total_mib", 0) or 0, default={})
        used = primary.get("vram_used_mib")
        total = primary.get("vram_total_mib")
        self.cards["gpu"][0].set(gib(used) + " / " + gib(total) if total else "Unavailable")
        self.cards["gpu"][1]["value"] = min(100, used * 100 / total) if total and used is not None else 0
        captions = ["%s · VRAM %s/%s · GTT %s" % (gpu.get("card", "GPU"), gib(gpu.get("vram_used_mib")).removesuffix(" GiB"), gib(gpu.get("vram_total_mib")), gib(gpu.get("gtt_used_mib"))) for gpu in sorted(gpus, key=lambda gpu: gpu is not primary)]
        self.gpu_caption.set("\n".join(captions) if captions else "Device statistics unavailable")
        self.cards["ram"][0].set(gib(status.get("mem_available_mib")))
        self.cards["swap"][0].set(gib(status.get("swap_used_mib")))

    def selected(self):
        selection = self.table.selection()
        return selection[0] if selection else None

    def set_read_only(self, read_only):
        self.read_only = read_only
        for widget in self.controls:
            widget.configure(state="disabled" if read_only else "normal")
        for key, widget in self.field_widgets.items():
            widget.configure(state="disabled" if read_only else "readonly" if key in ("priority", "mode") else "normal")

    def blocked(self):
        if self.read_only or self.profiles.get(self.selected(), {}).get("external"):
            self.message.set("External zVram launch is read-only; manage it from its original launcher.")
            return True
        return False

    def select(self, event=None):
        profile = self.profiles.get(self.selected())
        self.set_read_only(bool(profile and profile.get("external")))
        if profile:
            self.details.set(profile_details(profile))
        if profile and self.editing != profile["name"]:
            self.editing = profile["name"]
            for key in self.fields:
                self.fields[key].set(command_text(profile) if key == "command" else profile.get("display_name", profile["name"]) if key == "name" else str(profile.get(key) if profile.get(key) is not None else ""))

    def new(self):
        self.set_read_only(False)
        self.editing = None
        self.table.selection_remove(*self.table.selection())
        for key, variable in self.fields.items():
            variable.set("normal" if key == "priority" else "vulkan" if key == "mode" else "")
        self.message.set("New profile: enter a name and command, then Save")

    def refresh(self):
        selected = self.selected()
        try:
            profiles = self.manager.list_profiles()
            self.profiles = {profile["name"]: profile for profile in profiles}
            self.table.delete(*self.table.get_children())
            for profile in profiles:
                self.table.insert("", "end", iid=profile["name"], text=profile.get("display_name", profile["name"]), values=(
                    profile["priority"].upper(), "External · read-only" if profile.get("external") else "Running" if profile.get("running") else status_text(profile)))
            if selected in self.profiles:
                self.table.selection_set(selected)
                self.select()
            else:
                self.details.set("Select a profile to inspect its process.")
            status = self.manager.status()
            self.telemetry.set(system_status(status))
            self.update_cards(status)
        except Exception as error:
            self.message.set("Error: %s" % error)

    def save(self):
        if self.blocked():
            return
        try:
            profile = profile_from_fields(**{key: value.get() for key, value in self.fields.items()})
            if self.editing in self.profiles:
                profile = dict(self.profiles[self.editing], **profile)
            self.manager.save_profile(profile)
            self.refresh()
            self.table.selection_set(profile["name"])
            self.message.set("Saved. Running processes keep their current residency until restarted.")
        except Exception as error:
            self.message.set("Error: %s" % error)

    def act(self, action):
        if self.blocked():
            return
        name = self.selected()
        if not name:
            self.message.set("Select a profile first")
            return
        try:
            getattr(self.manager, action)(name)
            self.refresh()
            self.message.set("%s: %s requested" % (name, action))
        except Exception as error:
            self.message.set("Error: %s" % error)

    def logs(self):
        if self.blocked():
            return
        import tkinter as tk
        from tkinter import ttk
        name = self.selected()
        if not name:
            self.message.set("Select a profile first")
            return
        try:
            text = self.manager.log_tail(name, lines=50)
            window = tk.Toplevel(self.root)
            window.title("zVram log · " + name)
            window.configure(background=NX["bg"])
            content = tk.Text(window, width=100, height=24, wrap="word", background=NX["input"], foreground=NX["fg"], insertbackground=NX["fg"], padx=16, pady=16, relief="flat")
            content.pack(fill="both", expand=True)
            content.insert("1.0", text or "No log output yet")
            content.configure(state="disabled")
            ttk.Button(window, text="Close", command=window.destroy).pack(pady=8)
        except Exception as error:
            self.message.set("Error: %s" % error)

    def delete(self):
        if self.blocked():
            return
        from tkinter import messagebox
        name = self.selected()
        if name and messagebox.askyesno("Delete profile", "Delete profile %s? Running jobs must be stopped first." % name, parent=self.root):
            try:
                self.manager.delete(name)
                self.new()
                self.refresh()
                self.message.set("Profile deleted")
            except Exception as error:
                self.message.set("Error: %s" % error)

    def tick(self):
        self.refresh()
        self.root.after(2000, self.tick)


def run_gui(manager):
    import tkinter as tk
    try:
        root = tk.Tk()
    except tk.TclError as error:
        raise RuntimeError("Cannot open GUI display; use zvram tui instead") from error
    ManagerWindow(root, manager)
    root.mainloop()


def _draw(screen, row, text, highlight=False):
    import curses
    height, width = screen.getmaxyx()
    if 0 <= row < height and width > 1:
        try:
            screen.addnstr(row, 0, text, width - 1, curses.A_REVERSE if highlight else curses.A_NORMAL)
        except curses.error:
            pass  # A terminal resize can invalidate a line while drawing.


def _prompt(screen, label, initial=""):
    import curses
    height, width = screen.getmaxyx()
    if height < 5 or width < 24:
        raise ValueError("Resize terminal to at least 24 columns and 5 rows")
    screen.timeout(-1)
    curses.curs_set(1)
    value = initial
    try:
        while True:
            screen.move(height - 2, 0)
            screen.clrtoeol()
            _draw(screen, height - 2, (label + value)[-(width - 1):])
            screen.refresh()
            key = screen.get_wch()
            if key in ("\n", "\r", curses.KEY_ENTER):
                return value
            if key == "\x1b":
                return None
            if key in (curses.KEY_BACKSPACE, "\x7f", "\b"):
                value = value[:-1]
            elif key == "\x15":
                value = ""
            elif isinstance(key, str) and key.isprintable():
                value += key
    finally:
        curses.curs_set(0)
        screen.timeout(1000)


def _edit(screen, profile=None):
    profile = profile or dict(name="", priority="normal", resident_mib=None, command=[], mode="vulkan")
    fields = {}
    for key, label in (("name", "Name: "), ("priority", "Priority [high/normal/low]: "),
                       ("resident_mib", "Resident MiB [blank=default]: "), ("mode", "Mode [native/vulkan/wrapped]: "), ("command", "Command: ")):
        initial = command_text(profile) if key == "command" else str(profile.get(key) if profile.get(key) is not None else "")
        value = _prompt(screen, label, initial)
        if value is None:
            return None
        fields[key] = value
    edited = profile_from_fields(**fields)
    return dict(profile, **edited)


def _tui(screen, manager):
    import curses
    curses.curs_set(0)
    screen.timeout(1000)
    index, offset, message = 0, 0, "Ready"
    while True:
        try:
            profiles = manager.list_profiles()
        except Exception as error:
            profiles = []
            message = "Error: %s" % error
        index = min(index, max(0, len(profiles) - 1))
        height, width = screen.getmaxyx()
        visible = max(1, height - 9)
        offset = max(0, min(offset, index))
        if index >= offset + visible:
            offset = index - visible + 1
        screen.erase()
        _draw(screen, 0, "zVram Manager | zVram launches only; external apps are read-only")
        _draw(screen, 1, "Profile                   Priority  Resident MiB  Status")
        for row, profile in enumerate(profiles[offset:offset + visible], 2):
            text = "%-25.25s %-9s %-13s %s" % (profile.get("display_name", profile["name"]), profile["priority"], (profile.get("active_resident_mib") if profile.get("active_resident_mib") is not None else "Unknown") if profile.get("external") else profile.get("resident_mib") if profile.get("resident_mib") is not None else "Default", status_text(profile))
            _draw(screen, row, text, row - 2 + offset == index)
        try:
            _draw(screen, height - 6, system_status(manager.status()))
            if profiles:
                _draw(screen, height - 5, profile_details(profiles[index]))
        except Exception as error:
            message = "Error: %s" % error
        _draw(screen, height - 4, "Arrows: select | n: new | e: edit | p: priority | s: start | x: stop | l: logs | d: delete | q: quit")
        _draw(screen, height - 3, "Edits: Enter accepts, Esc cancels, Ctrl-U clears. Residency applies next launch.")
        _draw(screen, height - 1, message)
        screen.refresh()
        key = screen.getch()
        if key in (ord("q"), 27):
            return
        if key == curses.KEY_UP:
            index = max(0, index - 1)
        elif key == curses.KEY_DOWN:
            index = min(max(0, len(profiles) - 1), index + 1)
        else:
            profile = profiles[index] if profiles else None
            try:
                if profile and profile.get("external") and key in map(ord, "epsxld"):
                    message = "External zVram launch is read-only; manage it from its original launcher."
                    continue
                if key == ord("n") or key == ord("e") and profile:
                    edited = _edit(screen, profile if key == ord("e") else None)
                    if edited:
                        manager.save_profile(edited)
                        message = "Saved; residency applies next launch"
                elif key == ord("p") and profile:
                    priority = PRIORITIES[(PRIORITIES.index(profile["priority"]) + 1) % len(PRIORITIES)]
                    manager.set_priority(profile["name"], priority)
                    message = "Priority saved; manual residency cleared, default applies next launch"
                elif key == ord("l") and profile:
                    screen.erase()
                    for row, line in enumerate(manager.log_tail(profile["name"], lines=max(1, height - 3)).splitlines()):
                        _draw(screen, row, line)
                    _draw(screen, height - 1, "Any key returns")
                    screen.refresh()
                    screen.timeout(-1)
                    screen.getch()
                    screen.timeout(1000)
                elif key == ord("d") and profile:
                    answer = _prompt(screen, "Delete profile? Type yes: ")
                    if answer == "yes":
                        manager.delete(profile["name"])
                        message = "Profile deleted"
                elif key in (ord("s"), ord("x")) and profile:
                    action = "start" if key == ord("s") else "stop"
                    getattr(manager, action)(profile["name"])
                    message = "%s: %s requested" % (profile["name"], action)
            except Exception as error:
                message = "Error: %s" % error


def run_tui(manager):
    import curses
    import sys
    if not sys.stdin.isatty() or not sys.stdout.isatty():
        raise RuntimeError("TUI requires an interactive terminal")
    curses.wrapper(_tui, manager)
