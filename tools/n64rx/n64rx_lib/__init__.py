"""n64rx: build an N64 game for the original Xbox from your own ROM
(PLAN.md 5.1). The command-line front is cli.py; the pipeline is a library
so a GUI can sit on it later without touching the steps."""
import pathlib

ROOT = pathlib.Path(__file__).resolve().parents[3]   # the N64Recomp-X checkout
VERSION = (ROOT / 'VERSION').read_text().strip() if (ROOT / 'VERSION').exists() else 'unknown'


class N64rxError(Exception):
    """An error the user can act on: what went wrong, and what to do next."""

    def __init__(self, message, hint=None):
        super().__init__(message)
        self.message = message
        self.hint = hint
