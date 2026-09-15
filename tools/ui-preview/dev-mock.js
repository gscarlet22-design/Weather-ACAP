/* Dev-only: fakes weather_acap.cgi so preview.html renders with data.
 * NOT part of the ACAP build — index.html never loads this. */
(function () {
  "use strict";
  var CFG = {
    app_version: "2.4.1", config_ok: true,
    zip: "78702", lat_override: "", lon_override: "",
    weather_provider: "auto", poll_interval: "300",
    nws_user_agent: "WeatherACAP/2.4.1 (ops@example.com)",
    alert_map: "Tornado Warning:20:1|Severe Thunderstorm Warning:21:1|Flash Flood Warning:22:1|Tornado Watch:23:0|Flood Warning:26:0",
    threshold_map: "TempF:>:95:10:1|WindMph:>:40:12:1",
    alert_cooldown_min: "10", threshold_cooldown_min: "10",
    overlay_enabled: "yes", overlay_position: "topLeft", overlay_max_alerts: "3",
    overlay_template: "Temp: {temp}F | Conditions: {cond} | Wind: {wind}mph | Dir: {dir} | Hum: {hum}%",
    overlay_alert_template: "[ALERT: {alert_type}] ",
    snapshot_enabled: "yes", snapshot_resolution: "1280x720", snapshot_save_dir: "",
    snapshot_on_activate: "yes", snapshot_on_clear: "no", snapshot_max_count: "50",
    multicam_enabled: "yes", multicam_resolution: "1280x720",
    multicam_list: "192.168.1.42:root:__SET__:Yard south|192.168.1.57:root:__SET__:Gate",
    lightning_enabled: "yes", lightning_port: "35", lightning_min_risk: "3", lightning_poll_mult: "6",
    display_alert_enabled: "yes", display_alert_duration: "30",
    display_alert_text_color: "#FFFFFF", display_alert_bg_warning: "#CC0000", display_alert_bg_watch: "#FF8800",
    strobe_alert_enabled: "yes", strobe_alert_duration: "30",
    d4200_enabled: "no", d4200_host: "", d4200_user: "root", d4200_pass: "",
    d4200_warning_profile: "emergency", d4200_watch_profile: "caution",
    audio_alert_enabled: "no", audio_clip_warning: "-1", audio_clip_watch: "-1",
    system_enabled: "yes", vapix_user: "root", vapix_pass: "__SET__",
    webhook_enabled: "yes", webhook_url: "https://hooks.slack.com/services/T0/B0/xxxx",
    webhook_on_alerts_only: "yes", webhook_template: "",
    mqtt_enabled: "no", mqtt_broker_url: "", mqtt_topic: "weather/camera/alerts",
    mqtt_user: "", mqtt_pass: "", mqtt_on_alert_only: "yes", mqtt_retain: "no",
    email_enabled: "no", email_smtp_url: "", email_from: "", email_to: "",
    email_user: "", email_pass: "", email_on_clear: "no",
    mock_mode: "no", axis_events_enabled: "yes", json_logging: "no"
  };
  var now = new Date();
  function iso(minsAgo) { return new Date(now - minsAgo * 60000).toISOString(); }
  var HIST = [
    { ts: iso(340), action: "activated", event: "TempF > 95" },
    { ts: iso(210), action: "cleared",   event: "TempF > 95" },
    { ts: iso(96),  action: "activated", event: "SPC Lightning Risk (SLGT)" },
    { ts: iso(21),  action: "activated", event: "Flash Flood Warning" }
  ];
  var PTS = [];
  for (var i = 24; i >= 0; i--) {
    PTS.push({ ts: iso(i * 15),
      temp_f: 72 + Math.round(14 * Math.sin((24 - i) / 4)),
      wind_mph: 12 + Math.round(9 * Math.sin((24 - i) / 3 + 1)),
      humidity_pct: 58 + Math.round(16 * Math.cos((24 - i) / 5)) });
  }
  var RESP = {
    config: CFG,
    status: { ok: true, snapshot: {
      conditions: { valid: true, temp_f: 72.4, description: "Partly cloudy",
        wind_speed_mph: 12, wind_dir_str: "W", wind_dir_deg: 270,
        humidity_pct: 58, provider: "NWS" },
      alerts: [ { event: "Flash Flood Warning", headline: "Flash Flood Warning until 6:00 PM CDT \u2014 3.1 in of rain expected" } ],
      any_alert_active: true, alerts_fetch_ok: true,
      lat: 30.2672, lon: -97.7431, last_poll: "14:32:06",
      lightning_risk: "SLGT", lightning_risk_level: 3,
      overlay_id: 4, video_present: true, overlay_limit_reached: false, last_error: "" } },
    ports: { ok: true, max_ports: 32 },
    history: { ok: true, entries: HIST },
    cond_history: { ok: true, points: PTS },
    preview_overlay: { ok: true, text: "[ALERT: Flash Flood Warning] Temp: 72F | Conditions: Partly cloudy | Wind: 12mph | Dir: W | Hum: 58%" },
    device: { ok: true, raw: "brand=AXIS\nProdNbr=Q1656-LE\nProdType=Network Camera\nSerialNumber=B8A44F1C2D90\nVersion=11.11.61\nArchitecture=aarch64\nSoc=Axis Artpec-8" },
    clip_list: { ok: true, clips: [ { id: 0, name: "siren-loop" }, { id: 1, name: "take-shelter" }, { id: 2, name: "chime" } ] },
    snapshot_list: { ok: true, save_dir: "/var/spool/storage/SD_DISK/weather_acap", snapshots: [] },
    save: { ok: true, saved: 12, clamped: 0 }
  };
  var realFetch = window.fetch;
  window.fetch = function (url, init) {
    var u = String(url);
    if (u.indexOf("weather_acap.cgi") < 0) return realFetch.apply(this, arguments);
    var action = (u.match(/action=([^&]+)/) || [])[1] || "";
    var body = RESP[action] || { ok: true, http_code: 200, msg: "Mock OK \u2014 no camera attached" };
    return Promise.resolve(new Response(JSON.stringify(body), {
      status: 200, headers: { "Content-Type": "application/json" }
    }));
  };
})();
