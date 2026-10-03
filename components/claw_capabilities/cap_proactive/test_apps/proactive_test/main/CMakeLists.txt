idf_component_register(SRCS "test_cap_proactive.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES cap_proactive cap_scheduler claw_event_router claw_cap
                  fatfs wear_levelling nvs_flash unity
    WHOLE_ARCHIVE)
