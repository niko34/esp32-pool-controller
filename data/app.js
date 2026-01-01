(() => {
  "use strict";

  // ---------- Utils ----------
  const $ = (sel, root = document) => root.querySelector(sel);
  const $$ = (sel, root = document) => Array.from(root.querySelectorAll(sel));
  const clamp = (n, a, b) => Math.max(a, Math.min(b, n));
  const DEBUG = true;

  function debugLog(msg) {
    if (!DEBUG) return;
    console.log(`[dbg] ${msg}`);
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
  }

  // ---------- Router ----------
  function getRoute() {
    const hash = window.location.hash || "#/dashboard";
    const clean = hash.replace(/^#/, "");
    const parts = clean.split("/").filter(Boolean);
    if (parts.length === 0) return { view: "/dashboard" };
    if (parts[0] === "settings") return { view: "/settings", sub: parts[1] || "wifi" };
    return { view: `/${parts[0]}` };
  }

  function setActiveNav(routeObj) {
    const routeKey = routeObj.view;

    $$(".nav__item").forEach((a) => {
      const r = a.getAttribute("data-route");
      a.classList.toggle("is-active", r === routeKey);
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
    $(".sidebar")?.classList.remove("is-open");
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
      ticks: { color: '#6b7280' },
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
            ticks: { maxRotation: 0, minRotation: 0, color: '#6b7280' },
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
        console.log('No historical data available');
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

      console.log(`Loaded ${data.count} historical points (${range})`);
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
    $$(".segmented__btn").forEach((b) => b.classList.remove("is-active"));
    $(`.segmented__btn[data-settings-tab="${panelKey}"]`)?.classList.add("is-active");

    // Update WiFi display when WiFi panel is shown
    if (panelKey === "wifi") {
      updateWiFiDisplay();
    }
  }

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
    const mode = $("#filtration_mode")?.value || "auto";
    const start = $("#filtration_start");
    const end = $("#filtration_end");
    if (!start || !end) return;

    if (mode === "manual") {
      start.disabled = false;
      end.disabled = false;
    } else {
      start.disabled = true;
      end.disabled = true;
    }
  }

  function updateTimeControls(current) {
    const useNtp = $("#time_use_ntp")?.checked ?? true;
    const ntp = $("#time_ntp_server");
    const timeValue = $("#time_value");

    // Si NTP activé: serveur NTP actif, champ heure en lecture seule avec heure du serveur
    // Si NTP désactivé: serveur NTP grisé, champ heure modifiable
    if (ntp) ntp.disabled = !useNtp;
    if (timeValue) {
      timeValue.readOnly = useNtp;
      if (current) {
        timeValue.value = current;
      }
    }
  }

  function updatePhControls() {
    const enabled = $("#ph_enabled")?.checked ?? false;
    const target = $("#ph_target");
    const pump = $("#ph_pump");
    const limit = $("#ph_limit");
    if (target) target.disabled = !enabled;
    if (pump) pump.disabled = !enabled;
    if (limit) limit.disabled = !enabled;
  }

  function updateOrpControls() {
    const enabled = $("#orp_enabled")?.checked ?? false;
    const target = $("#orp_target");
    const pump = $("#orp_pump");
    const limit = $("#orp_limit");
    if (target) target.disabled = !enabled;
    if (pump) pump.disabled = !enabled;
    if (limit) limit.disabled = !enabled;
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

    const phLimitValue = parseInt($("#ph_limit")?.value || "60", 10);
    const orpLimitValue = parseInt($("#orp_limit")?.value || "60", 10);

    const timeUseNtp = $("#time_use_ntp")?.checked ?? true;
    const timeNtpServer = $("#time_ntp_server")?.value || "pool.ntp.org";
    const timeTimezone = $("#time_timezone")?.value || "europe_paris";
    const timeValue = $("#time_value")?.value || "";

    const filtrationMode = $("#filtration_mode")?.value || "auto";
    const filtrationStart = $("#filtration_start")?.value || "08:00";
    const filtrationEnd = $("#filtration_end")?.value || "20:00";

    const lightingScheduleMode = $("#lighting_schedule_mode")?.value || "disabled";
    const lightingStartTime = $("#lighting_start_time")?.value || "20:00";
    const lightingEndTime = $("#lighting_end_time")?.value || "23:00";

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
      ph_limit_seconds: isNaN(phLimitValue) ? 60 : phLimitValue,
      orp_limit_seconds: isNaN(orpLimitValue) ? 60 : orpLimitValue,
      time_use_ntp: timeUseNtp,
      ntp_server: timeNtpServer,
      manual_time: timeValue,
      timezone_id: timeTimezone,
      filtration_mode: filtrationMode,
      filtration_start: filtrationStart,
      filtration_end: filtrationEnd,
      lighting_schedule_enabled: lightingScheduleMode === "enabled",
      lighting_start_time: lightingStartTime,
      lighting_end_time: lightingEndTime,
    };
  }

  async function loadConfig() {
    const res = await authFetch("/get-config");
    const cfg = await res.json();

    // Stocker la config globalement pour les alertes et cartes status
    window._config = cfg;

    $("#mqtt_server").value = cfg.server || "";
    $("#mqtt_port").value = cfg.port || 1883;
    $("#mqtt_topic").value = cfg.topic || "";
    $("#mqtt_username").value = cfg.username || "";
    $("#mqtt_password").value = cfg.password || "";
    $("#mqtt_enabled").checked = cfg.enabled !== false;

    updateMqttStatusIndicator(cfg.enabled, cfg.mqtt_connected);

    $("#ph_target").value = typeof cfg.ph_target === "number" ? cfg.ph_target : 7.2;
    $("#orp_target").value = typeof cfg.orp_target === "number" ? cfg.orp_target : 650;

    $("#ph_enabled").checked = cfg.ph_enabled === true;
    $("#orp_enabled").checked = cfg.orp_enabled === true;

    $("#ph_pump").value = cfg.ph_pump === 2 ? "2" : "1";
    $("#orp_pump").value = cfg.orp_pump === 1 ? "1" : "2";

    $("#ph_limit").value = typeof cfg.ph_limit_seconds === "number" ? cfg.ph_limit_seconds : 60;
    $("#orp_limit").value = typeof cfg.orp_limit_seconds === "number" ? cfg.orp_limit_seconds : 60;

    // pH calibration info
    const phCalValid = cfg.ph_cal_valid === true;
    const phCalDateStr = cfg.ph_calibration_date || "";
    const phCalTemp = cfg.ph_calibration_temp;

    const phCalibratedStatus = $("#ph_calibrated_status");
    const phCalDate = $("#ph_cal_date");

    if (phCalibratedStatus) {
      phCalibratedStatus.style.display = phCalValid ? "block" : "none";
    }

    if (phCalDate) {
      if (phCalDateStr && phCalValid) {
        const d = new Date(phCalDateStr);
        let t = "Dernière calibration : " + d.toLocaleString("fr-FR");
        if (phCalTemp && !isNaN(phCalTemp)) t += ` à ${phCalTemp.toFixed(1)}°C`;
        phCalDate.textContent = t;
      } else {
        phCalDate.textContent = "Dernière calibration : —";
      }
    }

    // ORP calibration info
    const orpCalibrated = cfg.orp_calibration_date && cfg.orp_calibration_date !== "";
    const orpCalibratedStatus = $("#orp_calibrated_status");
    const orpCalDate = $("#orp_cal_date");

    if (orpCalibratedStatus) {
      orpCalibratedStatus.style.display = orpCalibrated ? "block" : "none";
    }

    if (orpCalDate) {
      if (orpCalibrated) {
        const d = new Date(cfg.orp_calibration_date);
        const ref = cfg.orp_calibration_reference;
        let t = "Dernière calibration : " + d.toLocaleString("fr-FR");
        if (ref && ref > 0) t += ` (réf: ${ref.toFixed(0)} mV)`;
        orpCalDate.textContent = t;
      } else {
        orpCalDate.textContent = "Dernière calibration : —";
      }
    }

    // Temperature calibration info
    const tempCalibrated = cfg.temp_calibration_date && cfg.temp_calibration_date !== "";
    const tempCalibratedStatus = $("#temp_calibrated_status");
    const tempCalDate = $("#temp_cal_date");

    if (tempCalibratedStatus) {
      tempCalibratedStatus.style.display = tempCalibrated ? "block" : "none";
    }

    if (tempCalDate) {
      if (tempCalibrated) {
        const d = new Date(cfg.temp_calibration_date);
        const offset = cfg.temp_calibration_offset;
        let t = "Dernière calibration : " + d.toLocaleString("fr-FR");
        if (offset != null && !isNaN(offset)) t += ` (offset: ${offset > 0 ? '+' : ''}${offset.toFixed(1)}°C)`;
        tempCalDate.textContent = t;
      } else {
        tempCalDate.textContent = "Dernière calibration : —";
      }
    }

    // Filtration
    if (cfg.filtration_mode) $("#filtration_mode").value = cfg.filtration_mode;
    if (cfg.filtration_start) $("#filtration_start").value = cfg.filtration_start;
    if (cfg.filtration_end) $("#filtration_end").value = cfg.filtration_end;

    if ($("#filtration_mode").value === "manual") {
      cachedManualStart = $("#filtration_start").value;
      cachedManualEnd = $("#filtration_end").value;
    }
    updateFiltrationControls();

    // Time
    $("#time_use_ntp").checked = cfg.time_use_ntp !== false;
    $("#time_ntp_server").value = cfg.ntp_server || "pool.ntp.org";

    const tz = cfg.timezone_id || "europe_paris";
    if ($(`#time_timezone option[value="${tz}"]`)) $("#time_timezone").value = tz;

    const timeValue = (cfg.time_use_ntp !== false)
      ? (cfg.time_current || "")
      : (cfg.manual_time || cfg.time_current || "");
    $("#time_value").value = timeValue;
    updateTimeControls(timeValue);

    // Wi-Fi summary - store for later update
    window._wifiData = {
      ssid: cfg.wifi_ssid ?? "—",
      ip: cfg.wifi_ip ?? "—",
      mode: cfg.wifi_mode ?? "—",
      mdns: cfg.mdns_host ?? "poolcontroller.local"
    };

    console.log("WiFi data loaded:", window._wifiData);

    // Update WiFi display immediately after loading config
    updateWiFiDisplay();

    updatePhControls();
    updateOrpControls();

    // Security panel
    $("#auth_cors_origins").value = cfg.auth_cors_origins || "";

    // Mettre à jour les alertes et cartes status après chargement de la config
    updateAlerts();
    updateStatusCards();
    updateDetailSections();
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

  // ========== ALERTES ==========
  let dismissedAlerts = {};

  // Types d'alertes
  const AlertType = {
    PH_OUT_OF_RANGE: 'ph_out_range',
    ORP_OUT_OF_RANGE: 'orp_out_range',
    PH_CALIBRATION_OLD: 'ph_cal_old',
    ORP_CALIBRATION_OLD: 'orp_cal_old',
    TEMP_CALIBRATION_OLD: 'temp_cal_old'
  };

  // Charger les alertes ignorées depuis localStorage
  function loadDismissedAlerts() {
    const stored = localStorage.getItem('dismissedAlerts');
    if (stored) {
      try {
        dismissedAlerts = JSON.parse(stored);
      } catch (e) {
        dismissedAlerts = {};
      }
    }
  }

  // Vérifier si une alerte est ignorée
  function isAlertDismissed(alertType) {
    const dismissed = dismissedAlerts[alertType];
    if (!dismissed) return false;

    // Vérifier si l'expiration est dépassée
    if (Date.now() > dismissed.expiresAt) {
      delete dismissedAlerts[alertType];
      localStorage.setItem('dismissedAlerts', JSON.stringify(dismissedAlerts));
      return false;
    }
    return true;
  }

  // Ignorer une alerte pour 24h
  window.dismissAlert = function(alertType) {
    dismissedAlerts[alertType] = {
      expiresAt: Date.now() + (24 * 60 * 60 * 1000) // 24h
    };
    localStorage.setItem('dismissedAlerts', JSON.stringify(dismissedAlerts));
    updateAlerts();
  };

  // Stocker le HTML précédent des alertes pour éviter les rerender inutiles
  let lastAlertsHTML = '';

  // Mettre à jour les alertes
  function updateAlerts() {
    const container = $("#alerts-container");
    if (!container) return;

    const alerts = [];

    // Alerte pH hors plage (7.0-7.4)
    if (latestSensorData && latestSensorData.ph != null && !isNaN(latestSensorData.ph)) {
      const ph = latestSensorData.ph;
      if ((ph < 7.0 || ph > 7.4) && !isAlertDismissed(AlertType.PH_OUT_OF_RANGE)) {
        alerts.push({
          type: 'danger',
          icon: '⚠️',
          message: `pH hors plage recommandée : ${ph.toFixed(1)} (7.0 - 7.4)`,
          action: 'Aller à pH',
          actionLink: '#/ph',
          dismissable: false
        });
      }
    }

    // Alerte ORP hors plage (600-800 mV)
    if (latestSensorData && latestSensorData.orp != null && !isNaN(latestSensorData.orp)) {
      const orp = Math.round(latestSensorData.orp);
      if ((orp < 600 || orp > 800) && !isAlertDismissed(AlertType.ORP_OUT_OF_RANGE)) {
        alerts.push({
          type: 'danger',
          icon: '⚠️',
          message: `ORP hors plage recommandée : ${orp} mV (600 - 800 mV)`,
          action: 'Aller à ORP',
          actionLink: '#/orp',
          dismissable: false
        });
      }
    }

    // Alerte calibration pH ancienne (>3 mois)
    if (window._config && window._config.phCalibrationDate) {
      try {
        const calDate = new Date(window._config.phCalibrationDate);
        const ageMonths = (Date.now() - calDate.getTime()) / (1000 * 60 * 60 * 24 * 30);
        if (ageMonths > 3 && !isAlertDismissed(AlertType.PH_CALIBRATION_OLD)) {
          alerts.push({
            type: 'warning',
            icon: '🔧',
            message: `Calibration du pH trop ancienne - Dernière calibration :: ${calDate.toLocaleDateString()}`,
            action: 'Aller à calibration',
            actionLink: '#/ph',
            dismissable: true,
            alertType: AlertType.PH_CALIBRATION_OLD
          });
        }
      } catch (e) {
        // Date invalide, ignorer
      }
    }

    // Générer le HTML (template compact pour éviter les variations d'espaces)
    const newHTML = alerts.map(alert =>
      `<div class="alert alert--${alert.type}">` +
      `<span class="alert__icon">${alert.icon}</span>` +
      `<span class="alert__content">${alert.message}</span>` +
      `<a href="${alert.actionLink}" class="alert__action">${alert.action}</a>` +
      (alert.dismissable ? `<button class="alert__dismiss" onclick="dismissAlert('${alert.alertType}')">Ignorer 24h</button>` : '') +
      `</div>`
    ).join('');

    // Ne mettre à jour le DOM que si le contenu a changé
    if (newHTML !== lastAlertsHTML) {
      container.innerHTML = newHTML;
      lastAlertsHTML = newHTML;
    }
  }

  // ========== CARTES STATUS ==========

  function getFiltrationState(config, data) {
    const isRunning = data && data.filtration_running;
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

    // === ORP COMPACT ===
    const compactOrp = $("#compact-orp");
    if (compactOrp && data.orp != null) {
      compactOrp.innerHTML = Math.round(data.orp) + '<span class="compact-unit">mV</span>';
    }

    const compactOrpTarget = $("#compact-orp-target");
    if (compactOrpTarget && config.orp_target != null) {
      compactOrpTarget.textContent = Math.round(config.orp_target);
    }

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
      if (tab.dataset.chart === chartType) {
        tab.classList.add('chart-tab--active');
      } else {
        tab.classList.remove('chart-tab--active');
      }
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
  }


  function bindChartTabs() {
    const tabs = document.querySelectorAll('.chart-tab');
    tabs.forEach(tab => {
      tab.addEventListener('click', () => {
        const chartType = tab.dataset.chart;
        switchChartTab(chartType);
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
    const data = latestSensorData || {};

    // === FILTRATION ===
    const detailFiltrationMode = $("#detail-filtration-mode");
    if (detailFiltrationMode) {
      const modes = {
        auto: 'Auto',
        manual: 'Manuel',
        always_on: 'Toujours allumée',
        always_off: 'Toujours éteinte'
      };
      detailFiltrationMode.textContent = modes[config.filtration_mode] || 'Auto';
    }

    const detailFiltrationSchedule = $("#detail-filtration-schedule");
    if (detailFiltrationSchedule) {
      const start = config.filtration_start || '08:00';
      const end = config.filtration_end || '20:00';
      detailFiltrationSchedule.textContent = `${start} - ${end}`;
    }

    const detailFiltrationStatus = $("#detail-filtration-status");
    if (detailFiltrationStatus) {
      const state = getFiltrationState(config, data);
      detailFiltrationStatus.textContent = state.text;
      detailFiltrationStatus.className = 'state-badge ' + state.class;
    }

    // === ÉCLAIRAGE ===
    const detailLightingStatus = $("#detail-lighting-status");
    if (detailLightingStatus) {
      const isOn = config.lighting_enabled;
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
        const now = new Date();
        const currentTime = `${String(now.getHours()).padStart(2, '0')}:${String(now.getMinutes()).padStart(2, '0')}`;
        const payload = {
          filtration_mode: "manual",
          filtration_start: currentTime,
          filtration_end: "23:59"
        };
        try {
          const result = await sendConfig(payload);
          if (result) {
            showToast("Filtration démarrée", "success");
            await loadConfig();
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
        const payload = { filtration_mode: "off" };
        try {
          const result = await sendConfig(payload);
          if (result) {
            showToast("Filtration arrêtée", "success");
            await loadConfig();
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
          // À ajuster selon votre backend
          console.log('Allumer éclairage');
        } catch (e) {
          console.error('Erreur allumage éclairage:', e);
        }
      });
    }

    if (lightOffBtn) {
      lightOffBtn.addEventListener('click', async () => {
        try {
          // À ajuster selon votre backend
          console.log('Éteindre éclairage');
        } catch (e) {
          console.error('Erreur extinction éclairage:', e);
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
    if (sensorDataLoadInFlight) return sensorDataLoadInFlight;
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
        const json = await res.json();
        jsonPerf?.end();

        latestSensorData = json;

        if (sensorDataRetryTimer) {
          clearTimeout(sensorDataRetryTimer);
          sensorDataRetryTimer = null;
        }

        // Reset failure counter on success
        consecutiveFailures = 0;
        setNetStatus("ok", "En ligne");

        const label = json.timestamp ? new Date(json.timestamp).toLocaleTimeString() : new Date().toLocaleTimeString();

        if (json.temperature != null && !isNaN(json.temperature) && tempChart) pushPoint(tempChart, json.temperature, label);
        if (json.ph != null && !isNaN(json.ph) && phChart) {
          pushPoint(phChart, Math.round(json.ph * 10) / 10, label);
          ensurePhReferenceDatasets(phChart);
          ensurePhPlaceholderLabels(phChart);
          syncPhReferenceDatasets(phChart);
          phChart.update('none');
        }
        if (json.orp != null && !isNaN(json.orp) && orpChart) {
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

        // Mettre à jour les alertes et cartes status
        updateAlerts();
        updateStatusCards();

        // Mettre à jour les sections détaillées
        updateDetailSections();

        // also update readouts in settings
        const phCurrentValue = $("#ph_current_value");
        if (phCurrentValue) {
          if (json.ph != null && typeof json.ph === "number" && !isNaN(json.ph)) {
            phCurrentValue.textContent = json.ph.toFixed(2);
          } else {
            phCurrentValue.textContent = "--";
          }
        }

        const orpCurrentValue = $("#orp_current_value");
        if (orpCurrentValue) {
          if (json.orp != null && typeof json.orp === "number" && !isNaN(json.orp)) {
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

          // Mise à jour immédiate (optimiste) : pendant la calibration 4.0, le pH corrigé doit être proche de 4.0
          const nowLabel = new Date().toLocaleTimeString();
          if (!latestSensorData) latestSensorData = {};
          latestSensorData.ph = 4.0;

          const mPh = $("#m-ph");
          if (mPh) mPh.textContent = (Math.round(4.0 * 10) / 10).toFixed(1);
          const mTime = $("#m-time");
          if (mTime) mTime.textContent = nowLabel;

          const phCurrentValue = $("#ph_current_value");
          if (phCurrentValue) phCurrentValue.textContent = (4.0).toFixed(2);

          if (phChart) pushPoint(phChart, Math.round(4.0 * 10) / 10, nowLabel);

          // Invalider le timestamp pour forcer le rechargement
          lastSensorDataLoadTime = 0;

          // Refresh réel depuis l'ESP (valeurs calculées après calibration)
          await loadConfig();
          updatePhCalibrationSteps();

          // Recharger les données après un court délai pour se resynchroniser avec le backend
          setTimeout(async () => {
            await loadSensorData();
          }, 2000);
        } catch (err) {
          alert("Erreur calibration pH 4.0:\n" + err.message);
          phCalibrationStep = 0;
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
        if (refInput1pt) refInput1pt.value = "";
        updateOrpCalibrationSteps1pt();
        loadConfig();
      }
    });

    startBtn1pt?.addEventListener("click", async () => {
      if (orpCalibrationStep1pt === 0) {
        orpCalibrationStep1pt = 1;
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
          if (refInput1pt) refInput1pt.value = "";

          // Mise à jour immédiate (optimiste) : l'ORP corrigé doit correspondre à la référence (point unique)
          const nowLabel = new Date().toLocaleTimeString();
          if (!latestSensorData) latestSensorData = {};
          latestSensorData.orp = referenceValue;
          latestSensorData.orp_raw = currentOrpRaw;

          const mOrp = $("#m-orp");
          if (mOrp) mOrp.textContent = String(Math.round(referenceValue));
          const mTime = $("#m-time");
          if (mTime) mTime.textContent = nowLabel;

          const orpCurrentValue = $("#orp_current_value");
          if (orpCurrentValue) orpCurrentValue.textContent = Math.round(referenceValue) + " mV";

          if (orpChart) pushPoint(orpChart, referenceValue, nowLabel);

          // Invalider le timestamp pour forcer le rechargement
          lastSensorDataLoadTime = 0;

          // Refresh réel depuis l'ESP
          await loadConfig();
          updateOrpCalibrationSteps1pt();

          // Recharger les données après un court délai pour se resynchroniser avec le backend
          setTimeout(async () => {
            await loadSensorData();
          }, 2000);
        } catch (err) {
          alert("Erreur calibration ORP:\n" + err.message);
          orpCalibrationStep1pt = 0;
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
          orpPoint1Measured = null;
          orpPoint1Reference = null;
          if (ref1) ref1.value = "";
          if (ref2) ref2.value = "";

          // Mise à jour immédiate (optimiste) : juste après calibration, on est souvent encore dans la solution r2
          const nowLabel = new Date().toLocaleTimeString();
          if (!latestSensorData) latestSensorData = {};
          latestSensorData.orp = r2;

          const mOrp = $("#m-orp");
          if (mOrp) mOrp.textContent = String(Math.round(r2));
          const mTime = $("#m-time");
          if (mTime) mTime.textContent = nowLabel;

          const orpCurrentValue = $("#orp_current_value");
          if (orpCurrentValue) orpCurrentValue.textContent = Math.round(r2) + " mV";

          if (orpChart) pushPoint(orpChart, r2, nowLabel);

          // Invalider le timestamp pour forcer le rechargement
          lastSensorDataLoadTime = 0;

          // Refresh réel depuis l'ESP
          await loadConfig();
          updateOrpCalibrationSteps2pt();

          // Recharger les données après un court délai pour se resynchroniser avec le backend
          setTimeout(async () => {
            await loadSensorData();
          }, 2000);
        } catch (err) {
          alert("Erreur calibration ORP:\n" + err.message);
          orpCalibrationStep2pt = 0;
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
    console.log("updateWiFiDisplay() called, _wifiData:", window._wifiData);

    if (!window._wifiData) {
      console.warn("No WiFi data available");
      return;
    }

    const wifi = window._wifiData;
    const ssidEl = $("#wifi_ssid");
    const ipEl = $("#wifi_ip");
    const modeEl = $("#wifi_mode");
    const mdnsEl = $("#wifi_mdns");

    console.log("WiFi elements found:", { ssidEl: !!ssidEl, ipEl: !!ipEl, modeEl: !!modeEl, mdnsEl: !!mdnsEl });

    if (ssidEl) ssidEl.textContent = wifi.ssid;
    if (ipEl) ipEl.textContent = wifi.ip;
    if (modeEl) modeEl.textContent = wifi.mode;
    if (mdnsEl) mdnsEl.textContent = wifi.mdns;

    console.log("WiFi display updated");
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
          alert("Erreur vérification mise à jour.\nVérifie la connexion Internet.");
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
      status.textContent = "Étape 1/2 : filesystem…";

      try {
        // step 1 FS
        let p = 0;
        let t = setInterval(() => {
          if (p < 45) {
            p += 5;
            bar.style.width = p + "%";
            bar.textContent = p + "%";
          }
        }, 500);

        const fsRes = await authFetch("/download-update", {
          method: "POST",
          headers: { "Content-Type": "application/x-www-form-urlencoded" },
          body: "url=" + encodeURIComponent(latestRelease.filesystem_url) + "&restart=false",
        });

        clearInterval(t);
        let fsJson = null;
        if (!fsRes.ok) {
          try {
            fsJson = await fsRes.json();
          } catch (_) {}
          const errMsg = fsJson?.error || "FS download fail";
          throw new Error(errMsg);
        }
        fsJson = await fsRes.json();
        if (fsJson.status !== "success") throw new Error("FS install fail");

        bar.style.width = "50%";
        bar.textContent = "50%";
        status.textContent = "Étape 2/2 : firmware…";

        // step 2 FW
        p = 50;
        t = setInterval(() => {
          if (p < 95) {
            p += 5;
            bar.style.width = p + "%";
            bar.textContent = p + "%";
          }
        }, 500);

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
          const errMsg = fwJson?.error || "FW download fail";
          throw new Error(errMsg);
        }
        fwJson = await fwRes.json();

        if (fwJson.status === "success") {
          bar.style.width = "100%";
          bar.textContent = "100%";
          status.textContent = "✓ Mise à jour OK. Redémarrage dans 30 secondes…";
          setTimeout(() => window.location.reload(), 30000);
        } else {
          throw new Error("FW install fail");
        }
      } catch (e) {
        const msg = String(e?.message || "");
        if (msg.includes("System time not synchronized")) {
          alert("Heure non synchronisée.\nActive le NTP ou règle l'heure dans les réglages avant de lancer la mise à jour.");
        } else {
          alert("Erreur mise à jour:\n" + e.message);
        }
      } finally {
        checkBtn.disabled = false;
        // installBtn restera disabled si pas re-check
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
        status.textContent = `Envoi: ${percent}%`;

        if (percent === 100 && !uploadCompleted) {
          uploadCompleted = true;
          status.textContent = "✓ Envoi terminé. Redémarrage…";
          setTimeout(() => window.location.reload(), 30000);
        }
      });

      xhr.addEventListener("load", () => {
        if (xhr.status !== 200 && !uploadCompleted) {
          alert("Erreur HTTP: " + xhr.status);
        }
      });

      xhr.addEventListener("error", () => {
        if (!uploadCompleted) alert("Erreur réseau upload");
      });

      xhr.open("POST", "/update");
      xhr.send(formData);
    });
  }

  // Logs (/get-logs)
  let logsAutoRefreshInterval = null;
  let lastLogTimestamp = 0;
  let allLogEntries = [];

  function matchesFilter(message, filter) {
    if (filter === "all") return true;
    const msg = String(message || "").toLowerCase();
    if (filter === "temp") return msg.includes("temp:") || msg.includes("température") || msg.includes("ds18b20");
    if (filter === "ph") return msg.includes("ph:");
    if (filter === "orp") return msg.includes("orp:");
    return true;
  }

  async function loadLogs(scroll = true, incremental = false) {
    try {
      // En mode incrémental, envoyer le dernier timestamp au backend
      let url = "/get-logs";
      if (incremental && lastLogTimestamp > 0) {
        url += `?since=${lastLogTimestamp}`;
      }

      const res = await authFetch(url);
      const data = await res.json();

      const filter = $("#logs_filter")?.value || "all";
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

      // Filtrer et afficher
      const filtered = allLogEntries.filter((l) => matchesFilter(l, filter));

      const content = $("#logs_content");
      if (content) {
        // Format each log entry as a string
        const formattedLogs = filtered.map(entry => {
          if (typeof entry === 'string') return entry;
          // Format: [timestamp] LEVEL: message
          const ts = entry.timestamp || '';
          const level = entry.level || '';
          const msg = entry.message || '';
          return `[${ts}] ${level}: ${msg}`;
        });

        // Use innerHTML with <div> elements to ensure proper line breaks
        if (formattedLogs.length === 0) {
          content.textContent = "(vide)";
        } else {
          content.innerHTML = formattedLogs.map(line => `<div>${line}</div>`).join('');
        }
      }

      const container = $("#logs_container");
      if (scroll && container) container.scrollTop = container.scrollHeight;
    } catch (e) {
      const content = $("#logs_content");
      if (content) content.textContent = "Erreur chargement logs.";
    }
  }

  function bindLogs() {
    $("#refresh_logs_btn")?.addEventListener("click", () => loadLogs(true, false));

    $("#clear_logs_display_btn")?.addEventListener("click", () => {
      const content = $("#logs_content");
      if (content) content.textContent = "";
      allLogEntries = [];
      lastLogTimestamp = 0;
    });

    $("#logs_filter")?.addEventListener("change", () => loadLogs(true, false));

    // Gestion de l'auto-refresh
    const autoRefreshCheckbox = $("#logs_auto_refresh");
    autoRefreshCheckbox?.addEventListener("change", (e) => {
      if (e.target.checked) {
        // Démarrer l'auto-refresh toutes les 2 secondes
        loadLogs(true, false); // Premier chargement complet
        logsAutoRefreshInterval = setInterval(() => {
          loadLogs(true, true); // Chargements suivants en mode incrémental
        }, 2000);
      } else {
        // Arrêter l'auto-refresh
        if (logsAutoRefreshInterval) {
          clearInterval(logsAutoRefreshInterval);
          logsAutoRefreshInterval = null;
        }
      }
    });
  }

  // Pump test
  function bindPumps() {
    const p1 = $("#pump1_test");
    const p2 = $("#pump2_test");
    const s1 = $("#pump1_duty");
    const s2 = $("#pump2_duty");

    function updateSliderReadout(slider, idPct, idPwm) {
      const duty = parseInt(slider.value, 10);
      const pct = Math.round((duty / 255) * 100);
      $(idPct).textContent = String(pct);
      $(idPwm).textContent = String(duty);
    }

    s1?.addEventListener("input", () => updateSliderReadout(s1, "#pump1_duty_value", "#pump1_duty_pwm"));
    s2?.addEventListener("input", () => updateSliderReadout(s2, "#pump2_duty_value", "#pump2_duty_pwm"));

    s1?.addEventListener("change", () => {
      if (p1?.checked) authFetch(`/pump1/duty/${parseInt(s1.value, 10)}`, { method: "POST" }).catch(() => {});
    });
    s2?.addEventListener("change", () => {
      if (p2?.checked) authFetch(`/pump2/duty/${parseInt(s2.value, 10)}`, { method: "POST" }).catch(() => {});
    });

    p1?.addEventListener("change", async () => {
      if (!p1.checked) {
        await authFetch("/pump1/off", { method: "POST" }).catch(() => {});
      } else {
        await authFetch(`/pump1/duty/${parseInt(s1.value, 10)}`, { method: "POST" }).catch(() => {
          p1.checked = false;
        });
      }
    });

    p2?.addEventListener("change", async () => {
      if (!p2.checked) {
        await authFetch("/pump2/off", { method: "POST" }).catch(() => {});
      } else {
        await authFetch(`/pump2/duty/${parseInt(s2.value, 10)}`, { method: "POST" }).catch(() => {
          p2.checked = false;
        });
      }
    });

    // init
    if (s1) updateSliderReadout(s1, "#pump1_duty_value", "#pump1_duty_pwm");
    if (s2) updateSliderReadout(s2, "#pump2_duty_value", "#pump2_duty_pwm");
  }

  // WiFi Configuration
  function bindWifi() {
    $("#wifi_config_btn")?.addEventListener("click", () => {
      // Rediriger vers la page de configuration WiFi
      window.location.href = "/wifi.html";
    });
  }

  // ---------- Auto-save bindings ----------
  function bindAutosave() {
    const save = () => sendConfig(collectConfig());

    // MQTT
    $("#mqtt_enabled")?.addEventListener("change", () => {
      updateMqttStatusIndicator($("#mqtt_enabled").checked, false);
      save();
      setTimeout(loadConfig, 6000);
    });
    ["mqtt_server", "mqtt_port", "mqtt_username", "mqtt_password"].forEach((id) => {
      $(`#${id}`)?.addEventListener("change", () => {
        save();
        setTimeout(loadConfig, 6000);
      });
    });
    $("#mqtt_topic")?.addEventListener("change", save);

    // Filtration
    $("#filtration_mode")?.addEventListener("change", () => {
      updateFiltrationControls();
      save();
    });
    $("#filtration_start")?.addEventListener("change", () => {
      cachedManualStart = $("#filtration_start").value;
      if ($("#filtration_mode").value === "manual") save();
    });
    $("#filtration_end")?.addEventListener("change", () => {
      cachedManualEnd = $("#filtration_end").value;
      if ($("#filtration_mode").value === "manual") save();
    });

    // Lighting schedule
    $("#lighting_schedule_mode")?.addEventListener("change", () => {
      const scheduleSettings = $("#lighting-schedule-settings");
      if (scheduleSettings) {
        scheduleSettings.style.display = $("#lighting_schedule_mode").value === "enabled" ? "block" : "none";
      }
      save();
    });
    $("#lighting_start_time")?.addEventListener("change", save);
    $("#lighting_end_time")?.addEventListener("change", save);

    // pH / ORP regulation
    $("#ph_enabled")?.addEventListener("change", () => { updatePhControls(); save(); });
    $("#orp_enabled")?.addEventListener("change", () => { updateOrpControls(); save(); });
    ["ph_target", "ph_limit", "ph_pump"].forEach((id) => $(`#${id}`)?.addEventListener("change", () => { updatePhControls(); save(); }));
    ["orp_target", "orp_limit", "orp_pump"].forEach((id) => $(`#${id}`)?.addEventListener("change", () => { updateOrpControls(); save(); }));

    // Time
    $("#time_use_ntp")?.addEventListener("change", async () => {
      const useNtp = $("#time_use_ntp").checked;
      if (useNtp) {
        // Fetch current time from server when NTP is enabled
        try {
          const res = await authFetch("/time-now");
          const data = await res.json();
          updateTimeControls(data.time || "");
        } catch (e) {
          updateTimeControls("");
        }
      } else {
        // Keep current value when switching to manual mode, just make it editable
        updateTimeControls($("#time_value").value);
      }
      save();
    });
    $("#time_ntp_server")?.addEventListener("change", () => { if ($("#time_use_ntp").checked) save(); });
    $("#time_timezone")?.addEventListener("change", save);
    $("#time_value")?.addEventListener("input", () => {
      // Auto-save when manually editing time (only when NTP is disabled)
      if (!$("#time_use_ntp").checked) save();
    });
  }

  // ---------- UI bindings ----------
  function bindUI() {
    // mobile burger (open from main)
    $("#burger-open")?.addEventListener("click", () => $(".sidebar")?.classList.add("is-open"));

    // mobile burger (close from sidebar)
    $("#burger")?.addEventListener("click", () => $(".sidebar")?.classList.remove("is-open"));

    // segmented -> route
    $$(".segmented__btn").forEach((btn) => {
      btn.addEventListener("click", () => goSettings(btn.getAttribute("data-settings-tab")));
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
  }

  // ---------- Security bindings ----------
  function bindSecurity() {
    // Save CORS configuration (auto-save)
    $("#auth_cors_origins")?.addEventListener("change", () => {
      const corsValue = $("#auth_cors_origins").value.trim();
      sendConfig({ auth_cors_origins: corsValue });
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

      // Validation: new password required and minimum length
      if (!newPassword || newPassword.length < 8) {
        alert("Le nouveau mot de passe doit contenir au moins 8 caractères.");
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
        // Clear all password fields
        $("#auth_current_password").value = "";
        $("#auth_password").value = "";
        $("#auth_password_confirm").value = "";
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

    // Charger les alertes ignorées depuis localStorage
    loadDismissedAlerts();

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
    bindSecurity();
    bindGithubUpdate();
    bindManualUpdate();
    bindPumps();
    bindLogs();

    // initial loads
    setNetStatus("mid", "Connexion…");
    const configPerf = debugStart("loadConfig");
    await loadConfig().catch(() => {});
    configPerf?.end();

    // Load historical data to populate charts
    const historyPerf = debugStart("loadHistoricalData");
    await loadHistoricalData('24h').catch(() => {});
    historyPerf?.end();

    await loadSensorData({ force: true, source: "init" }).catch(() => {}); // Charger les données AVANT d'afficher la route

    const calibPerf = debugStart("checkCalibrationDate");
    await checkCalibrationDate().catch(() => {});
    calibPerf?.end();

    const logsPerf = debugStart("loadLogs");
    await loadLogs(true).catch(() => {});
    logsPerf?.end();

    // router
    const applyRoute = () => {
      const perf = debugStart("applyRoute");
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
      perf?.end(`route=${r.view}`);
    };
    window.addEventListener("hashchange", applyRoute);
    applyRoute();

    // Load system info after a small delay to ensure DOM is ready
    // This is critical when direct linking to #/settings/dev or #/settings/system
    setTimeout(async () => {
      try {
        await loadSystemInfo();
      } catch (e) {
        console.error("Failed to load system info:", e);
      }
    }, 200);

    // loops (loadSensorData déjà appelé au démarrage ligne 2302)
    setInterval(() => loadSensorData({ source: "interval" }), SENSOR_REFRESH_MS); // 30 secondes
    setInterval(() => {
      if (!lastSensorDataLoadTime) return;
      const ageMs = Date.now() - lastSensorDataLoadTime;
      if (ageMs > SENSOR_REFRESH_MS * 2) {
        setNetStatus("bad", "Hors ligne");
      } else if (ageMs > SENSOR_REFRESH_MS * 1.2) {
        setNetStatus("mid", "Connexion…");
      }
    }, 5000);
    setInterval(checkCalibrationDate, 300000); // 5 min

    // If you want: refresh config status occasionally (MQTT connected, etc.)
    setInterval(() => {
      // ne recharge pas tout en permanence si tu veux limiter la charge
      loadConfig().catch(() => {});
    }, 15000);

    // ========== FILTRATION MANUAL CONTROL ==========
    setupFiltrationManualControl();

    // ========== LIGHTING SCHEDULE ==========
    setupLightingSchedule();

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
            showToast("Éclairage allumé", "success");
            updateLightingStatus(true);
            await loadConfig();
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
            showToast("Éclairage éteint", "success");
            updateLightingStatus(false);
            await loadConfig();
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
    const statusBadge = $("#filtration-current-status");

    // Update status badge from config
    function updateFiltrationStatus() {
      const config = window._config || {};
      if (statusBadge) {
        const isRunning = config.filtration_running;
        statusBadge.textContent = isRunning ? 'En marche' : 'Arrêtée';
        statusBadge.className = 'state-badge ' + (isRunning ? 'state-badge--ok' : 'state-badge--off');
      }
    }

    // Start filtration: switch to manual mode with current time to 23:59
    if (startBtn) {
      startBtn.addEventListener("click", async () => {
        const now = new Date();
        const currentTime = `${String(now.getHours()).padStart(2, '0')}:${String(now.getMinutes()).padStart(2, '0')}`;

        const payload = {
          filtration_mode: "manual",
          filtration_start: currentTime,
          filtration_end: "23:59"
        };

        try {
          const result = await sendConfig(payload);
          if (result) {
            showToast("Filtration démarrée", "success");
            await loadConfig();
            updateFiltrationStatus();
          } else {
            showToast("Erreur lors du démarrage", "error");
          }
        } catch (error) {
          console.error("Error starting filtration:", error);
          showToast("Erreur de connexion", "error");
        }
      });
    }

    // Stop filtration: switch to off mode
    if (stopBtn) {
      stopBtn.addEventListener("click", async () => {
        const payload = {
          filtration_mode: "off"
        };

        try {
          const result = await sendConfig(payload);
          if (result) {
            showToast("Filtration arrêtée", "success");
            await loadConfig();
            updateFiltrationStatus();
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
    updateFiltrationStatus();
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
