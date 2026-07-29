// jsvm_app.cpp — the app supervisor: where scripts come from, how you get back
// out of one, and what the serial port does.
//
// This is policy rather than hardware. Which file boots, what happens when it
// throws, what the corner button offers, how a script arrives over serial: none
// of it depends on the panel it runs on, so it lives here instead of being
// copied into every board sketch and drifting between them.
//
// The board's half of the deal is the hardware: bring up the display, touch and
// LVGL, mount storage, then call jsvm_app_begin() and jsvm_app_service().
//
// Two rules shape everything below.
//
// An app switch is always queued and performed from jsvm_app_service(), never
// where it was asked for (see jsvm_app_request). LVGL event dispatch, the serial
// REPL and the promise queue can all ask for one, and none of them survives
// having the context torn down underneath it.
//
// There is no way to end up staring at a dead panel. A missing or throwing app
// falls back to the launcher, and a broken launcher falls back to a screen built
// into the firmware.

#include <Arduino.h>
#include <FS.h>
#include <esp_heap_caps.h>
#include <lvgl.h>

#include "js_bindings.h"

// The script run when nothing else loads. Doubles as a minimal self-test:
// it builds widgets, wires a click handler and logs, so a working fallback
// screen proves the JS stack end to end.
static const char kFallbackScript[] = R"js(
const scr = lv.screen().set({ bg: "#1A1022" });
lv.label(scr, { align: "top-mid", y: 12, font: 20, color: "#FFFFFF", text: "no app.js found" });
lv.label(scr, { align: "center", y: -4, font: 14, color: "#C0B0D0",
                text: "put app.js on the SD card,\nthen long-press the board's button to reload" });
let taps = 0;
const btn = lv.button(scr, { w: 130, h: 34, align: "bottom-mid", y: -10, text: "tap me" });
btn.on("click", () => btn.set({ text: "taps: " + (++taps) }));
console.log("[fallback] bindings alive, quickjs", sys.info().quickjs);
)js";

static JsvmAppConfig g_cfg;
static char g_current_app[128] = "";
static char g_next_app[128] = "";

// ---------------------------------------------------------------- script loading

// Reads a whole file into a PSRAM buffer (caller frees). nullptr on any miss.
static char *readAll(fs::FS &fs, const char *path) {
  File f = fs.open(path, FILE_READ);
  if (!f || f.isDirectory()) return nullptr;
  const size_t n = f.size();
  char *buf = static_cast<char *>(heap_caps_malloc(n + 1, MALLOC_CAP_SPIRAM));
  if (!buf) { f.close(); return nullptr; }
  const size_t got = f.read(reinterpret_cast<uint8_t *>(buf), n);
  f.close();
  buf[got] = '\0';
  return buf;
}

// "flash:/x" reads the flash filesystem explicitly. Any other path prefers the
// card and falls back to flash, so the same script paths work whether or not a
// card is fitted.
static char *loadScript(const char *path) {
  if (strncmp(path, "flash:", 6) == 0) {
    return g_cfg.flash ? readAll(*g_cfg.flash, path + 6) : nullptr;
  }
  char *src = g_cfg.sd ? readAll(*g_cfg.sd, path) : nullptr;
  if (!src && g_cfg.flash) src = readAll(*g_cfg.flash, path);
  return src;
}

// Same convention for the host commands that touch storage directly.
static fs::FS *resolveFs(const char *path, const char **out_path) {
  if (strncmp(path, "flash:", 6) == 0) {
    *out_path = path + 6;
    return g_cfg.flash;
  }
  *out_path = path;
  return g_cfg.sd ? g_cfg.sd : g_cfg.flash;
}

bool jsvm_app_request(const char *path) {
  // Refuse rather than truncate. A truncated path is a request for a file
  // nobody asked for, and it comes back as "not found" against a path the user
  // typed correctly. jsvm_set_pinned_app() answers the same question the same
  // way.
  if (strlen(path) >= sizeof(g_next_app)) {
    Serial.printf("[app] path too long (max %u): %s\n", (unsigned)(sizeof(g_next_app) - 1), path);
    return false;
  }
  strcpy(g_next_app, path);
  return true;
}

