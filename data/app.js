(() => {
  "use strict";

  // ---------- Utils ----------
  const $ = (sel, root = document) => root.querySelector(sel);
  const $$ = (sel, root = document) => Array.from(root.querySelectorAll(sel));
  const clamp = (n, a, b) => Math.max(a, Math.min(b, n));
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
      // Forcer la fermeture pour déclencher la reconnexion
      if (_ws) _ws.close();
    }, kWsHeartbeatMs);
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
      debugLog('[WS] Disconnected, reconnecting in 3s...');
      _wsReconnectTimer = setTimeout(() => { _wsReconnectTimer = null; initWebSocket(); }, 3000);
    };
    _ws.onclose = _ws.onerror = _onDisconnect;
  }

  // Heure de démarrage de l'ESP32 estimée à partir de uptime_ms (mis à jour à chaque push sensor_data)
  let _bootEpochMs = null;

  function formatLogTimestamp(ms) {
    if (_bootEpochMs == null || ms == null) return ms ?? '';
    const d = new Date(_bootEpochMs + ms);
    const p = (n) => String(n).padStart(2, '0');
    return `${p(d.getHours())}:${p(d.getMinutes())}:${p(d.getSeconds())}`;
  }

  function _onWsSensorData(json) {
    if (json.uptime_ms != null) _bootEpochMs = Date.now() - json.uptime_ms;
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
    if ((entry.timestamp || 0) > lastLogTimestamp) lastLogTimestamp = entry.timestamp;
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

  function updateYAxisOverlay(chart) {
    if (!chart) return;
    const overlay = $("#mainChartYAxis");
    if (!overlay) return;

    const scale = chart.scales?.y;
    if (!scale) return;

    overlay.innerHTML = "";
    const ticks = scale.ticks || [];
    const top = chart.chartArea?.top ?? 0;

    ticks.forEach((tick, index) => {
      const label = tick.label ?? tick.value ?? "";
      const el = document.createElement("div");
      el.className = "chart-y-tick";
      el.textContent = label;
      const y = scale.getPixelForTick(index);
      el.style.top = `${y}px`;
      el.style.transform = "translateY(-50%)";
      overlay.appendChild(el);
    });
  }

  

  function updateMainChartScroll() {
    const container = $("#mainChartScroll");
    const inner = $("#mainChartScrollInner");
    if (!mainChart || !container || !inner) return;

    const pointCount = mainChart.data.labels ? mainChart.data.labels.length : 0;
    const styles = getComputedStyle(container);
    const paddingLeft = parseFloat(styles.paddingLeft || "0") || 0;
    const baseWidth = Math.max(0, (container.clientWidth || 0) - paddingLeft);
    const desiredWidth = Math.max(baseWidth, pointCount * CHART_POINT_PX);
    if (inner.style.width !== `${desiredWidth}px`) {
      inner.style.width = `${desiredWidth}px`;
      mainChart.resize();
    }

    if (chartAutoScroll) {
      const maxScrollLeft = container.scrollWidth - container.clientWidth;
      container.scrollLeft = Math.max(0, maxScrollLeft);
    }
  }

  function bindChartScroll() {
    const container = $("#mainChartScroll");
    if (!container) return;
    container.addEventListener('scroll', () => {
      const maxScrollLeft = container.scrollWidth - container.clientWidth;
      chartAutoScroll = (maxScrollLeft - container.scrollLeft) <= CHART_SCROLL_EPS;
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

    // Calculer les limites dynamiques de l'axe Y
    const dataPoints = chart.data.datasets[0]?.data || [];
    const limits = calculateAxisLimits(dataPoints, PH_AXIS_MIN_DEFAULT, PH_AXIS_MAX_DEFAULT);

    chart.data.datasets[1].data = build(limits.max);
    chart.data.datasets[2].data = build(PH_MAX);
    chart.data.datasets[3].data = build(PH_MIN);
    chart.data.datasets[4].data = build(limits.min);
  }

  function ensurePhPlaceholderLabels(chart) {
    if (!chart) return;
    if (chart.data.labels.length === 0) {
      chart.data.labels = ["", ""];
    }
    if (chart.data.datasets[0] && chart.data.datasets[0].data.length === 0) {
      chart.data.datasets[0].data = [null, null];
    }
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

    // Calculer les limites dynamiques de l'axe Y
    const dataPoints = chart.data.datasets[0]?.data || [];
    const limits = calculateAxisLimits(dataPoints, ORP_AXIS_MIN_DEFAULT, ORP_AXIS_MAX_DEFAULT);

    chart.data.datasets[1].data = build(limits.max);
    chart.data.datasets[2].data = build(ORP_MAX);
    chart.data.datasets[3].data = build(ORP_MIN);
    chart.data.datasets[4].data = build(limits.min);
  }

  function ensureOrpPlaceholderLabels(chart) {
    if (!chart) return;
    if (chart.data.labels.length === 0) {
      chart.data.labels = ["", ""];
    }
    if (chart.data.datasets[0] && chart.data.datasets[0].data.length === 0) {
      chart.data.datasets[0].data = [null, null];
    }
  }

  function clearReferenceDatasets(chart) {
    while (chart && chart.data.datasets.length > 1) {
      chart.data.datasets.pop();
    }
  }

  function pushPoint(chart, value, label) {
    chart.data.labels.push(label);
    chart.data.datasets[0].data.push(value);
    // Keep more points since we're loading historical data (up to 100 points)
    if (chart.data.labels.length > 100) {
      chart.data.labels.shift();
      chart.data.datasets[0].data.shift();
    }
    chart.update("none");
  }

  function initializePhReferenceLines(chart) {
    // Ajouter les zones de fond rouge et lignes de référence pour le pH
    if (chart && chart.data.datasets.length === 1) {
      chart.data.datasets.push(...buildPhReferenceDatasets());
    }
  }

  function initializeOrpReferenceLines(chart) {
    // Ajouter les zones de fond rouge et lignes de référence pour l'ORP
    if (chart && chart.data.datasets.length === 1) {
      chart.data.datasets.push(...buildOrpReferenceDatasets());
    }
  }

  async function loadHistoricalData(range = '24h') {
    try {
      const response = await authFetch(`/get-history?range=${range}`);
      if (!response.ok) throw new Error('Failed to load history');

      const data = await response.json();

      if (!data.history || data.history.length === 0) {
        debugLog('No historical data available');
        return;
      }

      // Clear existing chart data
      if (tempChart) {
        tempChart.data.labels = [];
        tempChart.data.datasets[0].data = [];
      }
      if (phChart) {
        phChart.data.labels = [];
        phChart.data.datasets[0].data = [];
        // Clear reference lines data if they exist
        if (phChart.data.datasets.length > 1) {
          for (let i = 1; i < phChart.data.datasets.length; i++) {
            phChart.data.datasets[i].data = [];
          }
        }
      }
      if (orpChart) {
        orpChart.data.labels = [];
        orpChart.data.datasets[0].data = [];
      }

      // Add historical data to charts
      data.history.forEach(point => {
        const timestamp = new Date(point.timestamp * 1000);
        const label = timestamp.toLocaleTimeString();

        if (point.temperature != null && !isNaN(point.temperature) && tempChart) {
          tempChart.data.labels.push(label);
          tempChart.data.datasets[0].data.push(point.temperature);
        }

        if (point.ph != null && !isNaN(point.ph) && phChart) {
          phChart.data.labels.push(label);
          phChart.data.datasets[0].data.push(Math.round(point.ph * 10) / 10);
        }

        if (point.orp != null && !isNaN(point.orp) && orpChart) {
          orpChart.data.labels.push(label);
          orpChart.data.datasets[0].data.push(Math.round(point.orp));
        }
      });

      // Update charts
      if (tempChart) tempChart.update('none');
      if (phChart) {
        ensurePhReferenceDatasets(phChart);
        ensurePhPlaceholderLabels(phChart);
        syncPhReferenceDatasets(phChart);
        phChart.update('none');
      }
      if (orpChart) {
        ensureOrpReferenceDatasets(orpChart);
        ensureOrpPlaceholderLabels(orpChart);
        syncOrpReferenceDatasets(orpChart);
        orpChart.update('none');
      }

      // Update main chart with active chart type data
      if (mainChart) {
        if (currentChartType === 'temperature' && tempChart) {
          mainChart.data.labels = [...tempChart.data.labels];
          mainChart.data.datasets[0].data = [...tempChart.data.datasets[0].data];
        } else if (currentChartType === 'ph' && phChart) {
          mainChart.data.labels = [...phChart.data.labels];
          mainChart.data.datasets[0].data = [...phChart.data.datasets[0].data];
          ensurePhReferenceDatasets(mainChart);
          ensurePhPlaceholderLabels(mainChart);
          syncPhReferenceDatasets(mainChart);
          mainChart.data.datasets[0].fill = false;
        } else if (currentChartType === 'orp' && orpChart) {
          mainChart.data.labels = [...orpChart.data.labels];
          mainChart.data.datasets[0].data = [...orpChart.data.datasets[0].data];
          ensureOrpReferenceDatasets(mainChart);
          ensureOrpPlaceholderLabels(mainChart);
          syncOrpReferenceDatasets(mainChart);
          mainChart.data.datasets[0].fill = false;
        }
        mainChart.update('none');
        updateYAxisOverlay(mainChart);
        updateMainChartScroll();
      }

      debugLog(`Loaded ${data.count} historical points (${range})`);
    } catch (error) {
      console.error('Error loading historical data:', error);
    }
  }

  let tempChart, phChart, orpChart;
  let mainChart; // Graphique unique avec onglets
  let currentChartType = 'temperature'; // Type de graphique actif

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

    // Update segmented buttons
    $$(".segmented__btn").forEach((b) => {
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
      temperature: "temperature_enabled",
      ph: "ph_enabled",
      orp: "orp_enabled"
    };
    const contentMap = {
      filtration: "filtration-content",
      lighting: "lighting-content",
      temperature: "temperature-content",
      ph: "ph-content",
      orp: "orp-content"
    };
    const dashboardCardMap = {
      filtration: "dashboard-filtration-card",
      lighting: "dashboard-lighting-card",
      temperature: "dashboard-temperature-card",
      ph: "dashboard-ph-card",
      orp: "dashboard-orp-card"
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

  function updatePhControls() {
    const enabled = $("#ph_enabled")?.checked ?? false;
    const target = $("#ph_target");
    const pump = $("#ph_pump");
    const limit = $("#ph_limit");
    const correctionType = $("#ph_correction_type");
    if (target) target.disabled = !enabled;
    if (pump) pump.disabled = !enabled;
    if (limit) limit.disabled = !enabled;
    if (correctionType) correctionType.disabled = !enabled;
    updateFeatureVisibility("ph");
  }

  function updateOrpControls() {
    const enabled = $("#orp_enabled")?.checked ?? false;
    const target = $("#orp_target");
    const pump = $("#orp_pump");
    const limit = $("#orp_limit");
    if (target) target.disabled = !enabled;
    if (pump) pump.disabled = !enabled;
    if (limit) limit.disabled = !enabled;
    updateFeatureVisibility("orp");
  }

  function collectConfig() {
    const mqttEnabled = $("#mqtt_enabled");
    const phToggle = $("#ph_enabled");
    const orpToggle = $("#orp_enabled");

    const portValue = parseInt($("#mqtt_port")?.value || "1883", 10);
    const phValue = parseFloat($("#ph_target")?.value || "7.2");
    const orpValue = parseFloat($("#orp_target")?.value || "650");

    const phPumpValue = parseInt($("#ph_pump")?.value || "1", 10);
    const orpPumpValue = parseInt($("#orp_pump")?.value || "2", 10);
    const pump1MaxDutyPct = parseInt($("#pump1_max_duty")?.value || "100", 10);
    const pump2MaxDutyPct = parseInt($("#pump2_max_duty")?.value || "100", 10);

    const phLimitValue = parseInt($("#ph_limit")?.value || "60", 10);
    const orpLimitValue = parseInt($("#orp_limit")?.value || "60", 10);
    const phDailyLimitValue = parseFloat($("#ph_daily_limit")?.value || "500");
    const orpDailyLimitValue = parseFloat($("#orp_daily_limit")?.value || "300");
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
      ph_enabled: phToggle?.checked === true,
      ph_pump: isNaN(phPumpValue) ? 1 : phPumpValue,
      orp_enabled: orpToggle?.checked === true,
      orp_pump: isNaN(orpPumpValue) ? 2 : orpPumpValue,
      pump1_max_duty_pct: isNaN(pump1MaxDutyPct) ? 100 : Math.min(100, Math.max(0, pump1MaxDutyPct)),
      pump2_max_duty_pct: isNaN(pump2MaxDutyPct) ? 100 : Math.min(100, Math.max(0, pump2MaxDutyPct)),
      ph_limit_seconds: isNaN(phLimitValue) ? 60 : phLimitValue,
      orp_limit_seconds: isNaN(orpLimitValue) ? 60 : orpLimitValue,
      max_ph_ml_per_day: isNaN(phDailyLimitValue) ? 500 : phDailyLimitValue,
      max_chlorine_ml_per_day: isNaN(orpDailyLimitValue) ? 300 : orpDailyLimitValue,
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

    $("#ph_enabled").checked = cfg.ph_enabled === true;
    $("#orp_enabled").checked = cfg.orp_enabled === true;
    updateFeatureVisibility("ph");
    updateFeatureVisibility("orp");

    $("#ph_pump").value = cfg.ph_pump === 2 ? "2" : "1";
    $("#orp_pump").value = cfg.orp_pump === 1 ? "1" : "2";

    const p1max = typeof cfg.pump1_max_duty_pct === "number" ? cfg.pump1_max_duty_pct : 100;
    const p2max = typeof cfg.pump2_max_duty_pct === "number" ? cfg.pump2_max_duty_pct : 100;
    if ($("#pump1_max_duty")) { $("#pump1_max_duty").value = p1max; $("#pump1_max_duty_value").textContent = String(p1max); }
    if ($("#pump2_max_duty")) { $("#pump2_max_duty").value = p2max; $("#pump2_max_duty_value").textContent = String(p2max); }
    if ($("#pump_max_flow_ml_per_min")) $("#pump_max_flow_ml_per_min").value = typeof cfg.pump_max_flow_ml_per_min === "number" ? cfg.pump_max_flow_ml_per_min : 90;

    $("#ph_limit").value = typeof cfg.ph_limit_seconds === "number" ? cfg.ph_limit_seconds : 60;
    $("#orp_limit").value = typeof cfg.orp_limit_seconds === "number" ? cfg.orp_limit_seconds : 60;
    $("#ph_daily_limit").value = typeof cfg.max_ph_ml_per_day === "number" ? cfg.max_ph_ml_per_day : 500;
    $("#orp_daily_limit").value = typeof cfg.max_chlorine_ml_per_day === "number" ? cfg.max_chlorine_ml_per_day : 300;
    $("#regulation_mode").value = cfg.regulation_mode || "pilote";
    if ($("#stabilization_delay_min") && cfg.stabilization_delay_min != null) {
      $("#stabilization_delay_min").value = cfg.stabilization_delay_min;
    }
    $("#ph_correction_type").value = cfg.ph_correction_type || "ph_minus";

    // pH calibration info
    const phCalValid = cfg.ph_cal_valid === true;
    const phCalDateStr = cfg.ph_calibration_date || "";
    const phCalTemp = cfg.ph_calibration_temp;

    const phCalibratedStatus = $("#ph_calibrated_status");
    const phCalDate = $("#ph_cal_date");

    if (phCalibratedStatus) {
      phCalibratedStatus.style.display = phCalValid ? "block" : "none";
    }

    const phCalDateHeader = $("#ph_cal_date_header");
    let phCalText = "Dernière calibration : —";
    if (phCalDateStr && phCalValid) {
      const d = new Date(phCalDateStr);
      phCalText = "Dernière calibration : " + d.toLocaleString("fr-FR");
      if (phCalTemp && !isNaN(phCalTemp)) phCalText += ` à ${phCalTemp.toFixed(1)}°C`;
    }
    if (phCalDate) phCalDate.textContent = phCalText;
    if (phCalDateHeader) phCalDateHeader.textContent = phCalText;

    // ORP calibration info
    const orpCalibrated = cfg.orp_calibration_date && cfg.orp_calibration_date !== "";
    const orpCalibratedStatus = $("#orp_calibrated_status");
    const orpCalDate = $("#orp_cal_date");

    if (orpCalibratedStatus) {
      orpCalibratedStatus.style.display = orpCalibrated ? "block" : "none";
    }

    const orpCalDateHeader = $("#orp_cal_date_header");
    let orpCalText = "Dernière calibration : —";
    if (orpCalibrated) {
      const d = new Date(cfg.orp_calibration_date);
      const ref = cfg.orp_calibration_reference;
      orpCalText = "Dernière calibration : " + d.toLocaleString("fr-FR");
      if (ref && ref > 0) orpCalText += ` (réf: ${ref.toFixed(0)} mV)`;
    }
    if (orpCalDate) orpCalDate.textContent = orpCalText;
    if (orpCalDateHeader) orpCalDateHeader.textContent = orpCalText;

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

    // Mettre à jour les badges et cartes status après chargement de la config
    updateSensorBadges();
    updateStatusCards();
    updateDetailSections();
    loadProductConfig(cfg);
  }

  // ---------- Calibration badges (Dashboard + chip) ----------
  async function checkCalibrationDate() {
    try {
      const res = await authFetch("/get-config");
      if (!res.ok) return;
      const config = await res.json();

      const threeMonthsInDays = 90;
      const now = new Date();

      const phBadgeEl = $("#ph-calibration-badge");
      const orpBadgeEl = $("#orp-calibration-badge");

      // pH
      if (phBadgeEl) {
        const phCalValid = config.ph_cal_valid === true;
        const phCalibrationDate = config.ph_calibration_date;

        let show = false;
        if (!phCalValid || !phCalibrationDate) show = true;
        else {
          let calDate = new Date(phCalibrationDate);
          if (isNaN(calDate.getTime())) {
            const ts = parseInt(phCalibrationDate);
            if (!isNaN(ts)) calDate = new Date(ts);
          }
          if (!calDate || isNaN(calDate.getTime())) show = true;
          else {
            const calDays = (now - calDate) / (1000 * 60 * 60 * 24);
            show = calDays > threeMonthsInDays;
          }
        }
        phBadgeEl.style.display = show ? "inline-flex" : "none";
      }

      // ORP
      if (orpBadgeEl) {
        const calibrationDate = config.orp_calibration_date;
        let show = false;
        if (!calibrationDate) show = true;
        else {
          const d = new Date(calibrationDate);
          const diffDays = (now - d) / (1000 * 60 * 60 * 24);
          show = diffDays > threeMonthsInDays;
        }
        orpBadgeEl.style.display = show ? "inline-flex" : "none";
      }

      // Global chip
      const chip = $("#calib-chip");
      if (chip) {
        const need = (phBadgeEl && phBadgeEl.style.display !== "none") || (orpBadgeEl && orpBadgeEl.style.display !== "none");
        chip.style.display = need ? "inline-flex" : "none";
      }

      // Dashboard calibration alerts
      const phDashAlert = $("#ph-calibration-alert");
      const orpDashAlert = $("#orp-calibration-alert");

      if (phDashAlert) {
        const phCalValid = config.ph_cal_valid === true;
        const phCalibrationDate = config.ph_calibration_date;
        let needsCal = false;
        if (!phCalValid || !phCalibrationDate) needsCal = true;
        else {
          let calDate = new Date(phCalibrationDate);
          if (isNaN(calDate.getTime())) {
            const ts = parseInt(phCalibrationDate);
            if (!isNaN(ts)) calDate = new Date(ts);
          }
          if (!calDate || isNaN(calDate.getTime())) needsCal = true;
          else {
            const calDays = (now - calDate) / (1000 * 60 * 60 * 24);
            needsCal = calDays > threeMonthsInDays;
          }
        }
        phDashAlert.style.display = needsCal ? "flex" : "none";
      }

      if (orpDashAlert) {
        const orpCalibrationDate = config.orp_calibration_date;
        let needsCal = false;
        if (!orpCalibrationDate) needsCal = true;
        else {
          const d = new Date(orpCalibrationDate);
          const diffDays = (now - d) / (1000 * 60 * 60 * 24);
          needsCal = diffDays > threeMonthsInDays;
        }
        orpDashAlert.style.display = needsCal ? "flex" : "none";
      }
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
    function setBadge(id, variant, text, link) {
      const el = $(`#${id}`);
      if (!el) return;
      if (!variant) {
        el.style.display = 'none';
        el.className = 'sensor-badge';
        el.innerHTML = '';
        return;
      }
      el.className = `sensor-badge sensor-badge--${variant}`;
      el.innerHTML = `<span>${text}</span><a href="${link}" class="sensor-badge__link">Corriger</a>`;
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
    if (!phVariant) {
      const rem = latestSensorData?.ph_remaining_ml;
      const thr = latestSensorData?.ph_alert_threshold_ml;
      if (rem != null && thr != null && rem <= thr && thr > 0) {
        phVariant = 'warning';
        phText = `Stock faible (${(rem / 1000).toFixed(1)} L)`;
      }
    }
    setBadge('ph-sensor-badge', phVariant, phText, '#/ph');

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
    if (!orpVariant) {
      const rem = latestSensorData?.orp_remaining_ml;
      const thr = latestSensorData?.orp_alert_threshold_ml;
      if (rem != null && thr != null && rem <= thr && thr > 0) {
        orpVariant = 'warning';
        orpText = `Stock faible (${(rem / 1000).toFixed(1)} L)`;
      }
    }
    setBadge('orp-sensor-badge', orpVariant, orpText, '#/orp');
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
    ["#detail-filtration-status", "#filtration-current-status"].forEach((sel) => {
      const badge = $(sel);
      if (badge) {
        badge.textContent = state.text;
        badge.className = 'state-badge ' + state.class;
      }
    });
  }

  function getFiltrationState(config, data) {
    const isRunning = filtrationRunningOverride !== null ? filtrationRunningOverride : (data && data.filtration_running);
    const temp = data && data.temperature;

    if (!config) return { text: 'Chargement...', class: 'state-badge--off' };

    // Température hors gel
    if (temp != null && temp < 5.0 && isRunning) {
      return { text: 'Hors gel', class: 'state-badge--warn' };
    }

    return {
      text: isRunning ? 'En marche' : 'Arrêtée',
      class: isRunning ? 'state-badge--ok' : 'state-badge--off'
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
    const limitSec  = sensor === "ph" ? (config.ph_limit_seconds || 0) : (config.orp_limit_seconds || 0);
    if (limitSec > 0 && usedMs >= limitSec * 1000) return "Limite de durée atteinte";

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

  // ========== GRAPHIQUE AVEC ONGLETS ==========
  function switchChartTab(chartType) {
    currentChartType = chartType;

    // Mettre à jour les onglets actifs
    const tabs = document.querySelectorAll('.chart-tab');
    tabs.forEach(tab => {
      const isActive = tab.dataset.chart === chartType;
      tab.classList.toggle('chart-tab--active', isActive);
      tab.setAttribute('aria-selected', isActive ? 'true' : 'false');
    });

    // Copier les données du graphique source vers le graphique principal
    const sourceChart = chartType === 'temperature' ? tempChart : chartType === 'ph' ? phChart : orpChart;

    if (mainChart && sourceChart) {
      mainChart.data.labels = [...sourceChart.data.labels];
      mainChart.data.datasets[0].data = [...sourceChart.data.datasets[0].data];

      // Mettre à jour la couleur et le label
      const colors = {
        temperature: '#4f8fff',
        ph: '#8b5cf6',
        orp: '#10b981'
      };
      const labels = {
        temperature: 'Température',
        ph: 'pH',
        orp: 'ORP'
      };

      mainChart.data.datasets[0].borderColor = colors[chartType];
      mainChart.data.datasets[0].backgroundColor = colors[chartType] + '20';
      mainChart.data.datasets[0].label = labels[chartType];
      mainChart.data.datasets[0].fill = chartType !== 'ph' && chartType !== 'orp';

      // Configurer l'échelle Y selon le type de graphique
      if (chartType === 'orp') {
        // ORP: échelle dynamique (min 500-900 mV) avec entiers uniquement
        const dataPoints = mainChart.data.datasets[0]?.data || [];
        const limits = calculateAxisLimits(dataPoints, ORP_AXIS_MIN_DEFAULT, ORP_AXIS_MAX_DEFAULT);

        mainChart.options.scales.y.min = limits.min;
        mainChart.options.scales.y.max = limits.max;
        mainChart.options.scales.y.ticks.callback = function(value) {
          if (Number.isInteger(value)) return value;
        };

        clearReferenceDatasets(mainChart);
        ensureOrpReferenceDatasets(mainChart);
        ensureOrpPlaceholderLabels(mainChart);
        syncOrpReferenceDatasets(mainChart);
      } else if (chartType === 'ph') {
        // pH: échelle dynamique (min 6-8) avec lignes de référence à 7.0 et 7.4
        const dataPoints = mainChart.data.datasets[0]?.data || [];
        const limits = calculateAxisLimits(dataPoints, PH_AXIS_MIN_DEFAULT, PH_AXIS_MAX_DEFAULT);

        mainChart.options.scales.y.min = limits.min;
        mainChart.options.scales.y.max = limits.max;
        delete mainChart.options.scales.y.ticks.callback;

        clearReferenceDatasets(mainChart);
        ensurePhReferenceDatasets(mainChart);
        ensurePhPlaceholderLabels(mainChart);
        syncPhReferenceDatasets(mainChart);
      } else {
        // Température: échelle automatique sans lignes de référence
        delete mainChart.options.scales.y.min;
        delete mainChart.options.scales.y.max;
        delete mainChart.options.scales.y.ticks.callback;

        // Supprimer les lignes de référence si elles existent
        clearReferenceDatasets(mainChart);
      }

      mainChart.update('none');
      updateYAxisOverlay(mainChart);
      updateMainChartScroll();
    }

    // Update accessible label on canvas and tabpanel
    const chartLabels = {
      temperature: 'Graphique Température (dernières 24 heures)',
      ph: 'Graphique pH (dernières 24 heures)',
      orp: 'Graphique ORP (dernières 24 heures)'
    };
    const tabIds = {
      temperature: 'tab-temperature',
      ph: 'tab-ph-chart',
      orp: 'tab-orp-chart'
    };
    const mainChartCanvas = document.getElementById('mainChart');
    if (mainChartCanvas) mainChartCanvas.setAttribute('aria-label', chartLabels[chartType] || '');
    const tabpanel = document.getElementById('chart-tabpanel');
    if (tabpanel) tabpanel.setAttribute('aria-labelledby', tabIds[chartType] || 'tab-temperature');
  }


  function bindChartTabs() {
    const tabs = Array.from(document.querySelectorAll('.chart-tab'));
    tabs.forEach((tab, i) => {
      tab.addEventListener('click', () => {
        switchChartTab(tab.dataset.chart);
      });
      tab.addEventListener('keydown', (e) => {
        let target = null;
        if (e.key === 'ArrowRight') {
          target = tabs[(i + 1) % tabs.length];
        } else if (e.key === 'ArrowLeft') {
          target = tabs[(i - 1 + tabs.length) % tabs.length];
        } else if (e.key === 'Home') {
          target = tabs[0];
        } else if (e.key === 'End') {
          target = tabs[tabs.length - 1];
        }
        if (target) {
          e.preventDefault();
          switchChartTab(target.dataset.chart);
          target.focus();
        }
      });
    });
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
      clearReferenceDatasets(phChart);
      phChart.update("none");
    }
    if (orpChart) {
      orpChart.data.labels = [];
      orpChart.data.datasets[0].data = [];
      clearReferenceDatasets(orpChart);
      orpChart.update("none");
    }
    if (mainChart) {
      mainChart.data.labels = [];
      mainChart.data.datasets[0].data = [];
      clearReferenceDatasets(mainChart);
      mainChart.update("none");
      updateYAxisOverlay(mainChart);
      updateMainChartScroll();
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

        const label = json.timestamp ? new Date(json.timestamp).toLocaleTimeString() : new Date().toLocaleTimeString();

        if (json.temperature != null && !isNaN(json.temperature) && tempChart) pushPoint(tempChart, json.temperature, label);
        if (!phCalibrationActive && json.ph != null && !isNaN(json.ph) && phChart) {
          pushPoint(phChart, Math.round(json.ph * 10) / 10, label);
          ensurePhReferenceDatasets(phChart);
          ensurePhPlaceholderLabels(phChart);
          syncPhReferenceDatasets(phChart);
          phChart.update('none');
        }
        if (!orpCalibrationActive && json.orp != null && !isNaN(json.orp) && orpChart) {
          pushPoint(orpChart, json.orp, label);
          ensureOrpReferenceDatasets(orpChart);
          ensureOrpPlaceholderLabels(orpChart);
          syncOrpReferenceDatasets(orpChart);
          orpChart.update('none');
        }

        // Mettre à jour le graphique principal avec les données du graphique source actif
        if (mainChart) {
          const sourceChart = currentChartType === 'temperature' ? tempChart : currentChartType === 'ph' ? phChart : orpChart;
          if (sourceChart) {
            mainChart.data.labels = [...sourceChart.data.labels];
            mainChart.data.datasets[0].data = [...sourceChart.data.datasets[0].data];
            if (currentChartType === 'ph') {
              ensurePhReferenceDatasets(mainChart);
              ensurePhPlaceholderLabels(mainChart);
              syncPhReferenceDatasets(mainChart);
              mainChart.data.datasets[0].fill = false;
            } else if (currentChartType === 'orp') {
              ensureOrpReferenceDatasets(mainChart);
              ensureOrpPlaceholderLabels(mainChart);
              syncOrpReferenceDatasets(mainChart);
              mainChart.data.datasets[0].fill = false;
            }
            mainChart.update('none');
            updateYAxisOverlay(mainChart);
            updateMainChartScroll();
          }
        }

        updateDashboardMetrics(json);
        updateClockBadge(json.time_synced === true);

        // Mettre à jour les badges et cartes status
        updateSensorBadges();
        updateStatusCards();
        updateProductUI(json);

        // Mettre à jour les sections détaillées
        updateDetailSections();

        // also update readouts in settings
        const phCurrentValue = $("#ph_current_value");
        if (phCurrentValue) {
          if (phCalibrationActive) {
            if (json.ph_voltage_mv != null && typeof json.ph_voltage_mv === "number" && !isNaN(json.ph_voltage_mv)) {
              phCurrentValue.textContent = json.ph_voltage_mv.toFixed(1) + " mV";
            } else {
              phCurrentValue.textContent = "--";
            }
          } else if (json.ph != null && typeof json.ph === "number" && !isNaN(json.ph)) {
            phCurrentValue.textContent = json.ph.toFixed(2);
          } else {
            phCurrentValue.textContent = "--";
          }
        }

        const orpCurrentValue = $("#orp_current_value");
        if (orpCurrentValue) {
          if (orpCalibrationActive) {
            if (json.orp_raw != null && typeof json.orp_raw === "number" && !isNaN(json.orp_raw)) {
              orpCurrentValue.textContent = Math.round(json.orp_raw) + " mV (brut)";
            } else {
              orpCurrentValue.textContent = "--";
            }
          } else if (json.orp != null && typeof json.orp === "number" && !isNaN(json.orp)) {
            orpCurrentValue.textContent = Math.round(json.orp) + " mV";
          } else {
            orpCurrentValue.textContent = "--";
          }
        }

        const tempCurrentValue = $("#temp_current_value");
        const tempCurrentRaw = $("#temp_current_raw");
        if (tempCurrentValue) {
          if (json.temperature != null && typeof json.temperature === "number" && !isNaN(json.temperature)) {
            tempCurrentValue.textContent = json.temperature.toFixed(1) + " °C";
          } else {
            tempCurrentValue.textContent = "--";
          }
        }
        if (tempCurrentRaw) {
          if (json.temperature_raw != null && typeof json.temperature_raw === "number" && !isNaN(json.temperature_raw)) {
            tempCurrentRaw.textContent = json.temperature_raw.toFixed(1) + " °C";
          } else {
            tempCurrentRaw.textContent = "--";
          }
        }

        // Mettre à jour le timestamp du dernier chargement
        lastSensorDataLoadTime = Date.now();
        perf?.end("success");
      } catch (e) {
        // Log detailed error information
        console.error('loadSensorData error:', {
          name: e?.name,
          message: e?.message,
          stack: e?.stack,
          type: typeof e,
          error: e
        });

        // Dégrader l'état rapidement en cas d'échec répété
        consecutiveFailures++;
        if (consecutiveFailures >= 2) {
          setNetStatus("bad", "Hors ligne");
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

        if (!latestSensorData || isNaN(latestSensorData.temperature_raw) || latestSensorData.temperature_raw == null) {
          alert("Aucune donnée capteur brute disponible (/data).");
          return;
        }

        const currentTempRaw = latestSensorData.temperature_raw;
        const offset = referenceValue - currentTempRaw;
        const calibrationDate = new Date().toISOString();

        let msg = `Calibration Température\n\n`;
        msg += `Brute: ${currentTempRaw.toFixed(2)} °C\n`;
        msg += `Réf: ${referenceValue.toFixed(1)} °C\n`;
        msg += `Offset: ${offset.toFixed(2)} °C\n\nAppliquer ?`;
        if (!confirm(msg)) {
          tempCalibrationStep = 0;
          updateTempCalibrationSteps();
          return;
        }

        startBtn.disabled = true;
        try {
          const cfg = collectConfig();
          cfg.temp_calibration_offset = offset;
          cfg.temp_calibration_date = calibrationDate;

          const ok = await sendConfig(cfg);
          if (!ok) throw new Error("Impossible d'enregistrer la configuration");

          alert(`Calibration température effectuée\nOffset: ${offset.toFixed(2)} °C`);
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
          latestSensorData.temperature_raw = currentTempRaw;

          // Dashboard
          const mTemp = $("#m-temp");
          if (mTemp) mTemp.textContent = correctedTemp.toFixed(1);
          const mTime = $("#m-time");
          if (mTime) mTime.textContent = nowLabel;

          // Page température
          const tempCurrentValue = $("#temp_current_value");
          if (tempCurrentValue) tempCurrentValue.textContent = correctedTemp.toFixed(1) + " °C";
          const tempCurrentRawEl = $("#temp_current_raw");
          if (tempCurrentRawEl) tempCurrentRawEl.textContent = currentTempRaw.toFixed(1) + " °C";

          // Graphe (optionnel, mais rend le changement visible immédiatement)
          if (tempChart) pushPoint(tempChart, correctedTemp, nowLabel);

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

  // ---------- pH calibration endpoints ----------
  let phCalibrationStep = 0; // 0=idle, 1=plunge7.0, 2=calibrate7.0, 3=plunge4.0, 4=calibrate4.0
  let phCalibrationActive = false;
  let orpCalibrationActive = false;
  let calibrationRefreshInterval = null;

  function startCalibrationRefresh() {
    // No-op: WebSocket pushes sensor data every 5s
  }

  function stopCalibrationRefresh() {
    if (calibrationRefreshInterval) {
      clearInterval(calibrationRefreshInterval);
      calibrationRefreshInterval = null;
    }
  }

  function updatePhCalibrationSteps() {
    const step1 = $("#ph_step1");
    const step2 = $("#ph_step2");
    const step3 = $("#ph_step3");
    const step4 = $("#ph_step4");
    const startBtn = $("#ph_cal_start_btn");
    const cancelBtn = $("#ph_cal_cancel_btn");

    // Reset all states
    [step1, step2, step3, step4].forEach(el => {
      el?.classList.remove("is-active", "is-completed");
    });

    if (phCalibrationStep === 0) {
      // Idle state
      if (startBtn) startBtn.textContent = "Commencer la calibration";
      if (cancelBtn) cancelBtn.style.display = "none";
    } else if (phCalibrationStep === 1) {
      // Step 1: Plunge in pH 7.0
      step1?.classList.add("is-active");
      if (startBtn) startBtn.textContent = "Étape suivante";
      if (cancelBtn) cancelBtn.style.display = "inline-block";
    } else if (phCalibrationStep === 2) {
      // Step 2: Calibrating pH 7.0
      step1?.classList.add("is-completed");
      step2?.classList.add("is-active");
      if (startBtn) startBtn.textContent = "Calibrer pH 7.0";
      if (cancelBtn) cancelBtn.style.display = "inline-block";
    } else if (phCalibrationStep === 3) {
      // Step 3: Plunge in pH 4.0
      step1?.classList.add("is-completed");
      step2?.classList.add("is-completed");
      step3?.classList.add("is-active");
      if (startBtn) startBtn.textContent = "Étape suivante";
      if (cancelBtn) cancelBtn.style.display = "inline-block";
    } else if (phCalibrationStep === 4) {
      // Step 4: Calibrating pH 4.0
      step1?.classList.add("is-completed");
      step2?.classList.add("is-completed");
      step3?.classList.add("is-completed");
      step4?.classList.add("is-active");
      if (startBtn) startBtn.textContent = "Calibrer pH 4.0";
      if (cancelBtn) cancelBtn.style.display = "inline-block";
    }
  }

  function bindPhCalibration() {
    const startBtn = $("#ph_cal_start_btn");
    const cancelBtn = $("#ph_cal_cancel_btn");
    const calibratedStatus = $("#ph_calibrated_status");

    cancelBtn?.addEventListener("click", () => {
      if (confirm("Annuler la calibration en cours ?")) {
        phCalibrationStep = 0;
        phCalibrationActive = false;
        stopCalibrationRefresh();
        updatePhCalibrationSteps();
        if (calibratedStatus) {
          // Re-show calibrated status if it was previously calibrated
          loadConfig();
        }
      }
    });

    startBtn?.addEventListener("click", async () => {
      if (phCalibrationStep === 0) {
        // Start calibration process
        phCalibrationStep = 1;
        phCalibrationActive = true;
        startCalibrationRefresh();
        updatePhCalibrationSteps();
        if (calibratedStatus) calibratedStatus.style.display = "none";
      } else if (phCalibrationStep === 1) {
        // Move to step 2
        phCalibrationStep = 2;
        updatePhCalibrationSteps();
      } else if (phCalibrationStep === 2) {
        // Calibrate pH 7.0
        if (!confirm("Sonde dans tampon pH 7.0 (stable) ? Continuer ?")) return;
        startBtn.disabled = true;

        try {
          const res = await authFetch("/calibrate_ph_neutral", { method: "POST" });
          const data = await res.json();
          if (!data.success) throw new Error(data.error || "Calibration échouée");

          alert("Calibration pH 7.0 effectuée.\nRincez la sonde puis plongez-la dans le tampon pH 4.0.");
          phCalibrationStep = 3;
          updatePhCalibrationSteps();
        } catch (err) {
          alert("Erreur calibration pH 7.0:\n" + err.message);
          phCalibrationStep = 0;
          updatePhCalibrationSteps();
        } finally {
          startBtn.disabled = false;
        }
      } else if (phCalibrationStep === 3) {
        // Move to step 4 (plunge in pH 4.0 done)
        phCalibrationStep = 4;
        updatePhCalibrationSteps();
      } else if (phCalibrationStep === 4) {
        // Calibrate pH 4.0
        if (!confirm("Sonde dans tampon pH 4.0 (stable) ? Continuer ?")) return;
        startBtn.disabled = true;

        try {
          const res = await authFetch("/calibrate_ph_acid", { method: "POST" });
          const data = await res.json();
          if (!data.success) throw new Error(data.error || "Calibration échouée");

          alert("Calibration pH 4.0 effectuée. Calibration complète !");

          // Mark all steps as completed
          $("#ph_step1")?.classList.add("is-completed");
          $("#ph_step2")?.classList.add("is-completed");
          $("#ph_step3")?.classList.add("is-completed");
          $("#ph_step4")?.classList.add("is-completed");
          $("#ph_step4")?.classList.remove("is-active");

          // Show calibrated status
          if (calibratedStatus) calibratedStatus.style.display = "block";

          // Reset to idle
          phCalibrationStep = 0;
          phCalibrationActive = false;
          stopCalibrationRefresh();

          // Invalider le timestamp pour forcer le rechargement
          lastSensorDataLoadTime = 0;

          // Refresh réel depuis l'ESP (valeurs calculées après calibration)
          await loadConfig();
          await checkCalibrationDate();
          updatePhCalibrationSteps();

          // Recharger les données après un court délai pour se resynchroniser avec le backend
          setTimeout(async () => {
            await loadSensorData({ force: true });
          }, 2000);
        } catch (err) {
          alert("Erreur calibration pH 4.0:\n" + err.message);
          phCalibrationStep = 0;
          phCalibrationActive = false;
          stopCalibrationRefresh();
          updatePhCalibrationSteps();
        } finally {
          startBtn.disabled = false;
        }
      }
    });

    // Initialize
    updatePhCalibrationSteps();
  }

  // ---------- ORP calibration ----------
  let orpCalibrationStep1pt = 0; // 0=idle, 1=plunge, 2=enterRef, 3=calibrate
  let orpCalibrationStep2pt = 0; // 0=idle, 1-6=steps
  let orpPoint1Measured = null;
  let orpPoint1Reference = null;

  function updateOrpCalibrationSteps1pt() {
    const steps = [$("#orp_step1_1pt"), $("#orp_step2_1pt"), $("#orp_step3_1pt")];
    const startBtn = $("#orp_cal_start_btn_1pt");
    const cancelBtn = $("#orp_cal_cancel_btn_1pt");
    const refInput = $("#orp_reference_value_1pt");

    steps.forEach(el => el?.classList.remove("is-active", "is-completed"));

    if (orpCalibrationStep1pt === 0) {
      if (startBtn) startBtn.textContent = "Commencer la calibration";
      if (cancelBtn) cancelBtn.style.display = "none";
      if (refInput) refInput.disabled = true;
    } else if (orpCalibrationStep1pt === 1) {
      steps[0]?.classList.add("is-active");
      if (startBtn) startBtn.textContent = "Étape suivante";
      if (cancelBtn) cancelBtn.style.display = "inline-block";
      if (refInput) refInput.disabled = true;
    } else if (orpCalibrationStep1pt === 2) {
      steps[0]?.classList.add("is-completed");
      steps[1]?.classList.add("is-active");
      if (startBtn) startBtn.textContent = "Étape suivante";
      if (cancelBtn) cancelBtn.style.display = "inline-block";
      if (refInput) refInput.disabled = false;
    } else if (orpCalibrationStep1pt === 3) {
      steps[0]?.classList.add("is-completed");
      steps[1]?.classList.add("is-completed");
      steps[2]?.classList.add("is-active");
      if (startBtn) startBtn.textContent = "Calibrer";
      if (cancelBtn) cancelBtn.style.display = "inline-block";
      if (refInput) refInput.disabled = true;
    }
  }

  function updateOrpCalibrationSteps2pt() {
    const steps = [
      $("#orp_step1_2pt"), $("#orp_step2_2pt"), $("#orp_step3_2pt"),
      $("#orp_step4_2pt"), $("#orp_step5_2pt"), $("#orp_step6_2pt")
    ];
    const startBtn = $("#orp_cal_start_btn_2pt");
    const cancelBtn = $("#orp_cal_cancel_btn_2pt");
    const ref1Input = $("#orp_ref1");
    const ref2Input = $("#orp_ref2");
    const statusHint = $("#orp_2pt_status");

    steps.forEach(el => el?.classList.remove("is-active", "is-completed"));

    if (orpCalibrationStep2pt === 0) {
      if (startBtn) startBtn.textContent = "Commencer la calibration";
      if (cancelBtn) cancelBtn.style.display = "none";
      if (ref1Input) ref1Input.disabled = true;
      if (ref2Input) ref2Input.disabled = true;
      if (statusHint) statusHint.style.display = "none";
    } else if (orpCalibrationStep2pt === 1) {
      steps[0]?.classList.add("is-active");
      if (startBtn) startBtn.textContent = "Étape suivante";
      if (cancelBtn) cancelBtn.style.display = "inline-block";
    } else if (orpCalibrationStep2pt === 2) {
      steps[0]?.classList.add("is-completed");
      steps[1]?.classList.add("is-active");
      if (startBtn) startBtn.textContent = "Étape suivante";
      if (ref1Input) ref1Input.disabled = false;
    } else if (orpCalibrationStep2pt === 3) {
      steps[0]?.classList.add("is-completed");
      steps[1]?.classList.add("is-completed");
      steps[2]?.classList.add("is-active");
      if (startBtn) startBtn.textContent = "Mémoriser point 1";
      if (ref1Input) ref1Input.disabled = true;
    } else if (orpCalibrationStep2pt === 4) {
      for (let i = 0; i < 3; i++) steps[i]?.classList.add("is-completed");
      steps[3]?.classList.add("is-active");
      if (startBtn) startBtn.textContent = "Étape suivante";
      if (statusHint) statusHint.style.display = "block";
    } else if (orpCalibrationStep2pt === 5) {
      for (let i = 0; i < 4; i++) steps[i]?.classList.add("is-completed");
      steps[4]?.classList.add("is-active");
      if (startBtn) startBtn.textContent = "Étape suivante";
      if (ref2Input) ref2Input.disabled = false;
    } else if (orpCalibrationStep2pt === 6) {
      for (let i = 0; i < 5; i++) steps[i]?.classList.add("is-completed");
      steps[5]?.classList.add("is-active");
      if (startBtn) startBtn.textContent = "Calibrer (2 points)";
      if (ref2Input) ref2Input.disabled = true;
    }
  }

  function bindOrpCalibration() {
    const typeSel = $("#orp_cal_type");
    const cal1 = $("#orp_cal_1point");
    const cal2 = $("#orp_cal_2points");
    const calibratedStatus = $("#orp_calibrated_status");

    // Type selector
    typeSel?.addEventListener("change", () => {
      const is2pt = typeSel.value === "2points";
      if (cal1) cal1.style.display = is2pt ? "none" : "block";
      if (cal2) cal2.style.display = is2pt ? "block" : "none";

      // Reset states
      orpCalibrationStep1pt = 0;
      orpCalibrationStep2pt = 0;
      orpCalibrationActive = false;
      stopCalibrationRefresh();
      orpPoint1Measured = null;
      orpPoint1Reference = null;
      updateOrpCalibrationSteps1pt();
      updateOrpCalibrationSteps2pt();
    });

    // 1-point calibration
    const startBtn1pt = $("#orp_cal_start_btn_1pt");
    const cancelBtn1pt = $("#orp_cal_cancel_btn_1pt");
    const refInput1pt = $("#orp_reference_value_1pt");

    cancelBtn1pt?.addEventListener("click", () => {
      if (confirm("Annuler la calibration en cours ?")) {
        orpCalibrationStep1pt = 0;
        orpCalibrationActive = false;
        stopCalibrationRefresh();
        if (refInput1pt) refInput1pt.value = "";
        updateOrpCalibrationSteps1pt();
        loadConfig();
      }
    });

    startBtn1pt?.addEventListener("click", async () => {
      if (orpCalibrationStep1pt === 0) {
        orpCalibrationStep1pt = 1;
        orpCalibrationActive = true;
        startCalibrationRefresh();
        updateOrpCalibrationSteps1pt();
        if (calibratedStatus) calibratedStatus.style.display = "none";
      } else if (orpCalibrationStep1pt === 1) {
        orpCalibrationStep1pt = 2;
        updateOrpCalibrationSteps1pt();
        if (refInput1pt) refInput1pt.focus();
      } else if (orpCalibrationStep1pt === 2) {
        const referenceValue = parseFloat(refInput1pt.value);
        if (isNaN(referenceValue) || referenceValue < 0 || referenceValue > 1000) {
          alert("Valeur de référence ORP invalide (0-1000 mV)");
          return;
        }
        orpCalibrationStep1pt = 3;
        updateOrpCalibrationSteps1pt();
      } else if (orpCalibrationStep1pt === 3) {
        const referenceValue = parseFloat(refInput1pt.value);
        if (isNaN(referenceValue) || referenceValue < 0 || referenceValue > 1000) {
          alert("Valeur de référence ORP invalide (0-1000 mV)");
          orpCalibrationStep1pt = 2;
          updateOrpCalibrationSteps1pt();
          return;
        }

        if (!latestSensorData || isNaN(latestSensorData.orp_raw) || latestSensorData.orp_raw == null) {
          alert("Aucune donnée capteur disponible (/data).");
          return;
        }

        const currentOrpRaw = latestSensorData.orp_raw;
        const offset = referenceValue - currentOrpRaw;
        const calibrationDate = new Date().toISOString();

        let msg = `Calibration ORP (1 point)\n\n`;
        msg += `Brut: ${currentOrpRaw.toFixed(1)} mV\n`;
        msg += `Réf: ${referenceValue.toFixed(0)} mV\n`;
        msg += `Offset: ${offset.toFixed(1)} mV\n\nAppliquer ?`;
        if (!confirm(msg)) {
          orpCalibrationStep1pt = 0;
          updateOrpCalibrationSteps1pt();
          return;
        }

        startBtn1pt.disabled = true;
        try {
          const cfg = collectConfig();
          cfg.orp_calibration_offset = offset;
          cfg.orp_calibration_slope = 1.0;
          cfg.orp_calibration_date = calibrationDate;
          cfg.orp_calibration_reference = referenceValue;

          const ok = await sendConfig(cfg);
          if (!ok) throw new Error("Impossible d'enregistrer la configuration");

          alert(`Calibration ORP effectuée\nOffset: ${offset.toFixed(1)} mV`);
          $("#orp_step1_1pt")?.classList.add("is-completed");
          $("#orp_step2_1pt")?.classList.add("is-completed");
          $("#orp_step3_1pt")?.classList.add("is-completed");
          $("#orp_step3_1pt")?.classList.remove("is-active");
          if (calibratedStatus) calibratedStatus.style.display = "block";

          orpCalibrationStep1pt = 0;
          orpCalibrationActive = false;
          stopCalibrationRefresh();
          if (refInput1pt) refInput1pt.value = "";

          // Invalider le timestamp pour forcer le rechargement
          lastSensorDataLoadTime = 0;

          // Refresh réel depuis l'ESP
          await loadConfig();
          await checkCalibrationDate();
          updateOrpCalibrationSteps1pt();

          // Recharger les données après un court délai pour se resynchroniser avec le backend
          setTimeout(async () => {
            await loadSensorData({ force: true });
          }, 2000);
        } catch (err) {
          alert("Erreur calibration ORP:\n" + err.message);
          orpCalibrationStep1pt = 0;
          orpCalibrationActive = false;
          stopCalibrationRefresh();
          updateOrpCalibrationSteps1pt();
        } finally {
          startBtn1pt.disabled = false;
        }
      }
    });

    // 2-point calibration
    const startBtn2pt = $("#orp_cal_start_btn_2pt");
    const cancelBtn2pt = $("#orp_cal_cancel_btn_2pt");
    const ref1 = $("#orp_ref1");
    const ref2 = $("#orp_ref2");

    cancelBtn2pt?.addEventListener("click", () => {
      if (confirm("Annuler la calibration en cours ?")) {
        orpCalibrationStep2pt = 0;
        orpCalibrationActive = false;
        stopCalibrationRefresh();
        orpPoint1Measured = null;
        orpPoint1Reference = null;
        if (ref1) ref1.value = "";
        if (ref2) ref2.value = "";
        updateOrpCalibrationSteps2pt();
        loadConfig();
      }
    });

    startBtn2pt?.addEventListener("click", async () => {
      if (orpCalibrationStep2pt === 0) {
        orpCalibrationStep2pt = 1;
        orpCalibrationActive = true;
        startCalibrationRefresh();
        updateOrpCalibrationSteps2pt();
        if (calibratedStatus) calibratedStatus.style.display = "none";
      } else if (orpCalibrationStep2pt === 1) {
        orpCalibrationStep2pt = 2;
        updateOrpCalibrationSteps2pt();
        if (ref1) ref1.focus();
      } else if (orpCalibrationStep2pt === 2) {
        const r1 = parseFloat(ref1.value);
        if (isNaN(r1) || r1 < 0 || r1 > 1000) {
          alert("Réf 1 invalide (0-1000 mV)");
          return;
        }
        orpCalibrationStep2pt = 3;
        updateOrpCalibrationSteps2pt();
      } else if (orpCalibrationStep2pt === 3) {
        // Memorize point 1
        const r1 = parseFloat(ref1.value);
        if (isNaN(r1) || r1 < 0 || r1 > 1000) {
          alert("Réf 1 invalide (0-1000 mV)");
          orpCalibrationStep2pt = 2;
          updateOrpCalibrationSteps2pt();
          return;
        }
        if (!latestSensorData || isNaN(latestSensorData.orp_raw) || latestSensorData.orp_raw == null) {
          alert("Aucune donnée capteur disponible (/data).");
          return;
        }
        orpPoint1Measured = latestSensorData.orp_raw;
        orpPoint1Reference = r1;
        orpCalibrationStep2pt = 4;
        updateOrpCalibrationSteps2pt();
      } else if (orpCalibrationStep2pt === 4) {
        orpCalibrationStep2pt = 5;
        updateOrpCalibrationSteps2pt();
        if (ref2) ref2.focus();
      } else if (orpCalibrationStep2pt === 5) {
        const r2 = parseFloat(ref2.value);
        if (isNaN(r2) || r2 < 0 || r2 > 1000) {
          alert("Réf 2 invalide (0-1000 mV)");
          return;
        }
        orpCalibrationStep2pt = 6;
        updateOrpCalibrationSteps2pt();
      } else if (orpCalibrationStep2pt === 6) {
        // Calibrate 2 points
        const r2 = parseFloat(ref2.value);
        if (isNaN(r2) || r2 < 0 || r2 > 1000) {
          alert("Réf 2 invalide (0-1000 mV)");
          orpCalibrationStep2pt = 5;
          updateOrpCalibrationSteps2pt();
          return;
        }
        if (!latestSensorData || !orpPoint1Measured || !orpPoint1Reference) {
          alert("Point 1 manquant ou données capteur indisponibles.");
          return;
        }

        const measured2 = latestSensorData.orp_raw;
        if (isNaN(measured2) || measured2 == null) {
          alert("orp_raw indisponible.");
          return;
        }

        if (Math.abs(orpPoint1Measured - measured2) < 10) {
          alert("⚠️ Points de mesure trop proches. Utilisez 2 solutions plus espacées.");
          return;
        }
        if (Math.abs(orpPoint1Reference - r2) < 50) {
          alert("⚠️ Références trop proches. Utilisez 2 solutions plus espacées.");
          return;
        }

        const slope = (r2 - orpPoint1Reference) / (measured2 - orpPoint1Measured);
        const offset = orpPoint1Reference - (orpPoint1Measured * slope);
        const calibrationDate = new Date().toISOString();

        let msg = `Calibration ORP 2 points\n\n`;
        msg += `P1: ${orpPoint1Measured.toFixed(1)} → ${orpPoint1Reference.toFixed(0)}\n`;
        msg += `P2: ${measured2.toFixed(1)} → ${r2.toFixed(0)}\n\n`;
        msg += `Slope: ${slope.toFixed(3)}\nOffset: ${offset.toFixed(1)}\n\nAppliquer ?`;
        if (!confirm(msg)) {
          orpCalibrationStep2pt = 0;
          updateOrpCalibrationSteps2pt();
          return;
        }

        startBtn2pt.disabled = true;
        try {
          const cfg = collectConfig();
          cfg.orp_calibration_slope = slope;
          cfg.orp_calibration_offset = offset;
          cfg.orp_calibration_date = calibrationDate;
          cfg.orp_calibration_reference = r2;

          const ok = await sendConfig(cfg);
          if (!ok) throw new Error("Impossible d'enregistrer la configuration");

          alert(`Calibration ORP 2 points OK\nSlope: ${slope.toFixed(3)}\nOffset: ${offset.toFixed(1)}`);
          for (let i = 1; i <= 6; i++) {
            $(`#orp_step${i}_2pt`)?.classList.add("is-completed");
            $(`#orp_step${i}_2pt`)?.classList.remove("is-active");
          }
          if (calibratedStatus) calibratedStatus.style.display = "block";

          orpCalibrationStep2pt = 0;
          orpCalibrationActive = false;
          stopCalibrationRefresh();
          orpPoint1Measured = null;
          orpPoint1Reference = null;
          if (ref1) ref1.value = "";
          if (ref2) ref2.value = "";

          // Invalider le timestamp pour forcer le rechargement
          lastSensorDataLoadTime = 0;

          // Refresh réel depuis l'ESP
          await loadConfig();
          await checkCalibrationDate();
          updateOrpCalibrationSteps2pt();

          // Recharger les données après un court délai pour se resynchroniser avec le backend
          setTimeout(async () => {
            await loadSensorData({ force: true });
          }, 2000);
        } catch (err) {
          alert("Erreur calibration ORP:\n" + err.message);
          orpCalibrationStep2pt = 0;
          orpCalibrationActive = false;
          stopCalibrationRefresh();
          updateOrpCalibrationSteps2pt();
        } finally {
          startBtn2pt.disabled = false;
        }
      }
    });

    // Initialize
    updateOrpCalibrationSteps1pt();
    updateOrpCalibrationSteps2pt();
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

      const lines = Array.isArray(data) ? data : data.logs || [];

      if (incremental) {
        // En mode incrémental, ajouter les nouveaux logs reçus
        if (lines.length > 0) {
          allLogEntries = [...allLogEntries, ...lines];

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

  function bindLogs() {
    $("#refresh_logs_btn")?.addEventListener("click", () => loadLogs(true, false));

    $("#clear_logs_display_btn")?.addEventListener("click", () => {
      const content = $("#logs_content");
      if (content) content.textContent = "";
      allLogEntries = [];
      lastLogTimestamp = 0;
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
      ? ["ph_enabled", "ph_target", "ph_limit", "ph_daily_limit", "ph_correction_type"]
      : ["orp_enabled", "orp_target", "orp_limit", "orp_daily_limit"];
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

    // pH / ORP regulation — sauvegarde manuelle via bouton
    $("#ph_enabled")?.addEventListener("change", () => updatePhControls());
    $("#orp_enabled")?.addEventListener("change", () => updateOrpControls());
    ["ph_target", "ph_limit", "ph_daily_limit", "ph_correction_type"].forEach((id) => $(`#${id}`)?.addEventListener("change", () => updatePhControls()));
    ["orp_target", "orp_limit", "orp_daily_limit"].forEach((id) => $(`#${id}`)?.addEventListener("change", () => updateOrpControls()));
    bindRegulationSave("ph",  "#ph_regulation_save_btn");
    bindRegulationSave("orp", "#orp_regulation_save_btn");
    // Onglet Régulation : save dédié
    $("#regulation_save_btn")?.addEventListener("click", async () => {
      const mode = $("#regulation_mode")?.value || "pilote";
      const delay = parseInt($("#stabilization_delay_min")?.value ?? "5", 10);
      const ok = await sendConfig({
        regulation_mode: mode,
        stabilization_delay_min: isNaN(delay) ? 5 : Math.min(60, Math.max(0, delay)),
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

    // Development panel - Sensor logs
    $("#sensor_logs_enabled")?.addEventListener("change", save);
  }

  // ---------- Sidebar helpers ----------
  function openSidebar() {
    const sidebar = $("#sidebar");
    const overlay = $("#sidebar-overlay");
    const burgerOpen = $("#burger-open");
    const burgerClose = $("#burger");
    sidebar?.classList.add("is-open");
    overlay?.classList.add("is-visible");
    overlay?.removeAttribute("aria-hidden");
    burgerOpen?.setAttribute("aria-expanded", "true");
    burgerClose?.setAttribute("aria-expanded", "true");
    burgerClose?.focus();
  }

  function closeSidebar() {
    const sidebar = $("#sidebar");
    const overlay = $("#sidebar-overlay");
    const burgerOpen = $("#burger-open");
    const burgerClose = $("#burger");
    sidebar?.classList.remove("is-open");
    overlay?.classList.remove("is-visible");
    overlay?.setAttribute("aria-hidden", "true");
    burgerOpen?.setAttribute("aria-expanded", "false");
    burgerClose?.setAttribute("aria-expanded", "false");
  }

  // ---------- UI bindings ----------
  function bindUI() {
    // mobile burger (open from main)
    $("#burger-open")?.addEventListener("click", openSidebar);

    // mobile burger (close from sidebar)
    $("#burger")?.addEventListener("click", closeSidebar);

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
    const segmentedBtns = $$(".segmented__btn");
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
        const res = await authFetch("/auth/change-password", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({
            currentPassword: currentPassword,
            newPassword: newPassword
          })
        });

        if (!res.ok) {
          const error = await res.json();
          alert("Erreur: " + (error.error || "Échec du changement de mot de passe"));
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
      // Initialiser les lignes de référence du pH dès la création
      initializePhReferenceLines(phChart);
    }
    if (orpChartCanvas) {
      orpChart = createLineChart(orpChartCanvas, "#10b981", "ORP", {
        integerOnly: true,
        yMin: ORP_AXIS_MIN_DEFAULT,
        yMax: ORP_AXIS_MAX_DEFAULT,
        fill: false
      });
      // Initialiser les lignes de référence de l'ORP dès la création
      initializeOrpReferenceLines(orpChart);
    }

    // Nouveau graphique principal avec onglets
    const mainChartCanvas = $("#mainChart");
    if (mainChartCanvas) {
      mainChart = createLineChart(mainChartCanvas, "#4f8fff", "Température", {
        hideYAxis: true,
        showYAxisGrid: true
      });
      bindChartTabs();
      bindChartScroll();
      window.addEventListener('resize', updateMainChartScroll);
    }

    bindUI();
    bindHistoryBackup();
    bindDetailActions();
    bindAutosave();
    bindPhCalibration();
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

    // WebSocket : démarre immédiatement pour ne pas bloquer sur les fetches HTTP initiaux
    setNetStatus("mid", "Connexion…");
    initWebSocket();

    // initial loads (en parallèle avec la connexion WS)
    const configPerf = debugStart("loadConfig");
    await loadConfig().catch(() => {});
    configPerf?.end();

    // Load historical data to populate charts
    const historyPerf = debugStart("loadHistoricalData");
    await loadHistoricalData('24h').catch(() => {});
    historyPerf?.end();

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

    if (statusBadge) {
      statusBadge.textContent = isOn ? "Allumé" : "Éteint";
      statusBadge.className = isOn ? "state-badge state-badge--ok" : "state-badge state-badge--off";
    }

    if (detailStatusBadge) {
      detailStatusBadge.textContent = isOn ? "Allumé" : "Éteint";
      detailStatusBadge.className = isOn ? "state-badge state-badge--ok" : "state-badge state-badge--off";
    }
  }

  window.addEventListener("DOMContentLoaded", init);
})();
