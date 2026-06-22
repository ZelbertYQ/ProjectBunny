from __future__ import annotations

import os
import subprocess
from typing import List, Optional

import bpy
from bpy.types import Context, Operator

from ..utils.importer import DrawImporter
from ..utils.parser import (
    ConstantBufferBinding,
    DrawCall,
    IndexBinding,
    LogParser,
    VertexBinding,
)
from ..utils.paths import Paths
from ..utils.trace import TraceBrowser, TraceCommandList, TraceDraw, TraceResource


class FrameAnalysisUI:
    """Helpers shared by operators and by panel/init-time refresh.

    All methods are @staticmethods because the helpers are stateless — they
    read from ``context.scene.zaychik_settings`` and manipulate Blender data.
    """

    @staticmethod
    def refresh_items(context: Context) -> int:
        """Re-scan the dump root directory and rebuild the FrameAnalysis list.

        Safe to call from non-draw contexts (operators, startup init). Writes
        to CollectionProperty / IntProperty; the index update triggers
        ``SettingsCallbacks.on_frameanalysis_index_changed`` which syncs
        ``selected_frameanalysis_name`` and Config.json, so we do not set
        ``selected_frameanalysis_name`` nor call ``ConfigManager.save_config``
        ourselves.
        """
        settings = context.scene.zaychik_settings
        root_dir = Paths.normalize(bpy.path.abspath(settings.dump_root_directory).strip())
        selected_name = settings.selected_frameanalysis_name

        settings.frameanalysis_items.clear()

        directories = Paths.scan_frameanalysis_directories(root_dir)
        selected_index = 0
        for index, (name, path) in enumerate(directories):
            item = settings.frameanalysis_items.add()
            item.name = name
            item.path = path
            if name == selected_name:
                selected_index = index

        if settings.frameanalysis_items:
            settings.frameanalysis_index = min(
                selected_index, len(settings.frameanalysis_items) - 1
            )
        else:
            settings.frameanalysis_index = 0

        return len(settings.frameanalysis_items)

    @staticmethod
    def selected_path(context: Context) -> Optional[str]:
        """Return the path of the currently selected FrameAnalysis, or None.

        READ-ONLY. Safe to call from ``panel.draw()`` (no ID writes).
        """
        settings = context.scene.zaychik_settings
        if not settings.frameanalysis_items:
            return None
        index = settings.frameanalysis_index
        if index < 0 or index >= len(settings.frameanalysis_items):
            return None
        return settings.frameanalysis_items[index].path

    @staticmethod
    def selected_trace_resource(context: Context):
        settings = context.scene.zaychik_settings
        if not settings.trace_resource_items:
            return None
        index = settings.trace_resource_index
        if index < 0 or index >= len(settings.trace_resource_items):
            return None
        return settings.trace_resource_items[index]

    @staticmethod
    def populate_trace_draws(context: Context, draws: List[TraceDraw]) -> None:
        settings = context.scene.zaychik_settings
        settings.trace_draw_items.clear()
        settings.trace_resource_items.clear()
        settings.trace_draw_index = 0
        settings.trace_resource_index = 0

        for draw in draws:
            item = settings.trace_draw_items.add()
            item.name = f"{draw.draw:06d}"
            item.draw = draw.draw
            item.shader = draw.shader
            item.resource_count = len(draw.resources)
            item.cmdlist = draw.cmdlist
            item.func = draw.func
            item.pso = draw.pso
            item.index_count = draw.index_count
            item.vertex_count = draw.vertex_count
            item.start_index = draw.start_index
            item.start_vertex = draw.start_vertex
            item.base_vertex = draw.base_vertex
            item.instance_count = draw.instance_count
            item.start_instance = draw.start_instance
            item.groups_x = draw.groups_x
            item.groups_y = draw.groups_y
            item.groups_z = draw.groups_z
            item.ib_gpu = draw.ib_gpu
            item.ib_bytes = draw.ib_bytes
            item.ib_fmt = draw.ib_fmt
            shader_text = f" shader={draw.shader[:8]}" if draw.shader and draw.shader != "-" else ""
            if draw.func == "Dispatch":
                params = f" groups={draw.groups_x},{draw.groups_y},{draw.groups_z}"
            elif draw.index_count:
                params = (
                    f" idx={draw.index_count}"
                    f" start={draw.start_index}"
                    f" base={draw.base_vertex}"
                    f" inst={draw.instance_count}"
                )
            else:
                params = (
                    f" vtx={draw.vertex_count}"
                    f" start={draw.start_vertex}"
                    f" inst={draw.instance_count}"
                )
            item.label = f"{draw.draw:06d} {draw.func}{shader_text}{params} res={len(draw.resources)}"

    @staticmethod
    def populate_trace_command_lists(
        context: Context,
        command_lists: List[TraceCommandList],
    ) -> None:
        settings = context.scene.zaychik_settings
        settings.trace_command_list_items.clear()
        settings.trace_command_list_index = 0
        settings.trace_draw_items.clear()
        settings.trace_draw_index = 0
        settings.trace_resource_items.clear()
        settings.trace_resource_index = 0

        for command_list in command_lists:
            item = settings.trace_command_list_items.add()
            item.name = command_list.cmdlist
            item.cmdlist = command_list.cmdlist
            item.draw_count = len(command_list.draws)
            item.label = f"{command_list.cmdlist[-8:]} draws={len(command_list.draws)}"

    @staticmethod
    def populate_trace_resources(context: Context, resources: List[TraceResource]) -> None:
        settings = context.scene.zaychik_settings
        settings.trace_resource_items.clear()
        settings.trace_resource_index = 0

        for resource in resources:
            item = settings.trace_resource_items.add()
            item.name = resource.slot
            item.slot = resource.slot
            item.kind = resource.kind
            item.hash = resource.hash
            item.shader = resource.shader
            item.cmdlist = resource.cmdlist
            item.call_index = resource.call_index
            item.register = resource.register
            item.bytes = resource.bytes
            item.offset = resource.offset
            item.stride = resource.stride
            item.fmt_name = resource.fmt_name
            item.path = resource.path
            item.target = resource.target
            item.text_path = resource.text_path
            item.summary = resource.summary

    @staticmethod
    def selected_trace_draw_number(context: Context) -> Optional[int]:
        settings = context.scene.zaychik_settings
        if not settings.trace_draw_items:
            return None
        index = settings.trace_draw_index
        if index < 0 or index >= len(settings.trace_draw_items):
            return None
        return settings.trace_draw_items[index].draw

    @staticmethod
    def selected_trace_command_list(context: Context) -> Optional[str]:
        settings = context.scene.zaychik_settings
        if not settings.trace_command_list_items:
            return None
        index = settings.trace_command_list_index
        if index < 0 or index >= len(settings.trace_command_list_items):
            return None
        return settings.trace_command_list_items[index].cmdlist

    @staticmethod
    def load_selected_command_list(context: Context) -> bool:
        settings = context.scene.zaychik_settings
        dump_dir = FrameAnalysisUI.selected_path(context)
        cmdlist = FrameAnalysisUI.selected_trace_command_list(context)
        if not dump_dir or not cmdlist:
            return False

        command_list = TraceBrowser.find_command_list(
            Paths.normalize(bpy.path.abspath(dump_dir).strip()),
            cmdlist,
        )
        if command_list is None:
            settings.trace_draw_items.clear()
            settings.trace_resource_items.clear()
            return False

        FrameAnalysisUI.populate_trace_draws(context, command_list.draws)
        if command_list.draws:
            FrameAnalysisUI.populate_trace_resources(context, command_list.draws[0].resources)
        return True

    @staticmethod
    def load_selected_draw_resources(context: Context) -> bool:
        settings = context.scene.zaychik_settings
        dump_dir = FrameAnalysisUI.selected_path(context)
        draw_number = FrameAnalysisUI.selected_trace_draw_number(context)
        if not dump_dir or draw_number is None:
            return False

        draw = TraceBrowser.find_draw(
            Paths.normalize(bpy.path.abspath(dump_dir).strip()),
            draw_number,
        )
        if draw is None:
            settings.trace_resource_items.clear()
            return False

        FrameAnalysisUI.populate_trace_resources(context, draw.resources)
        return True

    @staticmethod
    def selected_trace_draw(context: Context) -> Optional[TraceDraw]:
        dump_dir = FrameAnalysisUI.selected_path(context)
        draw_number = FrameAnalysisUI.selected_trace_draw_number(context)
        if not dump_dir or draw_number is None:
            return None
        return TraceBrowser.find_draw(
            Paths.normalize(bpy.path.abspath(dump_dir).strip()),
            draw_number,
        )

    @staticmethod
    def relative_dump_path(dump_dir: str, path: str) -> str:
        if not path:
            return ""
        try:
            return os.path.relpath(path, dump_dir)
        except ValueError:
            return path

    @staticmethod
    def trace_draw_to_import_draw(dump_dir: str, trace_draw: TraceDraw) -> DrawCall:
        draw = DrawCall(
            event=trace_draw.draw,
            vs=trace_draw.shader,
            topology=trace_draw.topology,
            index_count=trace_draw.index_count,
            start_vertex=trace_draw.start_vertex,
            start_index=trace_draw.start_index,
            base_vertex=trace_draw.base_vertex,
            instance_count=trace_draw.instance_count,
        )

        for resource in trace_draw.resources:
            relative_path = FrameAnalysisUI.relative_dump_path(dump_dir, resource.path)
            if resource.slot == "ib":
                draw.index_binding = IndexBinding(
                    bytes=resource.bytes,
                    fmt=0,
                    fmt_name=resource.fmt_name,
                    relative_path=relative_path,
                    gpu=0,
                    offset=resource.offset,
                )
                continue
            if resource.slot.startswith("vb"):
                slot_text = resource.slot[2:]
                if not slot_text.isdigit():
                    continue
                binding = VertexBinding(
                    slot=int(slot_text),
                    bytes=resource.bytes,
                    stride=resource.stride,
                    fmt=0,
                    fmt_name=resource.fmt_name,
                    skin_source=resource.skin_source or "unknown",
                    relative_path=relative_path,
                    offset=resource.offset,
                )
                draw.vertex_bindings[binding.slot] = binding
                if binding.skin_source != "not_applicable":
                    if draw.skin_source == "unknown" or binding.skin_source == "gpu_preskinning":
                        draw.skin_source = binding.skin_source
                continue
            if resource.kind != "CBV":
                continue
            draw.constant_buffers.append(
                ConstantBufferBinding(
                    bind_space="graphics_root",
                    root_index=0,
                    reg=0,
                    bytes=resource.bytes,
                    relative_path=relative_path,
                )
            )
        return draw

    @staticmethod
    def open_path(path: str) -> None:
        if not path:
            raise FileNotFoundError("No path selected")
        if not os.path.exists(path):
            raise FileNotFoundError(path)
        os.startfile(path)  # type: ignore[attr-defined]

    @staticmethod
    def reveal_path(path: str) -> None:
        if not path:
            raise FileNotFoundError("No path selected")
        if not os.path.exists(path):
            raise FileNotFoundError(path)
        subprocess.Popen(["explorer", f"/select,{path}"])

    @staticmethod
    def draw_import_priority(draw: object) -> tuple[int, int, int, int]:
        """Rank likely model draws before UI quads and tiny helper draws."""
        vertex_bindings = getattr(draw, "vertex_bindings", {})
        slot_count = len(vertex_bindings)
        index_count = int(getattr(draw, "index_count", 0) or 0)
        instance_count = int(getattr(draw, "instance_count", 0) or 0)
        has_full_ue_streams = all(slot in vertex_bindings for slot in (0, 1, 4))
        likely_model = 1 if index_count >= 3000 and slot_count >= 3 else 0
        return (
            likely_model,
            1 if has_full_ue_streams else 0,
            index_count,
            -instance_count,
        )


