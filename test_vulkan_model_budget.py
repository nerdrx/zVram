#!/usr/bin/env python3
"""CPU-only checks for token budget and decode evidence validation."""

import importlib.util
from pathlib import Path
import subprocess
import sys


MODULE = Path(__file__).with_name("check_vulkan_idle_model.py")
spec = importlib.util.spec_from_file_location("idle_model", MODULE)
idle_model = importlib.util.module_from_spec(spec)
spec.loader.exec_module(idle_model)


def main():
    assert idle_model.has_decode_tokens({"decode_runs": 1, "tokens_per_second": 0.1})
    for metrics in ({"decode_runs": 0, "tokens_per_second": 1},
                    {"decode_runs": 1, "tokens_per_second": 0},
                    {"decode_runs": None, "tokens_per_second": None}, None):
        assert not idle_model.has_decode_tokens(metrics), metrics

    for tokens in ("31", "129"):
        result = subprocess.run(
            [sys.executable, str(MODULE), "--binary", "/missing", "--model", "/missing",
             "--tokens", tokens], capture_output=True, text=True)
        assert result.returncode == 2, result
        assert "tokens must be 32..128" in result.stderr, result.stderr
        assert "existing executable" not in result.stderr, result.stderr


if __name__ == "__main__":
    main()
