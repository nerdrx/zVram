"""Small stdlib interfaces for zVram managed launch profiles.

Profiles affect wrapped launches only. Changed residency takes effect on restart.
"""
import shlex
import textwrap
from pathlib import Path

PRIORITIES = ("high", "normal", "low")
NOTICE = "zVram launches only. Live controls set per-device residency caps, not global scheduling. Reductions can remain pending until resident data fits. Saved profile presets apply next launch; an explicit MiB cap overrides them. Native mode has no cap."


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
        return ("External zVram (PID %s)" if profile.get("external") else "Running (PID %s)") % profile.get("pid", "?")
    return str(profile.get("state", "stopped")).capitalize()


def control_issue(profile):
    state = profile.get("control_diagnostics", {}).get("state")
    return {
        "unreadable": "Control status cannot be read. Check the control directory's ownership and permissions; run the manager as the same user as the app.",
        "invalid": "Control status failed validation. Check the control directory and use matching manager/backend versions; unsafe status was ignored.",
        "stale": "Control status belongs to an expired process or disappeared during refresh. Refresh again before applying changes.",
        "partial": "Some GPU control statuses were rejected or unreadable. Live changes affect only the available devices; check the control directory and refresh.",
    }.get(state, "")


def table_state(profile):
    diagnostic = profile.get("control_diagnostics", {}).get("state")
    if diagnostic in ("unreadable", "invalid", "stale", "partial"):
        return {"unreadable": "Unreadable", "invalid": "Invalid status", "stale": "Stale status", "partial": "Partial control"}[diagnostic]
    if not profile.get('external'):
        return 'Running' if profile.get('running') else status_text(profile)
    if profile.get('live_capable'):
        return 'Live control'
    if not profile.get('backend_loaded', profile.get('state') == 'Layer loaded'):
        return 'Layer missing'
    return 'Paging off' if profile.get('control_devices') else 'No endpoint'


def total_gpu_usage_mib(status):
    """Sum device counters, never treating missing telemetry as zero."""
    gpus = status.get("gpu", [])
    values = [gpu.get(key) for gpu in gpus for key in ("vram_used_mib", "gtt_used_mib")]
    return sum(values) if values and all(value is not None for value in values) else None


def system_status(status):
    ram = "RAM available: %s MiB | Swap used: %s MiB" % (status.get("mem_available_mib", "?"), status.get("swap_used_mib", "?"))
    gpu = ["%s VRAM %s/%s MiB, GTT %s MiB" % (item.get("card", "GPU"), item.get("vram_used_mib", "?"), item.get("vram_total_mib", "?"), item.get("gtt_used_mib", "?")) for item in status.get("gpu", [])]
    return "Total GPU usage (VRAM + GTT): %s | " % gib(total_gpu_usage_mib(status)) + ram + (" | " + " | ".join(gpu) if gpu else "")


def profile_details(profile):
    value = lambda key: "—" if profile.get(key) is None else str(profile[key])
    text = "RSS %s MiB · DRM VRAM %s MiB · GTT %s MiB (logical; not physical residency)" % (value("rss_mib"), value("vram_mib"), value("gtt_mib"))
    if profile.get("external"):
        cap = "%s MiB" % profile["active_resident_mib"] if profile.get("active_resident_mib") is not None else "unknown"
        text += " | External zVram launch | " + profile.get("state", "Launch configured") + " | Configured cap: " + cap
        if not profile.get("live_capable"):
            if not profile.get('backend_loaded', profile.get('state') == 'Layer loaded'):
                text += " · zVram layer is NOT loaded; inherited launch settings alone do not enable paging"
                text += " · restart through updated zVram for live residency controls"
            elif control_issue(profile):
                pass  # The precise diagnostic is appended below.
            elif profile.get('control_devices'):
                text += " · paging was disabled at launch; it cannot be enabled on an existing Vulkan device"
                text += " · restart with live management enabled"
            else:
                text += " · no live endpoint yet: an idle model router may have no Vulkan device; select its model worker once loaded"
                text += " · older backends or a different control directory also need a corrected launch"
    elif profile.get("mode") == "native":
        text += " | Native: no zVram residency cap"
    else:
        active = "%s MiB" % profile["active_resident_mib"] if profile.get("running") and profile.get("active_resident_mib") else "automatic" if profile.get("running") else "not running"
        pending = "%s MiB" % profile["resident_mib"] if profile.get("resident_mib") else profile.get("priority", "normal") + " preset"
        text += " | Launch cap: %s · Next launch: %s" % (active, pending)
        if not profile.get('running'):
            text += " · Apply live requires a running app with paging enabled"
        if profile.get("mode") == "wrapped":
            text += " · existing zVram command"
    if profile.get("steam_priority"):
        text += " | Saved Steam priority: %s (next launch)" % profile["steam_priority"]
    if profile.get("live_priority"):
        text += " | Live priority: %s (%s) · remembered for this process" % (profile["live_priority"], profile.get("live_priority_state", "pending"))
    if control_issue(profile):
        text += " | " + control_issue(profile)
    for index, device in enumerate(profile.get("control_devices", []), 1):
        result = device.get("result", 0)
        state = {0: "applied/idle", 1: "pending", 2: "rejected"}.get(result, "unknown")
        text += " | GPU %s: cap %s MiB · resident %s MiB · requested %s MiB · %s" % (index, device.get("current_limit_mib", "?"), device.get("resident_mib", "?"), device.get("requested_mib", "?"), state)
        if result == 2:
            text += " (" + {1: 'paging unavailable', 2: 'outside supported bounds', 3: 'decoder memory reserve'}.get(device.get('reason'), 'request rejected') + ")"
        if device.get("recovery_capable") == 1:
            text += " · Recovery: " + ("prompt (safe completed buffers)" if device.get("recovery_priority") == 1 else "configured delay")
    if profile.get("pid"):
        text += " | PID %s" % profile["pid"]
    if profile.get("lastlog"):
        text += " | " + profile["lastlog"]
    return text


