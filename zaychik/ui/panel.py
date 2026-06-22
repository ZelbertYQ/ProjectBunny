from __future__ import annotations

import os
import re

import bpy
from bpy.types import Panel, UIList

from ..common.properties import (
    ZAYCHIK_PG_frameanalysis_item,
    ZAYCHIK_PG_trace_command_list_item,
    ZAYCHIK_PG_trace_draw_item,
    ZAYCHIK_PG_trace_resource_item,
)
from .operators import (
    FrameAnalysisUI,
    ZAYCHIK_OT_import_dx12_dump,
    ZAYCHIK_OT_import_selected_trace_draw,
    ZAYCHIK_OT_open_trace_metadata,
    ZAYCHIK_OT_open_trace_resource,
    ZAYCHIK_OT_refresh_frameanalysis_list,
    ZAYCHIK_OT_reveal_trace_resource,
    ZAYCHIK_OT_scan_trace_browser,
)


def _compact_func(value: str) -> str:
    if value == "DrawIndexedInstanced":
        return "DrawIdxInst"
    if value == "DrawInstanced":
        return "DrawInst"
    if value == "DrawIndexed":
        return "DrawIdx"
    if value == "Dispatch":
        return "Dispatch"
    return value.replace("Instanced", "Inst")


def _compact_kind(value: str) -> str:
    return {
        "Index Buffer": "IB",
        "Vertex Buffer": "VB",
        "Constant Buffer": "CBV",
        "Texture/SRV": "SRV",
        "Resource": "Res",
    }.get(value, value)


def _compact_resource_bind(item: ZAYCHIK_PG_trace_resource_item) -> str:
    slot = item.slot or ""
    register = item.register or ""
    kind = _compact_kind(item.kind)

    if slot == "ib" or kind == "IB":
        return "IB"
    if slot.startswith("vb"):
        return slot.upper()

    root_match = re.search(r"root\s*(\d+)", register)
    if kind == "CBV":
        if root_match:
            return f"CBV{root_match.group(1)}"
        cb_match = re.search(r"cbv:root\s*(\d+)", slot, re.IGNORECASE)
        if cb_match:
            return f"CBV{cb_match.group(1)}"
        return "CBV"
    if root_match:
        return f"{kind}{root_match.group(1)}"
    if register:
        return f"{kind} {_compact_text(register, 8)}"
    return kind if kind else _compact_text(slot, 10)


def _compact_format(value: str) -> str:
    if not value:
        return "-"
    text = value.removeprefix("DXGI_FORMAT_")
    return text[:14] if len(text) > 14 else text


def _compact_text(value: str, limit: int) -> str:
    if not value:
        return "-"
    if len(value) <= limit:
        return value
    if limit <= 1:
        return value[:limit]
    return f"{value[:limit - 1]}..."


class ZAYCHIK_UL_frameanalysis_list(UIList):
    bl_idname = "ZAYCHIK_UL_frameanalysis_list"

    def draw_item(
        self,
        context: bpy.types.Context,
        layout: bpy.types.UILayout,
        data: bpy.types.ID,
        item: ZAYCHIK_PG_frameanalysis_item,
        icon: int,
        active_data: bpy.types.ID,
        active_propname: str, 
        index: int,
        flt_flag: int,
    ) -> None:
        del context, data, icon, active_data, active_propname, index, flt_flag
        layout.label(text=item.name, icon="FILE_FOLDER")


