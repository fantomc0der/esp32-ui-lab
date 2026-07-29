// app.jsx — the launcher, and the first thing the board runs.
//
// @out app/app.js
//
// Lists every script in /apps and hands the screen to whichever you tap. The
// firmware draws a home button on top of running apps to bring you back here,
// so this file never has to think about the return trip.
//
// Long-press a row to pin that app. A pinned board boots straight into it and
// the firmware draws nothing over it, which is what makes the device read as an
// appliance rather than a launcher with apps on it. This screen stays reachable
// with a long-press of BOOT, and long-pressing the pinned row again releases it.
//
// Nothing here is exclusive: a tap still runs any app without touching the pin,
// and long-pressing a different row moves the pin to it. Which gestures manage
// the pin is this file's decision, not the firmware's, so a replacement
// launcher can choose otherwise.
//
// The imperative original carried a warning: "Rows are repainted, never
// rebuilt: deleting the row LVGL is dispatching a touch to is the one thing
// this screen must not do." That is now structural rather than a rule to keep.
// Rows are keyed by path, so a pin change patches them in place, and a render
// could not run during dispatch even if it did rebuild them.

const ROW_BG = "#101E2C";
const PIN_BG = "#1D3A57";

// Panel size, read once — a display does not resize. Fonts are fixed-size
// bitmaps and do not scale, so the header's height is a pixel constant rather
// than a fraction and the list takes whatever is left below it. That is what
// fills a taller panel with more rows instead of more empty space.
const S = lv.size();
const HEADER_H = 34;
// The firmware draws a 34px button in this corner on a pinned board or when a
// network needs setting up. App rows are the whole point of this screen, so the
// list stops short of it rather than putting a row half underneath it.
const CORNER = 40;

// "weather.js" -> "Weather". Filenames are the app names; there is no manifest
// to keep in sync, so dropping a file in /apps is all it takes to add an app.
const title = f => {
  const base = f.slice(0, -3);
  return base.charAt(0).toUpperCase() + base.slice(1);
};

const fileOf = path => path.slice(path.lastIndexOf("/") + 1);

const names = fs.available()
  ? (fs.list("/apps") || []).filter(n => n.endsWith(".js")).sort()
  : [];

function Launcher() {
  // The pin survives reboots and lives in NVS, so it can name an app that is no
  // longer on the card. Reporting it anyway beats silently showing no pin on the
  // one screen where you would go to clear it.
  const [pinned, setPinned] = useState(() => sys.pinned());

  // LVGL sends a normal click when the finger lifts after a long press, so the
  // pin gesture has to claim it. Cleared when each press starts, so an abandoned
  // drag cannot swallow the next tap. This is about LVGL's event order, not
  // about rendering, so it survives the port unchanged.
  const handled = useRef(false);

  const rows = useMemo(() => {
    const out = names.map(n => ({ path: "/apps/" + n, label: title(n), gone: false }));
    // A pin outlives the file it names, and can point outside /apps. Give it a
    // row regardless: this screen is where a pin is released, and one you cannot
    // see is one you cannot release without a serial cable.
    if (pinned && !out.some(r => r.path === pinned)) {
      const gone = fs.available() && !fs.exists(pinned);
      out.push({ path: pinned, gone, label: title(fileOf(pinned)) + (gone ? " (missing)" : "") });
    }
    return out;
  }, [pinned]);

  const hold = path => {
    handled.current = true;
    if (pinned === path) {
      sys.unpin();
      setPinned(null);
    } else if (sys.pin(path)) {
      setPinned(path);
    }
  };

  const status = pinned ? "pinned: " + title(fileOf(pinned))
               : names.length ? "hold to pin"
               : fs.available() ? "empty" : "no storage";

  return (
    <>
      <label align="top-left" x={10} y={8} font={20} color="#F0F4F8" text="Apps" />
      <label align="top-right" x={-10} y={13} font={14} color="#64798C" text={status} />

      <list w={S.w - CORNER - 10} h={S.h - HEADER_H - 10} align="bottom-left" x={10} y={-5}
            bg={ROW_BG} border={0} radius={8}>
        {rows.length
          ? rows.map(r => (
              <row key={r.path}
                   text={r.label}
                   bg={r.path === pinned ? PIN_BG : ROW_BG}
                   color={r.path === pinned ? "#7FC4FF" : "#F0F4F8"}
                   onPress={() => { handled.current = false; }}
                   onLongPress={() => hold(r.path)}
                   onClick={() => { if (!handled.current && !r.gone) sys.launch(r.path); }} />
            ))
          // Say what to do about it rather than just reporting emptiness.
          : [
              <row key="empty" text="No apps in /apps" />,
              <row key="how" text="Add one: app-begin /apps/x.js" />,
            ]}
      </list>
    </>
  );
}

lv.screen().set({ bg: "#0B1622" });
render(<Launcher />);

console.log(`launcher: ${names.length} app(s) found, pinned ${sys.pinned() || "none"}`);