class ZAYCHIK_OT_refresh_frameanalysis_list(Operator):
    bl_idname = "zaychik.refresh_frameanalysis_list"
    bl_label = "Refresh FrameAnalysis List"
    bl_description = "Scan the selected Win64 directory and list all FrameAnalysis folders"

    def execute(self, context: Context) -> set[str]:
        settings = context.scene.zaychik_settings
        root_dir = Paths.normalize(bpy.path.abspath(settings.dump_root_directory).strip())
        if not root_dir:
            self.report({"ERROR"}, "Please select the Win64 directory first")
            return {"CANCELLED"}
        if not os.path.isdir(root_dir):
            self.report({"ERROR"}, "Selected Win64 directory does not exist")
            return {"CANCELLED"}

        count = FrameAnalysisUI.refresh_items(context)
        context.scene.zaychik_settings.last_status = (
            f"Found {count} FrameAnalysis folder(s)"
        )
        self.report({"INFO"}, context.scene.zaychik_settings.last_status)
        return {"FINISHED"}


class ZAYCHIK_OT_import_dx12_dump(Operator):
    bl_idname = "zaychik.import_dx12_dump"
    bl_label = "Analyze log.jsonl And Import"
    bl_description = (
        "Analyze log.jsonl in the selected dump directory and try importing model meshes"
    )
    bl_options = {"REGISTER", "UNDO"}

    def execute(self, context: Context) -> set[str]:
        settings = context.scene.zaychik_settings
        dump_dir = FrameAnalysisUI.selected_path(context)
        if not dump_dir:
            self.report({"ERROR"}, "Please select a FrameAnalysis directory from the list")
            return {"CANCELLED"}

        dump_dir = Paths.normalize(bpy.path.abspath(dump_dir).strip())
        log_path = os.path.join(dump_dir, "log.jsonl")
        if not os.path.isfile(log_path):
            # Fall back to old format
            log_path = os.path.join(dump_dir, "log.txt")
        if not os.path.isdir(dump_dir):
            self.report({"ERROR"}, "Dump directory does not exist")
            return {"CANCELLED"}
        if not os.path.isfile(log_path):
            self.report({"ERROR"}, "Selected directory does not contain log.jsonl or log.txt")
            return {"CANCELLED"}

        try:
            draws = LogParser.parse(log_path)
        except Exception as exc:  # pragma: no cover - Blender runtime path
            filename = os.path.basename(log_path)
            self.report({"ERROR"}, f"Failed to parse {filename}: {exc}")
            settings.last_status = "Parse failed"
            return {"CANCELLED"}

        matching_draws = [
            draw for draw in draws if draw.index_binding and draw.vertex_bindings
        ]
        if settings.skin_source_filter != "all":
            matching_draws = [
                draw for draw in matching_draws
                if draw.skin_source == settings.skin_source_filter
            ]
        if not matching_draws:
            self.report({"WARNING"}, "No usable indexed draw calls were found")
            settings.last_status = "No usable draw calls"
            return {"CANCELLED"}

        matching_draws.sort(key=FrameAnalysisUI.draw_import_priority, reverse=True)

        success_count = 0
        messages: List[str] = []
        limit = min(settings.max_imports, len(matching_draws))
        for draw in matching_draws[:limit]:
            try:
                ok, message = DrawImporter.import_draw_call(
                    context,
                    dump_dir,
                    draw,
                    settings.apply_world_matrices,
                    settings.world_matrix_scale,
                    settings.vertex_layout_preset,
                )
            except Exception as exc:  # pragma: no cover - Blender runtime path
                ok = False
                message = f"event {draw.event}: {exc}"

            if ok:
                success_count += 1
                messages.append(message)
                if not settings.import_all_matching:
                    break
            elif len(messages) < 3:
                messages.append(message)

        if success_count == 0:
            preview = messages[0] if messages else "No draw could be imported"
            self.report({"WARNING"}, preview)
            settings.last_status = preview
            return {"CANCELLED"}

        settings.last_status = f"Imported {success_count} mesh object(s)"
        self.report({"INFO"}, settings.last_status)
        return {"FINISHED"}


