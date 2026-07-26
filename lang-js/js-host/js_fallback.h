// js_fallback.h — the script baked into flash, run when no app.js is found on
// the SD card or the FATFS partition (or when the loaded script throws at
// boot). Doubles as a minimal binding self-test: builds widgets, wires a click
// handler, logs — so a working fallback screen proves the JS stack end-to-end.
#pragma once

static const char kFallbackScript[] = R"js(
const scr = lv.screen().set({ bg: "#1A1022" });
lv.label(scr, { align: "top-mid", y: 12, font: 20, color: "#FFFFFF", text: "no app.js found" });
lv.label(scr, { align: "center", y: -4, font: 14, color: "#C0B0D0",
                text: "put app.js on the SD card,\nthen long-press BOOT to reload" });
let taps = 0;
const btn = lv.button(scr, { w: 130, h: 34, align: "bottom-mid", y: -10, text: "tap me" });
btn.on("click", () => btn.set({ text: "taps: " + (++taps) }));
console.log("[fallback] bindings alive, quickjs", sys.info().quickjs);
)js";
