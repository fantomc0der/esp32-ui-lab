// bindings_fs.cpp — the `fs` global: file access for scripts.
//
// Operates on whatever filesystems the host registers with
// jsvm_set_filesystem(), so this module stays board-agnostic: it never names
// SD_MMC or FFat, only the fs::FS interface both implement.
//
// Paths are SD-card-relative by default. A "flash:" prefix targets the second
// filesystem instead, e.g. fs.read("flash:/app.js"). That is deliberately
// explicit rather than falling back between the two, so a script's writes
// always land somewhere predictable.
//
// Reads block the UI task, which is fine for the config-and-log sized files
// this is meant for and bad for megabytes; hence kMaxRead.

#include "jsvm_internal.h"

#if JSVM_WITH_FS

static fs::FS *g_sd = nullptr;
static fs::FS *g_flash = nullptr;

// A script can exhaust PSRAM by reading a huge file; cap it well below the
// JS heap so a bad read fails cleanly instead of taking the VM down.
static const size_t kMaxRead = 256 * 1024;

void jsvm_set_filesystem(fs::FS *sd, fs::FS *flash) {
  g_sd = sd;
  g_flash = flash;
}

// Resolves "flash:/x" to the flash FS and everything else to the card.
// Returns null when the requested filesystem is not mounted.
static fs::FS *resolve(const char *path, const char **out_path) {
  static const char kFlashPrefix[] = "flash:";
  if (strncmp(path, kFlashPrefix, sizeof(kFlashPrefix) - 1) == 0) {
    *out_path = path + sizeof(kFlashPrefix) - 1;
    return g_flash;
  }
  *out_path = path;
  // Unprefixed paths prefer the card, but fall back to flash so a script
  // written against "/apps/x.js" works on a board with no card fitted.
  return g_sd ? g_sd : g_flash;
}

// Every entry point needs the same unwrap-and-resolve, so it lives here.
// Leaves a JS exception pending and returns false when it fails.
static bool arg_path(JSContext *ctx, JSValueConst v, fs::FS **out_fs,
                     const char **out_path, const char **out_cstr) {
  const char *raw = JS_ToCString(ctx, v);
  if (!raw) return false;
  fs::FS *fs_ptr = resolve(raw, out_path);
  if (!fs_ptr) {
    JS_FreeCString(ctx, raw);
    JS_ThrowTypeError(ctx, "no filesystem mounted for that path");
    return false;
  }
  if (**out_path != '/') {
    JS_FreeCString(ctx, raw);
    JS_ThrowTypeError(ctx, "path must be absolute, e.g. \"/apps/clock.js\"");
    return false;
  }
  *out_fs = fs_ptr;
  *out_cstr = raw;  // caller frees
  return true;
}

static JSValue js_fs_read(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
  if (argc < 1) return JS_ThrowTypeError(ctx, "read(path) needs a path");
  fs::FS *fs_ptr; const char *path; const char *raw;
  if (!arg_path(ctx, argv[0], &fs_ptr, &path, &raw)) return JS_EXCEPTION;

  File f = fs_ptr->open(path, FILE_READ);
  if (!f || f.isDirectory()) {
    if (f) f.close();
    JS_FreeCString(ctx, raw);
    return JS_NULL;
  }
  const size_t n = f.size();
  if (n > kMaxRead) {
    f.close();
    JS_FreeCString(ctx, raw);
    return JS_ThrowRangeError(ctx, "file larger than the %u byte read limit",
                              (unsigned)kMaxRead);
  }
  char *buf = static_cast<char *>(heap_caps_malloc(n + 1, MALLOC_CAP_SPIRAM));
  if (!buf) {
    f.close();
    JS_FreeCString(ctx, raw);
    return JS_ThrowOutOfMemory(ctx);
  }
  const size_t got = f.read(reinterpret_cast<uint8_t *>(buf), n);
  f.close();
  JS_FreeCString(ctx, raw);
  JSValue s = JS_NewStringLen(ctx, buf, got);
  heap_caps_free(buf);
  return s;
}

// Shared by write() and append(); mode is the only difference.
static JSValue fs_write_common(JSContext *ctx, int argc, JSValueConst *argv, const char *mode) {
  if (argc < 2) return JS_ThrowTypeError(ctx, "needs (path, text)");
  fs::FS *fs_ptr; const char *path; const char *raw;
  if (!arg_path(ctx, argv[0], &fs_ptr, &path, &raw)) return JS_EXCEPTION;

  size_t len = 0;
  const char *data = JS_ToCStringLen(ctx, &len, argv[1]);
  if (!data) { JS_FreeCString(ctx, raw); return JS_EXCEPTION; }

  File f = fs_ptr->open(path, mode);
  bool ok = false;
  if (f) {
    ok = f.write(reinterpret_cast<const uint8_t *>(data), len) == len;
    f.close();
  }
  JS_FreeCString(ctx, data);
  JS_FreeCString(ctx, raw);
  return JS_NewBool(ctx, ok);
}

