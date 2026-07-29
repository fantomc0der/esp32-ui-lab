// lv_conf.h — LVGL configuration for the host test build.
//
// This does not copy the board's configuration, it *includes* it and overrides
// the few settings the host needs differently. That direction is deliberate: the
// settings that decide what the binding layer can do — which widgets exist,
// which Montserrat sizes a script may select, the RGB565 colour depth — are then
// physically the same values the board builds with, and a host test cannot pass
// against a configuration the panel does not run. The board's copy stays the
// source of truth; docs/portability.md and CLAUDE.md both say it is, and a
// second copy here would quietly make that untrue.
//
// Four things change, each because the host is not the board:
//
//   LV_USE_STDLIB_MALLOC   LVGL's builtin allocator is a fixed-size pool it
//                          manages itself, so ASan sees one big malloc and none
//                          of the allocations inside it. Routing LVGL onto the
//                          C library's malloc is what makes a widget-level
//                          use-after-free — the stale-handle bug this repo has
//                          already hit once — visible to the sanitizer.
//
//   LV_MEM_SIZE            Left defined at the board's value purely so the two
//                          files do not appear to disagree. LVGL only reads it
//                          under LV_STDLIB_BUILTIN, so with CLIB above it bounds
//                          nothing and could equally be omitted.
//
//   LV_USE_ASSERT_OBJ      Off on the board, because it costs a check on every
//                          object access and the panel is trying to hold a frame
//                          rate. Free here, and a backstop for a binding added
//                          later that forgets to validate its handle.
//
//   LV_ASSERT_HANDLER      The board's handler spins in a `while(1);`, which is
//                          right for a panel you can power-cycle and wrong for
//                          a CI job, where it would hang until the runner times
//                          out with no diagnostic. The host aborts instead, so
//                          the assert prints and ASan's exit path still runs.
//
// The log level is deliberately NOT overridden: the board's LV_LOG_LEVEL_WARN
// is what a host run should see too, and LVGL's warnings reach stderr already.
#pragma once

// The board's configuration, verbatim: widgets, fonts, colour depth, asserts.
#include "../boards/waveshare-s3-touch-147/lv_conf.h"

// ---- host overrides ---------------------------------------------------------

// LV_STDLIB_CLIB puts every LVGL allocation through malloc/free, which is the
// whole reason this target exists. Undef first: the board's copy has already
// defined it, and redefining without undef is a warning that becomes an error
// under -Werror.
#undef LV_USE_STDLIB_MALLOC
#define LV_USE_STDLIB_MALLOC LV_STDLIB_CLIB

// Only read under LV_STDLIB_BUILTIN, so with CLIB above this bounds nothing. Kept
// at the board's value so the two files do not appear to disagree.
#undef LV_MEM_SIZE
#define LV_MEM_SIZE (48 * 1024U)

// The board leaves this off because it costs a check on every object access, which
// is not free on a panel trying to hold a frame rate. Here it is free.
//
// It does NOT add coverage for the stale-handle cases in test_ownership: those
// return a TypeError from jsvm_arg_widget()'s lv_obj_is_valid() check before LVGL
// is handed the object at all, so no LV_ASSERT_OBJ is reachable on that path.
//
// What it buys is a backstop for the binding added later that forgets to validate.
// Note what that backstop actually is, because it is not the assert: LV_ASSERT_OBJ
// checks lv_obj_has_class() before lv_obj_is_valid() (lv_obj.h:481-486), and that
// dereferences the pointer — so on a freed handle ASan reports the use-after-free
// read from inside LVGL, and the assert never gets to run. The value here is
// therefore a clear report at the first LVGL entry point rather than wherever the
// freed memory is next touched; on a type mismatch that is still live memory, the
// assert itself fires and names the problem.
#undef LV_USE_ASSERT_OBJ
#define LV_USE_ASSERT_OBJ 1

// LVGL's own asserts stay on (the board enables ASSERT_NULL and ASSERT_MALLOC),
// and on the host an assert should end the process with a diagnostic rather than
// spin, so tests fail loudly instead of hanging a CI job.
#undef LV_ASSERT_HANDLER
#define LV_ASSERT_HANDLER host_lv_assert_fail(__FILE__, __LINE__);

#ifndef __ASSEMBLY__
#ifdef __cplusplus
extern "C" {
#endif
// Defined in host_harness.cpp. Prints and aborts, so ASan's exit path runs.
void host_lv_assert_fail(const char *file, int line);
#ifdef __cplusplus
}
#endif
#endif
