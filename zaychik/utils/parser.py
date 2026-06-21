"""Frame-analysis log parser (strict, typed)."""
from __future__ import annotations

import json
import os
from dataclasses import dataclass, field
from typing import Dict, Iterable, List, Optional


@dataclass
class VertexBinding:
    slot: int
    bytes: int
    stride: int
    fmt: int
    fmt_name: str
    skin_source: str
    relative_path: str
    gpu: int = 0
    offset: int = 0


@dataclass
class IndexBinding:
    bytes: int
    fmt: int
    fmt_name: str
    relative_path: str
    gpu: int = 0
    offset: int = 0


@dataclass
class ConstantBufferBinding:
    bind_space: str
    root_index: int
    reg: int
    bytes: int
    relative_path: str


@dataclass
class DrawCall:
    event: int
    vs: str
    topology: str
    index_count: int
    start_vertex: int
    start_index: int
    base_vertex: int
    instance_count: int
    skin_source: str = "unknown"
    vertex_bindings: Dict[int, VertexBinding] = field(default_factory=dict)
    index_binding: Optional[IndexBinding] = None
    constant_buffers: List[ConstantBufferBinding] = field(default_factory=list)


class LogParser:
    """Parses a DX12 frame-analysis log.jsonl into a list of DrawCall objects."""

    @staticmethod
    def _to_int(value: str, base: int = 10) -> int:
        return int(value, base) if value else 0

    @staticmethod
    def _iter_json_lines(path: str) -> Iterable[dict]:
        with open(path, "r", encoding="utf-8", errors="replace") as handle:
            for line in handle:
                line = line.strip()
                if line:
                    try:
                        yield json.loads(line)
                    except json.JSONDecodeError:
                        continue

    @classmethod
    def parse(cls, log_path: str) -> List[DrawCall]:
        # Try log.jsonl first, then fall back to log.txt
        if not os.path.exists(log_path) and log_path.endswith('.jsonl'):
            txt_path = log_path[:-6] + '.txt'
            if os.path.exists(txt_path):
                return cls._parse_txt(txt_path)
        elif not os.path.exists(log_path):
            jsonl_path = log_path[:-4] + '.jsonl' if log_path.endswith('.txt') else log_path + '.jsonl'
            if os.path.exists(jsonl_path):
                return cls.parse(jsonl_path)

        draws: Dict[int, DrawCall] = {}

        for obj in cls._iter_json_lines(log_path):
            type_str = obj.get('type')

            if type_str in ('call.draw', 'call.dispatch'):
                event = obj.get('event', 0)
                draws[event] = DrawCall(
                    event=event,
                    vs=obj.get('vs', '-'),
                    topology=obj.get('topology', 'UNKNOWN'),
                    index_count=obj.get('index_count', 0),
                    start_vertex=obj.get('start_vertex', 0),
                    start_index=obj.get('start_index', 0),
                    base_vertex=obj.get('base_vertex', 0),
                    instance_count=obj.get('instance_count', 0),
                )
                continue

            if type_str == 'bind.ia':
                event = obj.get('event', 0)
                draw = draws.get(event)
                role = obj.get('role', '')
                relative_path = obj.get('file', '')
                gpu_str = obj.get('gpu', '0x0')
                gpu = int(gpu_str, 16) if isinstance(gpu_str, str) else gpu_str
                offset = obj.get('offset', 0)

                if role == 'VB':
                    binding = VertexBinding(
                        slot=obj.get('slot', 0),
                        bytes=obj.get('bytes', 0),
                        stride=obj.get('stride', 0),
                        fmt=obj.get('fmt', 0),
                        fmt_name=obj.get('fmt_name', ''),
                        skin_source=obj.get('skin_source') or 'unknown',
                        relative_path=relative_path,
                        gpu=gpu,
                        offset=offset,
                    )
                    if draw is not None:
                        draw.vertex_bindings[binding.slot] = binding
                        if binding.skin_source != 'not_applicable':
                            if draw.skin_source == 'unknown' or binding.skin_source == 'gpu_preskinning':
                                draw.skin_source = binding.skin_source
                elif role == 'IB':
                    ib = IndexBinding(
                        bytes=obj.get('bytes', 0),
                        fmt=obj.get('fmt', 0),
                        fmt_name=obj.get('fmt_name', ''),
                        relative_path=relative_path,
                        gpu=gpu,
                        offset=offset,
                    )
                    if draw is not None:
                        draw.index_binding = ib
                continue

            if type_str == 'bind.resource':
                event = obj.get('event', 0)
                draw = draws.get(event)
                if draw is None:
                    continue
                if obj.get('kind') != 'CBV':
                    continue
                draw.constant_buffers.append(
                    ConstantBufferBinding(
                        bind_space=obj.get('bind', ''),
                        root_index=obj.get('root', 0),
                        reg=obj.get('reg', 0),
                        bytes=obj.get('bytes', 0),
                        relative_path=obj.get('file', ''),
                    )
                )

        return sorted(draws.values(), key=lambda item: item.event)

    @classmethod
    def _parse_txt(cls, log_path: str) -> List[DrawCall]:
        """Fallback parser for old log.txt format."""
        import re

        # Key=value pairs in the log can be nested inside longer names
        _TOPLEVEL = r"(?<![A-Za-z0-9_])"

        _CALL_DRAW_RE = re.compile(
            r"call\.draw function=draw_indexed event=(?P<event>\d+).*?"
            r"vs=(?P<vs>[0-9a-f-]+).*?"
            r"topology=(?P<topology>[A-Z0-9_]+).*?"
            + _TOPLEVEL + r"index_count=(?P<index_count>\d+).*?"
            + _TOPLEVEL + r"start_vertex=(?P<start_vertex>-?\d+).*?"
            + _TOPLEVEL + r"start_index=(?P<start_index>-?\d+).*?"
            + _TOPLEVEL + r"base_vertex=(?P<base_vertex>-?\d+).*?"
            + _TOPLEVEL + r"instance_count=(?P<instance_count>\d+)"
        )

        _BIND_IA_RE = re.compile(
            r"bind\.ia event=(?P<event>\d+).*?"
            r"role=(?P<role>VB|IB).*?"
            r"slot=(?P<slot>\d+).*?"
            + _TOPLEVEL + r"gpu=(?P<gpu>0x[0-9a-fA-F]+).*?"
            + _TOPLEVEL + r"offset=(?P<offset>\d+).*?"
            + _TOPLEVEL + r"bytes=(?P<bytes>\d+).*?"
            r"stride=(?P<stride>\d+).*?"
            r"fmt=(?P<fmt>\d+).*?"
            r"fmt_name=(?P<fmt_name>[A-Z0-9_]+).*?"
            r"(?:" + _TOPLEVEL + r"skin_source=(?P<skin_source>[a-z_]+).*?)?"
            r"file=(?P<file>deduped\\[^ ]+)"
        )

        _BIND_RESOURCE_RE = re.compile(
            r"bind\.resource event=(?P<event>\d+).*?"
            r"bind=(?P<bind>[a-z_]+).*?"
            r"kind=(?P<kind>[A-Z]+).*?"
            + _TOPLEVEL + r"root=(?P<root>\d+).*?"
            + _TOPLEVEL + r"reg=(?P<reg>\d+).*?"
            + _TOPLEVEL + r"bytes=(?P<bytes>\d+).*?"
            r"file=(?P<file>deduped\\[^ ]+)"
        )

        def _iter_lines(path: str) -> Iterable[str]:
            with open(path, "r", encoding="utf-8", errors="replace") as handle:
                for line in handle:
                    yield line.strip()

        draws: Dict[int, DrawCall] = {}

        for line in _iter_lines(log_path):
            draw_match = _CALL_DRAW_RE.search(line)
            if draw_match:
                event = int(draw_match.group("event"))
                draws[event] = DrawCall(
                    event=event,
                    vs=draw_match.group("vs"),
                    topology=draw_match.group("topology"),
                    index_count=int(draw_match.group("index_count")),
                    start_vertex=int(draw_match.group("start_vertex")),
                    start_index=int(draw_match.group("start_index")),
                    base_vertex=int(draw_match.group("base_vertex")),
                    instance_count=int(draw_match.group("instance_count")),
                )
                continue

            bind_match = _BIND_IA_RE.search(line)
            if bind_match:
                event = int(bind_match.group("event"))
                draw = draws.get(event)
                role = bind_match.group("role")
                relative_path = bind_match.group("file")
                gpu = cls._to_int(bind_match.group("gpu"), 16)
                offset = int(bind_match.group("offset"))
                if role == "VB":
                    binding = VertexBinding(
                        slot=int(bind_match.group("slot")),
                        bytes=int(bind_match.group("bytes")),
                        stride=int(bind_match.group("stride")),
                        fmt=int(bind_match.group("fmt")),
                        fmt_name=bind_match.group("fmt_name"),
                        skin_source=bind_match.group("skin_source") or "unknown",
                        relative_path=relative_path,
                        gpu=gpu,
                        offset=offset,
                    )
                    if draw is not None:
                        draw.vertex_bindings[binding.slot] = binding
                        if binding.skin_source != "not_applicable":
                            if draw.skin_source == "unknown" or binding.skin_source == "gpu_preskinning":
                                draw.skin_source = binding.skin_source
                elif role == "IB":
                    ib = IndexBinding(
                        bytes=int(bind_match.group("bytes")),
                        fmt=int(bind_match.group("fmt")),
                        fmt_name=bind_match.group("fmt_name"),
                        relative_path=relative_path,
                        gpu=gpu,
                        offset=offset,
                    )
                    if draw is not None:
                        draw.index_binding = ib
                continue

            resource_match = _BIND_RESOURCE_RE.search(line)
            if resource_match is None:
                continue

            draw = draws.get(int(resource_match.group("event")))
            if draw is None:
                continue
            if resource_match.group("kind") != "CBV":
                continue
            draw.constant_buffers.append(
                ConstantBufferBinding(
                    bind_space=resource_match.group("bind"),
                    root_index=int(resource_match.group("root")),
                    reg=int(resource_match.group("reg")),
                    bytes=int(resource_match.group("bytes")),
                    relative_path=resource_match.group("file"),
                )
            )

        return sorted(draws.values(), key=lambda item: item.event)
