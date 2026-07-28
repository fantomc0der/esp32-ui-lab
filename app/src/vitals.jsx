// vitals.jsx — the 4-tab dashboard, as components.
//
// A port of the imperative app/apps/vitals.js, kept behaviour-for-behaviour the
// same so the two can be compared: same tabs, same layout arithmetic, same
// 500 ms readout cycle, same wifi scan, same touch dot. The imperative original
// is in this repo's history at the commit that replaced it, and weather.js and
// wifi.js are still written that way.
//
// It is the honest comparison rather than the flattering one, because two parts
// of this app are *not* what a reconciler is good at, and both are called out
// below: the arc that has to measure its parent before it can size itself, and
// the dot that follows a finger. Everything else got shorter.

const S = lv.size();
const BAR_H = 30;
const TAB_H = S.h - BAR_H - 8;   // content height inside a tab, past pad: 4

const CYAN = "#00BCD4";

// ---------------------------------------------------------------- tab 1: Vitals

function VitalsTab({ heap, load, uptime, battery, chartRef }) {
  // The arc has to stay square and fit the column it is in, and the column's
  // real width is only knowable after a layout pass — 40% of the tab is not what
  // is left once padding and the theme's card border have taken theirs. The
  // imperative version reads bounds() at construction time, which it can,
  // because by then the parent exists.
  //
  // Declaratively it takes two passes: render with the estimate the original
  // used as its fallback, measure in an effect, and correct. The correction is
  // one .set() on one widget, on the first frame, and this is the clearest case
  // in the app where the imperative version is simply more direct.
  const right = useRef(null);
  const [arc, setArc] = useState(Math.min((S.w * 0.3) | 0, (TAB_H * 0.6) | 0));
  useEffect(() => {
    const w = right.current.bounds().w;
    if (w) setArc(Math.min(w, (TAB_H * 0.6) | 0));
  }, []);

  return (
    <tab name="Vitals" pad={4} scroll={false}>
      <obj w="58%" h="100%" align="left-mid" pad={4} scroll={false}>
        <label align="top-left" font={14} text={heap} />
        <chart
          ref={chartRef}
          w="100%" h="70%" align="bottom-mid"
          // One point per ~8px of width, so a wider panel shows more history
          // rather than the same history stretched.
          points={Math.max(20, (S.w / 8) | 0)}
          range={[0, 340]} divs={[4, 6]} seriesColor={CYAN}
        />
      </obj>

      <obj ref={right} w="40%" h="100%" align="right-mid" pad={4} scroll={false}>
        <arc w={arc} h={arc} align="top-mid"
             range={[0, 100]} rotation={135} angles={[0, 270]} knob={false}
             value={load}>
          <label align="center" font={16} text={load + "%"} />
        </arc>
        <label align="bottom-mid" font={14} text={uptime + "  " + battery} />
      </obj>
    </tab>
  );
}

// ---------------------------------------------------------------- tab 2: Touch

function TouchTab() {
  // The dot follows a finger, which is the one thing in this app that the docs
  // say not to put through the reconciler: a render per pointer move, to move
  // one widget, while the other three tabs are diffed for nothing. So it is a
  // ref and two .set() calls, exactly as the imperative version does it — the
  // escape hatch is there to be used, and this is what it is for.
  const dot = useRef(null);
  const box = useRef(null);
  const coords = useRef(null);

  const follow = e => {
    const b = box.current.bounds();
    dot.current.set({ x: e.x - b.x - 5, y: e.y - b.y - 5, hidden: false });
    coords.current.set({ text: `x ${e.x}  y ${e.y}` });
  };

  return (
    <tab name="Touch" pad={4} scroll={false}>
      <label ref={coords} align="top-left" x={2} y={0} font={14} text="touch the box" />
      <obj ref={box}
           // The coordinate readout above it is one 14px line; the box takes the rest.
           w="100%" h={TAB_H - 22} align="bottom-mid" y={-2}
           bg="#11202B" border={1} borderColor={CYAN} scroll={false} clickable
           onPress={follow} onPressing={follow}>
        <obj ref={dot} w={10} h={10} radius={5} bg="#FFC107" border={0} hidden />
      </obj>
    </tab>
  );
}

