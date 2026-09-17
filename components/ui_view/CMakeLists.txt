# Header-only view-model shared between the on-device UI (future) and the
# host LVGL emulator (tools/lvgl_sim/, which includes this directory directly
# via a plain include path -- it does not go through idf_component_register).
idf_component_register(
    INCLUDE_DIRS "include"
)
