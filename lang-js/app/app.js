// app.js — the launcher, and the first thing the board runs.
//
// Lists every script in /apps and hands the screen to whichever you tap. The
// firmware draws a home button on top of running apps to bring you back here,
// so this file never has to think about the return trip.
//
// Long-press a row to pin that app. A pinned board boots straight into it and
// the firmware draws nothing over it, which is what turns the device from a
// launcher with apps on it into an appliance that runs one thing. This screen
// stays reachable with a long-press of BOOT, and long-pressing the pinned row
// again releases it.

const scr = lv.screen().set({ bg: "#0B1622" });

const ROW_BG = "#101E2C";
const PIN_BG = "#1D3A57";

// Panel size, read once — a display does not resize. Fonts are fixed-size
// bitmaps and do not scale, so the header's height is a pixel constant rather
// than a fraction and the list takes whatever is left below it. That is what
// fills a taller panel with more rows instead of more empty space.
const S = lv.size();
const HEADER_H = 34;

lv.label(scr, { align: "top-left", x: 10, y: 8, font: 20, color: "#F0F4F8", text: "Apps" });
const status = lv.label(scr, { align: "top-right", x: -10, y: 13, font: 14, color: "#64798C", text: "" });

const list = lv.list(scr, {
  w: "94%", h: S.h - HEADER_H - 10, align: "bottom-mid", y: -5,
  bg: ROW_BG, border: 0, radius: 8,
});

// "weather.js" -> "Weather". Filenames are the app names; there is no manifest
// to keep in sync, so dropping a file in /apps is all it takes to add an app.
const title = f => {
  const base = f.slice(0, -3);
  return base.charAt(0).toUpperCase() + base.slice(1);
};

const fileOf = path => path.slice(path.lastIndexOf("/") + 1);

let names = [];
if (fs.available()) {
  names = (fs.list("/apps") || []).filter(n => n.endsWith(".js")).sort();
}

// The pin survives reboots and lives in NVS, so it can name an app that is no
// longer on the card. Reporting it anyway beats silently showing no pin on the
// one screen where you would go to clear it.
let pinned = sys.pinned();

// Rows are repainted, never rebuilt: deleting the row LVGL is dispatching a
// touch to is the one thing this screen must not do.
const rows = [];

function paint() {
  for (const row of rows) {
    const on = row.path === pinned;
    row.widget.set({ bg: on ? PIN_BG : ROW_BG, color: on ? "#7FC4FF" : "#F0F4F8" });
  }
  if (pinned) status.set({ text: "pinned: " + title(fileOf(pinned)) });
  else if (names.length) status.set({ text: "hold to pin" });
  else status.set({ text: fs.available() ? "empty" : "no storage" });
}

// LVGL sends a normal click when the finger lifts after a long press, so the
// pin gesture has to claim it. Cleared when each press starts, so an abandoned
// drag cannot swallow the next tap.
let handled = false;

if (names.length) {
  for (const name of names) {
    const path = "/apps/" + name;
    const widget = list.add(title(name));
    widget.on("press", () => { handled = false; });
    widget.on("longpress", () => {
      handled = true;
      if (pinned === path) {
        sys.unpin();
        pinned = null;
      } else if (sys.pin(path)) {
        pinned = path;
      }
      paint();
    });
    widget.on("click", () => { if (!handled) sys.launch(path); });
    rows.push({ path, widget });
  }
} else {
  // Say what to do about it rather than just reporting emptiness.
  list.add("No apps in /apps");
  list.add("Add one: app-begin /apps/x.js");
}

// A pin outlives the file it names, and can point outside /apps. Give it a row
// regardless: this screen is where a pin is released, and one you cannot see is
// one you cannot release without a serial cable.
if (pinned && !rows.some(r => r.path === pinned)) {
  const path = pinned;
  const gone = fs.available() && !fs.exists(path);
  const widget = list.add(title(fileOf(path)) + (gone ? " (missing)" : ""));
  widget.on("press", () => { handled = false; });
  widget.on("longpress", () => {
    handled = true;
    sys.unpin();
    pinned = null;
    paint();
  });
  widget.on("click", () => { if (!handled && !gone) sys.launch(path); });
  rows.push({ path, widget });
}

paint();

console.log(`launcher: ${names.length} app(s) found, pinned ${pinned || "none"}`);
