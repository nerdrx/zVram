#!/usr/bin/env python3
"""Serve existing GGUF models through zVram and register a Novum Xenium endpoint.

Ollama's existing runner cannot be wrapped by this Vulkan layer. This uses a
separate Vulkan llama-server; Ollama's model files remain unchanged.
"""
import argparse
import json
import os
from pathlib import Path
import re
import shutil
import time
import urllib.error
import urllib.parse
import urllib.request

_MODULE_DIR = Path(__file__).resolve().parent
ROOT = (_MODULE_DIR.parents[1] if _MODULE_DIR.parent.name == "src" and _MODULE_DIR.name == "python"
        else _MODULE_DIR)
DEFAULT_SERVER = ROOT / 'build/third-party/llama-vulkan-build/bin/llama-server'
_OLLAMA_MODEL_LAYER = 'application/vnd.ollama.image.model'
_OLLAMA_PROJECTOR_LAYER = 'application/vnd.ollama.image.projector'
_OLLAMA_BLOB_RE = re.compile(r'sha256:[0-9a-f]{64}')


def discover_xenium(config_path=None):
    config = Path(config_path or Path.home() / '.config/dev.novum.xenium/config.json')
    data = json.loads(config.read_text())
    checkout = Path(data['checkout']).expanduser().resolve()
    if not checkout.is_dir():
        raise ValueError('Novum Xenium checkout is unavailable')
    port = checked_port(data['port'])
    return {'checkout': str(checkout), 'port': port,
            'cookie_file': str(Path.home() / '.local/share/dev.novum.xenium/cookies')}


def checked_port(value):
    port = int(value)
    if not 1 <= port <= 65535:
        raise ValueError('port must be between 1 and 65535')
    return port


def discover_models(checkout):
    store = Path(checkout) / 'data/ollama/models'
    models = []
    for manifest in sorted((store / 'manifests').rglob('*')):
        if not manifest.is_file() or manifest.is_symlink():
            continue
        try:
            data = json.loads(manifest.read_text())
            layers = data.get('layers', [])
            if not isinstance(layers, list):
                continue
            model_layers = [layer for layer in layers
                            if isinstance(layer, dict) and layer.get('mediaType') == _OLLAMA_MODEL_LAYER]
            projector_paths = _projector_paths(store, layers)
            relative = manifest.relative_to(store / 'manifests')
            parts = relative.parts[1:]
            if parts and parts[0] == 'library':
                parts = parts[1:]
            name = '/'.join(parts[:-1]) + ':' + parts[-1]
            for layer in model_layers:
                path = _verified_gguf_blob(store, layer.get('digest', ''))
                if path is None:
                    continue
                row = {'name': name, 'path': str(path), 'size': path.stat().st_size}
                if len(projector_paths) == 1:
                    row['mmproj_path'] = str(projector_paths[0])
                models.append(row)
        except (OSError, ValueError, KeyError, IndexError):
            continue
    return models


def _verified_gguf_blob(store, digest):
    if not isinstance(digest, str) or not _OLLAMA_BLOB_RE.fullmatch(digest):
        return None
    blobs = (Path(store) / 'blobs').resolve()
    path = blobs / digest.replace(':', '-', 1)
    try:
        resolved = path.resolve(strict=True)
        if resolved.parent != blobs or not resolved.is_file():
            return None
        with resolved.open('rb') as stream:
            if stream.read(4) != b'GGUF':
                return None
        return resolved
    except OSError:
        return None


def _projector_paths(store, layers):
    return sorted({path for layer in layers
                   if isinstance(layer, dict) and layer.get('mediaType') == _OLLAMA_PROJECTOR_LAYER
                   and (path := _verified_gguf_blob(store, layer.get('digest', ''))) is not None})


def _ollama_store_for_blob(path):
    path = Path(path).resolve()
    blobs = path.parent
    store = blobs.parent
    if (blobs.name != 'blobs' or store.name != 'models'
            or store.parent.name != 'ollama' or path.name[0:7] != 'sha256-'
            or not re.fullmatch(r'sha256-[0-9a-f]{64}', path.name)):
        return None
    return store


