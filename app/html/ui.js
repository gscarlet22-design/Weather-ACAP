/* ===========================================================================
 * Weather ACAP — ui.js
 * Click-driven controls layered over the real form inputs.
 *
 * app.js is the single source of truth: it loads config into the inputs,
 * gathers them on save, and renders the tables. This file never touches the
 * CGI. It renders pill / card / swatch / token controls, writes the chosen
 * value into the underlying input, and fires input+change so app.js reacts
 * exactly as if the user had typed. Every control also stays in sync when
 * app.js repopulates the inputs after a load or save.
 *
 * Hooks read from index.html:
 *   <select data-pills [data-pill-labels="a,b"] [data-pills-live]>
 *   <select data-cards data-card-descs="d1|d2|d3">
 *   <select data-corners>                       overlay position
 *   <input type="number" data-presets="1,2" [data-preset-labels] [data-unit]>
 *   <input type="number" data-stepper>
 *   <input type="text" data-suggest="a,b,c">
 *   <div class="color-input-row" data-swatches="#fff,#000">
 * =========================================================================*/
(function () {
  "use strict";

  function $(id) { return document.getElementById(id); }
  function $$(sel, ctx) {
    return Array.prototype.slice.call((ctx || document).querySelectorAll(sel));
  }
  function el(tag, cls, text) {
    var n = document.createElement(tag);
    if (cls) n.className = cls;
    if (text !== undefined) n.textContent = text;
    return n;
  }
  function fire(node) {
    node.dispatchEvent(new Event("input", { bubbles: true }));
    node.dispatchEvent(new Event("change", { bubbles: true }));
  }
  function split(attr) {
    return (attr || "").split(",").map(function (s) { return s.trim(); }).filter(Boolean);
  }

  /* Controls register a sync function; the loop below keeps every control
   * matching its input, including after app.js repopulates on load/save. */
  var syncers = [];
  function syncAll() {
    for (var i = 0; i < syncers.length; i++) {
      try { syncers[i](); } catch (e) { /* never let one control break the rest */ }
    }
  }

  /* ── Option pills from a <select> ──────────────────────────────────────── */
  function pillsFromSelect(sel) {
    var labels = split(sel.getAttribute("data-pill-labels"));
    var row = el("div", "chip-row");
    var mono = sel.classList.contains("mono") || sel.hasAttribute("data-pills-live");
    sel.style.display = "none";
    sel.parentNode.insertBefore(row, sel);

    function build() {
      row.innerHTML = "";
      $$("option", sel).forEach(function (opt, i) {
        var b = el("button", "opt" + (mono ? " opt-mono" : ""), labels[i] || opt.textContent.trim());
        b.type = "button";
        b.title = opt.textContent.trim();
        b.addEventListener("click", function () {
          sel.value = opt.value;
          fire(sel);
          sync();
        });
        row.appendChild(b);
      });
      sync();
    }

    function sync() {
      var opts = $$("option", sel);
      $$("button", row).forEach(function (b, i) {
        b.setAttribute("aria-pressed", opts[i] && opts[i].value === sel.value ? "true" : "false");
      });
    }

    build();
    syncers.push(sync);

    /* Audio clip lists are rebuilt from the device after load */
    if (sel.hasAttribute("data-pills-live")) {
      new MutationObserver(build).observe(sel, { childList: true });
    }
  }

  /* ── Option cards from a <select> ──────────────────────────────────────── */
  function cardsFromSelect(sel) {
    var descs = (sel.getAttribute("data-card-descs") || "").split("|");
    var row = el("div", "chip-row");
    sel.style.display = "none";
    sel.parentNode.insertBefore(row, sel);

    $$("option", sel).forEach(function (opt, i) {
      var b = el("button", "opt opt-card");
      b.type = "button";
      b.appendChild(el("span", "opt-card-name", opt.textContent.trim()));
      if (descs[i]) b.appendChild(el("span", "opt-card-desc", descs[i].trim()));
      b.addEventListener("click", function () {
        sel.value = opt.value;
        fire(sel);
        sync();
      });
      row.appendChild(b);
    });

    function sync() {
      var opts = $$("option", sel);
      $$("button", row).forEach(function (b, i) {
        b.setAttribute("aria-pressed", opts[i] && opts[i].value === sel.value ? "true" : "false");
      });
    }

    sync();
    syncers.push(sync);
  }

  /* ── Corner picker (overlay position) ─────────────────────────────────── */
  var CORNER_CLS = {
    topLeft: "corner-tl", topRight: "corner-tr",
    bottomLeft: "corner-bl", bottomRight: "corner-br"
  };

  function cornersFromSelect(sel) {
    var grid = el("div", "corner-grid");
    sel.style.display = "none";
    sel.parentNode.insertBefore(grid, sel);

    $$("option", sel).forEach(function (opt) {
      var b = el("button", "corner " + (CORNER_CLS[opt.value] || "corner-tl"));
      b.type = "button";
      b.title = opt.textContent.trim();
      b.appendChild(el("span"));
      b.addEventListener("click", function () {
        sel.value = opt.value;
        fire(sel);
        sync();
      });
      grid.appendChild(b);
    });

    function sync() {
      var opts = $$("option", sel);
      $$("button", grid).forEach(function (b, i) {
        b.setAttribute("aria-pressed", opts[i] && opts[i].value === sel.value ? "true" : "false");
      });
    }

    sync();
    syncers.push(sync);
  }

  /* ── Preset pills for a number input ──────────────────────────────────── */
  function presetsForInput(input) {
    var vals = split(input.getAttribute("data-presets"));
    var labels = split(input.getAttribute("data-preset-labels"));
    var unit = input.getAttribute("data-unit");

    var wrap = el("div", "preset-wrap");
    input.parentNode.insertBefore(wrap, input);

    var row = el("div", "chip-row");
    wrap.appendChild(row);

    vals.forEach(function (v, i) {
      var b = el("button", "opt opt-mono", labels[i] || v);
      b.type = "button";
      b.addEventListener("click", function () {
        input.value = v;
        fire(input);
        sync();
      });
      row.appendChild(b);
    });

    /* the input itself stays reachable for values off the preset list */
    wrap.appendChild(input);
    if (unit) wrap.appendChild(el("span", "unit", unit));

    function sync() {
      $$("button", row).forEach(function (b, i) {
        b.setAttribute("aria-pressed", String(input.value) === vals[i] ? "true" : "false");
      });
    }

    sync();
    syncers.push(sync);
  }

  /* ── Stepper for a number input ───────────────────────────────────────── */
  function stepperForInput(input) {
    var wrap = el("div", "stepper");
    input.parentNode.insertBefore(wrap, input);

    var dec = el("button", null, "\u2212");
    var inc = el("button", null, "+");
    dec.type = inc.type = "button";

    wrap.appendChild(dec);
    wrap.appendChild(input);
    wrap.appendChild(inc);

    function bump(delta) {
      var min = parseFloat(input.min);
      var max = parseFloat(input.max);
      var cur = parseFloat(input.value);
      if (isNaN(cur)) cur = isNaN(min) ? 0 : min;
      var next = cur + delta;
      if (!isNaN(min) && next < min) next = min;
      if (!isNaN(max) && next > max) next = max;
      input.value = String(next);
      fire(input);
    }

    dec.addEventListener("click", function () { bump(-1); });
    inc.addEventListener("click", function () { bump(1); });
  }

  /* ── Colour swatches ──────────────────────────────────────────────────── */
  function swatchesForRow(row) {
    var hexes = split(row.getAttribute("data-swatches"));
    var picker = row.querySelector('input[type="color"]');
    var hex = row.querySelector(".color-hex");
    if (!picker || !hex) return;

    var bar = el("div", "swatch-row");
    row.insertBefore(bar, picker);

    hexes.forEach(function (h) {
      var b = el("button", "swatch");
      b.type = "button";
      b.style.background = h;
      b.title = h;
      b.addEventListener("click", function () {
        picker.value = h;
        hex.value = h;
        fire(picker);
        fire(hex);
        sync();
      });
      bar.appendChild(b);
    });

    function sync() {
      var cur = (hex.value || "").trim().toUpperCase();
      $$("button", bar).forEach(function (b, i) {
        b.setAttribute("aria-pressed", hexes[i].toUpperCase() === cur ? "true" : "false");
      });
    }

    sync();
    syncers.push(sync);
  }

  /* ── Suggestion pills for a free-text input ───────────────────────────── */
  function suggestForInput(input) {
    var vals = split(input.getAttribute("data-suggest"));
    var row = el("div", "chip-row");
    input.parentNode.insertBefore(row, input.nextSibling);

    vals.forEach(function (v) {
      var b = el("button", "opt opt-mono", v);
      b.type = "button";
      b.addEventListener("click", function () {
        input.value = v;
        fire(input);
        sync();
      });
      row.appendChild(b);
    });

    function sync() {
      $$("button", row).forEach(function (b, i) {
        b.setAttribute("aria-pressed", input.value.trim() === vals[i] ? "true" : "false");
      });
    }

    sync();
    syncers.push(sync);
  }

  /* ═══════════════════════ Overlay template builder ═══════════════════════
   * Fields are clicked in and out; the template string is regenerated and
   * written to #f-overlay-template, which app.js already watches (debounced)
   * to re-render the camera-side preview. Typing in the template directly
   * still works — the tray re-reads it.
   * ====================================================================== */
  var TOKENS = [
    { id: "cond",      label: "Conditions", unit: "" },
    { id: "temp",      label: "Temp",       unit: "F" },
    { id: "temp_f",    label: "Temp",       unit: "\u00B0F" },
    { id: "hum",       label: "Hum",        unit: "%" },
    { id: "wind",      label: "Wind",       unit: "mph" },
    { id: "dir",       label: "Dir",        unit: "" },
    { id: "arrow",     label: "Arrow",      unit: "" },
    { id: "lightning", label: "SPC risk",   unit: "" },
    { id: "provider",  label: "Provider",   unit: "" },
    { id: "sunrise",   label: "Sunrise",    unit: "" },
    { id: "sunset",    label: "Sunset",     unit: "" },
    { id: "time",      label: "Time",       unit: "" },
    { id: "utc",       label: "UTC",        unit: "" },
    { id: "lat",       label: "Lat",        unit: "" },
    { id: "lon",       label: "Lon",        unit: "" }
  ];
  var TOKMAP = {};
  TOKENS.forEach(function (t) { TOKMAP[t.id] = t; });

  var SEPS = [
    { id: "pipe",  str: " | ",        label: "|" },
    { id: "dot",   str: " \u00B7 ",   label: "\u00B7" },
    { id: "dash",  str: " \u2014 ",   label: "\u2014" },
    { id: "space", str: "   ",        label: "space" }
  ];

  var OV_PRESETS = [
    { label: "App default", fields: ["temp", "cond", "wind", "dir", "hum"], labels: true,  sep: "pipe" },
    { label: "Compact",     fields: ["temp", "cond"],                      labels: false, sep: "dot"  },
    { label: "Storm watch", fields: ["cond", "lightning", "wind", "dir", "time"], labels: true, sep: "pipe" },
    { label: "Full station",fields: ["temp", "cond", "wind", "dir", "hum", "provider", "time"], labels: true, sep: "pipe" }
  ];

  var PREFIXES = [
    { label: "[ALERT: type]", str: "[ALERT: {alert_type}] " },
    { label: "\u26A0 type \u2014", str: "\u26A0 {alert_type} \u2014 " },
    { label: "type |", str: "{alert_type} | " },
    { label: "No prefix", str: "" }
  ];

  function initOverlayBuilder() {
    var tplInput = $("f-overlay-template");
    var prefixInput = $("f-overlay-alert-template");
    var trayEl = $("overlay-chosen");
    var availEl = $("overlay-available");
    var presetEl = $("overlay-presets");
    var sepEl = $("overlay-seps");
    var labelEl = $("overlay-labels");
    var prefixEl = $("overlay-prefixes");
    if (!tplInput || !trayEl) return;

    var useLabels = true;
    var sepId = "pipe";
    var lastSeen = null;

    function sepStr() {
      for (var i = 0; i < SEPS.length; i++) if (SEPS[i].id === sepId) return SEPS[i].str;
      return SEPS[0].str;
    }

    /* Pure: the chosen fields, in template order. Must stay side-effect free —
     * every click handler re-reads the template through this. */
    function parse(str) {
      var order = [];
      var re = /\{(\w+)\}/g;
      var m;
      while ((m = re.exec(str)) !== null) {
        if (TOKMAP[m[1]] && order.indexOf(m[1]) < 0) order.push(m[1]);
      }
      return order;
    }

    /* Sniff separator + label style OUT of a template. Only called when the
     * template changed on us (config load, or typing in the field) — never
     * from a click path, or it would clobber the style just chosen. */
    function detect(str) {
      if (str.indexOf(" | ") >= 0) sepId = "pipe";
      else if (str.indexOf(" \u00B7 ") >= 0) sepId = "dot";
      else if (str.indexOf(" \u2014 ") >= 0) sepId = "dash";
      else if (parse(str).length > 1) sepId = "space";
      useLabels = /[A-Za-z][A-Za-z ]*:\s*\{/.test(str);
    }

    function compose(fields) {
      return fields.map(function (id) {
        var t = TOKMAP[id];
        var body = "{" + id + "}" + t.unit;
        return useLabels ? t.label + ": " + body : body;
      }).join(sepStr());
    }

    function apply(fields) {
      tplInput.value = compose(fields);
      fire(tplInput);
      render();
    }

    function currentFields() { return parse(tplInput.value || ""); }

    /* Static rows — built once */
    OV_PRESETS.forEach(function (p) {
      var b = el("button", "opt", p.label);
      b.type = "button";
      b.addEventListener("click", function () {
        useLabels = p.labels;
        sepId = p.sep;
        apply(p.fields.slice());
      });
      presetEl.appendChild(b);
    });

    TOKENS.forEach(function (t) {
      var b = el("button", "opt opt-round");
      b.type = "button";
      b.appendChild(el("span", null, t.label + (t.unit ? " " + t.unit : "")));
      b.appendChild(el("span", "tok", "{" + t.id + "}"));
      b.addEventListener("click", function () {
        var f = currentFields();
        var at = f.indexOf(t.id);
        if (at >= 0) f.splice(at, 1); else f.push(t.id);
        apply(f);
      });
      b.setAttribute("data-token", t.id);
      availEl.appendChild(b);
    });

    SEPS.forEach(function (s) {
      var b = el("button", "opt opt-mono", s.label);
      b.type = "button";
      b.setAttribute("data-sep", s.id);
      b.addEventListener("click", function () {
        sepId = s.id;
        apply(currentFields());
      });
      sepEl.appendChild(b);
    });

    [{ on: true, label: "Show labels" }, { on: false, label: "Values only" }].forEach(function (o) {
      var b = el("button", "opt", o.label);
      b.type = "button";
      b.setAttribute("data-labels", o.on ? "1" : "0");
      b.addEventListener("click", function () {
        useLabels = o.on;
        apply(currentFields());
      });
      labelEl.appendChild(b);
    });

    PREFIXES.forEach(function (p) {
      var b = el("button", "opt opt-mono", p.label);
      b.type = "button";
      b.setAttribute("data-prefix", p.str);
      b.addEventListener("click", function () {
        prefixInput.value = p.str;
        fire(prefixInput);
        render();
      });
      prefixEl.appendChild(b);
    });

    /* Chosen-field tray — rebuilt only when the template actually changes */
    function renderTray(fields) {
      trayEl.innerHTML = "";
      if (!fields.length) {
        trayEl.appendChild(el("span", "token-empty", "No fields chosen — the overlay would be blank."));
        return;
      }
      fields.forEach(function (id, i) {
        var t = TOKMAP[id];
        var tok = el("div", "token");

        var left = el("button", null, "\u2039");
        left.type = "button";
        left.title = "Move left";
        left.addEventListener("click", function () {
          if (i === 0) return;
          var f = currentFields();
          var tmp = f[i - 1]; f[i - 1] = f[i]; f[i] = tmp;
          apply(f);
        });

        var right = el("button", null, "\u203A");
        right.type = "button";
        right.title = "Move right";
        right.addEventListener("click", function () {
          var f = currentFields();
          if (i >= f.length - 1) return;
          var tmp = f[i + 1]; f[i + 1] = f[i]; f[i] = tmp;
          apply(f);
        });

        var del = el("button", "token-del", "\u00D7");
        del.type = "button";
        del.title = "Remove";
        del.addEventListener("click", function () {
          var f = currentFields();
          f.splice(i, 1);
          apply(f);
        });

        tok.appendChild(left);
        tok.appendChild(el("span", "token-name", t.label));
        tok.appendChild(right);
        tok.appendChild(del);
        trayEl.appendChild(tok);
      });
    }

    function render() {
      var str = tplInput.value || "";
      var fields = parse(str);

      if (str !== lastSeen) {
        var mine = lastSeen !== null && str === compose(fields);
        if (!mine) detect(str);
        lastSeen = str;
        renderTray(fields);
      }

      $$("button[data-token]", availEl).forEach(function (b) {
        b.setAttribute("aria-pressed", fields.indexOf(b.getAttribute("data-token")) >= 0 ? "true" : "false");
      });
      $$("button[data-sep]", sepEl).forEach(function (b) {
        b.setAttribute("aria-pressed", b.getAttribute("data-sep") === sepId ? "true" : "false");
      });
      $$("button[data-labels]", labelEl).forEach(function (b) {
        b.setAttribute("aria-pressed", (b.getAttribute("data-labels") === "1") === useLabels ? "true" : "false");
      });
      $$("button[data-prefix]", prefixEl).forEach(function (b) {
        b.setAttribute("aria-pressed", b.getAttribute("data-prefix") === (prefixInput.value || "") ? "true" : "false");
      });
    }

    tplInput.addEventListener("input", render);
    prefixInput.addEventListener("input", render);
    syncers.push(render);
  }

  /* ═══════════════════════ Quick-add rows ═══════════════════════ */
  var NWS_TYPES = [
    "Tornado Warning", "Severe Thunderstorm Warning", "Flash Flood Warning",
    "Flood Warning", "Winter Storm Warning", "Blizzard Warning",
    "Ice Storm Warning", "High Wind Warning", "Hurricane Warning",
    "Tropical Storm Warning", "Extreme Heat Warning", "Red Flag Warning",
    "Tornado Watch", "Severe Thunderstorm Watch", "Flash Flood Watch"
  ];

  var THRESH_RULES = [
    { label: "Heat \u2014 TempF > 95",      cond: "TempF",       op: ">", val: "95" },
    { label: "Freeze \u2014 TempF < 32",    cond: "TempF",       op: "<", val: "32" },
    { label: "High wind \u2014 WindMph > 40", cond: "WindMph",   op: ">", val: "40" },
    { label: "Humid \u2014 HumidityPct > 90", cond: "HumidityPct", op: ">", val: "90" }
  ];

  function existingTypes() {
    return $$(".alert-row .ar-type").map(function (i) { return i.value.trim().toLowerCase(); });
  }

  function initQuickAdd() {
    var alertsRow = $("alerts-quickadd");
    var addBtn = $("alerts-add");

    if (alertsRow && addBtn) {
      NWS_TYPES.forEach(function (type) {
        var b = el("button", "opt opt-dashed", "+ " + type);
        b.type = "button";
        b.setAttribute("data-type", type);
        b.addEventListener("click", function () {
          var have = existingTypes().indexOf(type.toLowerCase());
          if (have >= 0) {
            /* already mapped — arm it and bring it into view instead of duplicating */
            var row = $$(".alert-row")[have];
            var on = row.querySelector(".ar-on");
            if (on && !on.checked) { on.checked = true; fire(on); }
            row.classList.add("row-flash");
            setTimeout(function () { row.classList.remove("row-flash"); }, 900);
            return;
          }
          addBtn.click();
          var rows = $$(".alert-row");
          var last = rows[rows.length - 1];
          if (!last) return;
          last.querySelector(".ar-type").value = type;
          var on2 = last.querySelector(".ar-on");
          if (on2) on2.checked = true;
          sync();
        });
        alertsRow.appendChild(b);
      });

      function sync() {
        var have = existingTypes();
        var rows = $$(".alert-row");
        $$("button[data-type]", alertsRow).forEach(function (b) {
          var t = b.getAttribute("data-type").toLowerCase();
          var at = have.indexOf(t);
          var on = at >= 0 && rows[at] && rows[at].querySelector(".ar-on") &&
                   rows[at].querySelector(".ar-on").checked;
          b.setAttribute("aria-pressed", on ? "true" : "false");
          b.title = at < 0 ? "Add a row for this alert type"
                  : on ? "Mapped and armed" : "Mapped but switched off — click to arm";
          b.textContent = (at >= 0 ? "\u2713 " : "+ ") + b.getAttribute("data-type");
        });
      }
      sync();
      syncers.push(sync);
    }

    var threshRow = $("threshold-quickadd");
    var threshAdd = $("threshold-add");
    if (threshRow && threshAdd) {
      THRESH_RULES.forEach(function (r) {
        var b = el("button", "opt opt-dashed", "+ " + r.label);
        b.type = "button";
        b.addEventListener("click", function () {
          threshAdd.click();
          var rows = $$(".threshold-row");
          var last = rows[rows.length - 1];
          if (!last) return;
          last.querySelector(".tr-cond").value = r.cond;
          last.querySelector(".tr-op").value = r.op;
          last.querySelector(".tr-val").value = r.val;
        });
        threshRow.appendChild(b);
      });
    }
  }

  /* ── Armed-port chips on Diagnostics ─────────────────────────────────── */
  function initArmedPorts() {
    var row = $("diag-armed-ports");
    var input = $("diag-port-num");
    if (!row || !input) return;

    var lastKey = "";

    function sync() {
      var seen = [];
      $$(".alert-row").forEach(function (tr) {
        var on = tr.querySelector(".ar-on");
        var p = tr.querySelector(".ar-port");
        if (on && on.checked && p && seen.indexOf(p.value) < 0) seen.push(p.value);
      });
      $$(".threshold-row").forEach(function (tr) {
        var on = tr.querySelector(".tr-on");
        var p = tr.querySelector(".tr-port");
        if (on && on.checked && p && seen.indexOf(p.value) < 0) seen.push(p.value);
      });
      var lp = $("f-lightning-port");
      var le = $("f-lightning-enabled");
      if (lp && le && le.checked && seen.indexOf(lp.value) < 0) seen.push(lp.value);

      seen.sort(function (a, b) { return parseInt(a, 10) - parseInt(b, 10); });

      var key = seen.join(",");
      if (key !== lastKey) {
        lastKey = key;
        row.innerHTML = "";
        if (!seen.length) {
          row.appendChild(el("span", "token-empty", "No armed ports yet — map an alert on the Alerts tab."));
        }
        seen.forEach(function (p) {
          var b = el("button", "opt opt-mono", p);
          b.type = "button";
          b.setAttribute("data-port", p);
          b.addEventListener("click", function () {
            input.value = p;
            fire(input);
          });
          row.appendChild(b);
        });
      }
      $$("button[data-port]", row).forEach(function (b) {
        b.setAttribute("aria-pressed", b.getAttribute("data-port") === input.value ? "true" : "false");
      });
    }

    sync();
    syncers.push(sync);
  }

  /* ── Topbar follows the active tab ───────────────────────────────────── */
  var TAB_TEXT = {
    dashboard:   ["Dashboard", "Live conditions, ports and history"],
    location:    ["Location", "Where the camera is and where weather comes from"],
    alerts:      ["Alerts & triggers", "Map alerts and thresholds to virtual input ports"],
    overlay:     ["Overlay", "What the camera burns into the video stream"],
    snapshots:   ["Snapshots", "JPEG capture on alert, local and networked"],
    hardware:    ["Hardware output", "Display, strobe, siren and audio on alert"],
    diagnostics: ["Diagnostics", "Self-tests, manual ports and device info"],
    advanced:    ["Advanced", "Integrations, credentials and config backup"]
  };

  function initTopbar() {
    var title = $("topbar-title");
    var sub = $("topbar-sub");
    if (!title || !sub) return;

    $$(".tab-btn").forEach(function (btn) {
      btn.addEventListener("click", function () {
        var t = TAB_TEXT[btn.getAttribute("data-tab")];
        if (t) { title.textContent = t[0]; sub.textContent = t[1]; }
      });
    });
  }

  /* ── Boot ────────────────────────────────────────────────────────────── */
  function init() {
    $$("select[data-pills]").forEach(pillsFromSelect);
    $$("select[data-cards]").forEach(cardsFromSelect);
    $$("select[data-corners]").forEach(cornersFromSelect);
    $$("input[data-presets]").forEach(presetsForInput);
    $$("input[data-stepper]").forEach(stepperForInput);
    $$("input[data-suggest]").forEach(suggestForInput);
    $$(".color-input-row[data-swatches]").forEach(swatchesForRow);

    initOverlayBuilder();
    initQuickAdd();
    initArmedPorts();
    initTopbar();

    /* app.js writes input values directly (no events) when it loads config
     * or rebuilds a table, so poll for changes rather than guessing when. */
    setInterval(syncAll, 400);
    document.addEventListener("input", syncAll, true);
    document.addEventListener("change", syncAll, true);
    document.addEventListener("click", function () { setTimeout(syncAll, 0); }, true);
  }

  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", init);
  } else {
    init();
  }
})();