const char *jsvm_app_current() { return g_current_app[0] ? g_current_app : nullptr; }

// ---------------------------------------------------------------- corner button

// One slot in the bottom-right corner, and at most one control in it. What
// belongs there is derived from where you are and what the running app needs,
// never set by whoever caused the change, because a script can pin itself or
// discover it has no network at any moment.
enum CornerButton {
  kCornerNone,
  kCornerHome,  // to the launcher
  kCornerBack,  // to the pinned app, from a detour away from it
  kCornerWifi,  // to network setup
};

static lv_obj_t *g_corner_btn = nullptr;
static lv_obj_t *g_corner_icon = nullptr;
static CornerButton g_corner = kCornerNone;  // createCornerButton() starts it hidden

// The script a user pinned, or null. Without the sys module there is no pinning,
// so the corner is always a plain way home.
static const char *pinnedApp() {
#if JSVM_WITH_SYS
  return jsvm_pinned_app();
#else
  return nullptr;
#endif
}

// Where leaving the current app goes. Normally the launcher; on a pinned board
// the pinned app, since a pin says the launcher is not part of the product and
// the appliance is what you expect to come back to.
static const char *cornerBackTarget() {
  const char *pinned = pinnedApp();
  return pinned ? pinned : g_cfg.launcher;
}

// Cheap enough to call every service pass, which is what keeps the corner right
// without anything having to remember to update it.
static void updateCornerButton() {
  if (!g_corner_btn) return;

  CornerButton want;
#if JSVM_WITH_WIFI
  const bool wifi_wanted = g_cfg.wifi_app && jsvm_network_setup_needed() &&
                           strcmp(g_current_app, g_cfg.wifi_app) != 0;
#else
  const bool wifi_wanted = false;
#endif
  if (wifi_wanted) {
    // Setup outranks navigation. An app with no network to talk to is stuck at
    // something the user can fix in two taps, and on a pinned board this is the
    // only visible route to fixing it — the corner would otherwise be empty and
    // a button long-press is not something anyone discovers. Not offered inside
    // the Wi-Fi app itself, which is already the destination.
    want = kCornerWifi;
  } else if (strcmp(g_current_app, cornerBackTarget()) == 0) {
    // Nothing to offer while you are already where the button would take you:
    // an escape hatch you are not meant to use is a stray control on the UI.
    want = kCornerNone;
  } else {
    want = pinnedApp() ? kCornerBack : kCornerHome;
  }

  if (want == g_corner) return;
  g_corner = want;
  if (want == kCornerNone) {
    lv_obj_add_flag(g_corner_btn, LV_OBJ_FLAG_HIDDEN);
    return;
  }
  lv_label_set_text(g_corner_icon, want == kCornerWifi ? LV_SYMBOL_WIFI
                                 : want == kCornerBack ? LV_SYMBOL_LEFT
                                                       : LV_SYMBOL_HOME);
  lv_obj_remove_flag(g_corner_btn, LV_OBJ_FLAG_HIDDEN);
}

// Tapping only queues the switch; see jsvm_app_request(). Reading g_corner
// rather than recomputing keeps the tap honest: you get the button you saw.
static void cornerClicked(lv_event_t *) {
  jsvm_app_request(g_corner == kCornerWifi ? g_cfg.wifi_app : cornerBackTarget());
}

