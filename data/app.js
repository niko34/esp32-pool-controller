(() => {
  "use strict";

  // ---------- Utils ----------
  const $ = (sel, root = document) => root.querySelector(sel);
  const $$ = (sel, root = document) => Array.from(root.querySelectorAll(sel));
  const clamp = (n, a, b) => Math.max(a, Math.min(b, n));

  // Segmented value controls (ex: regulation_speed)
  function setSegmented(id, value) {
    const container = $(`#${id}`);
    if (!container) return;
    container.querySelectorAll(".segmented__btn").forEach(btn => {
      btn.classList.toggle("is-active", btn.dataset.value === value);
    });
  }
  function getSegmented(id) {
    const active = $(`#${id} .segmented__btn.is-active`);
    return active ? active.dataset.value : null;
  }
  function initSegmented(id) {
    const container = $(`#${id}`);
    if (!container) return;
    container.querySelectorAll(".segmented__btn").forEach(btn => {
      btn.addEventListener("click", () => setSegmented(id, btn.dataset.value));
    });
  }
  const DEBUG = false;

  // ---------- WebSocket ----------
  let _ws = null;
  let _wsReconnectTimer = null;

  // Heartbeat : si aucun message WS reçu depuis > 12 s, on considère l'ESP hors ligne
  const kWsHeartbeatMs = 12000;
  let _wsHeartbeatTimer = null;

  function _resetWsHeartbeat() {
    if (_wsHeartbeatTimer) clearTimeout(_wsHeartbeatTimer);
    _wsHeartbeatTimer = setTimeout(() => {
      setNetStatus('bad', 'Hors ligne');
      debugLog('[WS] Heartbeat timeout — no message received');
      _closeWs();
      _wsReconnectTimer = setTimeout(() => { _wsReconnectTimer = null; initWebSocket(); }, 3000);
    }, kWsHeartbeatMs);
  }

  function _closeWs() {
    if (!_ws) return;
    _ws.onopen = _ws.onclose = _ws.onerror = _ws.onmessage = null;
    try { _ws.close(); } catch (e) { /* ignore */ }
    _ws = null;
    if (_wsHeartbeatTimer) { clearTimeout(_wsHeartbeatTimer); _wsHeartbeatTimer = null; }
    if (_wsReconnectTimer) { clearTimeout(_wsReconnectTimer); _wsReconnectTimer = null; }
  }

  function initWebSocket() {
    if (_ws && (_ws.readyState === WebSocket.OPEN || _ws.readyState === WebSocket.CONNECTING)) return;
    // Detach handlers from previous WS to avoid stale events
    if (_ws) { _ws.onopen = _ws.onclose = _ws.onerror = _ws.onmessage = null; }
    const token = sessionStorage.getItem('authToken');
    const proto = location.protocol === 'https:' ? 'wss:' : 'ws:';
    const url = `${proto}//${location.host}/ws`;
    _ws = new WebSocket(url);

    _ws.onopen = () => {
      if (_wsReconnectTimer) { clearTimeout(_wsReconnectTimer); _wsReconnectTimer = null; }
      if (token) _ws.send(JSON.stringify({ type: 'auth', token }));
      setNetStatus('ok', 'En ligne');
      _resetWsHeartbeat();
      debugLog('[WS] Connected');
    };

    _ws.onmessage = (evt) => {
      _resetWsHeartbeat();
      try {
        const msg = JSON.parse(evt.data);
        if (msg.type === 'sensor_data') {
          _onWsSensorData(msg.data);
        } else if (msg.type === 'config') {
          loadConfig({ data: msg.data });
        } else if (msg.type === 'log') {
          _onWsLog(msg.data);
        }
      } catch (e) { console.error('[WS] parse error:', e); }
    };

    const _onDisconnect = () => {
      if (_wsHeartbeatTimer) { clearTimeout(_wsHeartbeatTimer); _wsHeartbeatTimer = null; }
      if (_wsReconnectTimer) return;  // onerror fires before onclose — schedule only once
      setNetStatus('bad', 'Déconnecté');
      ['#ph-card-stats', '#orp-card-stats'].forEach(sel => {
        const el = $(sel);
        if (el) el.classList.add('is-stale');
      });
      debugLog('[WS] Disconnected, reconnecting in 3s...');
      _wsReconnectTimer = setTimeout(() => { _wsReconnectTimer = null; initWebSocket(); }, 3000);
    };
    _ws.onclose = _ws.onerror = _onDisconnect;
  }

  // Heure de démarrage de l'ESP32 estimée à partir de uptime_ms (mis à jour à chaque push sensor_data)
  let _bootEpochMs = null;
  let _lastNotifiedResetReason = null;

  function formatLogTimestamp(ms) {
    if (_bootEpochMs == null || ms == null) return ms ?? '';
    const d = new Date(_bootEpochMs + ms);
    const p = (n) => String(n).padStart(2, '0');
    return `${p(d.getHours())}:${p(d.getMinutes())}:${p(d.getSeconds())}`;
  }

  function _showResetToast(reason) {
    const existing = document.getElementById('_reset-toast');
    if (existing) existing.remove();
    const toast = document.createElement('div');
    toast.id = '_reset-toast';
    toast.className = 'reset-toast';
    const msg = document.createTextNode('Redémarrage inattendu détecté (raison : ' + reason + ') ');
    const btn = document.createElement('button');
    btn.textContent = '×';
    btn.setAttribute('aria-label', 'Fermer');
    btn.onclick = () => toast.remove();
    toast.setAttribute('role', 'alert');
    toast.setAttribute('aria-live', 'assertive');
    toast.appendChild(msg);
    toast.appendChild(btn);
    document.body.appendChild(toast);
    setTimeout(() => { if (toast.isConnected) toast.remove(); }, 15000);
  }

  function _onWsSensorData(json) {
    // Statut MQTT temps réel (feature-015) — mise à jour PRIORITAIRE en tête de
    // fonction, blindée par try/catch, pour garantir l'exécution même si une
    // étape ultérieure (loadSensorData, _showResetToast) lance une exception.
    try {
      if (json.mqtt_connected !== undefined && window._config) {
        window._config.mqtt_connected = json.mqtt_connected;
        updateMqttStatusIndicator(window._config.enabled, json.mqtt_connected);
      }
    } catch (e) { console.error('[mqtt-status] update failed:', e); }

    if (json.uptime_ms != null) _bootEpochMs = Date.now() - json.uptime_ms;
    const reason = json.reset_reason;
    if (reason && !['POWER_ON', 'SW_RESET', 'DEEP_SLEEP'].includes(reason) && reason !== _lastNotifiedResetReason) {
      _lastNotifiedResetReason = reason;
      _showResetToast(reason);
    }
    loadSensorData({ data: json, force: true, source: 'ws' });
  }

  function isLogEntryVisible(entry) {
    const level = (typeof entry === 'string' ? '' : (entry.level || '')).toUpperCase();
    if (level === 'INFO')     return $("#log_level_info")?.checked !== false;
    if (level === 'WARN' || level === 'WARNING') return $("#log_level_warn")?.checked !== false;
    if (level === 'ERROR')    return $("#log_level_error")?.checked !== false;
    if (level === 'CRITICAL') return $("#log_level_critical")?.checked !== false;
    if (level === 'DEBUG')    return $("#log_level_debug")?.checked !== false;
    return true; // niveaux inconnus : affichés par défaut
  }

  function renderLogs(scroll = true) {
    const content = $("#logs_content");
    if (!content) return;
    const filtered = allLogEntries.filter(isLogEntryVisible);
    if (filtered.length === 0) {
      content.textContent = "(vide)";
    } else {
      content.innerHTML = filtered.map(entry => {
        if (typeof entry === 'string') return `<div>${entry}</div>`;
        return `<div>[${formatLogTimestamp(entry.timestamp)}] ${entry.level || ''}: ${entry.message || ''}</div>`;
      }).join('');
    }
    const autoScroll = $("#logs_auto_scroll")?.checked !== false;
    const container = $("#logs_container");
    if ((scroll || autoScroll) && container) requestAnimationFrame(() => { container.scrollTop = container.scrollHeight; });
  }

  function _onWsLog(entry) {
    allLogEntries.push(entry);
    if (allLogEntries.length > 500) allLogEntries.shift();
    if ((entry.timestamp || 0) > lastLogTimestamp) lastLogTimestamp = entry.timestamp;

    // Toast pour les interruptions d'injection (sécurité chimique pool-chemistry).
    // Le firmware émet un log critical "[Injection] {pH|ORP} INTERROMPUE — filtration
    // arrêtée" quand updateManualInject() détecte la filtration KO en cours d'injection.
    // L'utilisateur doit voir ce message immédiatement, pas seulement dans le panneau logs.
    if (entry.level === 'CRITICAL' && entry.message &&
        entry.message.includes('[Injection]') && entry.message.includes('INTERROMPUE')) {
      const product = entry.message.includes('ORP') ? 'ORP/chlore' : 'pH';
      showToast(
        `Injection ${product} interrompue : la filtration s'est arrêtée. Relancez l'injection après reprise de la filtration.`,
        'error'
      );
    }

    // Mettre à jour l'affichage si le panneau logs est visible
    const content = $("#logs_content");
    if (!content || content.children.length === 0) return;
    if (!isLogEntryVisible(entry)) return;
    const div = document.createElement('div');
    div.textContent = `[${formatLogTimestamp(entry.timestamp)}] ${entry.level || ''}: ${entry.message || ''}`;
    content.appendChild(div);
    // Auto-scroll si activé
    if ($("#logs_auto_scroll")?.checked !== false) {
      const container = $("#logs_container");
      if (container) requestAnimationFrame(() => { container.scrollTop = container.scrollHeight; });
    }
  }

  function debugLog(msg) {
    if (!DEBUG) return;
    console.log(`[dbg] ${msg}`);
  }

  // ---------- Toast ----------
  function showToast(message, type = 'info') {
    const toast = document.createElement('div');
    toast.className = `toast toast--${type}`;
    toast.textContent = message;
    // Errors use role="alert" (assertive); info/success use role="status" (polite)
    if (type === 'error') {
      toast.setAttribute('role', 'alert');
      toast.setAttribute('aria-live', 'assertive');
    } else {
      toast.setAttribute('role', 'status');
      toast.setAttribute('aria-live', 'polite');
    }
    toast.setAttribute('aria-atomic', 'true');
    document.body.appendChild(toast);
    setTimeout(() => {
      toast.style.opacity = '0';
      setTimeout(() => toast.remove(), 300);
    }, 3000);
  }

  // ---------- Auth Helper ----------
  function authFetch(url, options = {}) {
    const token = sessionStorage.getItem('authToken');
    if (token) {
      options.headers = options.headers || {};
      options.headers['X-Auth-Token'] = token;
    }
    return fetch(url, options).then(response => {
      // Si 401, rediriger vers login
      if (response.status === 401) {
        sessionStorage.removeItem('authToken');
        window.location.href = '/login.html';
        throw new Error('Authentication required');
      }
      return response;
    });
  }

  function debugStart(label) {
    if (!DEBUG) return null;
    const t0 = performance.now();
    debugLog(`${label} -> start`);
    return {
      end(extra = "") {
        const dt = (performance.now() - t0).toFixed(0);
        debugLog(`${label} -> end ${dt}ms${extra ? " " + extra : ""}`);
      }
    };
  }

  function setNetStatus(state, text) {
    const dot = $("#net-dot");
    const label = $("#net-text");
    dot.classList.remove("ok", "mid", "bad");
    if (state) dot.classList.add(state);
    if (label) label.textContent = text || "";

    // Bannière hors ligne + blocage des contrôles
    const appEl = document.querySelector(".app");
    if (appEl) {
      if (state === "ok") {
        appEl.classList.remove("app--offline");
      } else {
        appEl.classList.add("app--offline");
      }
    }

    if (state === "bad") {
      const dash = "–";
      const els = {
        "#compact-ph": dash,
        "#compact-orp": dash,
        "#compact-temperature": dash,
        "#m-temp": dash,
        "#m-ph": dash,
        "#m-orp": dash,
      };
      for (const [id, val] of Object.entries(els)) {
        const el = $(id);
        if (el) el.textContent = val;
      }
    }
  }

  // ---------- Router ----------
  function getRoute() {
    const hash = window.location.hash || "#/dashboard";
    // Nettoyer les query params du hash avant de parser
    const clean = hash.replace(/^#/, "").split("?")[0];
    const parts = clean.split("/").filter(Boolean);
    if (parts.length === 0) return { view: "/dashboard" };
    if (parts[0] === "settings") return { view: "/settings", sub: parts[1] || "wifi" };
    return { view: `/${parts[0]}` };
  }

  function setActiveNav(routeObj) {
    const routeKey = routeObj.view;

    $$(".nav__item").forEach((a) => {
      const r = a.getAttribute("data-route");
      const isActive = r === routeKey;
      a.classList.toggle("is-active", isActive);
      if (isActive) {
        a.setAttribute("aria-current", "page");
      } else {
        a.removeAttribute("aria-current");
      }
    });
  }

  function showView(routeObj) {
    const perf = debugStart(`showView ${routeObj.view}`);
    $$(".view").forEach((v) => v.classList.remove("is-active"));

    if (routeObj.view === "/settings") {
      $("#view-settings")?.classList.add("is-active");
      showSettingsPanel(routeObj.sub || "wifi");
    } else {
      const viewId = `view-${routeObj.view.substring(1)}`;
      $(`#${viewId}`)?.classList.add("is-active");
      showSettingsPanel(null);
    }

    setActiveNav(routeObj);

    // Load sensor data when navigating to dashboard or calibration pages
    if (routeObj.view === "/dashboard" || routeObj.view === "/temperature" || routeObj.view === "/ph" || routeObj.view === "/orp") {
      loadSensorData({ force: lastSensorDataLoadTime === 0, source: "route-view" });
    }

    // Mobile: close sidebar after navigation
    closeSidebar();
    perf?.end();
  }

  // ---------- Charts (Dashboard) ----------
  const PH_MIN = 7.0;
  const PH_MAX = 7.4;
  const PH_AXIS_MIN_DEFAULT = 6;
  const PH_AXIS_MAX_DEFAULT = 9;
  const PH_ZONE_COLOR = 'rgba(239, 68, 68, 0.08)';
  const PH_LINE_COLOR = 'rgba(239, 68, 68, 0.7)';

  const ORP_MIN = 600;
  const ORP_MAX = 800;
  const ORP_AXIS_MIN_DEFAULT = 500;
  const ORP_AXIS_MAX_DEFAULT = 900;
  const ORP_ZONE_COLOR = 'rgba(239, 68, 68, 0.08)';
  const ORP_LINE_COLOR = 'rgba(239, 68, 68, 0.7)';
  const CHART_POINT_PX = 18;
  const CHART_SCROLL_EPS = 20;
  let chartAutoScroll = true;

  // Fonction pour calculer les limites dynamiques de l'axe Y
  function calculateAxisLimits(dataPoints, defaultMin, defaultMax, padding = 0.1) {
    // Filtrer les valeurs null/undefined
    const validData = dataPoints.filter(v => v != null && !isNaN(v));

    if (validData.length === 0) {
      return { min: defaultMin, max: defaultMax };
    }

    const dataMin = Math.min(...validData);
    const dataMax = Math.max(...validData);

    // Utiliser les valeurs par défaut si les données sont dans la plage
    let min = defaultMin;
    let max = defaultMax;

    // Étendre vers le bas si nécessaire
    if (dataMin < defaultMin) {
      const range = defaultMax - dataMin;
      min = dataMin - (range * padding);
    }

    // Étendre vers le haut si nécessaire
    if (dataMax > defaultMax) {
      const range = dataMax - defaultMin;
      max = dataMax + (range * padding);
    }

    return { min, max };
  }

  // Mini charts pour les cartes pH et ORP du dashboard
  function getMiniChartRGB(value, target, tolerance) {
    if (value == null || isNaN(value)) return [156, 163, 175];
    const dev = Math.abs(value - target) - tolerance;
    if (dev <= 0) return [34, 197, 94];
    const t = Math.min(1, dev / tolerance);
    return [
      Math.round(34 + (239 - 34) * t),
      Math.round(197 - (197 - 68) * t),
      Math.round(94 - (94 - 68) * t)
    ];
  }

  function getMiniChartColor(value, target, tolerance, alpha) {
    const [r, g, b] = getMiniChartRGB(value, target, tolerance);
    return `rgba(${r},${g},${b},${alpha ?? 1})`;
  }

  function createMiniLineChart(canvasId, getTarget, getTolerance, formatValue) {
    const canvas = document.getElementById(canvasId);
    if (!canvas) return null;
    const c2d = canvas.getContext('2d');

    return new Chart(c2d, {
      type: 'line',
      data: {
        labels: [],
        datasets: [{
          data: [],
          tension: 0.4,
          borderWidth: 1.5,
          pointRadius: 0,
          fill: true,
          backgroundColor: (context) => {
            const { ctx: c, chartArea, data } = context.chart;
            if (!chartArea) return 'rgba(34,197,94,0.12)';
            const vals = data.datasets[0]?.data ?? [];
            let lastVal = null;
            for (let i = vals.length - 1; i >= 0; i--) {
              if (vals[i] != null && !isNaN(vals[i])) { lastVal = vals[i]; break; }
            }
            const [r, gv, bv] = getMiniChartRGB(lastVal, getTarget(), getTolerance());
            const grad = c.createLinearGradient(0, chartArea.top, 0, chartArea.bottom);
            grad.addColorStop(0, `rgba(${r},${gv},${bv},0.22)`);
            grad.addColorStop(1, `rgba(${r},${gv},${bv},0.02)`);
            return grad;
          },
          segment: {
            borderColor: ctx => getMiniChartColor(
              (ctx.p0.parsed.y + ctx.p1.parsed.y) / 2, getTarget(), getTolerance()
            )
          },
          borderColor: '#22c55e'
        }]
      },
      options: {
        responsive: true,
        maintainAspectRatio: false,
        animation: false,
        layout: { padding: { top: 4, left: 2, right: 2, bottom: 0 } },
        plugins: {
          legend: { display: false },
          tooltip: {
            enabled: true,
            displayColors: false,
            backgroundColor: 'rgba(17,24,39,0.88)',
            titleColor: 'rgba(156,163,175,1)',
            bodyColor: '#f9fafb',
            titleFont: { size: 10 },
            bodyFont: { size: 13, weight: '700' },
            padding: { x: 10, y: 6 },
            cornerRadius: 8,
            callbacks: {
              title: ctx => ctx[0]?.label ?? '',
              label: ctx => formatValue ? formatValue(ctx.parsed.y) : String(ctx.parsed.y)
            }
          }
        },
        scales: {
          x: {
            ticks: { maxTicksLimit: 4, maxRotation: 0, font: { size: 9 }, color: 'rgba(75,85,99,0.7)' },
            grid: { display: false },
            border: { display: false }
          },
          y: {
            position: 'right',
            ticks: { maxTicksLimit: 3, font: { size: 9 }, color: 'rgba(75,85,99,0.7)', padding: 2 },
            grid: { color: 'rgba(0,0,0,0.06)', drawTicks: false },
            border: { display: false }
          }
        }
      }
    });
  }

  let lastMiniChartTimestamp = 0;

  function miniChartPointLabel(timestamp) {
    const ts = new Date(timestamp * 1000);
    const isToday = ts.toDateString() === new Date().toDateString();
    return isToday
      ? ts.toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' })
      : ts.toLocaleDateString([], { month: 'numeric', day: 'numeric' }) + ' '
        + ts.toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' });
  }

  async function loadMiniChartData(since = 0) {
    try {
      const url = since > 0
        ? `/get-history?range=3d&since=${since}`
        : '/get-history?range=3d';
      const resp = await authFetch(url);
      if (!resp.ok) return;
      const data = await resp.json();
      if (!data.history) return;

      const phLabels = [], phValues = [];
      const orpLabels = [], orpValues = [];
      const tempLabels = [], tempValues = [];
      let maxTs = since;

      data.history.forEach(point => {
        if (point.timestamp > maxTs) maxTs = point.timestamp;
        const label = miniChartPointLabel(point.timestamp);
        if (point.ph != null && !isNaN(point.ph)) {
          phLabels.push(label);
          phValues.push(Math.round(point.ph * 10) / 10);
        }
        if (point.orp != null && !isNaN(point.orp)) {
          orpLabels.push(label);
          orpValues.push(Math.round(point.orp));
        }
        if (point.temperature != null && !isNaN(point.temperature)) {
          tempLabels.push(label);
          tempValues.push(Math.round(point.temperature * 10) / 10);
        }
      });

      if (since === 0) {
        // Chargement initial : remplace toutes les données
        updateMiniChart(phMiniChart, phLabels, phValues, window._config?.ph_target ?? 7.2, 0.2);
        updateMiniChart(orpMiniChart, orpLabels, orpValues, window._config?.orp_target ?? 700, 50);
        updateMiniChart(tempMiniChart, tempLabels, tempValues, 26, 8);
      } else {
        // Mise à jour incrémentale : ajoute les nouveaux points
        appendMiniChart(phMiniChart, phLabels, phValues, window._config?.ph_target ?? 7.2, 0.2);
        appendMiniChart(orpMiniChart, orpLabels, orpValues, window._config?.orp_target ?? 700, 50);
        appendMiniChart(tempMiniChart, tempLabels, tempValues, 26, 8);
      }

      if (maxTs > lastMiniChartTimestamp) lastMiniChartTimestamp = maxTs;
    } catch (e) {
      console.error('Error loading mini chart data:', e);
    }
  }

  function appendMiniChart(chart, newLabels, newValues, target, tolerance) {
    if (!chart || !newLabels.length) return;
    chart.data.labels.push(...newLabels);
    chart.data.datasets[0].data.push(...newValues);
    // Recalcule la couleur de fond basée sur la dernière valeur
    const vals = chart.data.datasets[0].data;
    const hasData = vals.some(v => v != null && !isNaN(v));
    if (!hasData && target != null) {
      chart.options.scales.y.min = target - tolerance * 4;
      chart.options.scales.y.max = target + tolerance * 4;
    } else {
      chart.options.scales.y.min = undefined;
      chart.options.scales.y.max = undefined;
    }
    chart.update('none');
  }

  function updateMiniChart(chart, labels, values, target, tolerance) {
    if (!chart) return;
    chart.data.labels = labels;
    chart.data.datasets[0].data = values;
    // Quand il n'y a pas de données, définir une plage Y sensée autour de la cible
    const hasData = values.some(v => v != null && !isNaN(v));
    if (!hasData && target != null && tolerance != null) {
      chart.options.scales.y.min = target - tolerance * 4;
      chart.options.scales.y.max = target + tolerance * 4;
    } else {
      chart.options.scales.y.min = undefined;
      chart.options.scales.y.max = undefined;
    }
    chart.update('none');
  }

  function createLineChart(ctx, color, label, options = {}) {
    const {
      integerOnly = false,
      yMin = null,
      yMax = null,
      annotation = null,
      fill = true,
      backgroundColor = null,
      extraPlugins = [],
      hideYAxis = false,
      showYAxisGrid = true
    } = options;

    const yAxisConfig = {
      beginAtZero: false,
      grid: {
        color: 'rgba(0, 0, 0, 0.05)',
        display: showYAxisGrid,
        drawTicks: !hideYAxis
      },
      ticks: { color: '#4b5563' },
      display: true
    };

    if (yMin !== null) yAxisConfig.min = yMin;
    if (yMax !== null) yAxisConfig.max = yMax;

    if (integerOnly) {
      yAxisConfig.ticks.callback = function (value) {
        if (Number.isInteger(value)) return value;
      };
    }

    if (hideYAxis) {
      yAxisConfig.ticks.display = false;
    }

    const chartConfig = {
      type: "line",
      data: {
        labels: [],
        datasets: [
          {
            label,
            data: [],
            borderColor: color,
            backgroundColor: backgroundColor ?? (color + '20'),
            tension: 0.3,
            borderWidth: 2,
            fill,
          },
        ],
      },
      options: {
        responsive: true,
        maintainAspectRatio: false,
        animation: false,
        scales: {
          x: {
            ticks: { maxRotation: 0, minRotation: 0, color: '#4b5563' },
            grid: { color: 'rgba(0, 0, 0, 0.05)' }
          },
          y: yAxisConfig,
        },
        plugins: {
          legend: { display: false },
          annotation: annotation ? { annotations: annotation } : undefined
        },
      },
      plugins: extraPlugins
    };

    return new Chart(ctx, chartConfig);
  }

  function bindDetailCharts() {
    document.querySelectorAll('.detail-chart-ranges').forEach(container => {
      container.querySelectorAll('.range-btn').forEach(btn => {
        btn.addEventListener('click', async () => {
          const range = btn.dataset.range;
          // Synchronise tous les sélecteurs de plage
          document.querySelectorAll('.detail-chart-ranges .range-btn').forEach(b => {
            b.classList.toggle('range-btn--active', b.dataset.range === range);
          });
          currentHistoryRange = range;
          await loadHistoricalData(range).catch(() => {});
        });
      });
    });
  }

  function buildPhReferenceDatasets() {
    return [
      {
        label: 'Zone hors plage (haute)',
        data: [],
        backgroundColor: PH_ZONE_COLOR,
        borderWidth: 0,
        fill: '+1',
        pointRadius: 0,
        tension: 0,
        order: 10
      },
      {
        label: `pH Max (${PH_MAX.toFixed(1)})`,
        data: [],
        borderColor: PH_LINE_COLOR,
        borderWidth: 2,
        fill: false,
        pointRadius: 0,
        tension: 0,
        order: 5
      },
      {
        label: `pH Min (${PH_MIN.toFixed(1)})`,
        data: [],
        borderColor: PH_LINE_COLOR,
        borderWidth: 2,
        fill: false,
        pointRadius: 0,
        tension: 0,
        order: 5
      },
      {
        label: 'Zone hors plage (basse)',
        data: [],
        backgroundColor: PH_ZONE_COLOR,
        borderWidth: 0,
        fill: '-1',
        pointRadius: 0,
        tension: 0,
        order: 10
      }
    ];
  }

  function ensurePhReferenceDatasets(chart) {
    if (!chart) return;
    if (chart.data.datasets.length === 1) {
      chart.data.datasets.push(...buildPhReferenceDatasets());
    }
  }

  function syncPhReferenceDatasets(chart) {
    if (!chart || chart.data.datasets.length < 5) return;
    const points = chart.data.labels.length;
    const build = (value) => Array(points).fill(value);

    const dataPoints = chart.data.datasets[0]?.data || [];
    const limits = calculateAxisLimits(dataPoints, PH_AXIS_MIN_DEFAULT, PH_AXIS_MAX_DEFAULT);

    chart.options.scales.y.min = limits.min;
    chart.options.scales.y.max = limits.max;
    chart.data.datasets[1].data = build(limits.max);
    chart.data.datasets[2].data = build(PH_MAX);
    chart.data.datasets[3].data = build(PH_MIN);
    chart.data.datasets[4].data = build(limits.min);
  }

  function buildOrpReferenceDatasets() {
    return [
      {
        label: 'Zone hors plage (haute)',
        data: [],
        backgroundColor: ORP_ZONE_COLOR,
        borderWidth: 0,
        fill: '+1',
        pointRadius: 0,
        tension: 0,
        order: 10
      },
      {
        label: `ORP Max (${ORP_MAX}mV)`,
        data: [],
        borderColor: ORP_LINE_COLOR,
        borderWidth: 2,
        fill: false,
        pointRadius: 0,
        tension: 0,
        order: 5
      },
      {
        label: `ORP Min (${ORP_MIN}mV)`,
        data: [],
        borderColor: ORP_LINE_COLOR,
        borderWidth: 2,
        fill: false,
        pointRadius: 0,
        tension: 0,
        order: 5
      },
      {
        label: 'Zone hors plage (basse)',
        data: [],
        backgroundColor: ORP_ZONE_COLOR,
        borderWidth: 0,
        fill: '-1',
        pointRadius: 0,
        tension: 0,
        order: 10
      }
    ];
  }

  function ensureOrpReferenceDatasets(chart) {
    if (!chart) return;
    if (chart.data.datasets.length === 1) {
      chart.data.datasets.push(...buildOrpReferenceDatasets());
    }
  }

  function syncOrpReferenceDatasets(chart) {
    if (!chart || chart.data.datasets.length < 5) return;
    const points = chart.data.labels.length;
    const build = (value) => Array(points).fill(value);

    const dataPoints = chart.data.datasets[0]?.data || [];
    const limits = calculateAxisLimits(dataPoints, ORP_AXIS_MIN_DEFAULT, ORP_AXIS_MAX_DEFAULT);

    chart.options.scales.y.min = limits.min;
    chart.options.scales.y.max = limits.max;
    chart.data.datasets[1].data = build(limits.max);
    chart.data.datasets[2].data = build(ORP_MAX);
    chart.data.datasets[3].data = build(ORP_MIN);
    chart.data.datasets[4].data = build(limits.min);
  }

  async function loadHistoricalData(range = 'all') {
    try {
      const response = await authFetch(`/get-history?range=${range}`);
      if (!response.ok) throw new Error('Failed to load history');

      const data = await response.json();
      const history = data.history || [];

      // Agréger par jour calendaire — dernier point connu par jour par capteur
      // Clé ISO YYYY-MM-DD (tri lexicographique correct, pas de re-parsing par Safari)
      // Noms de mois en dur pour éviter toLocaleDateString("fr-FR") qui lance SyntaxError dans certains Safari
      const MOIS_FR = ['janv.','févr.','mars','avr.','mai','juin','juil.','août','sept.','oct.','nov.','déc.'];
      const nowD = new Date();
      const todayKey = `${nowD.getFullYear()}-${String(nowD.getMonth()+1).padStart(2,'0')}-${String(nowD.getDate()).padStart(2,'0')}`;
      const dayMap = new Map();

      history.forEach(point => {
        const d = new Date(point.timestamp * 1000);
        if (isNaN(d.getTime())) return;
        const key = `${d.getFullYear()}-${String(d.getMonth()+1).padStart(2,'0')}-${String(d.getDate()).padStart(2,'0')}`;
        if (!dayMap.has(key)) {
          const isToday = key === todayKey;
          const label = isToday
            ? "Aujourd'hui"
            : `${d.getDate()} ${MOIS_FR[d.getMonth()]}`;
          dayMap.set(key, { label, ph: null, orp: null, temperature: null });
        }
        const entry = dayMap.get(key);
        if (point.ph != null && !isNaN(point.ph)) entry.ph = Math.round(point.ph * 10) / 10;
        if (point.orp != null && !isNaN(point.orp)) entry.orp = Math.round(point.orp);
        if (point.temperature != null && !isNaN(point.temperature)) entry.temperature = point.temperature;
      });

      // Si aujourd'hui absent de l'historique mais données temps réel disponibles, l'injecter
      if (!dayMap.has(todayKey) && latestSensorData) {
        const s = latestSensorData;
        dayMap.set(todayKey, {
          label: "Aujourd'hui",
          ph: s.ph != null ? Math.round(s.ph * 10) / 10 : null,
          orp: s.orp != null ? Math.round(s.orp) : null,
          temperature: s.temperature ?? null
        });
      }

      // Trier par date croissante — tri lexicographique direct sur YYYY-MM-DD
      const entries = [...dayMap.entries()]
        .sort((a, b) => (a[0] < b[0] ? -1 : a[0] > b[0] ? 1 : 0))
        .map(([, e]) => e);

      if (tempChart) {
        const labels = [], values = [];
        entries.forEach(e => { if (e.temperature != null) { labels.push(e.label); values.push(e.temperature); } });
        tempChart.data.labels = labels;
        tempChart.data.datasets[0].data = values;
        tempChart.update('none');
      }

      if (phChart) {
        const labels = [], values = [];
        entries.forEach(e => { if (e.ph != null) { labels.push(e.label); values.push(e.ph); } });
        phChart.data.labels = labels;
        phChart.data.datasets[0].data = values;
        syncPhReferenceDatasets(phChart);
        phChart.update('none');
      }

      if (orpChart) {
        const labels = [], values = [];
        entries.forEach(e => { if (e.orp != null) { labels.push(e.label); values.push(e.orp); } });
        orpChart.data.labels = labels;
        orpChart.data.datasets[0].data = values;
        syncOrpReferenceDatasets(orpChart);
        orpChart.update('none');
      }

      debugLog(`Loaded ${history.length} points → ${dayMap.size} jours agrégés (${range})`);
    } catch (error) {
      console.error('Error loading historical data:', error);
      if (error && error.stack) console.error('Stack:', error.stack);
    }
  }

  // Met à jour uniquement le point "Aujourd'hui" sur les graphiques détaillés
  function updateTodayOnCharts(sensorData) {
    const todayLabel = "Aujourd'hui";

    function updateChart(chart, value) {
      if (!chart || value == null || isNaN(value)) return;
      const labels = chart.data.labels;
      const values = chart.data.datasets[0].data;
      if (labels.length > 0 && labels[labels.length - 1] === todayLabel) {
        values[values.length - 1] = value;
      } else {
        labels.push(todayLabel);
        values.push(value);
      }
    }

    if (tempChart && sensorData.temperature != null) {
      updateChart(tempChart, sensorData.temperature);
      tempChart.update('none');
    }
    if (!phCalibrationActive && phChart && sensorData.ph != null) {
      updateChart(phChart, Math.round(sensorData.ph * 10) / 10);
      syncPhReferenceDatasets(phChart);
      phChart.update('none');
    }
    if (!orpCalibrationActive && orpChart && sensorData.orp != null) {
      updateChart(orpChart, Math.round(sensorData.orp));
      syncOrpReferenceDatasets(orpChart);
      orpChart.update('none');
    }
  }

  let tempChart, phChart, orpChart;
  let phMiniChart = null, orpMiniChart = null, tempMiniChart = null;
  let currentHistoryRange = 'all';

  // ---------- State ----------
  let latestSensorData = null;
  let consecutiveFailures = 0; // Track consecutive fetch failures

  let cachedManualStart = "08:00";
  let cachedManualEnd = "20:00";

  // ---------- Settings UI (segmented + panels) ----------
  function showSettingsPanel(panelKey) {
    if (!panelKey) {
      // Pas dans settings, masquer tout
      $$(".panel").forEach((p) => p.classList.remove("is-active"));
      return;
    }

    $$(".panel").forEach((p) => p.classList.remove("is-active"));
    $(`.panel[data-settings-panel="${panelKey}"]`)?.classList.add("is-active");

    // Update segmented buttons (navigation tabs only)
    $$(".segmented__btn[data-settings-tab]").forEach((b) => {
      b.classList.remove("is-active");
      b.setAttribute("aria-selected", "false");
    });
    const activeBtn = $(`.segmented__btn[data-settings-tab="${panelKey}"]`);
    if (activeBtn) {
      activeBtn.classList.add("is-active");
      activeBtn.setAttribute("aria-selected", "true");
    }

    // Update WiFi display when WiFi panel is shown
    if (panelKey === "wifi") {
      updateWiFiDisplay();
      checkWiFiNotification();
    }

    // Re-render du badge MQTT à l'activation du panel : garantit l'affichage
    // correct même si le bloc mqtt_connected dans _onWsSensorData n'a pas
    // propagé jusqu'à window._config (cf. feature-015).
    if (panelKey === "mqtt") {
      const connected = latestSensorData?.mqtt_connected ?? window._config?.mqtt_connected ?? false;
      updateMqttStatusIndicator(window._config?.enabled, connected);
    }
  }

  // Vérifier et afficher une notification WiFi basée sur les paramètres URL
  function checkWiFiNotification() {
    const urlParams = new URLSearchParams(window.location.search);
    const wifiStatus = urlParams.get('wifi');
    const ssid = urlParams.get('ssid');

    if (!wifiStatus) return;

    const notifEl = $("#wifi-notification");
    if (!notifEl) return;

    // Nettoyer l'URL sans recharger la page
    const cleanUrl = window.location.pathname + window.location.hash.split('?')[0];
    window.history.replaceState({}, document.title, cleanUrl);

    // Afficher la notification
    notifEl.classList.remove('hidden', 'success', 'error');

    if (wifiStatus === 'success') {
      notifEl.classList.add('success');
      notifEl.setAttribute('role', 'status');
      notifEl.setAttribute('aria-live', 'polite');
      notifEl.innerHTML = `
        <span class="wifi-notification__icon" aria-hidden="true">✓</span>
        <span class="wifi-notification__text">Connexion au réseau "${ssid || 'WiFi'}" réussie</span>
        <button class="wifi-notification__close" type="button" aria-label="Fermer la notification" onclick="hideWifiNotification()">×</button>
      `;
    } else if (wifiStatus === 'failed') {
      notifEl.classList.add('error');
      notifEl.setAttribute('role', 'alert');
      notifEl.setAttribute('aria-live', 'assertive');
      notifEl.innerHTML = `
        <span class="wifi-notification__icon" aria-hidden="true">✕</span>
        <span class="wifi-notification__text">Échec de la connexion au réseau "${ssid || 'WiFi'}"</span>
        <button class="wifi-notification__close" type="button" aria-label="Fermer la notification" onclick="hideWifiNotification()">×</button>
      `;
    }

    // Masquer automatiquement après 8 secondes
    setTimeout(() => {
      hideWifiNotification();
    }, 8000);
  }

  // Fonction globale pour fermer la notification
  window.hideWifiNotification = function() {
    const notifEl = $("#wifi-notification");
    if (notifEl) {
      notifEl.classList.add('hidden');
    }
  };

  function goSettings(panelKey) {
    window.location.hash = `#/settings/${panelKey}`;
  }

  // ---------- Config helpers ----------
  function sendConfig(data) {
    return authFetch("/save-config", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(data),
    })
      .then((res) => {
        if (!res.ok) throw new Error("save-config failed");
        return true;
      })
      .catch(() => false);
  }

  function updateMqttStatusIndicator(enabled, mqttConnected) {
    const pill = $("#mqtt_status_pill");
    if (!pill) return;

    pill.classList.remove("ok", "mid", "bad");
    if (!enabled) {
      pill.textContent = "Désactivé";
      return;
    }
    if (mqttConnected === true) {
      pill.textContent = "Connecté";
      pill.classList.add("ok");
    } else {
      pill.textContent = "Non connecté";
      pill.classList.add("bad");
    }
  }

  function updateFiltrationControls() {
    const modeSelect = $("#filtration_mode");
    const start = $("#filtration_start");
    const end = $("#filtration_end");
    const autoHint = $("#filtration_auto_hint");
    const autoOption = modeSelect?.querySelector('option[value="auto"]');
    if (!modeSelect || !start || !end) return;

    // Auto disponible uniquement si la fonctionnalité température est active ET qu'une valeur est mesurée.
    // Si les données capteur ne sont pas encore chargées, on ne bloque pas (pas de faux positif au démarrage).
    const tempEnabled = $("#temperature_enabled")?.checked ?? false;
    const sensorDataLoaded = latestSensorData != null;
    const tempValue = latestSensorData?.temperature;
    const tempMeasured = !sensorDataLoaded || (tempEnabled && tempValue != null && !isNaN(tempValue));

    if (autoOption) {
      autoOption.disabled = !tempMeasured;
    }

    if (autoHint) {
      if (!tempEnabled) {
        autoHint.textContent = "Le mode Auto nécessite l'activation de la mesure de température.";
        autoHint.style.display = "block";
      } else if (!tempMeasured) {
        autoHint.textContent = "Le mode Auto nécessite une mesure de température valide. Vérifiez le capteur.";
        autoHint.style.display = "block";
      } else {
        autoHint.style.display = "none";
      }
    }

    const mode = modeSelect.value || "auto";
    const manualCard = $("#filtration-manual-card");
    const scheduleFields = $("#filtration-schedule-fields");
    const startBtn = $("#filtration-manual-start");
    const stopBtn = $("#filtration-manual-stop");
    const modeHint = $("#filtration_mode_hint");

    // La carte Contrôle manuel est toujours visible
    if (manualCard) manualCard.style.display = "";
    // Boutons désactivés uniquement en mode "Désactivé"
    const buttonsDisabled = (mode === "off");
    if (startBtn) startBtn.disabled = buttonsDisabled;
    if (stopBtn) stopBtn.disabled = buttonsDisabled;

    if (mode === "manual") {
      // Programmation : horaires éditables
      if (scheduleFields) scheduleFields.style.display = "";
      if (modeHint) modeHint.style.display = "none";
      start.disabled = false;
      end.disabled = false;
      start.classList.remove("is-computed");
      end.classList.remove("is-computed");
    } else if (mode === "auto") {
      // Auto : horaires calculés (lecture seule)
      if (scheduleFields) scheduleFields.style.display = "";
      if (modeHint) modeHint.style.display = "";
      start.disabled = true;
      end.disabled = true;
      start.classList.add("is-computed");
      end.classList.add("is-computed");
    } else {
      // Manuel ou Désactivé : pas d'horaires
      if (scheduleFields) scheduleFields.style.display = "none";
      if (modeHint) modeHint.style.display = "none";
      start.disabled = true;
      end.disabled = true;
    }
  }

  // Variable pour stocker l'heure complète avec secondes (mode manuel)
  let fullTimeValue = "";

  function updateTimeControls(current, syncing = false) {
    const useNtp = $("#time_use_ntp")?.checked ?? true;
    const ntp = $("#time_ntp_server");
    const timeValue = $("#time_value");
    const timeSaveBtn = $("#time_save_btn");
    const syncSpinner = $("#time_sync_spinner");

    // Si NTP activé: serveur NTP actif, champ heure en lecture seule avec heure du serveur
    // Si NTP désactivé: serveur NTP grisé, champ heure modifiable
    if (ntp) ntp.disabled = !useNtp;

    if (useNtp && syncing) {
      // NTP activé mais en cours de sync : afficher spinner, masquer champ
      if (syncSpinner) syncSpinner.style.display = "flex";
      if (timeValue) timeValue.style.display = "none";
    } else {
      // Sync terminée ou mode manuel : masquer spinner, afficher champ
      if (syncSpinner) syncSpinner.style.display = "none";
      if (timeValue) {
        timeValue.style.display = "block";
        timeValue.readOnly = useNtp;
        if (current) {
          // Stocker l'heure complète pour le mode manuel
          fullTimeValue = current;
          // Toujours masquer les secondes à l'affichage (sauf si en cours d'édition)
          let displayValue = current;
          if (displayValue.length > 16) {
            displayValue = displayValue.substring(0, 16);
          }
          timeValue.value = displayValue;
        }
      }
    }

    if (timeSaveBtn) {
      timeSaveBtn.style.display = "inline-flex";
    }
  }

  // Toggle visibility of feature content based on enable switch
  function updateFeatureVisibility(feature) {
    const switchMap = {
      filtration: "filtration_enabled",
      lighting: "lighting_feature_enabled",
      temperature: "temperature_enabled"
      // orp n'est plus un toggle binaire — la visibilité est gérée par updateOrpModeControls()
    };
    const contentMap = {
      filtration: "filtration-content",
      lighting: "lighting-content",
      temperature: "temperature-content"
    };
    const dashboardCardMap = {
      filtration: "dashboard-filtration-card",
      lighting: "dashboard-lighting-card",
      temperature: "dashboard-temperature-card"
    };

    const switchId = switchMap[feature];
    const contentId = contentMap[feature];
    const dashboardCardId = dashboardCardMap[feature];
    if (!switchId || !contentId) return;

    const switchEl = $(`#${switchId}`);
    const contentEl = $(`#${contentId}`);
    const dashboardCardEl = dashboardCardId ? $(`#${dashboardCardId}`) : null;
    if (!switchEl || !contentEl) return;

    const enabled = switchEl.checked;
    contentEl.style.display = enabled ? "block" : "none";
    if (dashboardCardEl) {
      dashboardCardEl.style.display = enabled ? "" : "none";
    }
  }

  function updatePhModeControls() {
    const mode = getSegmented("ph_regulation_mode") || "automatic";
    const params = $("#ph-regulation-params");      // sous-bloc Auto
    const scheduled = $("#ph-params-scheduled");    // sous-bloc Programmée
    const inject = $("#ph-inject-section");         // Injection manuelle

    if (params)    params.style.display    = mode === "automatic"  ? "" : "none";
    if (scheduled) scheduled.style.display = mode === "scheduled"  ? "" : "none";
    if (inject)    inject.style.display    = mode === "manual"     ? "" : "none";
    // feature-034 itération 3 (volet A) : le bouton Calibrer la sonde vit désormais
    // sous la rangée des badges (#ph-card-stats, toujours visible) → accessible
    // dans tous les modes par construction, sans gestion de display ici.
  }

  function updatePhControls() {
    updatePhModeControls();
  }

  function updateOrpModeControls() {
    const mode = getSegmented("orp_regulation_mode") || "automatic";
    const params = $("#orp-regulation-params");      // sous-bloc Automatique
    const scheduled = $("#orp-params-scheduled");    // sous-bloc Programmée
    const inject = $("#orp-inject-section");         // Injection manuelle

    if (params)    params.style.display    = mode === "automatic"  ? "" : "none";
    if (scheduled) scheduled.style.display = mode === "scheduled"  ? "" : "none";
    if (inject)    inject.style.display    = mode === "manual"     ? "" : "none";
    // feature-034 itération 3 (volet A) : le bouton Calibrer la sonde vit désormais
    // sous la rangée des badges (#orp-card-stats, toujours visible) → accessible
    // dans tous les modes par construction, sans gestion de display ici.
  }

  function updateOrpControls() {
    updateOrpModeControls();
    updateFeatureVisibility("orp");
  }

  function collectConfig() {
    const mqttEnabled = $("#mqtt_enabled");

    const portValue = parseInt($("#mqtt_port")?.value || "1883", 10);
    const phValue = parseFloat($("#ph_target")?.value || "7.2");
    const orpValue = parseFloat($("#orp_target")?.value || "650");

    const phPumpValue = parseInt($("#ph_pump")?.value || "1", 10);
    const orpPumpValue = parseInt($("#orp_pump")?.value || "2", 10);
    const pump1MaxDutyPct = parseInt($("#pump1_max_duty")?.value || "100", 10);
    const pump2MaxDutyPct = parseInt($("#pump2_max_duty")?.value || "100", 10);

    const phLimitValue = parseInt($("#ph_limit")?.value || "5", 10);
    const orpLimitValue = parseInt($("#orp_limit")?.value || "10", 10);
    const phDailyLimitValue = parseFloat($("#ph_daily_limit")?.value || "300");
    const orpDailyLimitValue = parseFloat($("#orp_daily_limit")?.value || "500");
    const phCorrectionType = $("#ph_correction_type")?.value || "ph_minus";

    const timeUseNtp = $("#time_use_ntp")?.checked ?? true;
    const timeNtpServer = $("#time_ntp_server")?.value || "pool.ntp.org";
    const timeTimezone = $("#time_timezone")?.value || "europe_paris";
    const timeValue = $("#time_value")?.value || "";

    const lightingFeatureEnabled = $("#lighting_feature_enabled")?.checked ?? true;
    const lightingScheduleMode = $("#lighting_schedule_mode")?.value || "disabled";
    const lightingStartTime = $("#lighting_start_time")?.value || "20:00";
    const lightingEndTime = $("#lighting_end_time")?.value || "23:00";

    const temperatureEnabled = $("#temperature_enabled")?.checked ?? true;

    return {
      enabled: mqttEnabled?.checked ?? true,
      server: $("#mqtt_server")?.value || "",
      port: isNaN(portValue) ? 1883 : portValue,
      topic: $("#mqtt_topic")?.value || "",
      username: $("#mqtt_username")?.value || "",
      password: $("#mqtt_password")?.value || "",
      ph_target: isNaN(phValue) ? 7.2 : phValue,
      orp_target: isNaN(orpValue) ? 650 : orpValue,
      ph_enabled: (window._config?.ph_regulation_mode || "automatic") !== "manual",
      ph_regulation_mode: window._config?.ph_regulation_mode || "automatic",
      ph_daily_target_ml: parseInt($("#ph_daily_target_ml")?.value ?? "0", 10) || 0,
      ph_pump: isNaN(phPumpValue) ? 1 : phPumpValue,
      orp_regulation_mode: window._config?.orp_regulation_mode || "automatic",
      orp_enabled: (window._config?.orp_regulation_mode || "automatic") !== "manual",
      orp_daily_target_ml: parseInt($("#orp_daily_target_ml")?.value ?? "0", 10) || 0,
      orp_pump: isNaN(orpPumpValue) ? 2 : orpPumpValue,
      pump1_max_duty_pct: isNaN(pump1MaxDutyPct) ? 50 : Math.min(100, Math.max(0, pump1MaxDutyPct)),
      pump2_max_duty_pct: isNaN(pump2MaxDutyPct) ? 50 : Math.min(100, Math.max(0, pump2MaxDutyPct)),
      ph_limit_minutes: isNaN(phLimitValue) ? 5 : Math.min(60, Math.max(1, phLimitValue)),
      orp_limit_minutes: isNaN(orpLimitValue) ? 10 : Math.min(60, Math.max(1, orpLimitValue)),
      max_ph_ml_per_day: isNaN(phDailyLimitValue) ? 300 : phDailyLimitValue,
      max_chlorine_ml_per_day: isNaN(orpDailyLimitValue) ? 500 : orpDailyLimitValue,
      ph_correction_type: phCorrectionType,
      time_use_ntp: timeUseNtp,
      ntp_server: timeNtpServer,
      manual_time: timeValue,
      timezone_id: timeTimezone,
      lighting_feature_enabled: lightingFeatureEnabled,
      lighting_schedule_enabled: lightingScheduleMode === "enabled",
      lighting_start_time: lightingStartTime,
      lighting_end_time: lightingEndTime,
      temperature_enabled: temperatureEnabled,
      sensor_logs_enabled: $("#sensor_logs_enabled")?.checked === true,
      debug_logs_enabled: $("#debug_logs_enabled")?.checked === true,
      screen_enabled: $("#screen_enabled")?.checked === true,
    };
  }

  // Séparé de collectConfig() pour éviter qu'un save() ORP/pH
  // n'écrase accidentellement le mode de filtration en cours.
  function collectFiltrationConfig() {
    return {
      filtration_enabled: $("#filtration_enabled")?.checked ?? true,
      filtration_mode: $("#filtration_mode")?.value || "auto",
      filtration_start: $("#filtration_start")?.value || "08:00",
      filtration_end: $("#filtration_end")?.value || "20:00",
    };
  }

  function collectMqttConfig() {
    const mqttEnabled = $("#mqtt_enabled");
    const portValue = parseInt($("#mqtt_port")?.value || "1883", 10);

    return {
      enabled: mqttEnabled?.checked ?? true,
      server: $("#mqtt_server")?.value || "",
      port: isNaN(portValue) ? 1883 : portValue,
      topic: $("#mqtt_topic")?.value || "",
      username: $("#mqtt_username")?.value || "",
      password: $("#mqtt_password")?.value || "",
    };
  }

  function collectTimeConfig() {
    const timeUseNtp = $("#time_use_ntp")?.checked ?? true;
    const timeNtpServer = $("#time_ntp_server")?.value || "pool.ntp.org";
    const timeTimezone = $("#time_timezone")?.value || "europe_paris";
    const timeValue = $("#time_value")?.value || "";

    return {
      time_use_ntp: timeUseNtp,
      ntp_server: timeNtpServer,
      timezone_id: timeTimezone,
      manual_time: timeValue,
    };
  }

  async function loadConfig(options = {}) {
    let cfg;
    if (options.data) {
      cfg = options.data;
    } else {
      const res = await authFetch("/get-config");
      cfg = await res.json();
    }

    // Stocker la config globalement pour les alertes et cartes status
    window._config = cfg;

    const activeId = document.activeElement?.id || "";
    const mqttEditing = ["mqtt_server", "mqtt_port", "mqtt_topic", "mqtt_username", "mqtt_password", "mqtt_enabled"].includes(activeId);

    if (!mqttEditing) {
      $("#mqtt_server").value = cfg.server || "";
      $("#mqtt_port").value = cfg.port || 1883;
      $("#mqtt_topic").value = cfg.topic || "";
      $("#mqtt_username").value = cfg.username || "";
      $("#mqtt_password").value = cfg.password || "";
      $("#mqtt_enabled").checked = cfg.enabled !== false;
    }

    updateMqttStatusIndicator(cfg.enabled, cfg.mqtt_connected);

    $("#ph_target").value = typeof cfg.ph_target === "number" ? cfg.ph_target : 7.2;
    $("#orp_target").value = typeof cfg.orp_target === "number" ? cfg.orp_target : 650;

    setSegmented("ph_regulation_mode", cfg.ph_regulation_mode || "automatic");
    updatePhModeControls();
    if ($("#ph_daily_target_ml") && cfg.ph_daily_target_ml != null)
      $("#ph_daily_target_ml").value = cfg.ph_daily_target_ml;
    const maxMl = typeof cfg.max_ph_ml_per_day === "number" ? cfg.max_ph_ml_per_day : 0;
    const hint = $("#ph_daily_target_hint");
    if (hint) hint.textContent = maxMl > 0 ? `limite de sécurité : ${maxMl} mL` : "";
    if ($("#ph_daily_target_ml") && maxMl > 0) $("#ph_daily_target_ml").max = maxMl;

    // ORP regulation mode
    setSegmented("orp_regulation_mode", cfg.orp_regulation_mode || "automatic");
    updateOrpModeControls();
    if ($("#orp_daily_target_ml") && cfg.orp_daily_target_ml != null)
      $("#orp_daily_target_ml").value = cfg.orp_daily_target_ml;
    const maxOrpMl = typeof cfg.max_orp_ml_per_day === "number" ? cfg.max_orp_ml_per_day
                   : (typeof cfg.max_chlorine_ml_per_day === "number" ? cfg.max_chlorine_ml_per_day : 0);
    const orpHint = $("#orp_daily_target_hint");
    if (orpHint) orpHint.textContent = maxOrpMl > 0 ? `limite de sécurité : ${maxOrpMl} mL` : "";
    if ($("#orp_daily_target_ml") && maxOrpMl > 0) $("#orp_daily_target_ml").max = maxOrpMl;
    updateFeatureVisibility("orp");

    $("#ph_pump").value = cfg.ph_pump === 2 ? "2" : "1";
    $("#orp_pump").value = cfg.orp_pump === 1 ? "1" : "2";

    const p1max = typeof cfg.pump1_max_duty_pct === "number" ? cfg.pump1_max_duty_pct : 100;
    const p2max = typeof cfg.pump2_max_duty_pct === "number" ? cfg.pump2_max_duty_pct : 100;
    if ($("#pump1_max_duty")) { $("#pump1_max_duty").value = p1max; $("#pump1_max_duty_value").textContent = String(p1max); }
    if ($("#pump2_max_duty")) { $("#pump2_max_duty").value = p2max; $("#pump2_max_duty_value").textContent = String(p2max); }
    if ($("#pump_max_flow_ml_per_min")) $("#pump_max_flow_ml_per_min").value = typeof cfg.pump_max_flow_ml_per_min === "number" ? cfg.pump_max_flow_ml_per_min : 90;

    $("#ph_limit").value = typeof cfg.ph_limit_minutes === "number" ? cfg.ph_limit_minutes : 5;
    $("#orp_limit").value = typeof cfg.orp_limit_minutes === "number" ? cfg.orp_limit_minutes : 10;
    $("#ph_daily_limit").value = typeof cfg.max_ph_ml_per_day === "number" ? cfg.max_ph_ml_per_day : 300;
    $("#orp_daily_limit").value = typeof cfg.max_chlorine_ml_per_day === "number" ? cfg.max_chlorine_ml_per_day : 500;
    $("#regulation_mode").value = cfg.regulation_mode || "pilote";
    if ($("#stabilization_delay_min") && cfg.stabilization_delay_min != null) {
      $("#stabilization_delay_min").value = cfg.stabilization_delay_min;
    }
    setSegmented("regulation_speed", cfg.regulation_speed || "normal");
    $("#ph_correction_type").value = cfg.ph_correction_type || "ph_minus";

    // pH/ORP calibration info — Atlas EZO : basé sur phCalPoints / orpCalPoints (WS sensor_data)
    renderCalibrationStatus();

    // Temperature enabled and calibration info
    $("#temperature_enabled").checked = cfg.temperature_enabled !== false;
    updateFeatureVisibility("temperature");

    const tempCalibrated = cfg.temp_calibration_date && cfg.temp_calibration_date !== "";
    const tempCalibratedStatus = $("#temp_calibrated_status");
    const tempCalDate = $("#temp_cal_date");

    if (tempCalibratedStatus) {
      tempCalibratedStatus.style.display = tempCalibrated ? "block" : "none";
    }

    const tempCalDateHeader = $("#temp_cal_date_header");
    let tempCalText = "Dernière calibration : —";
    if (tempCalibrated) {
      const d = new Date(cfg.temp_calibration_date);
      const offset = cfg.temp_calibration_offset;
      tempCalText = "Dernière calibration : " + d.toLocaleString("fr-FR");
      if (offset != null && !isNaN(offset)) tempCalText += ` (offset: ${offset > 0 ? '+' : ''}${offset.toFixed(1)}°C)`;
    }
    if (tempCalDate) tempCalDate.textContent = tempCalText;
    if (tempCalDateHeader) tempCalDateHeader.textContent = tempCalText;

    // Filtration
    $("#filtration_enabled").checked = cfg.filtration_enabled !== false;
    if (!filtrationDirty) {
      // Ne pas écraser les modifications non sauvegardées de l'utilisateur
      if (cfg.filtration_mode) $("#filtration_mode").value = cfg.filtration_mode;
      if (cfg.filtration_start) $("#filtration_start").value = cfg.filtration_start;
      if (cfg.filtration_end) $("#filtration_end").value = cfg.filtration_end;

      if ($("#filtration_mode").value === "manual") {
        cachedManualStart = $("#filtration_start").value;
        cachedManualEnd = $("#filtration_end").value;
      }
    }
    updateFiltrationControls();
    updateFeatureVisibility("filtration");

    // Time
    const timeActiveId = document.activeElement?.id || "";
    const timeEditing = ["time_use_ntp", "time_ntp_server", "time_timezone", "time_value"].includes(timeActiveId);

    if (!timeEditing) {
      const useNtp = cfg.time_use_ntp !== false;
      $("#time_use_ntp").checked = useNtp;
      $("#time_ntp_server").value = cfg.ntp_server || "pool.ntp.org";

      const tz = cfg.timezone_id || "europe_paris";
      if ($(`#time_timezone option[value="${tz}"]`)) $("#time_timezone").value = tz;

      // Toujours afficher l'heure actuelle du système (time_current)
      let timeValue = cfg.time_current || "";
      const year = parseInt(timeValue.substring(0, 4), 10);
      const timeValid = year >= 2021;

      // Si NTP activé mais heure non valide, afficher le spinner de sync
      const syncing = useNtp && !timeValid;
      updateTimeControls(timeValue, syncing);
    }

    // Wi-Fi summary - store for later update
    window._wifiData = {
      ssid: cfg.wifi_ssid ?? "—",
      ip: cfg.wifi_ip ?? "—",
      mode: cfg.wifi_mode ?? "—",
      mdns: cfg.mdns_host ?? "poolcontroller.local"
    };

    debugLog("WiFi data loaded");

    // Update WiFi display immediately after loading config
    updateWiFiDisplay();

    updatePhControls();
    updateOrpControls();

    // Security panel
    $("#auth_cors_origins").value = cfg.auth_cors_origins || "";

    // Development panel
    if ($("#sensor_logs_enabled")) {
      $("#sensor_logs_enabled").checked = cfg.sensor_logs_enabled === true;
    }
    if ($("#debug_logs_enabled")) {
      $("#debug_logs_enabled").checked = cfg.debug_logs_enabled === true;
    }
    if ($("#screen_enabled")) {
      $("#screen_enabled").checked = cfg.screen_enabled === true;
    }

    // Mettre à jour les badges et cartes status après chargement de la config
    updateSensorBadges();
    updateStatusCards();
    updateDetailSections();
    loadProductConfig(cfg);
  }

  // ---------- Calibration badges (Dashboard + chip) ----------
  // Atlas EZO : statut basé sur phCalPoints (-1..3) et orpCalPoints (-1..1) du WS sensor_data.
  //   pH OK si >= 2 points calibrés. ORP OK si >= 1 point.
  //   -1 = EZO injoignable.
  async function checkCalibrationDate() {
    try {
      const phPts = (latestSensorData?.phCalPoints != null) ? latestSensorData.phCalPoints : null;
      const orpPts = (latestSensorData?.orpCalPoints != null) ? latestSensorData.orpCalPoints : null;

      const phNeedsCal = (phPts === null) ? false : (phPts < 2); // inclut -1 (injoignable) et 0..1
      const orpNeedsCal = (orpPts === null) ? false : (orpPts < 1);

      const phBadgeEl = $("#ph-calibration-badge");
      const orpBadgeEl = $("#orp-calibration-badge");

      if (phBadgeEl) phBadgeEl.style.display = phNeedsCal ? "inline-flex" : "none";
      if (orpBadgeEl) orpBadgeEl.style.display = orpNeedsCal ? "inline-flex" : "none";

      // Global chip
      const chip = $("#calib-chip");
      if (chip) {
        const need = phNeedsCal || orpNeedsCal;
        chip.style.display = need ? "inline-flex" : "none";
      }

      // Dashboard calibration alerts
      const phDashAlert = $("#ph-calibration-alert");
      const orpDashAlert = $("#orp-calibration-alert");
      if (phDashAlert) phDashAlert.style.display = phNeedsCal ? "flex" : "none";
      if (orpDashAlert) orpDashAlert.style.display = orpNeedsCal ? "flex" : "none";
    } catch (e) {
      // ignore
    }
  }

  // ---------- Sensor data loop (/data) ----------
  const SENSOR_REFRESH_MS = 30000;
  const SENSOR_RETRY_MS = 2000;
  let lastSensorDataLoadTime = 0; // Timestamp du dernier chargement des données
  let sensorDataLoadInFlight = null;
  let sensorDataRetryTimer = null;
  let filtrationRunningOverride = null; // Valeur optimiste après sauvegarde, null = utiliser /data
  let filtrationDirty = false; // Modifications non sauvegardées dans la section Programmation

  // ========== BADGES CAPTEURS ==========

  function updateSensorBadges() {
    function setBadge(id, variant, text) {
      const el = $(`#${id}`);
      if (!el) return;
      if (!variant) {
        el.style.display = 'none';
        el.className = 'sensor-badge';
        el.innerHTML = '';
        return;
      }
      el.className = `sensor-badge sensor-badge--${variant}`;
      el.innerHTML = `<span>${text}</span>`;
      el.style.display = '';
    }

    // Badge pH
    let phVariant = null, phText = '';
    if (latestSensorData?.ph != null && !isNaN(latestSensorData.ph)) {
      const ph = latestSensorData.ph;
      const phTarget = window._config?.ph_target ?? 7.2;
      const phLow = +(phTarget - 0.2).toFixed(1);
      const phHigh = +(phTarget + 0.2).toFixed(1);
      if (ph < phLow || ph > phHigh) {
        phVariant = 'danger';
        phText = ph < phLow ? `Trop bas (${ph.toFixed(1)})` : `Trop élevé (${ph.toFixed(1)})`;
      }
    }
    if (!phVariant && latestSensorData?.ph_limit_reached) {
      phVariant = 'warning';
      phText = 'Limite journalière atteinte';
    }
    { const rem = latestSensorData?.ph_remaining_ml;
      const thr = latestSensorData?.ph_alert_threshold_ml;
      if (rem != null && thr != null && rem <= thr && thr > 0) {
        const stockText = `Stock faible (${(rem / 1000).toFixed(1)} L)`;
        if (!phVariant) { phVariant = 'warning'; phText = stockText; }
        else phText += ` · ${stockText}`;
      }
    }
    setBadge('ph-sensor-badge', phVariant, phText);

    // Badge ORP
    let orpVariant = null, orpText = '';
    if (latestSensorData?.orp != null && !isNaN(latestSensorData.orp)) {
      const orp = Math.round(latestSensorData.orp);
      const orpTarget = window._config?.orp_target ?? 650;
      const orpLow = Math.round(orpTarget - 150);
      const orpHigh = Math.round(orpTarget + 150);
      if (orp < orpLow || orp > orpHigh) {
        orpVariant = 'danger';
        orpText = orp < orpLow ? `Trop bas (${orp} mV)` : `Trop élevé (${orp} mV)`;
      }
    }
    if (!orpVariant && latestSensorData?.orp_limit_reached) {
      orpVariant = 'warning';
      orpText = 'Limite journalière atteinte';
    }
    { const rem = latestSensorData?.orp_remaining_ml;
      const thr = latestSensorData?.orp_alert_threshold_ml;
      if (rem != null && thr != null && rem <= thr && thr > 0) {
        const stockText = `Stock faible (${(rem / 1000).toFixed(1)} L)`;
        if (!orpVariant) { orpVariant = 'warning'; orpText = stockText; }
        else orpText += ` · ${stockText}`;
      }
    }
    setBadge('orp-sensor-badge', orpVariant, orpText);
  }

  // ========== ÉCRAN PRODUITS ==========

  function updateProductUI(data) {
    function updateCard(prefix, remainingMl, containerMl, thresholdMl) {
      const pct = containerMl > 0 ? Math.max(0, Math.min(100, (remainingMl / containerMl) * 100)) : 0;
      const bar = $(`#product-${prefix}-bar`);
      const remEl = $(`#product-${prefix}-remaining`);
      const totEl = $(`#product-${prefix}-total`);
      const status = $(`#product-${prefix}-status`);

      if (bar) {
        bar.style.width = pct.toFixed(1) + '%';
        bar.classList.remove('is-low', 'is-critical');
        if (pct < 10) bar.classList.add('is-critical');
        else if (thresholdMl > 0 && remainingMl <= thresholdMl) bar.classList.add('is-low');
      }
      if (remEl) remEl.textContent = (remainingMl / 1000).toFixed(1) + ' L';
      if (totEl) totEl.textContent = (containerMl / 1000).toFixed(1);

      if (status) {
        const isLow = thresholdMl > 0 && remainingMl <= thresholdMl;
        status.textContent = isLow ? 'Stock faible' : 'OK';
        status.className = 'state-badge ' + (isLow ? 'state-badge--warn' : 'state-badge--on');
      }
    }

    if (data.ph_remaining_ml != null) {
      updateCard('ph', data.ph_remaining_ml, data.ph_container_ml ?? 20000, data.ph_alert_threshold_ml ?? 2000);
    }
    if (data.orp_remaining_ml != null) {
      updateCard('orp', data.orp_remaining_ml, data.orp_container_ml ?? 20000, data.orp_alert_threshold_ml ?? 2000);
    }
    // Synchroniser les toggles depuis sensor_data (source de vérité pour ces champs)
    const phToggle = $('#ph_tracking_enabled');
    const orpToggle = $('#orp_tracking_enabled');
    if (phToggle && data.ph_tracking_enabled != null) phToggle.checked = data.ph_tracking_enabled === true;
    if (orpToggle && data.orp_tracking_enabled != null) orpToggle.checked = data.orp_tracking_enabled === true;
    if (data.ph_tracking_enabled != null) applyProductTracking('ph', data.ph_tracking_enabled === true);
    if (data.orp_tracking_enabled != null) applyProductTracking('orp', data.orp_tracking_enabled === true);
  }

  function applyProductTracking(prefix, enabled) {
    const body = $(`#product-${prefix}-body`);
    if (body) body.style.display = enabled ? '' : 'none';
  }

  function loadProductConfig(cfg) {
    if (!cfg) return;
    const phCont = $(`#ph_container_l`);
    const phAlert = $(`#ph_alert_threshold_l`);
    const orpCont = $(`#orp_container_l`);
    const orpAlert = $(`#orp_alert_threshold_l`);
    if (phCont && cfg.ph_container_ml != null) phCont.value = (cfg.ph_container_ml / 1000).toFixed(1);
    if (phAlert && cfg.ph_alert_threshold_ml != null) phAlert.value = (cfg.ph_alert_threshold_ml / 1000).toFixed(1);
    if (orpCont && cfg.orp_container_ml != null) orpCont.value = (cfg.orp_container_ml / 1000).toFixed(1);
    if (orpAlert && cfg.orp_alert_threshold_ml != null) orpAlert.value = (cfg.orp_alert_threshold_ml / 1000).toFixed(1);

    const phToggle = $('#ph_tracking_enabled');
    const orpToggle = $('#orp_tracking_enabled');
    if (phToggle && cfg.ph_tracking_enabled != null) phToggle.checked = cfg.ph_tracking_enabled === true;
    if (orpToggle && cfg.orp_tracking_enabled != null) orpToggle.checked = cfg.orp_tracking_enabled === true;
    if (cfg.ph_tracking_enabled != null) applyProductTracking('ph', cfg.ph_tracking_enabled === true);
    if (cfg.orp_tracking_enabled != null) applyProductTracking('orp', cfg.orp_tracking_enabled === true);

    // Mettre à jour le nom du produit pH selon la correction
    const phTitle = $('#product-ph-title');
    if (phTitle && cfg.ph_correction_type) {
      phTitle.textContent = cfg.ph_correction_type === 'ph_plus' ? 'pH+ (base)' : 'pH';
    }

    updateProductUI({
      ph_remaining_ml: cfg.ph_remaining_ml,
      ph_container_ml: cfg.ph_container_ml,
      ph_alert_threshold_ml: cfg.ph_alert_threshold_ml,
      orp_remaining_ml: cfg.orp_remaining_ml,
      orp_container_ml: cfg.orp_container_ml,
      orp_alert_threshold_ml: cfg.orp_alert_threshold_ml
    });
  }

  function setupProductScreen() {
    async function saveProduct(prefix) {
      const contL = parseFloat($(`#${prefix}_container_l`)?.value);
      const alertL = parseFloat($(`#${prefix}_alert_threshold_l`)?.value);
      if (isNaN(contL) || isNaN(alertL)) { showToast('Valeurs invalides', 'error'); return; }
      const payload = {
        [`${prefix}_container_ml`]: contL * 1000,
        [`${prefix}_alert_threshold_ml`]: alertL * 1000
      };
      const ok = await sendConfig(payload);
      if (ok) {
        showToast('Sauvegardé', 'success');
        loadConfig().catch(() => {});
      } else {
        showToast('Erreur lors de la sauvegarde', 'error');
      }
    }

    async function resetProduct(prefix) {
      const label = prefix === 'ph' ? 'pH' : 'chlore';
      const contL = parseFloat($(`#${prefix}_container_l`)?.value) || 20;
      const ok = await sendConfig({
        [`${prefix}_container_ml`]: contL * 1000,
        [`${prefix}_reset_container`]: true
      });
      if (ok) {
        showToast(`Bidon ${label} réinitialisé`, 'success');
        loadConfig().catch(() => {});
      } else {
        showToast('Erreur lors de la réinitialisation', 'error');
      }
    }

    $('#product-ph-save')?.addEventListener('click', () => saveProduct('ph'));
    $('#product-orp-save')?.addEventListener('click', () => saveProduct('orp'));
    $('#product-ph-reset')?.addEventListener('click', () => resetProduct('ph'));
    $('#product-orp-reset')?.addEventListener('click', () => resetProduct('orp'));

    $('#ph_tracking_enabled')?.addEventListener('change', async (e) => {
      const enabled = e.target.checked;
      applyProductTracking('ph', enabled);
      await sendConfig({ ph_tracking_enabled: enabled });
    });
    $('#orp_tracking_enabled')?.addEventListener('change', async (e) => {
      const enabled = e.target.checked;
      applyProductTracking('orp', enabled);
      await sendConfig({ orp_tracking_enabled: enabled });
    });
  }

  // ========== CARTES STATUS ==========

  function predictFiltrationRunning(cfg) {
    if (!cfg || !cfg.filtration_enabled) return false;
    const mode = (cfg.filtration_mode || '').toLowerCase();
    if (mode === 'off') return false;
    if (mode === 'manual' || mode === 'auto') {
      const now = new Date();
      const nowMin = now.getHours() * 60 + now.getMinutes();
      const parseTime = (s) => {
        if (!s || s.length < 5) return -1;
        const parts = s.split(':');
        const h = parseInt(parts[0], 10);
        const m = parseInt(parts[1], 10);
        if (isNaN(h) || isNaN(m)) return -1;
        return h * 60 + m;
      };
      const startMin = parseTime(cfg.filtration_start);
      const endMin = parseTime(cfg.filtration_end);
      if (startMin === -1 || endMin === -1) return false;
      if (startMin === endMin) return true;
      if (startMin < endMin) return nowMin >= startMin && nowMin < endMin;
      return nowMin >= startMin || nowMin < endMin;
    }
    return false;
  }

  function updateFiltrationBadges() {
    const state = getFiltrationState(window._config || {}, latestSensorData || {});
    // Dashboard : conserve la classe legacy .state-badge--*
    const dashBadge = $("#detail-filtration-status");
    if (dashBadge) {
      dashBadge.textContent = state.text;
      dashBadge.className = 'state-badge ' + state.class;
    }
    // Card "Contrôle manuel" : badge .pill placé dans .card__head
    const headBadge = $("#filtration-current-status");
    if (headBadge) {
      headBadge.textContent = state.text;
      headBadge.className = 'pill ' + state.pillClass;
    }
  }

  function getFiltrationState(config, data) {
    const isRunning = filtrationRunningOverride !== null ? filtrationRunningOverride : (data && data.filtration_running);
    const temp = data && data.temperature;

    if (!config) return { text: 'Chargement...', class: 'state-badge--off', pillClass: '' };

    // Température hors gel
    if (temp != null && temp < 5.0 && isRunning) {
      return { text: 'Hors gel', class: 'state-badge--warn', pillClass: 'mid' };
    }

    return {
      text: isRunning ? 'En marche' : 'Arrêtée',
      class: isRunning ? 'state-badge--ok' : 'state-badge--off',
      pillClass: isRunning ? 'ok' : 'bad'
    };
  }

  function updateDosingStatus(sensor, isActive, stopReason) {
    const el = $(`#${sensor}-dosing-status`);
    if (!el) return;
    if (isActive) {
      el.innerHTML = '<span class="dosing-status__dot"></span>Injection en cours';
      el.classList.add("is-visible", "is-active");
    } else if (stopReason) {
      el.textContent = stopReason;
      el.classList.add("is-visible");
      el.classList.remove("is-active");
    } else {
      el.textContent = "";
      el.classList.remove("is-visible", "is-active");
    }
  }

  function getDosingStopReason(data, config, sensor) {
    const enabled = sensor === "ph" ? config.ph_enabled : config.orp_enabled;
    if (!enabled) return "Régulation désactivée";

    const remainS = data.stabilization_remaining_s || 0;
    if (remainS > 0) {
      const m = Math.floor(remainS / 60);
      const s = remainS % 60;
      return m > 0 ? `Stabilisation : ${m} min ${String(s).padStart(2, '0')} s` : `Stabilisation : ${s} s`;
    }

    const regulationMode = config.regulation_mode || "pilote";
    if (regulationMode === "pilote" && !data.filtration_running) return "Filtration arrêtée";

    if (sensor === "ph"  && data.ph_limit_reached)  return "Limite journalière atteinte";
    if (sensor === "orp" && data.orp_limit_reached)  return "Limite journalière atteinte";

    const usedMs    = sensor === "ph" ? (data.ph_used_ms  || 0) : (data.orp_used_ms  || 0);
    const limitMin  = sensor === "ph" ? (config.ph_limit_minutes || 0) : (config.orp_limit_minutes || 0);
    if (limitMin > 0 && usedMs >= limitMin * 60000) return "Limite de durée atteinte";

    return "";  // Pas d'erreur identifiée (pause anti-cycling ou valeur OK)
  }

  function updateStatusCards() {
    if (!latestSensorData) return;

    const data = latestSensorData;
    const config = window._config || {};

    // === FILTRATION ===
    // Les cartes de filtration et éclairage compactes ont été supprimées et fusionnées avec les cartes détaillées

    // === pH COMPACT ===
    const compactPh = $("#compact-ph");
    if (compactPh && data.ph != null) {
      compactPh.textContent = (Math.round(data.ph * 10) / 10).toFixed(1);
    }

    const compactPhTarget = $("#compact-ph-target");
    if (compactPhTarget && config.ph_target != null) {
      compactPhTarget.textContent = config.ph_target.toFixed(1);
    }

    updateDosingStatus("ph",  !!data.ph_dosing,  getDosingStopReason(data, config, "ph"));

    // === ORP COMPACT ===
    const compactOrp = $("#compact-orp");
    if (compactOrp && data.orp != null) {
      compactOrp.innerHTML = Math.round(data.orp) + '<span class="compact-unit">mV</span>';
    }

    const compactOrpTarget = $("#compact-orp-target");
    if (compactOrpTarget && config.orp_target != null) {
      compactOrpTarget.textContent = Math.round(config.orp_target);
    }

    updateDosingStatus("orp", !!data.orp_dosing, getDosingStopReason(data, config, "orp"));

    // === ÉCLAIRAGE ===
    if (data.lighting_enabled != null) updateLightingStatus(data.lighting_enabled);

    // === TEMPÉRATURE COMPACT ===
    const compactTemperature = $("#compact-temperature");
    if (compactTemperature && data.temperature != null) {
      compactTemperature.innerHTML = (Math.round(data.temperature * 10) / 10).toFixed(1) + '<span class="compact-unit">°C</span>';
    }
  }

  function updateClockBadge(isSynced) {
    const badge = $("#clockSyncBadge");
    if (!badge) return;
    if (isSynced === false) {
      badge.classList.add("is-warn");
      badge.textContent = "Horloge non synchronisée";
    } else {
      badge.classList.remove("is-warn");
      badge.textContent = "";
    }
  }

  function formatDateTimeFromEpoch(epochSeconds) {
    if (!Number.isFinite(epochSeconds)) return "";
    const date = new Date(epochSeconds * 1000);
    const pad = (value) => String(value).padStart(2, "0");
    return `${date.getFullYear()}-${pad(date.getMonth() + 1)}-${pad(date.getDate())}T${pad(date.getHours())}:${pad(date.getMinutes())}:${pad(date.getSeconds())}`;
  }

  function parseDateTimeToEpoch(value) {
    if (!value) return null;
    const trimmed = String(value).trim();
    const numeric = Number(trimmed);
    if (Number.isFinite(numeric)) {
      if (numeric > 10000000000) return Math.floor(numeric / 1000);
      return Math.floor(numeric);
    }

    const isoMatch = trimmed.match(/^(\d{4})[-/](\d{2})[-/](\d{2})(?:[ T]?(\d{2}):(\d{2})(?::(\d{2}))?)?$/);
    if (isoMatch) {
      const [, year, month, day, hour, minute, second] = isoMatch;
      const date = new Date(
        Number(year),
        Number(month) - 1,
        Number(day),
        Number(hour || 0),
        Number(minute || 0),
        Number(second || 0)
      );
      return Math.floor(date.getTime() / 1000);
    }

    const frMatch = trimmed.match(/^(\d{2})[-/](\d{2})[-/](\d{4})(?:[ T](\d{2}):(\d{2})(?::(\d{2}))?)?$/);
    if (!frMatch) return null;
    const [, day, month, year, hour, minute, second] = frMatch;
    const date = new Date(
      Number(year),
      Number(month) - 1,
      Number(day),
      Number(hour || 0),
      Number(minute || 0),
      Number(second || 0)
    );
    return Math.floor(date.getTime() / 1000);
  }

  function parseCsvLine(line, delimiter) {
    const result = [];
    let current = "";
    let inQuotes = false;
    for (let i = 0; i < line.length; i += 1) {
      const char = line[i];
      if (char === "\"") {
        if (inQuotes && line[i + 1] === "\"") {
          current += "\"";
          i += 1;
        } else {
          inQuotes = !inQuotes;
        }
        continue;
      }
      if (!inQuotes && char === delimiter) {
        result.push(current);
        current = "";
        continue;
      }
      current += char;
    }
    result.push(current);
    return result;
  }

  function parseHistoryCsv(text) {
    const cleaned = String(text || "").replace(/^\uFEFF/, "");
    const lines = cleaned
      .split(/\r?\n/)
      .map((line) => line.trim())
      .filter((line) => line.length);
    if (lines.length < 2) return [];

    const delimiter = lines[0].includes(";") ? ";" : (lines[0].includes("\t") ? "\t" : ",");
    const headers = parseCsvLine(lines[0], delimiter).map((h) => h.trim().toLowerCase());
    const idxDatetime = headers.findIndex((h) => ["datetime", "date", "date_time", "date_heure"].includes(h));
    const idxTimestamp = headers.findIndex((h) => ["timestamp", "timestamp_s", "epoch", "epoch_s"].includes(h));
    const idxPh = headers.indexOf("ph");
    const idxOrp = headers.indexOf("orp");
    const idxTemp = headers.indexOf("temperature");
    const idxFiltration = headers.indexOf("filtration");
    const idxDosing = headers.indexOf("dosing");
    const idxGranularity = headers.indexOf("granularity");

    const toValue = (value) => {
      const trimmed = String(value || "").trim();
      if (trimmed.startsWith("\"") && trimmed.endsWith("\"")) {
        return trimmed.slice(1, -1);
      }
      return trimmed;
    };

    const toNumber = (value) => {
      const cleaned = toValue(value);
      if (!cleaned) return null;
      const normalized = cleaned.includes(",") && !cleaned.includes(".")
        ? cleaned.replace(",", ".")
        : cleaned;
      const parsed = Number(normalized);
      return Number.isFinite(parsed) ? parsed : null;
    };

    const toBool = (value) => {
      const cleaned = toValue(value).toLowerCase();
      return cleaned === "1" || cleaned === "true" || cleaned === "yes" || cleaned === "y" || cleaned === "on";
    };

    const result = [];
    for (let i = 1; i < lines.length; i += 1) {
      const cols = parseCsvLine(lines[i], delimiter);
      const getCol = (idx) => (idx >= 0 && idx < cols.length ? cols[idx] : "");

      let timestamp = null;
      if (idxTimestamp >= 0) {
        const tsValue = toNumber(getCol(idxTimestamp));
        if (tsValue != null) {
          timestamp = tsValue > 10000000000 ? Math.floor(tsValue / 1000) : Math.floor(tsValue);
        }
      } else if (idxDatetime >= 0) {
        timestamp = parseDateTimeToEpoch(toValue(getCol(idxDatetime)));
      }

      if (!timestamp) {
        continue;
      }

      const ph = idxPh >= 0 ? toNumber(getCol(idxPh)) : null;
      const orp = idxOrp >= 0 ? toNumber(getCol(idxOrp)) : null;
      const temperature = idxTemp >= 0 ? toNumber(getCol(idxTemp)) : null;

      result.push({
        timestamp,
        ph,
        orp,
        temperature,
        filtration: idxFiltration >= 0 ? toBool(getCol(idxFiltration)) : false,
        dosing: idxDosing >= 0 ? toBool(getCol(idxDosing)) : false,
        granularity: idxGranularity >= 0 ? Math.max(0, parseInt(getCol(idxGranularity), 10) || 0) : 0
      });
    }

    return result;
  }

  function bindHistoryBackup() {
    const exportBtn = $("#history_export_btn");
    const importInput = $("#history_import_file");
    const importBtn = $("#history_import_btn");
    const clearBtn = $("#history_clear_btn");

    exportBtn?.addEventListener("click", async () => {
      exportBtn.disabled = true;
      try {
        const res = await authFetch("/get-history?range=all");
        if (!res.ok) throw new Error("export failed");
        const data = await res.json();
        const rows = [
          ["datetime", "ph", "orp", "temperature", "filtration", "dosing", "granularity"].join(",")
        ];

        (data.history || []).forEach((point) => {
          const row = [
            formatDateTimeFromEpoch(point.timestamp),
            point.ph ?? "",
            point.orp ?? "",
            point.temperature ?? "",
            point.filtration ? 1 : 0,
            point.dosing ? 1 : 0,
            point.granularity ?? ""
          ];
          rows.push(row.join(","));
        });

        const csv = rows.join("\n");
        const blob = new Blob([csv], { type: "text/csv;charset=utf-8;" });
        const url = URL.createObjectURL(blob);
        const a = document.createElement("a");
        const stamp = new Date().toISOString().replace(/[:.]/g, "-");
        a.href = url;
        a.download = `history-backup-${stamp}.csv`;
        document.body.appendChild(a);
        a.click();
        document.body.removeChild(a);
        setTimeout(() => URL.revokeObjectURL(url), 1000);
      } catch (err) {
        alert(`Erreur export historique: ${err.message || err}`);
      } finally {
        exportBtn.disabled = false;
      }
    });

    importInput?.addEventListener("change", () => {
      if (importBtn) importBtn.disabled = !(importInput.files && importInput.files.length);
    });

    importBtn?.addEventListener("click", async () => {
      if (!(importInput.files && importInput.files.length)) return;
      if (!confirm("Importer ce fichier va remplacer l'historique actuel. Continuer ?")) return;

      importBtn.disabled = true;
      importInput.disabled = true;

      try {
        const text = await importInput.files[0].text();
        const points = parseHistoryCsv(text);
        if (!points.length) {
          alert("Aucune donnée valide trouvée dans le fichier.");
          return;
        }

        const res = await authFetch("/history/import", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({ history: points })
        });

        let response = null;
        if (!res.ok) {
          try {
            response = await res.json();
          } catch (_) {}
          throw new Error(response?.error || "Import impossible");
        }
        response = await res.json();
        alert(`Import terminé (${response?.count || points.length} points).`);
        importInput.value = "";
        if (importBtn) importBtn.disabled = true;
      } catch (err) {
        alert(`Erreur import historique: ${err.message || err}`);
      } finally {
        importBtn.disabled = !(importInput.files && importInput.files.length);
        importInput.disabled = false;
      }
    });

    clearBtn?.addEventListener("click", async () => {
      if (!confirm("Supprimer l'historique enregistré ? Cette action est irréversible.")) return;
      clearBtn.disabled = true;
      try {
        const res = await authFetch("/history/clear", { method: "POST" });
        if (!res.ok) {
          let errJson = null;
          try {
            errJson = await res.json();
          } catch (_) {}
          throw new Error(errJson?.error || "Suppression impossible");
        }
        clearHistoryCharts();
        alert("Historique supprimé.");
      } catch (err) {
        alert(`Erreur suppression historique: ${err.message || err}`);
      } finally {
        clearBtn.disabled = false;
      }
    });
  }

  function clearHistoryCharts() {
    if (tempChart) {
      tempChart.data.labels = [];
      tempChart.data.datasets[0].data = [];
      tempChart.update("none");
    }
    if (phChart) {
      phChart.data.labels = [];
      phChart.data.datasets[0].data = [];
      syncPhReferenceDatasets(phChart);
      phChart.update("none");
    }
    if (orpChart) {
      orpChart.data.labels = [];
      orpChart.data.datasets[0].data = [];
      syncOrpReferenceDatasets(orpChart);
      orpChart.update("none");
    }
  }

  // ========== DOSAGE JOURNALIER ==========
  // type : 'ph' ou 'orp'
  function updateDailyDoseDisplay(type) {
    const injectedMl = type === 'ph'
      ? (latestSensorData?.ph_daily_ml ?? null)
      : (latestSensorData?.orp_daily_ml ?? null);
    const limitReached = type === 'ph'
      ? (latestSensorData?.ph_limit_reached === true)
      : (latestSensorData?.orp_limit_reached === true);
    const maxMl = type === 'ph'
      ? (window._config?.max_ph_ml_per_day ?? 0)
      : (window._config?.max_chlorine_ml_per_day ?? 0);

    const valueEl  = $(`#daily-dose-${type}-value`);
    const barEl    = $(`#daily-dose-${type}-bar`);
    const statusEl = $(`#daily-dose-${type}-status`);
    if (!valueEl || !barEl || !statusEl) return;

    if (injectedMl === null) {
      valueEl.textContent = '— mL';
      barEl.style.width = '0%';
      barEl.setAttribute('aria-valuenow', '0');
      barEl.className = 'daily-dose__bar dose-bar--ok';
      statusEl.textContent = '';
      return;
    }

    const injected = Math.round(injectedMl);

    if (!maxMl || maxMl <= 0) {
      // Limite non configurée
      valueEl.textContent = `${injected} mL`;
      barEl.style.width = '0%';
      barEl.setAttribute('aria-valuenow', '0');
      barEl.className = 'daily-dose__bar dose-bar--ok';
      statusEl.textContent = 'Limite non configurée';
      statusEl.style.color = 'var(--muted)';
      return;
    }

    const max = Math.round(maxMl);
    valueEl.textContent = `${injected} / ${max} mL`;

    const pct = Math.min(100, Math.round((injected / max) * 100));
    barEl.style.width = `${pct}%`;
    barEl.setAttribute('aria-valuenow', String(pct));

    let barClass = 'dose-bar--ok';
    if (limitReached || pct >= 90) {
      barClass = 'dose-bar--danger';
    } else if (pct >= 75) {
      barClass = 'dose-bar--warning';
    }
    barEl.className = `daily-dose__bar ${barClass}`;

    if (limitReached) {
      statusEl.textContent = 'Limite atteinte — dosage suspendu jusqu\'à minuit';
      statusEl.style.color = 'var(--danger)';
    } else {
      statusEl.textContent = '';
    }
  }

  // ========== SECTIONS DÉTAILLÉES ==========
  function updateDetailSections() {
    const config = window._config || {};

    // === FILTRATION ===
    const detailFiltrationMode = $("#detail-filtration-mode");
    if (detailFiltrationMode) {
      const modes = {
        auto: 'Auto',
        manual: 'Programmation',
        force: 'Manuel',
        off: 'Désactivé'
      };
      detailFiltrationMode.textContent = modes[config.filtration_mode] || 'Auto';
    }

    const detailFiltrationSchedule = $("#detail-filtration-schedule");
    if (detailFiltrationSchedule) {
      const start = config.filtration_start || '08:00';
      const end = config.filtration_end || '20:00';
      detailFiltrationSchedule.textContent = `${start} - ${end}`;
    }

    updateFiltrationBadges();

    // Dosage journalier pH et ORP
    updateDailyDoseDisplay('ph');
    updateDailyDoseDisplay('orp');

    // Boutons Démarrer/Arrêter du tableau de bord : grisés en mode Désactivé
    const isOff = (config.filtration_mode || '').toLowerCase() === 'off';
    const detailStartBtn = $("#detail-filtration-start");
    const detailStopBtn = $("#detail-filtration-stop");
    if (detailStartBtn) detailStartBtn.disabled = isOff;
    if (detailStopBtn) detailStopBtn.disabled = isOff;

    // === ÉCLAIRAGE ===
    // lighting_enabled vient exclusivement des push sensor_data (état réel du relais)
    // Ne pas utiliser config.lighting_enabled (= état du toggle config, pas du relais)
    const detailLightingStatus = $("#detail-lighting-status");
    if (detailLightingStatus && latestSensorData?.lighting_enabled != null) {
      const isOn = latestSensorData.lighting_enabled;
      detailLightingStatus.textContent = isOn ? 'Allumé' : 'Éteint';
      detailLightingStatus.className = 'state-badge ' + (isOn ? 'state-badge--ok' : 'state-badge--off');
    }

    const detailLightingMode = $("#detail-lighting-mode");
    if (detailLightingMode) {
      const scheduleEnabled = config.lighting_schedule_enabled;
      detailLightingMode.textContent = scheduleEnabled ? 'Programmation activée' : 'Programmation désactivée';
    }

    const detailLightingSchedule = $("#detail-lighting-schedule");
    if (detailLightingSchedule) {
      const startTime = config.lighting_start_time || '20:00';
      const endTime = config.lighting_end_time || '23:00';
      detailLightingSchedule.textContent = startTime + ' - ' + endTime;
    }
  }

  function bindDetailActions() {
    // Boutons filtration
    const startBtn = $("#detail-filtration-start");
    const stopBtn = $("#detail-filtration-stop");

    if (startBtn) {
      startBtn.addEventListener('click', async () => {
        const payload = { filtration_enabled: true, filtration_force_on: true, filtration_force_off: false };
        try {
          const result = await sendConfig(payload);
          if (result) {
            filtrationRunningOverride = true;
            updateDetailSections();
            updateStatusCards();
            showToast("Filtration démarrée", "success");
            loadConfig();
            setTimeout(() => loadSensorData({ force: true, source: "filtration-save" }), 500);
          } else {
            showToast("Erreur lors du démarrage", "error");
          }
        } catch (e) {
          console.error('Erreur démarrage filtration:', e);
          showToast("Erreur de connexion", "error");
        }
      });
    }

    if (stopBtn) {
      stopBtn.addEventListener('click', async () => {
        const payload = { filtration_force_on: false, filtration_force_off: true };
        try {
          const result = await sendConfig(payload);
          if (result) {
            filtrationRunningOverride = false;
            updateDetailSections();
            updateStatusCards();
            showToast("Filtration arrêtée", "success");
            loadConfig();
            setTimeout(() => loadSensorData({ force: true, source: "filtration-save" }), 500);
          } else {
            showToast("Erreur lors de l'arrêt", "error");
          }
        } catch (e) {
          console.error('Erreur arrêt filtration:', e);
          showToast("Erreur de connexion", "error");
        }
      });
    }

    // Boutons éclairage
    const lightOnBtn = $("#detail-lighting-on");
    const lightOffBtn = $("#detail-lighting-off");

    if (lightOnBtn) {
      lightOnBtn.addEventListener('click', async () => {
        try {
          const response = await authFetch("/lighting/on", { method: "POST" });
          if (response.ok) {
            if (latestSensorData) latestSensorData.lighting_enabled = true;
            updateLightingStatus(true);
            showToast("Éclairage allumé", "success");
          } else {
            showToast("Erreur lors de l'allumage", "error");
          }
        } catch (e) {
          console.error('Erreur allumage éclairage:', e);
          showToast("Erreur de connexion", "error");
        }
      });
    }

    if (lightOffBtn) {
      lightOffBtn.addEventListener('click', async () => {
        try {
          const response = await authFetch("/lighting/off", { method: "POST" });
          if (response.ok) {
            if (latestSensorData) latestSensorData.lighting_enabled = false;
            updateLightingStatus(false);
            showToast("Éclairage éteint", "success");
          } else {
            showToast("Erreur lors de l'extinction", "error");
          }
        } catch (e) {
          console.error('Erreur extinction éclairage:', e);
          showToast("Erreur de connexion", "error");
        }
      });
    }
  }

  function updateDashboardMetrics(json) {
    // Anciennes métriques (compatibilité si les éléments existent encore)
    const mTime = $("#m-time");
    const mTemp = $("#m-temp");
    const mPh = $("#m-ph");
    const mOrp = $("#m-orp");

    if (mTime) {
      const ts = json.timestamp ? new Date(json.timestamp) : new Date();
      mTime.textContent = ts.toLocaleTimeString();
    }

    if (mTemp && json.temperature != null && !isNaN(json.temperature)) {
      mTemp.textContent = json.temperature.toFixed(1);
    }
    if (mPh && json.ph != null && !isNaN(json.ph)) {
      mPh.textContent = (Math.round(json.ph * 10) / 10).toFixed(1);
    }
    if (mOrp && json.orp != null && !isNaN(json.orp)) {
      mOrp.textContent = String(Math.round(json.orp));
    }
  }

  // ---------- État sonde pH (feature-024) ----------
  // UINT32_MAX renvoyé par le firmware = "jamais lu"
  const PH_SLOPE_NEVER = 0xFFFFFFFF;
  const PH_PROBE_STALE_MS = 36 * 3600 * 1000; // 36 h
  const PH_PROBE_REFRESH_TIMEOUT_MS = 8000;   // 8 s max d'attente après POST

  // État du refresh manuel : null = idle, sinon { startedAt, ageAtStart }
  let _phProbeRefreshState = null;

  // Classification de la sonde pH selon les seuils livrés par ux-ui-designer.
  // feature-034 : chip unique « sonde + calibration ».
  //   calPoints null/<0 → EZO indisponible ; 0 → calibration requise ;
  //   1 → calibration 1/2 ; >=2 → diagnostic de pente (santé sonde).
  function classifyPhProbe({ acid, base, zero, calPoints, ageMs }) {
    if (calPoints == null || calPoints < 0) {
      return { cls: 'unknown', label: 'EZO indisponible' };
    }
    if (calPoints === 0) {
      return { cls: 'bad', label: 'Calibration requise' };
    }
    if (calPoints === 1) {
      return { cls: 'warn', label: 'Calibration 1/2' };
    }
    const acidNum = (acid == null || isNaN(acid)) ? null : Number(acid);
    const baseNum = (base == null || isNaN(base)) ? null : Number(base);
    if (acidNum == null || baseNum == null) {
      return { cls: 'unknown', label: 'Pente non disponible' };
    }
    const min = Math.min(acidNum, baseNum);
    const zeroAbs = (zero == null || isNaN(zero)) ? 0 : Math.abs(Number(zero));
    // Sonde calibrée : préfixer « Calibré » pour uniformiser avec le chip ORP, tout en
    // conservant l'info de santé (pente %). Cas « à remplacer » : l'alerte prime sur « Calibré ».
    if (min < 85 || zeroAbs > 30) {
      return { cls: 'bad', label: `Sonde à remplacer · ${Math.round(min)} %` };
    }
    if (min < 90) {
      return { cls: 'warn2', label: `Calibré · sonde usée ${Math.round(min)} %` };
    }
    if (min < 95 || zeroAbs > 15) {
      return { cls: 'warn', label: `Calibré · sonde ${Math.round(min)} %` };
    }
    return { cls: 'good', label: `Calibré · sonde ${Math.round(min)} %` };
  }

  // Met en forme l'âge en libellé lisible : "il y a 14 h", "il y a 12 min", "jamais"
  function formatProbeAge(ageMs) {
    if (ageMs == null || ageMs >= PH_SLOPE_NEVER) return 'jamais';
    if (ageMs < 60 * 1000) return 'à l\'instant';
    const minutes = Math.floor(ageMs / 60000);
    if (minutes < 60) return `il y a ${minutes} min`;
    const hours = Math.floor(minutes / 60);
    const remMin = minutes - hours * 60;
    if (hours < 24) {
      return remMin === 0 ? `il y a ${hours} h` : `il y a ${hours} h ${remMin} min`;
    }
    const days = Math.floor(hours / 24);
    const remHours = hours - days * 24;
    return remHours === 0 ? `il y a ${days} j` : `il y a ${days} j ${remHours} h`;
  }

  // Met à jour le chip d'état sonde pH dans la carte « Lecture pH ».
  // Appelé depuis loadSensorData après chaque push WS / fetch /data.
  function updatePhProbeChip(json) {
    const chip = $("#ph-probe-chip");
    const lblEl = $("#ph-probe-chip-label");
    if (!chip || !lblEl) return;

    const acid = json?.phSlopeAcid;
    const base = json?.phSlopeBase;
    const zero = json?.phSlopeZero;
    const calPoints = json?.phCalPoints;
    const ageMs = json?.phSlopeAgeMs;

    const { cls, label } = classifyPhProbe({ acid, base, zero, calPoints, ageMs });

    // Reset les classes de variantes uniquement
    chip.classList.remove(
      'chip--probe-good', 'chip--probe-warn', 'chip--probe-warn2',
      'chip--probe-bad', 'chip--probe-unknown', 'chip--probe-stale'
    );
    chip.classList.add(`chip--probe-${cls}`);

    // État stale : âge connu et > 36 h
    const isStale = (typeof ageMs === 'number') && ageMs < PH_SLOPE_NEVER && ageMs > PH_PROBE_STALE_MS;
    if (isStale) chip.classList.add('chip--probe-stale');

    lblEl.textContent = label;

    // Détection de refresh terminé : si on attend un refresh et que ageMs est redescendu sous 60 s
    if (_phProbeRefreshState && typeof ageMs === 'number' && ageMs < PH_SLOPE_NEVER && ageMs < 60000) {
      _phProbeRefreshFinish(true);
    }

    // Mettre à jour le contenu du modal si ouvert
    const modal = $("#ph-probe-modal");
    if (modal && modal.open) _renderPhProbeModalValues(json);
  }

  function _renderPhProbeModalValues(json) {
    const acidEl = $("#ph-probe-acid");
    const baseEl = $("#ph-probe-base");
    const zeroEl = $("#ph-probe-zero");
    const ageEl = $("#ph-probe-age");
    if (!acidEl || !baseEl || !zeroEl || !ageEl) return;

    const acid = json?.phSlopeAcid;
    const base = json?.phSlopeBase;
    const zero = json?.phSlopeZero;
    const ageMs = json?.phSlopeAgeMs;
    const calPoints = json?.phCalPoints;

    const fmtPct = (v) => (v == null || isNaN(v)) ? '--' : `${Number(v).toFixed(1)} %`;
    const fmtMv = (v) => (v == null || isNaN(v)) ? '--' : `${Number(v).toFixed(2)} mV`;

    if (calPoints != null && calPoints < 2) {
      acidEl.textContent = '--';
      baseEl.textContent = '--';
      zeroEl.textContent = '--';
    } else {
      acidEl.textContent = fmtPct(acid);
      baseEl.textContent = fmtPct(base);
      zeroEl.textContent = fmtMv(zero);
    }
    ageEl.textContent = formatProbeAge(ageMs);
  }

  function _phProbeRefreshFinish(success) {
    if (!_phProbeRefreshState) return;
    if (_phProbeRefreshState.timeoutId) clearTimeout(_phProbeRefreshState.timeoutId);
    const btn = $("#ph-probe-refresh");
    if (btn) {
      btn.disabled = false;
      btn.textContent = 'Rafraîchir';
    }
    if (success) {
      showToast('Pente sonde pH rafraîchie', 'success');
    }
    _phProbeRefreshState = null;
  }

  async function _phProbeRefresh() {
    if (_phProbeRefreshState) return; // refresh déjà en cours
    const btn = $("#ph-probe-refresh");
    try {
      const res = await authFetch('/debug/ph_slope_refresh', { method: 'POST' });
      if (res.status === 503) {
        showToast('Queue saturée, réessayer dans 1s', 'warning');
        return;
      }
      if (!res.ok) {
        showToast('Erreur lors du rafraîchissement', 'error');
        return;
      }
      // 200 OK : on enregistre l'état et on attend que phSlopeAgeMs retombe sous 60s via WS
      if (btn) {
        btn.disabled = true;
        btn.textContent = 'Rafraîchissement…';
      }
      _phProbeRefreshState = {
        startedAt: Date.now(),
        timeoutId: setTimeout(() => {
          showToast('Sonde non joignable', 'error');
          _phProbeRefreshFinish(false);
        }, PH_PROBE_REFRESH_TIMEOUT_MS),
      };
    } catch (e) {
      console.error('[ph-probe] refresh failed:', e);
      showToast('Erreur réseau', 'error');
    }
  }

  function bindPhProbeChip() {
    const chip = $("#ph-probe-chip");
    const modal = $("#ph-probe-modal");
    const closeBtn = $("#ph-probe-close");
    const refreshBtn = $("#ph-probe-refresh");
    if (!chip || !modal) return;

    // Fallback si <dialog> non supporté : on log + on désactive le clic
    if (typeof modal.showModal !== 'function') {
      console.warn('[ph-probe] <dialog> non supporté par ce navigateur, popup désactivé');
      chip.setAttribute('aria-disabled', 'true');
      chip.style.cursor = 'default';
      return;
    }

    chip.addEventListener('click', () => {
      _renderPhProbeModalValues(latestSensorData || {});
      try { modal.showModal(); } catch (e) { console.error('[ph-probe] showModal:', e); }
    });
    chip.addEventListener('keydown', (e) => {
      if (e.key === 'Enter' || e.key === ' ') {
        e.preventDefault();
        chip.click();
      }
    });

    if (closeBtn) closeBtn.addEventListener('click', () => modal.close());
    if (refreshBtn) refreshBtn.addEventListener('click', _phProbeRefresh);

    // Backdrop close : clic en dehors du contenu du dialog
    modal.addEventListener('click', (e) => {
      if (e.target === modal) modal.close();
    });
    // Cleanup state si le modal est fermé (ESC ou autre)
    modal.addEventListener('close', () => {
      // Pas de reset du refresh ici : il continue en arrière-plan jusqu'au timeout
    });
  }

  // ---- feature-025 : chips + modals « état filtre » pH/ORP ----
  // La valeur principale (#xx_current_value) affiche déjà la valeur FILTRÉE (json.ph/json.orp).
  // Ces helpers gèrent la ligne brute discrète, le chip d'état et le modal détail.
  const FILTER_REJECT_WINDOW_MS = 30000; // fenêtre « pics rejetés récents »
  const _filterState = {
    ph:  { lastRawAt: 0, prevRejected: null, lastRejectAt: 0 },
    orp: { lastRawAt: 0, prevRejected: null, lastRejectAt: 0 },
  };

  function _filterRawIsValid(v) { return v != null && !(typeof v === 'number' && isNaN(v)); }

  function _fmtFilterVal(prefix, v) {
    if (!_filterRawIsValid(v)) return '--';
    return prefix === 'orp' ? `${Math.round(v)} mV` : Number(v).toFixed(2);
  }

  function _filterAgeText(prefix) {
    const st = _filterState[prefix];
    if (!st || !st.lastRawAt) return '--';
    const age = Date.now() - st.lastRawAt;
    if (age < 10000) return "à l'instant";
    if (age < 60000) return `il y a ${Math.round(age / 1000)} s`;
    if (age < 3600000) return `il y a ${Math.round(age / 60000)} min`;
    return formatProbeAge(age);
  }

  // Le compteur de rejets est cumulatif côté firmware : on ne signale « Pics rejetés »
  // que si le compteur a augmenté récemment (fenêtre FILTER_REJECT_WINDOW_MS).
  function _classifyFilterState(json, prefix, st) {
    const raw = json?.[prefix + 'Raw'];
    if (!_filterRawIsValid(raw)) return { cls: 'unknown', label: 'EZO indisponible' };
    if (json?.[prefix + 'FilterUnstable'] === true) return { cls: 'bad', label: 'Capteur instable' };
    if (json?.[prefix + 'FilterReady'] === false) return { cls: 'warn', label: 'Stabilisation…' };
    if (st.lastRejectAt && (Date.now() - st.lastRejectAt) < FILTER_REJECT_WINDOW_MS) {
      return { cls: 'warn2', label: 'Pics rejetés' };
    }
    return { cls: 'good', label: 'Mesure stable' };
  }

  function _renderFilterSub(prefix, json) {
    const el = $(`#${prefix}-filter-sub`);
    if (!el) return;
    const raw = _fmtFilterVal(prefix, json?.[prefix + 'Raw']);
    const med = _fmtFilterVal(prefix, json?.[prefix + 'Median']);
    el.textContent = `brut ${raw} · médiane ${med} · maj ${_filterAgeText(prefix)}`;
  }

  function _doseBlockReasonFr(r) {
    if (!r) return '';
    const s = String(r).toLowerCase();
    if (s.includes('warm') || s.includes('stabil')) return 'Stabilisation du filtre';
    if (s.includes('unstab') || s.includes('instab')) return 'Capteur instable';
    if (s.includes('mix') || s.includes('mélange') || s.includes('melange')) return 'Mélange en cours';
    if (s.includes('ezo') || s.includes('indispo')) return 'EZO indisponible';
    return r; // déjà rédigé en clair côté firmware
  }

  function _renderFilterModalValues(prefix, json) {
    const rawEl = $(`#${prefix}-fm-raw`);
    if (!rawEl) return;
    const medEl = $(`#${prefix}-fm-median`);
    const filEl = $(`#${prefix}-fm-filtered`);
    const rejEl = $(`#${prefix}-fm-rejected`);
    const ageEl = $(`#${prefix}-fm-age`);
    const rdyEl = $(`#${prefix}-fm-ready`);
    const blockedRow = $(`#${prefix}-fm-blocked-row`);
    const blockedEl = $(`#${prefix}-fm-blocked`);

    rawEl.textContent = _fmtFilterVal(prefix, json?.[prefix + 'Raw']);
    if (medEl) medEl.textContent = _fmtFilterVal(prefix, json?.[prefix + 'Median']);
    if (filEl) filEl.textContent = _fmtFilterVal(prefix, json?.[prefix + 'Filtered']);
    if (rejEl) { const r = json?.[prefix + 'RejectedCount']; rejEl.textContent = (typeof r === 'number') ? String(r) : '--'; }
    if (ageEl) ageEl.textContent = _filterAgeText(prefix);
    if (rdyEl) {
      const ready = json?.[prefix + 'FilterReady'];
      rdyEl.textContent = ready === true ? 'Oui' : (ready === false ? 'Non' : '--');
    }

    let reason = _doseBlockReasonFr(json?.[prefix + 'DoseBlockedReason']);
    if (!reason && json?.[prefix + 'MixingDelayActive'] === true) reason = 'Mélange en cours';
    if (blockedRow && blockedEl) {
      if (reason) { blockedEl.textContent = reason; blockedRow.style.display = ''; }
      else { blockedRow.style.display = 'none'; }
    }
  }

  // Met à jour le chip d'état filtre + la ligne brute. Appelé depuis loadSensorData
  // après chaque push WS / fetch /data.
  function updateFilterChip(prefix, json) {
    const chip = $(`#${prefix}-filter-chip`);
    const lblEl = $(`#${prefix}-filter-chip-label`);
    const st = _filterState[prefix];
    if (!chip || !lblEl || !st) return;

    const raw = json?.[prefix + 'Raw'];
    if (_filterRawIsValid(raw)) st.lastRawAt = Date.now();
    const rejected = json?.[prefix + 'RejectedCount'];
    if (typeof rejected === 'number') {
      if (st.prevRejected != null && rejected > st.prevRejected) st.lastRejectAt = Date.now();
      st.prevRejected = rejected;
    }

    const { cls, label } = _classifyFilterState(json, prefix, st);
    chip.classList.remove(
      'chip--probe-good', 'chip--probe-warn', 'chip--probe-warn2',
      'chip--probe-bad', 'chip--probe-unknown', 'chip--probe-stale'
    );
    chip.classList.add(`chip--probe-${cls}`);
    lblEl.textContent = label;

    _renderFilterSub(prefix, json);
    const modal = $(`#${prefix}-filter-modal`);
    if (modal && modal.open) _renderFilterModalValues(prefix, json);
  }

  function bindFilterChip(prefix) {
    const chip = $(`#${prefix}-filter-chip`);
    const modal = $(`#${prefix}-filter-modal`);
    const closeBtn = $(`#${prefix}-filter-close`);
    if (!chip || !modal) return;

    if (typeof modal.showModal !== 'function') {
      chip.setAttribute('aria-disabled', 'true');
      chip.style.cursor = 'default';
      return;
    }

    chip.addEventListener('click', () => {
      _renderFilterModalValues(prefix, latestSensorData || {});
      try { modal.showModal(); } catch (e) { console.error(`[${prefix}-filter] showModal:`, e); }
    });
    chip.addEventListener('keydown', (e) => {
      if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); chip.click(); }
    });
    if (closeBtn) closeBtn.addEventListener('click', () => modal.close());
    modal.addEventListener('click', (e) => { if (e.target === modal) modal.close(); });
  }

  async function loadSensorData(options = {}) {
    if (sensorDataLoadInFlight && !options.data) return sensorDataLoadInFlight;
    const force = options.force === true;
    const source = options.source || "unknown";
    const perf = debugStart(`loadSensorData force=${force} source=${source}`);

    if (!force && lastSensorDataLoadTime !== 0) {
      const dataAge = Date.now() - lastSensorDataLoadTime;
      if (dataAge < SENSOR_REFRESH_MS) {
        perf?.end("cached");
        return latestSensorData;
      }
    }

    sensorDataLoadInFlight = (async () => {
      try {
        let json;
        if (options.data) {
          // Données fournies directement (depuis WebSocket) : pas de fetch HTTP
          json = options.data;
        } else {
          // Add timeout to detect disconnection (increased to 10 seconds for ESP32)
          const controller = new AbortController();
          const timeoutId = setTimeout(() => controller.abort(), 10000); // 10 second timeout

          // Bypass any HTTP cache so the UI can refresh immediately after calibration
          const fetchPerf = debugStart("fetch /data");
          const res = await authFetch(`/data?t=${Date.now()}`, { signal: controller.signal, cache: "no-store" });
          clearTimeout(timeoutId);
          fetchPerf?.end(`status=${res.status}`);

          if (!res.ok) throw new Error("bad");
          const jsonPerf = debugStart("parse /data json");
          json = await res.json();
          jsonPerf?.end();
        }

        latestSensorData = json;
        document.querySelectorAll('.is-stale').forEach(el => el.classList.remove('is-stale'));
        updateFiltrationControls(); // Réévaluer la disponibilité du mode Auto après réception des données

        // Les données serveur font autorité — lever l'override optimiste
        filtrationRunningOverride = null;

        if (sensorDataRetryTimer) {
          clearTimeout(sensorDataRetryTimer);
          sensorDataRetryTimer = null;
        }

        // Reset failure counter on success
        consecutiveFailures = 0;
        setNetStatus("ok", "En ligne");

        updateTodayOnCharts(json);


        updateDashboardMetrics(json);
        updateClockBadge(json.time_synced === true);

        // Mettre à jour les badges et cartes status
        updateSensorBadges();
        updateStatusCards();
        updateProductUI(json);
        _updateSondesChip();
        if (window._updateInjectButtons) window._updateInjectButtons(json);

        // Mettre à jour les sections détaillées
        updateDetailSections();

        // also update readouts in settings — Atlas EZO : la sonde retourne directement la valeur
        // calibrée (pas de "tension brute" à exposer). Pendant la calibration on affiche la
        // lecture pH/ORP live (3 décimales pour pH).
        const phCurrentValue = $("#ph_current_value");
        const phCurrentLabel = $("#ph_current_label");
        if (phCurrentValue) {
          if (phCurrentLabel) phCurrentLabel.textContent = "Valeur pH actuelle";
          if (json.ph != null && typeof json.ph === "number" && !isNaN(json.ph)) {
            // Arrondi à 1 décimale, cohérent avec le tableau de bord (updateDashboardMetrics).
            phCurrentValue.textContent = (Math.round(json.ph * 10) / 10).toFixed(1);
          } else {
            phCurrentValue.textContent = "--";
          }
        }

        // Chip d'état sonde pH (feature-024) — basé sur phSlopeAcid/Base/Zero/AgeMs + phCalPoints
        updatePhProbeChip(json);
        // Chip d'état filtre pH (feature-025) — brut/médiane/filtré + stabilité
        updateFilterChip('ph', json);

        const orpCurrentValue = $("#orp_current_value");
        const orpCurrentLabel = $("#orp_current_label");
        if (orpCurrentValue) {
          if (orpCurrentLabel) orpCurrentLabel.textContent = "Valeur ORP actuelle";
          if (json.orp != null && typeof json.orp === "number" && !isNaN(json.orp)) {
            orpCurrentValue.textContent = Math.round(json.orp) + " mV";
          } else {
            orpCurrentValue.textContent = "--";
          }
        }
        // Chip d'état filtre ORP (feature-025)
        updateFilterChip('orp', json);

        const tempCurrentValue = $("#temp_current_value");
        const tempCurrentLabel = $("#temp_current_label");
        if (tempCurrentValue) {
          if (tempCurrentLabel) tempCurrentLabel.textContent = "Température actuelle";
          if (json.temperature != null && typeof json.temperature === "number" && !isNaN(json.temperature)) {
            tempCurrentValue.textContent = json.temperature.toFixed(1) + " °C";
          } else {
            tempCurrentValue.textContent = "--";
          }
        }

        // Mise à jour des readouts de la carte de calibration EZO (live)
        if (typeof updateCalibrationReadouts === "function") updateCalibrationReadouts(json);

        // Mettre à jour le timestamp du dernier chargement
        lastSensorDataLoadTime = Date.now();
        perf?.end("success");
      } catch (e) {
        // AbortError = timeout volontaire (10s) — ESP32 lent à répondre, pas une vraie erreur
        const isAbort = e?.name === 'AbortError';
        if (!isAbort) {
          console.error('loadSensorData error:', {
            name: e?.name,
            message: e?.message,
            stack: e?.stack,
            type: typeof e,
            error: e
          });
        }

        // Dégrader l'état rapidement en cas d'échec répété
        consecutiveFailures++;
        if (consecutiveFailures >= 2) {
          setNetStatus("bad", isAbort ? "Connexion lente" : "Hors ligne");
        } else {
          setNetStatus("mid", "Connexion…");
        }

        // If we never loaded data, retry quickly to get first paint asap
        if (lastSensorDataLoadTime === 0 && !sensorDataRetryTimer) {
          sensorDataRetryTimer = setTimeout(() => {
            sensorDataRetryTimer = null;
            loadSensorData({ force: true, source: "retry" });
          }, SENSOR_RETRY_MS);
        }
        perf?.end(`error=${e?.name || "Error"}`);
      } finally {
        sensorDataLoadInFlight = null;
      }
    })();

    // WS path has no awaits, so the IIFE above ran synchronously and the outer assignment
    // left sensorDataLoadInFlight as a resolved (truthy) Promise. Clear it so future
    // calls (WS or HTTP) are not incorrectly blocked.
    if (options.data) sensorDataLoadInFlight = null;

    return sensorDataLoadInFlight;
  }

  // ---------- Temperature calibration ----------
  let tempCalibrationStep = 0; // 0=idle, 1=plunge, 2=enterRef, 3=calibrate

  function updateTempCalibrationSteps() {
    const steps = [$("#temp_step1"), $("#temp_step2"), $("#temp_step3")];
    const startBtn = $("#temp_cal_start_btn");
    const cancelBtn = $("#temp_cal_cancel_btn");
    const refInput = $("#temp_reference_value");

    steps.forEach(el => el?.classList.remove("is-active", "is-completed"));

    if (tempCalibrationStep === 0) {
      if (startBtn) startBtn.textContent = "Commencer la calibration";
      if (cancelBtn) cancelBtn.style.display = "none";
      if (refInput) refInput.disabled = true;
    } else if (tempCalibrationStep === 1) {
      steps[0]?.classList.add("is-active");
      if (startBtn) startBtn.textContent = "Étape suivante";
      if (cancelBtn) cancelBtn.style.display = "inline-block";
      if (refInput) refInput.disabled = true;
    } else if (tempCalibrationStep === 2) {
      steps[0]?.classList.add("is-completed");
      steps[1]?.classList.add("is-active");
      if (startBtn) startBtn.textContent = "Étape suivante";
      if (cancelBtn) cancelBtn.style.display = "inline-block";
      if (refInput) refInput.disabled = false;
    } else if (tempCalibrationStep === 3) {
      steps[0]?.classList.add("is-completed");
      steps[1]?.classList.add("is-completed");
      steps[2]?.classList.add("is-active");
      if (startBtn) startBtn.textContent = "Calibrer";
      if (cancelBtn) cancelBtn.style.display = "inline-block";
      if (refInput) refInput.disabled = true;
    }
  }

  function bindTempCalibration() {
    const startBtn = $("#temp_cal_start_btn");
    const cancelBtn = $("#temp_cal_cancel_btn");
    const refInput = $("#temp_reference_value");
    const calibratedStatus = $("#temp_calibrated_status");

    cancelBtn?.addEventListener("click", () => {
      if (confirm("Annuler la calibration en cours ?")) {
        tempCalibrationStep = 0;
        if (refInput) refInput.value = "";
        updateTempCalibrationSteps();
        loadConfig();
      }
    });

    startBtn?.addEventListener("click", async () => {
      if (tempCalibrationStep === 0) {
        tempCalibrationStep = 1;
        updateTempCalibrationSteps();
        if (calibratedStatus) calibratedStatus.style.display = "none";
      } else if (tempCalibrationStep === 1) {
        tempCalibrationStep = 2;
        updateTempCalibrationSteps();
        if (refInput) refInput.focus();
      } else if (tempCalibrationStep === 2) {
        const referenceValue = parseFloat(refInput.value);
        if (isNaN(referenceValue) || referenceValue < -10 || referenceValue > 50) {
          alert("Température de référence invalide (-10 à 50 °C)");
          return;
        }
        tempCalibrationStep = 3;
        updateTempCalibrationSteps();
      } else if (tempCalibrationStep === 3) {
        const referenceValue = parseFloat(refInput.value);
        if (isNaN(referenceValue) || referenceValue < -10 || referenceValue > 50) {
          alert("Température de référence invalide (-10 à 50 °C)");
          tempCalibrationStep = 2;
          updateTempCalibrationSteps();
          return;
        }

        if (!latestSensorData || isNaN(latestSensorData.temperature) || latestSensorData.temperature == null) {
          alert("Aucune donnée capteur disponible (/data).");
          return;
        }

        // T° brute fournie par le firmware via le champ WS `temperature_raw` (sans offset).
        // Évite la formule fragile `calibrated - offset_précédent`. Fallback rétrocompat
        // si le champ est absent (firmware antérieur à feature-021 hotfix WS raw).
        let currentTempRaw;
        if (typeof latestSensorData.temperature_raw === "number" && !isNaN(latestSensorData.temperature_raw)) {
          currentTempRaw = latestSensorData.temperature_raw;
        } else {
          const currentTempCalibrated = latestSensorData.temperature;
          const previousOffset = (typeof window._config?.temp_calibration_offset === "number")
            ? window._config.temp_calibration_offset : 0;
          currentTempRaw = currentTempCalibrated - previousOffset;
        }
        const offset = referenceValue - currentTempRaw;
        const calibrationDate = new Date().toISOString();

        startBtn.disabled = true;
        try {
          const cfg = collectConfig();
          cfg.temp_calibration_offset = offset;
          cfg.temp_calibration_date = calibrationDate;

          const ok = await sendConfig(cfg);
          if (!ok) throw new Error("Impossible d'enregistrer la configuration");
          $("#temp_step1")?.classList.add("is-completed");
          $("#temp_step2")?.classList.add("is-completed");
          $("#temp_step3")?.classList.add("is-completed");
          $("#temp_step3")?.classList.remove("is-active");
          if (calibratedStatus) calibratedStatus.style.display = "block";

          tempCalibrationStep = 0;
          if (refInput) refInput.value = "";

          // Mise à jour immédiate de l'UI (optimiste) : la température calibrée doit correspondre à la référence
          const correctedTemp = referenceValue;
          const nowLabel = new Date().toLocaleTimeString();

          // Met à jour l'état local pour que l'écran reflète la calibration sans attendre
          if (!latestSensorData) latestSensorData = {};
          latestSensorData.temperature = correctedTemp;

          // Dashboard
          const mTemp = $("#m-temp");
          if (mTemp) mTemp.textContent = correctedTemp.toFixed(1);
          const mTime = $("#m-time");
          if (mTime) mTime.textContent = nowLabel;

          // Page température
          const tempCurrentValue = $("#temp_current_value");
          if (tempCurrentValue) tempCurrentValue.textContent = correctedTemp.toFixed(1) + " °C";

          // Invalider le timestamp pour forcer le rechargement
          lastSensorDataLoadTime = 0;

          // Rafraîchir la config pour mettre à jour la date de calibration
          await loadConfig();
          updateTempCalibrationSteps();

          // Recharger les données après un court délai pour se resynchroniser avec le backend
          setTimeout(async () => {
            await loadSensorData();
          }, 2000);
        } catch (err) {
          alert("Erreur calibration température:\n" + err.message);
          tempCalibrationStep = 0;
          updateTempCalibrationSteps();
        } finally {
          startBtn.disabled = false;
        }
      }
    });

    // Initialize
    updateTempCalibrationSteps();
  }

  // ---------- Atlas EZO Calibration (pH 2 points + ORP 1 point) ----------
  // Backend Pass 4a : POST /calibrate_ph {step:'mid'|'low'} et /calibrate_orp {reference:200..1000}.
  // L'EZO exécute la commande en background ; le firmware met ensuite à jour phCalPoints/orpCalPoints
  // via le push WS sensor_data. On poll latestSensorData jusqu'à constater l'incrément.
  let phCalibrationActive = false;
  let orpCalibrationActive = false;

  function _sleep(ms) { return new Promise(r => setTimeout(r, ms)); }

  // feature-034 itération 2 : applique l'état du chip de calibration (NON cliquable)
  // + le disabled du bouton Calibrer + le hint contextuel.
  // feature-034 itération 3 (volet A) : le bouton porte un LIBELLÉ FIXE
  // « Calibrer la sonde » (défini dans le HTML) ; on ne réécrit plus son texte
  // selon l'état (btnLabel ignoré). L'état reste porté par le chip et le hint.
  // Le garde « injection en cours » (_updateInjectButtons) reste PRIORITAIRE et
  // peut écraser btnDisabled/hint après cet appel.
  function setCalChip(prefix, { cls, label, btnDisabled, hint }) {
    const chip = $(`#${prefix}-cal-chip`);
    if (chip) chip.className = "chip chip--probe chip--cal " + cls;
    const lbl = $(`#${prefix}-cal-chip-label`);
    if (lbl) lbl.textContent = label;
    const btn = $(`#${prefix}_cal_trigger_btn`);
    if (btn) {
      // Libellé fixe « Calibrer la sonde » (cf. HTML) — on ne modifie que disabled.
      btn.disabled = !!btnDisabled;
    }
    const hintEl = $(`#${prefix}_cal_hint`);
    if (hintEl) hintEl.textContent = hint || "";
  }

  // Met à jour le chip d'état de calibration, les badges des sous-cartes, et les readouts live
  function renderCalibrationStatus() {
    const phPts = (latestSensorData?.phCalPoints != null) ? latestSensorData.phCalPoints : null;
    const orpPts = (latestSensorData?.orpCalPoints != null) ? latestSensorData.orpCalPoints : null;
    const phRaw = (typeof latestSensorData?.phRaw === "number" && !isNaN(latestSensorData.phRaw)) ? latestSensorData.phRaw
                : (typeof latestSensorData?.ph === "number" && !isNaN(latestSensorData.ph)) ? latestSensorData.ph : null;
    const orpRaw = (typeof latestSensorData?.orpRaw === "number" && !isNaN(latestSensorData.orpRaw)) ? latestSensorData.orpRaw
                 : (typeof latestSensorData?.orp === "number" && !isNaN(latestSensorData.orp)) ? latestSensorData.orp : null;

    // ===== pH =====
    // feature-034 : le chip « sonde + calibration » pH est piloté par
    // updatePhProbeChip (#ph-probe-chip). Ici on ne gère plus que le bouton
    // « Calibrer la sonde », son hint, et le header de la carte calibration.
    const phUnreachable = (phPts === -1);
    const phComplete = (phPts != null && phPts >= 2);
    const phPartial = (phPts === 1);

    // Bouton + hint pH (priorité : EZO injoignable > état calibration).
    // Le garde injection est appliqué après (prioritaire).
    const phEzoDown = (phPts != null && phPts < 0);
    const phBtn = $("#ph_cal_trigger_btn");
    if (phBtn) phBtn.disabled = phEzoDown;
    const phHint = $("#ph_cal_hint");
    if (phHint) {
      if (phEzoDown) phHint.textContent = "EZO pH injoignable — vérifiez le câblage I²C et l'alimentation.";
      else if (phPartial) phHint.textContent = "Régulation auto inhibée (1/2 point).";
      else if (phPts === 0) phHint.textContent = "Régulation auto inhibée tant que non calibré.";
      else phHint.textContent = "";
    }

    // Status header de la carte Calibration pH
    const phHeader = $("#ph_cal_status_header");
    if (phHeader) {
      if (phUnreachable) phHeader.textContent = "EZO pH injoignable";
      else if (phComplete) phHeader.textContent = "Calibré 2 points ✓";
      else if (phPartial) phHeader.textContent = "1 point calibré sur 2";
      else phHeader.textContent = "Aucun point calibré";
    }

    // ===== ORP =====
    const orpUnreachable = (orpPts === -1);
    const orpCalibrated = (orpPts != null && orpPts >= 1);

    // Chip d'état de calibration ORP (1 point, pas d'état warn).
    if (orpPts == null || orpPts < 0 || orpRaw == null) {
      const ezoDown = (orpPts != null && orpPts < 0);
      setCalChip("orp", {
        cls: "chip--probe-unknown",
        label: ezoDown ? "EZO indisponible" : "Calibration —",
        btnDisabled: ezoDown,
        hint: ezoDown ? "EZO ORP injoignable — vérifiez le câblage I²C." : "",
      });
    } else if (orpCalibrated) {
      setCalChip("orp", { cls: "chip--probe-good", label: "Calibré", btnDisabled: false, hint: "" });
    } else { // orpPts === 0
      setCalChip("orp", { cls: "chip--probe-bad", label: "Calibration requise", btnDisabled: false, hint: "Régulation auto inhibée tant que non calibré." });
    }

    const orpHeader = $("#orp_cal_status_header");
    if (orpHeader) {
      if (orpUnreachable) orpHeader.textContent = "EZO ORP injoignable";
      else if (orpCalibrated) orpHeader.textContent = "Calibré ✓";
      else orpHeader.textContent = "Non calibré";
    }

    // INVARIANT : le garde « injection en cours » est PRIORITAIRE sur l'état
    // calibration. Appliqué en dernier ici pour rester robuste quel que soit
    // l'ordre d'appel (renderCalibrationStatus vs _updateInjectButtons).
    // Ordre de priorité : injection > EZO injoignable > état calibration.
    const phInjecting = (latestSensorData?.ph_inject_remaining_s ?? 0) > 0
                      || (latestSensorData?.ph_dosing === true);
    const orpInjecting = (latestSensorData?.orp_inject_remaining_s ?? 0) > 0
                       || (latestSensorData?.orp_dosing === true);
    if (phInjecting && !phCalibrationActive) {
      const b = $("#ph_cal_trigger_btn"); if (b) b.disabled = true;
      const h = $("#ph_cal_hint"); if (h) h.textContent = "Injection en cours, calibration indisponible";
    }
    if (orpInjecting && !orpCalibrationActive) {
      const b = $("#orp_cal_trigger_btn"); if (b) b.disabled = true;
      const h = $("#orp_cal_hint"); if (h) h.textContent = "Injection en cours, calibration indisponible";
    }

    // feature-034 it.5 : détection événementielle de fin de calibration (remplace le polling
    // bloquant). Exécuté à chaque mise à jour de données puisque renderCalibrationStatus est
    // appelé par updateCalibrationReadouts (lui-même appelé à chaque fetch /data ET WS push).
    checkCalAwait();
  }

  // Appelé par la boucle sensor_data : met à jour les readouts live des cards de calibration
  function updateCalibrationReadouts(json) {
    // Calibration (feature-034 it.4) : on N'affiche PAS la valeur lissée de la
    // RÉGULATION (médiane+EMA+rejet de saut) — elle se figerait ~1 min en changeant
    // de solution étalon (le rejet `maxStep` bloque le grand saut). À la place on
    // affiche un LISSAGE LÉGER dédié calibration : la médiane des ~5 dernières mesures
    // brutes (sans rejet, sans latch) → suit immédiatement la solution tout en gommant
    // le jitter. Repli sur le brut si la fenêtre est vide.
    const phRaw = (typeof json.phRaw === "number" && !isNaN(json.phRaw)) ? json.phRaw
                : (typeof json.ph === "number" && !isNaN(json.ph)) ? json.ph : null;
    const orpRaw = (typeof json.orpRaw === "number" && !isNaN(json.orpRaw)) ? json.orpRaw
                 : (typeof json.orp === "number" && !isNaN(json.orp)) ? json.orp : null;

    // Alimenter d'abord la fenêtre glissante (sert au lissage ET à la stabilité).
    if (phCalibrationActive && phRaw != null) pushStabilitySample("ph", phRaw);
    if (orpCalibrationActive && orpRaw != null) pushStabilitySample("orp", orpRaw);

    // FIGEAGE des étapes franchies : une fois une étape « Calibrer » passée (son index <
    // étape active courante), on STOPPE la mise à jour de sa lecture (elle reste figée à la
    // valeur lue au moment de la calibration) et on DÉSACTIVE son bouton. Étapes calibrer :
    // pH mid = step 1, pH low = step 3, ORP = step 2.
    const phActive = _calActiveIdx.ph;
    const phMid = $("#cal-ph-mid-readout");
    const phLow = $("#cal-ph-low-readout");
    const phV = phCalibrationActive ? calSmoothed("ph", phRaw) : phRaw;
    const phStr = (phV != null) ? phV.toFixed(2) : "--";
    if (phMid && phActive <= 1) phMid.textContent = phStr;   // figé si étape mid (1) franchie
    if (phLow && phActive <= 3) phLow.textContent = phStr;   // figé si étape low (3) franchie

    const orpActive = _calActiveIdx.orp;
    const orpRo = $("#cal-orp-readout");
    if (orpRo && orpActive <= 2) {   // figé si étape calibrer ORP (2) franchie
      const v = orpCalibrationActive ? calSmoothed("orp", orpRaw) : orpRaw;
      orpRo.textContent = (v != null) ? Math.round(v) + " mV" : "--";
    }
    _calRefreshFrozenButtons(); // désactive les boutons des étapes calibrer franchies

    if (phCalibrationActive && phRaw != null) renderStability("ph");
    if (orpCalibrationActive && orpRaw != null) renderStability("orp");
    // Synchroniser badges/chips/header à chaque push
    renderCalibrationStatus();
  }

  // Lissage léger dédié calibration : médiane des ~5 dernières mesures brutes de la
  // fenêtre glissante (aucun rejet, aucun latch). Repli sur `fallback` si vide.
  function calSmoothed(product, fallback) {
    const arr = _calStab[product];
    if (!arr || !arr.length) return fallback;
    const recent = arr.slice(-5).map(s => s.v).sort((a, b) => a - b);
    const mid = Math.floor(recent.length / 2);
    return recent.length % 2 ? recent[mid] : (recent[mid - 1] + recent[mid]) / 2;
  }

  // ---------- feature-034 : moteur stepper + stabilité ----------
  const CAL_STAB_WINDOW_MS = 20000;          // fenêtre glissante d'analyse de stabilité (~20 s)
  // Seuils INDICATIFS au-dessus du bruit de lecture (~0,07 pH observé) pour distinguer
  // « sonde stabilisée (bruit seul) » de « encore en dérive ». Cosmétiques (non bloquants).
  const CAL_STAB_THRESHOLD = { ph: 0.9, orp: 10 };
  const CAL_STAB_WINDOW_S = Math.round(CAL_STAB_WINDOW_MS / 1000);

  // Indices d'étapes du stepper que l'utilisateur fait avancer manuellement.
  const CAL_MANUAL_STEPS = { ph: [0, 2], orp: [0, 1] };
  const CAL_STEP_COUNT   = { ph: 5, orp: 4 };

  const _calStab = { ph: [], orp: [] };      // [{t, v}] fenêtre glissante
  const _calActiveIdx = { ph: 0, orp: 0 };   // index de l'étape active (pour figer les étapes franchies)

  function pushStabilitySample(product, value) {
    const now = Date.now();
    const arr = _calStab[product];
    arr.push({ t: now, v: value });
    const cutoff = now - CAL_STAB_WINDOW_MS;
    while (arr.length && arr[0].t < cutoff) arr.shift();
  }

  function renderStability(product) {
    // Cible UNIQUEMENT le libellé de stabilité de l'étape calibrer ACTIVE (mid=1, low=3, ORP=2).
    // Les étapes franchies gardent leur libellé figé (comme leur lecture).
    let el = null;
    if (product === "ph") {
      if (_calActiveIdx.ph === 1) el = $("#cal-ph-mid-stability");
      else if (_calActiveIdx.ph === 3) el = $("#cal-ph-low-stability");
    } else if (_calActiveIdx.orp === 2) {
      el = $("#cal-orp-stability");
    }
    if (!el) return;
    const arr = _calStab[product];
    let txt = "○ Stabilisation…", stable = false;
    if (arr.length >= 3) {
      let min = arr[0].v, max = arr[0].v;
      for (const s of arr) { if (s.v < min) min = s.v; if (s.v > max) max = s.v; }
      const delta = max - min;
      stable = delta <= CAL_STAB_THRESHOLD[product];
      const fmt = product === "ph" ? delta.toFixed(2) : Math.round(delta) + " mV";
      txt = stable
        ? "● Stable — vous pouvez calibrer"
        : "○ Stabilisation… (Δ" + CAL_STAB_WINDOW_S + "s " + fmt + ")";
    }
    el.textContent = txt;
    el.classList.toggle("is-stable", stable);
  }

  // --- progression du stepper ---
  function setCalStepState(product, idx) {
    _calActiveIdx[product] = idx;
    const steps = $$(`#${product}-cal-steps .step`);
    steps.forEach((li) => {
      const i = parseInt(li.dataset.step, 10);
      li.classList.remove("is-active", "is-completed", "is-upcoming");
      li.removeAttribute("aria-current");
      if (i < idx) {
        li.classList.add("is-completed");
      } else if (i === idx) {
        li.classList.add("is-active");
        li.setAttribute("aria-current", "step");
      } else {
        li.classList.add("is-upcoming");
      }
    });
  }

  function resetCalStepper(product) {
    _calStab[product] = [];
    renderStability(product);
    setCalStepState(product, 0);
    // Ré-armer les boutons « Calibrer » figés d'une session précédente (réouverture).
    const btnIds = product === "ph" ? ["btn-cal-ph-mid", "btn-cal-ph-low"] : ["btn-cal-orp"];
    btnIds.forEach(id => { const b = $("#" + id); if (b) b.disabled = false; });
  }

  // Avance manuelle (boutons « C'est fait → »).
  function advanceCalStepper(product, fromIdx) {
    const active = $$(`#${product}-cal-steps .step.is-active`)[0];
    if (!active || parseInt(active.dataset.step, 10) !== fromIdx) return;
    setCalStepState(product, fromIdx + 1);
    _calStab[product] = []; // repartir d'une fenêtre propre à chaque changement de solution
    renderStability(product);
    focusActiveStep(product);
  }

  // Marque une étape « calibrer » comme faite après succès EZO et passe à la suivante.
  function completeCalStep(product, idx) {
    setCalStepState(product, idx + 1);
    _calStab[product] = [];
    renderStability(product);
    focusActiveStep(product);
  }

  function focusActiveStep(product) {
    const active = $$(`#${product}-cal-steps .step.is-active`)[0];
    const btn = active ? active.querySelector("button, input") : null;
    if (btn) { try { btn.focus(); } catch (_) {} }
  }
  function focusFirstStep(product) {
    // léger délai pour laisser la carte s'afficher avant de déplacer le focus
    setTimeout(() => focusActiveStep(product), 60);
  }

  function bindStepperControls(product) {
    $$(`#${product}-cal-stepper .cal-step-advance`).forEach(btn => {
      btn.addEventListener("click", () => advanceCalStepper(product, parseInt(btn.dataset.advance, 10)));
    });
  }

  function showPhCalibrationCard() {
    const reg = $("#ph-card-regulation");
    const hist = $("#ph-card-history");
    const cal = $("#ph-card-calibration");
    const calBtn = $("#ph_cal_trigger_btn");
    if (reg) reg.style.display = "none";
    if (hist) hist.style.display = "none";
    if (cal) cal.style.display = "";
    if (calBtn) calBtn.disabled = true;
  }
  function hidePhCalibrationCard() {
    _calEndAwait(); // annuler une attente de calibration en cours
    const reg = $("#ph-card-regulation");
    const hist = $("#ph-card-history");
    const cal = $("#ph-card-calibration");
    const calBtn = $("#ph_cal_trigger_btn");
    if (reg) reg.style.display = "";
    if (hist) hist.style.display = "";
    if (cal) cal.style.display = "none";
    if (calBtn) calBtn.disabled = false;
    updatePhModeControls();
  }
  function showOrpCalibrationCard() {
    const reg = $("#orp-card-regulation");
    const hist = $("#orp-card-history");
    const cal = $("#orp-card-calibration");
    const calBtn = $("#orp_cal_trigger_btn");
    if (reg) reg.style.display = "none";
    if (hist) hist.style.display = "none";
    if (cal) cal.style.display = "";
    if (calBtn) calBtn.disabled = true;
  }
  function hideOrpCalibrationCard() {
    _calEndAwait(); // annuler une attente de calibration en cours
    const reg = $("#orp-card-regulation");
    const hist = $("#orp-card-history");
    const cal = $("#orp-card-calibration");
    const calBtn = $("#orp_cal_trigger_btn");
    if (reg) reg.style.display = "";
    if (hist) hist.style.display = "";
    if (cal) cal.style.display = "none";
    if (calBtn) calBtn.disabled = false;
    updateOrpModeControls();
  }

  // ---------- feature-034 : calibration ÉVÉNEMENTIELLE (remplace le polling bloquant) ----------
  // Plus de boucle `while + await`. Au clic « Calibrer », on POST puis on enregistre une
  // attente (_calAwait) ; la détection de fin se fait dans checkCalAwait(), appelé à CHAQUE
  // mise à jour de données (WS push, fetch /data périodique/forcé, retour de focus). C'est
  // robuste : insensible à la pause des timers/WebSocket par Safari (au retour sur l'onglet,
  // le refresh déclenche la détection). _calPollTimer force /data en best-effort pendant l'attente.
  let _calAwait = null;
  let _calPollTimer = null;
  const CAL_AWAIT_TIMEOUT_MS = 20000;
  // Délai d'exécution EZO avant de valider une RECALIBRATION (point déjà calibré : le compteur
  // de points ne change pas, donc on ne peut pas se fier à un incrément). Au-delà de ce délai,
  // si la sonde est joignable et que le point attendu est présent, on considère la cal réussie.
  const CAL_SETTLE_MS = 4000;

  function _calStartPolling() {
    if (_calPollTimer) clearInterval(_calPollTimer);
    _calPollTimer = setInterval(() => { loadSensorData({ force: true, source: "cal-await" }); }, 1500);
    loadSensorData({ force: true, source: "cal-await" }); // refresh immédiat
  }

  // Désactive les boutons « Calibrer » des étapes déjà franchies (figeage). Idempotent.
  function _calRefreshFrozenButtons() {
    { const b = $("#btn-cal-ph-mid"); if (b && _calActiveIdx.ph > 1) b.disabled = true; }
    { const b = $("#btn-cal-ph-low"); if (b && _calActiveIdx.ph > 3) b.disabled = true; }
    { const b = $("#btn-cal-orp");    if (b && _calActiveIdx.orp > 2) b.disabled = true; }
  }

  function _calEndAwait() {
    if (_calPollTimer) { clearInterval(_calPollTimer); _calPollTimer = null; }
    if (_calAwait) {
      const a = _calAwait;
      _calAwait = null; // libérer AVANT de toucher le DOM (évite toute ré-entrance)
      const btn = $("#" + a.btnId);
      if (btn) { btn.disabled = false; btn.textContent = a.originalLabel; }
      if (a.otherBtnId) { const o = $("#" + a.otherBtnId); if (o) o.disabled = false; }
      if (a.roId) { const r = $("#" + a.roId); if (r) r.classList.remove("is-pulsing"); }
    }
    // Succès → l'étape vient d'être franchie : re-figer son bouton (sinon réactivé ci-dessus).
    _calRefreshFrozenButtons();
  }

  // Détection de fin de calibration — appelée depuis renderCalibrationStatus() à chaque rendu.
  function checkCalAwait() {
    if (!_calAwait) return;
    const a = _calAwait;
    const pts = a.product === "ph" ? latestSensorData?.phCalPoints : latestSensorData?.orpCalPoints;
    if (pts === -1) {
      showToast("Calibration " + a.stepLabel + " : EZO injoignable", "error");
      _calEndAwait();
      return;
    }
    const elapsed = Date.now() - a.startMs;
    // Nombre de points attendu après cette étape (sert au cas recalibration).
    const expectedMin = a.product === "ph" ? (a.step === "mid" ? 1 : 2) : 1;
    // Succès si : (a) le compteur a augmenté → nouvelle calibration d'un point non encore fait ;
    // OU (b) cas RECALIBRATION — le point était déjà calibré donc le compteur ne change pas :
    //   après le délai d'exécution EZO, la sonde est joignable et le point attendu est présent.
    const incremented = (typeof pts === "number") && (pts > a.baseline);
    const settled = (typeof pts === "number") && (pts >= expectedMin) && (elapsed > CAL_SETTLE_MS);
    if (incremented || settled) {
      const detail = a.product === "ph" ? " (" + pts + "/2 points)"
                   : (a.reference != null ? " (référence " + a.reference + " mV)" : "");
      showToast("Calibration " + a.stepLabel + " réussie" + detail, "success");
      if (a.product === "ph") completeCalStep("ph", a.step === "mid" ? 1 : 3);
      else completeCalStep("orp", 2);
      _calEndAwait();
      return;
    }
    if (elapsed > CAL_AWAIT_TIMEOUT_MS) {
      showToast("Calibration " + a.stepLabel + " : délai dépassé — vérifier la sonde", "warning");
      _calEndAwait();
    }
  }

  // POST /calibrate_ph {step} — non bloquant : la détection se fait via checkCalAwait().
  async function calibratePh(step /* 'mid' | 'low' */) {
    if (_calAwait) return; // une calibration déjà en attente
    const btnId = step === "mid" ? "btn-cal-ph-mid" : "btn-cal-ph-low";
    const otherBtnId = step === "mid" ? "btn-cal-ph-low" : "btn-cal-ph-mid";
    const roId = step === "mid" ? "cal-ph-mid-readout" : "cal-ph-low-readout";
    const stepLabel = step === "mid" ? "pH 7.0" : "pH 4.0";
    const btn = $("#" + btnId);
    const originalLabel = btn ? btn.textContent : "Calibrer";
    const baseline = (typeof latestSensorData?.phCalPoints === "number" && latestSensorData.phCalPoints >= 0)
                   ? latestSensorData.phCalPoints : 0;
    try {
      const res = await authFetch("/calibrate_ph", {
        method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify({ step })
      });
      if (!res.ok) {
        let errMsg = "HTTP " + res.status;
        try { const j = await res.json(); if (j?.error) errMsg = j.error; } catch (_) {}
        showToast("Erreur calibration " + stepLabel + " : " + errMsg, "error");
        return;
      }
      showToast("Calibration " + stepLabel + " envoyée — exécution EZO en cours…", "info");
      if (btn) { btn.disabled = true; btn.textContent = "Calibration en cours…"; }
      const other = $("#" + otherBtnId); if (other) other.disabled = true;
      const ro = $("#" + roId); if (ro) ro.classList.add("is-pulsing");
      _calAwait = { product: "ph", step, baseline, startMs: Date.now(), btnId, otherBtnId, roId, originalLabel, stepLabel };
      _calStartPolling();
    } catch (e) {
      showToast("Erreur réseau : " + (e?.message || e), "error");
    }
  }

  // POST /calibrate_orp {reference} — non bloquant.
  async function calibrateOrp(reference) {
    if (_calAwait) return;
    const btnId = "btn-cal-orp";
    const roId = "cal-orp-readout";
    const stepLabel = "ORP";
    const btn = $("#" + btnId);
    const originalLabel = btn ? btn.textContent : "Calibrer";
    const baseline = (typeof latestSensorData?.orpCalPoints === "number" && latestSensorData.orpCalPoints >= 0)
                   ? latestSensorData.orpCalPoints : 0;
    try {
      const res = await authFetch("/calibrate_orp", {
        method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify({ reference })
      });
      if (!res.ok) {
        let errMsg = "HTTP " + res.status;
        try { const j = await res.json(); if (j?.error) errMsg = j.error; } catch (_) {}
        showToast("Erreur calibration ORP : " + errMsg, "error");
        return;
      }
      showToast("Calibration ORP envoyée — exécution EZO en cours…", "info");
      if (btn) { btn.disabled = true; btn.textContent = "Calibration en cours…"; }
      const ro = $("#" + roId); if (ro) ro.classList.add("is-pulsing");
      _calAwait = { product: "orp", step: "orp", baseline, startMs: Date.now(), btnId, otherBtnId: null, roId, originalLabel, stepLabel, reference };
      _calStartPolling();
    } catch (e) {
      showToast("Erreur réseau : " + (e?.message || e), "error");
    }
  }

  function bindPhCalibration() {
    // Bouton "Calibrer" (carte Régulation) → ouvre la carte de calibration
    const triggerBtn = $("#ph_cal_trigger_btn");
    triggerBtn?.addEventListener("click", () => {
      if (phCalibrationActive) return;
      // feature-034 (B5) : garde injection pH (calqué sur le pattern ORP).
      const phInjecting = (latestSensorData?.ph_inject_remaining_s ?? 0) > 0
                        || (latestSensorData?.ph_dosing === true);
      if (phInjecting) { showToast("Calibration impossible pendant une injection pH", "error"); return; }
      phCalibrationActive = true;
      showPhCalibrationCard();
      resetCalStepper("ph");
      renderCalibrationStatus();
      focusFirstStep("ph");
    });

    // Bouton "Fermer" → revient à la carte Régulation
    const closeBtn = $("#ph_cal_close_btn");
    closeBtn?.addEventListener("click", () => {
      phCalibrationActive = false;
      hidePhCalibrationCard();
      $("#ph_cal_trigger_btn")?.focus();
    });

    // Boutons de calibration
    $("#btn-cal-ph-mid")?.addEventListener("click", () => calibratePh("mid"));
    $("#btn-cal-ph-low")?.addEventListener("click", () => calibratePh("low"));

    bindStepperControls("ph");
    renderCalibrationStatus();
  }

  function bindOrpCalibration() {
    const triggerBtn = $("#orp_cal_trigger_btn");
    triggerBtn?.addEventListener("click", () => {
      if (orpCalibrationActive) return;
      const orpInjecting = (latestSensorData?.orp_inject_remaining_s ?? 0) > 0
                        || (latestSensorData?.orp_dosing === true);
      if (orpInjecting) { showToast("Calibration impossible pendant une injection ORP", "error"); return; }
      orpCalibrationActive = true;
      showOrpCalibrationCard();
      resetCalStepper("orp");
      renderCalibrationStatus();
      focusFirstStep("orp");
    });

    const closeBtn = $("#orp_cal_close_btn");
    closeBtn?.addEventListener("click", () => {
      orpCalibrationActive = false;
      hideOrpCalibrationCard();
      $("#orp_cal_trigger_btn")?.focus();
    });

    $("#btn-cal-orp")?.addEventListener("click", () => {
      const refInput = $("#orp-cal-reference");
      const reference = parseInt(refInput?.value, 10);
      if (isNaN(reference) || reference < 200 || reference > 1000) {
        showToast("Valeur de référence invalide (200–1000 mV)", "error");
        return;
      }
      calibrateOrp(reference);
    });

    bindStepperControls("orp");
    renderCalibrationStatus();
  }

  // ---------- Wi-Fi / System / Logs / Updates (ported endpoints) ----------
  function updateWiFiDisplay() {
    if (!window._wifiData) return;

    const wifi = window._wifiData;
    const ssidEl = $("#wifi_ssid");
    const ipEl = $("#wifi_ip");
    const modeEl = $("#wifi_mode");
    const mdnsEl = $("#wifi_mdns");

    if (ssidEl) ssidEl.textContent = wifi.ssid;
    if (ipEl) ipEl.textContent = wifi.ip;
    if (modeEl) modeEl.textContent = wifi.mode;
    if (mdnsEl) mdnsEl.textContent = wifi.mdns;
  }

  async function loadSystemInfo() {
    try {
      const res = await authFetch("/get-system-info");
      const data = await res.json();

      $("#sys_firmware_version").textContent = data.firmware_version || "—";
      $("#sys_current_firmware_version").textContent = data.firmware_version || "—";
      $("#sys_build_date").textContent = data.build_date || "—";

      // Format uptime
      if (data.uptime_days !== undefined) {
        const days = data.uptime_days;
        const hours = data.uptime_hours || 0;
        const minutes = data.uptime_minutes || 0;
        $("#sys_uptime").textContent = `${days}j ${hours}h ${minutes}m`;
      } else {
        $("#sys_uptime").textContent = "—";
      }

      $("#sys_chip_model").textContent = data.chip_model || "—";
      $("#sys_cpu_freq").textContent = data.cpu_freq_mhz ? `${data.cpu_freq_mhz} MHz` : "—";
      $("#sys_free_heap").textContent = data.free_heap ? `${Math.round(data.free_heap / 1024)} KB` : "—";
      $("#sys_flash_size").textContent = data.flash_size ? `${Math.round(data.flash_size / (1024 * 1024))} MB` : "—";

      if (data.fs_used_bytes && data.fs_total_bytes) {
        const used = Math.round(data.fs_used_bytes / 1024);
        const total = Math.round(data.fs_total_bytes / 1024);
        const pct = Math.round((data.fs_used_bytes / data.fs_total_bytes) * 100);
        $("#sys_fs_usage").textContent = `${used} KB / ${total} KB (${pct}%)`;
      } else {
        $("#sys_fs_usage").textContent = "—";
      }

      $("#sys_wifi_rssi").textContent = data.wifi_rssi != null ? `${data.wifi_rssi} dBm` : "—";
      $("#sys_mac_address").textContent = data.wifi_mac || "—";
    } catch (e) {
      // ignore
    }
  }

  async function loadCoredumpInfo() {
    try {
      const res = await authFetch("/coredump/info");
      if (!res.ok) { $("#coredump_status").textContent = "Erreur API"; return; }
      const d = await res.json();
      const available = d.available === true;
      $("#coredump_status").textContent = available ? "Disponible" : "Aucun coredump";
      const details = $("#coredump_details");
      if (details) details.style.display = available ? "" : "none";
      if (available) {
        $("#coredump_task").textContent  = d.task  || "—";
        $("#coredump_exc").textContent   = d.exc_cause_str ? `${d.exc_cause_str} (${d.exc_cause})` : "—";
        $("#coredump_pc").textContent    = d.pc ? "0x" + d.pc.toString(16) : "—";
      }
      const dl = $("#coredump_download_btn");
      const er = $("#coredump_erase_btn");
      if (dl) dl.disabled = !available;
      if (er) er.disabled = !available;
    } catch (e) {
      $("#coredump_status").textContent = "Erreur";
    }
  }

  // GitHub update
  let latestRelease = null;

  function bindGithubUpdate() {
    const checkBtn = $("#check_update_btn");
    const installBtn = $("#install_update_btn");

    const info = $("#github_update_info");
    const progress = $("#github_update_progress");
    const bar = $("#github_update_progress_bar");
    const status = $("#github_update_status");

    checkBtn?.addEventListener("click", async () => {
      checkBtn.disabled = true;
      checkBtn.textContent = "Vérification…";
      if (info) info.style.display = "none";
      if (installBtn) installBtn.disabled = true;

      try {
        const res = await authFetch("/check-update");
        let data = null;
        if (!res.ok) {
          try {
            data = await res.json();
          } catch (_) {}
          const errMsg = data?.error || "Erreur vérification";
          throw new Error(errMsg);
        }
        data = await res.json();
        latestRelease = data;

        $("#current_version").textContent = data.current_version;
        $("#latest_version").textContent = data.latest_version;
        if (info) info.style.display = "block";

        if (data.no_release) {
          $("#update_available_msg").style.display = "none";
          $("#no_update_msg").style.display = "block";
          $("#no_update_msg").innerHTML = "<strong>ℹ️ " + (data.message || "Aucune release") + "</strong>";
          $("#release_info").style.display = "none";
          installBtn.disabled = true;
        } else if (data.update_available) {
          $("#update_available_msg").style.display = "block";
          $("#no_update_msg").style.display = "none";
          $("#release_info").style.display = "block";
          $("#release_name").textContent = data.release_name || "-";
          $("#release_date").textContent = data.published_at ? new Date(data.published_at).toLocaleString("fr-FR") : "-";
          $("#release_notes").textContent = data.release_notes || "Aucune note";
          installBtn.disabled = !data.firmware_url;
        } else {
          $("#update_available_msg").style.display = "none";
          $("#no_update_msg").style.display = "block";
          $("#no_update_msg").innerHTML = "<strong>✓ Vous avez la dernière version</strong>";
          $("#release_info").style.display = "none";
          installBtn.disabled = true;
        }
      } catch (e) {
        const msg = String(e?.message || "");
        if (msg.includes("System time not synchronized")) {
          alert("Heure non synchronisée.\nActive le NTP ou règle l'heure dans les réglages avant de vérifier les mises à jour.");
        } else {
          alert("Erreur vérification mise à jour.\n" + (msg || "Vérifie la connexion Internet de l'ESP32."));
        }
      } finally {
        checkBtn.textContent = "Vérifier";
        checkBtn.disabled = false;
      }
    });

    installBtn?.addEventListener("click", async () => {
      if (!latestRelease || !latestRelease.firmware_url || !latestRelease.filesystem_url) {
        alert("Aucune mise à jour disponible.");
        return;
      }

      if (
        !confirm(
          `⚠️ ATTENTION\n\nInstaller ${latestRelease.latest_version}\n\n• Filesystem puis firmware\n• Ne pas couper l'alimentation\n\nContinuer ?`
        )
      ) {
        return;
      }

      checkBtn.disabled = true;
      installBtn.disabled = true;

      progress.style.display = "block";
      bar.style.width = "0%";
      bar.textContent = "0%";
      status.textContent = "1/2 Filesystem : téléchargement…";

      try {
        // step 1 FS - téléchargement + installation
        let p = 0;
        let t = setInterval(() => {
          if (p < 40) {
            p += 2;
            bar.style.width = p + "%";
            bar.textContent = p + "%";
          }
        }, 800);

        const fsRes = await authFetch("/download-update", {
          method: "POST",
          headers: { "Content-Type": "application/x-www-form-urlencoded" },
          body: "url=" + encodeURIComponent(latestRelease.filesystem_url) + "&restart=false",
        });

        clearInterval(t);
        bar.style.width = "45%";
        bar.textContent = "45%";
        status.textContent = "1/2 Filesystem : installation…";

        let fsJson = null;
        if (!fsRes.ok) {
          try {
            fsJson = await fsRes.json();
          } catch (_) {}
          const errMsg = fsJson?.error || "Erreur téléchargement filesystem";
          throw new Error(errMsg);
        }
        fsJson = await fsRes.json();
        if (fsJson.status !== "success") throw new Error("Erreur installation filesystem");

        bar.style.width = "50%";
        bar.textContent = "50%";
        status.textContent = "✓ Filesystem OK — 2/2 Firmware : téléchargement…";

        // step 2 FW - téléchargement + installation
        p = 50;
        t = setInterval(() => {
          if (p < 90) {
            p += 2;
            bar.style.width = p + "%";
            bar.textContent = p + "%";
          }
        }, 800);

        let fwSuccess = false;
        try {
          const fwRes = await authFetch("/download-update", {
            method: "POST",
            headers: { "Content-Type": "application/x-www-form-urlencoded" },
            body: "url=" + encodeURIComponent(latestRelease.firmware_url) + "&restart=true",
          });

          clearInterval(t);

          let fwJson = null;
          if (!fwRes.ok) {
            try {
              fwJson = await fwRes.json();
            } catch (_) {}
            const errMsg = fwJson?.error || "Erreur téléchargement firmware";
            throw new Error(errMsg);
          }
          fwJson = await fwRes.json();
          fwSuccess = fwJson.status === "success";
        } catch (fwErr) {
          clearInterval(t);
          // Network error is expected when ESP32 restarts after firmware install
          // If we got here after filesystem was OK, assume firmware install succeeded
          const errMsg = String(fwErr?.message || "");
          if (errMsg.includes("System time not synchronized")) {
            throw fwErr; // Re-throw time sync errors
          }
          // For network errors (fetch failed), assume success since ESP32 restarted
          fwSuccess = true;
        }

        if (fwSuccess) {
          bar.style.width = "100%";
          bar.textContent = "100%";
          // Countdown before reload
          let countdown = 15;
          const updateCountdown = () => {
            status.textContent = `✓ Mise à jour terminée. Rechargement dans ${countdown}s…`;
            if (countdown <= 0) {
              window.location.reload();
            } else {
              countdown--;
              setTimeout(updateCountdown, 1000);
            }
          };
          updateCountdown();
          return; // Exit to avoid finally block re-enabling buttons
        } else {
          throw new Error("Erreur installation firmware");
        }
      } catch (e) {
        const msg = String(e?.message || "");
        if (msg.includes("System time not synchronized")) {
          alert("Heure non synchronisée.\nActive le NTP ou règle l'heure dans les réglages avant de lancer la mise à jour.");
        } else {
          alert("Erreur mise à jour:\n" + e.message);
        }
        checkBtn.disabled = false;
      }
    });
  }

  // Manual update (/update)
  function bindManualUpdate() {
    const file = $("#update_file");
    const btn = $("#update_btn");
    const type = $("#update_type");

    file?.addEventListener("change", () => {
      btn.disabled = !(file.files && file.files.length);
    });

    btn?.addEventListener("click", () => {
      if (!(file.files && file.files.length)) return;

      const formData = new FormData();
      formData.append("update_type", type.value);
      formData.append("update_file", file.files[0]);

      const progressWrap = $("#update_progress");
      const bar = $("#update_progress_bar");
      const status = $("#update_status");

      progressWrap.style.display = "block";
      bar.style.width = "0%";
      bar.textContent = "0%";
      status.textContent = "Préparation…";

      btn.disabled = true;
      file.disabled = true;
      type.disabled = true;

      const xhr = new XMLHttpRequest();
      let uploadCompleted = false;

      xhr.upload.addEventListener("progress", (e) => {
        if (!e.lengthComputable) return;
        const percent = Math.round((e.loaded / e.total) * 100);
        bar.style.width = percent + "%";
        bar.textContent = percent + "%";
        status.textContent = percent < 100 ? `Envoi: ${percent}%` : "Envoi terminé, application en cours…";
      });

      xhr.addEventListener("load", () => {
        if (xhr.status === 200 && xhr.responseText.trim() === "OK") {
          bar.style.width = "100%";
          bar.textContent = "100%";
          let countdown = 30;
          const updateCountdown = () => {
            status.textContent = `✓ Mise à jour réussie. Rechargement dans ${countdown}s…`;
            if (countdown <= 0) {
              window.location.reload();
            } else {
              countdown--;
              setTimeout(updateCountdown, 1000);
            }
          };
          updateCountdown();
        } else {
          bar.style.width = "100%";
          bar.classList.add("error");
          const msg = xhr.responseText.trim() || `Erreur HTTP ${xhr.status}`;
          status.textContent = `✗ Échec de la mise à jour: ${msg}`;
          btn.disabled = false;
          file.disabled = false;
          type.disabled = false;
        }
      });

      xhr.addEventListener("error", () => {
        status.textContent = "✗ Erreur réseau";
        btn.disabled = false;
        file.disabled = false;
        type.disabled = false;
      });

      xhr.open("POST", "/update");
      const token = sessionStorage.getItem('authToken');
      if (token) xhr.setRequestHeader("X-Auth-Token", token);
      xhr.send(formData);
    });
  }

  // Logs (/get-logs)
  let lastLogTimestamp = 0;
  let allLogEntries = [];

  async function loadLogs(scroll = true, incremental = false) {
    try {
      // En mode incrémental, envoyer le dernier timestamp au backend
      let url = "/get-logs";
      if (incremental && lastLogTimestamp > 0) {
        url += `?since=${lastLogTimestamp}`;
      }

      const res = await authFetch(url);
      const data = await res.json();

      // Initialiser _bootEpochMs depuis uptime_ms si pas encore connu via WebSocket
      if (_bootEpochMs == null && data.uptime_ms != null) {
        _bootEpochMs = Date.now() - data.uptime_ms;
      }

      const lines = Array.isArray(data) ? data : data.logs || [];

      if (incremental) {
        // En mode incrémental, ajouter les nouveaux logs reçus
        if (lines.length > 0) {
          allLogEntries = [...allLogEntries, ...lines];
          if (allLogEntries.length > 500) allLogEntries = allLogEntries.slice(-500);

          // Mettre à jour le dernier timestamp
          lines.forEach(entry => {
            const ts = typeof entry === 'string' ? 0 : (entry.timestamp || 0);
            if (ts > lastLogTimestamp) lastLogTimestamp = ts;
          });
        }
      } else {
        // En mode complet, remplacer tous les logs
        allLogEntries = lines;

        // Mettre à jour le dernier timestamp
        lastLogTimestamp = 0;
        lines.forEach(entry => {
          const ts = typeof entry === 'string' ? 0 : (entry.timestamp || 0);
          if (ts > lastLogTimestamp) lastLogTimestamp = ts;
        });
      }

      renderLogs(scroll);
    } catch (e) {
      const content = $("#logs_content");
      if (content) content.textContent = "Erreur chargement logs.";
    }
  }

  let _logsAutoRefreshTimer = null;

  function _startLogsAutoRefresh() {
    if (_logsAutoRefreshTimer) return;
    _logsAutoRefreshTimer = setInterval(() => loadLogs(false, true), 5000);
  }

  function _stopLogsAutoRefresh() {
    if (_logsAutoRefreshTimer) { clearInterval(_logsAutoRefreshTimer); _logsAutoRefreshTimer = null; }
  }

  // ========== SONDES 1-WIRE (feature-020) ==========
  // Polling 2 s sur /sensors/onewire/scan UNIQUEMENT quand le panel-dev est actif.
  // Les boutons d'identification déclenchent un POST /sensors/onewire/identify
  // (auto-permutation côté firmware). Le bouton « Réinitialiser » appelle
  // POST /sensors/onewire/reset après confirmation.
  let _sondesPollTimer = null;
  let _sondesIdentifyInFlight = false;
  let _sondesLastScan = null; // dernière réponse JSON pour éviter re-render inutile

  function _isPanelDevActive() {
    const panel = $("#panel-dev");
    return !!(panel && panel.classList.contains("is-active"));
  }

  function _renderSondesOnewire(scan) {
    const list = $("#sondes-list");
    const pill = $("#sondes-status-pill");
    const warn = $("#sondes-warn");
    const resetBtn = $("#sondes-reset-btn");
    const instructions = $("#sondes-instructions");
    if (!list || !pill) return;

    const sondes = Array.isArray(scan?.sondes) ? scan.sondes : [];
    const detected = scan?.detected_count ?? sondes.length;
    const identified = scan?.identified_count ?? sondes.filter(s => s.role === "water" || s.role === "circuit").length;

    // --- Pill statut ---
    pill.classList.remove("ok", "bad", "mid");
    if (detected === 0) {
      pill.textContent = "Aucune sonde détectée";
      pill.classList.add("bad");
    } else if (identified === 0) {
      pill.textContent = `0/${detected} sondes identifiées`;
      pill.classList.add("bad");
    } else if (identified < detected || (detected < 2 && identified < 2)) {
      pill.textContent = `${identified}/${detected} sondes identifiées`;
      pill.classList.add("mid");
    } else {
      pill.textContent = `${identified}/${detected} ✓`;
      pill.classList.add("ok");
    }

    // --- Warning (sonde précédemment identifiée disparue) ---
    // Heuristique : si on a vu auparavant une sonde identifiée puis qu'elle
    // n'est plus dans la liste, c'est un câblage à vérifier. On s'appuie
    // ici uniquement sur la réponse courante : si detected < 2 et
    // identified === 0 alors qu'on avait déjà vu plus, on prévient.
    // Simplification : laisser firmware piloter. Ici on n'affiche le warn
    // que si la spec firmware ajoute un champ explicite à l'avenir ; pour
    // l'instant on masque. (Cas couvert par l'UI : pill « 0/X » + boutons.)
    if (warn) warn.style.display = "none";

    // --- Hint dynamique selon nombre de sondes ---
    if (instructions) {
      if (detected === 1) {
        instructions.textContent =
          "Branchez la 2ᵉ sonde pour terminer la configuration. " +
          "Vous pouvez identifier la sonde déjà branchée en attendant.";
      } else if (detected === 0) {
        instructions.textContent =
          "Aucune sonde détectée. Vérifiez le câblage du bus 1-Wire.";
      } else if (identified >= 2) {
        instructions.textContent =
          "Les deux sondes sont identifiées. Vous pouvez réinitialiser pour recommencer.";
      } else {
        instructions.textContent =
          "Tenez l'une des sondes dans votre main pendant 30 secondes. " +
          "Sa température va monter en temps réel : cliquez sur le bouton correspondant en face de celle qui chauffe.";
      }
    }

    // --- Liste des sondes ---
    if (sondes.length === 0) {
      list.innerHTML = `<div class="muted small" style="padding:12px 0;">Aucune sonde 1-Wire détectée.</div>`;
    } else {
      // Identifier les rôles déjà assignés
      const hasWater = sondes.some(s => s.role === "water");
      const hasCircuit = sondes.some(s => s.role === "circuit");

      list.innerHTML = sondes.map((s, idx) => {
        const addr = String(s.address || "").toUpperCase();
        const tempStr = (typeof s.temperature === "number" && !isNaN(s.temperature))
          ? `${s.temperature.toFixed(1)} °C`
          : "—";
        const role = s.role || "unknown";

        let badgeHtml = "";
        let actionsHtml = "";
        if (role === "water") {
          badgeHtml = `<span class="state-badge state-badge--ok">✓ Eau</span>`;
        } else if (role === "circuit") {
          badgeHtml = `<span class="state-badge state-badge--ok">✓ Circuit</span>`;
        } else {
          // Sonde non identifiée : afficher les boutons pour les rôles encore libres
          const buttons = [];
          if (!hasWater) {
            buttons.push(`<button class="btn btn--secondary" data-role="water" data-address="${addr}">C'est l'eau de la piscine</button>`);
          }
          if (!hasCircuit) {
            buttons.push(`<button class="btn btn--secondary" data-role="circuit" data-address="${addr}">C'est le circuit interne</button>`);
          }
          // Si les deux rôles sont déjà pris (ne devrait pas arriver mais safety net),
          // proposer les deux quand même (auto-permutation côté firmware).
          if (buttons.length === 0) {
            buttons.push(`<button class="btn btn--secondary" data-role="water" data-address="${addr}">C'est l'eau de la piscine</button>`);
            buttons.push(`<button class="btn btn--secondary" data-role="circuit" data-address="${addr}">C'est le circuit interne</button>`);
          }
          actionsHtml = `<div class="detail-actions">${buttons.join("")}</div>`;
        }

        return `
          <div class="detail-row sonde-row" data-address="${addr}">
            <div>
              <div class="row__title">
                Sonde ${idx + 1}
                ${badgeHtml}
              </div>
              <div class="muted small">
                <code>${addr}</code> — <span class="sonde-temp">${tempStr}</span>
              </div>
            </div>
            ${actionsHtml}
          </div>
        `;
      }).join("");

      // Brancher les handlers d'identification
      list.querySelectorAll("button[data-role]").forEach(btn => {
        btn.addEventListener("click", () => _onSondeIdentifyClick(btn));
      });
    }

    // --- Bouton « Réinitialiser » : visible uniquement si au moins une sonde identifiée ---
    if (resetBtn) {
      resetBtn.hidden = (identified === 0);
    }
  }

  async function _onSondeIdentifyClick(btn) {
    if (_sondesIdentifyInFlight) return;
    const address = btn.getAttribute("data-address");
    const role = btn.getAttribute("data-role");
    if (!address || !role) return;

    // Optimistic UI : disable tous les boutons pendant la requête
    const allBtns = $("#sondes-list")?.querySelectorAll("button[data-role]") || [];
    allBtns.forEach(b => { b.disabled = true; });
    _sondesIdentifyInFlight = true;

    try {
      const resp = await authFetch("/sensors/onewire/identify", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ address, role })
      });
      if (!resp.ok) throw new Error(`HTTP ${resp.status}`);
      const json = await resp.json().catch(() => ({}));
      if (json.success === false) throw new Error("identify failed");
      const roleLabel = role === "water" ? "Eau" : "Circuit";
      showToast(`Sonde identifiée : ${roleLabel}`, "success");
      // Rafraîchir immédiatement pour afficher la mise à jour
      await _refreshSondesOnewire().catch(() => {});
    } catch (e) {
      console.error("[sondes] identify failed:", e);
      showToast("Échec de l'identification", "error");
      allBtns.forEach(b => { b.disabled = false; });
    } finally {
      _sondesIdentifyInFlight = false;
    }
  }

  async function _refreshSondesOnewire() {
    try {
      const resp = await authFetch("/sensors/onewire/scan");
      if (!resp.ok) return;
      const scan = await resp.json();
      _sondesLastScan = scan;
      _renderSondesOnewire(scan);
    } catch (e) {
      // Réseau temporairement indisponible : on garde l'affichage précédent
      debugLog("[sondes] scan failed: " + e.message);
    }
  }

  function _startSondesPoll() {
    if (_sondesPollTimer) return;
    _refreshSondesOnewire().catch(() => {});
    _sondesPollTimer = setInterval(() => {
      if (!_isPanelDevActive()) {
        _stopSondesPoll();
        return;
      }
      _refreshSondesOnewire().catch(() => {});
    }, 2000);
  }

  function _stopSondesPoll() {
    if (_sondesPollTimer) { clearInterval(_sondesPollTimer); _sondesPollTimer = null; }
  }

  // Met à jour la chip Dashboard à partir des champs WS sondes_*
  function _updateSondesChip() {
    const chip = $("#sondes-chip");
    if (!chip) return;
    const identified = latestSensorData?.sondes_identified;
    const detected = latestSensorData?.sondes_detected;
    if (identified === false && typeof detected === "number" && detected >= 1) {
      chip.hidden = false;
    } else {
      chip.hidden = true;
    }
  }

  function bindSondesOnewire() {
    // Clic sur la chip Dashboard : navigation + scroll vers la card
    const chip = $("#sondes-chip");
    if (chip) {
      chip.addEventListener("click", () => {
        window.location.hash = "#/settings/dev";
        // Attendre que le panel soit affiché puis scroller
        setTimeout(() => {
          const card = $("#card-sensors-onewire");
          if (card) card.scrollIntoView({ behavior: "smooth", block: "start" });
        }, 120);
      });
    }

    // Bouton « Réinitialiser »
    const resetBtn = $("#sondes-reset-btn");
    if (resetBtn) {
      resetBtn.addEventListener("click", async () => {
        if (!confirm("Réinitialiser l'identification des deux sondes ?\n\nVous devrez les ré-identifier ensuite.")) return;
        try {
          const resp = await authFetch("/sensors/onewire/reset", {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            body: "{}"
          });
          if (!resp.ok) throw new Error(`HTTP ${resp.status}`);
          showToast("Identification réinitialisée", "success");
          await _refreshSondesOnewire().catch(() => {});
        } catch (e) {
          console.error("[sondes] reset failed:", e);
          showToast("Échec de la réinitialisation", "error");
        }
      });
    }

    // Démarrage / arrêt du polling sur changement de route
    const onRouteChange = () => {
      if (_isPanelDevActive()) {
        _startSondesPoll();
      } else {
        _stopSondesPoll();
      }
    };
    window.addEventListener("hashchange", onRouteChange);
    // État initial
    onRouteChange();
  }

  function bindLogs() {
    $("#refresh_logs_btn")?.addEventListener("click", () => loadLogs(true, false));

    $("#clear_logs_display_btn")?.addEventListener("click", () => {
      const content = $("#logs_content");
      if (content) content.textContent = "";
      allLogEntries = [];
      lastLogTimestamp = 0;
    });

    $("#clear_logs_firmware_btn")?.addEventListener("click", async () => {
      if (!confirm("Effacer tous les logs côté ESP32 ?\n\nLa mémoire RAM ET le fichier persistant seront vidés.\nCette action est irréversible.")) return;
      try {
        const resp = await authFetch("/logs", { method: "DELETE" });
        if (!resp.ok) { showToast("Erreur lors de l'effacement", "error"); return; }
        const content = $("#logs_content");
        if (content) content.textContent = "";
        allLogEntries = [];
        lastLogTimestamp = 0;
        showToast("Logs effacés (RAM + fichier)", "success");
      } catch (e) {
        showToast("Erreur lors de l'effacement", "error");
      }
    });

    $("#download_logs_btn")?.addEventListener("click", async () => {
      const resp = await authFetch("/download-logs");
      if (!resp.ok) { showToast("Erreur lors du téléchargement des logs", "error"); return; }
      const blob = await resp.blob();
      const url = URL.createObjectURL(blob);
      const a = document.createElement("a");
      a.href = url;
      a.download = "pool_logs.txt";
      document.body.appendChild(a);
      a.click();
      document.body.removeChild(a);
      URL.revokeObjectURL(url);
    });

    ["log_level_info", "log_level_warn", "log_level_error", "log_level_critical", "log_level_debug"].forEach(id => {
      $(`#${id}`)?.addEventListener("change", () => renderLogs(false));
    });

    $("#logs_auto_refresh")?.addEventListener("change", (e) => {
      if (e.target.checked) _startLogsAutoRefresh();
      else _stopLogsAutoRefresh();
    });
  }


  // WiFi Configuration
  function bindWifi() {
    $("#wifi_config_btn")?.addEventListener("click", () => {
      // Rediriger vers la page de configuration WiFi
      window.location.href = "/wifi.html";
    });
  }

  // Désactive un bouton Sauvegarder par défaut ; l'active dès qu'un champ est modifié.
  function trackDirtyState(btnSelector, fieldIds) {
    const btn = $(btnSelector);
    if (!btn) return;
    btn.disabled = true;
    const enable = () => { btn.disabled = false; };
    fieldIds.forEach((id) => {
      const el = $(`#${id}`);
      if (!el) return;
      el.addEventListener("change", enable);
      el.addEventListener("input", enable);
    });
  }

  function bindMqttManualSave() {
    const saveBtn = $("#mqtt_save_btn");
    const mqttEnabled = $("#mqtt_enabled");
    const defaultLabel = "Sauvegarder";
    const savingLabel = "Sauvegarde...";
    const savedLabel = "Sauvegarde réussie";

    const setButtonState = (label, showSpinner, disabled, status) => {
      saveBtn.disabled = disabled;
      saveBtn.classList.remove("btn--primary", "btn--ok", "btn--danger");
      if (status === "success") {
        saveBtn.classList.add("btn--ok");
      } else if (status === "error") {
        saveBtn.classList.add("btn--danger");
      } else {
        saveBtn.classList.add("btn--primary");
      }
      if (showSpinner) {
        saveBtn.innerHTML = `<span class="btn__spinner"></span><span>${label}</span>`;
      } else {
        saveBtn.textContent = label;
      }
    };

    trackDirtyState("#mqtt_save_btn", ["mqtt_enabled", "mqtt_server", "mqtt_port", "mqtt_topic", "mqtt_username", "mqtt_password"]);

    mqttEnabled?.addEventListener("change", () => {
      updateMqttStatusIndicator(mqttEnabled.checked, false);
    });

    saveBtn?.addEventListener("click", async () => {
      if (saveBtn.disabled) return;
      setButtonState(savingLabel, true, true, "default");

      const ok = await sendConfig(collectMqttConfig());
      if (ok) {
        updateMqttStatusIndicator($("#mqtt_enabled").checked, false);
        setTimeout(loadConfig, 6000);
        setButtonState(savedLabel, false, true, "success");
        setTimeout(() => setButtonState(defaultLabel, false, true, "default"), 2000);
      } else {
        alert("Erreur lors de la sauvegarde MQTT.");
        setButtonState("Erreur", false, true, "error");
        setTimeout(() => setButtonState(defaultLabel, false, false, "default"), 2000);
      }
    });
  }

  function bindLightingManualSave() {
    trackDirtyState("#lighting_save_btn", ["lighting_schedule_mode", "lighting_start_time", "lighting_end_time"]);
    const saveBtn = $("#lighting_save_btn");
    if (!saveBtn) return;
    const defaultLabel = "Sauvegarder";
    const setBtn = (label, spinner, disabled, status) => {
      saveBtn.disabled = disabled;
      saveBtn.classList.remove("btn--primary", "btn--ok", "btn--danger");
      if (status === "success") saveBtn.classList.add("btn--ok");
      else if (status === "error") saveBtn.classList.add("btn--danger");
      else saveBtn.classList.add("btn--primary");
      saveBtn.innerHTML = spinner ? `<span class="btn__spinner"></span><span>${label}</span>` : label;
    };
    saveBtn.addEventListener("click", async () => {
      if (saveBtn.disabled) return;
      setBtn("Sauvegarde...", true, true, "default");
      const cfg = {
        lighting_schedule_enabled: $("#lighting_schedule_mode")?.value === "enabled",
        lighting_start_time: $("#lighting_start_time")?.value,
        lighting_end_time: $("#lighting_end_time")?.value,
      };
      const ok = await sendConfig(cfg);
      if (ok) {
        loadConfig();
        setBtn("Sauvegarde réussie", false, true, "success");
        setTimeout(() => setBtn(defaultLabel, false, true, "default"), 2000);
      } else {
        setBtn("Erreur", false, true, "error");
        setTimeout(() => setBtn(defaultLabel, false, false, "default"), 2000);
      }
    });
  }

  function bindFiltrationManualSave() {
    trackDirtyState("#filtration_save_btn", ["filtration_mode", "filtration_start", "filtration_end"]);
    const saveBtn = $("#filtration_save_btn");
    const defaultLabel = "Sauvegarder";
    const savingLabel = "Sauvegarde...";
    const savedLabel = "Sauvegarde réussie";

    const setFiltBtnState = (label, showSpinner, disabled, status) => {
      if (!saveBtn) return;
      saveBtn.disabled = disabled;
      saveBtn.classList.remove("btn--primary", "btn--ok", "btn--danger");
      if (status === "success") saveBtn.classList.add("btn--ok");
      else if (status === "error") saveBtn.classList.add("btn--danger");
      else saveBtn.classList.add("btn--primary");
      if (showSpinner) {
        saveBtn.innerHTML = `<span class="btn__spinner"></span><span>${label}</span>`;
      } else {
        saveBtn.textContent = label;
      }
    };

    saveBtn?.addEventListener("click", async () => {
      if (saveBtn.disabled) return;
      setFiltBtnState(savingLabel, true, true, "default");
      const cfg = collectFiltrationConfig();
      const ok = await sendConfig(cfg);
      if (ok) {
        filtrationDirty = false;
        filtrationRunningOverride = predictFiltrationRunning(cfg);
        // Mise à jour optimiste de window._config pour que updateDetailSections
        // reflète immédiatement le nouveau mode (ex: grise les boutons si "off")
        if (window._config) {
          window._config.filtration_mode = cfg.filtration_mode;
          window._config.filtration_start = cfg.filtration_start;
          window._config.filtration_end = cfg.filtration_end;
        }
        updateDetailSections();
        updateStatusCards();
        setFiltBtnState(savedLabel, false, true, "success");
        setTimeout(() => setFiltBtnState(defaultLabel, false, true, "default"), 2000);
        setTimeout(loadConfig, 1500);
        setTimeout(() => loadSensorData({ force: true, source: "filtration-save" }), 500);
      } else {
        setFiltBtnState("Erreur", false, true, "error");
        setTimeout(() => setFiltBtnState(defaultLabel, false, false, "default"), 2000);
      }
    });
  }

  function bindPumpConfigSave() {
    trackDirtyState("#pump_config_save_btn", ["ph_pump", "orp_pump", "pump1_max_duty", "pump2_max_duty"]);
    const saveBtn = $("#pump_config_save_btn");
    if (!saveBtn) return;
    const defaultLabel = "Sauvegarder";
    const savingLabel = "Sauvegarde...";
    const savedLabel = "Sauvegarde réussie";

    const setButtonState = (label, showSpinner, disabled, status) => {
      saveBtn.disabled = disabled;
      saveBtn.classList.remove("btn--primary", "btn--ok", "btn--danger");
      if (status === "success") saveBtn.classList.add("btn--ok");
      else if (status === "error") saveBtn.classList.add("btn--danger");
      else saveBtn.classList.add("btn--primary");
      if (showSpinner) {
        saveBtn.innerHTML = `<span class="btn__spinner"></span><span>${label}</span>`;
      } else {
        saveBtn.textContent = label;
      }
    };

    // Mise à jour de l'affichage des sliders en temps réel (sans sauvegarder)
    ["pump1_max_duty", "pump2_max_duty"].forEach((id) => {
      const slider = $(`#${id}`);
      const display = $(`#${id}_value`);
      if (slider && display) slider.addEventListener("input", () => { display.textContent = slider.value; });
    });

    // Basculement automatique de la pompe ORP quand la pompe pH change (et vice-versa)
    const phPumpSel  = $("#ph_pump");
    const orpPumpSel = $("#orp_pump");
    if (phPumpSel && orpPumpSel) {
      phPumpSel.addEventListener("change", () => {
        orpPumpSel.value = phPumpSel.value === "1" ? "2" : "1";
      });
      orpPumpSel.addEventListener("change", () => {
        phPumpSel.value = orpPumpSel.value === "1" ? "2" : "1";
      });
    }

    saveBtn.addEventListener("click", async () => {
      if (saveBtn.disabled) return;
      setButtonState(savingLabel, true, true, "default");
      const cfg = {
        ph_pump:            parseInt($("#ph_pump")?.value || "1", 10),
        orp_pump:           parseInt($("#orp_pump")?.value || "2", 10),
        pump1_max_duty_pct:       parseInt($("#pump1_max_duty")?.value || "100", 10),
        pump2_max_duty_pct:       parseInt($("#pump2_max_duty")?.value || "100", 10),
        pump_max_flow_ml_per_min: parseFloat($("#pump_max_flow_ml_per_min")?.value || "90"),
      };
      const ok = await sendConfig(cfg);
      if (ok) {
        setButtonState(savedLabel, false, true, "success");
        setTimeout(() => setButtonState(defaultLabel, false, true, "default"), 2000);
      } else {
        setButtonState("Erreur", false, true, "error");
        setTimeout(() => setButtonState(defaultLabel, false, false, "default"), 2000);
      }
    });

    // Boutons test pompes
    const PUMP_TEST_DURATION = 10;
    let pumpTestTimer = null;
    let pumpCountdownInterval = null;

    const resetPumpButtons = () => {
      const b1 = $("#pump1_test_btn"), b2 = $("#pump2_test_btn");
      if (b1) { b1.textContent = "▶ Pompe 1"; b1.disabled = false; }
      if (b2) { b2.textContent = "▶ Pompe 2"; b2.disabled = false; }
    };

    const stopAllPumps = async () => {
      clearTimeout(pumpTestTimer);
      clearInterval(pumpCountdownInterval);
      pumpTestTimer = null;
      pumpCountdownInterval = null;
      resetPumpButtons();
      await authFetch("/pump1/off", { method: "POST" }).catch(() => {});
      await authFetch("/pump2/off", { method: "POST" }).catch(() => {});
    };

    const startPumpTest = async (onRoute, btnId, otherBtnId, label) => {
      clearTimeout(pumpTestTimer);
      clearInterval(pumpCountdownInterval);

      const btn = document.querySelector(btnId);
      const otherBtn = document.querySelector(otherBtnId);

      // Feedback immédiat avant la requête
      let remaining = PUMP_TEST_DURATION;
      if (btn) { btn.textContent = `⏳ ${label} — ${remaining}s`; btn.disabled = true; }
      if (otherBtn) otherBtn.disabled = true;

      // Décompte
      pumpCountdownInterval = setInterval(() => {
        remaining--;
        if (remaining > 0) {
          if (btn) btn.textContent = `⏳ ${label} — ${remaining}s`;
        }
      }, 1000);

      // Arrêt auto
      pumpTestTimer = setTimeout(async () => {
        clearInterval(pumpCountdownInterval);
        resetPumpButtons();
        await authFetch(onRoute.replace("/on", "/off"), { method: "POST" }).catch(() => {});
      }, PUMP_TEST_DURATION * 1000);

      // Requête démarrage (après le feedback visuel)
      await authFetch(onRoute, { method: "POST" }).catch(() => {});
    };

    $("#pump1_test_btn")?.addEventListener("click", () => startPumpTest("/pump1/on", "#pump1_test_btn", "#pump2_test_btn", "Pompe 1"));
    $("#pump2_test_btn")?.addEventListener("click", () => startPumpTest("/pump2/on", "#pump2_test_btn", "#pump1_test_btn", "Pompe 2"));
    $("#pumps_stop_btn")?.addEventListener("click", stopAllPumps);

    // Boutons injection manuelle pH / ORP
    // L'état est géré côté ESP32 (durée, auto-stop) — le JS lit ph_inject_remaining_s / orp_inject_remaining_s
    const injectInterval = { ph: null, orp: null };

    const fmtInjectRemaining = (s) => {
      if (s >= 60) {
        const m = Math.floor(s / 60), sec = s % 60;
        return sec > 0 ? `${m}min ${sec}s` : `${m}min`;
      }
      return `${s}s`;
    };

    // Calcule la durée d'injection (secondes) pour un volume donné (mL)
    // Utilise les paramètres de pompe depuis window._config
    const calcInjectDurationS = (product, volumeMl) => {
      const cfg = window._config;
      if (!cfg) return null;
      const pumpNum = product === "ph" ? cfg.ph_pump : cfg.orp_pump; // 1 ou 2
      const dutyPct = pumpNum === 1 ? (cfg.pump1_max_duty_pct ?? 50) : (cfg.pump2_max_duty_pct ?? 50);
      const maxFlow = cfg.pump_max_flow_ml_per_min ?? 90;
      const MIN_FLOW = 5.2, MAX_PWM = 255, MIN_DUTY = 80;
      let duty = Math.floor((dutyPct * MAX_PWM) / 100);
      if (duty < MIN_DUTY) duty = MIN_DUTY;
      const normalized = (duty - MIN_DUTY) / (MAX_PWM - MIN_DUTY);
      const flow = MIN_FLOW + normalized * (maxFlow - MIN_FLOW); // mL/min
      return Math.max(1, Math.round((volumeMl / flow) * 60));
    };

    // Met à jour le hint "≈ Xmin" sous le champ volume
    const updateInjectHint = (product) => {
      const mlInput = $(`#${product}_inject_ml`);
      const hint = $(`#${product}_inject_hint`);
      if (!mlInput || !hint) return;
      const vol = parseFloat(mlInput.value);
      if (!vol || vol <= 0) { hint.textContent = ""; return; }
      const dur = calcInjectDurationS(product, vol);
      hint.textContent = dur != null ? `≈ ${fmtInjectRemaining(dur)}` : "";
    };

    // Appelé à chaque push sensor_data pour mettre à jour les boutons
    window._updateInjectButtons = (data) => {
      const phInjecting = (data["ph_inject_remaining_s"] ?? 0) > 0;
      const orpInjecting = (data["orp_inject_remaining_s"] ?? 0) > 0 || (data["orp_dosing"] === true);
      const phCalBtn = $("#ph_cal_trigger_btn");
      const orpCalBtn = $("#orp_cal_trigger_btn");

      ["ph", "orp"].forEach(product => {
        const remaining = data[`${product}_inject_remaining_s`] ?? 0;
        const btn = $(`#${product}_inject_btn`);
        const mlInput = $(`#${product}_inject_ml`);
        if (!btn) return;

        if (remaining > 0) {
          btn.textContent = `⏹ Arrêter — ${fmtInjectRemaining(remaining)}`;
          if (mlInput) mlInput.disabled = true;
          // Décompte local entre deux pushes WebSocket (~5s)
          if (!injectInterval[product]) {
            injectInterval[product] = setInterval(() => {
              const cur = data[`${product}_inject_remaining_s`] ?? 0;
              if (cur <= 0) {
                clearInterval(injectInterval[product]);
                injectInterval[product] = null;
                btn.textContent = "▶ Injecter";
                if (mlInput) mlInput.disabled = false;
                // Réactiver le bouton Calibrer si plus aucune injection
                if (product === "ph" && !phCalibrationActive && phCalBtn) {
                  phCalBtn.disabled = false;
                }
                if (product === "orp" && !orpCalibrationActive && orpCalBtn) {
                  orpCalBtn.disabled = false;
                }
                return;
              }
              data[`${product}_inject_remaining_s`]--;
              btn.textContent = `⏹ Arrêter — ${fmtInjectRemaining(data[`${product}_inject_remaining_s`])}`;
            }, 1000);
          }
        } else {
          if (injectInterval[product]) {
            clearInterval(injectInterval[product]);
            injectInterval[product] = null;
          }
          btn.textContent = "▶ Injecter";
          if (mlInput) mlInput.disabled = false;
        }
      });

      // Désactiver le bouton Calibrer pendant l'injection pH
      if (phCalBtn && !phCalibrationActive) {
        phCalBtn.disabled = phInjecting;
      }
      // Désactiver le bouton Calibrer ORP pendant l'injection ORP
      if (orpCalBtn && !orpCalibrationActive) {
        orpCalBtn.disabled = orpInjecting;
      }
    };

    const stopInject = async (product) => {
      await authFetch(`/${product}/inject/stop`, { method: "POST" }).catch(() => {});
    };

    const startInject = async (product) => {
      const remaining = latestSensorData?.[`${product}_inject_remaining_s`] ?? 0;
      // Si déjà en cours → arrêt
      if (remaining > 0) {
        await stopInject(product);
        return;
      }
      const btn = $(`#${product}_inject_btn`);
      const mlInput = $(`#${product}_inject_ml`);
      const volumeMl = Math.max(1, Math.min(2000, parseFloat(mlInput?.value) || 50));
      // Feedback immédiat : afficher la durée estimée pendant que l'ESP32 traite
      const estDur = calcInjectDurationS(product, volumeMl);
      if (btn) btn.textContent = estDur ? `⏳ Démarrage — ≈ ${fmtInjectRemaining(estDur)}` : "⏳ Démarrage…";
      if (mlInput) mlInput.disabled = true;
      const resp = await authFetch(`/${product}/inject/start?volume=${volumeMl}`, { method: "POST" }).catch(() => null);
      if (!resp || !resp.ok) {
        // Erreur : restaurer le bouton et afficher un message clair
        if (btn) btn.textContent = "▶ Injecter";
        if (mlInput) mlInput.disabled = false;

        // Récupérer le message d'erreur du backend (text/plain) pour le toast
        let errorMsg = "Erreur lors du démarrage de l'injection";
        if (resp) {
          if (resp.status === 409) {
            // Sécurité chimique : filtration arrêtée
            errorMsg = "Injection refusée : la filtration doit être active avant d'injecter (sécurité chimique : pas de circulation = surdosage local).";
          } else {
            const body = await resp.text().catch(() => "");
            if (body && body.length < 200) errorMsg = body;
          }
        }
        showToast(errorMsg, "error");
      }
      // Le bouton sera mis à jour précisément par le prochain push WebSocket
    };

    // Hints dynamiques au changement de volume
    ["ph", "orp"].forEach(product => {
      $(`#${product}_inject_ml`)?.addEventListener("input", () => updateInjectHint(product));
      updateInjectHint(product);
    });

    // Initialiser l'état des boutons depuis les données courantes (si déjà connecté)
    if (latestSensorData) window._updateInjectButtons(latestSensorData);

    $("#ph_inject_btn")?.addEventListener("click", () => startInject("ph"));
    $("#orp_inject_btn")?.addEventListener("click", () => startInject("orp"));
  }

  function bindTimeManualSave() {
    trackDirtyState("#time_save_btn", ["time_use_ntp", "time_timezone", "time_ntp_server", "time_value"]);
    const saveBtn = $("#time_save_btn");
    const timeUseNtp = $("#time_use_ntp");
    const defaultLabel = "Sauvegarder";
    const savingLabel = "Sauvegarde...";
    const savedLabel = "Sauvegarde réussie";

    const setButtonState = (label, showSpinner, disabled, status) => {
      saveBtn.disabled = disabled;
      saveBtn.classList.remove("btn--primary", "btn--ok", "btn--danger");
      if (status === "success") {
        saveBtn.classList.add("btn--ok");
      } else if (status === "error") {
        saveBtn.classList.add("btn--danger");
      } else {
        saveBtn.classList.add("btn--primary");
      }
      if (showSpinner) {
        saveBtn.innerHTML = `<span class="btn__spinner"></span><span>${label}</span>`;
      } else {
        saveBtn.textContent = label;
      }
    };

    // Mise à jour de l'UI uniquement lors du changement du switch NTP (pas de sauvegarde auto)
    timeUseNtp?.addEventListener("change", () => {
      updateTimeControls($("#time_value").value, false);
    });

    // Quand la timezone change en mode NTP : recalculer l'heure affichée sans appel serveur
    const tzMap = {
      europe_paris: "Europe/Paris",
      utc: "UTC",
      america_new_york: "America/New_York",
      america_los_angeles: "America/Los_Angeles",
      asia_tokyo: "Asia/Tokyo",
      australia_sydney: "Australia/Sydney"
    };
    $("#time_timezone")?.addEventListener("change", () => {
      if (!(timeUseNtp?.checked ?? true)) return;
      const ianaTimezone = tzMap[$("#time_timezone").value] || "UTC";
      const parts = new Intl.DateTimeFormat("en-CA", {
        timeZone: ianaTimezone,
        year: "numeric", month: "2-digit", day: "2-digit",
        hour: "2-digit", minute: "2-digit", hour12: false
      }).formatToParts(new Date());
      const g = (t) => parts.find(p => p.type === t)?.value || "00";
      updateTimeControls(`${g("year")}-${g("month")}-${g("day")}T${g("hour")}:${g("minute")}`, false);
    });

    saveBtn?.addEventListener("click", async () => {
      if (saveBtn.disabled) return;
      setButtonState(savingLabel, true, true, "default");

      const ok = await sendConfig(collectTimeConfig());

      if (ok) {
        setButtonState(savedLabel, false, true, "success");
        setTimeout(() => setButtonState(defaultLabel, false, true, "default"), 2000);
        setTimeout(loadConfig, 1500);
      } else {
        setButtonState("Erreur", false, true, "error");
        setTimeout(() => setButtonState(defaultLabel, false, false, "default"), 2000);
      }
    });

    // En mode manuel : afficher les secondes au focus, les masquer au blur
    const timeValueInput = $("#time_value");
    timeValueInput?.addEventListener("focus", () => {
      const useNtp = $("#time_use_ntp")?.checked ?? true;
      if (!useNtp && fullTimeValue) {
        // Mode manuel : afficher l'heure complète avec secondes
        timeValueInput.value = fullTimeValue;
      }
    });

    timeValueInput?.addEventListener("blur", () => {
      const useNtp = $("#time_use_ntp")?.checked ?? true;
      if (!useNtp) {
        // Mode manuel : stocker la nouvelle valeur et masquer les secondes
        fullTimeValue = timeValueInput.value;
        if (timeValueInput.value.length > 16) {
          timeValueInput.value = timeValueInput.value.substring(0, 16);
        }
      }
    });
  }

  // ---------- Auto-save bindings ----------
  function bindRegulationSave(sensor, btnSelector) {
    const fields = sensor === "ph"
      ? ["ph_target", "ph_correction_type"]
      : ["orp_target"];
    trackDirtyState(btnSelector, fields);
    const saveBtn = $(btnSelector);
    if (!saveBtn) return;
    const defaultLabel = "Sauvegarder";
    const setBtn = (label, spinner, disabled, status) => {
      saveBtn.disabled = disabled;
      saveBtn.classList.remove("btn--primary", "btn--ok", "btn--danger");
      saveBtn.classList.add(status === "success" ? "btn--ok" : status === "error" ? "btn--danger" : "btn--primary");
      saveBtn.innerHTML = spinner ? `<span class="btn__spinner"></span><span>${label}</span>` : label;
    };
    saveBtn.addEventListener("click", async () => {
      if (saveBtn.disabled) return;
      setBtn("Sauvegarde...", true, true, "default");
      const ok = await sendConfig(collectConfig());
      if (ok) {
        setBtn("Sauvegarde réussie", false, true, "success");
        setTimeout(() => setBtn(defaultLabel, false, true, "default"), 2000);
      } else {
        setBtn("Erreur", false, true, "error");
        setTimeout(() => setBtn(defaultLabel, false, false, "default"), 2000);
      }
    });
  }

  function bindAutosave() {
    const save = () => {
      const cfg = collectConfig();
      [
        "enabled", "server", "port", "topic", "username", "password",
        "time_use_ntp", "ntp_server", "timezone_id", "manual_time"
      ].forEach((key) => {
        delete cfg[key];
      });
      return sendConfig(cfg);
    };

    // Filtration feature toggle — sauvegarde immédiate (interrupteur on/off)
    $("#filtration_enabled")?.addEventListener("change", () => {
      updateFeatureVisibility("filtration");
      const cfg = collectFiltrationConfig();
      sendConfig(cfg).then((ok) => {
        if (ok) {
          filtrationRunningOverride = predictFiltrationRunning(cfg);
          updateDetailSections();
          updateStatusCards();
          loadConfig();
          setTimeout(() => loadSensorData({ force: true, source: "filtration-save" }), 500);
        }
      });
    });
    // Programmation (mode, start, end) — pas d'auto-save, bouton Sauvegarder uniquement
    $("#filtration_mode")?.addEventListener("change", () => {
      filtrationDirty = true;
      updateFiltrationControls();
    });
    $("#filtration_start")?.addEventListener("change", () => {
      cachedManualStart = $("#filtration_start").value;
      filtrationDirty = true;
    });
    $("#filtration_end")?.addEventListener("change", () => {
      cachedManualEnd = $("#filtration_end").value;
      filtrationDirty = true;
    });

    // Lighting feature — sauvegarde immédiate (interrupteur on/off)
    $("#lighting_feature_enabled")?.addEventListener("change", () => {
      updateFeatureVisibility("lighting");
      save().then((ok) => { if (ok !== false) loadConfig(); });
    });

    // Lighting schedule — sauvegarde manuelle via bouton
    $("#lighting_schedule_mode")?.addEventListener("change", () => {
      const scheduleSettings = $("#lighting-schedule-settings");
      if (scheduleSettings) {
        scheduleSettings.style.display = $("#lighting_schedule_mode").value === "enabled" ? "block" : "none";
      }
    });
    bindLightingManualSave();

    // pH / ORP regulation — sauvegarde immédiate sur le sélecteur de mode
    initSegmented("ph_regulation_mode");
    $$(`#ph_regulation_mode .segmented__btn`).forEach(btn => {
      btn.addEventListener("click", () => {
        updatePhModeControls();
        const mode = btn.dataset.value;
        if (window._config) { window._config.ph_regulation_mode = mode; window._config.ph_enabled = (mode !== "manual"); }
        sendConfig({ ph_regulation_mode: mode }).then(ok => { if (ok) loadConfig(); });
      });
    });
    const schedBtn = $("#ph_scheduled_save_btn");
    if (schedBtn) {
      const setSchedBtn = (label, spinner, disabled, status) => {
        schedBtn.disabled = disabled;
        schedBtn.classList.remove("btn--primary", "btn--ok", "btn--danger");
        schedBtn.classList.add(status === "success" ? "btn--ok" : status === "error" ? "btn--danger" : "btn--primary");
        schedBtn.innerHTML = spinner ? `<span class="btn__spinner"></span><span>${label}</span>` : label;
      };
      schedBtn.addEventListener("click", async () => {
        if (schedBtn.disabled) return;
        const rawVal = parseInt($("#ph_daily_target_ml")?.value ?? "0", 10);
        const maxMl = typeof window._config?.max_ph_ml_per_day === "number" ? window._config.max_ph_ml_per_day : 0;
        if (maxMl > 0 && rawVal > maxMl) { showToast(`Volume supérieur à la limite journalière (${maxMl} mL)`, "error"); return; }
        setSchedBtn("Sauvegarde...", true, true, "default");
        const ok = await sendConfig({ ph_daily_target_ml: isNaN(rawVal) ? 0 : Math.max(0, rawVal) });
        if (ok) { setSchedBtn("Sauvegarde réussie", false, true, "success"); setTimeout(() => setSchedBtn("Sauvegarder", false, false, "default"), 2000); }
        else { setSchedBtn("Erreur", false, false, "error"); setTimeout(() => setSchedBtn("Sauvegarder", false, false, "default"), 2000); }
      });
    }
    // ORP regulation — sauvegarde immédiate sur le sélecteur de mode
    initSegmented("orp_regulation_mode");
    $$(`#orp_regulation_mode .segmented__btn`).forEach(btn => {
      btn.addEventListener("click", () => {
        updateOrpModeControls();
        const mode = btn.dataset.value;
        if (window._config) { window._config.orp_regulation_mode = mode; window._config.orp_enabled = (mode !== "manual"); }
        sendConfig({ orp_regulation_mode: mode }).then(ok => { if (ok) loadConfig(); });
      });
    });
    const orpSchedBtn = $("#orp_scheduled_save_btn");
    if (orpSchedBtn) {
      const setOrpSchedBtn = (label, spinner, disabled, status) => {
        orpSchedBtn.disabled = disabled;
        orpSchedBtn.classList.remove("btn--primary", "btn--ok", "btn--danger");
        orpSchedBtn.classList.add(status === "success" ? "btn--ok" : status === "error" ? "btn--danger" : "btn--primary");
        orpSchedBtn.innerHTML = spinner ? `<span class="btn__spinner"></span><span>${label}</span>` : label;
      };
      orpSchedBtn.addEventListener("click", async () => {
        if (orpSchedBtn.disabled) return;
        const rawVal = parseInt($("#orp_daily_target_ml")?.value ?? "0", 10);
        const maxOrpMl = typeof window._config?.max_orp_ml_per_day === "number" ? window._config.max_orp_ml_per_day
                       : (typeof window._config?.max_chlorine_ml_per_day === "number" ? window._config.max_chlorine_ml_per_day : 0);
        if (maxOrpMl > 0 && rawVal > maxOrpMl) { showToast(`Volume supérieur à la limite journalière (${maxOrpMl} mL)`, "error"); return; }
        setOrpSchedBtn("Sauvegarde...", true, true, "default");
        const ok = await sendConfig({ orp_daily_target_ml: isNaN(rawVal) ? 0 : Math.max(0, rawVal) });
        if (ok) { setOrpSchedBtn("Sauvegarde réussie", false, true, "success"); setTimeout(() => setOrpSchedBtn("Sauvegarder", false, false, "default"), 2000); }
        else { setOrpSchedBtn("Erreur", false, false, "error"); setTimeout(() => setOrpSchedBtn("Sauvegarder", false, false, "default"), 2000); }
      });
    }
    ["ph_target", "ph_limit", "ph_daily_limit", "ph_correction_type"].forEach((id) => $(`#${id}`)?.addEventListener("change", () => updatePhControls()));
    ["orp_target", "orp_limit", "orp_daily_limit"].forEach((id) => $(`#${id}`)?.addEventListener("change", () => updateOrpControls()));
    bindRegulationSave("ph",  "#ph_regulation_save_btn");
    bindRegulationSave("orp", "#orp_regulation_save_btn");
    // Onglet Régulation : initialisation segmented + save dédié
    initSegmented("regulation_speed");
    $("#regulation_save_btn")?.addEventListener("click", async () => {
      const mode = $("#regulation_mode")?.value || "pilote";
      const delay = parseInt($("#stabilization_delay_min")?.value ?? "5", 10);
      const speed = getSegmented("regulation_speed") || "normal";
      const phLimit = parseInt($("#ph_limit")?.value ?? "5", 10);
      const phDaily = parseFloat($("#ph_daily_limit")?.value ?? "300");
      const orpLimit = parseInt($("#orp_limit")?.value ?? "10", 10);
      const orpDaily = parseFloat($("#orp_daily_limit")?.value ?? "500");
      const ok = await sendConfig({
        regulation_mode: mode,
        stabilization_delay_min: isNaN(delay) ? 5 : Math.min(60, Math.max(0, delay)),
        regulation_speed: speed,
        ph_limit_minutes:        isNaN(phLimit)  ? 5  : Math.min(60, Math.max(1, phLimit)),
        max_ph_ml_per_day:       isNaN(phDaily)  ? 300 : Math.max(0, phDaily),
        orp_limit_minutes:       isNaN(orpLimit) ? 10 : Math.min(60, Math.max(1, orpLimit)),
        max_chlorine_ml_per_day: isNaN(orpDaily) ? 500 : Math.max(0, orpDaily),
      });
      if (ok) showToast("Enregistré", "success");
      else showToast("Erreur lors de la sauvegarde", "error");
    });

    // Temperature feature
    $("#temperature_enabled")?.addEventListener("change", () => {
      updateFeatureVisibility("temperature");
      updateFiltrationControls();  // Mode auto dépend de la température
      save();
    });

    // Development panel - Sensor logs / Debug logs / Screen
    $("#sensor_logs_enabled")?.addEventListener("change", save);
    $("#debug_logs_enabled")?.addEventListener("change", save);
    $("#screen_enabled")?.addEventListener("change", save);
  }

  // ---------- Sidebar helpers ----------
  function openSidebar() {
    const sidebar = $("#sidebar");
    const overlay = $("#sidebar-overlay");
    const burgerOpen = $("#burger-open");
    sidebar?.classList.add("is-open");
    overlay?.classList.add("is-visible");
    overlay?.removeAttribute("aria-hidden");
    if (burgerOpen) {
      burgerOpen.setAttribute("aria-expanded", "true");
      burgerOpen.setAttribute("aria-label", "Fermer le menu de navigation");
      burgerOpen.querySelector("span").textContent = "✕";
    }
  }

  function closeSidebar() {
    const sidebar = $("#sidebar");
    const overlay = $("#sidebar-overlay");
    const burgerOpen = $("#burger-open");
    sidebar?.classList.remove("is-open");
    overlay?.classList.remove("is-visible");
    overlay?.setAttribute("aria-hidden", "true");
    if (burgerOpen) {
      burgerOpen.setAttribute("aria-expanded", "false");
      burgerOpen.setAttribute("aria-label", "Ouvrir le menu de navigation");
      burgerOpen.querySelector("span").textContent = "☰";
    }
  }

  // ---------- UI bindings ----------
  function bindUI() {
    // burger topbar : toggle open/close
    $("#burger-open")?.addEventListener("click", () => {
      if ($("#sidebar")?.classList.contains("is-open")) closeSidebar();
      else openSidebar();
    });

    // Close sidebar on overlay click
    $("#sidebar-overlay")?.addEventListener("click", closeSidebar);

    // Close sidebar on Escape key
    document.addEventListener("keydown", (e) => {
      if (e.key === "Escape" && $("#sidebar")?.classList.contains("is-open")) {
        closeSidebar();
        $("#burger-open")?.focus();
      }
    });

    // segmented -> route (with arrow key navigation)
    const segmentedBtns = $$(".segmented__btn[data-settings-tab]");
    segmentedBtns.forEach((btn, i) => {
      btn.addEventListener("click", () => goSettings(btn.getAttribute("data-settings-tab")));
      btn.addEventListener("keydown", (e) => {
        let target = null;
        if (e.key === "ArrowRight") {
          target = segmentedBtns[(i + 1) % segmentedBtns.length];
        } else if (e.key === "ArrowLeft") {
          target = segmentedBtns[(i - 1 + segmentedBtns.length) % segmentedBtns.length];
        } else if (e.key === "Home") {
          target = segmentedBtns[0];
        } else if (e.key === "End") {
          target = segmentedBtns[segmentedBtns.length - 1];
        }
        if (target) {
          e.preventDefault();
          target.click();
          target.focus();
        }
      });
    });

    $("#refresh_info_btn")?.addEventListener("click", loadSystemInfo);

    $("#coredump_refresh_btn")?.addEventListener("click", loadCoredumpInfo);

    $("#coredump_download_btn")?.addEventListener("click", async () => {
      try {
        const resp = await authFetch("/coredump/download");
        if (!resp.ok) { showToast("Aucun coredump disponible", "error"); return; }
        const blob = await resp.blob();
        const url = URL.createObjectURL(blob);
        const a = document.createElement("a");
        a.href = url;
        a.download = "coredump.bin";
        document.body.appendChild(a);
        a.click();
        document.body.removeChild(a);
        URL.revokeObjectURL(url);
      } catch (e) {
        showToast("Erreur lors du téléchargement", "error");
      }
    });

    $("#coredump_erase_btn")?.addEventListener("click", async () => {
      if (!confirm("Effacer le coredump ?\n\nL'analyse du crash actuel sera définitivement perdue.")) return;
      try {
        const resp = await authFetch("/coredump", { method: "DELETE" });
        if (!resp.ok) { showToast("Erreur lors de l'effacement", "error"); return; }
        showToast("Coredump effacé", "success");
        await loadCoredumpInfo();
      } catch (e) {
        showToast("Erreur lors de l'effacement", "error");
      }
    });

    // ===== Debug oscillation pH (feature-021 hotfix) =====
    let debugPhChart = null;
    let debugPhPauseTimer = null;
    const __debugPhRender = (json) => {
      const samples = (json && Array.isArray(json.samples)) ? json.samples : [];
      const count = samples.length;
      const countEl = document.getElementById("debug_ph_count");
      if (countEl) countEl.textContent = String(count);
      const wrap = document.getElementById("debug_ph_chart_wrap");
      const stats = document.getElementById("debug_ph_stats");
      if (count === 0) {
        if (wrap) wrap.style.display = "none";
        if (stats) stats.style.display = "none";
        return;
      }
      // Convertir t (millis ESP32) en delta secondes vs maintenant côté ESP32
      const nowEsp = json.now || (samples[count - 1].t);
      const labels = samples.map(s => `-${Math.round((nowEsp - s.t) / 1000)}s`);
      const phData = samples.map(s => (typeof s.ph === "number") ? s.ph : null);
      const phFilteredData = samples.map(s => (typeof s.phFiltered === "number") ? s.phFiltered : null);
      const orpData = samples.map(s => (typeof s.orp === "number") ? s.orp : null);
      const tData = samples.map(s => (typeof s.tempC === "number") ? s.tempC : null);

      const ctx = document.getElementById("debug_ph_chart")?.getContext("2d");
      if (!ctx) return;
      if (debugPhChart) debugPhChart.destroy();
      debugPhChart = new Chart(ctx, {
        type: "line",
        data: {
          labels,
          datasets: [
            { label: "pH brut", data: phData, borderColor: "#1976d2", backgroundColor: "transparent", yAxisID: "y", tension: 0.1, pointRadius: 1.5, spanGaps: true },
            { label: "pH lissé", data: phFilteredData, borderColor: "#ff9800", backgroundColor: "transparent", yAxisID: "y", tension: 0.1, pointRadius: 0, borderWidth: 2, spanGaps: true },
            { label: "ORP (mV)", data: orpData, borderColor: "#d32f2f", backgroundColor: "transparent", yAxisID: "y1", tension: 0.1, pointRadius: 1.5, spanGaps: true, hidden: true },
            { label: "T° envoyée (°C)", data: tData, borderColor: "#388e3c", backgroundColor: "transparent", yAxisID: "y2", tension: 0.1, pointRadius: 1.5, spanGaps: true, hidden: true },
          ]
        },
        options: {
          responsive: true,
          interaction: { mode: "index", intersect: false },
          scales: {
            y:  { type: "linear", position: "left",  title: { display: true, text: "pH" } },
            y1: { type: "linear", position: "right", title: { display: true, text: "ORP (mV)" }, grid: { drawOnChartArea: false } },
            y2: { type: "linear", display: false }
          },
          plugins: { legend: { position: "top" } }
        }
      });
      if (wrap) wrap.style.display = "block";

      // Stats
      const phNum  = phData.filter(v => v !== null);
      const phFilteredNum = phFilteredData.filter(v => v !== null);
      const orpNum = orpData.filter(v => v !== null);
      const tNum   = tData.filter(v => v !== null);
      const fmt = (arr, dec) => arr.length ? `${Math.min(...arr).toFixed(dec)} / ${Math.max(...arr).toFixed(dec)} / Δ ${(Math.max(...arr) - Math.min(...arr)).toFixed(dec)}` : "—";
      document.getElementById("debug_ph_stats_ph").textContent  = fmt(phNum, 3);
      { const el = document.getElementById("debug_ph_stats_ph_filtered"); if (el) el.textContent = fmt(phFilteredNum, 3); }
      document.getElementById("debug_ph_stats_orp").textContent = fmt(orpNum, 1);
      document.getElementById("debug_ph_stats_t").textContent   = tNum.length ? `${Math.min(...tNum).toFixed(1)}°C / ${Math.max(...tNum).toFixed(1)}°C` : "—";
      if (stats) stats.style.display = "block";
    };

    const __debugPhLoad = async () => {
      try {
        const resp = await authFetch("/debug/ph_trace");
        if (!resp.ok) { showToast("Erreur chargement trace pH", "error"); return; }
        const json = await resp.json();
        __debugPhRender(json);
      } catch (e) {
        showToast("Erreur réseau /debug/ph_trace", "error");
      }
    };

    const __debugWifiPauseStatus = async () => {
      try {
        const resp = await authFetch("/debug/wifi_pause");
        if (!resp.ok) return;
        const json = await resp.json();
        const el = document.getElementById("debug_wifi_pause_status");
        if (!el) return;
        if (json.active) {
          const sec = Math.ceil((json.remaining_ms || 0) / 1000);
          el.textContent = `active — ${sec} s restantes`;
        } else {
          el.textContent = "inactive";
        }
      } catch (e) { /* ignore */ }
    };

    document.getElementById("debug_ph_refresh_btn")?.addEventListener("click", __debugPhLoad);

    document.getElementById("debug_ph_clear_btn")?.addEventListener("click", async () => {
      try {
        const resp = await authFetch("/debug/ph_trace_clear", { method: "POST" });
        if (resp.ok) {
          showToast("Buffer vidé", "success");
          if (debugPhChart) { debugPhChart.destroy(); debugPhChart = null; }
          document.getElementById("debug_ph_chart_wrap").style.display = "none";
          document.getElementById("debug_ph_stats").style.display = "none";
          document.getElementById("debug_ph_count").textContent = "0";
        }
      } catch (e) { showToast("Erreur réseau", "error"); }
    });

    document.getElementById("debug_wifi_pause_btn")?.addEventListener("click", async () => {
      const seconds = 120;
      if (!confirm(`Couper le WiFi pendant ${seconds} secondes ?\n\nLa page sera inaccessible. Recharge-la après ${seconds + 10} s pour voir la trace.`)) return;
      try {
        const resp = await authFetch("/debug/wifi_pause", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({ seconds })
        });
        if (!resp.ok) {
          const err = await resp.json().catch(() => ({}));
          showToast(`Erreur : ${err.error || resp.status}`, "error");
          return;
        }
        showToast(`WiFi va se couper. Recharge la page dans ${seconds + 10}s.`, "info");
      } catch (e) {
        // Le WiFi peut s'être déjà coupé → c'est attendu
        showToast(`WiFi coupé. Recharge la page dans ${seconds + 10}s.`, "info");
      }
    });

    // Auto-load à l'arrivée sur la page Diagnostic
    document.querySelector('[data-target="advanced"]')?.addEventListener("click", () => {
      setTimeout(() => { __debugPhLoad(); __debugWifiPauseStatus(); }, 200);
    });

    // ===== Diagnostic EZO — commandes manuelles (debug feature-024) =====
    const __debugEzoHistory = [];
    const __debugEzoMaxHistory = 30;

    const __debugEzoFormatHistoryEntry = (addrLabel, cmd, json) => {
      const t = new Date();
      const ts = `${String(t.getHours()).padStart(2,'0')}:${String(t.getMinutes()).padStart(2,'0')}:${String(t.getSeconds()).padStart(2,'0')}`;
      const status = json.status_code != null ? json.status_code : '?';
      const label = json.status_label || 'unknown';
      const resp = json.response || '';
      return `⏱ ${ts} ${addrLabel} ${cmd.padEnd(10)} → ${status} (${label})${resp ? ': ' + resp : ''}`;
    };

    const __debugEzoUpdateHistoryUI = () => {
      const div = document.getElementById('debug_ezo_history');
      if (!div) return;
      if (__debugEzoHistory.length === 0) {
        div.innerHTML = '<em class="muted">Aucune commande envoyée.</em>';
        return;
      }
      div.innerHTML = __debugEzoHistory.slice().reverse()
        .map(e => `<div>${e.replace(/</g,'&lt;')}</div>`).join('');
    };

    const __debugEzoSend = async (cmd, delayMs) => {
      const addrSelect = document.getElementById('debug_ezo_addr');
      const addr = parseInt(addrSelect?.value || '98', 10);
      const addrLabel = addrSelect?.selectedOptions?.[0]?.textContent?.split(' ')?.[0] || 'EZO';
      const respDiv = document.getElementById('debug_ezo_response');
      const statusEl = document.getElementById('debug_ezo_status');
      const respTextEl = document.getElementById('debug_ezo_resp_text');
      const respHexEl = document.getElementById('debug_ezo_resp_hex');

      try {
        const res = await authFetch('/debug/ezo_command', {
          method: 'POST',
          headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify({ addr, cmd, delay_ms: delayMs })
        });
        const json = await res.json().catch(() => ({}));

        if (!res.ok) {
          const errMsg = json.error || `HTTP ${res.status}`;
          if (statusEl) statusEl.textContent = `⚠ ${errMsg}`;
          if (respTextEl) respTextEl.textContent = '—';
          if (respHexEl) respHexEl.textContent = '—';
          if (respDiv) respDiv.style.display = 'block';
          showToast(`Erreur EZO : ${errMsg}`, 'error');
          __debugEzoHistory.push(`⚠ ${addrLabel} ${cmd} → ${errMsg}`);
          while (__debugEzoHistory.length > __debugEzoMaxHistory) __debugEzoHistory.shift();
          __debugEzoUpdateHistoryUI();
          return;
        }

        if (statusEl) {
          const sc = json.status_code;
          const lbl = json.status_label || '';
          const cls = sc === 1 ? 'success' : sc === 2 ? 'danger' : 'warn';
          statusEl.innerHTML = `<span style="color:var(--${cls})">${sc} (${lbl})</span>`;
        }
        if (respTextEl) respTextEl.textContent = json.response || '(vide)';
        if (respHexEl) respHexEl.textContent = (json.raw_hex || []).join(' ');
        if (respDiv) respDiv.style.display = 'block';

        __debugEzoHistory.push(__debugEzoFormatHistoryEntry(addrLabel, cmd, json));
        while (__debugEzoHistory.length > __debugEzoMaxHistory) __debugEzoHistory.shift();
        __debugEzoUpdateHistoryUI();
      } catch (e) {
        showToast(`Erreur réseau : ${e.message}`, 'error');
      }
    };

    // Boutons commandes courantes
    document.querySelectorAll('[data-ezo-cmd]').forEach(btn => {
      btn.addEventListener('click', () => {
        const cmd = btn.getAttribute('data-ezo-cmd');
        const delayMs = parseInt(btn.getAttribute('data-ezo-delay') || '900', 10);
        __debugEzoSend(cmd, delayMs);
      });
    });

    // Bouton « Envoyer » pour commande personnalisée
    document.getElementById('debug_ezo_send_btn')?.addEventListener('click', () => {
      const cmdInput = document.getElementById('debug_ezo_cmd');
      const delayInput = document.getElementById('debug_ezo_delay');
      const cmd = (cmdInput?.value || '').trim();
      const delayMs = parseInt(delayInput?.value || '900', 10);
      if (!cmd) {
        showToast('Saisis une commande', 'warning');
        return;
      }
      __debugEzoSend(cmd, delayMs);
    });

    // Enter dans le champ commande perso → envoyer
    document.getElementById('debug_ezo_cmd')?.addEventListener('keydown', (e) => {
      if (e.key === 'Enter') {
        e.preventDefault();
        document.getElementById('debug_ezo_send_btn')?.click();
      }
    });

    // Logo topbar → blur immédiat pour éviter le focus ring persistant sur iOS Safari
    $("#topbar-home-link")?.addEventListener("click", (e) => {
      e.currentTarget.blur();
    });

    // Logout button
    $("#logout-btn")?.addEventListener("click", (e) => {
      e.preventDefault();
      if (confirm("Voulez-vous vraiment vous déconnecter ?")) {
        // Clear session storage
        sessionStorage.removeItem('authToken');
        // Redirect to login page
        window.location.href = '/login.html';
      }
    });

    // Reboot button
    $("#reboot_btn")?.addEventListener("click", async () => {
      if (!confirm("Voulez-vous vraiment redémarrer ?\n\nL'appareil sera indisponible pendant environ 10 secondes.")) {
        return;
      }

      try {
        const res = await authFetch("/reboot", {
          method: "POST"
        });

        if (res.ok) {
          alert("Redémarrage en cours...\n\nVeuillez patienter environ 10 secondes puis actualiser la page.");
          // Désactiver le bouton pour éviter les clics multiples
          $("#reboot_btn").disabled = true;
          $("#reboot_btn").textContent = "Redémarrage en cours...";
        } else {
          alert("Erreur lors du redémarrage de l'ESP32");
        }
      } catch (error) {
        alert("Erreur de connexion: " + error.message);
      }
    });

    // Factory reset button
    $("#factory_reset_btn")?.addEventListener("click", async () => {
      if (!confirm("Réinitialisation du système\n\n⚠ Cette action est irréversible.\n\nToutes les données seront supprimées :\n- Configuration (pH, ORP, MQTT, Wi-Fi…)\n- Calibrations\n- Historique des produits\n\nLe système redémarrera sur l'assistant de configuration.\n\nConfirmer la réinitialisation ?")) {
        return;
      }
      // Double confirmation
      if (!confirm("Dernière confirmation.\n\nVoulez-vous vraiment effacer toutes les données ?")) {
        return;
      }
      try {
        const btn = $("#factory_reset_btn");
        btn.disabled = true;
        btn.textContent = "Réinitialisation…";
        const res = await authFetch("/factory-reset", { method: "POST" });
        if (res.ok) {
          alert("Réinitialisation effectuée.\n\nL'appareil redémarre. Connectez-vous au réseau Wi-Fi de l'ESP32 pour accéder à l'assistant de configuration.");
        } else {
          alert("Erreur lors de la réinitialisation.");
          btn.disabled = false;
          btn.textContent = "Réinitialiser";
        }
      } catch {
        alert("Erreur de connexion.");
        const btn = $("#factory_reset_btn");
        if (btn) { btn.disabled = false; btn.textContent = "Réinitialiser"; }
      }
    });
  }

  // ---------- Security bindings ----------
  function bindSecurity() {
    trackDirtyState("#cors-save-btn", ["auth_cors_origins"]);
    // Save CORS configuration (bouton manuel)
    const corsBtn = $("#cors-save-btn");
    const setCorsBtnState = (label, status) => {
      if (!corsBtn) return;
      corsBtn.disabled = status === "saving" || status === "clean";
      corsBtn.classList.remove("btn--primary", "btn--ok", "btn--danger");
      if (status === "success") corsBtn.classList.add("btn--ok");
      else if (status === "error") corsBtn.classList.add("btn--danger");
      else corsBtn.classList.add("btn--primary");
      corsBtn.textContent = label;
    };
    corsBtn?.addEventListener("click", async () => {
      setCorsBtnState("Sauvegarde...", "saving");
      const ok = await sendConfig({ auth_cors_origins: $("#auth_cors_origins").value.trim() });
      if (ok) {
        setCorsBtnState("Sauvegarde réussie", "success");
        setTimeout(() => setCorsBtnState("Sauvegarder", "clean"), 2000);
      } else {
        setCorsBtnState("Erreur", "error");
        setTimeout(() => setCorsBtnState("Sauvegarder", "default"), 2000);
      }
    });

    // Password validation helper
    function checkPasswordStrength(password) {
      const rules = {
        length: password.length >= 8,
        number: /\d/.test(password),
        special: /[!@#$%^&*()_+\-=\[\]{};':"\\|,.<>\/?]/.test(password)
      };
      const validCount = Object.values(rules).filter(Boolean).length;
      let strength = 'weak';
      if (validCount === 3) strength = 'strong';
      else if (validCount >= 2) strength = 'medium';
      return { rules, strength };
    }

    // Update password feedback UI
    function updatePasswordFeedback() {
      const password = $("#auth_password")?.value || "";
      const checklist = $("#passwordChecklist");
      const strengthBar = $("#passwordStrength");
      const strengthLabel = $("#passwordStrengthLabel");

      if (!checklist || !strengthBar) return;

      if (password.length > 0) {
        checklist.style.display = 'block';
        strengthBar.style.display = 'flex';

        const { rules, strength } = checkPasswordStrength(password);

        // Update checklist items
        Object.keys(rules).forEach(rule => {
          const item = checklist.querySelector(`[data-rule="${rule}"]`);
          if (item) {
            item.classList.toggle('valid', rules[rule]);
          }
        });

        // Update strength bar
        strengthBar.className = `password-strength ${strength}`;

        // Update strength label
        const strengthText = { weak: 'Faible', medium: 'Moyen', strong: 'Fort' };
        if (strengthLabel) {
          strengthLabel.textContent = strengthText[strength];
          strengthLabel.classList.add('visible');
        }
      } else {
        checklist.style.display = 'none';
        strengthBar.style.display = 'none';
        if (strengthLabel) strengthLabel.classList.remove('visible');
      }
    }

    // Password validation event listeners
    $("#auth_password")?.addEventListener("input", updatePasswordFeedback);

    // Password toggle buttons
    document.querySelectorAll(".password-toggle").forEach(btn => {
      btn.addEventListener("click", () => {
        const targetId = btn.dataset.target;
        const input = document.getElementById(targetId);
        const eyeOpen = btn.querySelector('.eye-open');
        const eyeClosed = btn.querySelector('.eye-closed');
        if (input && input.type === 'password') {
          input.type = 'text';
          eyeOpen?.classList.add('hidden');
          eyeClosed?.classList.remove('hidden');
        } else if (input) {
          input.type = 'password';
          eyeOpen?.classList.remove('hidden');
          eyeClosed?.classList.add('hidden');
        }
      });
    });

    // Change password
    $("#change_password_btn")?.addEventListener("click", async () => {
      const currentPassword = $("#auth_current_password").value;
      const newPassword = $("#auth_password").value;
      const confirmPassword = $("#auth_password_confirm").value;

      // Validation: current password required
      if (!currentPassword) {
        alert("Veuillez saisir votre mot de passe actuel.");
        return;
      }

      // Validation: new password strength
      const { rules } = checkPasswordStrength(newPassword);
      const allValid = Object.values(rules).every(Boolean);
      if (!allValid) {
        const missing = [];
        if (!rules.length) missing.push('8 caractères minimum');
        if (!rules.number) missing.push('1 chiffre');
        if (!rules.special) missing.push('1 caractère spécial');
        alert(`Le nouveau mot de passe doit contenir: ${missing.join(', ')}`);
        return;
      }

      // Validation: passwords match
      if (newPassword !== confirmPassword) {
        alert("Les nouveaux mots de passe ne correspondent pas.");
        return;
      }

      // Validation: new password must be different
      if (currentPassword === newPassword) {
        alert("Le nouveau mot de passe doit être différent du mot de passe actuel.");
        return;
      }

      if (!confirm("Voulez-vous vraiment modifier le mot de passe administrateur ?")) {
        return;
      }

      try {
        const token = sessionStorage.getItem('authToken');
        const res = await fetch("/auth/change-password", {
          method: "POST",
          headers: Object.assign({ "Content-Type": "application/json" }, token ? { "X-Auth-Token": token } : {}),
          body: JSON.stringify({
            currentPassword: currentPassword,
            newPassword: newPassword
          })
        });

        if (!res.ok) {
          const error = await res.json().catch(() => ({}));
          if (res.status === 401) {
            alert("L'ancien mot de passe est incorrect.");
          } else {
            alert("Erreur: " + (error.error || "Échec du changement de mot de passe"));
          }
          return;
        }

        const data = await res.json();

        // Update the token in sessionStorage
        if (data.token) {
          sessionStorage.setItem('authToken', data.token);
        }

        alert("Mot de passe modifié avec succès !");
        // Clear all password fields and reset validation UI
        $("#auth_current_password").value = "";
        $("#auth_password").value = "";
        $("#auth_password_confirm").value = "";
        updatePasswordFeedback();
      } catch (error) {
        alert("Erreur de connexion: " + error.message);
      }
    });
  }

  // ---------- Init ----------
  async function init() {
    const perf = debugStart("init");

    // Vérifier l'authentification au chargement
    const token = sessionStorage.getItem('authToken');
    if (!token) {
      debugLog("Pas de token trouvé, redirection vers login");
      window.location.href = '/login.html';
      return;
    }

    // Router - Appliquer immédiatement pour éviter le flash d'écran
    const applyRoute = () => {
      const routePerf = debugStart("applyRoute");
      const r = getRoute();
      showView(r);

      // Forcer un rafraîchissement court quand on arrive sur le dashboard
      if (r.view === "/dashboard") {
        const now = Date.now();
        const dataAge = now - lastSensorDataLoadTime;
        const maxDataAge = 5000; // 5 secondes de tolérance pour éviter double chargement

        if (lastSensorDataLoadTime === 0 || dataAge > maxDataAge) {
          loadSensorData({ force: lastSensorDataLoadTime === 0, source: "route-dashboard" });
        }
      }
      routePerf?.end(`route=${r.view}`);
    };
    window.addEventListener("hashchange", applyRoute);
    applyRoute(); // Afficher la vue immédiatement

    // charts - Les anciens graphiques restent pour stocker les données historiques
    const tempChartCanvas = $("#tempChart");
    const phChartCanvas = $("#phChart");
    const orpChartCanvas = $("#orpChart");

    // Créer les graphiques cachés seulement s'ils existent (compatibilité)
    if (tempChartCanvas) tempChart = createLineChart(tempChartCanvas, "#4f8fff", "Température");
    if (phChartCanvas) {
      phChart = createLineChart(phChartCanvas, "#8b5cf6", "pH", {
        yMin: PH_AXIS_MIN_DEFAULT,
        yMax: PH_AXIS_MAX_DEFAULT,
        fill: false
      });
      ensurePhReferenceDatasets(phChart);
    }
    if (orpChartCanvas) {
      orpChart = createLineChart(orpChartCanvas, "#10b981", "ORP", {
        integerOnly: true,
        yMin: ORP_AXIS_MIN_DEFAULT,
        yMax: ORP_AXIS_MAX_DEFAULT,
        fill: false
      });
      ensureOrpReferenceDatasets(orpChart);
    }

    bindDetailCharts();

    bindUI();
    bindHistoryBackup();
    bindDetailActions();
    bindAutosave();
    bindPhCalibration();
    bindPhProbeChip();
    bindFilterChip('ph');
    bindFilterChip('orp');
    bindOrpCalibration();
    bindTempCalibration();
    bindWifi();
    bindMqttManualSave();
    bindFiltrationManualSave();
    bindPumpConfigSave();
    bindTimeManualSave();
    bindSecurity();
    bindGithubUpdate();
    bindManualUpdate();
    bindLogs();
    bindSondesOnewire();

    // Reconnexion immédiate lors du retour sur la page (iPad/mobile : après déverrouillage)
    document.addEventListener('visibilitychange', () => {
      if (!document.hidden) {
        _closeWs();
        // Petit délai pour laisser Safari libérer la socket avant d'en ouvrir une nouvelle
        setTimeout(initWebSocket, 100);
      }
    });

    // Charger la config avant d'ouvrir le WebSocket : l'ESP32 a fini de traiter
    // les requêtes HTTP initiales (assets statiques), les sockets lwIP sont libérées,
    // et le WS ne concurrence plus les fetches de chargement.
    const configPerf = debugStart("loadConfig");
    await loadConfig().catch(() => {});
    configPerf?.end();

    setNetStatus("mid", "Connexion…");
    initWebSocket();

    // Create mini charts for dashboard cards
    phMiniChart = createMiniLineChart(
      'ph-mini-chart',
      () => window._config?.ph_target ?? 7.2,
      () => 0.2,
      v => v.toFixed(1)
    );
    orpMiniChart = createMiniLineChart(
      'orp-mini-chart',
      () => window._config?.orp_target ?? 700,
      () => 50,
      v => Math.round(v) + ' mV'
    );
    tempMiniChart = createMiniLineChart(
      'temp-mini-chart',
      () => 26,
      () => 8,
      v => v.toFixed(1) + ' °C'
    );

    // Load historical data to populate charts
    const historyPerf = debugStart("loadHistoricalData");
    await loadHistoricalData('all').catch(() => {});
    historyPerf?.end();

    // Chargement initial des mini charts (3 jours)
    loadMiniChartData(0).catch(() => {});
    // Polling incrémental toutes les 5 minutes
    setInterval(() => {
      loadMiniChartData(lastMiniChartTimestamp).catch(() => {});
    }, 5 * 60 * 1000);

    const calibPerf = debugStart("checkCalibrationDate");
    await checkCalibrationDate().catch(() => {});
    calibPerf?.end();

    const logsPerf = debugStart("loadLogs");
    await loadLogs(true).catch(() => {});
    logsPerf?.end();

    // Load system info after a small delay to ensure DOM is ready
    // This is critical when direct linking to #/settings/dev or #/settings/system
    setTimeout(async () => {
      try {
        await loadSystemInfo();
        await loadCoredumpInfo().catch(() => {});
      } catch (e) {
        console.error("Failed to load system info:", e);
      }
    }, 200);
    setInterval(checkCalibrationDate, 300000); // 5 min

    // Rafraîchissement de l'heure au changement de minute (si on est sur settings et pas en édition)
    const refreshTimeField = async () => {
      const currentView = window.location.hash || "#view-home";
      if (!currentView.includes("settings")) return;

      const activeId = document.activeElement?.id || "";
      const timeEditing = ["time_use_ntp", "time_ntp_server", "time_timezone", "time_value"].includes(activeId);
      if (timeEditing) return;

      try {
        const res = await authFetch("/get-config");
        if (!res.ok) return;
        const cfg = await res.json();
        if (cfg.time_current) {
          // Masquer les secondes pour l'affichage
          let timeValue = cfg.time_current;
          if (timeValue.length > 16) {
            timeValue = timeValue.substring(0, 16);
          }
          $("#time_value").value = timeValue;
        }
      } catch (e) { /* ignore */ }
    };

    // Synchroniser avec le changement de minute réel
    const msUntilNextMinute = (60 - new Date().getSeconds()) * 1000;
    setTimeout(() => {
      refreshTimeField();
      setInterval(refreshTimeField, 60000);
    }, msUntilNextMinute);

    // La config est maintenant poussée par WebSocket après chaque save-config.
    // Le polling HTTP (15s) est supprimé.

    // ========== PRODUITS ==========
    setupProductScreen();

    // ========== FILTRATION MANUAL CONTROL ==========
    setupFiltrationManualControl();

    // ========== LIGHTING SCHEDULE ==========
    setupLightingSchedule();

    // ========== WIFI CONFIG BUTTON ==========
    const wifiConfigBtn = $("#wifi_config_btn");
    if (wifiConfigBtn) {
      wifiConfigBtn.addEventListener("click", () => {
        window.location.href = '/wifi.html';
      });
    }

    perf?.end();
  }

  // ========== LIGHTING SCHEDULE SETUP ==========
  function setupLightingSchedule() {
    const manualOnBtn = $("#lighting-manual-on");
    const manualOffBtn = $("#lighting-manual-off");

    // Manual control buttons
    if (manualOnBtn) {
      manualOnBtn.addEventListener("click", async () => {
        try {
          const response = await authFetch("/lighting/on", { method: "POST" });
          if (response.ok) {
            if (latestSensorData) latestSensorData.lighting_enabled = true;
            updateLightingStatus(true);
            showToast("Éclairage allumé", "success");
          }
        } catch (error) {
          console.error("Error turning on lighting:", error);
          showToast("Erreur de connexion", "error");
        }
      });
    }

    if (manualOffBtn) {
      manualOffBtn.addEventListener("click", async () => {
        try {
          const response = await authFetch("/lighting/off", { method: "POST" });
          if (response.ok) {
            if (latestSensorData) latestSensorData.lighting_enabled = false;
            updateLightingStatus(false);
            showToast("Éclairage éteint", "success");
          }
        } catch (error) {
          console.error("Error turning off lighting:", error);
          showToast("Erreur de connexion", "error");
        }
      });
    }

    // Load current lighting config
    loadLightingConfig();
  }

  async function loadLightingConfig() {
    try {
      const config = window._config || {};

      // Feature enabled switch
      $("#lighting_feature_enabled").checked = config.lighting_feature_enabled !== false;
      updateFeatureVisibility("lighting");

      const scheduleMode = $("#lighting_schedule_mode");
      const scheduleSettings = $("#lighting-schedule-settings");

      if (scheduleMode && config.lighting_schedule_enabled !== undefined) {
        scheduleMode.value = config.lighting_schedule_enabled ? "enabled" : "disabled";
        scheduleSettings.style.display = config.lighting_schedule_enabled ? "block" : "none";
      }

      if (config.lighting_start_time) {
        const startInput = $("#lighting_start_time");
        if (startInput) startInput.value = config.lighting_start_time;
      }

      if (config.lighting_end_time) {
        const endInput = $("#lighting_end_time");
        if (endInput) endInput.value = config.lighting_end_time;
      }
    } catch (error) {
      console.error("Error loading lighting config:", error);
    }
  }

  // ========== FILTRATION MANUAL CONTROL ==========
  function setupFiltrationManualControl() {
    const startBtn = $("#filtration-manual-start");
    const stopBtn = $("#filtration-manual-stop");

    // Start filtration: force ON sans modifier la programmation
    if (startBtn) {
      startBtn.addEventListener("click", async () => {
        const payload = { filtration_enabled: true, filtration_force_on: true, filtration_force_off: false };
        try {
          const result = await sendConfig(payload);
          if (result) {
            filtrationRunningOverride = true;
            updateFiltrationControls();
            updateFeatureVisibility("filtration");
            updateFiltrationBadges();
            showToast("Filtration démarrée", "success");
            setTimeout(() => {
              loadSensorData({ force: true, source: "filtration-start" });
              loadConfig().catch(() => {});
            }, 1000);
          } else {
            showToast("Erreur lors du démarrage", "error");
          }
        } catch (error) {
          console.error("Error starting filtration:", error);
          showToast("Erreur de connexion", "error");
        }
      });
    }

    // Stop filtration: force OFF (overrides schedule)
    if (stopBtn) {
      stopBtn.addEventListener("click", async () => {
        const payload = { filtration_force_on: false, filtration_force_off: true };
        try {
          const result = await sendConfig(payload);
          if (result) {
            filtrationRunningOverride = false;
            updateFiltrationControls();
            updateFiltrationBadges();
            showToast("Filtration arrêtée", "success");
            setTimeout(() => {
              loadSensorData({ force: true, source: "filtration-stop" });
              loadConfig().catch(() => {});
            }, 1000);
          } else {
            showToast("Erreur lors de l'arrêt", "error");
          }
        } catch (error) {
          console.error("Error stopping filtration:", error);
          showToast("Erreur de connexion", "error");
        }
      });
    }

    // Initial status update
    updateFiltrationBadges();
  }

  function updateLightingStatus(isOn) {
    const statusBadge = $("#lighting-current-status");
    const detailStatusBadge = $("#detail-lighting-status");

    // Card "Contrôle manuel" : badge .pill placé dans .card__head
    if (statusBadge) {
      statusBadge.textContent = isOn ? "Allumé" : "Éteint";
      statusBadge.className = isOn ? "pill ok" : "pill bad";
    }

    // Dashboard : conserve la classe legacy .state-badge--*
    if (detailStatusBadge) {
      detailStatusBadge.textContent = isOn ? "Allumé" : "Éteint";
      detailStatusBadge.className = isOn ? "state-badge state-badge--ok" : "state-badge state-badge--off";
    }
  }

  window.addEventListener("DOMContentLoaded", init);
})();
