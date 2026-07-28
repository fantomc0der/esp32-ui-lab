// wifi-imperative.js — FROZEN BASELINE, not a script the board runs.
//
// app/apps/wifi.js as it was written directly against the `lv` bindings, kept as
// the "before" side of tools/test-parity.mjs. See tools/fixtures/vitals-imperative.js
// for why these are files rather than git revisions, and why they must not be
// updated to track the ports.

// wifi.js — join a network from the touchscreen, no recompiling and no
// credentials in any file.
//
// Three screens: status, a scan list, and a password prompt with the on-screen
// keyboard. wifi.save() stores what you type in NVS, so the board rejoins by
// itself on every later boot. Nothing here can read the password back; the
// binding layer only accepts it.

const scr = lv.screen().set({ bg: "#0B1622", pad: 0, scroll: false });

// Panel size, read once. The three screens below place text at fixed pixel
// offsets (fonts are fixed bitmaps, so a 16px line is 16px on any panel) but
// size their lists, keyboard and buttons from what is actually there.
const S = lv.size();
// The firmware draws a 34px button in the bottom-right of every screen. Anything
// tappable that reaches into that corner is partly unreachable, so the keyboard,
// the buttons and the scan list all stop short of it.
const CORNER = 40;

// Rebuilding the screen from inside a click handler would delete the very
// widget LVGL is dispatching to. A 20 ms timer moves the work just past the
// end of the event, which is the pattern for any redraw triggered by a tap.
const next = fn => {
  const t = lv.timer(20, () => { t.stop(); fn(); });
};

// Screens that poll have to stop when they are torn down: their labels are
// gone after a clean(), and writing through a stale handle throws.
let screenTimer = null;
function reset() {
  if (screenTimer) { screenTimer.stop(); screenTimer = null; }
  scr.clean();
}

const header = text => {
  lv.label(scr, { align: "top-left", x: 10, y: 8, font: 20, color: "#F0F4F8", text });
};

// ---------------------------------------------------------------- status

function showStatus() {
  reset();
  header("Wi-Fi");

  const line1 = lv.label(scr, { align: "top-left", x: 10, y: 40, font: 16, text: "" });
  const line2 = lv.label(scr, { align: "top-left", x: 10, y: 64, font: 14, color: "#8A9BAB", text: "" });

  // Buttons are decided once, from the state at build time; only the text
  // above them refreshes, so nothing moves under your finger.
  // Two buttons share the width left of the corner button, whatever that is.
  const initial = wifi.status();
  const BTN_W = Math.min(150, ((S.w - CORNER - 30) / 2) | 0);
  lv.button(scr, { w: BTN_W, h: 36, align: "bottom-left", x: 10, y: -10, text: "Scan" })
    .on("click", () => next(showScan));
  if (initial.saved) {
    lv.button(scr, { w: BTN_W, h: 36, align: "bottom-left", x: 20 + BTN_W, y: -10, text: "Forget" })
      .on("click", () => next(() => { wifi.forget(); showStatus(); }));
  }

  const paint = () => {
    const st = wifi.status();
    if (st.connected) {
      line1.set({ text: st.ssid, color: "#4CAF50" });
      line2.set({ text: `${st.ip}    ${st.rssi} dBm`, color: "#8A9BAB" });
    } else if (st.saved && st.error) {
      // The case that had you guessing: credentials are stored but wrong.
      // Say which, and say what to do about it.
      line1.set({ text: st.error, color: "#FF5252" });
      line2.set({
        text: st.error === "wrong password"
          ? "Tap Forget, then Scan to retype it."
          : `Retrying (${st.attempts})... or Forget and pick again.`,
        color: "#C8D8E4",
      });
    } else if (st.saved) {
      line1.set({ text: "Connecting...", color: "#FFB74D" });
      line2.set({ text: "", color: "#8A9BAB" });
    } else {
      line1.set({ text: "Not set up", color: "#8A9BAB" });
      line2.set({ text: "Tap Scan to choose a network.", color: "#64798C" });
    }
  };

  paint();
  screenTimer = lv.timer(1500, paint);
}

// ---------------------------------------------------------------- scan list

