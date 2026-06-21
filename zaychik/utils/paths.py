from __future__ import annotations

import os
from typing import List, Tuple


class Paths:
    """Filesystem helpers for dump directory navigation (pure utility namespace)."""

    @staticmethod
    def normalize(path: str) -> str:
        return os.path.normpath(path)

    @staticmethod
    def resolve_binding(root_dir: str, relative_path: str) -> str:
        fixed_relative = relative_path.replace("\\", os.sep)
        if os.sep not in fixed_relative:
            fixed_relative = os.path.join("deduped", fixed_relative)
        return Paths.normalize(os.path.join(root_dir, fixed_relative))

    @staticmethod
    def scan_frameanalysis_directories(root_dir: str) -> List[Tuple[str, str]]:
        if not root_dir or not os.path.isdir(root_dir):
            return []

        entries: List[Tuple[str, str]] = []
        for entry in os.scandir(root_dir):
            if not entry.is_dir():
                continue
            if not entry.name.startswith("FrameAnalysis-"):
                continue
            entries.append((entry.name, Paths.normalize(entry.path)))

        entries.sort(key=lambda item: item[0], reverse=True)
        return entries
