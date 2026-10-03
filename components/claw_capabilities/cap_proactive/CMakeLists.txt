idf_component_register(
    SRCS "src/cap_proactive.c" "src/cap_proactive_caps.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_common
    PRIV_REQUIRES cap_scheduler claw_event_router claw_cap json freertos log
)
