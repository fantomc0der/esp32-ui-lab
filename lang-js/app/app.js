// app.js — the launcher, and the first thing the board runs.
//
// Lists every script in /apps and hands the screen to whichever you tap. The
// firmware draws a home button on top of running apps to bring you back here,
// so this file never has to think about the return trip.

const scr = lv.screen().set({ bg: "#0B1622" });

lv.label(scr, { align: "top-left", x: 10, y: 8, font: 20, color: "#F0F4F8", text: "Apps" });
const status = lv.label(scr, { align: "top-right", x: -10, y: 13, font: 14, color: "#64798C", text: "" });

const list = lv.list(scr, {
  w: 300, h: 126, align: "bottom-mid", y: -5,
  bg: "#101E2C", border: 0, radius: 8,
});

// "weather.js" -> "Weather". Filenames are the app names; there is no manifest
// to keep in sync, so dropping a file in /apps is all it takes to add an app.
const title = f => {
  const base = f.slice(0, -3);
  return base.charAt(0).toUpperCase() + base.slice(1);
};

let names = [];
if (fs.available()) {
  names = (fs.list("/apps") || []).filter(n => n.endsWith(".js")).sort();
}

if (names.length) {
  status.set({ text: names.length + (names.length === 1 ? " app" : " apps") });
  for (const name of names) {
    list.add(title(name)).on("click", () => sys.launch("/apps/" + name));
  }
} else {
  // Say what to do about it rather than just reporting emptiness.
  status.set({ text: fs.available() ? "empty" : "no storage" });
  list.add("No apps in /apps");
  list.add("Add one: app-begin /apps/x.js");
}

console.log(`launcher: ${names.length} app(s) found`);
