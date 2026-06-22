from __future__ import annotations

import os
import json
import re
from dataclasses import dataclass
from typing import ClassVar, Dict, List, Optional, Tuple


@dataclass
class TraceResource:
    draw: int
    slot: str
    kind: str
    shader: str
    hash: str
    path: str
    cmdlist: str = ""
    call_index: int = 0
    register: str = ""
    bytes: int = 0
    offset: int = 0
    stride: int = 0
    fmt_name: str = ""
    skin_source: str = ""
    text_path: str = ""
    target: str = ""
    summary: str = ""


@dataclass
class TraceDraw:
    draw: int
    shader: str
    cmdlist: str
    func: str
    pso: int
    topology: str
    index_count: int
    vertex_count: int
    start_index: int
    start_vertex: int
    base_vertex: int
    instance_count: int
    start_instance: int
    groups_x: int
    groups_y: int
    groups_z: int
    ib_gpu: str
    ib_bytes: int
    ib_fmt: int
    resources: List[TraceResource]


@dataclass
class TraceCommandList:
    cmdlist: str
    draws: List[TraceDraw]


@dataclass
class TraceResult:
    command_lists: List[TraceCommandList]
    resources: List[TraceResource]


class TraceBrowser:
    """Parse ProjectBunny DX12 FrameAnalysis log.jsonl into command-list traces."""

    _CACHE: ClassVar[Dict[Tuple[str, int, int], TraceResult]] = {}

    _DX11_NAME_RE = re.compile(
        r"^(?P<draw>\d+)-(?P<slot>(?:ib|vb\d+|[a-z]{2}-cb\d+|[a-z]{2}-t\d+|[a-z]{2}-u\d+|o\d+))"
        r"(?:=(?P<hash>[0-9a-fA-F]+))?"
        r"(?:-(?P<shader>[a-z]{2})=(?P<shader_hash>[0-9a-fA-F]+))?"
        r"\.(?P<ext>buf|txt|dds)$"
    )

    @staticmethod
    def _resource_kind(slot: str, ext: str) -> str:
        if slot == "ib":
            return "Index Buffer"
        if slot.startswith("vb"):
            return "Vertex Buffer"
        if "-cb" in slot:
            return "Constant Buffer"
        if "-t" in slot:
            return "Texture/SRV"
        if "-u" in slot:
            return "UAV"
        if slot.startswith("o"):
            return "Output"
        if ext == "dds":
            return "Texture"
        return "Resource"

    @staticmethod
    def _to_int(value: object) -> int:
        if value is None or value == "":
            return 0
        if isinstance(value, bool):
            return int(value)
        if isinstance(value, int):
            return value
        text = str(value).strip()
        try:
            if text.lower().startswith("0x"):
                return int(text, 16)
            return int(text)
        except ValueError:
            return 0

    @staticmethod
    def _to_str(value: object, default: str = "") -> str:
        if value is None:
            return default
        return str(value)

    @staticmethod
    def _read_summary(path: str) -> str:
        if not path or not os.path.isfile(path):
            return ""
        try:
            lines: List[str] = []
            with open(path, "r", encoding="utf-8", errors="replace") as handle:
                for _ in range(4):
                    line = handle.readline()
                    if not line:
                        break
                    line = line.strip()
                    if line:
                        lines.append(line)
            return "; ".join(lines)
        except OSError:
            return ""

    @staticmethod
    def _resolve_target(path: str) -> str:
        try:
            if os.path.islink(path):
                target = os.readlink(path)
                if not os.path.isabs(target):
                    target = os.path.join(os.path.dirname(path), target)
                return os.path.normpath(target)
        except OSError:
            pass
        return os.path.normpath(path)

    @staticmethod
    def _resolve_dump_file(dump_dir: str, relative_file: str) -> str:
        if not relative_file:
            return ""
        fixed = relative_file.replace("\\", os.sep)
        if os.sep not in fixed:
            fixed = os.path.join("deduped", fixed)
        return os.path.normpath(os.path.join(dump_dir, fixed))

    @staticmethod
    def _iter_json_lines(path: str):
        with open(path, "r", encoding="utf-8", errors="replace") as handle:
            for line in handle:
                line = line.strip()
                if not line:
                    continue
                try:
                    obj = json.loads(line)
                except json.JSONDecodeError:
                    continue
                if isinstance(obj, dict):
                    yield obj

    @classmethod
    def _dx12_resource_from_obj(cls, dump_dir: str, obj: dict) -> TraceResource:
        func = cls._to_str(obj.get("func"))
        call_index = cls._to_int(obj.get("call_index", obj.get("index")))
        cmdlist = cls._to_str(obj.get("cmdlist"))
        relative_file = cls._to_str(obj.get("file"))
        path = cls._resolve_dump_file(dump_dir, relative_file)
        text_path = cls._resolve_dump_file(dump_dir, cls._to_str(obj.get("text")))
        if not text_path and path:
            candidate = f"{path}.txt"
            if os.path.isfile(candidate):
                text_path = candidate

        if func == "BindIA":
            role = cls._to_str(obj.get("role"))
            slot = "ib" if role == "IB" else f"vb{cls._to_int(obj.get('slot'))}"
            kind = "Index Buffer" if role == "IB" else "Vertex Buffer"
            register = role if role == "IB" else f"slot {cls._to_int(obj.get('slot'))}"
        else:
            kind = cls._to_str(obj.get("kind"), "Resource")
            root = cls._to_int(obj.get("root"))
            reg = cls._to_int(obj.get("reg"))
            space = cls._to_int(obj.get("space"))
            if reg == 4294967295:
                register = f"root{root}"
            else:
                register = f"root{root} r{reg} s{space}"
            slot = f"{kind.lower()}:{register}"

        shader = cls._to_str(obj.get("vs"))
        if not shader or shader == "-":
            shader = cls._to_str(obj.get("cs"), "-")

        summary_parts = [
            part for part in (
                f"bytes={cls._to_int(obj.get('bytes'))}" if obj.get("bytes") is not None else "",
                f"stride={cls._to_int(obj.get('stride'))}" if obj.get("stride") is not None else "",
                cls._to_str(obj.get("fmt_name")) if obj.get("fmt_name") else "",
                cls._to_str(obj.get("skin_source")) if obj.get("skin_source") else "",
            )
            if part
        ]
        text_summary = cls._read_summary(text_path)
        if text_summary:
            summary_parts.append(text_summary)

        return TraceResource(
            draw=call_index,
            slot=slot,
            kind=kind,
            shader=shader,
            hash=cls._to_str(obj.get("hash")),
            path=path,
            cmdlist=cmdlist,
            call_index=call_index,
            register=register,
            bytes=cls._to_int(obj.get("bytes")),
            offset=cls._to_int(obj.get("offset")),
            stride=cls._to_int(obj.get("stride")),
            fmt_name=cls._to_str(obj.get("fmt_name")),
            skin_source=cls._to_str(obj.get("skin_source")),
            text_path=text_path,
            target=cls._resolve_target(path) if path else "",
            summary="; ".join(summary_parts),
        )

    @classmethod
    def _parse_dx12(cls, dump_dir: str) -> TraceResult:
        log_path = os.path.join(dump_dir, "log.jsonl")
        if not os.path.isfile(log_path):
            return TraceResult(command_lists=[], resources=[])

        draw_objs: Dict[int, dict] = {}
        resources_by_call: Dict[int, List[TraceResource]] = {}
        for obj in cls._iter_json_lines(log_path):
            func = cls._to_str(obj.get("func"))
            if func in {"DrawIndexedInstanced", "DrawInstanced", "Dispatch"}:
                call_index = cls._to_int(obj.get("call_index", obj.get("index")))
                draw_objs[call_index] = obj
                continue
            if func not in {"BindIA", "BindResource"}:
                continue
            resource = cls._dx12_resource_from_obj(dump_dir, obj)
            resources_by_call.setdefault(resource.call_index, []).append(resource)

        command_lists: Dict[str, List[TraceDraw]] = {}
        all_resources: List[TraceResource] = []
        for call_index, obj in sorted(draw_objs.items()):
            cmdlist = cls._to_str(obj.get("cmdlist"), "-")
            shader = cls._to_str(obj.get("vs"))
            if not shader or shader == "-":
                shader = cls._to_str(obj.get("cs"), "-")
            resources = sorted(
                resources_by_call.get(call_index, []),
                key=cls._slot_sort_key,
            )
            for resource in resources:
                resource.cmdlist = cmdlist
            all_resources.extend(resources)
            trace_draw = TraceDraw(
                draw=call_index,
                shader=shader,
                cmdlist=cmdlist,
                func=cls._to_str(obj.get("func")),
                pso=cls._to_int(obj.get("pso")),
                topology=cls._to_str(obj.get("topology")),
                index_count=cls._to_int(obj.get("index_count")),
                vertex_count=cls._to_int(obj.get("vertex_count")),
                start_index=cls._to_int(obj.get("start_index")),
                start_vertex=cls._to_int(obj.get("start_vertex")),
                base_vertex=cls._to_int(obj.get("base_vertex")),
                instance_count=cls._to_int(obj.get("instance_count")),
                start_instance=cls._to_int(obj.get("start_instance")),
                groups_x=cls._to_int(obj.get("groups_x")),
                groups_y=cls._to_int(obj.get("groups_y")),
                groups_z=cls._to_int(obj.get("groups_z")),
                ib_gpu=cls._to_str(obj.get("ib_gpu")),
                ib_bytes=cls._to_int(obj.get("ib_bytes")),
                ib_fmt=cls._to_int(obj.get("ib_fmt")),
                resources=resources,
            )
            command_lists.setdefault(cmdlist, []).append(trace_draw)

        grouped = [
            TraceCommandList(cmdlist=cmdlist, draws=draws)
            for cmdlist, draws in command_lists.items()
        ]
        grouped.sort(key=lambda item: item.cmdlist)
        return TraceResult(command_lists=grouped, resources=all_resources)

    @staticmethod
    def _slot_sort_key(resource: TraceResource) -> tuple[int, str, int, str]:
        slot = resource.slot
        if slot == "ib":
            return (0, "ib", 0, slot)
        match = re.match(r"^vb(\d+)$", slot)
        if match:
            return (1, "vb", int(match.group(1)), slot)
        match = re.match(r"^([a-z]{2})-(cb|t|u)(\d+)$", slot)
        if match:
            stage, bind_type, index = match.groups()
            order = {"cb": 2, "t": 3, "u": 4}.get(bind_type, 9)
            return (order, stage, int(index), slot)
        match = re.match(r"^o(\d+)$", slot)
        if match:
            return (5, "o", int(match.group(1)), slot)
        return (9, slot, 0, slot)

    @classmethod
    def parse(cls, dump_dir: str) -> List[TraceDraw]:
        result = cls.parse_result(dump_dir)
        return [draw for command_list in result.command_lists for draw in command_list.draws]

    @classmethod
    def parse_result(cls, dump_dir: str) -> TraceResult:
        log_path = os.path.join(dump_dir, "log.jsonl")
        if os.path.isfile(log_path):
            stat = os.stat(log_path)
            key = (os.path.normpath(dump_dir), stat.st_mtime_ns, stat.st_size)
            cached = cls._CACHE.get(key)
            if cached is not None:
                return cached
            dx12 = cls._parse_dx12(dump_dir)
            if dx12.command_lists:
                cls._CACHE.clear()
                cls._CACHE[key] = dx12
                return dx12

        dx12 = cls._parse_dx12(dump_dir)
        if dx12.command_lists:
            return dx12
        return cls._parse_dx11_fallback(dump_dir)

    @classmethod
    def _parse_dx11_fallback(cls, dump_dir: str) -> TraceResult:
        grouped: Dict[int, Dict[str, TraceResource]] = {}
        if not dump_dir or not os.path.isdir(dump_dir):
            return TraceResult(command_lists=[], resources=[])

        for entry in os.scandir(dump_dir):
            if not entry.is_file():
                continue
            match = cls._DX11_NAME_RE.match(entry.name)
            if not match:
                continue

            draw = int(match.group("draw"))
            slot = match.group("slot")
            ext = match.group("ext")
            key = f"{draw}:{slot}"
            draw_group = grouped.setdefault(draw, {})
            resource = draw_group.get(key)
            if resource is None:
                resource = TraceResource(
                    draw=draw,
                    slot=slot,
                    kind=cls._resource_kind(slot, ext),
                    shader=match.group("shader_hash") or "",
                    hash=match.group("hash") or "",
                    path="",
                )
                draw_group[key] = resource

            path = os.path.normpath(entry.path)
            if ext == "txt":
                resource.text_path = path
                resource.summary = cls._read_summary(path)
            else:
                resource.path = path
                resource.target = cls._resolve_target(path)
                if ext == "dds" and not resource.kind.startswith("Texture"):
                    resource.kind = cls._resource_kind(slot, ext)

        draws: List[TraceDraw] = []
        for draw, resources_by_key in grouped.items():
            resources = sorted(resources_by_key.values(), key=cls._slot_sort_key)
            shader = next((item.shader for item in resources if item.shader), "")
            draws.append(
                TraceDraw(
                    draw=draw,
                    shader=shader,
                    cmdlist="DX11",
                    func="Draw",
                    pso=0,
                    topology="",
                    index_count=0,
                    vertex_count=0,
                    start_index=0,
                    start_vertex=0,
                    base_vertex=0,
                    instance_count=0,
                    start_instance=0,
                    groups_x=0,
                    groups_y=0,
                    groups_z=0,
                    ib_gpu="",
                    ib_bytes=0,
                    ib_fmt=0,
                    resources=resources,
                )
            )
        draws.sort(key=lambda item: item.draw)
        resources = [resource for draw in draws for resource in draw.resources]
        return TraceResult(
            command_lists=[TraceCommandList(cmdlist="DX11", draws=draws)] if draws else [],
            resources=resources,
        )

    @classmethod
    def find_command_list(cls, dump_dir: str, cmdlist: str) -> Optional[TraceCommandList]:
        for command_list in cls.parse_result(dump_dir).command_lists:
            if command_list.cmdlist == cmdlist:
                return command_list
        return None

    @classmethod
    def find_draw(cls, dump_dir: str, draw_number: int) -> Optional[TraceDraw]:
        for draw in cls.parse(dump_dir):
            if draw.draw == draw_number:
                return draw
        return None
