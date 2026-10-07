#!/usr/bin/env python3
"""Inspect or run a small, cgroup-scoped AMDGPU dmem reclaim probe."""

import argparse
import os
from pathlib import Path
import pwd
import re
import selectors
import signal
import subprocess
import sys
import time


CGROUP = Path("/sys/fs/cgroup")
MIB = 1024 * 1024
MEMORY_LIMIT = 512 * MIB
SWAP_LIMIT = 256 * MIB
PROBE_TIMEOUT = 30
OUTPUT_LIMIT = 64 * 1024
POLL_INTERVAL = 0.05


def read_text(path):
    try:
        return path.read_text().strip()
    except OSError:
        return None


def keyed_bytes(path):
    result = {}
    content = read_text(path)
    if content is None:
        return result
    for line in content.splitlines():
        fields = line.split()
        if len(fields) == 2 and fields[1].isdigit():
            result[fields[0]] = int(fields[1])
    return result


def scalar_bytes(path):
    content = read_text(path)
    return int(content) if content and content.isdigit() else None


def self_cgroup_path():
    for line in Path("/proc/self/cgroup").read_text().splitlines():
        if line.startswith("0::"):
            rel = Path(line[3:].lstrip("/"))
            if ".." in rel.parts:
                raise RuntimeError("unsafe current cgroup path")
            return CGROUP / rel
    raise RuntimeError("unified cgroup-v2 path unavailable")


def inspect():
    root_capacity = keyed_bytes(CGROUP / "dmem.capacity")
    root_current = keyed_bytes(CGROUP / "dmem.current")
    own = self_cgroup_path()
    own_current = keyed_bytes(own / "dmem.current")
    own_max = read_text(own / "dmem.max")
    mem_available = None
    for line in Path("/proc/meminfo").read_text().splitlines():
        if line.startswith("MemAvailable:"):
            mem_available = int(line.split()[1]) * 1024
            break
    swaps = []
    for line in Path("/proc/swaps").read_text().splitlines()[1:]:
        fields = line.split()
        if len(fields) >= 4:
            total, used = int(fields[2]) * 1024, int(fields[3]) * 1024
            swaps.append((fields[0], total, used))
    zram = [(name, total, used) for name, total, used in swaps
            if Path(name).name.startswith("zram")]

    print(f"cgroup-v2 root: {CGROUP}")
    print(f"root dmem.capacity: {root_capacity or 'unavailable'}")
    print(f"root dmem.current:  {root_current or 'unavailable'}")
    print(f"self cgroup: {own}")
    print(f"self dmem.current:  {own_current or 'not exposed'}")
    print(f"self dmem.max:      {own_max or 'not exposed'}")
    print(f"MemAvailable:       {mem_available if mem_available is not None else 'unavailable'} bytes")
    if zram:
        for name, total, used in zram:
            print(f"active zram {name}: total={total} used={used} available={max(0, total-used)} bytes")
    else:
        print("active zram: none")
    zswap = read_text(Path("/sys/module/zswap/parameters/enabled"))
    print(f"zswap enabled:      {zswap if zswap is not None else 'unknown'}")


def dmem_regions(path):
    regions = keyed_bytes(path)
    valid = {}
    for name, size in regions.items():
        match = re.fullmatch(r"drm/([0-9a-fA-F:.]+)/vram[0-9]*", name)
        if match and size > 0:
            vendor = read_text(Path("/sys/bus/pci/devices") / match[1] / "vendor")
            if vendor == "0x1002":
                valid[name] = size
    return valid


def required_identity():
    if os.geteuid() != 0:
        raise RuntimeError("--run requires root; invoke through sudo")
    try:
        uid = int(os.environ["SUDO_UID"])
        gid = int(os.environ["SUDO_GID"])
    except (KeyError, ValueError):
        raise RuntimeError("--run requires original SUDO_UID and SUDO_GID")
    if uid <= 0 or gid <= 0:
        raise RuntimeError("refusing to run as root without an unprivileged sudo user")
    try:
        user = pwd.getpwuid(uid)
    except KeyError:
        raise RuntimeError("SUDO_UID does not identify a local user")
    return uid, gid, user


