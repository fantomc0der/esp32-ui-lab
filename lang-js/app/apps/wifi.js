// wifi.js — join a network from the touchscreen, no recompiling and no
// credentials in any file.
//
// Three screens: status, a scan list, and a password prompt with the on-screen
// keyboard. wifi.save() stores what you type in NVS, so the board rejoins by
// itself on every later boot. Nothing here can read the password back; the
// binding layer only accepts it.

const scr = lv.screen().set({ bg: "#0B1622", pad: 0, scroll: false });

// Rebuilding the screen from inside a click handler would delete the very
// widget LVGL is dispatching to. A 20 ms timer moves the work just past the
// end of the event, which is the pattern for any redraw triggered by a tap.
const next = fn => {
  const t = lv.timer(20, () => { t.stop(); fn(); });
};

const header = text => {
  lv.label(scr, { align: "top-left", x: 10, y: 8, font: 20, color: "#F0F4F8", text });
};

// ---------------------------------------------------------------- status

function showStatus() {
  scr.clean();
  const st = wifi.status();
  header("Wi-Fi");

  lv.label(scr, {
    align: "top-left", x: 10, y: 40, font: 16,
    color: st.connected ? "#4CAF50" : "#FF8A65",
    text: st.connected ? st.ssid : "Not connected",
  });
  if (st.connected) {
    lv.label(scr, {
      align: "top-left", x: 10, y: 64, font: 14, color: "#8A9BAB",
      text: `${st.ip}    ${st.rssi} dBm`,
    });
  }

  lv.button(scr, { w: 130, h: 36, align: "bottom-left", x: 10, y: -10, text: "Scan" })
    .on("click", () => next(showScan));

  if (st.saved) {
    lv.button(scr, { w: 120, h: 36, align: "bottom-left", x: 150, y: -10, text: "Forget" })
      .on("click", () => next(() => { wifi.forget(); showStatus(); }));
  }
}

// ---------------------------------------------------------------- scan list

function showScan() {
  scr.clean();
  header("Choose a network");
  const note = lv.label(scr, { align: "top-right", x: -46, y: 14, font: 14, color: "#64798C", text: "scanning..." });
  const list = lv.list(scr, { w: 300, h: 118, align: "bottom-mid", y: -5, bg: "#101E2C", border: 0, radius: 8 });

  wifi.scan(nets => {
    if (!nets) { note.set({ text: "scan failed" }); return; }
    // Strongest first, and skip the unnamed ones a scan usually turns up.
    const seen = new Set();
    const rows = nets.filter(n => n.ssid && !seen.has(n.ssid) && seen.add(n.ssid))
                     .sort((a, b) => b.rssi - a.rssi);
    note.set({ text: `${rows.length} found` });
    for (const n of rows) {
      const bars = n.rssi > -60 ? "▮▮▮" : n.rssi > -75 ? "▮▮" : "▮";
      list.add(`${n.ssid}   ${bars}${n.open ? "" : " ·"}`)
          .on("click", () => next(() => (n.open ? join(n.ssid, "") : showPassword(n.ssid))));
    }
  });
}

// ---------------------------------------------------------------- password

function showPassword(ssid) {
  scr.clean();
  lv.label(scr, { align: "top-left", x: 10, y: 4, font: 16, color: "#F0F4F8", text: ssid });

  const field = lv.textarea(scr, {
    w: 240, h: 34, align: "top-left", x: 10, y: 26,
    placeholder: "password", password: true, oneLine: true, maxLength: 63,
  });

  // Typing a long password blind on a 320x172 panel is how you end up entering
  // it three times. LVGL reveals each character for 1.5 s as it is typed, which
  // helps while typing but not when checking what you already have, so this
  // unmasks the whole field.
  let masked = true;
  const reveal = lv.button(scr, { w: 60, h: 34, align: "top-right", x: -10, y: 26, text: "Show" });
  reveal.on("click", () => {
    masked = !masked;
    field.set({ password: masked });
    reveal.set({ text: masked ? "Show" : "Hide" });
  });

  // The keyboard stops short of the right edge so it does not sit underneath
  // the firmware's home button.
  const kb = lv.keyboard(scr, { w: 280, h: 104, align: "bottom-left", x: 0, y: 0 });
  kb.target(field);
  kb.on("ready", () => next(() => join(ssid, field.value())));
  kb.on("cancel", () => next(showScan));
}

// ---------------------------------------------------------------- connecting

function join(ssid, password) {
  scr.clean();
  header("Connecting");
  const detail = lv.label(scr, { align: "center", font: 16, color: "#8A9BAB", text: ssid });

  wifi.save(ssid, password);

  // Poll until the driver settles, then hand back to the status screen.
  let waited = 0;
  const t = lv.timer(500, () => {
    waited += 500;
    const st = wifi.status();
    if (st.connected) {
      t.stop();
      showStatus();
    } else if (waited >= 15000) {
      t.stop();
      detail.set({ text: "Could not connect", color: "#FF5252" });
      lv.button(scr, { w: 130, h: 36, align: "bottom-mid", y: -10, text: "Try again" })
        .on("click", () => next(showScan));
    } else {
      detail.set({ text: `${ssid}\n${waited / 1000}s` });
    }
  });
}

showStatus();
console.log("wifi: setup app ready");
