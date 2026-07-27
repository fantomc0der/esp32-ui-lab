// app.js — the C demo (lang-c/app) recreated as data instead of
// firmware: the same 4-tab dashboard, now editable on a PC and hot-reloaded
// with a long-press of BOOT. Tab-for-tab mirror of the C original:
//   Vitals  — free-heap line chart, render-load arc gauge, uptime/battery
//   Touch   — draw box with a dot that follows your finger + live coordinates
//   WiFi    — async scan into a list, UI stays alive while the radio works
//   System  — chip facts, live fps, backlight slider

const scr = lv.screen().set({ bg: "#0A1520" });
const BAR_H = 30;
const tabs = lv.tabview(scr, { bar: BAR_H, bg: "#0A1520" });

// Everything below is sized from the panel rather than for one of them. Widths
// go through percentages where the parent is sized; heights are computed,
// because what is left inside a tab depends on the tab bar and the padding.
// Fonts are fixed bitmaps and never scale, so anything holding text keeps a
// pixel height.
const S = lv.size();
const TAB_H = S.h - BAR_H - 8;  // content height inside a tab, past pad: 4

// ---------------------------------------------------------------- tab 1: Vitals
const vitals = tabs.addTab("Vitals").set({ pad: 4, scroll: false });

// Two columns, 58/40 with the remainder as the gutter between them. They align
// to opposite edges rather than being positioned, so the split holds at any width.
const left = lv.obj(vitals, { w: "58%", h: "100%", align: "left-mid", pad: 4, scroll: false });
const heapLabel = lv.label(left, { align: "top-left", font: 14, text: "heap --" });
const chart = lv.chart(left, {
  w: "100%", h: "70%", align: "bottom-mid",
  // One point per ~8px of width, so a wider panel shows more history rather
  // than the same history stretched.
  points: Math.max(20, (S.w / 8) | 0),
  range: [0, 340], divs: [4, 6], seriesColor: "#00BCD4",
});

const right = lv.obj(vitals, { w: "40%", h: "100%", align: "right-mid", pad: 4, scroll: false });
// An arc has to stay square, so it is the smaller of what the column and the
// height allow. The width comes from the column's own content area rather than a
// fraction of the screen: 40% of the tab is not what the arc can occupy once the
// pad and the theme's card border have taken theirs, and guessing at that
// overflowed the column on a taller panel. The column does not scroll, so an
// oversized arc is clipped rather than reachable.
// The `|| ` guards a zero: bounds() forces a layout pass, but a container that
// somehow measured empty would otherwise size the arc to nothing at all.
const ARC = Math.min(right.bounds().w || ((S.w * 0.3) | 0), (TAB_H * 0.6) | 0);
const loadArc = lv.arc(right, {
  w: ARC, h: ARC, align: "top-mid",
  range: [0, 100], rotation: 135, angles: [0, 270], knob: false,
});
const loadLabel = lv.label(loadArc, { align: "center", font: 16, text: "0%" });
const uptimeLabel = lv.label(right, { align: "bottom-mid", font: 14, text: "up 0s" });

// ---------------------------------------------------------------- tab 2: Touch
const touch = tabs.addTab("Touch").set({ pad: 4, scroll: false });

const coordLabel = lv.label(touch, { align: "top-left", x: 2, y: 0, font: 14, text: "touch the box" });
const box = lv.obj(touch, {
  // The coordinate readout above it is one 14px line; the box takes the rest.
  w: "100%", h: TAB_H - 22, align: "bottom-mid", y: -2,
  bg: "#11202B", border: 1, borderColor: "#00BCD4", scroll: false, clickable: true,
});
const dot = lv.obj(box, { w: 10, h: 10, radius: 5, bg: "#FFC107", border: 0, hidden: true });

const followFinger = (w, x, y) => {
  const b = box.bounds();
  dot.set({ x: x - b.x - 5, y: y - b.y - 5, hidden: false });
  coordLabel.set({ text: `x ${x}  y ${y}` });
};
box.on("press", followFinger).on("pressing", followFinger);

// ---------------------------------------------------------------- tab 3: WiFi
const wifiTab = tabs.addTab("WiFi").set({ pad: 4, scroll: false });

const SCAN_W = 84;
const scanBtn = lv.button(wifiTab, { w: SCAN_W, h: 30, align: "top-left", text: "Scan" });
const wifiStatus = lv.label(wifiTab, { align: "top-left", x: SCAN_W + 8, y: 8, font: 14, text: "idle" });
// Stops short of the firmware's 34px corner button, so no row is half under it.
const wifiList = lv.list(wifiTab, { w: S.w - 48, h: TAB_H - 36, align: "bottom-left" });

function doScan() {
  const started = wifi.scan(nets => {
    if (!nets) { wifiStatus.set({ text: "scan failed" }); return; }
    wifiStatus.set({ text: `${nets.length} found` });
    for (const n of nets.slice(0, 12)) wifiList.add(`${n.ssid}  ${n.rssi}dBm`);
  });
  if (started) {
    wifiList.clean();
    wifiStatus.set({ text: "scanning..." });
  }
}
scanBtn.on("click", doScan);

// ---------------------------------------------------------------- tab 4: System
const sysTab = tabs.addTab("System").set({ pad: 4, scroll: false });

const i = sys.info();
lv.label(sysTab, {
  align: "top-left", x: 2, y: 0, font: 14,
  text: `${i.model}  rev${i.rev}  ${i.cores}x${i.mhz}MHz\n` +
        `flash ${i.flashMB}MB   psram ${i.psramMB}MB   lvgl ${i.lvgl}   qjs ${i.quickjs}`,
});
const fpsLabel = lv.label(sysTab, { align: "top-right", x: -2, y: 34, font: 16, color: "#00BCD4", text: "-- fps" });
lv.label(sysTab, { align: "bottom-left", x: 2, y: -34, font: 14, text: "backlight" });
const slider = lv.slider(sysTab, { w: "90%", h: 14, align: "bottom-mid", y: -8, range: [5, 100], value: 80 });
slider.on("change", () => sys.backlight(slider.value()));

// ---------------------------------------------------------------- live readouts
lv.timer(500, () => {
  const h = sys.heap();
  chart.push((h.internal / 1024) | 0);
  heapLabel.set({ text: `heap ${(h.internal / 1024) | 0} kB  psram ${(h.psram / 1024) | 0} kB` });

  // Render throughput as a fraction of a 30 flush/s target, same as the C demo.
  const load = Math.min(100, ((sys.fps() * 100) / 30) | 0);
  loadArc.value(load);
  loadLabel.set({ text: load + "%" });

  const up = (sys.uptime() / 1000) | 0;
  const bat = sys.battery();
  uptimeLabel.set({
    text: `up ${(up / 60) | 0}m ${up % 60}s  ` + (bat === null ? "bat n/a" : bat.toFixed(2) + "V"),
  });
  fpsLabel.set({ text: sys.fps() + " fps" });
});

console.log("vitals: dashboard up (4 tabs)");