class ZAYCHIK_OT_scan_trace_browser(Operator):
    bl_idname = "zaychik.scan_trace_browser"
    bl_label = "Scan FrameAnalysis"
    bl_description = "Scan DX12 log.jsonl and build CommandList -> Draw -> Resource relationships"

    def execute(self, context: Context) -> set[str]:
        settings = context.scene.zaychik_settings
        dump_dir = FrameAnalysisUI.selected_path(context)
        if not dump_dir:
            self.report({"ERROR"}, "Please select a FrameAnalysis directory first")
            return {"CANCELLED"}

        result = TraceBrowser.parse_result(Paths.normalize(bpy.path.abspath(dump_dir).strip()))
        FrameAnalysisUI.populate_trace_command_lists(context, result.command_lists)
        FrameAnalysisUI.load_selected_command_list(context)

        draw_count = sum(len(command_list.draws) for command_list in result.command_lists)
        settings.last_status = (
            f"Trace browser loaded {len(result.command_lists)} command list(s), "
            f"{draw_count} draw/dispatch call(s)"
        )
        self.report({"INFO"}, settings.last_status)
        return {"FINISHED"}


class ZAYCHIK_OT_load_trace_command_list(Operator):
    bl_idname = "zaychik.load_trace_command_list"
    bl_label = "Load CommandList"
    bl_description = "Load draw/dispatch calls for the selected CommandList"

    def execute(self, context: Context) -> set[str]:
        settings = context.scene.zaychik_settings
        dump_dir = FrameAnalysisUI.selected_path(context)
        cmdlist = FrameAnalysisUI.selected_trace_command_list(context)
        if not dump_dir or not cmdlist:
            self.report({"ERROR"}, "Please scan and select a CommandList first")
            return {"CANCELLED"}

        command_list = TraceBrowser.find_command_list(
            Paths.normalize(bpy.path.abspath(dump_dir).strip()),
            cmdlist,
        )
        if command_list is None:
            self.report({"WARNING"}, "Selected CommandList was not found in the dump")
            settings.trace_draw_items.clear()
            settings.trace_resource_items.clear()
            return {"CANCELLED"}

        FrameAnalysisUI.populate_trace_draws(context, command_list.draws)
        FrameAnalysisUI.load_selected_draw_resources(context)
        settings.last_status = (
            f"Loaded {len(command_list.draws)} draw/dispatch call(s) for {cmdlist}"
        )
        self.report({"INFO"}, settings.last_status)
        return {"FINISHED"}