def _manifest_model_name(store, manifest):
    relative = Path(manifest).relative_to(Path(store) / 'manifests')
    parts = relative.parts[1:]
    if parts and parts[0] == 'library':
        parts = parts[1:]
    if len(parts) < 2:
        return None
    return '/'.join(parts[:-1]) + ':' + parts[-1]


def _associated_projectors(model_path, model_name=None):
    store = _ollama_store_for_blob(model_path)
    if store is None:
        return []
    try:
        model = Path(model_path).resolve(strict=True)
        with model.open('rb') as stream:
            if stream.read(4) != b'GGUF':
                return []
        digest = 'sha256:' + model.name[len('sha256-'):]
        matching = []
        for manifest in (store / 'manifests').rglob('*'):
            if not manifest.is_file() or manifest.is_symlink():
                continue
            try:
                layers = json.loads(manifest.read_text()).get('layers', [])
            except (OSError, ValueError, AttributeError):
                continue
            if not isinstance(layers, list) or not any(
                    isinstance(layer, dict) and layer.get('mediaType') == _OLLAMA_MODEL_LAYER
                    and layer.get('digest') == digest for layer in layers):
                continue
            name = _manifest_model_name(store, manifest)
            if model_name is None or name == model_name:
                matching.append((name, _projector_paths(store, layers)))
        # A blob may be reused by multiple Ollama tags. Without the exact
        # manifest identity, attaching another tag's projector is unsafe.
        if model_name is None and len(matching) != 1:
            return []
        if len(matching) != 1:
            return []
        return matching[0][1]
    except OSError:
        return []


def session_cookie(cookie_file, now=None):
    """Read Netscape 7-column cookies or WebKit's 8-column variant."""
    now = time.time() if now is None else now
    for line in Path(cookie_file).read_text().splitlines():
        if line.startswith('#HttpOnly_'):
            line = line[len('#HttpOnly_'):]
        elif line.startswith('#'):
            continue
        fields = line.split('\t')
        if len(fields) not in (7, 8):
            continue
        domain, _, path, secure, expiry, name, value = fields[:7]
        if domain != '127.0.0.1' or path != '/' or secure.upper() != 'FALSE':
            continue
        try:
            if int(expiry) != 0 and int(expiry) <= now:
                continue
        except ValueError:
            continue
        if name == 'odysseus_session' and value and not re.search(r'[\s;\r\n]', value):
            return name + '=' + value
    raise ValueError('No valid local Novum Xenium session; sign in in the desktop app')