function showScan() {
  reset();
  header("Choose a network");
  const note = lv.label(scr, { align: "top-right", x: -12, y: 14, font: 14, color: "#64798C", text: "scanning..." });
  // Everything below the header, which is one 20px line plus its margin. Anchored
  // bottom-left and stopping short of the corner button, so the last row is not
  // partly underneath it — a row you can see but only half tap is worse than a
  // slightly narrower list.
  const list = lv.list(scr, {
    w: S.w - CORNER - 10, h: S.h - 44, align: "bottom-left", x: 10, y: -5,
    bg: "#101E2C", border: 0, radius: 8,
  });

  wifi.scan(nets => {
    if (!nets) { note.set({ text: "scan failed" }); return; }
    // Strongest first, and skip the unnamed ones a scan usually turns up.
    const seen = new Set();
    const rows = nets.filter(n => n.ssid && !seen.has(n.ssid) && seen.add(n.ssid))
                     .sort((a, b) => b.rssi - a.rssi);
    note.set({ text: `${rows.length} found` });
    for (const n of rows) {
      const bars = n.rssi > -60 ? "|||" : n.rssi > -75 ? "||" : "|";
      list.add(`${n.ssid}   ${bars}${n.open ? "" : " *"}`)
          .on("click", () => next(() => (n.open ? join(n.ssid, "") : showPassword(n.ssid))));
    }
  });
}

// ---------------------------------------------------------------- password

function showPassword(ssid) {
  reset();
  lv.label(scr, { align: "top-left", x: 10, y: 4, font: 16, color: "#F0F4F8", text: ssid });

  // The field takes the row, less the reveal button beside it.
  const REVEAL_W = 60;
  const field = lv.textarea(scr, {
    w: S.w - REVEAL_W - 20, h: 34, align: "top-left", x: 10, y: 26,
    placeholder: "password", password: true, oneLine: true, maxLength: 63,
  });

  // Typing a long password blind on a small panel is how you end up entering
  // it three times. LVGL reveals each character for 1.5 s as it is typed, which
  // helps while typing but not when checking what you already have, so this
  // unmasks the whole field.
  let masked = true;
  const reveal = lv.button(scr, { w: REVEAL_W, h: 34, align: "top-right", x: -10, y: 26, text: "Show" });
  reveal.on("click", () => {
    masked = !masked;
    field.set({ password: masked });
    reveal.set({ text: masked ? "Show" : "Hide" });
  });

  // The keyboard stops short of the right edge so it does not sit underneath
  // the firmware's corner button, and takes everything below the field: a taller
  // panel gets taller keys rather than a gap under them.
  const kb = lv.keyboard(scr, { w: S.w - CORNER, h: S.h - 68, align: "bottom-left", x: 0, y: 0 });
  kb.target(field);
  kb.on("ready", () => next(() => join(ssid, field.value())));
  kb.on("cancel", () => next(showScan));
}

// ---------------------------------------------------------------- connecting

function join(ssid, password) {
  reset();
  header("Connecting");
  const detail = lv.label(scr, { align: "center", font: 16, color: "#8A9BAB", text: ssid });

  wifi.save(ssid, password);

  let waited = 0;
  screenTimer = lv.timer(500, () => {
    waited += 500;
    const st = wifi.status();
    if (st.connected) {
      showStatus();
    } else if (st.error === "wrong password") {
      // No point waiting out the timeout for something that will not improve.
      screenTimer.stop();
      screenTimer = null;
      detail.set({ text: "Wrong password", color: "#FF5252" });
      lv.button(scr, { w: 150, h: 36, align: "bottom-mid", y: -10, text: "Try again" })
        .on("click", () => next(() => showPassword(ssid)));
    } else if (waited >= 15000) {
      screenTimer.stop();
      screenTimer = null;
      detail.set({ text: st.error || "Could not connect", color: "#FF5252" });
      lv.button(scr, { w: 150, h: 36, align: "bottom-mid", y: -10, text: "Try again" })
        .on("click", () => next(showScan));
    } else {
      detail.set({ text: `${ssid}\n${waited / 1000}s` });
    }
  });
}

showStatus();
console.log("wifi: setup app ready");
