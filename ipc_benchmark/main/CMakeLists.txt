if(BUILD_SCALING_BENCHMARK)
    set(APP_SRCS "wamr_scaling_benchmark.c")
else()
    set(APP_SRCS "ipc_benchmark.c")
endif()

idf_component_register(SRCS ${APP_SRCS}
                    INCLUDE_DIRS ".")
