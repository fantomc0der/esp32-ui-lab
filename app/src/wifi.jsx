// wifi.jsx — join a network from the touchscreen, no recompiling and no
// credentials in any file.
//
// Four screens: status, a scan list, a password prompt with the on-screen
// keyboard, and connecting. wifi.save() stores what you type in NVS, so the
// board rejoins by itself on every later boot. Nothing here can read the
// password back; the binding layer only accepts it.
//
// This is the app the component model was worth porting for, because three
// pieces of bookkeeping in the imperative original are gone rather than
// rewritten:
//
//   next(fn), the lv.timer(20) that moved a screen rebuild just past the end of
//   the click that asked for it. Used six times. A state write is already
//   deferred out of dispatch, so switching screens is now just setScreen().
//
//   reset(), which called scr.clean() and stopped whatever timer the outgoing
//   screen had left running. Unmounting does both: widgets go with the subtree,
//   and useInterval stops with its component.
//
//   screenTimer, the module-level handle that had to be nulled in three places
//   so a later stop() would not throw through a stale reference.

const S = lv.size();
// The firmware draws a 34px button in the bottom-right of every screen. Anything
// tappable that reaches into that corner is partly unreachable, so the keyboard,
// the buttons and the scan list all stop short of it.
const CORNER = 40;

function Header({ text }) {
  return <label align="top-left" x={10} y={8} font={20} color="#F0F4F8" text={text} />;
}

// ---------------------------------------------------------------- status

function Status({ go }) {
  // Buttons are decided once, from the state at mount; only the text above them
  // refreshes, so nothing moves under your finger. That is why this is its own
  // state rather than read from `st` below — a Forget button that appeared
  // mid-poll would shift the row as you reached for it.
  const [initial] = useState(() => wifi.status());
  const [st, setSt] = useState(initial);
  useInterval(() => setSt(wifi.status()), 1500);

  const BTN_W = Math.min(150, ((S.w - CORNER - 30) / 2) | 0);

  let line1, line2;
  if (st.connected) {
    line1 = [st.ssid, "#4CAF50"];
    line2 = [`${st.ip}    ${st.rssi} dBm`, "#8A9BAB"];
  } else if (st.saved && st.error) {
    // The case that had you guessing: credentials are stored but wrong. Say
    // which, and say what to do about it.
    line1 = [st.error, "#FF5252"];
    line2 = [st.error === "wrong password"
               ? "Tap Forget, then Scan to retype it."
               : `Retrying (${st.attempts})... or Forget and pick again.`,
             "#C8D8E4"];
  } else if (st.saved) {
    line1 = ["Connecting...", "#FFB74D"];
    line2 = ["", "#8A9BAB"];
  } else {
    line1 = ["Not set up", "#8A9BAB"];
    line2 = ["Tap Scan to choose a network.", "#64798C"];
  }

  return (
    <>
      <Header text="Wi-Fi" />
      <label align="top-left" x={10} y={40} font={16} text={line1[0]} color={line1[1]} />
      <label align="top-left" x={10} y={64} font={14} color={line2[1]} text={line2[0]} />
      {/* Two buttons share the width left of the corner button, whatever that is. */}
      <button w={BTN_W} h={36} align="bottom-left" x={10} y={-10} text="Scan"
              onClick={() => go({ name: "scan" })} />
      {initial.saved && (
        <button w={BTN_W} h={36} align="bottom-left" x={20 + BTN_W} y={-10} text="Forget"
                onClick={() => { wifi.forget(); go({ name: "status" }); }} />
      )}
    </>
  );
}

// ---------------------------------------------------------------- scan list

function Scan({ go }) {
  const [rows, setRows] = useState(null);   // null while scanning
  const [failed, setFailed] = useState(false);

  useEffect(() => {
    wifi.scan(nets => {
      if (!nets) { setFailed(true); return; }
      // Strongest first, and skip the unnamed ones a scan usually turns up.
      const seen = new Set();
      setRows(nets.filter(n => n.ssid && !seen.has(n.ssid) && seen.add(n.ssid))
                  .sort((a, b) => b.rssi - a.rssi));
    });
  }, []);

  const note = failed ? "scan failed" : rows ? `${rows.length} found` : "scanning...";

  return (
    <>
      <Header text="Choose a network" />
      <label align="top-right" x={-12} y={14} font={14} color="#64798C" text={note} />
      {/* Everything below the header, which is one 20px line plus its margin.
          Anchored bottom-left and stopping short of the corner button, so the
          last row is not partly underneath it — a row you can see but only half
          tap is worse than a slightly narrower list. */}
      <list w={S.w - CORNER - 10} h={S.h - 44} align="bottom-left" x={10} y={-5}
            bg="#101E2C" border={0} radius={8}>
        {(rows || []).map(n => (
          <row key={n.ssid}
               text={`${n.ssid}   ${n.rssi > -60 ? "|||" : n.rssi > -75 ? "||" : "|"}${n.open ? "" : " *"}`}
               onClick={() => go(n.open ? { name: "connecting", ssid: n.ssid, password: "" }
                                        : { name: "password", ssid: n.ssid })} />
        ))}
      </list>
    </>
  );
}

