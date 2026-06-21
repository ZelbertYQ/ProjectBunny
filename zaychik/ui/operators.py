from __future__ import annotations

import os
from typing import List, Optional

import bpy
from bpy.types import Context, Operator

from ..utils.importer import DrawImporter
from ..utils.parser import LogParser
from ..utils.paths import Paths


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

        success_count = 0
        messages: List[str] = []
        limit = min(settings.max_imports, len(matching_draws))
        for draw in matching_draws[:limit]:
            try:
                ok, message = DrawImporter.import_draw_call(context, dump_dir, draw)
            except Exception as exc:  # pragma: no cover - Blender runtime path
                ok = False
                message = f"event {draw.event}: {exc}"

            if ok:
                success_count += 1
                messages.append(message)
                if not settings.import_all_matching:
                    break

        if success_count == 0:
            preview = messages[0] if messages else "No draw could be imported"
            self.report({"WARNING"}, preview)
            settings.last_status = "Import attempt finished with 0 success"
            return {"CANCELLED"}

        settings.last_status = f"Imported {success_count} mesh object(s)"
        self.report({"INFO"}, settings.last_status)
        return {"FINISHED"}


CLASSES = (
    ZAYCHIK_OT_refresh_frameanalysis_list,
    ZAYCHIK_OT_import_dx12_dump,
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