class ZAYCHIK_OT_load_trace_draw_resources(Operator):
    bl_idname = "zaychik.load_trace_draw_resources"
    bl_label = "Load Draw Resources"
    bl_description = "Load slot resources for the selected draw/dispatch call"

    def execute(self, context: Context) -> set[str]:
        settings = context.scene.zaychik_settings
        dump_dir = FrameAnalysisUI.selected_path(context)
        draw_number = FrameAnalysisUI.selected_trace_draw_number(context)
        if not dump_dir or draw_number is None:
            self.report({"ERROR"}, "Please scan and select a draw/dispatch call first")
            return {"CANCELLED"}

        if not FrameAnalysisUI.load_selected_draw_resources(context):
            self.report({"WARNING"}, "Selected draw was not found in the dump")
            return {"CANCELLED"}

        settings.last_status = f"Loaded resource(s) for draw {draw_number:06d}"
        self.report({"INFO"}, settings.last_status)
        return {"FINISHED"}


class ZAYCHIK_OT_import_selected_trace_draw(Operator):
    bl_idname = "zaychik.import_selected_trace_draw"
    bl_label = "Identify And Import Draw"
    bl_description = "Identify the selected draw's slot layout with presets and import it into Blender"
    bl_options = {"REGISTER", "UNDO"}

    def execute(self, context: Context) -> set[str]:
        settings = context.scene.zaychik_settings
        dump_dir = FrameAnalysisUI.selected_path(context)
        if not dump_dir:
            self.report({"ERROR"}, "Please select a FrameAnalysis directory first")
            return {"CANCELLED"}

        dump_dir = Paths.normalize(bpy.path.abspath(dump_dir).strip())
        trace_draw = FrameAnalysisUI.selected_trace_draw(context)
        if trace_draw is None:
            self.report({"ERROR"}, "Please select a DrawIndexedInstanced/DrawInstanced call first")
            return {"CANCELLED"}
        if trace_draw.func == "Dispatch":
            self.report({"ERROR"}, "Dispatch calls cannot be imported as meshes")
            return {"CANCELLED"}

        draw = FrameAnalysisUI.trace_draw_to_import_draw(dump_dir, trace_draw)
        try:
            ok, message = DrawImporter.import_draw_call(
                context,
                dump_dir,
                draw,
                settings.apply_world_matrices,
                settings.world_matrix_scale,
                settings.vertex_layout_preset,
            )
        except Exception as exc:  # pragma: no cover - Blender runtime path
            ok = False
            message = f"draw {trace_draw.draw}: {exc}"

        settings.last_status = message
        self.report({"INFO"} if ok else {"WARNING"}, message)
        return {"FINISHED"} if ok else {"CANCELLED"}


