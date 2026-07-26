// jsvm_internal.h — the surface the VM core shares with the binding modules.
//
// Private to this library; hosts include js_bindings.h instead. The split it
// enforces:
//
//   jsvm_core.cpp     owns QuickJS, JSValue lifetime, and teardown order. It
//                     knows nothing about which widgets or device APIs exist,
//                     beyond being the composition root that installs them.
//   bindings_*.cpp    own one domain each, and know only this header. They
//                     never touch another module's state.
//
// Everything a module needs in order to hold a JS callback safely lives here,
// because getting that wrong is how this library corrupts memory rather than
// failing cleanly (see docs/lang-js/architecture.md).
#pragma once

#include <lvgl.h>

#include "js_bindings.h"
#include "quickjs.h"

// ---------------------------------------------------------------- live context
// Valid only between jsvm_start() and jsvm_stop(); null otherwise.
extern JSContext *jsvm_ctx;

// ---------------------------------------------------------------- diagnostics
void jsvm_report_exception();

// Calls fn and reports (never propagates) an exception, so a broken script
// handler cannot unwind into C. BORROWS fn: JS_Call does not retain it, so the
// caller must hold a reference for the whole call. Never pass a value that the
// callback itself could free — dup it first. That is the rule the trampolines
// exist to honour.
void jsvm_call_reporting(JSValue fn, int argc, JSValueConst *argv);

// ---------------------------------------------------------------- widget handles
extern JSClassID jsvm_widget_class;

JSValue jsvm_wrap_widget(JSContext *ctx, lv_obj_t *obj);

// Unwraps and validates. Returns null with a JS exception pending if the value
// is not a widget, or if its widget has since been deleted.
lv_obj_t *jsvm_arg_widget(JSContext *ctx, JSValueConst v);

// ---------------------------------------------------------------- owned bindings
// Both of these hand a JS callback to the core, which dups it, dispatches it
// through a trampoline, and frees it at exactly one release point. A module
// never stores a JSValue itself.

// Registers fn for `code` on obj. `widget` is the wrapper handed back to the
// callback. Released by the widget's LV_EVENT_DELETE hook, or at teardown.
bool jsvm_bind_event(JSContext *ctx, lv_obj_t *obj, lv_event_code_t code,
                     JSValueConst fn, JSValueConst widget);

// Creates an LVGL timer driving fn. Returns a JS handle with .stop(), or an
// exception. Released by .stop(), or at teardown.
JSValue jsvm_create_timer(JSContext *ctx, int32_t ms, JSValueConst fn);

// ---------------------------------------------------------------- modules
// Each module installs its globals on a fresh context at every jsvm_start(),
// since a reload destroys the previous one. A module holding state across the
// call also provides a teardown, which the core runs first, before any widget
// is deleted.
void js_install_lv(JSContext *ctx);

#if JSVM_WITH_SYS
void js_install_sys(JSContext *ctx);
#endif

#if JSVM_WITH_WIFI
void js_install_wifi(JSContext *ctx);
void js_teardown_wifi();
#endif
