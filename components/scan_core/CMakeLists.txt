# Pure core of the site scanner: no ESP-IDF headers (tests/host compiles it).
idf_component_register(SRCS "scan_core.c"
                       INCLUDE_DIRS "include")
