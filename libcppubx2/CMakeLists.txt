cmake_minimum_required(VERSION 3.24)
project(cppubx2 VERSION 0.1.0 LANGUAGES CXX)

if(PROJECT_IS_TOP_LEVEL)
    include(CTest)
endif()
option(CPPUBX2_BUILD_EXAMPLES "Build the POSIX neoubxlogger application" ON)
set(Python3_EXECUTABLE "${CMAKE_CURRENT_SOURCE_DIR}/../.venv/bin/python")
if(NOT EXISTS "${Python3_EXECUTABLE}")
    message(FATAL_ERROR "Create the repository environment first: uv venv .venv")
endif()
find_package(Python3 3.11 REQUIRED COMPONENTS Interpreter)
set(CPPUBX2_PYUBX2_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../contrib/pyubx2")
if(NOT EXISTS "${CPPUBX2_PYUBX2_DIR}/src/pyubx2/ubxtypes_get.py")
    message(FATAL_ERROR "Initialize contrib/pyubx2: git submodule update --init contrib/pyubx2")
endif()
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env
        "PYTHONPATH=${CPPUBX2_PYUBX2_DIR}/src" PYTHONDONTWRITEBYTECODE=1
        "${Python3_EXECUTABLE}" -c "import click, pyubx2"
    RESULT_VARIABLE codegen_ready ERROR_VARIABLE codegen_error)
if(NOT codegen_ready EQUAL 0)
    message(FATAL_ERROR "Install libcppubx2/requirements-codegen.txt into the selected Python environment.\n${codegen_error}")
endif()

set(generated_include "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(generated_dir "${generated_include}/cppubx2")
set(generated_sources "${generated_dir}/ubx_dump_gen.cpp" "${generated_dir}/ubx_names_gen.cpp")
set(generated_headers "${generated_dir}/ubx_ids_gen.hpp"
    "${generated_dir}/ubx_struct_gen.hpp" "${generated_dir}/ubx_dump_gen.hpp")
foreach(cls nav rxm mon tim esf hnr log sec cfg ack)
    list(APPEND generated_sources "${generated_dir}/ubx_${cls}_gen.cpp")
    list(APPEND generated_headers "${generated_dir}/ubx_${cls}_gen.hpp")
endforeach()
file(GLOB_RECURSE schema_sources CONFIGURE_DEPENDS "${CPPUBX2_PYUBX2_DIR}/src/pyubx2/*.py")
add_custom_command(
    OUTPUT ${generated_sources} ${generated_headers}
    COMMAND "${CMAKE_COMMAND}" -E env PYTHONDONTWRITEBYTECODE=1
        "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/scripts/generate_ubx_parsers.py"
        --output-dir "${generated_dir}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/scripts/generate_ubx_parsers.py" ${schema_sources}
    COMMENT "Generating UBX parsers from the pinned pyubx2 schema"
    VERBATIM)
add_custom_target(cppubx2_codegen DEPENDS ${generated_sources} ${generated_headers})
add_library(cppubx2 src/ubx.cpp src/ubx_names.cpp src/ubx_nav.cpp src/ubx_reader.cpp src/ubx_archive.cpp
    ${generated_sources} ${generated_headers})
add_library(cppubx2::cppubx2 ALIAS cppubx2)
add_dependencies(cppubx2 cppubx2_codegen)
target_compile_features(cppubx2 PUBLIC cxx_std_20)
set_target_properties(cppubx2 PROPERTIES CXX_EXTENSIONS OFF POSITION_INDEPENDENT_CODE ON)
target_include_directories(cppubx2 PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<BUILD_INTERFACE:${generated_include}>")
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(cppubx2 PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(CPPUBX2_BUILD_EXAMPLES)
    find_package(OpenSSL REQUIRED COMPONENTS Crypto)
    add_executable(cppubx2_archive_index examples/archive_index.cpp)
    target_link_libraries(cppubx2_archive_index PRIVATE cppubx2::cppubx2 OpenSSL::Crypto)
    if(NOT UNIX)
        message(FATAL_ERROR "neoubxlogger requires POSIX; use -DCPPUBX2_BUILD_EXAMPLES=OFF")
    endif()
    add_executable(neoubxlogger examples/ubxlogger.cpp)
    target_link_libraries(neoubxlogger PRIVATE cppubx2::cppubx2)
    add_executable(cppubx2_inspect_frame examples/inspect_frame.cpp)
    target_link_libraries(cppubx2_inspect_frame PRIVATE cppubx2::cppubx2)
endif()

if(BUILD_TESTING)
    add_executable(cppubx2_test tests/test_ubx.cpp)
    target_link_libraries(cppubx2_test PRIVATE cppubx2::cppubx2)
    add_executable(cppubx2_reader_test tests/test_reader.cpp)
    target_link_libraries(cppubx2_reader_test PRIVATE cppubx2::cppubx2)
    # Assertions perform checks (and some I/O) in these inherited tests.
    foreach(test_target cppubx2_test cppubx2_reader_test)
        if(MSVC)
            target_compile_options(${test_target} PRIVATE /UNDEBUG)
        else()
            target_compile_options(${test_target} PRIVATE -UNDEBUG)
        endif()
    endforeach()
    add_test(NAME cppubx2.decode COMMAND cppubx2_test)
    add_test(NAME cppubx2.reader COMMAND cppubx2_reader_test)
    add_test(NAME cppubx2.codegen COMMAND "${CMAKE_COMMAND}" -E env
        PYTHONDONTWRITEBYTECODE=1
        "CPPUBX2_CXX=${CMAKE_CXX_COMPILER}"
        "CPPUBX2_GENERATED_INCLUDE=${generated_include}"
        "CPPUBX2_LIBRARY=$<TARGET_FILE:cppubx2>"
        "${Python3_EXECUTABLE}" -m unittest discover -s "${CMAKE_CURRENT_SOURCE_DIR}/tests" -p test_codegen.py)
    if(CPPUBX2_BUILD_EXAMPLES)
        add_test(NAME cppubx2.archive COMMAND "${CMAKE_COMMAND}" -E env
            PYTHONDONTWRITEBYTECODE=1
            "PYTHONPATH=${CMAKE_CURRENT_SOURCE_DIR}/../python-src"
            "CPPUBX2_ARCHIVE_INDEX=$<TARGET_FILE:cppubx2_archive_index>"
            "${Python3_EXECUTABLE}" -m unittest discover
            -s "${CMAKE_CURRENT_SOURCE_DIR}/../tests" -p test_restitch.py)
        add_test(NAME cppubx2.example COMMAND cppubx2_inspect_frame)
        add_test(NAME cppubx2.reader_smoke COMMAND sh
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/smoke_reader.sh" "$<TARGET_FILE:neoubxlogger>")
        add_test(NAME cppubx2.logger COMMAND "${CMAKE_COMMAND}" -E env
            PYTHONDONTWRITEBYTECODE=1 "CPPUBX2_LOGGER=$<TARGET_FILE:neoubxlogger>"
            "${Python3_EXECUTABLE}" -m unittest discover -s "${CMAKE_CURRENT_SOURCE_DIR}/tests" -p test_logger.py)
        set_tests_properties(cppubx2.logger PROPERTIES TIMEOUT 45)
    endif()
endif()
