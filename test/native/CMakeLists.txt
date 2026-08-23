# Desktop-only test suite: see docs/testing.md. Explicit list rather than a
# glob, same rationale as TinyMPG/TinyH264's own test/native/CMakeLists.txt:
# a glob silently misses newly-added test files without a manual
# re-configure.
set(TINYJPEG_TESTS
    test_decode_gradient
    test_decode_checker420
    test_decode_grayscale
    test_get_size
    test_scale
    test_error_handling
    test_file_source
    test_callback_source
    test_multiple_instances
)

foreach(test_name ${TINYJPEG_TESTS})
    add_executable(${test_name} ${test_name}.cpp)
    target_link_libraries(${test_name} PRIVATE TinyJPEG)
    add_test(NAME ${test_name} COMMAND ${test_name})
    # Every test reads its assets via relative paths (e.g. "assets/foo.jpg"),
    # so it must run with test/native/ as the working directory regardless
    # of where the build tree actually lives.
    set_tests_properties(${test_name} PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
endforeach()

# Native x86 timing, for docs/decoding.md-adjacent performance reporting -
# not a correctness test, so not added to ctest above; build/run manually
# (see bench_native.cpp's own file comment).
add_executable(bench_native EXCLUDE_FROM_ALL bench_native.cpp)
target_link_libraries(bench_native PRIVATE TinyJPEG)
target_compile_options(bench_native PRIVATE -O2)

