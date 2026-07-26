// weather.js — one glanceable screen, deliberately the opposite of the tabbed
// dashboard: no navigation, no chrome, one number you can read across a room.
//
// Shows what the whole stack is for. Config comes off the card, the reading
// comes over the network, and the last good value is cached so the screen says
// something useful the moment it opens, before any request finishes.
//
// Location comes from /config/weather.json:
//   { "name": "Berlin", "lat": 52.52, "lon": 13.41 }

const CONFIG = "/config/weather.json";
const CACHE = "/cache/weather.json";
const REFRESH_MS = 10 * 60 * 1000;
// The radio is usually still associating when this script starts — more so on a
// board pinned to it, which reaches the first update seconds after power-on. So
// "not connected" at boot means "not yet", and is worth asking about again
// shortly rather than sitting on a stale screen for a full refresh interval.
const RETRY_MS = 3000;

const place = (() => {
  const fallback = { name: "Berlin", lat: 52.52, lon: 13.41 };
  try {
    const raw = fs.available() && fs.read(CONFIG);
    return raw ? Object.assign(fallback, JSON.parse(raw)) : fallback;
  } catch (e) {
    console.log("weather: bad config, using default —", e.message);
    return fallback;
  }
})();

// WMO codes, condensed to what fits on one line.
const CONDITIONS = {
  0: "Clear", 1: "Mainly clear", 2: "Partly cloudy", 3: "Overcast",
  45: "Fog", 48: "Rime fog", 51: "Light drizzle", 53: "Drizzle", 55: "Heavy drizzle",
  61: "Light rain", 63: "Rain", 65: "Heavy rain", 66: "Freezing rain", 67: "Freezing rain",
  71: "Light snow", 73: "Snow", 75: "Heavy snow", 77: "Snow grains",
  80: "Showers", 81: "Showers", 82: "Violent showers",
  85: "Snow showers", 86: "Snow showers", 95: "Thunderstorm",
  96: "Thunderstorm", 99: "Thunderstorm",
};

// ---------------------------------------------------------------- layout

const scr = lv.screen().set({ bg: "#0E1A24", pad: 0, scroll: false });

lv.label(scr, { align: "top-left", x: 12, y: 10, font: 16, color: "#7FA8C4", text: place.name });
const state = lv.label(scr, { align: "top-right", x: -46, y: 12, font: 14, color: "#5A7285", text: "" });

const temp = lv.label(scr, { align: "left-mid", x: 12, y: 2, font: 40, color: "#FFFFFF", text: "--°" });
const condition = lv.label(scr, { align: "bottom-left", x: 14, y: -34, font: 20, color: "#C8D8E4", text: "" });
const wind = lv.label(scr, { align: "bottom-left", x: 14, y: -10, font: 14, color: "#5A7285", text: "" });

// ---------------------------------------------------------------- rendering

let lastUpdate = 0;
let waitingForWifi = null;  // the retry timer, only alive while offline

function render(data, source) {
  temp.set({ text: `${Math.round(data.temp)}°` });
  condition.set({ text: CONDITIONS[data.code] || `Code ${data.code}` });
  wind.set({ text: `wind ${Math.round(data.wind)} km/h` });
  state.set({ text: source, color: source === "live" ? "#4CAF50" : "#8A6D3B" });
}

function showCached() {
  try {
    const raw = fs.available() && fs.read(CACHE);
    if (raw) render(JSON.parse(raw), "cached");
  } catch (e) {
    // A corrupt cache is not worth failing over; the fetch will replace it.
  }
}

function cache(data) {
  if (!fs.available()) return;
  if (!fs.isDir("/cache")) fs.mkdir("/cache");
  fs.write(CACHE, JSON.stringify(data));
}

// ---------------------------------------------------------------- fetching

async function update() {
  const net = wifi.status();
  if (!net.connected) {
    state.set({ text: "offline", color: "#FF8A65" });
    // Keep asking until the radio comes up, whether that is eight seconds after
    // boot or an hour after the router came back. Only wifi.status() is polled,
    // which is a local driver read, not a request.
    if (!waitingForWifi) waitingForWifi = lv.timer(RETRY_MS, update);
    return;
  }
  // Safe to stop from in here: this call may be the retry timer's own callback,
  // and the binding layer holds its references for the duration of the call.
  if (waitingForWifi) {
    waitingForWifi.stop();
    waitingForWifi = null;
  }

  state.set({ text: "updating", color: "#5A7285" });
  const url = `https://api.open-meteo.com/v1/forecast?latitude=${place.lat}` +
              `&longitude=${place.lon}&current=temperature_2m,weather_code,wind_speed_10m`;
  try {
    const res = await fetch(url);
    if (!res.ok) throw new Error(`http ${res.status}`);
    const cur = JSON.parse(res.body).current;
    const data = { temp: cur.temperature_2m, code: cur.weather_code, wind: cur.wind_speed_10m };
    render(data, "live");
    cache(data);
    lastUpdate = sys.uptime();
  } catch (e) {
    console.log("weather: update failed —", e.message);
    state.set({ text: "failed", color: "#FF5252" });
  }
}

// Show whatever we knew last time immediately, then go find out for real.
showCached();
update();
lv.timer(REFRESH_MS, update);

// Tap anywhere to refresh now — the only interaction this app has.
scr.set({ clickable: true }).on("click", () => {
  if (sys.uptime() - lastUpdate > 5000) update();
});

console.log(`weather: ${place.name} (${place.lat}, ${place.lon})`);