static JSValue js_fs_write(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
  return fs_write_common(ctx, argc, argv, FILE_WRITE);
}

static JSValue js_fs_append(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
  return fs_write_common(ctx, argc, argv, FILE_APPEND);
}

static JSValue js_fs_exists(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
  if (argc < 1) return JS_ThrowTypeError(ctx, "exists(path) needs a path");
  fs::FS *fs_ptr; const char *path; const char *raw;
  if (!arg_path(ctx, argv[0], &fs_ptr, &path, &raw)) return JS_EXCEPTION;
  const bool ok = fs_ptr->exists(path);
  JS_FreeCString(ctx, raw);
  return JS_NewBool(ctx, ok);
}

static JSValue js_fs_remove(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
  if (argc < 1) return JS_ThrowTypeError(ctx, "remove(path) needs a path");
  fs::FS *fs_ptr; const char *path; const char *raw;
  if (!arg_path(ctx, argv[0], &fs_ptr, &path, &raw)) return JS_EXCEPTION;
  const bool ok = fs_ptr->remove(path);
  JS_FreeCString(ctx, raw);
  return JS_NewBool(ctx, ok);
}

static JSValue js_fs_mkdir(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
  if (argc < 1) return JS_ThrowTypeError(ctx, "mkdir(path) needs a path");
  fs::FS *fs_ptr; const char *path; const char *raw;
  if (!arg_path(ctx, argv[0], &fs_ptr, &path, &raw)) return JS_EXCEPTION;
  const bool ok = fs_ptr->mkdir(path);
  JS_FreeCString(ctx, raw);
  return JS_NewBool(ctx, ok);
}

// Returns bare names, not paths, so a script can filter by extension and then
// rejoin. Directories are included; use fs.isDir() to tell them apart.
static JSValue js_fs_list(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
  if (argc < 1) return JS_ThrowTypeError(ctx, "list(dir) needs a path");
  fs::FS *fs_ptr; const char *path; const char *raw;
  if (!arg_path(ctx, argv[0], &fs_ptr, &path, &raw)) return JS_EXCEPTION;

  File dir = fs_ptr->open(path);
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    JS_FreeCString(ctx, raw);
    return JS_NULL;
  }
  JSValue arr = JS_NewArray(ctx);
  uint32_t i = 0;
  for (File e = dir.openNextFile(); e; e = dir.openNextFile()) {
    const char *full = e.name();
    // Some FS implementations return a full path, others a bare name; the
    // script wants the leaf either way.
    const char *slash = strrchr(full, '/');
    JS_SetPropertyUint32(ctx, arr, i++, JS_NewString(ctx, slash ? slash + 1 : full));
    e.close();
  }
  dir.close();
  JS_FreeCString(ctx, raw);
  return arr;
}

static JSValue js_fs_is_dir(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
  if (argc < 1) return JS_ThrowTypeError(ctx, "isDir(path) needs a path");
  fs::FS *fs_ptr; const char *path; const char *raw;
  if (!arg_path(ctx, argv[0], &fs_ptr, &path, &raw)) return JS_EXCEPTION;
  File f = fs_ptr->open(path);
  const bool ok = f && f.isDirectory();
  if (f) f.close();
  JS_FreeCString(ctx, raw);
  return JS_NewBool(ctx, ok);
}

// True when unprefixed paths will reach *some* storage, so a script can
// degrade gracefully rather than throwing on every call.
static JSValue js_fs_available(JSContext *ctx, JSValueConst, int, JSValueConst *) {
  return JS_NewBool(ctx, g_sd != nullptr || g_flash != nullptr);
}

void js_install_fs(JSContext *ctx) {
  JSValue global = JS_GetGlobalObject(ctx);

  JSValue o = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, o, "read", JS_NewCFunction(ctx, js_fs_read, "read", 1));
  JS_SetPropertyStr(ctx, o, "write", JS_NewCFunction(ctx, js_fs_write, "write", 2));
  JS_SetPropertyStr(ctx, o, "append", JS_NewCFunction(ctx, js_fs_append, "append", 2));
  JS_SetPropertyStr(ctx, o, "exists", JS_NewCFunction(ctx, js_fs_exists, "exists", 1));
  JS_SetPropertyStr(ctx, o, "remove", JS_NewCFunction(ctx, js_fs_remove, "remove", 1));
  JS_SetPropertyStr(ctx, o, "mkdir", JS_NewCFunction(ctx, js_fs_mkdir, "mkdir", 1));
  JS_SetPropertyStr(ctx, o, "list", JS_NewCFunction(ctx, js_fs_list, "list", 1));
  JS_SetPropertyStr(ctx, o, "isDir", JS_NewCFunction(ctx, js_fs_is_dir, "isDir", 1));
  JS_SetPropertyStr(ctx, o, "available", JS_NewCFunction(ctx, js_fs_available, "available", 0));
  JS_SetPropertyStr(ctx, global, "fs", o);

  JS_FreeValue(ctx, global);
}

#endif  // JSVM_WITH_FS
