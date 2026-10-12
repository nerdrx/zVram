"""Private same-user runtime requests; the layer acknowledges every applied cap."""
import contextlib
import os
from pathlib import Path
import re
import stat
import uuid


def directory(home=None):
    from zvram_manager import private_directory
    explicit = os.environ.get('ZVRAM_CONTROL_DIR')
    runtime = os.environ.get('XDG_RUNTIME_DIR')
    if explicit:
        return private_directory(explicit)
    if runtime:
        root = Path(runtime)
        st = root.lstat()
        if not root.is_symlink() and st.st_uid == os.getuid() and stat.S_ISDIR(st.st_mode) and st.st_mode & 0o077 == 0:
            return private_directory(root / 'zvram-control')
    return private_directory(Path(home or os.environ.get('ZVRAM_MANAGER_HOME') or
                                  Path(os.environ.get('XDG_STATE_HOME', Path.home() / '.local/state')) / 'zvram') / 'control')


@contextlib.contextmanager
def process_directory(base, pid, start):
    basefd = os.open(base, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
    childfd = None
    try:
        st = os.fstat(basefd)
        if st.st_uid != os.getuid() or st.st_mode & 0o7777 != 0o700:
            raise ValueError('Unsafe control directory')
        childfd = os.open(f'{int(pid)}-{int(start)}', os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW, dir_fd=basefd)
        st = os.fstat(childfd)
        if st.st_uid != os.getuid() or st.st_mode & 0o7777 != 0o700:
            raise ValueError('Unsafe process control directory')
        yield childfd
    finally:
        if childfd is not None:
            os.close(childfd)
        os.close(basefd)


def read_file(fd, name):
    filefd = os.open(name, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK, dir_fd=fd)
    with os.fdopen(filefd, 'rb') as stream:
        st = os.fstat(stream.fileno())
        if not stat.S_ISREG(st.st_mode) or st.st_uid != os.getuid() or st.st_mode & 0o7777 != 0o600 or st.st_nlink != 1 or st.st_size > 1024:
            raise ValueError('Unsafe control file')
        data = stream.read(1025)
    if len(data) > 1024:
        raise ValueError('Oversized control file')
    values = {}
    for line in data.decode('ascii').splitlines():
        key, sep, value = line.partition('=')
        if not sep or key in values or not value.isdecimal() or len(value) > 20:
            raise ValueError('Invalid control message')
        values[key] = int(value)
        if values[key] > (1 << 64) - 1:
            raise ValueError('Control integer overflow')
    if values.get('version') != 1:
        raise ValueError('Unsupported control protocol')
    return values


def status(base, pid, start):
    devices = []
    try:
        with process_directory(base, pid, start) as fd:
            for filename in sorted(os.listdir(fd)):
                if not re.fullmatch(r'\d+\.status', filename):
                    continue
                try:
                    value = read_file(fd, filename)
                    if value.get('pid') != pid or str(value.get('start')) != str(start) or str(value.get('device')) + '.status' != filename:
                        continue
                    value['device'] = str(value['device'])
                    value['capable'] = value.get('capable') == 1
                    devices.append(value)
                except (OSError, ValueError, UnicodeError):
                    continue
    except (OSError, ValueError):
        pass
    return devices


def request(manager, name, mib=None, priority=None):
    from zvram_manager import identity, PRIORITIES
    if priority is not None and priority not in PRIORITIES:
        raise ValueError('Unknown priority preset')
    if priority is None and (type(mib) is not int or not 0 < mib <= 1048576):
        raise ValueError('Resident limit must be positive MiB, at most 1048576')
    with manager.lock():
        row, saved = manager.control_target(name)
        devices = [d for d in row.get('control_devices', []) if d['capable']]
        if not devices:
            raise ValueError('Restart with the updated layer and --live-control to enable live residency controls')
        planned = []
        for device in devices:
            limit = mib if priority is None else max(device.get('min_limit_mib', 1), int(device['max_limit_mib'] * PRIORITIES[priority]))
            if not device.get('min_limit_mib', 1) <= limit <= device.get('max_limit_mib', 1048576):
                raise ValueError('Limit is outside this device\'s supported range')
            planned.append((device, limit))
        requests = []
        with process_directory(manager.control_home, saved['pid'], saved['start']) as fd:
            for device, limit in planned:
                filename = device['device'] + '.request'
                sequence = device.get('seq', 0)
                try:
                    sequence = max(sequence, read_file(fd, filename)['seq'])
                except FileNotFoundError:
                    pass
                if identity(saved['pid']) != saved:
                    raise ValueError('Process identity changed; request refused')
                sequence += 1
                if sequence >= 1 << 64:
                    raise ValueError('Control sequence exhausted; restart this app')
                temp = filename + '.' + uuid.uuid4().hex
                filefd = os.open(temp, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600, dir_fd=fd)
                try:
                    with os.fdopen(filefd, 'w') as stream:
                        stream.write(f'version=1\nseq={sequence}\nresident_mib={limit}\n')
                    os.replace(temp, filename, src_dir_fd=fd, dst_dir_fd=fd)
                finally:
                    try:
                        os.unlink(temp, dir_fd=fd)
                    except FileNotFoundError:
                        pass
                requests.append(dict(device=device['device'], seq=sequence, resident_mib=limit))
        return requests
