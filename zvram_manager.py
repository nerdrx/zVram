"""Local launch profiles and owned-process supervision. No global GPU scheduler."""
import argparse
import contextlib
import fcntl
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import sys
import time
import uuid

ROOT = Path(__file__).resolve().parent
PRIORITIES = {"high": 0.85, "normal": 0.5, "low": 0.25}


def private_directory(path):
    path = Path(path)
    path.mkdir(parents=True, exist_ok=True, mode=0o700)
    st = path.lstat()
    if path.is_symlink() or st.st_uid != os.getuid() or not path.is_dir():
        raise ValueError("Manager directory must be owned by this user, without a symlink")
    path.chmod(0o700)
    return path


def write_json(path, value):
    temp = path.with_name(path.name + "." + uuid.uuid4().hex)
    fd = os.open(temp, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(fd, "w") as stream:
        json.dump(value, stream, indent=2)
    os.replace(temp, path)


def read_json(path, fallback):
    try:
        if path.is_symlink() or path.stat().st_uid != os.getuid():
            raise ValueError("Unsafe manager state file")
        return json.loads(path.read_text())
    except FileNotFoundError:
        return fallback


def identity(pid):
    try:
        proc = Path("/proc") / str(pid)
        return {"pid": pid, "start": proc.joinpath("stat").read_text().rsplit(")", 1)[1].split()[19],
                "uid": proc.stat().st_uid, "boot": Path("/proc/sys/kernel/random/boot_id").read_text().strip()}
    except (OSError, ValueError, IndexError):
        return None


def owned_worker(job):
    saved = job.get("identity")
    if not saved or saved != identity(saved["pid"]) or saved["uid"] != os.getuid():
        return False
    try:
        argv = [item for item in Path(f'/proc/{saved["pid"]}/cmdline').read_bytes().split(b"\0") if item]
        script = job.get("worker_script") or (os.fsdecode(argv[1]) if len(argv) > 1 else "")
        return (len(argv) == 5 and Path(script).name == "zvram_manager.py" and argv[1] == os.fsencode(script)
                and argv[2:] == [b"_worker", job.get("profile", {}).get("name", "").encode(), job["token"].encode()])
    except OSError:
        return False


def memory_status():
    values = {}
    for line in Path("/proc/meminfo").read_text().splitlines():
        key, value = line.split(":", 1)
        values[key] = int(value.split()[0]) // 1024
    return {"mem_available_mib": values["MemAvailable"],
            "swap_used_mib": values["SwapTotal"] - values["SwapFree"]}


def gpu_status():
    cards = []
    for card in sorted(Path("/sys/class/drm").glob("card[0-9]*")):
        if not re.fullmatch(r"card\d+", card.name):
            continue
        values = {"card": card.name}
        for field, filename in (("vram_used_mib", "mem_info_vram_used"),
                                ("vram_total_mib", "mem_info_vram_total"),
                                ("gtt_used_mib", "mem_info_gtt_used")):
            try:
                values[field] = int((card / "device" / filename).read_text()) // (1024 * 1024)
            except (OSError, ValueError):
                values[field] = None
        if values["vram_total_mib"] is not None:
            cards.append(values)
    return cards


def process_usage(pid):
    """fdinfo is allocation accounting, not proof of physical local residency."""
    result = {"rss_mib": None, "vram_mib": None, "gtt_mib": None}
    if not pid:
        return result
    try:
        for line in Path(f"/proc/{pid}/status").read_text().splitlines():
            if line.startswith("VmRSS:"):
                result["rss_mib"] = int(line.split()[1]) // 1024
        clients = set()
        for path in Path(f"/proc/{pid}/fdinfo").iterdir():
            try:
                fields = dict(line.split(":", 1) for line in path.read_text().splitlines() if ":" in line)
                client = (fields.get("drm-pdev"), fields.get("drm-client-id"))
                if client[1] is None or client in clients:
                    continue
                clients.add(client)
                for key, field in (("vram_mib", "drm-memory-vram"), ("gtt_mib", "drm-memory-gtt")):
                    if field in fields:
                        count, unit = fields[field].split()[:2]
                        scale = {"B": 1, "KiB": 1024, "MiB": 1024**2}.get(unit)
                        if scale:
                            result[key] = (result[key] or 0) + int(count) * scale / (1024**2)
            except (OSError, ValueError):
                continue
    except OSError:
        pass
    return result


def discovered_processes(exclude=(), proc_root=Path('/proc')):
    """Observe same-user zVram launches without granting process ownership."""
    processes = {}
    for proc in proc_root.iterdir():
        if not proc.name.isdecimal():
            continue
        try:
            if proc.stat().st_uid != os.getuid():
                continue
            stat = (proc / 'stat').read_text().rsplit(')', 1)[1].split()
            processes[int(proc.name)] = (proc, int(stat[1]), stat[19])
        except (OSError, ValueError, IndexError):
            continue
    excluded = set(exclude)
    # Exclude all descendants of supervised jobs, not just their immediate child.
    while True:
        children = {pid for pid, (_, parent, _) in processes.items() if parent in excluded}
        if children <= excluded:
            break
        excluded.update(children)
    rows = []
    for pid, (proc, _, start) in sorted(processes.items()):
        if pid in excluded:
            continue
        env, loaded = {}, False
        try:
            with (proc / 'environ').open('rb') as stream:
                for entry in stream.read(131072).split(b'\0'):
                    key, sep, value = entry.partition(b'=')
                    if sep and key in (b'VK_INSTANCE_LAYERS', b'LD_PRELOAD', b'ZVRAM_VULKAN_RESIDENT_MIB'):
                        env[key.decode()] = os.fsdecode(value)
        except OSError:
            pass
        configured = ('VK_LAYER_NX_zvram' in env.get('VK_INSTANCE_LAYERS', '').split(':') or
                      any(Path(p).name == 'libzvram_hip.so' for p in re.split(r'[:\s]+', env.get('LD_PRELOAD', ''))))
        try:
            with (proc / 'maps').open() as stream:
                loaded = any(line.rstrip().removesuffix(' (deleted)').rsplit('/', 1)[-1] in
                             ('libzvram_layer.so', 'libzvram_hip.so') for line in stream)
        except OSError:
            pass
        if not (configured or loaded):
            continue
        try:
            name = (proc / 'comm').read_text().strip()
            # Ignore a vanished/reused PID rather than attributing stale observations.
            if (proc / 'stat').read_text().rsplit(')', 1)[1].split()[19] != start:
                continue
        except (OSError, IndexError):
            continue
        cap = env.get('ZVRAM_VULKAN_RESIDENT_MIB', '')
        row = dict(name=f'@{pid}-{start}', display_name=name, external=True, source='zVram',
                   mode='wrapped', priority='—', running=True, pid=pid, process_start=start,
                   state='Layer loaded' if loaded else 'Launch configured', command=[],
                   resident_mib=None, active_resident_mib=int(cap) if cap.isdecimal() else None)
        row.update(process_usage(pid))
        rows.append(row)
    return rows


class Manager:
    def __init__(self, home=None):
        self.home = private_directory(home or os.environ.get("ZVRAM_MANAGER_HOME") or
                                      Path(os.environ.get("XDG_STATE_HOME", Path.home() / ".local/state")) / "zvram")
        self.profiles_path = self.home / "profiles.json"
        self._workers = {}
        self._discovered = []
        self._discovered_at = 0

    @contextlib.contextmanager
    def lock(self):
        fd = os.open(self.home / "lock", os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW, 0o600)
        with os.fdopen(fd, "w") as stream:
            fcntl.flock(stream, fcntl.LOCK_EX)
            yield

    def job_path(self, name):
        self.validate_name(name)
        return self.home / (name + ".job.json")

    @staticmethod
    def validate_name(name):
        if isinstance(name, str) and name.startswith('@'):
            raise ValueError('Detected external processes are read-only')
        if not isinstance(name, str) or not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_.-]{0,63}", name):
            raise ValueError("Name: 1–64 letters, numbers, dots, underscores or hyphens")

    def save_profile(self, profile):
        profile = dict(profile)
        self.validate_name(profile.get("name"))
        if profile.get("priority", "normal") not in PRIORITIES:
            raise ValueError("Priority must be high, normal or low")
        profile.setdefault("priority", "normal")
        profile.setdefault("mode", "native")
        if profile["mode"] not in ("native", "vulkan", "wrapped"):
            raise ValueError("Mode must be native, vulkan or wrapped")
        command = profile.get("command")
        if not isinstance(command, list) or not command or not all(isinstance(x, str) and x and "\0" not in x for x in command):
            raise ValueError("Command must be a nonempty argument list")
        for key in ("resident_mib", "cold_mib", "min_available_mib", "max_swap_growth_mib"):
            value = profile.get(key)
            if value is not None and (type(value) is not int or not 0 < value <= 1048576):
                raise ValueError(key + " must be positive MiB, at most 1048576")
        env = profile.get("env", {})
        if not isinstance(env, dict) or not all(isinstance(k, str) and re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", k)
                                               and isinstance(v, str) and "\0" not in v for k, v in env.items()):
            raise ValueError("Environment must contain valid string keys and values")
        # Persist only launch configuration, never process identity or telemetry from UI rows.
        profile = {k: v for k, v in profile.items() if k in
                   ("name", "priority", "mode", "command", "resident_mib", "cold_mib", "env", "min_available_mib", "max_swap_growth_mib")}
        profile = {k: v for k, v in profile.items() if v is not None}
        with self.lock():
            profiles = read_json(self.profiles_path, {})
            profiles[profile["name"]] = profile
            write_json(self.profiles_path, profiles)
        return profile

    def list_profiles(self):
        for name, worker in list(self._workers.items()):
            if worker.poll() is not None:
                worker.wait()
                del self._workers[name]
        profiles = read_json(self.profiles_path, {})
        rows = []
        excluded = set()
        for name, profile in sorted(profiles.items()):
            job = read_json(self.job_path(name), {})
            running = owned_worker(job)
            if running:
                excluded.update(pid for pid in (job['identity']['pid'], job.get('child_pid')) if pid)
            row = dict(profile, running=running, pid=job.get("child_pid") if running else None,
                       state="running" if running else job.get("state", "stopped"),
                       active_resident_mib=job.get("resident_mib"), lastlog=job.get("reason", ""))
            row.update(process_usage(row["pid"]))
            rows.append(row)
        if time.monotonic() - self._discovered_at >= 2:
            self._discovered = discovered_processes(excluded)
            self._discovered_at = time.monotonic()
        return rows + [row for row in self._discovered if row['pid'] not in excluded and
                       (identity(row['pid']) or {}).get('start') == row['process_start']]

    def status(self):
        return dict(memory_status(), gpu=gpu_status(), jobs=self.list_profiles())

    def log_tail(self, name, lines=30):
        self.validate_name(name)
        path = self.home / (name + ".log")
        try:
            with path.open("rb") as stream:
                stream.seek(0, 2)
                stream.seek(max(0, stream.tell() - 16384))
                return "\n".join(stream.read().decode(errors="replace").splitlines()[-lines:])
        except FileNotFoundError:
            return "No log yet."

    def set_priority(self, name, priority):
        self.validate_name(name)
        profiles = read_json(self.profiles_path, {})
        return self.save_profile(dict(profiles[name], priority=priority, resident_mib=None))

    def delete(self, name):
        with self.lock():
            if owned_worker(read_json(self.job_path(name), {})):
                raise ValueError("Stop this managed process before deleting its profile")
            profiles = read_json(self.profiles_path, {})
            profiles.pop(name, None)
            write_json(self.profiles_path, profiles)

    def launch_command(self, profile):
        command = list(profile["command"])
        if Path(command[0]).name == "zvram":
            command[0] = str(ROOT / "zvram")
        total = max((c["vram_total_mib"] for c in gpu_status()), default=8192)
        resident = profile.get("resident_mib") or max(256, int(total * PRIORITIES[profile["priority"]]))
        if profile.get("mode") == "vulkan":
            command = [str(ROOT / "zvram"), "--vulkan-virtual-gib", "96", "--vulkan-auto-idle-ms", "1000",
                       "--vulkan-cold-mib", str(profile.get("cold_mib") or 4096),
                       "--vulkan-selective-restore", "--vulkan-active-eviction", "--vulkan-range-mib", "32",
                       "--vulkan-resident-mib", str(resident), "--vulkan-lazy-backing", "--vulkan-headroom-mib", "1536",
                       "--vulkan-clean-cache", "--vulkan-clean-cache-mib", str(min(1024, profile.get("cold_mib") or 4096)),
                       "--", *command]
        elif Path(command[0]).name == "zvram" and "--vulkan-resident-mib" in command:
            command[command.index("--vulkan-resident-mib") + 1] = str(resident)
        else:
            resident = None
        return command, resident

    def start(self, name):
        self.validate_name(name)
        if not Path(__file__).is_file():
            raise ValueError("Manager was updated; reopen it before starting a process")
        with self.lock():
            profiles = read_json(self.profiles_path, {})
            profile = profiles[name]
            path = self.job_path(name)
            if owned_worker(read_json(path, {})):
                raise ValueError("Profile is already running")
            command, resident = self.launch_command(profile)
            floor = profile.get("min_available_mib", 16384 if "--vulkan-resident-mib" in command else 4096)
            if memory_status()["mem_available_mib"] < floor:
                raise ValueError(f"Available RAM is below the {floor} MiB launch guard")
            token = uuid.uuid4().hex
            env = dict(os.environ, ZVRAM_MANAGER_HOME=str(self.home))
            worker = subprocess.Popen([sys.executable, __file__, "_worker", name, token], env=env,
                                      stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                      start_new_session=True)
            self._workers[name] = worker
            job = {"token": token, "identity": identity(worker.pid), "worker_script": __file__, "profile": profile, "command": command,
                   "resident_mib": resident, "min_available_mib": floor, "state": "starting"}
            write_json(path, job)
            return job

    def stop(self, name):
        with self.lock():
            job = read_json(self.job_path(name), {})
            if not owned_worker(job):
                return False
            pid = job["identity"]["pid"]
            fd = os.pidfd_open(pid)
            try:
                if not owned_worker(job):
                    raise ValueError("Process identity changed; refusing to signal")
                signal.pidfd_send_signal(fd, signal.SIGTERM)
            finally:
                os.close(fd)
        return True

    def worker(self, name, token):
        with self.lock():
            path = self.job_path(name)
            job = read_json(path, {})
            if job.get("token") != token or job.get("identity") != identity(os.getpid()):
                return 1
        stopping = []
        signal.signal(signal.SIGTERM, lambda *_: stopping.append("Stopped by user"))
        signal.signal(signal.SIGINT, lambda *_: stopping.append("Stopped by user"))
        profile = job["profile"]
        baseline = memory_status()["swap_used_mib"]
        logfd = os.open(self.home / (name + ".log"), os.O_CREAT | os.O_WRONLY | os.O_APPEND | os.O_NOFOLLOW, 0o600)
        with os.fdopen(logfd, "w") as log:
            log.write("\nManaged launch: " + json.dumps(job["command"]) + "\n")
            log.flush()
            child = None
            try:
                child = subprocess.Popen(job["command"], env=dict(os.environ, **profile.get("env", {})),
                                         stdin=subprocess.DEVNULL, stdout=log, stderr=log, start_new_session=True)
                job.update(child_pid=child.pid, state="running")
                with self.lock():
                    write_json(path, job)
                while child.poll() is None and not stopping:
                    usage = memory_status()
                    if usage["mem_available_mib"] < job["min_available_mib"]:
                        stopping.append("Stopped: available RAM guard")
                    if usage["swap_used_mib"] - baseline > profile.get("max_swap_growth_mib", 4096):
                        stopping.append("Stopped: system swap growth guard")
                    time.sleep(0.5)
                if child.poll() is None:
                    os.killpg(child.pid, signal.SIGTERM)
                    try:
                        child.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        os.killpg(child.pid, signal.SIGKILL)
                        child.wait()
                job.update(state="stopped" if stopping else "exited", returncode=child.returncode,
                           reason=stopping[0] if stopping else f"Process exited ({child.returncode})")
            except OSError as exc:
                job.update(state="failed", reason=str(exc))
            finally:
                if child is not None and child.poll() is None:
                    os.killpg(child.pid, signal.SIGTERM)
                    try:
                        child.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        os.killpg(child.pid, signal.SIGKILL)
                        child.wait()
            log.write(job["reason"] + "\n")
        with self.lock():
            write_json(path, job)
        return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description="zVram local launch profiles; priority caps apply on next launch")
    sub = parser.add_subparsers(dest="action", required=True)
    for action in ("gui", "tui", "list", "status"):
        sub.add_parser(action)
    add = sub.add_parser("add")
    add.add_argument("name")
    add.add_argument("--priority", choices=PRIORITIES, default="normal")
    add.add_argument("--mode", choices=("native", "vulkan", "wrapped"), default="native")
    add.add_argument("--resident-mib", type=int)
    add.add_argument("command", nargs="+")
    run = sub.add_parser("run", help="supervise a foreground command, including Steam's %%command%%")
    run.add_argument("--name")
    run.add_argument("--priority", choices=PRIORITIES, default="normal")
    run.add_argument("--mode", choices=("native", "vulkan", "wrapped"), default="vulkan")
    run.add_argument("--resident-mib", type=int)
    run.add_argument("command", nargs="+")
    for action in ("start", "stop", "delete", "logs"):
        sub.add_parser(action).add_argument("name")
    priority = sub.add_parser("priority")
    priority.add_argument("name")
    priority.add_argument("priority", choices=PRIORITIES)
    worker = sub.add_parser("_worker")
    worker.add_argument("name")
    worker.add_argument("token")
    args = parser.parse_args(argv)
    manager = Manager()
    if args.action in ("gui", "tui"):
        import zvram_ui
        return getattr(zvram_ui, "run_" + args.action)(manager)
    if args.action == "_worker":
        return manager.worker(args.name, args.token)
    if args.action == "run":
        command = args.command[1:] if args.command[:1] == ["--"] else args.command
        name = args.name or (re.sub(r"[^A-Za-z0-9_.-]", "-", Path(command[0]).name)[:48] + "-" + str(os.getpid()))
        manager.save_profile(dict(name=name, priority=args.priority, mode=args.mode,
                                  resident_mib=args.resident_mib, command=command))
        manager.start(name)
        interrupted = []
        previous = {s: signal.signal(s, lambda *_: interrupted.append(True)) for s in (signal.SIGINT, signal.SIGTERM)}
        try:
            while owned_worker(read_json(manager.job_path(name), {})):
                if interrupted:
                    manager.stop(name)
                time.sleep(.2)
        finally:
            manager.stop(name)
            for sig, handler in previous.items():
                signal.signal(sig, handler)
        job = read_json(manager.job_path(name), {})
        return job.get("returncode", 1) or 0
    if args.action == "add":
        command = args.command[1:] if args.command[:1] == ["--"] else args.command
        result = manager.save_profile(dict(name=args.name, priority=args.priority, mode=args.mode,
                                           resident_mib=args.resident_mib, command=command))
    elif args.action == "priority":
        result = manager.set_priority(args.name, args.priority)
    elif args.action == "list":
        result = manager.list_profiles()
    elif args.action == "logs":
        print(manager.log_tail(args.name))
        return 0
    else:
        result = getattr(manager, args.action)(*([args.name] if hasattr(args, "name") else []))
        if args.action == "start":
            result = {"profile": args.name, "state": result["state"], "resident_mib": result["resident_mib"]}
    print(json.dumps(result, indent=2))
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, KeyError) as exc:
        print("zVram manager: " + str(exc), file=sys.stderr)
        sys.exit(1)