// Drawn once and owned by the firmware. It lives on LVGL's top layer rather
// than the active screen, so jsvm_stop()'s lv_obj_clean() cannot delete it and
// no script can reach it — every app gets an escape hatch it is incapable of
// breaking.
static void createCornerButton() {
  g_corner_btn = lv_button_create(lv_layer_top());
  lv_obj_set_size(g_corner_btn, 34, 34);
  lv_obj_align(g_corner_btn, LV_ALIGN_BOTTOM_RIGHT, -3, -3);
  lv_obj_set_style_radius(g_corner_btn, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(g_corner_btn, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(g_corner_btn, LV_OPA_60, 0);
  lv_obj_set_style_border_color(g_corner_btn, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_border_width(g_corner_btn, 1, 0);
  lv_obj_set_style_border_opa(g_corner_btn, LV_OPA_50, 0);
  lv_obj_set_style_shadow_width(g_corner_btn, 0, 0);
  lv_obj_add_event_cb(g_corner_btn, cornerClicked, LV_EVENT_CLICKED, nullptr);

  g_corner_icon = lv_label_create(g_corner_btn);
  lv_label_set_text(g_corner_icon, LV_SYMBOL_HOME);
  lv_obj_center(g_corner_icon);
  lv_obj_add_flag(g_corner_btn, LV_OBJ_FLAG_HIDDEN);
}

// ---------------------------------------------------------------- running apps

// Starts a script. A missing or throwing app falls back to the launcher, and
// a broken launcher falls back to the built-in screen. Recursion is bounded at
// one level: the retry always targets the launcher, which takes the built-in
// branch if it fails.
static void runApp(const char *path) {
  jsvm_stop();

  char *src = loadScript(path);
  bool ok = false;
  if (src) {
    Serial.printf("[app] running %s\n", path);
    ok = jsvm_start(src, path);
    heap_caps_free(src);
  } else {
    Serial.printf("[app] %s not found\n", path);
  }

  if (ok) {
    strncpy(g_current_app, path, sizeof(g_current_app) - 1);
    g_current_app[sizeof(g_current_app) - 1] = '\0';
  } else {
    jsvm_stop();
    if (strcmp(path, g_cfg.launcher) != 0) {
      Serial.println("[app] falling back to the launcher");
      runApp(g_cfg.launcher);
      return;
    }
    Serial.println("[app] no launcher, using the built-in screen");
    jsvm_start(kFallbackScript, "built-in fallback");
    g_current_app[0] = '\0';
  }

  updateCornerButton();
}

// ---------------------------------------------------------------- serial

static bool writeScript(const char *path, const String &text) {
  const char *p;
  fs::FS *dest = resolveFs(path, &p);
  if (!dest) return false;

  if (!*p) return false;  // "flash:" with nothing after it leaves p empty

  // open(FILE_WRITE) will not create intervening directories, so uploading
  // /apps/x.js to a freshly formatted card reported only "write FAILED" — which
  // reads as a broken card rather than a missing folder. Walk the path and create
  // each level: mkdir() itself is not recursive either, so creating only the
  // immediate parent would leave a nested path failing the same silent way.
  // Starting at p + 1 skips a leading slash, which is the root and always exists.
  for (const char *slash = strchr(p + 1, '/'); slash; slash = strchr(slash + 1, '/')) {
    String dir(p);
    dir.remove(slash - p);
    if (!dest->exists(dir.c_str()) && !dest->mkdir(dir.c_str())) {
      // Say which level failed. Otherwise this lands as the same undifferentiated
      // "write FAILED" the walk exists to eliminate.
      Serial.printf("[fs] mkdir %s FAILED\n", dir.c_str());
      return false;
    }
  }

  File f = dest->open(p, FILE_WRITE);
  if (!f) return false;
  f.print(text);
  f.close();
  return true;
}

// One line from serial = one JS eval in the running app's context, except for
// host commands:
//   home                       open the launcher
//   reload                     restart the current app from storage
//   pin [path]                 boot straight into this app from now on,
//                              defaulting to whatever is running
//   unpin                      go back to booting the launcher
//   ls [dir]                   list a directory (default /)
//   rm <path>                  delete a file
//   app-begin [path]           start receiving a script; defaults to whatever
//     ...lines...              is running now, so the usual edit loop is just
//   app-end                    app-begin / paste / app-end, then it reloads
static void pollSerialRepl() {
  static String line;
  static String upload;
  static String upload_path;
  static bool uploading = false;
  // Set when a line could not be stored in full, either because it passed the
  // cap or because the String would not grow. Both are checked at the newline
  // rather than here: what matters is that the line never reaches the REPL or
  // the file, and that whoever sent it is told.
  static bool line_lost = false;
  // Caps so a hostile/broken sender can't grow these Strings until the
  // internal heap dies: REPL lines beyond 4 KB are refused, uploads beyond
  // 256 KB abort the transfer. Both Strings live in internal RAM, so an upload
  // fails on allocation well before the 256 KB cap on this board; the cap is
  // the backstop, the allocation check below is the real limit.
  constexpr size_t kMaxLine = 4 * 1024;
  constexpr size_t kMaxUpload = 256 * 1024;
  while (Serial.available()) {
    const char ch = static_cast<char>(Serial.read());
    if (ch == '\r') continue;
    if (ch != '\n') {
      if (line.length() >= kMaxLine || !line.concat(ch)) line_lost = true;
      continue;
    }
    // A partial line is not a line. Dropping the tail and carrying on used to
    // write a corrupted script to the card and report success.
    if (line_lost) {
      line_lost = false;
      if (uploading) {
        uploading = false;
        upload = "";
        Serial.println("[app] upload aborted: a line was too long or memory ran out");
      } else {
        Serial.println("[app] line ignored: longer than the 4 KB cap");
      }
      line = "";
      continue;
    }
    if (uploading && upload.length() + line.length() > kMaxUpload) {
      uploading = false;
      upload = "";
      Serial.println("[app] upload aborted: exceeds 256 KB cap");
      line = "";
      continue;
    }

    if (uploading) {
      if (line == "app-end") {
        uploading = false;
        if (writeScript(upload_path.c_str(), upload)) {
          Serial.printf("[app] wrote %u bytes to %s\n", upload.length(), upload_path.c_str());
          upload = "";
          jsvm_app_request(upload_path.c_str());
        } else {
          Serial.printf("[app] write to %s FAILED\n", upload_path.c_str());
          upload = "";
        }
      } else if (!upload.concat(line) || !upload.concat('\n')) {
        // Out of internal RAM. String's += reports this by quietly not
        // growing, which would surface as a script that is missing a line
        // somewhere in the middle and still gets written out.
        uploading = false;
        upload = "";
        Serial.println("[app] upload aborted: out of memory");
      }
    } else if (line == "home") {
      jsvm_app_request(g_cfg.launcher);
    } else if (line == "reload") {
      Serial.println("[app] reload requested over serial");
      jsvm_app_request(g_current_app[0] ? g_current_app : g_cfg.launcher);
#if JSVM_WITH_SYS
    } else if (line == "pin" || line.startsWith("pin ")) {
      // Bare `pin` means "this one", the case you are in after tapping an app.
      const String target = (line.length() > 4) ? line.substring(4) : String(g_current_app);
      if (target.isEmpty() || target == g_cfg.launcher) {
        Serial.println("[app] pin <path>, or run the app you want pinned first");
      } else if (!jsvm_set_pinned_app(target.c_str())) {
        Serial.println("[app] pin FAILED (NVS unavailable)");
      }
    } else if (line == "unpin") {
      jsvm_set_pinned_app(nullptr);
#endif
    } else if (line == "ls" || line.startsWith("ls ")) {
      const String dir = (line.length() > 3) ? line.substring(3) : String("/");
      const char *p; fs::FS *f = resolveFs(dir.c_str(), &p);
      File d = f ? f->open(p) : File();
      if (!d || !d.isDirectory()) {
        Serial.printf("[fs] %s is not a directory\n", dir.c_str());
      } else {
        for (File e = d.openNextFile(); e; e = d.openNextFile()) {
          Serial.printf("  %-28s %8u%s\n", e.name(), (unsigned)e.size(),
                        e.isDirectory() ? "  <dir>" : "");
          e.close();
        }
      }
      if (d) d.close();
    } else if (line.startsWith("rm ")) {
      const String target = line.substring(3);
      const char *p; fs::FS *f = resolveFs(target.c_str(), &p);
      Serial.printf("[fs] rm %s %s\n", target.c_str(),
                    (f && f->remove(p)) ? "ok" : "FAILED");
    } else if (line == "app-begin" || line.startsWith("app-begin ")) {
      // No path given means "replace what is running", which is the common
      // case while iterating on one app.
      upload_path = (line.length() > 10) ? line.substring(10)
                                         : String(g_current_app[0] ? g_current_app : g_cfg.launcher);
      uploading = true;
      upload = "";
      Serial.printf("[app] receiving into %s; finish with app-end\n", upload_path.c_str());
    } else if (line.length()) {
      jsvm_repl_line(line.c_str());
    }
    line = "";
  }
}

// ---------------------------------------------------------------- begin / service

void jsvm_app_begin(const JsvmAppConfig &cfg) {
  g_cfg = cfg;
  if (!g_cfg.launcher) g_cfg.launcher = "/app.js";

  jsvm_set_filesystem(g_cfg.sd, g_cfg.flash);
  Serial.printf("[app] storage: sd %s, flash %s\n", g_cfg.sd ? "ok" : "none",
                g_cfg.flash ? "ok" : "none");

  if (g_cfg.home_button_pin >= 0) {
    // Pulled up because the button shorts to ground; a board with its own
    // external pull-up is unaffected.
    pinMode(g_cfg.home_button_pin, INPUT_PULLUP);
  }

  createCornerButton();

#if JSVM_WITH_WIFI
  // Safe here rather than earlier: LVGL exists by now, so the binding can
  // create the lv_timer that supervises reconnection.
  if (!jsvm_wifi_autoconnect()) {
    Serial.println("[wifi] no saved network — use the Wifi app or wifi.save()");
  }
#endif

  // A pinned app takes the place of the launcher entirely. If it is missing or
  // throws, runApp() still lands on the launcher, so a bad pin costs you the
  // appliance behaviour but never the board.
  const char *pinned = pinnedApp();
  if (pinned) Serial.printf("[app] %s is pinned — skipping the launcher\n", pinned);
  runApp(pinned ? pinned : g_cfg.launcher);

  Serial.println("[app] ready — serial is a JS REPL; 'reload' restarts the app, 'home' opens the launcher");
}

void jsvm_app_service() {
  // Long-press (>= 700 ms) opens the launcher. This is the hardware escape
  // hatch: it works even if an app has covered the corner button, a pin has
  // removed it, or the touch panel has stopped responding.
  if (g_cfg.home_button_pin >= 0) {
    static uint32_t down_since = 0;
    static bool fired = false;
    const uint32_t now = millis();
    if (digitalRead(g_cfg.home_button_pin) == LOW) {
      if (down_since == 0) down_since = now;
      if (!fired && now - down_since >= 700) {
        fired = true;
        Serial.println("[app] launcher requested via button long-press");
        jsvm_app_request(g_cfg.launcher);
      }
    } else {
      down_since = 0;
      fired = false;
    }
  }

  pollSerialRepl();
  jsvm_pump();  // promise reactions / async continuations
  // A script may have pinned itself, or found it has no network, since the
  // last pass.
  updateCornerButton();

  // Switch apps only here, once everything that could be mid-callback has
  // finished: LVGL event dispatch, the serial REPL, and the promise queue can
  // all ask for a switch, and none of them can survive their own teardown.
  const char *from_js = jsvm_take_pending_launch();
  if (from_js) jsvm_app_request(from_js);
  if (g_next_app[0] != '\0') {
    char path[sizeof(g_next_app)];
    strncpy(path, g_next_app, sizeof(path));
    g_next_app[0] = '\0';
    runApp(path);
  }
}
