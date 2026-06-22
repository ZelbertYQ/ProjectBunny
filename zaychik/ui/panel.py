from __future__ import annotations

import os

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
    ZAYCHIK_OT_open_trace_metadata,
    ZAYCHIK_OT_open_trace_resource,
    ZAYCHIK_OT_refresh_frameanalysis_list,
    ZAYCHIK_OT_reveal_trace_resource,
    ZAYCHIK_OT_scan_trace_browser,
)


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
        label = item.label or item.name
        layout.label(text=label, icon="RESTRICT_SELECT_OFF")


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
        row.label(text=item.slot, icon="OUTLINER_DATA_MESH")
        hash_text = item.hash[:8] if item.hash else "-"
        detail = item.register or item.kind
        row.label(text=f"{detail} {hash_text}")


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
        layout.template_list(
            ZAYCHIK_UL_trace_draw_list.bl_idname,
            "",
            settings,
            "trace_draw_items",
            settings,
            "trace_draw_index",
            rows=5,
        )

        layout.label(text="Resources")
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
            if resource.summary:
                summary_box = layout.box()
                summary_box.label(text="Metadata")
                for chunk in resource.summary.split("; ")[:4]:
                    summary_box.label(text=chunk[:96])

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
