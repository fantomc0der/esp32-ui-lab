# CMakeLists.txt — the host build of the binding layer, under sanitizers.
#
# What this compiles: LVGL 9.5.0 (fetched, pinned to the version the board is
# verified against), the vendored quickjs-ng, and firmware/lvgl-js-bindings/src,
# against the Arduino stubs in stubs/. What it produces: one test binary per
# suite under tests/, all built with -fsanitize=address,undefined.
#
# Scope: JSVM_WITH_WIFI=0. The wifi module needs WiFi.h, HTTPClient.h and
# NetworkClientSecure.h, which is the largest stub surface in the layer and the
# least testable without a network; issue #16 scopes it out of the first pass.
# sys and fs are compiled in, because their stubs are small and real.

cmake_minimum_required(VERSION 3.22)
project(lvgl_js_bindings_host_test CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  # Debug, specifically NOT RelWithDebInfo: that one is `-O2 -g -DNDEBUG`, and
  # the NDEBUG compiles out all 139 assert()s in quickjs.c — including
  # `assert(p->ref_count > 0)` on the GC's decref path, which is the cheapest
  # detector available for an over-released JSValue. ASan only reports that once
  # the object is recycled into another allocation; the assert fires at the
  # moment the refcount goes wrong. Since catching exactly that class of bug is
  # the point of this target, giving it up for -O2 would be a poor trade.
  #
  # -O1 rather than Debug's -O0: it keeps stack traces readable and the
  # interpreter fast enough that the suites stay well under a second.
  set(CMAKE_BUILD_TYPE Debug)
endif()
set(CMAKE_CXX_FLAGS_DEBUG "-O1 -g")
set(CMAKE_C_FLAGS_DEBUG "-O1 -g")

# Strip -DNDEBUG out of every configuration, not just the default one. Choosing
# Debug above is not enough on its own: someone configuring with an explicit
# -DCMAKE_BUILD_TYPE=RelWithDebInfo would silently get NDEBUG back and lose the
# QuickJS asserts this target is built to keep. Removing it from the flags for all
# configurations means no build of this project can switch them off by accident.
foreach(cfg DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
  foreach(lang CXX C)
    string(REPLACE "-DNDEBUG" "" CMAKE_${lang}_FLAGS_${cfg} "${CMAKE_${lang}_FLAGS_${cfg}}")
  endforeach()
endforeach()

set(FIRMWARE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(BINDINGS_DIR ${FIRMWARE_DIR}/lvgl-js-bindings/src)
set(QUICKJS_DIR ${FIRMWARE_DIR}/quickjs-ng/src)

# ---- sanitizers -------------------------------------------------------------
# The reason this target exists. Applied to every target below, including LVGL
# and QuickJS, so an overflow anywhere in the chain is caught rather than only
# one in our own code.
set(SAN_FLAGS -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all)
add_compile_options(${SAN_FLAGS})
add_link_options(${SAN_FLAGS})

# ---- LVGL -------------------------------------------------------------------
# Fetched rather than vendored, pinned to the same 9.5.0 the firmware CI job
# installs through arduino-cli. A different version here would make a green host
# run say nothing about the board.
include(FetchContent)
set(LV_CONF_PATH ${CMAKE_CURRENT_SOURCE_DIR}/lv_conf.h CACHE FILEPATH "" FORCE)
set(LV_CONF_BUILD_DISABLE_EXAMPLES 1)
set(LV_CONF_BUILD_DISABLE_DEMOS 1)
FetchContent_Declare(
  lvgl
  GIT_REPOSITORY https://github.com/lvgl/lvgl.git
  GIT_TAG v9.5.0
  GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(lvgl)

# ---- quickjs-ng -------------------------------------------------------------
# The vendored copy, unmodified, so the host runs the same engine as the board
# (including the five Xtensa int32_t fixes, which are correct on x86-64 too).
# The four .c files the vendored copy ships; cutils is header-only here.
add_library(host_quickjs STATIC
  ${QUICKJS_DIR}/quickjs.c
  ${QUICKJS_DIR}/libregexp.c
  ${QUICKJS_DIR}/libunicode.c
  ${QUICKJS_DIR}/dtoa.c
)
target_include_directories(host_quickjs PUBLIC ${QUICKJS_DIR})
target_compile_definitions(host_quickjs PRIVATE _GNU_SOURCE)
# Third-party code we do not maintain: build it quietly so our own warnings are
# the ones visible, but keep it under the sanitizers.
target_compile_options(host_quickjs PRIVATE -w)

# ---- the binding layer + stubs ---------------------------------------------
# jsvm_app.cpp is deliberately absent. It is the supervisor — the boot chain, the
# corner button, the serial upload protocol — and it is written against Arduino
# String, Serial.available()/read() and File.print(), none of which the layer's
# own logic needs. Stubbing them is a bigger surface than everything here put
# together, and it is the surface most likely to rot. Issue #16 scopes the first
# pass to jsvm_core.cpp + bindings_lv.cpp for exactly this reason; the path
# resolution in jsvm_app.cpp it also lists is the natural next step, and
# docs/host-test.md records what adding it would cost.
add_library(host_bindings STATIC
  ${BINDINGS_DIR}/jsvm_core.cpp
  ${BINDINGS_DIR}/bindings_lv.cpp
  ${BINDINGS_DIR}/bindings_sys.cpp
  ${BINDINGS_DIR}/bindings_fs.cpp
  stubs/host_stubs.cpp
  stubs/fs_stub.cpp
  stubs/preferences_stub.cpp
  host_harness.cpp
)
# -Wall -Wextra, so "third-party built quietly, ours not" is actually true rather
# than everything being built with Ubuntu's bare defaults.
#
# -Wno-unused-parameter is the one exclusion, and it is not laziness: QuickJS's
# JSCFunction signature is fixed, so a binding that ignores an argument it is
# obliged to accept (js_timer_stop takes ctx and does not need it) is correct code.
# The alternative would be editing firmware to silence a host-only diagnostic,
# which is exactly the tail wagging the dog this target must avoid.
#
# Not -Werror either: the binding layer targets a 32-bit machine, so a host warning
# about width is information rather than grounds to fail a build.
target_compile_options(host_bindings PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_include_directories(host_bindings PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${BINDINGS_DIR}
)
target_link_libraries(host_bindings PUBLIC host_quickjs lvgl)
target_compile_definitions(host_bindings PUBLIC JSVM_WITH_WIFI=0 JSVM_WITH_SYS=1 JSVM_WITH_FS=1)

# ---- tests ------------------------------------------------------------------
enable_testing()

# One binary per suite: a crash under ASan takes the process down, so separate
# binaries mean one failing suite still lets the others report.
foreach(suite test_ownership test_props test_reload test_modules)
  add_executable(${suite} tests/${suite}.cpp)
  target_link_libraries(${suite} PRIVATE host_bindings)
  add_test(NAME ${suite} COMMAND ${suite})
  # halt_on_error keeps the first report as the failure rather than letting a
  # cascade bury it; detect_leaks is the point of the reload suite.
  set_tests_properties(${suite} PROPERTIES
    ENVIRONMENT "ASAN_OPTIONS=detect_leaks=1:halt_on_error=1:abort_on_error=0;UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1")
endforeach()
