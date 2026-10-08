# Novum Xenium integration handoff

This implementation lives entirely in zVram. It does not modify Novum Xenium
(formerly Odysseus), its Ollama service, its default model, or model files.
Changes inside Novum Xenium should be implemented through a separate PR.
The zVram management GUI/TUI are standalone applications. Do not embed the
management GUI in Novum; the two projects share styling, not their UI lifetime.

## Current bridge

`zvram_model.py` reads the desktop checkout and port from
`~/.config/dev.novum.xenium/config.json`. `discover_models(checkout)` resolves
GGUF blobs from that checkout's existing Ollama manifests. Explicit GGUF paths
also work. Model discovery does not start inference or copy weights.

The existing Ollama process cannot be retroactively wrapped in the zVram Vulkan
layer. This bridge starts a separate Vulkan `llama-server` using the same model
file. Its OpenAI-compatible endpoint binds to `127.0.0.1`, has one inference
slot, and has an explicit context size. It does not expose a LAN service.

Build the server if absent:

```sh
cmake --build build/third-party/llama-vulkan-build --target llama-server -j 4
python3 zvram_model.py list
```

`build_server_command(path, alias, ...)` returns `(argv, environment)` for a
controller to pass directly to `subprocess.Popen`, without shell expansion.
`mode="native"` produces an unwrapped reference command. The default `spill`
mode uses the virtual heap; `compressed=True` explicitly opts into experimental
BP16 range paging. The latter accepts `resident_mib`, `cold_mib`,
`clean_cache_mib`, and `headroom_mib`. These are eligible allocation budgets,
not global physical VRAM reservations or cross-process scheduling guarantees.
The default compression profile is experimental and must be validated for each
model, context size, and GPU. Native images and untracked allocations remain
outside the resident limit.

The helper's `command` action prints a command preview and only its zVram
environment settings; it never starts a model. A controller must retain the
complete returned environment, including the GGML Vulkan settings, when
launching. Use the manager for execution and lifetime management.

## Endpoint registration

After the controller observes a healthy server and `/v1/models` advertises the
requested alias, call `register_endpoint(alias, port)`. Registration uses the
existing local desktop session cookie, accepts Netscape and WebKit cookie
formats, rejects expired/wrong-host cookies, ignores HTTP proxy variables,
and refuses redirects. Cookie values must never be logged.

The helper creates a user-owned `zVram · <alias>` model endpoint through
Novum's existing `/api/model-endpoints` API. Re-registration updates only that
port's existing zVram endpoint. It refuses to overwrite an endpoint belonging
to another provider. Other endpoints and defaults remain unchanged. Login in
the desktop app is required; the helper does not bypass authentication.

## Suggested Novum PR

Add a provider action that lists existing local GGUF models, lets the user
choose the alias, port, context, and opt-in paging profile, then invokes the
manager's existing model command. Show startup/health/error state and offer an
explicit stop action. Register only after the server is healthy. Keep the
current Ollama endpoint available so users choose either provider per chat.
Do not automatically start large models or unload unrelated Ollama models.

The active Novum web backend runs inside Docker while zVram and the model
server run on the host. Implement host launch/control through a narrow desktop
adapter with typed, allowlisted model actions, rather than a web endpoint that
executes arbitrary commands. The browser-only deployment can consume an
already-running loopback model provider; it does not gain host process-control
permissions. A future host agent, if needed, should be a user service with an
authenticated local IPC contract, not a privileged GPU daemon.

Suggested desktop verbs: discover models, save a validated model profile,
start a named profile, read its health/status, stop that owned profile, and
register its verified provider. Use argv arrays and preserve the manager's
RAM/swap guards. Do not expose arbitrary shell, environment, or PID-signal
arguments to web content. The standalone manager keeps general application
profiles; Novum's adapter only needs its model profiles.

Runtime priority changes may require restarting an owned model server. Label
that behavior clearly and preserve chat history. Show memory limits as zVram
eligible allocation budgets, not reserved physical VRAM.

CPU-only bridge checks: `python3 -m unittest -v test_zvram_model.py`.
