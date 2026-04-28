/* ===========================================================================
 * Weather ACAP — app.js
 * Tab routing, config load/save, live polling, alert table CRUD,
 * overlay preview, diagnostic actions, export/import, toast notifications.
 * =========================================================================*/
(function () {
  "use strict";

  /* ── Constants ──────────────────────────────────────────────────────────── */
  var CGI = "weather_acap.cgi";
  var POLL_MS = 5000;
  var TOAST_MS = 4000;

  /* ── Utility ────────────────────────────────────────────────────────────── */
  function $(id) { return document.getElementById(id); }
  function $$(sel, ctx) { return Array.prototype.slice.call((ctx || document).querySelectorAll(sel)); }

  function cgi(action, opts) {
    opts = opts || {};
    var url = CGI + "?action=" + encodeURIComponent(action);
    if (opts.qs) url += "&" + opts.qs;
    var init = { method: opts.method || "GET" };
    if (opts.body !== undefined) {
      init.method = "POST";
      init.body = opts.body;
      if (typeof opts.body === "string" && !opts.json)
        init.headers = { "Content-Type": "application/x-www-form-urlencoded" };
      if (opts.json)
        init.headers = { "Content-Type": "application/json" };
    }
    return fetch(url, init).then(function (r) {
      if (!r.ok) {
        return r.text().then(function (body) {
          var preview = body.substring(0, 80).replace(/</g, "&lt;");
          throw new Error("HTTP " + r.status + " from " + url + " — " + preview);
        });
      }
      var ct = r.headers.get("content-type") || "";
      if (ct.indexOf("json") < 0) {
        return r.text().then(function (body) {
          var preview = body.substring(0, 80).replace(/</g, "&lt;");
          throw new Error("Expected JSON but got " + ct + " from " + url + " — " + preview);
        });
      }
      return r.json();
    });
  }

  function encField(key, val) {
    return encodeURIComponent(key) + "=" + encodeURIComponent(val || "");
  }

  function escHtml(s) {
    if (!s) return "";
    return s.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;")
            .replace(/"/g, "&quot;");
  }

  /* ── Toast ──────────────────────────────────────────────────────────────── */
  function toast(msg, type) {
    var el = document.createElement("div");
    el.className = "toast toast-" + (type || "ok");
    el.textContent = msg;
    $("toast-container").appendChild(el);
    setTimeout(function () {
      el.classList.add("toast-out");
      setTimeout(function () { el.remove(); }, 350);
    }, TOAST_MS);
  }

  /* ── Tabs ────────────────────────────────────────────────────────────────── */
  function initTabs() {
    $$(".tab-btn").forEach(function (btn) {
      btn.addEventListener("click", function () {
        var tab = btn.getAttribute("data-tab");
        $$(".tab-btn").forEach(function (b) { b.classList.remove("active"); });
        $$(".tab-pane").forEach(function (p) { p.classList.remove("active"); });
        btn.classList.add("active");
        var pane = $("tab-" + tab);
        if (pane) pane.classList.add("active");
        /* lazy-load some panes */
        if (tab === "snapshots")   refreshSnapshots();
        if (tab === "diagnostics") refreshDeviceInfo();
        if (tab === "diagnostics") refreshHistory();
        if (tab === "dashboard")   refreshSparklines();
      });
    });
  }

  /* ── Config load ────────────────────────────────────────────────────────── */
  var currentConfig = {};

  function loadConfig() {
    return cgi("config").then(function (cfg) {
      currentConfig = cfg;
      /* Location tab */
      $("f-zip").value            = cfg.zip || "";
      $("f-lat").value            = cfg.lat_override || "";
      $("f-lon").value            = cfg.lon_override || "";
      $("f-provider").value       = cfg.weather_provider || "auto";
      $("f-interval").value       = cfg.poll_interval || "300";
      $("f-ua").value             = cfg.nws_user_agent || "";
      /* Overlay */
      $("f-overlay-enabled").checked   = (cfg.overlay_enabled || "").toLowerCase() === "yes";
      $("f-overlay-position").value    = cfg.overlay_position || "topLeft";
      $("f-overlay-max").value         = cfg.overlay_max_alerts || "3";
      $("f-overlay-template").value    = cfg.overlay_template || "";
      $("f-overlay-alert-template").value = cfg.overlay_alert_template || "";
      /* Advanced */
      $("f-system-enabled").checked    = (cfg.system_enabled || "").toLowerCase() === "yes";
      $("f-vapix-user").value          = cfg.vapix_user || "";
      $("f-vapix-pass").value          = "";
      $("f-vapix-pass").placeholder    = cfg.vapix_pass === "__SET__" ? "(unchanged)" : "(not set)";
      $("f-webhook-enabled").checked   = (cfg.webhook_enabled || "").toLowerCase() === "yes";
      $("f-webhook-url").value         = cfg.webhook_url || "";
      $("f-webhook-alerts-only").checked = (cfg.webhook_on_alerts_only || "").toLowerCase() === "yes";
      $("f-mock-mode").checked         = (cfg.mock_mode || "").toLowerCase() === "yes";
      /* MQTT */
      $("f-mqtt-enabled").checked       = (cfg.mqtt_enabled || "").toLowerCase() === "yes";
      $("f-mqtt-broker").value          = cfg.mqtt_broker_url || "";
      $("f-mqtt-topic").value           = cfg.mqtt_topic || "weather/camera/alerts";
      $("f-mqtt-user").value            = cfg.mqtt_user || "";
      $("f-mqtt-pass").value            = "";
      $("f-mqtt-pass").placeholder      = cfg.mqtt_pass === "__SET__" ? "(unchanged)" : "(not set)";
      $("f-mqtt-on-alert-only").checked = (cfg.mqtt_on_alert_only !== undefined
                                           ? cfg.mqtt_on_alert_only : "yes").toLowerCase() === "yes";
      $("f-mqtt-retain").checked        = (cfg.mqtt_retain || "").toLowerCase() === "yes";
      /* Email */
      $("f-email-enabled").checked   = (cfg.email_enabled || "").toLowerCase() === "yes";
      $("f-email-smtp").value        = cfg.email_smtp_url || "";
      $("f-email-from").value        = cfg.email_from || "";
      $("f-email-to").value          = cfg.email_to || "";
      $("f-email-user").value        = cfg.email_user || "";
      $("f-email-pass").value        = "";
      $("f-email-pass").placeholder  = cfg.email_pass === "__SET__" ? "(unchanged)" : "(not set)";
      $("f-email-on-clear").checked  = (cfg.email_on_clear || "").toLowerCase() === "yes";
      /* Snapshot */
      $("f-snapshot-enabled").checked     = (cfg.snapshot_enabled || "").toLowerCase() === "yes";
      $("f-snapshot-resolution").value    = cfg.snapshot_resolution || "1280x720";
      $("f-snapshot-save-dir").value      = cfg.snapshot_save_dir || "";
      $("f-snapshot-on-activate").checked = (cfg.snapshot_on_activate || "yes").toLowerCase() === "yes";
      $("f-snapshot-on-clear").checked    = (cfg.snapshot_on_clear || "").toLowerCase() === "yes";
      $("f-snapshot-max-count").value     = cfg.snapshot_max_count !== undefined ? cfg.snapshot_max_count : "50";
      /* Alert table */
      buildAlertTable(cfg.alert_map || "");
      /* Threshold table (Sprint 5) */
      buildThresholdTable(cfg.threshold_map || "");
    });
  }

  /* ── Config save ────────────────────────────────────────────────────────── */
  function gatherFields(section) {
    var pairs = [];
    if (section === "location") {
      pairs.push(encField("zip",              $("f-zip").value.trim()));
      pairs.push(encField("lat_override",     $("f-lat").value.trim()));
      pairs.push(encField("lon_override",     $("f-lon").value.trim()));
      pairs.push(encField("weather_provider", $("f-provider").value));
      pairs.push(encField("poll_interval",    $("f-interval").value));
      pairs.push(encField("nws_user_agent",   $("f-ua").value.trim()));
    } else if (section === "alerts") {
      pairs.push(encField("alert_map", serializeAlertMap()));
      pairs.push(encField("threshold_map", serializeThresholdMap()));
    } else if (section === "overlay") {
      pairs.push(encField("overlay_enabled",        $("f-overlay-enabled").checked ? "yes" : "no"));
      pairs.push(encField("overlay_position",       $("f-overlay-position").value));
      pairs.push(encField("overlay_max_alerts",     $("f-overlay-max").value));
      pairs.push(encField("overlay_template",       $("f-overlay-template").value));
      pairs.push(encField("overlay_alert_template", $("f-overlay-alert-template").value));
    } else if (section === "snapshots") {
      pairs.push(encField("snapshot_enabled",     $("f-snapshot-enabled").checked ? "yes" : "no"));
      pairs.push(encField("snapshot_resolution",  $("f-snapshot-resolution").value));
      pairs.push(encField("snapshot_save_dir",    $("f-snapshot-save-dir").value.trim()));
      pairs.push(encField("snapshot_on_activate", $("f-snapshot-on-activate").checked ? "yes" : "no"));
      pairs.push(encField("snapshot_on_clear",    $("f-snapshot-on-clear").checked ? "yes" : "no"));
      pairs.push(encField("snapshot_max_count",   $("f-snapshot-max-count").value || "50"));
    } else if (section === "advanced") {
      pairs.push(encField("system_enabled",          $("f-system-enabled").checked ? "yes" : "no"));
      pairs.push(encField("vapix_user",              $("f-vapix-user").value.trim()));
      var pw = $("f-vapix-pass").value;
      pairs.push(encField("vapix_pass",              pw || "__SET__"));
      pairs.push(encField("webhook_enabled",         $("f-webhook-enabled").checked ? "yes" : "no"));
      pairs.push(encField("webhook_url",             $("f-webhook-url").value.trim()));
      pairs.push(encField("webhook_on_alerts_only",  $("f-webhook-alerts-only").checked ? "yes" : "no"));
      /* MQTT */
      pairs.push(encField("mqtt_enabled",       $("f-mqtt-enabled").checked ? "yes" : "no"));
      pairs.push(encField("mqtt_broker_url",    $("f-mqtt-broker").value.trim()));
      pairs.push(encField("mqtt_topic",         $("f-mqtt-topic").value.trim()));
      pairs.push(encField("mqtt_user",          $("f-mqtt-user").value.trim()));
      var mqpw = $("f-mqtt-pass").value;
      pairs.push(encField("mqtt_pass",          mqpw || "__SET__"));
      pairs.push(encField("mqtt_on_alert_only", $("f-mqtt-on-alert-only").checked ? "yes" : "no"));
      pairs.push(encField("mqtt_retain",        $("f-mqtt-retain").checked ? "yes" : "no"));
      /* Email */
      pairs.push(encField("email_enabled",   $("f-email-enabled").checked ? "yes" : "no"));
      pairs.push(encField("email_smtp_url",  $("f-email-smtp").value.trim()));
      pairs.push(encField("email_from",      $("f-email-from").value.trim()));
      pairs.push(encField("email_to",        $("f-email-to").value.trim()));
      pairs.push(encField("email_user",      $("f-email-user").value.trim()));
      var empw = $("f-email-pass").value;
      pairs.push(encField("email_pass",      empw || "__SET__"));
      pairs.push(encField("email_on_clear",  $("f-email-on-clear").checked ? "yes" : "no"));
      pairs.push(encField("mock_mode",       $("f-mock-mode").checked ? "yes" : "no"));
    }
    return pairs.join("&");
  }

  function initSaveButtons() {
    $$("[data-save]").forEach(function (btn) {
      var originalText = btn.textContent;
      btn.addEventListener("click", function () {
        var section = btn.getAttribute("data-save");
        btn.disabled = true;
        btn.textContent = "Saving\u2026";
        cgi("save", { body: gatherFields(section) })
          .then(function (r) {
            if (r.ok) toast("Settings saved (" + r.saved + " fields)", "ok");
            else toast("Save had errors: " + (r.errors || 0) + " fields failed", "error");
          })
          .catch(function (e) { toast("Save failed: " + e.message, "error"); })
          .then(function () {
            btn.disabled = false;
            btn.textContent = originalText;
            loadConfig(); /* refresh */
          });
      });
    });
  }

  /* ── Alert table ────────────────────────────────────────────────────────── */
  var maxPorts = 32;

  function parseAlertMap(mapStr) {
    if (!mapStr) return [];
    return mapStr.split("|").filter(Boolean).map(function (seg) {
      var p = seg.split(":");
      return { type: p[0] || "", port: parseInt(p[1], 10) || 20, enabled: p[2] !== "0" };
    });
  }

  function serializeAlertMap() {
    var rows = [];
    $$(".alert-row").forEach(function (tr) {
      var type    = tr.querySelector(".ar-type").value.trim();
      var port    = tr.querySelector(".ar-port").value;
      var enabled = tr.querySelector(".ar-on").checked;
      if (type) rows.push(type + ":" + port + ":" + (enabled ? "1" : "0"));
    });
    return rows.join("|");
  }

  function buildAlertTable(mapStr) {
    var rules = parseAlertMap(mapStr);
    var tbody = $("alerts-tbody");
    tbody.innerHTML = "";
    rules.forEach(function (r) { addAlertRow(r.type, r.port, r.enabled); });
  }

  function addAlertRow(type, port, enabled) {
    var tbody = $("alerts-tbody");
    var tr = document.createElement("tr");
    tr.className = "alert-row";
    tr.innerHTML =
      '<td><input type="checkbox" class="ar-on"' + (enabled !== false ? " checked" : "") + '></td>' +
      '<td><input type="text" class="ar-type" value="' + escHtml(type || "") + '" placeholder="e.g. Tornado Warning"></td>' +
      '<td><input type="number" class="ar-port" min="1" max="' + maxPorts + '" value="' + (port || 20) + '"></td>' +
      '<td><button class="btn btn-ghost btn-small ar-test" type="button">Fire</button>' +
      '<button class="btn btn-ghost btn-small ar-clear" type="button">Clear</button>' +
      '<span class="row-test-result"></span></td>' +
      '<td><button class="btn row-del" type="button" title="Remove">&times;</button></td>';
    tbody.appendChild(tr);

    tr.querySelector(".row-del").addEventListener("click", function () { tr.remove(); });
    tr.querySelector(".ar-test").addEventListener("click", function () {
      var p = tr.querySelector(".ar-port").value;
      var res = tr.querySelector(".row-test-result");
      res.textContent = "\u2026";
      res.className = "row-test-result busy";
      cgi("fire_port", { method: "POST", qs: "port=" + p, body: "" })
        .then(function (r) {
          res.textContent = r.ok ? "Fired " + p : "Err " + r.http_code;
          res.className = "row-test-result " + (r.ok ? "ok" : "bad");
        })
        .catch(function () {
          res.textContent = "fail";
          res.className = "row-test-result bad";
        });
    });
    tr.querySelector(".ar-clear").addEventListener("click", function () {
      var p = tr.querySelector(".ar-port").value;
      var res = tr.querySelector(".row-test-result");
      res.textContent = "\u2026";
      res.className = "row-test-result busy";
      cgi("clear_port", { method: "POST", qs: "port=" + p, body: "" })
        .then(function (r) {
          res.textContent = r.ok ? "Cleared " + p : "Err " + r.http_code;
          res.className = "row-test-result " + (r.ok ? "ok" : "bad");
        })
        .catch(function () {
          res.textContent = "fail";
          res.className = "row-test-result bad";
        });
    });
  }

  function initAlertButtons() {
    $("alerts-add").addEventListener("click", function () {
      /* Find next unused port */
      var used = {};
      $$(".ar-port").forEach(function (inp) { used[inp.value] = true; });
      var next = 20;
      while (used[next] && next <= maxPorts) next++;
      addAlertRow("", next, true);
    });

    $("alerts-auto-port").addEventListener("click", function () {
      var start = 20;
      $$(".alert-row").forEach(function (tr, i) {
        tr.querySelector(".ar-port").value = start + i;
      });
      toast("Ports auto-assigned starting at " + start, "ok");
    });
  }

  /* ── Threshold table (Sprint 5) ────────────────────────────────────────── */
  var THRESH_CONDITIONS = ["TempF", "WindMph", "HumidityPct", "WindDirDeg"];
  var THRESH_COND_LABELS = {
    TempF:        "Temperature (\u00B0F)",
    WindMph:      "Wind speed (mph)",
    HumidityPct:  "Humidity (%)",
    WindDirDeg:   "Wind dir (deg)"
  };
  var THRESH_OPS = [">", "<", ">=", "<="];

  function parseThresholdMap(mapStr) {
    if (!mapStr) return [];
    return mapStr.split("|").filter(Boolean).map(function (seg) {
      var p = seg.split(":");
      if (p.length < 5) return null;
      return {
        condition: p[0],
        op:        p[1],
        value:     parseFloat(p[2]) || 0,
        port:      parseInt(p[3], 10) || 1,
        enabled:   p[4] !== "0"
      };
    }).filter(Boolean);
  }

  function serializeThresholdMap() {
    var rows = [];
    $$(".threshold-row").forEach(function (tr) {
      var cond    = tr.querySelector(".tr-cond").value;
      var op      = tr.querySelector(".tr-op").value;
      var val     = tr.querySelector(".tr-val").value;
      var port    = tr.querySelector(".tr-port").value;
      var enabled = tr.querySelector(".tr-on").checked;
      if (cond && op && val !== "" && port) {
        rows.push(cond + ":" + op + ":" + val + ":" + port + ":" + (enabled ? "1" : "0"));
      }
    });
    return rows.join("|");
  }

  function buildThresholdTable(mapStr) {
    var rules = parseThresholdMap(mapStr);
    var tbody = $("threshold-tbody");
    tbody.innerHTML = "";
    rules.forEach(function (r) {
      addThresholdRow(r.condition, r.op, r.value, r.port, r.enabled);
    });
  }

  function addThresholdRow(condition, op, value, port, enabled) {
    var tbody = $("threshold-tbody");
    var tr = document.createElement("tr");
    tr.className = "threshold-row";

    /* Condition dropdown */
    var condOpts = THRESH_CONDITIONS.map(function (c) {
      return '<option value="' + c + '"' + (c === condition ? " selected" : "") + '>' +
             escHtml(THRESH_COND_LABELS[c] || c) + '</option>';
    }).join("");

    /* Operator dropdown */
    var opOpts = THRESH_OPS.map(function (o) {
      return '<option value="' + escHtml(o) + '"' + (o === op ? " selected" : "") + '>' +
             escHtml(o) + '</option>';
    }).join("");

    tr.innerHTML =
      '<td><input type="checkbox" class="tr-on"' + (enabled !== false ? " checked" : "") + '></td>' +
      '<td><select class="tr-cond">' + condOpts + '</select></td>' +
      '<td><select class="tr-op">' + opOpts + '</select></td>' +
      '<td><input type="number" class="tr-val" value="' + (value !== undefined ? value : "") +
           '" step="any" placeholder="e.g. 90" style="width:80px;"></td>' +
      '<td><input type="number" class="tr-port" min="1" max="' + maxPorts + '" value="' + (port || 1) + '"></td>' +
      '<td><button class="btn btn-ghost btn-small tr-test" type="button">Fire</button>' +
           '<button class="btn btn-ghost btn-small tr-clear" type="button">Clear</button>' +
           '<span class="row-test-result"></span></td>' +
      '<td><button class="btn row-del" type="button" title="Remove">&times;</button></td>';
    tbody.appendChild(tr);

    tr.querySelector(".row-del").addEventListener("click", function () { tr.remove(); });

    tr.querySelector(".tr-test").addEventListener("click", function () {
      var p = tr.querySelector(".tr-port").value;
      var res = tr.querySelector(".row-test-result");
      res.textContent = "\u2026";
      res.className = "row-test-result busy";
      cgi("fire_port", { method: "POST", qs: "port=" + p, body: "" })
        .then(function (r) {
          res.textContent = r.ok ? "Fired " + p : "Err " + r.http_code;
          res.className = "row-test-result " + (r.ok ? "ok" : "bad");
        })
        .catch(function () { res.textContent = "fail"; res.className = "row-test-result bad"; });
    });

    tr.querySelector(".tr-clear").addEventListener("click", function () {
      var p = tr.querySelector(".tr-port").value;
      var res = tr.querySelector(".row-test-result");
      res.textContent = "\u2026";
      res.className = "row-test-result busy";
      cgi("clear_port", { method: "POST", qs: "port=" + p, body: "" })
        .then(function (r) {
          res.textContent = r.ok ? "Cleared " + p : "Err " + r.http_code;
          res.className = "row-test-result " + (r.ok ? "ok" : "bad");
        })
        .catch(function () { res.textContent = "fail"; res.className = "row-test-result bad"; });
    });
  }

  function initThresholdButtons() {
    $("threshold-add").addEventListener("click", function () {
      /* Find next unused port — prefer ports not used by alert table or other threshold rows */
      var used = {};
      $$(".ar-port").forEach(function (inp) { used[inp.value] = true; });
      $$(".tr-port").forEach(function (inp) { used[inp.value] = true; });
      var next = 10;
      while (used[next] && next <= maxPorts) next++;
      addThresholdRow("TempF", ">", "", next, true);
    });
  }

  /* ── Live status polling ────────────────────────────────────────────────── */
  var pollTimer = null;

  function pollStatus() {
    cgi("status").then(function (r) {
      var s = r.snapshot || {};
      var c = s.conditions || {};
      /* Header */
      $("current-temp").textContent = c.valid ? Math.round(c.temp_f) : "--";
      $("current-desc").textContent = c.valid ? c.description : "No data";

      var pill = $("status-pill");
      if (!c.valid) {
        pill.textContent = "No data";
        pill.className = "pill pill-neutral";
      } else if (s.any_alert_active) {
        pill.textContent = "Alert active";
        pill.className = "pill pill-alert";
      } else if (s.last_error && s.last_error !== "") {
        pill.textContent = "Error";
        pill.className = "pill pill-warning";
      } else {
        pill.textContent = "OK";
        pill.className = "pill pill-ok";
      }

      /* Dashboard — conditions */
      $("dash-temp").textContent     = c.valid ? Math.round(c.temp_f) : "--";
      $("dash-desc").textContent     = c.valid ? c.description : "\u2014";
      $("dash-wind").textContent     = c.valid ? (Math.round(c.wind_speed_mph) + " mph " + (c.wind_dir_str || "")) : "\u2014";
      $("dash-hum").textContent      = c.valid ? (c.humidity_pct + "%") : "\u2014";
      $("dash-provider").textContent = c.provider || "\u2014";
      $("dash-coords").textContent   = s.lat ? (s.lat.toFixed(4) + ", " + s.lon.toFixed(4)) : "\u2014";
      $("dash-lastpoll").textContent = s.last_poll || "\u2014";

      /* Dashboard — alerts */
      var al = s.alerts || [];
      var ac = $("alerts-count");
      if (al.length > 0) {
        ac.textContent = al.length;
        ac.classList.remove("hidden");
      } else {
        ac.classList.add("hidden");
      }
      var ul = $("dash-alerts");
      ul.innerHTML = "";
      if (al.length === 0) {
        ul.innerHTML = '<li class="empty">No active alerts for this location.</li>';
      } else {
        al.forEach(function (a) {
          var li = document.createElement("li");
          li.innerHTML = '<span class="event">' + escHtml(a.event) + '</span>' +
                         '<span class="headline">' + escHtml(a.headline) + '</span>';
          ul.appendChild(li);
        });
      }

      /* Dashboard — ports */
      renderPortsGrid(s);

      /* Overlay preview position sync */
      updatePreviewPosition();
    }).catch(function () {
      /* silent — will retry next tick */
    });
  }

  function renderPortsGrid(snap) {
    var grid = $("dash-ports");
    /* Gather current AlertMap from loaded config */
    var rules = parseAlertMap(currentConfig.alert_map || "");
    if (rules.length === 0) {
      grid.innerHTML = '<p class="hint">No alert-port mappings configured yet. Go to the Alerts &amp; Triggers tab to set them up.</p>';
      return;
    }

    /* Determine which alerts are active */
    var activeEvents = {};
    (snap.alerts || []).forEach(function (a) {
      activeEvents[a.event.toLowerCase()] = true;
    });

    var html = "";
    rules.forEach(function (r) {
      var isActive = activeEvents[r.type.toLowerCase()] && r.enabled;
      var cls = "port-tile";
      if (isActive) cls += " active";
      if (!r.enabled) cls += " disabled";
      html += '<div class="' + cls + '">' +
              '<span class="port-num">Port ' + r.port + '</span>' +
              '<span class="port-type">' + escHtml(r.type) + '</span>' +
              '</div>';
    });
    grid.innerHTML = html;
  }

  /* ── Overlay preview ────────────────────────────────────────────────────── */
  function updatePreviewPosition() {
    var pos = $("f-overlay-position").value;
    var el  = $("overlay-preview-text");
    el.className = "overlay-text overlay-pos-" + pos;
  }

  function refreshOverlayPreview() {
    cgi("preview_overlay").then(function (r) {
      $("overlay-preview-text").textContent = r.text || "(empty)";
      updatePreviewPosition();
    }).catch(function () {
      $("overlay-preview-text").textContent = "(preview unavailable)";
    });
  }

  function initOverlay() {
    $("f-overlay-position").addEventListener("change", updatePreviewPosition);
    $("overlay-refresh-preview").addEventListener("click", refreshOverlayPreview);
    /* Auto-refresh preview when template fields change */
    var debounce = null;
    ["f-overlay-template", "f-overlay-alert-template"].forEach(function (id) {
      $(id).addEventListener("input", function () {
        clearTimeout(debounce);
        debounce = setTimeout(refreshOverlayPreview, 600);
      });
    });
  }

  /* ── Diagnostics ────────────────────────────────────────────────────────── */
  function setDiag(id, text, cls) {
    var el = $(id);
    el.textContent = text;
    el.className = "diag-result" + (cls ? " " + cls : "");
  }

  function initDiagnostics() {
    /* Test VAPIX */
    $("diag-test-vapix").addEventListener("click", function () {
      setDiag("diag-result-vapix", "Testing\u2026", "busy");
      cgi("test_vapix").then(function (r) {
        var msg = "HTTP " + r.http_code;
        if (r.ok) msg += " OK | Video: " + (r.has_video ? "yes" : "no") + " | Ports: " + r.max_ports;
        else msg += " FAILED";
        if (r.brand_raw) msg += "\n" + r.brand_raw.substring(0, 200);
        setDiag("diag-result-vapix", msg, r.ok ? "ok" : "bad");
      }).catch(function (e) { setDiag("diag-result-vapix", "Error: " + e.message, "bad"); });
    });

    /* Test weather */
    $("diag-test-weather").addEventListener("click", function () {
      setDiag("diag-result-weather", "Fetching\u2026", "busy");
      cgi("test_weather").then(function (r) {
        if (r.ok && r.valid) {
          setDiag("diag-result-weather",
            r.provider + ": " + r.temp_f.toFixed(1) + "\u00B0F, " + r.description +
            "\nWind " + r.wind_mph.toFixed(0) + " mph | Humidity " + r.humidity_pct + "%" +
            "\nAlerts: " + r.alert_count +
            "\nCoords: " + r.lat.toFixed(4) + ", " + r.lon.toFixed(4), "ok");
        } else {
          setDiag("diag-result-weather", "Fetch failed (ok=" + r.ok + ", valid=" + r.valid + ")", "bad");
        }
      }).catch(function (e) { setDiag("diag-result-weather", "Error: " + e.message, "bad"); });
    });

    /* Test webhook */
    $("diag-test-webhook").addEventListener("click", function () {
      setDiag("diag-result-webhook", "Posting\u2026", "busy");
      cgi("test_webhook", { method: "POST", body: "" }).then(function (r) {
        var msg = r.ok ? "Success (HTTP " + r.http_code + ")" : "Failed: " + (r.error || "HTTP " + r.http_code);
        setDiag("diag-result-webhook", msg, r.ok ? "ok" : "bad");
      }).catch(function (e) { setDiag("diag-result-webhook", "Error: " + e.message, "bad"); });
    });

    /* Manual port fire/clear */
    $("diag-fire-port").addEventListener("click", function () {
      var p = $("diag-port-num").value;
      if (!p) { toast("Enter a port number", "warn"); return; }
      setDiag("diag-manual-result", "Activating port " + p + "\u2026", "busy");
      cgi("fire_port", { method: "POST", qs: "port=" + p, body: "" }).then(function (r) {
        setDiag("diag-manual-result", r.ok ? "Port " + p + " activated" : "Failed (HTTP " + r.http_code + ")", r.ok ? "ok" : "bad");
      }).catch(function (e) { setDiag("diag-manual-result", "Error: " + e.message, "bad"); });
    });

    $("diag-clear-port").addEventListener("click", function () {
      var p = $("diag-port-num").value;
      if (!p) { toast("Enter a port number", "warn"); return; }
      setDiag("diag-manual-result", "Clearing port " + p + "\u2026", "busy");
      cgi("clear_port", { method: "POST", qs: "port=" + p, body: "" }).then(function (r) {
        setDiag("diag-manual-result", r.ok ? "Port " + p + " cleared" : "Failed (HTTP " + r.http_code + ")", r.ok ? "ok" : "bad");
      }).catch(function (e) { setDiag("diag-manual-result", "Error: " + e.message, "bad"); });
    });

    /* Fire drill */
    $("diag-firedrill").addEventListener("click", function () {
      if (!confirm("Fire Drill: This will activate ALL enabled virtual input ports.\n\nAny Action Rules or VMS event triggers pointing at those ports will fire.\n\nContinue?")) return;
      setDiag("diag-manual-result", "Firing all enabled ports\u2026", "busy");
      cgi("fire_drill", { method: "POST", body: "" }).then(function (r) {
        var msg = r.ok ? "Fired " + r.fired + " ports. " + (r.note || "") : "Failed";
        setDiag("diag-manual-result", msg, r.ok ? "ok" : "bad");
        toast("Fire drill: " + r.fired + " ports activated", "warn");
      }).catch(function (e) { setDiag("diag-manual-result", "Error: " + e.message, "bad"); });
    });

    /* Clear all */
    $("diag-clear-all").addEventListener("click", function () {
      setDiag("diag-manual-result", "Clearing\u2026", "busy");
      cgi("clear_all", { method: "POST", body: "" }).then(function (r) {
        setDiag("diag-manual-result", "Cleared " + r.cleared + " of " + r.total + " mapped ports", r.ok ? "ok" : "bad");
        toast("All ports cleared", "ok");
      }).catch(function (e) { setDiag("diag-manual-result", "Error: " + e.message, "bad"); });
    });
  }

  /* ── Device info ────────────────────────────────────────────────────────── */
  function refreshDeviceInfo() {
    $("diag-device-info").textContent = "Loading\u2026";
    cgi("device").then(function (r) {
      $("diag-device-info").textContent = r.raw || "(no info returned)";
    }).catch(function () {
      $("diag-device-info").textContent = "(could not reach device)";
    });
  }

  function initDeviceRefresh() {
    $("diag-refresh-device").addEventListener("click", refreshDeviceInfo);
  }

  /* ── History ────────────────────────────────────────────────────────────── */
  function refreshHistory() {
    cgi("history").then(function (r) {
      var entries = r.entries || [];
      /* Dashboard history */
      renderHistoryList($("dash-history"), entries.slice(-10).reverse());
      /* Diagnostics history */
      renderHistoryList($("diag-history"), entries.slice(-30).reverse());
    }).catch(function () { /* silent */ });
  }

  function renderHistoryList(ol, entries) {
    ol.innerHTML = "";
    if (entries.length === 0) {
      ol.innerHTML = '<li class="empty">No alert activity recorded yet.</li>';
      return;
    }
    entries.forEach(function (e) {
      var li = document.createElement("li");
      var when = e.ts || "";
      if (when.length > 19) when = when.substring(0, 19).replace("T", " ");
      var actionCls = e.action === "fire" ? "act-fire" : "act-clear";
      li.innerHTML = '<span class="when">' + escHtml(when) + '</span>' +
                     '<span class="what"><span class="' + actionCls + '">' +
                     escHtml(e.action || "") + '</span> ' +
                     escHtml(e.event || "") + '</span>';
      ol.appendChild(li);
    });
  }

  /* ── Ports probe ────────────────────────────────────────────────────────── */
  function probePorts() {
    cgi("ports").then(function (r) {
      maxPorts = r.max_ports || 32;
      $("ports-max-info").textContent = maxPorts + " virtual ports available";
      /* Update existing port inputs */
      $$(".ar-port").forEach(function (inp) { inp.max = maxPorts; });
    }).catch(function () { /* keep default */ });
  }

  /* ── Export / Import ────────────────────────────────────────────────────── */
  function initExportImport() {
    $("adv-import-file").addEventListener("change", function (e) {
      var file = e.target.files[0];
      if (!file) return;
      var reader = new FileReader();
      reader.onload = function (ev) {
        var text = ev.target.result;
        try { JSON.parse(text); } catch (err) {
          toast("Invalid JSON file: " + err.message, "error");
          return;
        }
        if (!confirm("Import config from " + file.name + "?\n\nThis will overwrite current settings.")) return;
        cgi("import", { body: text, json: true }).then(function (r) {
          if (r.ok) {
            toast("Config imported (" + r.saved + " fields)", "ok");
            loadConfig();
          } else {
            toast("Import had errors", "error");
          }
        }).catch(function (err) { toast("Import failed: " + err.message, "error"); });
      };
      reader.readAsText(file);
      /* Reset so same file can be re-imported */
      e.target.value = "";
    });
  }

  /* ── Snapshots ──────────────────────────────────────────────────────────── */

  function refreshSnapshots() {
    cgi("snapshot_list").then(function (r) {
      var info = $("snap-save-dir-info");
      if (info) info.textContent = r.save_dir ? ("Saving to: " + r.save_dir) : "";

      var gallery = $("snap-gallery");
      if (!gallery) return;

      var snaps = r.snapshots || [];
      if (snaps.length === 0) {
        gallery.innerHTML = '<p class="empty">No snapshots yet. Enable capture above and wait for an alert, or click \u201cTake test snapshot now.\u201d</p>';
        return;
      }

      var html = '<div class="snap-grid">';
      snaps.forEach(function (s) {
        var ts = (s.ts || "").substring(0, 19).replace("T", " ");
        var url = CGI + "?action=snapshot_image&file=" + encodeURIComponent(s.filename);
        /* Derive a human-readable label from the filename
         * e.g. "20260427_142537_Tornado_Warning.jpg" → "Tornado Warning" */
        var label = s.filename.replace(/^\d{8}_\d{6}_/, "").replace(/\.jpg$/i, "").replace(/_/g, " ");
        html += '<div class="snap-item">' +
                '<a href="' + url + '" target="_blank" rel="noopener">' +
                '<img src="' + url + '" class="snap-thumb" alt="' + escHtml(s.filename) + '" loading="lazy">' +
                '</a>' +
                '<div class="snap-meta">' +
                '<span class="snap-ts">' + escHtml(ts) + '</span>' +
                '<span class="snap-name">' + escHtml(label) + '</span>' +
                '</div></div>';
      });
      html += '</div>';
      gallery.innerHTML = html;
    }).catch(function () { /* silent — directory may not exist yet */ });
  }

  function initSnapshots() {
    /* "Capture now" — clean on-demand capture, no diagnostic probe */
    $("snap-capture-btn").addEventListener("click", function () {
      var res = $("snap-capture-result");
      res.textContent = "Capturing\u2026";
      res.className = "diag-result busy";
      cgi("capture_now", { method: "POST", body: "" })
        .then(function (r) {
          if (r.ok) {
            res.textContent = "Saved \u2713";
            res.className = "diag-result ok";
            refreshSnapshots();
          } else {
            res.textContent = "Failed \u2014 check credentials, or use \u201cTest & diagnose\u201d for details.";
            res.className = "diag-result bad";
          }
        })
        .catch(function (e) {
          res.textContent = "Error: " + e.message;
          res.className = "diag-result bad";
        });
    });

    /* "Test & diagnose" — attempts capture + VAPIX probe for detailed error */
    $("snap-test-btn").addEventListener("click", function () {
      var res = $("snap-test-result");
      res.textContent = "Testing\u2026";
      res.className = "diag-result busy";
      cgi("test_snapshot", { method: "POST", body: "" })
        .then(function (r) {
          if (r.ok) {
            res.textContent = "OK \u2014 saved: " + (r.path || r.save_dir || "?");
            res.className = "diag-result ok";
            refreshSnapshots();
          } else {
            var msgs = {
              "dir":     "Directory error \u2014 VAPIX responded (HTTP " + r.probe_http + ") but the save directory couldn\u2019t be created. Check syslog for mkdir errors.",
              "auth":    "VAPIX auth failed (HTTP " + r.probe_http + "). Verify VAPIX username and password in the Advanced tab.",
              "connect": "Cannot reach camera VAPIX at localhost. Is the daemon running?",
              "":        "Capture failed \u2014 check syslog for details."
            };
            res.textContent = msgs[r.fail_step || ""] || msgs[""];
            res.className = "diag-result bad";
          }
        })
        .catch(function (e) {
          res.textContent = "Error: " + e.message;
          res.className = "diag-result bad";
        });
    });

    $("snap-refresh-btn").addEventListener("click", refreshSnapshots);
  }

  /* ── Notification tests (MQTT + Email) ─────────────────────────────────── */
  function initNotifications() {
    $("adv-test-mqtt").addEventListener("click", function () {
      var res = $("adv-result-mqtt");
      res.textContent = "Publishing\u2026";
      res.className = "diag-result busy";
      cgi("test_mqtt", { method: "POST", body: "" })
        .then(function (r) {
          if (r.ok) {
            res.textContent = "Published successfully";
            res.className = "diag-result ok";
          } else {
            res.textContent = r.error
              ? "Failed: " + r.error
              : "Failed \u2014 check syslog (broker unreachable or libcurl lacks MQTT support)";
            res.className = "diag-result bad";
          }
        })
        .catch(function (e) {
          res.textContent = "Error: " + e.message;
          res.className = "diag-result bad";
        });
    });

    $("adv-test-email").addEventListener("click", function () {
      var res = $("adv-result-email");
      res.textContent = "Sending\u2026";
      res.className = "diag-result busy";
      cgi("test_email", { method: "POST", body: "" })
        .then(function (r) {
          if (r.ok) {
            res.textContent = "Email sent successfully";
            res.className = "diag-result ok";
          } else {
            res.textContent = r.error
              ? "Failed: " + r.error
              : "Failed \u2014 check syslog for SMTP error details";
            res.className = "diag-result bad";
          }
        })
        .catch(function (e) {
          res.textContent = "Error: " + e.message;
          res.className = "diag-result bad";
        });
    });
  }

  /* ── Sparkline trend charts (Sprint 6) ─────────────────────────────────── */
  var sparkTimer = null;

  function drawSparkline(canvasId, values, color) {
    var canvas = $(canvasId);
    if (!canvas) return;

    /* Match canvas pixel width to its CSS layout width for crisp rendering */
    var rect = canvas.getBoundingClientRect();
    var w = Math.round(rect.width) || 300;
    var h = canvas.height || 48;
    canvas.width = w;

    var ctx = canvas.getContext("2d");
    ctx.clearRect(0, 0, w, h);

    if (!values || values.length < 2) return;

    var min = values[0], max = values[0];
    for (var i = 1; i < values.length; i++) {
      if (values[i] < min) min = values[i];
      if (values[i] > max) max = values[i];
    }
    /* Give a small pad so flat lines are visible */
    var range = max - min || 1;
    var pad = range * 0.15;
    min -= pad; max += pad; range = max - min;

    function xp(i)   { return (i / (values.length - 1)) * w; }
    function yp(val) { return h - ((val - min) / range) * (h - 4) - 2; }

    /* Filled area */
    ctx.beginPath();
    ctx.moveTo(xp(0), h);
    ctx.lineTo(xp(0), yp(values[0]));
    for (var j = 1; j < values.length; j++)
      ctx.lineTo(xp(j), yp(values[j]));
    ctx.lineTo(xp(values.length - 1), h);
    ctx.closePath();
    ctx.fillStyle = color.replace(")", ", 0.18)").replace("rgb(", "rgba(");
    ctx.fill();

    /* Line on top */
    ctx.beginPath();
    ctx.moveTo(xp(0), yp(values[0]));
    for (var k = 1; k < values.length; k++)
      ctx.lineTo(xp(k), yp(values[k]));
    ctx.strokeStyle = color;
    ctx.lineWidth = 1.5;
    ctx.lineJoin = "round";
    ctx.stroke();

    /* Dot at the latest value */
    ctx.beginPath();
    ctx.arc(xp(values.length - 1), yp(values[values.length - 1]), 3, 0, Math.PI * 2);
    ctx.fillStyle = color;
    ctx.fill();
  }

  function refreshSparklines() {
    cgi("cond_history").then(function (r) {
      var pts = r.points || [];
      var empty = $("trend-empty");

      if (pts.length < 2) {
        if (empty) empty.style.display = "";
        return;
      }
      if (empty) empty.style.display = "none";

      var temps  = pts.map(function (p) { return p.temp_f; });
      var winds  = pts.map(function (p) { return p.wind_mph; });
      var hums   = pts.map(function (p) { return p.humidity_pct; });

      drawSparkline("spark-temp", temps, "rgb(79, 195, 247)");   /* sky blue */
      drawSparkline("spark-wind", winds, "rgb(129, 199, 132)");  /* green   */
      drawSparkline("spark-hum",  hums,  "rgb(255, 183, 77)");   /* amber   */

      /* Update current-value labels */
      var last = pts[pts.length - 1];
      $("spark-temp-cur").textContent = last.temp_f.toFixed(1) + "\u00B0";
      $("spark-wind-cur").textContent = last.wind_mph.toFixed(0) + " mph";
      $("spark-hum-cur").textContent  = last.humidity_pct + "%";

      /* Range label: first ts → last ts */
      var t0 = (pts[0].ts || "").substring(0, 16).replace("T", " ");
      var t1 = (last.ts  || "").substring(0, 16).replace("T", " ");
      var range = $("trend-range");
      if (range) range.textContent = t0 + " \u2013 " + t1 + " UTC";

    }).catch(function () { /* silent — daemon may not have polled yet */ });
  }

  /* ── Boot ────────────────────────────────────────────────────────────────── */
  function init() {
    initTabs();
    initSaveButtons();
    initAlertButtons();
    initThresholdButtons();
    initOverlay();
    initSnapshots();
    initNotifications();
    initDiagnostics();
    initDeviceRefresh();
    initExportImport();

    loadConfig()
      .then(function () {
        probePorts();
        refreshOverlayPreview();
        pollStatus();
        refreshHistory();
        refreshSparklines();
      })
      .catch(function (e) {
        toast("Failed to load config: " + e.message, "error");
      });

    /* Start periodic status polling */
    pollTimer = setInterval(pollStatus, POLL_MS);

    /* Refresh sparklines every 5 minutes */
    sparkTimer = setInterval(refreshSparklines, 5 * 60 * 1000);
  }

  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", init);
  } else {
    init();
  }
})();
