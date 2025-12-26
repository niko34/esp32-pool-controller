(() => {
  "use strict";

  // ---------- Utils ----------
  const $ = (sel, root = document) => root.querySelector(sel);
  const $$ = (sel, root = document) => Array.from(root.querySelectorAll(sel));
  const clamp = (n, a, b) => Math.max(a, Math.min(b, n));

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

    // Load sensor data when navigating to calibration pages if data is stale (>30s) or never loaded
    if (routeObj.view === "/temperature" || routeObj.view === "/ph" || routeObj.view === "/orp") {
      const now = Date.now();
      const dataAge = now - lastSensorDataLoadTime;
      const maxDataAge = 30000; // 30 secondes

      if (lastSensorDataLoadTime === 0 || dataAge > maxDataAge) {
        loadSensorData();
      }
    }

    // Mobile: close sidebar after navigation
    $(".sidebar")?.classList.remove("is-open");
  }

  // ---------- Charts (Dashboard) ----------
  function createLineChart(ctx, color, label, integerOnly = false) {
    const yAxisConfig = {
      beginAtZero: false,
      grid: { color: 'rgba(0, 0, 0, 0.05)' },
      ticks: { color: '#6b7280' }
    };
    if (integerOnly) {
      yAxisConfig.ticks.callback = function (value) {
        if (Number.isInteger(value)) return value;
      };
    }

    return new Chart(ctx, {
      type: "line",
      data: {
        labels: [],
        datasets: [
          {
            label,
            data: [],
            borderColor: color,
            backgroundColor: color + '20',
            tension: 0.3,
            borderWidth: 2,
            fill: true,
          },
        ],
      },
      options: {
        responsive: true,
        maintainAspectRatio: true,
        aspectRatio: 16 / 9,
        animation: false,
        scales: {
          x: {
            ticks: { maxRotation: 0, minRotation: 0, color: '#6b7280' },
            grid: { color: 'rgba(0, 0, 0, 0.05)' }
          },
          y: yAxisConfig,
        },
        plugins: { legend: { display: false } },
      },
    });
  }

  function pushPoint(chart, value, label) {
    chart.data.labels.push(label);
    chart.data.datasets[0].data.push(value);
    if (chart.data.labels.length > 30) {
      chart.data.labels.shift();
      chart.data.datasets[0].data.shift();
    }
    chart.update("none");
  }

  let tempChart, phChart, orpChart;

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
    return fetch("/save-config", {
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
    };
  }

  async function loadConfig() {
    const res = await fetch("/get-config");
    const cfg = await res.json();

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

    const timeValue = cfg.manual_time || cfg.time_current || "";
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
  }

  // ---------- Calibration badges (Dashboard + chip) ----------
  async function checkCalibrationDate() {
    try {
      const res = await fetch("/get-config");
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
  let lastSensorDataLoadTime = 0; // Timestamp du dernier chargement des données

  function updateDashboardMetrics(json) {
    const ts = json.timestamp ? new Date(json.timestamp) : new Date();
    $("#m-time").textContent = ts.toLocaleTimeString();

    if (json.temperature != null && !isNaN(json.temperature)) $("#m-temp").textContent = json.temperature.toFixed(1);
    if (json.ph != null && !isNaN(json.ph)) $("#m-ph").textContent = (Math.round(json.ph * 10) / 10).toFixed(1);
    if (json.orp != null && !isNaN(json.orp)) $("#m-orp").textContent = String(Math.round(json.orp));
  }

  async function loadSensorData() {
    try {
      // Add timeout to detect disconnection (increased to 10 seconds for ESP32)
      const controller = new AbortController();
      const timeoutId = setTimeout(() => controller.abort(), 10000); // 10 second timeout

      // Bypass any HTTP cache so the UI can refresh immediately after calibration
      const res = await fetch(`/data?t=${Date.now()}`, { signal: controller.signal, cache: "no-store" });
      clearTimeout(timeoutId);

      if (!res.ok) throw new Error("bad");
      const json = await res.json();

      latestSensorData = json;

      // Reset failure counter on success
      consecutiveFailures = 0;
      setNetStatus("ok", "En ligne");

      const label = json.timestamp ? new Date(json.timestamp).toLocaleTimeString() : new Date().toLocaleTimeString();

      if (json.temperature != null && !isNaN(json.temperature)) pushPoint(tempChart, json.temperature, label);
      if (json.ph != null && !isNaN(json.ph)) pushPoint(phChart, Math.round(json.ph * 10) / 10, label);
      if (json.orp != null && !isNaN(json.orp)) pushPoint(orpChart, json.orp, label);

      updateDashboardMetrics(json);

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
    } catch (e) {
      // Only mark as offline after 3 consecutive failures (avoid false positives)
      consecutiveFailures++;
      if (consecutiveFailures >= 3) {
        setNetStatus("bad", "Hors ligne");
      }
    }
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
          const res = await fetch("/calibrate_ph_neutral", { method: "POST" });
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
          const res = await fetch("/calibrate_ph_acid", { method: "POST" });
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
      const res = await fetch("/get-system-info");
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
        const res = await fetch("/check-update");
        if (!res.ok) throw new Error("Erreur vérification");
        const data = await res.json();
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
        alert("Erreur vérification mise à jour.\nVérifie la connexion Internet.");
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

        const fsRes = await fetch("/download-update", {
          method: "POST",
          headers: { "Content-Type": "application/x-www-form-urlencoded" },
          body: "url=" + encodeURIComponent(latestRelease.filesystem_url) + "&restart=false",
        });

        clearInterval(t);
        if (!fsRes.ok) throw new Error("FS download fail");
        const fsJson = await fsRes.json();
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

        const fwRes = await fetch("/download-update", {
          method: "POST",
          headers: { "Content-Type": "application/x-www-form-urlencoded" },
          body: "url=" + encodeURIComponent(latestRelease.firmware_url) + "&restart=true",
        });

        clearInterval(t);
        if (!fwRes.ok) throw new Error("FW download fail");
        const fwJson = await fwRes.json();

        if (fwJson.status === "success") {
          bar.style.width = "100%";
          bar.textContent = "100%";
          status.textContent = "✓ Mise à jour OK. Redémarrage…";
          alert("Mise à jour OK. Rechargement dans ~30s.");
          setTimeout(() => window.location.reload(), 30000);
        } else {
          throw new Error("FW install fail");
        }
      } catch (e) {
        alert("Erreur mise à jour:\n" + e.message);
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

      const res = await fetch(url);
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
      if (p1?.checked) fetch(`/pump1/duty/${parseInt(s1.value, 10)}`, { method: "POST" }).catch(() => {});
    });
    s2?.addEventListener("change", () => {
      if (p2?.checked) fetch(`/pump2/duty/${parseInt(s2.value, 10)}`, { method: "POST" }).catch(() => {});
    });

    p1?.addEventListener("change", async () => {
      if (!p1.checked) {
        await fetch("/pump1/off", { method: "POST" }).catch(() => {});
      } else {
        await fetch(`/pump1/duty/${parseInt(s1.value, 10)}`, { method: "POST" }).catch(() => {
          p1.checked = false;
        });
      }
    });

    p2?.addEventListener("change", async () => {
      if (!p2.checked) {
        await fetch("/pump2/off", { method: "POST" }).catch(() => {});
      } else {
        await fetch(`/pump2/duty/${parseInt(s2.value, 10)}`, { method: "POST" }).catch(() => {
          p2.checked = false;
        });
      }
    });

    // init
    if (s1) updateSliderReadout(s1, "#pump1_duty_value", "#pump1_duty_pwm");
    if (s2) updateSliderReadout(s2, "#pump2_duty_value", "#pump2_duty_pwm");
  }

  // Restart AP
  function bindWifi() {
    $("#restart_ap_btn")?.addEventListener("click", async () => {
      if (!confirm("Redémarrer en mode AP ?")) return;
      await fetch("/reboot-ap", { method: "POST" }).catch(() => {});
      alert("Commande envoyée. L’ESP32 va redémarrer.");
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
          const res = await fetch("/time-now");
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
  }

  // ---------- Init ----------
  async function init() {
    // charts
    tempChart = createLineChart($("#tempChart"), "#4f8fff", "Température");
    phChart = createLineChart($("#phChart"), "#8b5cf6", "pH");
    orpChart = createLineChart($("#orpChart"), "#10b981", "ORP", true);

    bindUI();
    bindAutosave();
    bindPhCalibration();
    bindOrpCalibration();
    bindTempCalibration();
    bindWifi();
    bindGithubUpdate();
    bindManualUpdate();
    bindPumps();
    bindLogs();

    // router
    const applyRoute = () => {
      const r = getRoute();
      showView(r);
    };
    window.addEventListener("hashchange", applyRoute);
    applyRoute();

    // initial loads
    try {
      setNetStatus("mid", "Connexion…");
      await loadConfig();
      await checkCalibrationDate();
      await loadLogs(true);
    } catch (_) {}

    // Load system info after a small delay to ensure DOM is ready
    // This is critical when direct linking to #/settings/dev or #/settings/system
    setTimeout(async () => {
      try {
        await loadSystemInfo();
      } catch (e) {
        console.error("Failed to load system info:", e);
      }
    }, 200);

    // loops
    await loadSensorData();
    setInterval(loadSensorData, 30000); // 30 secondes
    setInterval(checkCalibrationDate, 300000); // 5 min

    // If you want: refresh config status occasionally (MQTT connected, etc.)
    setInterval(() => {
      // ne recharge pas tout en permanence si tu veux limiter la charge
      loadConfig().catch(() => {});
    }, 15000);
  }

  window.addEventListener("DOMContentLoaded", init);
})();