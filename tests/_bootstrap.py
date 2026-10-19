"""Add source modules and repository tools when tests run from tests/."""
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT / "src/python"), str(ROOT / "tools"), str(ROOT)]