class ZAYCHIK_UL_trace_draw_list(UIList):
    bl_idname = "ZAYCHIK_UL_trace_draw_list"

    def draw_item(
        self,
        context: bpy.types.Context,
        layout: bpy.types.UILayout,
        data: bpy.types.ID,
        item: ZAYCHIK_PG_trace_draw_item,
        icon: int,
        active_data: bpy.types.ID,
        active_propname: str,
        index: int,
        flt_flag: int,
    ) -> None:
        del context, data, icon, active_data, active_propname, index, flt_flag
        row = layout.row(align=True)
        row.label(text=item.name, icon="RESTRICT_SELECT_OFF")
        split = row.split(factor=0.18, align=True)
        split.label(text=_compact_func(item.func))
        split = split.split(factor=0.10, align=True)
        split.label(text=str(item.pso))
        split = split.split(factor=0.14, align=True)
        split.label(text=item.shader[:8] if item.shader and item.shader != "-" else "-")
        split = split.split(factor=0.13, align=True)
        if item.func == "Dispatch":
            split.label(text=f"{item.groups_x},{item.groups_y},{item.groups_z}")
        elif item.index_count:
            split.label(text=str(item.index_count))
        else:
            split.label(text=str(item.vertex_count))
        split = split.split(factor=0.12, align=True)
        split.label(text=str(item.start_index if item.index_count else item.start_vertex))
        split = split.split(factor=0.11, align=True)
        split.label(text=str(item.base_vertex))
        split = split.split(factor=0.12, align=True)
        split.label(text=str(item.instance_count))
        split = split.split(factor=0.14, align=True)
        split.label(text=str(item.ib_fmt) if item.ib_fmt else "-")
        split = split.split(factor=0.22, align=True)
        split.label(text=str(item.ib_bytes) if item.ib_bytes else "-")
        split.label(text=str(item.resource_count))


class ZAYCHIK_UL_trace_command_list(UIList):
    bl_idname = "ZAYCHIK_UL_trace_command_list"

    def draw_item(
        self,
        context: bpy.types.Context,
        layout: bpy.types.UILayout,
        data: bpy.types.ID,
        item: ZAYCHIK_PG_trace_command_list_item,
        icon: int,
        active_data: bpy.types.ID,
        active_propname: str,
        index: int,
        flt_flag: int,
    ) -> None:
        del context, data, icon, active_data, active_propname, index, flt_flag
        layout.label(text=item.label or item.cmdlist, icon="OUTLINER_OB_EMPTY")


class ZAYCHIK_UL_trace_resource_list(UIList):
    bl_idname = "ZAYCHIK_UL_trace_resource_list"

    def draw_item(
        self,
        context: bpy.types.Context,
        layout: bpy.types.UILayout,
        data: bpy.types.ID,
        item: ZAYCHIK_PG_trace_resource_item,
        icon: int,
        active_data: bpy.types.ID,
        active_propname: str,
        index: int,
        flt_flag: int,
    ) -> None:
        del context, data, icon, active_data, active_propname, index, flt_flag
        row = layout.row(align=True)
        row.label(text=_compact_resource_bind(item), icon="OUTLINER_DATA_MESH")
        split = row.split(factor=0.17, align=True)
        split.label(text=item.hash[:8] if item.hash else "-")
        split = split.split(factor=0.14, align=True)
        split.label(text=str(item.bytes))
        split = split.split(factor=0.18, align=True)
        split.label(text=str(item.offset))
        split = split.split(factor=0.11, align=True)
        split.label(text=str(item.stride))
        split = split.split(factor=0.32, align=True)
        split.label(text=_compact_format(item.fmt_name))
        split.label(text=_compact_text(item.skin_source, 10))


class ZAYCHIK_PT_sidebar(Panel):
    bl_label = "Zaychik"
    bl_idname = "ZAYCHIK_PT_sidebar"
    bl_space_type = "VIEW_3D"
    bl_region_type = "UI"
    bl_category = "Zaychik"

    def draw(self, context: bpy.types.Context) -> None:
        layout = self.layout
        settings = context.scene.zaychik_settings

        layout.prop(settings, "dump_root_directory")
        layout.operator(ZAYCHIK_OT_refresh_frameanalysis_list.bl_idname, icon="FILE_REFRESH")
        layout.template_list(
            ZAYCHIK_UL_frameanalysis_list.bl_idname,
            "",
            settings,
            "frameanalysis_items",
            settings,
            "frameanalysis_index",
            rows=8,
        )
        layout.prop(settings, "max_imports")
        layout.prop(settings, "import_all_matching")
        layout.prop(settings, "apply_world_matrices")
        if settings.apply_world_matrices:
            layout.prop(settings, "world_matrix_scale")
        layout.prop(settings, "skin_source_filter")
        layout.prop(settings, "vertex_layout_preset")
        layout.operator(ZAYCHIK_OT_import_dx12_dump.bl_idname, icon="IMPORT")

        box = layout.box()
        box.label(text="Status")
        box.label(text=settings.last_status)

        selected_path = FrameAnalysisUI.selected_path(context)
        if selected_path:
            box.label(text=os.path.basename(selected_path))