def build_server_command(model_path, alias, port=8097, compressed=False,
                         resident_mib=19456, cold_mib=26624,
                         clean_cache_mib=1024, headroom_mib=1536,
                         virtual_gib=96, context=4096, server=None, build_dir=None,
                         mode='spill', live_control=True, mmproj=None,
                         model_name=None):
    model = Path(model_path).expanduser().resolve()
    with model.open('rb') as stream:
        if stream.read(4) != b'GGUF':
            raise ValueError('model must be a GGUF file')
    # The alias commonly is the source Ollama name; callers using a renamed
    # alias can pass --model-name to retain that exact manifest binding.
    projectors = _associated_projectors(model, model_name or alias)
    if mmproj is not None:
        projector = Path(mmproj).expanduser().resolve()
        if projector not in projectors:
            raise ValueError('projector must be paired with this Ollama model manifest')
    elif len(projectors) == 1:
        projector = projectors[0]
    elif len(projectors) > 1:
        raise ValueError('model has multiple paired projectors; select one with --mmproj')
    else:
        projector = None
    if not alias or len(alias) > 128 or not re.fullmatch(r'[A-Za-z0-9_.:/-]+', alias):
        raise ValueError('model alias must contain only letters, numbers, _ . : / -')
    port = checked_port(port)
    if mode not in ('spill', 'native') or (mode == 'native' and compressed):
        raise ValueError('mode must be spill or native; native cannot use compressed paging')
    if type(live_control) is not bool:
        raise ValueError('live_control must be a boolean')
    values = (resident_mib, cold_mib, clean_cache_mib, headroom_mib, virtual_gib, context)
    if any(not isinstance(v, int) or v <= 0 for v in values):
        raise ValueError('memory sizes and context must be positive integers')
    if clean_cache_mib > cold_mib:
        raise ValueError('clean cache cannot exceed cold storage')
    default = DEFAULT_SERVER if DEFAULT_SERVER.is_file() else shutil.which('llama-server')
    binary = Path(server or default or DEFAULT_SERVER).expanduser().resolve()
    if not binary.is_file() or not os.access(binary, os.X_OK):
        raise ValueError('Vulkan llama-server is missing; put it on PATH or specify --server')
    command = [str(ROOT / 'zvram')]
    # Model launches follow the launcher's live-management default. Plain spill
    # remains an explicit opt-out; BP16 paging already supplies its own settings.
    command.append('--live-control' if live_control or compressed else '--no-live-control')
    if live_control and not compressed:
        command += ['--vulkan-range-mib', '32', '--vulkan-resident-mib', str(resident_mib)]
    if build_dir:
        command += ['--build-dir', str(Path(build_dir).expanduser().resolve())]
    command += ['--vulkan-virtual-gib', str(virtual_gib)]
    env = {k: v for k, v in os.environ.items() if not k.startswith('ZVRAM_')}
    if compressed:
        command += ['--vulkan-auto-idle-ms', '60000', '--vulkan-cold-mib', str(cold_mib),
                    '--vulkan-codec', 'bp16', '--vulkan-bp16-gpu',
                    '--vulkan-bp16-workers', '8', '--vulkan-selective-restore',
                    '--vulkan-active-eviction', '--vulkan-async-compression',
                    '--vulkan-range-mib', '32', '--vulkan-resident-mib', str(resident_mib),
                    '--vulkan-lazy-backing', '--vulkan-headroom-mib', str(headroom_mib),
                    '--vulkan-strict-robustness', '--vulkan-clean-cache',
                    '--vulkan-clean-cache-mib', str(clean_cache_mib)]
        env.update(ZVRAM_VULKAN_CLEAN_CACHE_POLICY='lfu',
                   ZVRAM_VULKAN_ADMISSION_BUDGET_SNAPSHOT='1',
                   ZVRAM_VULKAN_CLEAN_FIRST_EVICTION='1',
                   ZVRAM_VULKAN_BP16_GPU_ENCODE='1',
                   ZVRAM_VULKAN_BP16_ALLOCATED_HOST_INPUT='1',
                   ZVRAM_VULKAN_BP16_ALLOCATED_HOST_MIB=str(cold_mib),
                   ZVRAM_VULKAN_BP16_RAW_HOST_INPUT='1',
                   GGML_VK_DISABLE_HOST_VISIBLE_VIDMEM='1',
                   GGML_VK_MAX_NODES_PER_SUBMIT='4')
    # Keep the endpoint private. One slot bounds simultaneous KV allocations.
    command += ['--', str(binary), '--model', str(model), '--alias', alias,
                '--host', '127.0.0.1', '--port', str(port), '--gpu-layers', '999',
                '--ctx-size', str(context), '--parallel', '1', '--batch-size', '128',
                '--fit', 'off', '--no-warmup']
    if projector is not None:
        command += ['--mmproj', str(projector)]
    if compressed:
        command += ['--load-mode', 'dio']
    if mode == 'native':
        command = command[command.index('--') + 1:]
    return command, env


class _NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, request, fp, code, message, headers, newurl):
        return None


def _request(url, cookie=None, method='GET', data=None, form=False):
    headers = {}
    if cookie:
        headers['Cookie'] = cookie
    if data is not None:
        headers['Content-Type'] = 'application/x-www-form-urlencoded' if form else 'application/json'
        data = (urllib.parse.urlencode(data) if form else json.dumps(data)).encode()
    request = urllib.request.Request(url, data=data, headers=headers, method=method)
    # Ignore proxy environment variables for private loopback requests.
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}), _NoRedirect())
    try:
        with opener.open(request, timeout=10) as response:
            return json.load(response)
    except urllib.error.HTTPError as exc:
        raise ValueError(f'Local endpoint request failed (HTTP {exc.code})') from None
    except (urllib.error.URLError, TimeoutError):
        raise ValueError('Local endpoint is unavailable') from None


