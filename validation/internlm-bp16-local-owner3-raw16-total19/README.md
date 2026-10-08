# Failed 3 GiB local-owner / 16 GiB shared-raw attempt

The 92-token InternLM2.5-20B F16 attempt exited before input with status 1
while loading, reporting `vk::Queue::submit: ErrorOutOfDeviceMemory`. It has no
accepted rate or inference result. The configuration used a 3 GiB local-owner
allowance and 16 GiB shared/raw allowance inside a 19 GiB combined quota. Logs
show a budget change between admission and restore shortly before failure, but
the evidence does not prove that as the exact cause; do not generalize this as a
confirmed admission-race failure. Runtime commit was `d796dead`; the captured
layer binary hash is `2d1590ba44618b94ecd51193135a529d8325b7ef4b00ec3a168374c7c8df3103`.

Original file sizes and SHA-256 values are retained in
`original-bytes-sha256.json`.
