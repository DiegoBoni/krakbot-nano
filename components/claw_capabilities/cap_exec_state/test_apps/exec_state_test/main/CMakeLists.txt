idf_component_register(SRCS "test_cap_exec_state.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES cap_exec_state claw_core fatfs wear_levelling unity
                       WHOLE_ARCHIVE)
