// app.js — the C demo's System tab, reimplemented as data instead of firmware.
// Lives on the SD card (or the FATFS partition); edit it on a PC, long-press
// BOOT on the board, and the new UI is live. No compiler involved.

const scr = lv.screen().set({ bg: "#0A1520" });

const i = sys.info();
lv.label(scr, {
  align: "top-left", x: 6, y: 6, font: 14, color: "#E8E8E8",
  text: `${i.model}  rev${i.rev}  ${i.cores}x${i.mhz}MHz\n` +
        `flash ${i.flashMB}MB  psram ${i.psramMB}MB\n` +
        `lvgl ${i.lvgl}  quickjs ${i.quickjs}`,
});

const fps = lv.label(scr, { align: "top-right", x: -6, y: 6, font: 16, color: "#00BCD4", text: "-- fps" });
const live = lv.label(scr, { align: "left-mid", x: 6, y: 14, font: 14, color: "#B0C4D0", text: "..." });

// Backlight slider — a JS event handler driving a native PWM call.
lv.label(scr, { align: "bottom-left", x: 6, y: -34, font: 14, color: "#B0C4D0", text: "backlight" });
const slider = lv.slider(scr, { w: 280, h: 14, align: "bottom-mid", y: -10, range: [5, 100], value: 80 });
slider.on("change", () => sys.backlight(slider.value()));

// WiFi scan — an async native call whose result re-enters JS.
const scanBtn = lv.button(scr, { w: 90, h: 26, align: "top-right", x: -6, y: 34, text: "scan wifi" });
scanBtn.on("click", () => {
  scanBtn.set({ text: "scanning" });
  wifi.scan(nets => {
    scanBtn.set({ text: `${nets ? nets.length : "!"} networks` });
    if (nets) for (const n of nets) console.log(`  ${n.ssid}  ${n.rssi}dBm`);
  });
});

// Live readouts on a JS timer.
lv.timer(500, () => {
  const h = sys.heap();
  const up = (sys.uptime() / 1000) | 0;
  const bat = sys.battery();
  live.set({
    text: `heap ${(h.internal / 1024) | 0} kB  psram ${(h.psram / 1024) | 0} kB\n` +
          `up ${(up / 60) | 0}m ${up % 60}s   bat ${bat === null ? "n/a" : bat.toFixed(2) + "V"}`,
  });
  fps.set({ text: sys.fps() + " fps" });
});

console.log("app.js: system panel up");