class ZAYCHIK_OT_open_trace_resource(Operator):
    bl_idname = "zaychik.open_trace_resource"
    bl_label = "Open Resource"
    bl_description = "Open the selected buffer or texture file"

    use_target: bpy.props.BoolProperty(default=True)  # type: ignore

    def execute(self, context: Context) -> set[str]:
        resource = FrameAnalysisUI.selected_trace_resource(context)
        if resource is None:
            self.report({"ERROR"}, "Please select a resource first")
            return {"CANCELLED"}

        path = resource.target if self.use_target and resource.target else resource.path
        try:
            FrameAnalysisUI.open_path(path)
        except OSError as exc:
            self.report({"ERROR"}, f"Cannot open resource: {exc}")
            return {"CANCELLED"}
        return {"FINISHED"}


class ZAYCHIK_OT_reveal_trace_resource(Operator):
    bl_idname = "zaychik.reveal_trace_resource"
    bl_label = "Reveal Resource"
    bl_description = "Reveal the selected buffer or texture in Explorer"

    use_target: bpy.props.BoolProperty(default=True)  # type: ignore

    def execute(self, context: Context) -> set[str]:
        resource = FrameAnalysisUI.selected_trace_resource(context)
        if resource is None:
            self.report({"ERROR"}, "Please select a resource first")
            return {"CANCELLED"}

        path = resource.target if self.use_target and resource.target else resource.path
        try:
            FrameAnalysisUI.reveal_path(path)
        except OSError as exc:
            self.report({"ERROR"}, f"Cannot reveal resource: {exc}")
            return {"CANCELLED"}
        return {"FINISHED"}


