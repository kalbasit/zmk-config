if(CONFIG_TOUCAN_DEBUG_INSTRUMENTATION)
  zephyr_library_include_directories(${CMAKE_SOURCE_DIR}/include)
  zephyr_library_sources(debug_instrumentation.c)
endif()