def set_child_limits(cgroup, region, gpu_limit):
    controllers = set((read_text(CGROUP / "cgroup.controllers") or "").split())
    enabled = set((read_text(CGROUP / "cgroup.subtree_control") or "").split())
    for name in ("dmem", "memory"):
        if name not in controllers or name not in enabled:
            raise RuntimeError(f"root cgroup must expose and enable {name}")
    (cgroup / "dmem.max").write_text(f"{region} {gpu_limit}\n")
    (cgroup / "memory.max").write_text(f"{MEMORY_LIMIT}\n")
    (cgroup / "memory.swap.max").write_text(f"{SWAP_LIMIT}\n")


def drop_into_cgroup(cgroup_procs, uid, gid, user):
    fd = os.open(cgroup_procs, os.O_WRONLY)
    try:
        os.write(fd, f"{os.getpid()}\n".encode())
    finally:
        os.close(fd)
    groups = os.getgrouplist(user.pw_name, gid)
    os.setgroups(groups)
    os.setresgid(gid, gid, gid)
    os.setresuid(uid, uid, uid)


def child_environment(user):
    env = {}
    for name, value in os.environ.items():
        upper = name.upper()
        if name.startswith("SUDO_") or name.startswith(("LD_", "PYTHON")):
            continue
        if name in {"PATH", "LANG", "HOME", "USER", "LOGNAME", "SHELL", "TMPDIR",
                    "DISPLAY", "WAYLAND_DISPLAY", "DBUS_SESSION_BUS_ADDRESS", "XAUTHORITY"} \
                or name.startswith(("LC_", "XDG_", "VK_", "MESA_", "RADV_")):
            if (name in {"DBUS_SESSION_BUS_ADDRESS", "XAUTHORITY"} or
                    not re.search(r"TOKEN|SECRET|PASS(WOR)?D|CREDENTIAL|(?:^|_)KEY(?:_|$)|APIKEY|AUTH", upper)):
                env[name] = value
        elif not re.search(r"TOKEN|SECRET|PASS(WOR)?D|CREDENTIAL|(?:^|_)KEY(?:_|$)|APIKEY|AUTH", upper):
            # Preserve ordinary application settings without forwarding credential-like variables.
            env[name] = value
    env.update(HOME=user.pw_dir, USER=user.pw_name, LOGNAME=user.pw_name)
    return env


def sample(cgroup, region):
    dmem = keyed_bytes(cgroup / "dmem.current").get(region, 0)
    memory = scalar_bytes(cgroup / "memory.current") or 0
    swap = scalar_bytes(cgroup / "memory.swap.current") or 0
    return dmem, memory, swap


def append_tail(buffer, chunk):
    buffer.extend(chunk)
    if len(buffer) > OUTPUT_LIMIT:
        del buffer[:-OUTPUT_LIMIT]