class ZAYCHIK_OT_open_trace_metadata(Operator):
    bl_idname = "zaychik.open_trace_metadata"
    bl_label = "Open Metadata"
    bl_description = "Open the selected resource metadata txt file"

    def execute(self, context: Context) -> set[str]:
        resource = FrameAnalysisUI.selected_trace_resource(context)
        if resource is None:
            self.report({"ERROR"}, "Please select a resource first")
            return {"CANCELLED"}

        try:
            FrameAnalysisUI.open_path(resource.text_path)
        except OSError as exc:
            self.report({"ERROR"}, f"Cannot open metadata: {exc}")
            return {"CANCELLED"}
        return {"FINISHED"}


CLASSES = (
    ZAYCHIK_OT_refresh_frameanalysis_list,
    ZAYCHIK_OT_import_dx12_dump,
    ZAYCHIK_OT_scan_trace_browser,
    ZAYCHIK_OT_load_trace_command_list,
    ZAYCHIK_OT_load_trace_draw_resources,
    ZAYCHIK_OT_import_selected_trace_draw,
    ZAYCHIK_OT_open_trace_resource,
    ZAYCHIK_OT_reveal_trace_resource,
    ZAYCHIK_OT_open_trace_metadata,
)


def register() -> None:
    for klass in CLASSES:
        bpy.utils.register_class(klass)


def unregister() -> None:
    for klass in reversed(CLASSES):
        try:
            bpy.utils.unregister_class(klass)
        except (RuntimeError, ValueError):
            pass