// ---------------------------------------------------------------- tab 3: WiFi

function WifiTab() {
  const [status, setStatus] = useState("idle");
  const [nets, setNets] = useState([]);

  const SCAN_W = 84;

  const scan = () => {
    const started = wifi.scan(found => {
      if (!found) { setStatus("scan failed"); setNets([]); return; }
      setStatus(found.length + " found");
      setNets(found.slice(0, 12));
    });
    if (started) {
      setNets([]);
      setStatus("scanning...");
    }
  };

  return (
    <tab name="WiFi" pad={4} scroll={false}>
      <button w={SCAN_W} h={30} align="top-left" text="Scan" onClick={scan} />
      <label align="top-left" x={SCAN_W + 8} y={8} font={14} text={status} />
      {/* Stops short of the firmware's 34px corner button, so no row is half under it. */}
      <list w={S.w - 48} h={TAB_H - 36} align="bottom-left">
        {nets.map(n => (
          <row key={n.ssid + " " + n.rssi} text={`${n.ssid}  ${n.rssi}dBm`} />
        ))}
      </list>
    </tab>
  );
}

// ---------------------------------------------------------------- tab 4: System

function SystemTab({ fps }) {
  const i = useMemo(() => sys.info(), []);
  return (
    <tab name="System" pad={4} scroll={false}>
      <label align="top-left" x={2} y={0} font={14}
             text={`${i.model}  rev${i.rev}  ${i.cores}x${i.mhz}MHz\n` +
                   `flash ${i.flashMB}MB   psram ${i.psramMB}MB   lvgl ${i.lvgl}   qjs ${i.quickjs}`} />
      <label align="top-right" x={-2} y={34} font={16} color={CYAN} text={fps + " fps"} />
      <label align="bottom-left" x={2} y={-34} font={14} text="backlight" />
      <slider w="90%" h={14} align="bottom-mid" y={-8} range={[5, 100]} value={80}
              onChange={e => sys.backlight(e.value)} />
    </tab>
  );
}

// ---------------------------------------------------------------- the dashboard

function Dashboard() {
  const chart = useRef(null);
  const [live, setLive] = useState({
    heap: "heap --", load: 0, uptime: "up 0s", battery: "", fps: 0,
  });

  // One timer for every readout, as the original has. Each pass writes one
  // object, so one state write re-renders the tree once; useState's bail-out
  // means a value that has not moved costs nothing further down.
  useInterval(() => {
    const h = sys.heap();
    const fps = sys.fps();
    const up = (sys.uptime() / 1000) | 0;
    const bat = sys.battery();

    chart.current.push((h.internal / 1024) | 0);

    setLive({
      heap: `heap ${(h.internal / 1024) | 0} kB  psram ${(h.psram / 1024) | 0} kB`,
      // Render throughput as a fraction of a 30 flush/s target, same as the C demo.
      load: Math.min(100, ((fps * 100) / 30) | 0),
      uptime: `up ${(up / 60) | 0}m ${up % 60}s`,
      battery: bat === null ? "bat n/a" : bat.toFixed(2) + "V",
      fps,
    });
  }, 500);

  return (
    <tabview bar={BAR_H} bg="#0A1520">
      <VitalsTab heap={live.heap} load={live.load} uptime={live.uptime}
                 battery={live.battery} chartRef={chart} />
      <TouchTab />
      <WifiTab />
      <SystemTab fps={live.fps} />
    </tabview>
  );
}

lv.screen().set({ bg: "#0A1520" });
render(<Dashboard />);

console.log("vitals: dashboard up (4 tabs)");