class FrameAnalysisPanelUI:
    @staticmethod
    def draw_table_header(layout: bpy.types.UILayout) -> None:
        row = layout.row(align=True)
        row.label(text="Call")
        split = row.split(factor=0.18, align=True)
        split.label(text="Func")
        split = split.split(factor=0.10, align=True)
        split.label(text="PSO")
        split = split.split(factor=0.14, align=True)
        split.label(text="Shader")
        split = split.split(factor=0.13, align=True)
        split.label(text="Count")
        split = split.split(factor=0.12, align=True)
        split.label(text="Start")
        split = split.split(factor=0.11, align=True)
        split.label(text="Base")
        split = split.split(factor=0.12, align=True)
        split.label(text="Inst")
        split = split.split(factor=0.14, align=True)
        split.label(text="IBFmt")
        split = split.split(factor=0.22, align=True)
        split.label(text="IBBytes")
        split.label(text="Res")

    @staticmethod
    def draw_resource_header(layout: bpy.types.UILayout) -> None:
        row = layout.row(align=True)
        row.label(text="Bind")
        split = row.split(factor=0.17, align=True)
        split.label(text="Hash")
        split = split.split(factor=0.14, align=True)
        split.label(text="Bytes")
        split = split.split(factor=0.18, align=True)
        split.label(text="Offset")
        split = split.split(factor=0.11, align=True)
        split.label(text="Stride")
        split = split.split(factor=0.32, align=True)
        split.label(text="Format")
        split.label(text="Source")

class ZAYCHIK_PT_frameanalysis(Panel):
    bl_label = "FrameAnalysis"
    bl_idname = "ZAYCHIK_PT_frameanalysis"
    bl_space_type = "VIEW_3D"
    bl_region_type = "UI"
    bl_category = "Zaychik"

    def draw(self, context: bpy.types.Context) -> None:
        layout = self.layout
        settings = context.scene.zaychik_settings

        layout.operator(ZAYCHIK_OT_scan_trace_browser.bl_idname, icon="FILE_REFRESH")

        layout.label(text="CommandLists")
        layout.template_list(
            ZAYCHIK_UL_trace_command_list.bl_idname,
            "",
            settings,
            "trace_command_list_items",
            settings,
            "trace_command_list_index",
            rows=4,
        )

        layout.label(text="Draw / Dispatch")
        FrameAnalysisPanelUI.draw_table_header(layout)
        layout.template_list(
            ZAYCHIK_UL_trace_draw_list.bl_idname,
            "",
            settings,
            "trace_draw_items",
            settings,
            "trace_draw_index",
            rows=5,
        )
        layout.operator(ZAYCHIK_OT_import_selected_trace_draw.bl_idname, icon="IMPORT")

        layout.label(text="Resources")
        FrameAnalysisPanelUI.draw_resource_header(layout)
        layout.template_list(
            ZAYCHIK_UL_trace_resource_list.bl_idname,
            "",
            settings,
            "trace_resource_items",
            settings,
            "trace_resource_index",
            rows=8,
        )

        resource = FrameAnalysisUI.selected_trace_resource(context)
        if resource:
            row = layout.row(align=True)
            row.operator(ZAYCHIK_OT_open_trace_resource.bl_idname, icon="FILE")
            row.operator(ZAYCHIK_OT_reveal_trace_resource.bl_idname, icon="FILE_FOLDER")
            row.operator(ZAYCHIK_OT_open_trace_metadata.bl_idname, icon="TEXT")


CLASSES = (
    ZAYCHIK_UL_frameanalysis_list,
    ZAYCHIK_UL_trace_command_list,
    ZAYCHIK_UL_trace_draw_list,
    ZAYCHIK_UL_trace_resource_list,
    ZAYCHIK_PT_sidebar,
    ZAYCHIK_PT_frameanalysis,
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