def register_endpoint(alias, port=8097, xenium=None):
    xenium = xenium or discover_xenium()
    port = checked_port(port)
    if port == checked_port(xenium['port']):
        raise ValueError('model port cannot equal the Novum Xenium port')
    base = f'http://127.0.0.1:{port}/v1'
    inventory = _request(base + '/models')
    if alias not in [item.get('id') for item in inventory.get('data', [])]:
        raise ValueError('Start the requested model server before registering its endpoint')
    cookie = session_cookie(xenium['cookie_file'])
    api = f'http://127.0.0.1:{checked_port(xenium["port"])}/api/model-endpoints'
    endpoints = _request(api, cookie)
    name = 'zVram · ' + alias
    existing = [ep for ep in endpoints if ep.get('base_url', '').rstrip('/') == base]
    if existing:
        ep = existing[0]
        if not ep.get('name', '').startswith('zVram · '):
            raise ValueError('This port already belongs to another endpoint; choose a different port')
        return _request(api + '/' + urllib.parse.quote(str(ep['id']), safe=''), cookie,
                        'PATCH', {'name': name, 'is_enabled': True, 'pinned_models': [alias]})
    return _request(api, cookie, 'POST', {'name': name, 'base_url': base,
                    'endpoint_kind': 'local', 'model_type': 'llm', 'skip_probe': 'false',
                    'pinned_models': json.dumps([alias]), 'shared': 'false'}, form=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=('list', 'command', 'setup', 'register'))
    parser.add_argument('--name', help='manager profile name (setup only)')
    parser.add_argument('--model', type=Path)
    parser.add_argument('--mmproj', type=Path, help='select one projector paired to this Ollama model manifest')
    parser.add_argument('--model-name', help='exact Ollama manifest name for safe projector pairing')
    parser.add_argument('--alias', default='zvram-local')
    parser.add_argument('--port', type=int, default=8097)
    parser.add_argument('--compressed', action='store_true', help='experimental lossless paging profile')
    parser.add_argument('--mode', choices=('spill', 'native'), default='spill')
    live = parser.add_mutually_exclusive_group()
    live.add_argument('--live-control', dest='live_control', action='store_true', default=True,
                      help='enable live VRAM management (default; uses automatic paging)')
    live.add_argument('--no-live-control', dest='live_control', action='store_false',
                      help='use plain spill without live VRAM management; BP16 still enables paging')
    parser.add_argument('--resident-mib', type=int, default=19456)
    parser.add_argument('--cold-mib', type=int, default=26624)
    parser.add_argument('--clean-cache-mib', type=int, default=1024)
    parser.add_argument('--headroom-mib', type=int, default=1536)
    parser.add_argument('--virtual-gib', type=int, default=96)
    parser.add_argument('--context', type=int, default=4096)
    parser.add_argument('--ignore-swap-guard', action='store_true',
                        help='disable the managed profile swap-growth guard; available-RAM guard stays enabled')
    parser.add_argument('--server', type=Path)
    parser.add_argument('--build-dir', type=Path)
    args = parser.parse_args()
    try:
        if args.action == 'list':
            print(json.dumps(discover_models(discover_xenium()['checkout']), indent=2))
        elif args.action == 'register':
            result = register_endpoint(args.alias, args.port)
            print(json.dumps({'registered': True, 'id': result.get('id')}))
        else:
            if not args.model:
                parser.error('--model is required for command')
            options = vars(args).copy()
            for key in ('action', 'model', 'name', 'ignore_swap_guard'):
                options.pop(key)
            command, env = build_server_command(args.model, **options)
            overrides = {k: v for k, v in env.items() if k.startswith(('ZVRAM_', 'GGML_'))}
            if args.action == 'setup':
                from zvram_manager import Manager
                name = args.name or re.sub(r'[^A-Za-z0-9_.-]', '-', args.alias)
                profile = {'name': name, 'priority': 'normal', 'mode': 'native' if args.mode == 'native' else 'wrapped',
                           'command': command, 'env': overrides, 'ignore_swap_guard': args.ignore_swap_guard}
                if args.mode != 'native' and (args.compressed or args.live_control):
                    profile['resident_mib'] = args.resident_mib
                    profile['min_available_mib'] = 16384 if args.compressed else 4096
                Manager().save_profile(profile)
                print(json.dumps({'profile': name, 'started': False}))
            else:
                print(json.dumps({'command': command, 'environment': overrides}, indent=2))
    except (OSError, ValueError, KeyError) as exc:
        parser.exit(1, f'{exc}\n')


if __name__ == '__main__':
    main()