# Novum Xenium static/style.css core palette; keep this stdlib app visually aligned.
NX = dict(bg="#101116", fg="#eeeef5", panel="#181a22", border="#2a2d39",
          accent="#b9a0ef", muted="#a9afc1", input="#12141b", selected="#343047")


def gib(value):
    return "—" if value is None else "%.1f GiB" % (value / 1024)


def usage_mib(profile, key):
    value = profile.get(key)
    return "—" if value is None else "%.0f" % value


class ManagerWindow:
    def __init__(self, root, manager):
        import tkinter as tk
        from tkinter import ttk
        self.root, self.manager = root, manager
        self.profiles, self.editing = {}, None
        self.read_only = False
        self.controls, self.field_widgets = {}, {}
        root.title("zVram Manager")
        self.app_icon = tk.PhotoImage(master=root, file=str(Path(__file__).resolve().parent / "assets/icon.png"))
        self.header_icon = self.app_icon.subsample(3, 3)
        root.iconphoto(True, self.app_icon)
        root.minsize(1000, 800)
        root.geometry("1280x800")
        root.configure(background=NX["bg"])
        root.option_add("*TCombobox*Listbox.background", NX["input"])
        root.option_add("*TCombobox*Listbox.foreground", NX["fg"])
        root.option_add("*TCombobox*Listbox.selectBackground", NX["selected"])
        style = ttk.Style(root)
        style.theme_use("clam")
        style.configure(".", background=NX["bg"], foreground=NX["fg"], font=("DejaVu Sans", 10))
        style.configure("TFrame", background=NX["bg"])
        style.configure("Card.TFrame", background=NX["panel"], bordercolor=NX["border"], lightcolor=NX["border"], darkcolor=NX["border"], relief="flat", borderwidth=0)
        style.configure("Panel.TFrame", background=NX["panel"])
        style.configure("TLabel", background=NX["bg"], foreground=NX["fg"])
        style.configure("Muted.TLabel", foreground=NX["muted"])
        style.configure("Card.TLabel", background=NX["panel"], foreground=NX["fg"])
        style.configure("CardMuted.TLabel", background=NX["panel"], foreground=NX["muted"])
        style.configure("CardValue.TLabel", background=NX["panel"], foreground=NX["fg"], font=("DejaVu Sans", 19, "bold"))
        style.configure("Title.TLabel", font=("DejaVu Sans", 22, "bold"))
        style.configure("Section.TLabel", background=NX["panel"], foreground=NX["fg"], font=("DejaVu Sans", 13, "bold"))
        style.configure("Card.TCheckbutton", background=NX["panel"], foreground=NX["muted"])
        style.map("Card.TCheckbutton", background=[("active", NX["panel"])], foreground=[("disabled", NX["muted"])])
        style.configure("Badge.TLabel", background=NX["selected"], foreground=NX["accent"], padding=(10, 6), font=("DejaVu Sans", 9, "bold"))
        style.configure("TButton", background=NX["panel"], foreground=NX["fg"], bordercolor=NX["border"], lightcolor=NX["border"], darkcolor=NX["border"], padding=(12, 9), focusthickness=2, focuscolor=NX["accent"])
        style.map("TButton", background=[("active", NX["selected"])], bordercolor=[("focus", NX["accent"])])
        style.configure("Accent.TButton", background=NX["accent"], foreground=NX["bg"], font=("DejaVu Sans", 10, "bold"))
        style.map("Accent.TButton", background=[("disabled", NX["panel"]), ("active", "#c1a9e4")],
                  foreground=[("disabled", NX["muted"]), ("active", NX["bg"])])
        style.configure("TEntry", fieldbackground=NX["input"], foreground=NX["fg"], insertcolor=NX["fg"], bordercolor=NX["border"], lightcolor=NX["border"], darkcolor=NX["border"], padding=8)
        style.map("TEntry", bordercolor=[("focus", NX["accent"])])
        style.configure("TCombobox", fieldbackground=NX["input"], foreground=NX["fg"], background=NX["panel"], arrowcolor=NX["accent"], bordercolor=NX["border"], lightcolor=NX["border"], darkcolor=NX["border"], padding=8)
        style.map("TCombobox", fieldbackground=[("readonly", NX["input"])], foreground=[("readonly", NX["fg"])], selectbackground=[("readonly", NX["selected"])], selectforeground=[("readonly", NX["fg"])])
        style.configure("Treeview", background=NX["panel"], fieldbackground=NX["panel"], foreground=NX["fg"], borderwidth=1, bordercolor=NX["border"], lightcolor=NX["border"], rowheight=44)
        style.configure("Treeview.Heading", background=NX["panel"], foreground=NX["muted"], borderwidth=0, padding=(8, 10), font=("DejaVu Sans", 9, "bold"))
        style.map("Treeview", background=[("selected", NX["selected"])], foreground=[("selected", NX["fg"])],
                  bordercolor=[("focus", NX["accent"])], lightcolor=[("focus", NX["accent"])])
        style.map("Treeview.Heading", background=[("active", NX["selected"])])
        style.configure("Horizontal.TProgressbar", background=NX["accent"], troughcolor=NX["border"], bordercolor=NX["border"], lightcolor=NX["accent"], darkcolor=NX["accent"], borderwidth=0, thickness=5)
        root.columnconfigure(0, weight=1)
        root.rowconfigure(0, weight=1)
        body = ttk.Frame(root, padding=20)
        body.grid(sticky="nsew")
        body.columnconfigure(0, weight=1)
        body.rowconfigure(2, weight=1)
        header = ttk.Frame(body)
        header.grid(row=0, column=0, sticky="ew", pady=(0, 10))
        header.columnconfigure(0, weight=1)
        ttk.Label(header, text="zVram", image=self.header_icon, compound="left", style="Title.TLabel").grid(row=0, column=0, sticky="w")
        ttk.Label(header, text="NX  /  zVram Manager", style="Badge.TLabel").grid(row=0, column=1, sticky="e")
        overview = ttk.Frame(body)
        overview.grid(row=1, column=0, sticky="ew", pady=(0, 16))
        self.cards = {}
        for index, (key, title, caption) in enumerate((("gpu", "TOTAL GPU USAGE", "VRAM + GTT · all GPUs"),
                                                      ("ram", "AVAILABLE RAM", "Space for your apps and desktop"),
                                                      ("swap", "SWAP IN USE", "Across all applications"))):
            overview.columnconfigure(index, weight=1, uniform="cards")
            card = ttk.Frame(overview, padding=(16, 12), style="Card.TFrame")
            card.grid(row=0, column=index, sticky="nsew", padx=(0 if index == 0 else 8, 0 if index == 2 else 8))
            ttk.Label(card, text=title, style="CardMuted.TLabel", font=("DejaVu Sans", 9, "bold")).pack(anchor="w")
            value = tk.StringVar(value="—")
            ttk.Label(card, textvariable=value, style="CardValue.TLabel").pack(anchor="w", pady=(6, 6))
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
        workspace.columnconfigure(0, weight=1)
        workspace.columnconfigure(1, weight=0, minsize=340)
        workspace.rowconfigure(0, weight=1)
        left = ttk.Frame(workspace, style="Card.TFrame", padding=16)
        left.grid(row=0, column=0, sticky="nsew", padx=(0, 14))
        left.columnconfigure(0, weight=1)
        left.rowconfigure(2, weight=1)
        titlebar = ttk.Frame(left, style="Panel.TFrame")
        titlebar.grid(row=0, column=0, sticky="ew")
        titlebar.columnconfigure(0, weight=1)
        ttk.Label(titlebar, text="Your apps", style="Section.TLabel").grid(row=0, column=0, sticky="w")
        ttk.Button(titlebar, text="+ New", command=self.new).grid(row=0, column=1, sticky="e")
        ttk.Label(left, text="Live allocations in MiB · select an app to manage", style="CardMuted.TLabel").grid(row=1, column=0, sticky="w", pady=(10, 12))
        table_area = ttk.Frame(left, style="Panel.TFrame")
        table_area.grid(row=2, column=0, sticky="nsew")
        table_area.columnconfigure(0, weight=1)
        table_area.rowconfigure(0, weight=1)
        self.table = ttk.Treeview(table_area, columns=("vram", "gtt", "rss", "priority", "state"), height=7)
        self.table.heading("#0", text="APP / PROFILE", anchor="w")
        self.table.column("#0", width=150, minwidth=115, stretch=True)
        for key, label, width in (("vram", "VRAM", 78), ("gtt", "GTT", 72),
                                  ("rss", "RAM", 72), ("priority", "PRIORITY", 155),
                                  ("state", "STATUS", 100)):
            self.table.heading(key, text=label, anchor="w")
            self.table.column(key, width=width, minwidth=width, stretch=False, anchor="e" if key in ("vram", "gtt", "rss") else "w")
        self.table.grid(row=0, column=0, sticky="nsew")
        vertical = ttk.Scrollbar(table_area, orient="vertical", command=self.table.yview)
        vertical.grid(row=0, column=1, sticky="ns")
        horizontal = ttk.Scrollbar(table_area, orient="horizontal", command=self.table.xview)
        horizontal.grid(row=1, column=0, sticky="ew")
        self.table.configure(yscrollcommand=vertical.set, xscrollcommand=horizontal.set)
        self.table.bind("<<TreeviewSelect>>", self.select)
        leftactions = ttk.Frame(left, style="Panel.TFrame")
        leftactions.grid(row=3, column=0, sticky="ew", pady=(12, 0))
        for label, callback in (("Refresh", self.refresh), ("View logs", self.logs), ("Delete", self.delete)):
            button = ttk.Button(leftactions, text=label, command=callback)
            button.pack(side="left", padx=(0, 7))
            if label != "Refresh":
                self.controls[label] = button
        right = ttk.Frame(workspace, style="Card.TFrame", padding=16)
        right.grid(row=0, column=1, sticky="nsew")
        right.columnconfigure(0, weight=1)
        right.rowconfigure(2, weight=1)
        self.inspector_title = tk.StringVar(value="App controls")
        ttk.Label(right, textvariable=self.inspector_title, style="Section.TLabel", wraplength=300).grid(row=0, column=0, sticky="w", pady=(0, 4))
        self.inspector_hint = tk.StringVar(value="Select an app or create a profile")
        ttk.Label(right, textvariable=self.inspector_hint, style="CardMuted.TLabel", wraplength=300).grid(row=1, column=0, sticky="w", pady=(0, 10))
        style.configure("TNotebook", background=NX["panel"], borderwidth=0, bordercolor=NX["panel"], lightcolor=NX["panel"], darkcolor=NX["panel"])
        style.configure("TNotebook.Tab", background=NX["panel"], foreground=NX["muted"], padding=(8, 8), borderwidth=0, bordercolor=NX["panel"], lightcolor=NX["panel"], darkcolor=NX["panel"])
        style.map("TNotebook.Tab", background=[("selected", NX["selected"])], foreground=[("selected", NX["accent"])])
        style.configure("TScrollbar", background=NX["border"], troughcolor=NX["panel"], arrowcolor=NX["muted"], borderwidth=0, lightcolor=NX["border"], darkcolor=NX["border"], bordercolor=NX["panel"])
        style.layout("TNotebook.Tab", [("Notebook.padding", {"sticky": "nswe", "children": [
            ("Notebook.focus", {"sticky": "nswe", "children": [("Notebook.label", {"sticky": ""})]})]})])
        for scrollbar in ("Vertical.TScrollbar", "Horizontal.TScrollbar"):
            style.configure(scrollbar, background=NX["border"], troughcolor=NX["panel"],
                            arrowcolor=NX["muted"], bordercolor=NX["panel"],
                            lightcolor=NX["border"], darkcolor=NX["border"], gripcount=0)
            style.map(scrollbar, background=[("active", NX["muted"]), ("!active", NX["border"])])
        self.inspector = ttk.Notebook(right)
        self.inspector.grid(row=2, column=0, sticky="nsew")
        controls_tab = ttk.Frame(self.inspector, style="Panel.TFrame", padding=(2, 12))
        launch_tab = ttk.Frame(self.inspector, style="Panel.TFrame", padding=(2, 12))
        details_tab = ttk.Frame(self.inspector, style="Panel.TFrame", padding=(2, 12))
        safety_tab = ttk.Frame(self.inspector, style="Panel.TFrame", padding=(2, 12))
        for tab, label in ((controls_tab, "Controls"), (launch_tab, "Launch"), (details_tab, "Details"), (safety_tab, "Safety")):
            self.inspector.add(tab, text=label)
            tab.columnconfigure(0, weight=1)
        self.fields = {}
        self.swap_guard = tk.BooleanVar(value=False)
        fields = (("priority", "VRAM priority", controls_tab, 0),
                  ("resident_mib", "Custom limit · MiB", controls_tab, 3),
                  ("name", "Profile name", launch_tab, 0),
                  ("mode", "Launch mode", launch_tab, 2),
                  ("command", "Command", launch_tab, 4))
        for key, label, tab, row in fields:
            ttk.Label(tab, text=label, style="CardMuted.TLabel").grid(row=row, column=0, sticky="w", pady=(0, 6))
            variable = tk.StringVar(value="normal" if key == "priority" else "vulkan" if key == "mode" else "")
            self.fields[key] = variable
            widget = ttk.Combobox(tab, width=24, textvariable=variable, values=PRIORITIES if key == "priority" else ("native", "vulkan", "wrapped"), state="readonly") if key in ("priority", "mode") else ttk.Entry(tab, width=24, textvariable=variable)
            self.field_widgets[key] = widget
            if key == "priority":
                widget.bind("<<ComboboxSelected>>", lambda event: self.fields["resident_mib"].set(""))
            widget.grid(row=row + 1, column=0, sticky="ew", pady=(0, 10))
        ttk.Label(controls_tab, text="A preset clears the custom limit.", style="CardMuted.TLabel", wraplength=290).grid(row=2, column=0, sticky="w", pady=(0, 10))
        ttk.Label(controls_tab, text="Blank uses the preset. Available memory and safety checks still apply.", style="CardMuted.TLabel", wraplength=290).grid(row=5, column=0, sticky="w")
        guard = ttk.Checkbutton(safety_tab, text="Ignore swap-growth guard", variable=self.swap_guard, style="Card.TCheckbutton")
        guard.grid(row=0, column=0, sticky="w", pady=(0, 6))
        ttk.Label(safety_tab, text="The available-RAM safety floor stays active. Leave this override off unless you understand the memory pressure risk.", style="CardMuted.TLabel", wraplength=290).grid(row=1, column=0, sticky="w")
        self.field_widgets["ignore_swap_guard"] = guard
        actions = ttk.Frame(right, style="Panel.TFrame")
        actions.grid(row=3, column=0, sticky="ew", pady=(14, 0))
        for column in (0, 1):
            actions.columnconfigure(column, weight=1, uniform="actions")
        for index, (label, callback, kind) in enumerate((("Save settings", self.save, "TButton"), ("Apply live", self.apply_live, "Accent.TButton"),
                                     ("Start app", lambda: self.act("start"), "TButton"), ("Stop app", lambda: self.act("stop"), "TButton"))):
            button = ttk.Button(actions, text=label, command=callback, style=kind, width=0, padding=(8, 9))
            button.grid(row=index // 2, column=index % 2, sticky="ew", padx=(0 if index % 2 == 0 else 4, 4 if index % 2 == 0 else 0), pady=(0, 8))
            self.controls[label] = button
        self.details = tk.StringVar(value="Select a profile to inspect its process.")
        details_tab.rowconfigure(0, weight=1)
        details_area = ttk.Frame(details_tab, style="Panel.TFrame")
        details_area.grid(row=0, column=0, sticky="nsew")
        details_area.columnconfigure(0, weight=1)
        details_area.rowconfigure(0, weight=1)
        self.details_text = tk.Text(details_area, height=3, width=1, wrap="word", relief="flat",
                                    background=NX["panel"], foreground=NX["muted"],
                                    font=("DejaVu Sans", 9), highlightthickness=1,
                                    highlightbackground=NX["panel"], highlightcolor=NX["accent"])
        self.details_text.grid(row=0, column=0, sticky="nsew")
        details_scroll = ttk.Scrollbar(details_area, orient="vertical", command=self.details_text.yview)
        details_scroll.grid(row=0, column=1, sticky="ns")
        self.details_text.configure(yscrollcommand=details_scroll.set)
        def update_details(*args):
            text = self.details.get().replace(" | ", "\n\n")
            if self.details_text.get("1.0", "end-1c") == text:
                return
            self.details_text.configure(state="normal")
            self.details_text.delete("1.0", "end")
            self.details_text.insert("1.0", text)
            self.details_text.configure(state="disabled")
        self.details.trace_add("write", update_details)
        update_details()
        self.telemetry = tk.StringVar(value="")
        footer = ttk.Frame(body)
        footer.grid(row=3, column=0, sticky="ew", pady=(16, 0))
        ttk.Label(footer, text="Device allocations, not a physical VRAM guarantee. Live changes may wait for safe headroom.", style="Muted.TLabel", wraplength=1120, font=("DejaVu Sans", 9)).pack(anchor="w")
        self.message = tk.StringVar(value="Ready · Ctrl+S saves settings · F5 refreshes")
        ttk.Label(footer, textvariable=self.message, wraplength=1120, font=("DejaVu Sans", 9)).pack(anchor="w", pady=(7, 0))
        root.protocol("WM_DELETE_WINDOW", self.close)
        root.bind("<Control-s>", lambda event: self.save())
        root.bind("<F5>", lambda event: self.refresh())
        self.mark_form_clean()
        self.refresh()
        self.root.after(2000, self.tick)

    def update_cards(self, status):
        gpus = status.get("gpu", [])
        primary = max(gpus, key=lambda gpu: gpu.get("vram_total_mib", 0) or 0, default={})
        used = primary.get("vram_used_mib")
        total = primary.get("vram_total_mib")
        self.cards["gpu"][0].set(gib(total_gpu_usage_mib(status)))
        self.cards["gpu"][1]["value"] = min(100, used * 100 / total) if total and used is not None else 0
        captions = ["%s · VRAM %s/%s · GTT %s" % (gpu.get("card", "GPU"), gib(gpu.get("vram_used_mib")).removesuffix(" GiB"), gib(gpu.get("vram_total_mib")), gib(gpu.get("gtt_used_mib"))) for gpu in sorted(gpus, key=lambda gpu: gpu is not primary)]
        self.gpu_caption.set("VRAM + GTT · all GPUs\nBar: primary GPU VRAM\n" + "\n".join(captions) if captions else "Device statistics unavailable")
        self.cards["ram"][0].set(gib(status.get("mem_available_mib")))
        self.cards["swap"][0].set(gib(status.get("swap_used_mib")))

    def selected(self):
        selection = self.table.selection()
        return selection[0] if selection else None

    def form_values(self):
        return {**{key: value.get() for key, value in self.fields.items()},
                "ignore_swap_guard": self.swap_guard.get()}

    def mark_form_clean(self):
        self.form_baseline = self.form_values()

    def form_is_dirty(self):
        return getattr(self, "form_baseline", None) is not None and self.form_values() != self.form_baseline

    def confirm_discard(self):
        if not self.form_is_dirty():
            return True
        from tkinter import messagebox
        return messagebox.askyesno("Discard unsaved changes?", "Discard your unsaved profile changes?", parent=self.root)

    def close(self):
        if self.confirm_discard():
            self.root.destroy()

    def clear_form(self):
        self.inspector_title.set("New profile")
        self.inspector_hint.set("Configure your app, then save")
        self.configure_controls()
        self.editing = None
        for key, variable in self.fields.items():
            variable.set("normal" if key == "priority" else "vulkan" if key == "mode" else "")
        self.swap_guard.set(False)
        self.mark_form_clean()

    def configure_controls(self, profile=None):
        external = bool(profile and profile.get("external"))
        live = bool(profile and profile.get("live_capable"))
        self.read_only = external
        for label, widget in self.controls.items():
            enabled = not external or label == "Stop app" or label == "Save settings" and live
            if label == "Apply live":
                enabled = live
            widget.configure(state="normal" if enabled else "disabled")
        self.controls["Save settings"].configure(text="Save settings")
        if external:
            self.controls["Save settings"].grid_remove()
        else:
            self.controls["Save settings"].grid()
        self.inspector.tab(1, state="disabled" if external else "normal")
        self.inspector.tab(3, state="disabled" if external else "normal")
        for key, widget in self.field_widgets.items():
            enabled = (not external or live and key in ("priority", "resident_mib")) if key != "ignore_swap_guard" else not external
            widget.configure(state="disabled" if not enabled else "readonly" if key in ("priority", "mode") or key == "name" and profile else "normal")

    def blocked(self, action=None):
        if self.read_only or self.profiles.get(self.selected(), {}).get("external"):
            if action == "stop" and self.selected() in self.profiles:
                return False
            self.message.set("External launch: use live cap controls or stop; no saved profile is changed.")
            return True
        return False

    def apply_live(self):
        profile = self.profiles.get(self.selected())
        if not profile or not profile.get("live_capable"):
            self.message.set("Restart through updated zVram for live residency controls.")
            return
        try:
            resident = self.fields["resident_mib"].get().strip()
            if resident:
                self.manager.set_live_limit(profile["name"], int(resident))
            else:
                priority = self.fields["priority"].get()
                if priority not in PRIORITIES:
                    self.message.set("Choose a priority or enter a residency cap before applying live changes.")
                    return
                self.manager.set_live_priority(profile["name"], priority)
            self.refresh()
            if profile.get("external"):
                self.mark_form_clean()
            self.message.set("Live change requested. Check device acknowledgement; reductions may remain pending.")
        except Exception as error:
            self.message.set("Error: %s" % error)

    def select(self, event=None, confirm=True):
        profile = self.profiles.get(self.selected())
        if profile and self.editing != profile["name"] and self.form_is_dirty() and (not confirm or not self.confirm_discard()):
            if self.editing in self.profiles and self.table.exists(self.editing):
                self.table.selection_set(self.editing)
            else:
                self.table.selection_remove(*self.table.selection())
            return
        self.configure_controls(profile)
        self.inspector_title.set(profile.get("display_name", profile["name"]) if profile else "App controls")
        self.inspector_hint.set("Steam preferences remembered" if profile and profile.get("steam_app_id") else "Live controls for this process" if profile and profile.get("external") else "Saved launch profile" if profile else "Select an app or create a profile")
        if profile:
            self.details.set(profile_details(profile))
        if profile and self.editing != profile["name"]:
            self.swap_guard.set(bool(profile.get("ignore_swap_guard", False)))
            self.editing = profile["name"]
            for key in self.fields:
                if key == "priority" and profile.get("external"):
                    self.fields[key].set(profile.get("live_priority") or profile.get("steam_priority") or (profile.get("priority") if profile.get("priority") in PRIORITIES else ""))
                    continue
                self.fields[key].set(command_text(profile) if key == "command" else profile.get("display_name", profile["name"]) if key == "name" else str(profile.get(key) if profile.get(key) is not None else ""))
            self.mark_form_clean()

    def new(self):
        if not self.confirm_discard():
            return
        self.table.selection_remove(*self.table.selection())
        self.clear_form()
        self.message.set("New profile: enter a name and command, then Save")

    def refresh(self):
        selected = self.selected()
        focused = self.table.focus()
        previous = self.profiles.get(selected)
        try:
            profiles = self.manager.list_profiles()
            self.profiles = {profile["name"]: profile for profile in profiles}
            self.table.delete(*self.table.get_children())
            for profile in profiles:
                self.table.insert("", "end", iid=profile["name"], text=profile.get("display_name", profile["name"]), values=(
                    usage_mib(profile, "vram_mib"), usage_mib(profile, "gtt_mib"),
                    usage_mib(profile, "rss_mib"), (profile["live_priority"] + " · " + profile.get("live_priority_state", "pending")).upper() if profile.get("live_priority") else profile["priority"].upper(), table_state(profile)))
            if focused in self.profiles:
                self.table.focus(focused)
            if selected in self.profiles:
                self.table.selection_set(selected)
                self.select(confirm=False)
            else:
                if selected is not None:
                    preserve_draft = bool(previous and not previous.get("external") and self.form_is_dirty())
                    self.table.selection_remove(*self.table.selection())
                    self.configure_controls()
                    self.editing = None
                    if not preserve_draft:
                        for key, variable in self.fields.items():
                            variable.set("normal" if key == "priority" else "vulkan" if key == "mode" else "")
                        self.swap_guard.set(False)
                        self.mark_form_clean()
                    else:
                        self.form_baseline = {"name": "", "priority": "normal", "mode": "vulkan",
                                              "resident_mib": "", "command": "", "ignore_swap_guard": False}
                    self.message.set("The selected app or profile is no longer available. Select another item or create a profile.")
                self.details.set("Select a profile to inspect its process.")
            status = self.manager.status()
            self.telemetry.set(system_status(status))
            self.update_cards(status)
        except Exception as error:
            self.message.set("Error: %s" % error)

    def save(self):
        if self.profiles.get(self.selected(), {}).get("external"):
            self.apply_live()
            return
        if self.blocked():
            return
        try:
            profile = profile_from_fields(**{key: value.get() for key, value in self.fields.items()})
            profile["ignore_swap_guard"] = self.swap_guard.get()
            if self.editing in self.profiles:
                if profile["name"] != self.editing:
                    raise ValueError("Existing profile names are fixed. Use New profile to create a different one.")
                profile = dict(self.profiles[self.editing], **profile)
            self.manager.save_profile(profile)
            self.refresh()
            self.mark_form_clean()
            self.table.selection_set(profile["name"])
            if self.profiles.get(profile["name"], {}).get("live_capable"):
                try:
                    if profile.get("resident_mib") is not None:
                        self.manager.set_live_limit(profile["name"], profile["resident_mib"])
                    else:
                        self.manager.set_live_priority(profile["name"], profile["priority"])
                    self.message.set("Profile saved. Live change requested; check device status for acknowledgement.")
                except Exception as error:
                    self.message.set("Profile saved, but live change failed: %s" % error)
            else:
                self.message.set("Profile saved for the next launch. No live change applied.")
        except Exception as error:
            self.message.set("Error: %s" % error)

    def act(self, action):
        if self.blocked(action):
            return
        if action == "start" and self.form_is_dirty():
            self.message.set("Save your profile changes before starting the app.")
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
        if name and messagebox.askyesno("Delete profile", "Delete profile %s and discard its unsaved edits? Running jobs must be stopped first." % name, parent=self.root):
            try:
                self.manager.delete(name)
                self.table.selection_remove(*self.table.selection())
                self.clear_form()
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
        root = tk.Tk(className="zVram")
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
    cursor = len(value)
    try:
        while True:
            screen.move(height - 2, 0)
            screen.clrtoeol()
            caret = len(label) + cursor
            start = max(0, caret - (width - 2))
            _draw(screen, height - 2, (label + value)[start:])
            screen.move(height - 2, caret - start)
            screen.refresh()
            key = screen.get_wch()
            if key in ("\n", "\r", curses.KEY_ENTER):
                return value
            if key == "\x1b":
                return None
            if key in (curses.KEY_BACKSPACE, "\x7f", "\b"):
                if cursor:
                    value = value[:cursor - 1] + value[cursor:]
                    cursor -= 1
            elif key == curses.KEY_LEFT:
                cursor = max(0, cursor - 1)
            elif key == curses.KEY_RIGHT:
                cursor = min(len(value), cursor + 1)
            elif key == curses.KEY_HOME:
                cursor = 0
            elif key == curses.KEY_END:
                cursor = len(value)
            elif key == curses.KEY_DC:
                value = value[:cursor] + value[cursor + 1:]
            elif key == "\x15":
                value = ""
                cursor = 0
            elif isinstance(key, str) and key.isprintable():
                value = value[:cursor] + key + value[cursor:]
                cursor += len(key)
    finally:
        curses.curs_set(0)
        screen.timeout(1000)


def _edit(screen, profile=None):
    existing_name = profile["name"] if profile else None
    profile = profile or dict(name="", priority="normal", resident_mib=None, command=[], mode="vulkan")
    fields = {}
    for key, label in (("name", "Name: "), ("priority", "Priority [high/normal/low]: "),
                       ("resident_mib", "Resident MiB [blank=default]: "), ("mode", "Mode [native/vulkan/wrapped]: "), ("command", "Command: ")):
        if key == "name" and existing_name is not None:
            fields[key] = existing_name
            continue
        initial = command_text(profile) if key == "command" else str(profile.get(key) if profile.get(key) is not None else "")
        value = _prompt(screen, label, initial)
        if value is None:
            return None
        fields[key] = value
    edited = profile_from_fields(**fields)
    return dict(profile, **edited)


def _details(screen, profile):
    """Read-only, wrapped details; usable even when the main table is narrow."""
    import curses
    offset = 0
    screen.timeout(-1)
    try:
        while True:
            height, width = screen.getmaxyx()
            lines = []
            for section in profile_details(profile).split(" | "):
                lines.extend(textwrap.wrap(section, max(1, width - 2)) or [""])
            visible = max(1, height - 3)
            offset = min(offset, max(0, len(lines) - visible))
            screen.erase()
            _draw(screen, 0, "App details · " + profile.get("display_name", profile["name"]))
            for row, line in enumerate(lines[offset:offset + visible], 1):
                _draw(screen, row, line)
            _draw(screen, height - 1, "Up/Down: scroll · Esc/q: back")
            screen.refresh()
            key = screen.getch()
            if key in (27, ord("q")):
                return
            if key == curses.KEY_DOWN:
                offset = min(offset + 1, max(0, len(lines) - visible))
            elif key == curses.KEY_UP:
                offset = max(0, offset - 1)
            elif key == curses.KEY_NPAGE:
                offset = min(offset + visible, max(0, len(lines) - visible))
            elif key == curses.KEY_PPAGE:
                offset = max(0, offset - visible)
    finally:
        screen.timeout(1000)


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
        _draw(screen, 0, "zVram Manager | zVram launches only; external e/p: live cap, x: stop")
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
        _draw(screen, height - 4, "Arrows: select | n: new | e: edit | i: details | p: priority | s: start | x: stop | l: logs | d: delete | q: quit")
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
                if profile and profile.get("external") and key in map(ord, "espld"):
                    if key in map(ord, "ep") and profile.get("live_capable"):
                        if key == ord("e"):
                            value = _prompt(screen, "Live resident cap MiB: ")
                            if value is not None:
                                manager.set_live_limit(profile["name"], int(value))
                        else:
                            priority = _prompt(screen, "Live priority [high/normal/low]: ", "normal")
                            if priority is not None:
                                manager.set_live_priority(profile["name"], priority)
                        message = "Live cap requested; reduction may remain pending until resident data fits"
                    else:
                        message = "External launch: restart through updated zVram for live residency controls" if key in map(ord, "ep") else "External launch has no saved profile or manager log"
                    continue
                if key == ord("i") and profile:
                    _details(screen, profile)
                elif key == ord("n") or key == ord("e") and profile:
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
