# Host-native unit tests for the picoCluster layer.
#
#   ./build tests
# or by hand:
#   cmake -B tests/build tests && make -C tests/build && ctest --test-dir tests/build

cmake_minimum_required(VERSION 3.13)
project(picocluster_tests C)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)   # for tests/.clangd

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

enable_testing()

set(CLUSTER_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../cluster)
set(PICOOS_TESTS ${CMAKE_CURRENT_SOURCE_DIR}/../picoOS/tests)   # framework.h

# Same warning set as the firmware build.
set(TEST_FLAGS
    -Wall -Wextra -Wshadow -Wundef -Wpointer-arith -Wwrite-strings
    -Wformat=2 -Wformat-truncation=2 -Wimplicit-fallthrough
    -Wnull-dereference -Wdouble-promotion -Wredundant-decls
    -Wstrict-prototypes -Wmissing-prototypes -Wold-style-definition
    -Wnested-externs -Werror
)

add_executable(test_proto
    test_proto.c
    ${CLUSTER_SRC}/proto.c
)
target_include_directories(test_proto PRIVATE ${CLUSTER_SRC} ${PICOOS_TESTS})
target_compile_options(test_proto PRIVATE ${TEST_FLAGS})
add_test(NAME proto COMMAND test_proto)
