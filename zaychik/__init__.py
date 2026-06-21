from __future__ import annotations

import importlib
import sys
import types

import bpy
from bpy.props import PointerProperty

# When the addon is reloaded (Blender "Reload Scripts" / VSCode addon update),
# addon_utils.enable() only re-executes zaychik/__init__.py; submodules in
# zaychik.* stay cached in sys.modules and retain old class references / stale
# pyc. This leads to "unregister" running against a previous version of the
# module (e.g. AttributeError on del bpy.types.Scene.zaychik_settings), and to
# register_class warnings about "already registered". Force a clean reload of
# every zaychik.* submodule so that register/unregister below reflect the
# current source on disk.
_SUBMODULE_NAMES: tuple[str, ...] = (
    "zaychik.common.config",
    "zaychik.common.properties",
    "zaychik.utils.paths",
    "zaychik.utils.parser",
    "zaychik.utils.importer",
    "zaychik.ui.operators",
    "zaychik.ui.panel",
)


def _reload_submodules() -> None:
    for name in _SUBMODULE_NAMES:
        module = sys.modules.get(name)
        if isinstance(module, types.ModuleType):
            importlib.reload(module)


_reload_submodules()

from .common.config import load_config  # noqa: E402
from .common.properties import CLASSES as PROPERTY_CLASSES  # noqa: E402
from .common.properties import ZAYCHIK_PG_settings  # noqa: E402
from .ui.operators import CLASSES as OPERATOR_CLASSES  # noqa: E402
from .ui.operators import refresh_frameanalysis_items  # noqa: E402
from .ui.panel import CLASSES as PANEL_CLASSES  # noqa: E402


bl_info = {
    "name": "Zaychik DX12 Dump Importer",
    "author": "OpenAI",
    "description": "Analyze a DX12 frame dump log.txt and try importing meshes into Blender",
    "blender": (4, 2, 0),
    "version": (0, 1, 0),
    "location": "View3D > Sidebar > Zaychik",
    "warning": "Early prototype importer for DX12 frame dumps",
    "category": "Import-Export",
}


CLASSES = (
    *PROPERTY_CLASSES,
    *OPERATOR_CLASSES,
    *PANEL_CLASSES,
)


def _restore_config_and_refresh() -> None:
    """One-shot delayed init: restore saved config and refresh the FA list.

    Runs via bpy.app.timers a short moment after register() returns, so all
    classes are registered, the PointerProperty is attached to Scene, and we
    are no longer inside the addon-enable context where writing to ID data
    can raise "Writing to ID classes in this context is not allowed".
    """ 
    try:
        config = load_config()
        for scene in bpy.data.scenes:
            settings = scene.zaychik_settings
            settings.dump_root_directory = config.get("dump_root_directory", "")
            settings.selected_frameanalysis_name = config.get("selected_frameanalysis_name", "")

        # If there's a usable context, populate the FA list for the active scene.
        context = bpy.context
        if context and context.scene and context.scene.zaychik_settings.dump_root_directory:
            refresh_frameanalysis_items(context)
    except Exception:
        # Initialization failures must never break addon loading.
        pass
    return None  # don't reschedule


def register() -> None:
    for cls in CLASSES:
        bpy.utils.register_class(cls)
    bpy.types.Scene.zaychik_settings = PointerProperty(type=ZAYCHIK_PG_settings)

    # Schedule the first restore/refresh to run once the addon is fully loaded.
    bpy.app.timers.register(_restore_config_and_refresh, first_interval=0.1)


def unregister() -> None:
    # Defensive cleanup: the timer might still be pending; if register()
    # failed partway through, some classes / the PointerProperty may not
    # have been attached.
    try:
        bpy.app.timers.unregister(_restore_config_and_refresh)
    except (ValueError, KeyError, RuntimeError):
        pass

    if hasattr(bpy.types.Scene, "zaychik_settings"):
        del bpy.types.Scene.zaychik_settings

    for cls in reversed(CLASSES):
        try:
            bpy.utils.unregister_class(cls)
        except (RuntimeError, ValueError):
            # Class was not registered (e.g. register() failed mid-way).
            pass