// ---------------------------------------------------------------- password

function Password({ ssid, go }) {
  const [masked, setMasked] = useState(true);
  const field = useRef(null);
  const keyboard = useRef(null);

  // Routing the keyboard at a field is a call, not a prop — LVGL wires the key
  // handling internally and a script never sees a keystroke. Both widgets exist
  // by the time an effect runs, which is exactly what effects are for.
  useEffect(() => { keyboard.current.target(field.current); }, []);

  // The field takes the row, less the reveal button beside it.
  const REVEAL_W = 60;

  return (
    <>
      <label align="top-left" x={10} y={4} font={16} color="#F0F4F8" text={ssid} />
      <textarea ref={field}
                w={S.w - REVEAL_W - 20} h={34} align="top-left" x={10} y={26}
                placeholder="password" password={masked} oneLine maxLength={63} />
      {/* Typing a long password blind on a small panel is how you end up
          entering it three times. LVGL reveals each character for 1.5 s as it is
          typed, which helps while typing but not when checking what you already
          have, so this unmasks the whole field. */}
      <button w={REVEAL_W} h={34} align="top-right" x={-10} y={26}
              text={masked ? "Show" : "Hide"} onClick={() => setMasked(!masked)} />
      {/* The keyboard stops short of the right edge so it does not sit underneath
          the firmware's corner button, and takes everything below the field: a
          taller panel gets taller keys rather than a gap under them. */}
      <keyboard ref={keyboard} w={S.w - CORNER} h={S.h - 68} align="bottom-left" x={0} y={0}
                onReady={() => go({ name: "connecting", ssid, password: field.current.value() })}
                onCancel={() => go({ name: "scan" })} />
    </>
  );
}

// ---------------------------------------------------------------- connecting

function Connecting({ ssid, password, go }) {
  const [waited, setWaited] = useState(0);
  const [failure, setFailure] = useState(null);   // null while still trying

  useEffect(() => { wifi.save(ssid, password); }, []);

  // Passing null for the interval is what stops the poll on a terminal result,
  // so there is no timer handle to stop and nothing to null out afterwards.
  useInterval(() => {
    const st = wifi.status();
    if (st.connected) {
      go({ name: "status" });
    } else if (st.error === "wrong password") {
      // No point waiting out the timeout for something that will not improve.
      setFailure({ text: "Wrong password", retry: "password" });
    } else if (waited + 500 >= 15000) {
      setFailure({ text: st.error || "Could not connect", retry: "scan" });
    } else {
      setWaited(waited + 500);
    }
  }, failure ? null : 500);

  return (
    <>
      <Header text="Connecting" />
      <label align="center" font={16}
             color={failure ? "#FF5252" : "#8A9BAB"}
             text={failure ? failure.text : `${ssid}\n${waited / 1000}s`} />
      {failure && (
        <button w={150} h={36} align="bottom-mid" y={-10} text="Try again"
                onClick={() => go(failure.retry === "password"
                  ? { name: "password", ssid }
                  : { name: "scan" })} />
      )}
    </>
  );
}

// ---------------------------------------------------------------- the app

function Wifi() {
  const [screen, setScreen] = useState({ name: "status" });

  // Switching screens from inside a click handler, with no deferral: the render
  // this schedules cannot run until lv_timer_handler() has returned, so the
  // widget being dispatched to is still alive for the rest of the event.
  const go = setScreen;

  if (screen.name === "scan") return <Scan go={go} />;
  if (screen.name === "password") return <Password ssid={screen.ssid} go={go} />;
  if (screen.name === "connecting") {
    return <Connecting ssid={screen.ssid} password={screen.password} go={go} />;
  }
  return <Status go={go} />;
}

lv.screen().set({ bg: "#0B1622", pad: 0, scroll: false });
render(<Wifi />);

console.log("wifi: setup app ready");