def stop_owned_process(proc, cgroup):
    kill_file = cgroup / "cgroup.kill"
    try:
        kill_file.write_text("1\n")
    except OSError:
        try:
            os.killpg(proc.pid, signal.SIGTERM)
        except ProcessLookupError:
            pass
    try:
        proc.wait(timeout=2)
    except subprocess.TimeoutExpired:
        try:
            os.killpg(proc.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        try:
            proc.wait(timeout=2)
        except subprocess.TimeoutExpired:
            pass


def run_probe(limit_mib):
    uid, gid, user = required_identity()
    root_capacity = dmem_regions(CGROUP / "dmem.capacity")
    if not root_capacity:
        raise RuntimeError("no AMDGPU VRAM region found in root dmem.capacity")
    region, capacity = max(root_capacity.items(), key=lambda item: item[1])
    gpu_limit = limit_mib * MIB
    if gpu_limit > capacity:
        raise RuntimeError("GPU cgroup limit exceeds selected VRAM region capacity")

    cgroup = CGROUP / f"zvram-probe-{os.getpid()}"
    if cgroup.exists():
        raise RuntimeError(f"refusing to reuse existing cgroup: {cgroup}")
    cgroup.mkdir(mode=0o755)
    proc = None
    try:
        set_child_limits(cgroup, region, gpu_limit)
        baseline = sample(cgroup, region)
        repo = Path(__file__).resolve().parent
        command = [
            str(repo / "zvram"), "--validate", "--isolate-layers", "--",
            str(repo / "build" / "zvram-capacity-check"),
            "--mib", "64", "--chunk-mib", "8",
        ]
        if not os.access(command[0], os.X_OK) or not os.access(command[4], os.X_OK):
            raise RuntimeError("build the launcher and capacity check before --run")

        preexec = lambda: drop_into_cgroup(cgroup / "cgroup.procs", uid, gid, user)
        proc = subprocess.Popen(
            command, cwd=repo, env=child_environment(user),
            stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            bufsize=0, close_fds=True, start_new_session=True, preexec_fn=preexec,
        )
        selector = selectors.DefaultSelector()
        outputs = {proc.stdout: bytearray(), proc.stderr: bytearray()}
        for pipe in outputs:
            os.set_blocking(pipe.fileno(), False)
            selector.register(pipe, selectors.EVENT_READ)
        peak = list(baseline)
        deadline = time.monotonic() + PROBE_TIMEOUT
        timed_out = False
        while True:
            current = sample(cgroup, region)
            peak = [max(old, new) for old, new in zip(peak, current)]
            now = time.monotonic()
            if now >= deadline:
                timed_out = True
                stop_owned_process(proc, cgroup)
                # A descendant may keep pipes open after the original child exits.
                # The fixed deadline covers the entire probe, including that case.
                for pipe in list(selector.get_map().values()):
                    selector.unregister(pipe.fileobj)
                    pipe.fileobj.close()
                break
            for key, _ in selector.select(min(POLL_INTERVAL, max(0, deadline-now))):
                try:
                    chunk = os.read(key.fileobj.fileno(), 8192)
                except BlockingIOError:
                    continue
                if chunk:
                    append_tail(outputs[key.fileobj], chunk)
                else:
                    selector.unregister(key.fileobj)
                    key.fileobj.close()
            if proc.poll() is not None and not selector.get_map():
                break
            if timed_out and proc.poll() is None:
                stop_owned_process(proc, cgroup)
                if proc.poll() is None:
                    break
        selector.close()
        returncode = proc.poll()
        if returncode is not None:
            proc.wait()
        for pipe in outputs:
            if not pipe.closed:
                pipe.close()
        stdout = outputs[proc.stdout].decode(errors="replace")
        stderr = outputs[proc.stderr].decode(errors="replace")
        print(f"probe command: {' '.join(command)}")
        print(f"child uid/gid: {uid}/{gid} ({user.pw_name})")
        print(f"cgroup: {cgroup}")
        print(f"dmem region: {region}; capacity={capacity}; limit={gpu_limit}")
        print(f"child memory limits: memory.max={MEMORY_LIMIT}; memory.swap.max={SWAP_LIMIT}")
        print(f"baseline bytes (dmem,memory,swap): {tuple(baseline)}")
        print(f"sampled peak bytes (dmem,memory,swap): {tuple(peak)}; interval={POLL_INTERVAL}s")
        if stdout:
            print("--- child stdout (last 64 KiB) ---")
            print(stdout, end="" if stdout.endswith("\n") else "\n")
        if stderr:
            print("--- child stderr (last 64 KiB) ---", file=sys.stderr)
            print(stderr, end="" if stderr.endswith("\n") else "\n", file=sys.stderr)
        if timed_out:
            print(f"probe timed out after {PROBE_TIMEOUT}s; only its cgroup/process group was stopped")
            return 124
        if returncode == 0:
            print("integrity result: capacity-check exited successfully; this does not prove TTM shmem swap or compression")
        else:
            print(f"integrity result: capacity-check exited {returncode}")
        return returncode
    finally:
        if proc is not None and proc.poll() is None:
            stop_owned_process(proc, cgroup)
        kill_file = cgroup / "cgroup.kill"
        if kill_file.exists():
            try:
                kill_file.write_text("1\n")
            except OSError:
                pass
        for _ in range(20):
            events = read_text(cgroup / "cgroup.events")
            if events is None or "populated 0" in events.splitlines():
                try:
                    cgroup.rmdir()
                    break
                except OSError:
                    pass
            time.sleep(0.05)
        if cgroup.exists():
            print(f"warning: probe cgroup still exists after cleanup: {cgroup}", file=sys.stderr)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--inspect", action="store_true", help="read cgroup, RAM, and swap state only")
    mode.add_argument("--run", action="store_true", help="run the fixed 64 MiB integrity probe in a temporary cgroup")
    parser.add_argument("--gpu-limit-mib", type=int, default=16, metavar="1..256",
                        help="child dmem.max limit in MiB (default: 16)")
    args = parser.parse_args()
    if not 1 <= args.gpu_limit_mib <= 256:
        parser.error("--gpu-limit-mib must be 1..256")
    try:
        if args.run:
            return run_probe(args.gpu_limit_mib)
        inspect()
        return 0
    except (OSError, RuntimeError, ValueError, subprocess.SubprocessError) as error:
        print(f"dmem_probe: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
