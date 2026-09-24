cmake_minimum_required(VERSION 3.24)
project(cppgnss VERSION 0.1.0 LANGUAGES CXX)

if(PROJECT_IS_TOP_LEVEL)
    include(CTest)
endif()
option(CPPGNSS_BUILD_EXAMPLES "Build the UBX/SBF logger application" ON)
if(NOT DEFINED Python3_EXECUTABLE AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../.venv/bin/python")
    set(Python3_EXECUTABLE "${CMAKE_CURRENT_SOURCE_DIR}/../.venv/bin/python")
endif()
find_package(Python3 3.11 REQUIRED COMPONENTS Interpreter)
set(CPPGNSS_PYUBX2_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../contrib/pyubx2")
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../contrib/pysbf2/src/pysbf2/sbftypes_blocks.py")
    message(FATAL_ERROR "Initialize contrib/pysbf2: git submodule update --init contrib/pysbf2")
endif()
if(NOT EXISTS "${CPPGNSS_PYUBX2_DIR}/src/pyubx2/ubxtypes_get.py")
    message(FATAL_ERROR "Initialize contrib/pyubx2: git submodule update --init contrib/pyubx2")
endif()
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env
        PYTHONDONTWRITEBYTECODE=1
        "${Python3_EXECUTABLE}" -c "import sys; sys.path.insert(0, '${CPPGNSS_PYUBX2_DIR}/src'); import click, pyubx2"
    RESULT_VARIABLE codegen_ready ERROR_VARIABLE codegen_error)
if(NOT codegen_ready EQUAL 0)
    message(FATAL_ERROR "Install libcppgnss/requirements-codegen.txt into the selected Python environment.\n${codegen_error}")
endif()

set(generated_include "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(generated_dir "${generated_include}/cppgnss")
foreach(protocol_repo pyrtcm pynmeagps)
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../contrib/${protocol_repo}/src/${protocol_repo}")
        message(FATAL_ERROR "Initialize contrib/${protocol_repo}: git submodule update --init contrib/${protocol_repo}")
    endif()
endforeach()
set(text_bit_generated "${generated_dir}/rtcm3_gen.cpp" "${generated_dir}/nmea_gen.cpp"
    "${generated_dir}/rtcm3_gen.hpp" "${generated_dir}/nmea_gen.hpp")
file(GLOB text_bit_schemas CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/../contrib/pyrtcm/src/pyrtcm/*.py"
    "${CMAKE_CURRENT_SOURCE_DIR}/../contrib/pynmeagps/src/pynmeagps/*.py")
add_custom_command(OUTPUT ${text_bit_generated}
    COMMAND "${CMAKE_COMMAND}" -E env PYTHONDONTWRITEBYTECODE=1
        "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/scripts/generate_text_bit_parsers.py" --output-dir "${generated_dir}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/scripts/generate_text_bit_parsers.py" ${text_bit_schemas}
    COMMENT "Generating pinned RTCM3 and NMEA typed parsers" VERBATIM)
set(generated_sources "${generated_dir}/ubx_dump_gen.cpp" "${generated_dir}/ubx_names_gen.cpp")
set(sbf_generated "${generated_dir}/sbf_dispatch_gen.cpp")
set(sbf_generated_headers "${generated_dir}/sbf_ids_gen.hpp")
foreach(group measurement navigation_page gps_navigation glonass_navigation
        galileo_navigation beidou_navigation navic_navigation qzss_navigation
        sbas_navigation pvt attitude receiver_time external_event
        differential_correction lband status miscellaneous)
    list(APPEND sbf_generated "${generated_dir}/sbf_${group}_gen.cpp")
    list(APPEND sbf_generated_headers "${generated_dir}/sbf_${group}_gen.hpp")
endforeach()
file(GLOB sbf_schema_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/../contrib/pysbf2/src/pysbf2/sbftypes_*.py")
add_custom_command(OUTPUT ${sbf_generated} ${sbf_generated_headers}
    COMMAND "${CMAKE_COMMAND}" -E env PYTHONDONTWRITEBYTECODE=1
        "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/scripts/generate_sbf_parsers.py" --output-dir "${generated_dir}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/scripts/generate_sbf_parsers.py" ${sbf_schema_sources}
    COMMENT "Generating complete pinned pysbf2 SBF schemas" VERBATIM)
set(generated_headers "${generated_dir}/ubx_ids_gen.hpp"
    "${generated_dir}/ubx_struct_gen.hpp")
foreach(cls nav rxm mon tim esf hnr log sec cfg ack)
    list(APPEND generated_sources "${generated_dir}/ubx_${cls}_gen.cpp")
    list(APPEND generated_headers "${generated_dir}/ubx_${cls}_gen.hpp")
endforeach()
file(GLOB_RECURSE schema_sources CONFIGURE_DEPENDS "${CPPGNSS_PYUBX2_DIR}/src/pyubx2/*.py")
add_custom_command(
    OUTPUT ${generated_sources} ${generated_headers}
    COMMAND "${CMAKE_COMMAND}" -E env PYTHONDONTWRITEBYTECODE=1
        "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/scripts/generate_ubx_parsers.py"
        --output-dir "${generated_dir}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/scripts/generate_ubx_parsers.py" ${schema_sources}
    COMMENT "Generating UBX parsers from the pinned pyubx2 schema"
    VERBATIM)
add_custom_target(cppgnss_codegen DEPENDS ${generated_sources} ${generated_headers} ${sbf_generated} ${sbf_generated_headers} ${text_bit_generated})
add_library(cppgnss src/parse.cpp src/ubx_nav.cpp src/stream.cpp
    src/ubx_subframe.cpp ${sbf_generated} ${text_bit_generated}
    ${generated_sources} ${generated_headers})
add_library(cppgnss::cppgnss ALIAS cppgnss)
add_dependencies(cppgnss cppgnss_codegen)
target_compile_features(cppgnss PUBLIC cxx_std_20)
set_target_properties(cppgnss PROPERTIES CXX_EXTENSIONS OFF POSITION_INDEPENDENT_CODE ON)
target_include_directories(cppgnss PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<BUILD_INTERFACE:${generated_include}>")
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(cppgnss PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(CPPGNSS_BUILD_EXAMPLES)
    if(NOT UNIX)
        message(FATAL_ERROR "neognsslogger requires POSIX; use -DCPPGNSS_BUILD_EXAMPLES=OFF")
    endif()
    add_executable(neognsslogger examples/gnsslogger.cpp)
    find_package(Threads REQUIRED)
    target_link_libraries(neognsslogger PRIVATE cppgnss::cppgnss Threads::Threads)
endif()

if(BUILD_TESTING)
    add_executable(cppgnss_test tests/test_ubx.cpp)
    target_link_libraries(cppgnss_test PRIVATE cppgnss::cppgnss)
    # Assertions perform checks (and some I/O) in these inherited tests.
    foreach(test_target cppgnss_test)
        if(MSVC)
            target_compile_options(${test_target} PRIVATE /UNDEBUG)
        else()
            target_compile_options(${test_target} PRIVATE -UNDEBUG)
        endif()
    endforeach()
    add_test(NAME cppgnss.decode COMMAND cppgnss_test)
    add_test(NAME cppgnss.codegen COMMAND "${CMAKE_COMMAND}" -E env
        PYTHONDONTWRITEBYTECODE=1
        "CPPGNSS_CXX=${CMAKE_CXX_COMPILER}"
        "CPPGNSS_CXX_FLAGS=${CMAKE_CXX_FLAGS}"
        "CPPGNSS_LINK_FLAGS=${CMAKE_EXE_LINKER_FLAGS}"
        "CPPGNSS_GENERATED_INCLUDE=${generated_include}"
        "CPPGNSS_LIBRARY=$<TARGET_FILE:cppgnss>"
        "${Python3_EXECUTABLE}" -m unittest discover -s "${CMAKE_CURRENT_SOURCE_DIR}/tests" -p test_codegen.py)
endif()
