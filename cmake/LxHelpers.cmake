# LoucuraXXXX GPU - shared CMake helpers.

# lx_add_test(<name> SOURCES ... LIBS ...)
# Registers a test executable with CTest.
function(lx_add_test name)
    cmake_parse_arguments(ARG "" "" "SOURCES;LIBS" ${ARGN})
    add_executable(${name} ${ARG_SOURCES})
    target_link_libraries(${name} PRIVATE ${ARG_LIBS} lx_test_support)
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES TIMEOUT 120)
endfunction()

# lx_add_benchmark(<name> SOURCES ... LIBS ...)
function(lx_add_benchmark name)
    cmake_parse_arguments(ARG "" "" "SOURCES;LIBS" ${ARGN})
    add_executable(${name} ${ARG_SOURCES})
    target_link_libraries(${name} PRIVATE ${ARG_LIBS})
endfunction()
