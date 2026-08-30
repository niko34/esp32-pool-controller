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
  // Durée d'affichage adaptée à la longueur du message : les messages courts
  // ("Sauvegardé", "Filtration démarrée"…) gardent les 3 s historiques
  // (≤ 75 caractères), les messages longs sur 2-3 lignes (ex. refus de mise à
  // jour en ligne sans PSRAM, ~220 caractères) restent affichés le temps
  // d'être lus (~40 ms/caractère ≈ 250 mots/min), plafonné à 9 s.
  const kToastMinDurationMs = 3000;
  const kToastMaxDurationMs = 9000;
  const kToastMsPerChar = 40;

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
    const duration = Math.min(
      kToastMaxDurationMs,
      Math.max(kToastMinDurationMs, String(message || '').length * kToastMsPerChar)
    );
    setTimeout(() => {
      toast.style.opacity = '0';
      setTimeout(() => toast.remove(), 300);
    }, duration);
  }

  // ---------- Modale de confirmation générique (feature-031) ----------
  // Remplace confirm() natif par une <dialog> stylée (#confirm-modal).
  // Retourne Promise<boolean> : true si confirmé, false sinon (Annuler / Escape / backdrop).
  let _confirmDialogState = null; // { resolve, confirmed } quand la modale est ouverte

  function confirmDialog({ title, message, confirmLabel = 'Confirmer', cancelLabel = 'Annuler', danger = false } = {}) {
    const modal = $("#confirm-modal");
    if (!modal || typeof modal.showModal !== 'function') {
      // Fallback navigateur sans <dialog> : on ne supprime JAMAIS une confirmation
      // de sécurité → window.confirm() natif.
      return Promise.resolve(window.confirm(message));
    }
    // Réentrance : une confirmation est déjà ouverte → refus immédiat
    if (modal.open || _confirmDialogState) return Promise.resolve(false);

    return new Promise((resolve) => {
      _confirmDialogState = { resolve, confirmed: false };

      const titleEl = $("#confirm-modal-title");
      const messageEl = $("#confirm-modal-message");
      const okBtn = $("#confirm-modal-ok");
      const cancelBtn = $("#confirm-modal-cancel");
      if (titleEl) titleEl.textContent = title || 'Confirmation';
      // textContent uniquement (jamais innerHTML) — les \n\n sont rendus via
      // white-space: pre-line (CSS .confirm-modal__message)
      if (messageEl) messageEl.textContent = message || '';
      if (okBtn) {
        okBtn.textContent = confirmLabel;
        okBtn.classList.toggle('btn--danger', danger);
        okBtn.classList.toggle('btn--primary', !danger);
      }
      if (cancelBtn) cancelBtn.textContent = cancelLabel;

      try {
        modal.showModal();
      } catch (e) {
        // Refus sûr : l'action protégée n'est PAS exécutée si la modale ne s'ouvre pas
        console.error('[confirm-modal] showModal:', e);
        _confirmDialogState = null;
        resolve(false);
        return;
      }
      // Focus initial TOUJOURS sur Annuler (action sûre par défaut)
      if (cancelBtn) cancelBtn.focus();
    });
  }

  // Listeners bindés UNE seule fois au démarrage
  function bindConfirmModal() {
    const modal = $("#confirm-modal");
    if (!modal || typeof modal.showModal !== 'function') return; // fallback géré dans confirmDialog()

    $("#confirm-modal-ok")?.addEventListener('click', () => {
      if (_confirmDialogState) _confirmDialogState.confirmed = true;
      modal.close();
    });
    $("#confirm-modal-cancel")?.addEventListener('click', () => modal.close());
    // Backdrop : clic en dehors du contenu du dialog
    modal.addEventListener('click', (e) => {
      if (e.target === modal) modal.close();
    });
    // Toute fermeture (bouton, Escape → cancel/close, backdrop) résout la promesse
    modal.addEventListener('close', () => {
      const state = _confirmDialogState;
      _confirmDialogState = null;
      if (state) state.resolve(!!state.confirmed);
    });
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
      showSettingsPanel(routeObj.sub || "install");
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

  // ---------- Infrastructure uPlot (migration Chart.js → uPlot, feature-043) ----------
  const UPLOT_FONT_STACK = '-apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, "Helvetica Neue", Arial, sans-serif';

  // Incréments entiers pour l'axe X catégoriel (x = index → label), évite les ticks fractionnaires
  const X_INT_INCRS = [1, 2, 5, 10, 20, 50, 100, 200, 500, 1000, 2000, 5000];

  // Un seul ResizeObserver module-level pour tous les graphiques (uPlot ne se redimensionne pas seul).
  // Re-mesure débouncée (~100 ms) : Safari iOS déclenche des rafales de reflows
  // (barre d'URL, rotation) et setSize() re-rend tout le graphe. setSize() n'est
  // appelé que si les dimensions sont > 0 et ont réellement changé.
  const UPLOT_RESIZE_DEBOUNCE_MS = 100;
  const _uplotResizeMap = new Map(); // HTMLElement -> { u, timer, w, h }

  function _applyChartSize(el) {
    const entry = _uplotResizeMap.get(el);
    if (!entry) return;
    entry.timer = null;
    const w = el.clientWidth;
    const h = el.clientHeight;
    if (w > 0 && h > 0 && (w !== entry.w || h !== entry.h)) {
      entry.w = w;
      entry.h = h;
      entry.u.setSize({ width: w, height: h });
    }
  }

  const _uplotResizeObserver = (typeof ResizeObserver !== 'undefined')
    ? new ResizeObserver(entries => {
        for (const e of entries) {
          const entry = _uplotResizeMap.get(e.target);
          if (!entry) continue;
          if (entry.timer) clearTimeout(entry.timer);
          entry.timer = setTimeout(() => _applyChartSize(e.target), UPLOT_RESIZE_DEBOUNCE_MS);
        }
      })
    : null;

  // w/h initialisés à 0 : le premier callback de l'observer (émis dès observe())
  // re-synchronise la taille réelle, corrigeant le repli Math.max(…, 50) si le
  // conteneur avait une taille nulle/provisoire à la création.
  function observeChart(el, u) {
    _uplotResizeMap.set(el, { u, timer: null, w: 0, h: 0 });
    _uplotResizeObserver?.observe(el);
  }

  // À appeler avant u.destroy() si un graphe venait à être recréé (aucun site
  // aujourd'hui : les graphes vivent toute la session — SPA sans démontage).
  function unobserveChart(el) {
    const entry = _uplotResizeMap.get(el);
    if (!entry) return;
    if (entry.timer) clearTimeout(entry.timer);
    _uplotResizeMap.delete(el);
    _uplotResizeObserver?.unobserve(el);
  }

  // Curseur uPlot : pas de crosshair ni de drag-zoom (parité Chart.js).
  // pointsFilter(u, seriesIdx) → bool : limite le point de survol à certaines séries
  // (uPlot attend un HTMLElement en retour de cursor.points.show, ou falsy pour masquer)
  function makeUplotCursor(pointsFilter) {
    const cursor = { x: false, y: false, drag: { x: false, y: false, setScale: false } };
    if (pointsFilter) {
      cursor.points = {
        show: (u, seriesIdx) => {
          if (!pointsFilter(u, seriesIdx)) return false;
          // Réplique du point de survol par défaut d'uPlot (couleurs appliquées par uPlot)
          const size = u.cursor.points.size(u, seriesIdx);
          const pt = document.createElement('div');
          pt.style.width = size + 'px';
          pt.style.height = size + 'px';
          pt.style.marginLeft = (-size / 2) + 'px';
          pt.style.marginTop = (-size / 2) + 'px';
          return pt;
        }
      };
    }
    return cursor;
  }

  // Plage X pour axe catégoriel : gère série vide et point unique
  function xAxisRange(u, dataMin, dataMax) {
    if (dataMin == null) return [0, 1];
    return (dataMax - dataMin) < 1 ? [dataMin, dataMin + 1] : [dataMin, dataMax];
  }

  // Formatte les ticks X depuis le tableau de labels catégoriels (conservés tels quels)
  function xLabelValues(getLabels) {
    return (u, splits) => {
      const labels = getLabels();
      return splits.map(i => (Number.isInteger(i) ? (labels[i] ?? '') : ''));
    };
  }

  // Plugin tooltip (uPlot n'a pas de tooltip riche natif)
  // variant 'mini' : style des mini-charts dashboard ; défaut : style sobre type Chart.js
  // title(u, idx) → string ; rows(u, idx) → [{ text, color? }]
  function tooltipPlugin({ variant = 'default', title, rows }) {
    let tt = null;
    return {
      hooks: {
        init(u) {
          tt = document.createElement('div');
          tt.className = 'u-tooltip' + (variant === 'mini' ? ' u-tooltip--mini' : '');
          tt.style.display = 'none';
          u.over.appendChild(tt);
        },
        setData(u) {
          if (tt) tt.style.display = 'none';
        },
        setCursor(u) {
          if (!tt) return;
          const { idx, left, top } = u.cursor;
          if (idx == null || left == null || left < 0 || top == null || top < 0) {
            tt.style.display = 'none';
            return;
          }
          const lines = rows(u, idx);
          if (!lines.length) { tt.style.display = 'none'; return; }
          tt.textContent = '';
          const titleText = title ? title(u, idx) : '';
          if (titleText) {
            const t = document.createElement('div');
            t.className = 'u-tooltip__title';
            t.textContent = titleText;
            tt.appendChild(t);
          }
          lines.forEach(line => {
            const row = document.createElement('div');
            row.className = 'u-tooltip__row';
            if (line.color) {
              const swatch = document.createElement('span');
              swatch.className = 'u-tooltip__swatch';
              swatch.style.background = line.color;
              row.appendChild(swatch);
            }
            row.appendChild(document.createTextNode(line.text));
            tt.appendChild(row);
          });
          tt.style.display = 'block';
          // Position près du curseur, bornée aux limites du graphe
          const ow = u.over.clientWidth, oh = u.over.clientHeight;
          const tw = tt.offsetWidth, th = tt.offsetHeight;
          let x = left + 12, y = top + 12;
          if (x + tw > ow) x = left - tw - 12;
          if (y + th > oh) y = top - th - 12;
          x = Math.max(0, Math.min(x, ow - tw));
          y = Math.max(0, Math.min(y, oh - th));
          tt.style.transform = `translate(${x}px, ${y}px)`;
        },
        destroy() {
          tt = null;
        }
      }
    };
  }

  // Espacement max entre 2 points consécutifs (90 min) avant coupure de courbe :
  // au-delà (arrêt de filtration, coupure de courant), une entrée null est insérée
  // dans la série → uPlot lève le crayon (même convention que les mesures invalides)
  const CHART_GAP_SECONDS = 5400;

  // Type de point selon la granularité fournie par /get-history
  // (0 = mesure brute 5 min, 1 = moyenne horaire, 2 = moyenne journalière)
  const GRANULARITY_LABELS = ['mesure', 'moyenne horaire', 'moyenne journalière'];

  function granularityLabel(g) {
    return (g != null && GRANULARITY_LABELS[g] != null) ? GRANULARITY_LABELS[g] : '';
  }

  // Titre de tooltip exhaustif : date + heure TOUJOURS (jj/mm hh:mm) —
  // les étiquettes de l'axe X, elles, restent courtes (heure seule si aujourd'hui)
  function tooltipDateTime(timestamp) {
    if (timestamp == null) return '';
    const d = new Date(timestamp * 1000);
    return d.toLocaleDateString([], { day: '2-digit', month: '2-digit' }) + ' '
      + d.toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' });
  }

  function createMiniLineChart(hostId, getTarget, getTolerance, formatValue) {
    const host = document.getElementById(hostId);
    if (!host || typeof uPlot === 'undefined') return null;

    // État du graphique : labels catégoriels + valeurs + timestamps/granularités
    // (tableaux parallèles, pour le tooltip) + bornes Y de repli (série vide)
    const st = { labels: [], values: [], ts: [], gran: [], yMin: null, yMax: null };

    // Coloration conditionnelle par segment : gradient HORIZONTAL à hard stops,
    // couleur du segment i = getMiniChartRGB(moyenne des 2 points) — formule Chart.js exacte
    const strokeGradient = (u) => {
      const xs = u.data[0], ys = u.data[1];
      const n = xs.length;
      if (n === 0) return getMiniChartColor(null, getTarget(), getTolerance());
      if (n === 1) return getMiniChartColor(ys[0], getTarget(), getTolerance());
      const x0 = u.valToPos(xs[0], 'x', true);
      const x1 = u.valToPos(xs[n - 1], 'x', true);
      const span = (x1 - x0) || 1;
      const grad = u.ctx.createLinearGradient(x0, 0, x1, 0);
      const clamp01 = v => Math.max(0, Math.min(1, v));
      for (let i = 0; i < n - 1; i++) {
        const color = getMiniChartColor((ys[i] + ys[i + 1]) / 2, getTarget(), getTolerance());
        const p0 = clamp01((u.valToPos(xs[i], 'x', true) - x0) / span);
        const p1 = clamp01((u.valToPos(xs[i + 1], 'x', true) - x0) / span);
        grad.addColorStop(p0, color);
        grad.addColorStop(p1, color);
      }
      return grad;
    };

    // Remplissage : dégradé VERTICAL alpha 0.22 → 0.02, couleur = dernière valeur non nulle
    const fillGradient = (u) => {
      const ys = u.data[1];
      let lastVal = null;
      for (let i = ys.length - 1; i >= 0; i--) {
        if (ys[i] != null && !isNaN(ys[i])) { lastVal = ys[i]; break; }
      }
      const [r, g, b] = getMiniChartRGB(lastVal, getTarget(), getTolerance());
      const grad = u.ctx.createLinearGradient(0, u.bbox.top, 0, u.bbox.top + u.bbox.height);
      grad.addColorStop(0, `rgba(${r},${g},${b},0.22)`);
      grad.addColorStop(1, `rgba(${r},${g},${b},0.02)`);
      return grad;
    };

    const opts = {
      width: Math.max(host.clientWidth, 50),
      height: Math.max(host.clientHeight, 50),
      padding: [4, 2, 0, 2],
      legend: { show: false },
      cursor: (() => {
        const cursor = makeUplotCursor();
        // Le stroke de la série est un CanvasGradient (inutilisable comme couleur CSS) :
        // colorer le point de survol selon la valeur survolée
        const ptColor = (u2) => {
          const idx = u2.cursor.idx;
          const v = idx != null ? u2.data[1][idx] : null;
          return getMiniChartColor(v, getTarget(), getTolerance());
        };
        cursor.points = { stroke: ptColor, fill: ptColor };
        return cursor;
      })(),
      scales: {
        x: { time: false, range: xAxisRange },
        y: {
          range: (u, dataMin, dataMax) => {
            if (st.yMin != null) return [st.yMin, st.yMax]; // repli : cible ± 4×tolérance
            if (dataMin == null) return [0, 1];
            return uPlot.rangeNum(dataMin, dataMax, 0.1, true);
          }
        }
      },
      axes: [
        {
          scale: 'x',
          stroke: 'rgba(75,85,99,0.7)',
          font: `9px ${UPLOT_FONT_STACK}`,
          size: 16,
          gap: 3,
          grid: { show: false },
          ticks: { show: false },
          incrs: X_INT_INCRS,
          space: (u, axisIdx, scaleMin, scaleMax, plotDim) => Math.max(50, plotDim / 3), // ~4 ticks max
          values: xLabelValues(() => st.labels)
        },
        {
          scale: 'y',
          side: 1, // axe Y à droite
          stroke: 'rgba(75,85,99,0.7)',
          font: `9px ${UPLOT_FONT_STACK}`,
          size: 30,
          gap: 3,
          grid: { stroke: 'rgba(0,0,0,0.06)', width: 1 },
          ticks: { show: false },
          space: 30 // ~3 ticks
        }
      ],
      series: [
        {},
        {
          scale: 'y',
          width: 1.5,
          paths: uPlot.paths.spline(),
          points: { show: false },
          stroke: strokeGradient,
          fill: fillGradient
        }
      ],
      plugins: [
        tooltipPlugin({
          variant: 'mini',
          title: (u, idx) => tooltipDateTime(st.ts[idx]) || (st.labels[idx] ?? ''),
          rows: (u, idx) => {
            const v = u.data[1][idx];
            if (v == null || isNaN(v)) return [];
            const lines = [{ text: formatValue ? formatValue(v) : String(v) }];
            const type = granularityLabel(st.gran[idx]);
            if (type) lines.push({ text: type });
            return lines;
          }
        })
      ]
    };

    const u = new uPlot(opts, [[], []], host);
    observeChart(host, u);

    return {
      u,
      st,
      render() {
        u.setData([st.labels.map((_, i) => i), st.values]);
      }
    };
  }

  let lastMiniChartTimestamp = 0;
  // Minuit local (en secondes) du jour affiché par les mini-graphiques : sert à
  // détecter le passage de minuit entre deux rafraîchissements incrémentaux
  let miniChartDayStart = 0;

  // Minuit local du jour courant, en secondes epoch
  function startOfTodaySeconds() {
    const d = new Date();
    d.setHours(0, 0, 0, 0);
    return Math.floor(d.getTime() / 1000);
  }

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
      // Les mini-graphiques n'affichent que la journée en cours : tout point
      // antérieur à minuit local est écarté côté navigateur (la requête reste
      // range=24h, aucun changement de contrat HTTP)
      const dayStart = startOfTodaySeconds();
      if (dayStart !== miniChartDayStart) {
        // Premier appel ou passage de minuit : rechargement complet (remplacement)
        // pour que les points de la veille disparaissent sans action utilisateur
        miniChartDayStart = dayStart;
        lastMiniChartTimestamp = 0;
        since = 0;
      }
      const url = since > 0
        ? `/get-history?range=24h&since=${since}`
        : '/get-history?range=24h';
      const resp = await authFetch(url);
      if (!resp.ok) return;
      const data = await resp.json();
      if (!data.history) return;

      // Tableaux parallèles partagés par les 3 mini-charts (mêmes points sources) ;
      // updateMiniChart/appendMiniChart copient pour que chaque graphe reste indépendant
      const labels = [], ts = [], gran = [];
      const phValues = [], orpValues = [], tempValues = [];
      let maxTs = since;
      // Coupure de courbe : sur le chemin incrémental, le « point précédent » du
      // premier nouveau point est le dernier point déjà affiché — un arrêt entre
      // l'existant et le nouveau crée aussi un trou
      let prevTs = (since > 0 && lastMiniChartTimestamp >= dayStart) ? lastMiniChartTimestamp : null;

      data.history.forEach(point => {
        // Filtrage AVANT le calcul des coupures : sinon le premier point du jour
        // créerait un faux trou avec le dernier point de la veille
        if (point.timestamp < dayStart) return;
        if (point.timestamp > maxTs) maxTs = point.timestamp;
        // Espacement > 90 min avec le point précédent → entrée null (uPlot lève le crayon)
        if (prevTs != null && point.timestamp - prevTs > CHART_GAP_SECONDS) {
          labels.push(''); ts.push(null); gran.push(null);
          phValues.push(null); orpValues.push(null); tempValues.push(null);
        }
        prevTs = point.timestamp;
        // feature-061 : pH/ORP/température null (filtration arrêtée) → pousser null,
        // pas filtrer : uPlot lève le crayon sur null (trou visible), y compris sur
        // le chemin incrémental appendMiniChart. Température incluse en itération 2.
        labels.push(miniChartPointLabel(point.timestamp));
        ts.push(point.timestamp);
        gran.push(point.granularity ?? 0);
        phValues.push((point.ph != null && !isNaN(point.ph)) ? Math.round(point.ph * 10) / 10 : null);
        orpValues.push((point.orp != null && !isNaN(point.orp)) ? Math.round(point.orp) : null);
        tempValues.push((point.temperature != null && !isNaN(point.temperature))
          ? Math.round(point.temperature * 10) / 10 : null);
      });

      if (since === 0) {
        // Chargement initial : remplace toutes les données
        updateMiniChart(phMiniChart, labels, phValues, ts, gran, window._config?.ph_target ?? 7.2, 0.2);
        updateMiniChart(orpMiniChart, labels, orpValues, ts, gran, window._config?.orp_target ?? 700, 50);
        updateMiniChart(tempMiniChart, labels, tempValues, ts, gran, 26, 8);
      } else {
        // Mise à jour incrémentale : ajoute les nouveaux points
        appendMiniChart(phMiniChart, labels, phValues, ts, gran, window._config?.ph_target ?? 7.2, 0.2);
        appendMiniChart(orpMiniChart, labels, orpValues, ts, gran, window._config?.orp_target ?? 700, 50);
        appendMiniChart(tempMiniChart, labels, tempValues, ts, gran, 26, 8);
      }

      if (maxTs > lastMiniChartTimestamp) lastMiniChartTimestamp = maxTs;
    } catch (e) {
      console.error('Error loading mini chart data:', e);
    }
  }

  // Quand il n'y a pas de données, définir une plage Y sensée autour de la cible
  function applyMiniChartFallbackScale(chart, target, tolerance) {
    const hasData = chart.st.values.some(v => v != null && !isNaN(v));
    if (!hasData && target != null && tolerance != null) {
      chart.st.yMin = target - tolerance * 4;
      chart.st.yMax = target + tolerance * 4;
    } else {
      chart.st.yMin = null;
      chart.st.yMax = null;
    }
  }

  function appendMiniChart(chart, newLabels, newValues, newTs, newGran, target, tolerance) {
    if (!chart || !newLabels.length) return;
    chart.st.labels.push(...newLabels);
    chart.st.values.push(...newValues);
    chart.st.ts.push(...newTs);
    chart.st.gran.push(...newGran);
    applyMiniChartFallbackScale(chart, target, tolerance);
    chart.render();
  }

  function updateMiniChart(chart, labels, values, ts, gran, target, tolerance) {
    if (!chart) return;
    // slice() : labels/ts/gran sont partagés entre les 3 mini-charts — chaque
    // graphe doit posséder sa propre copie (appendMiniChart mute st.* par push)
    chart.st.labels = labels.slice();
    chart.st.values = values;
    chart.st.ts = ts.slice();
    chart.st.gran = gran.slice();
    applyMiniChartFallbackScale(chart, target, tolerance);
    chart.render();
  }

  // Usine des graphiques détail (Température / pH / ORP)
  // referenceLines : { max, min, axisMin, axisMax, zoneColor, lineColor } → lignes + zones
  // hors plage (bandes uPlot) avec bornes Y dynamiques via calculateAxisLimits
  function createLineChart(host, color, label, options = {}) {
    if (!host || typeof uPlot === 'undefined') return null;
    const { integerOnly = false, fill = true, referenceLines = null } = options;

    const hasBands = referenceLines != null;
    const measureIdx = hasBands ? 5 : 1;
    const st = {
      labels: [],
      values: [],
      ts: [],   // timestamp du dernier point contribuant à chaque entrée (tooltip)
      gran: [], // granularité de ce point (0/1/2)
      yMin: hasBands ? referenceLines.axisMin : null,
      yMax: hasBands ? referenceLines.axisMax : null
    };

    // Séries invisibles portant les bornes des zones hors plage
    const boundLine = () => ({
      scale: 'y',
      stroke: 'transparent',
      width: 0,
      points: { show: false }
    });
    // Lignes de référence min/max
    const refLine = () => ({
      scale: 'y',
      stroke: referenceLines.lineColor,
      width: 2,
      points: { show: false }
    });

    const measureSeries = {
      label,
      scale: 'y',
      stroke: color,
      width: 2,
      paths: uPlot.paths.spline(),
      // Points visibles rayon 3 (défaut Chart.js)
      points: { show: true, size: 6, width: 1, stroke: color, fill: color + '20' }
    };
    if (fill) measureSeries.fill = color + '20';

    const series = hasBands
      ? [{}, boundLine(), refLine(), refLine(), boundLine(), measureSeries]
      : [{}, measureSeries];

    const yAxis = {
      scale: 'y',
      stroke: '#4b5563',
      font: `12px ${UPLOT_FONT_STACK}`,
      grid: { stroke: 'rgba(0,0,0,0.05)', width: 1 },
      ticks: { stroke: 'rgba(0,0,0,0.05)', width: 1 }
    };
    if (integerOnly) {
      // Parité Chart.js : n'afficher que les ticks entiers
      yAxis.values = (u, splits) => splits.map(v => (Number.isInteger(v) ? v : ''));
    }

    const opts = {
      width: Math.max(host.clientWidth, 50),
      height: Math.max(host.clientHeight, 50),
      legend: { show: false },
      cursor: makeUplotCursor((u, seriesIdx) => seriesIdx === measureIdx),
      scales: {
        x: { time: false, range: xAxisRange },
        y: {
          range: (u, dataMin, dataMax) => {
            if (st.yMin != null) return [st.yMin, st.yMax];
            if (dataMin == null) return [0, 1];
            return uPlot.rangeNum(dataMin, dataMax, 0.1, true);
          }
        }
      },
      bands: hasBands
        ? [
            { series: [1, 2], fill: referenceLines.zoneColor }, // zone hors plage (haute)
            { series: [3, 4], fill: referenceLines.zoneColor }  // zone hors plage (basse)
          ]
        : undefined,
      axes: [
        {
          scale: 'x',
          stroke: '#4b5563',
          font: `12px ${UPLOT_FONT_STACK}`,
          grid: { stroke: 'rgba(0,0,0,0.05)', width: 1 },
          ticks: { stroke: 'rgba(0,0,0,0.05)', width: 1 },
          incrs: X_INT_INCRS,
          space: 70,
          values: xLabelValues(() => st.labels)
        },
        yAxis
      ],
      series,
      plugins: [
        tooltipPlugin({
          title: (u, idx) => tooltipDateTime(st.ts[idx]) || (st.labels[idx] ?? ''),
          rows: (u, idx) => {
            const v = u.data[measureIdx][idx];
            if (v == null || isNaN(v)) return [];
            const lines = [{ color, text: `${label}: ${v}` }];
            const type = granularityLabel(st.gran[idx]);
            if (type) lines.push({ text: type });
            return lines;
          }
        })
      ]
    };

    const initialData = hasBands ? [[], [], [], [], [], []] : [[], []];
    const u = new uPlot(opts, initialData, host);
    observeChart(host, u);

    return {
      u,
      st,
      // Reconstruit les données columnar + bornes Y dynamiques (les zones suivent)
      render() {
        const xs = st.labels.map((_, i) => i);
        if (hasBands) {
          const limits = calculateAxisLimits(st.values, referenceLines.axisMin, referenceLines.axisMax);
          st.yMin = limits.min;
          st.yMax = limits.max;
          const constant = (v) => Array(xs.length).fill(v);
          u.setData([
            xs,
            constant(limits.max),          // borne haute de la zone haute
            constant(referenceLines.max),  // ligne max
            constant(referenceLines.min),  // ligne min
            constant(limits.min),          // borne basse de la zone basse
            st.values
          ]);
        } else {
          u.setData([xs, st.values]);
        }
      },
      // ts/gran optionnels (tableaux parallèles pour le tooltip) : les appels
      // existants sans ces arguments (ex. clearHistoryCharts) restent valides
      setPoints(labels, values, ts, gran) {
        st.labels = labels;
        st.values = values;
        st.ts = ts || labels.map(() => null);
        st.gran = gran || labels.map(() => null);
        this.render();
      }
    };
  }

  // Graphe uPlot à 2 séries pour la trace des signaux capteurs (feature-057).
  // Axe X numérique (secondes relatives). Les null créent des trous (uPlot les gère).
  // seriesDefs : [{ label, color }, ...] + options par série (feature-072,
  // rétro-compatibles — défauts = comportement historique) :
  //   width (défaut 2), dash (pointillés), linear (paths linéaires au lieu de
  //   spline — obligatoire pour la dispersion : un point valide isolé entre
  //   deux trous doit rester visible), points (point visible taille 4),
  //   noTooltip (exclu du tooltip et du curseur), tooltip(v) → texte custom.
  // opts.yInclude : seuil mutable TOUJOURS inclus dans l'échelle Y
  // (jamais d'autorange qui zoomerait sous le seuil).
  function createTraceChart(host, seriesDefs, opts = {}) {
    if (!host || typeof uPlot === 'undefined') return null;
    const st = { xs: [], yInclude: opts.yInclude ?? null };

    const series = [{}];
    seriesDefs.forEach(sd => {
      series.push({
        label: sd.label,
        scale: 'y',
        stroke: sd.color,
        width: sd.width ?? 2,
        dash: sd.dash,
        paths: sd.linear ? uPlot.paths.linear() : uPlot.paths.spline(),
        points: sd.points ? { show: true, size: 4 } : { show: false }
      });
    });

    const gridStroke = 'rgba(0,0,0,0.05)';
    const uOpts = {
      width: Math.max(host.clientWidth, 50),
      height: Math.max(host.clientHeight, 50),
      legend: { show: false },
      cursor: makeUplotCursor((u, seriesIdx) => seriesIdx > 0 && !seriesDefs[seriesIdx - 1].noTooltip),
      scales: {
        x: { time: false },
        y: {
          range: (u, dataMin, dataMax) => {
            // Seuil mutable toujours dans le cadre : la marge sous le seuil
            // se lit graphiquement même quand la courbe est très basse
            if (st.yInclude != null) return [0, Math.max(dataMax ?? st.yInclude, st.yInclude) * 1.1];
            if (dataMin == null) return [0, 1];
            return uPlot.rangeNum(dataMin, dataMax, 0.1, true);
          }
        }
      },
      axes: [
        {
          scale: 'x',
          stroke: '#4b5563',
          font: `12px ${UPLOT_FONT_STACK}`,
          grid: { stroke: gridStroke, width: 1 },
          ticks: { stroke: gridStroke, width: 1 },
          space: 60,
          values: (u, splits) => splits.map(v => (v != null ? `${v}s` : ''))
        },
        {
          scale: 'y',
          stroke: '#4b5563',
          font: `12px ${UPLOT_FONT_STACK}`,
          grid: { stroke: gridStroke, width: 1 },
          ticks: { stroke: gridStroke, width: 1 }
        }
      ],
      series,
      plugins: [
        tooltipPlugin({
          title: (u, idx) => (st.xs[idx] != null ? `${st.xs[idx]} s` : ''),
          rows: (u, idx) => seriesDefs.map((sd, si) => {
            if (sd.noTooltip) return null;
            const v = u.data[si + 1][idx];
            if (v == null || isNaN(v)) return null;
            return { color: sd.color, text: sd.tooltip ? sd.tooltip(v) : `${sd.label}: ${v}` };
          }).filter(Boolean)
        })
      ]
    };

    const initialData = [[], ...seriesDefs.map(() => [])];
    const u = new uPlot(uOpts, initialData, host);
    observeChart(host, u);

    return {
      u,
      st,
      setData(xs, seriesData) {
        st.xs = xs;
        u.setData([xs, ...seriesData]);
      },
      // Change le seuil inclus dans l'échelle Y ; pris en compte au prochain
      // setData (le range est une fonction, réévaluée à chaque setData)
      setYInclude(v) {
        st.yInclude = v;
      }
    };
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

      // Entrée journalière vierge : valeurs + timestamp/granularité PAR capteur
      // (le tooltip de chaque graphe affiche la date/heure du point qui a fourni SA valeur)
      const emptyDay = (label) => ({
        label, ph: null, orp: null, temperature: null,
        phTs: null, phG: null, orpTs: null, orpG: null, tempTs: null, tempG: null
      });

      history.forEach(point => {
        const d = new Date(point.timestamp * 1000);
        if (isNaN(d.getTime())) return;
        const key = `${d.getFullYear()}-${String(d.getMonth()+1).padStart(2,'0')}-${String(d.getDate()).padStart(2,'0')}`;
        if (!dayMap.has(key)) {
          const isToday = key === todayKey;
          const label = isToday
            ? "Aujourd'hui"
            : `${d.getDate()} ${MOIS_FR[d.getMonth()]}`;
          dayMap.set(key, emptyDay(label));
        }
        const entry = dayMap.get(key);
        const g = point.granularity ?? 0;
        if (point.ph != null && !isNaN(point.ph)) {
          entry.ph = Math.round(point.ph * 10) / 10;
          entry.phTs = point.timestamp;
          entry.phG = g;
        }
        if (point.orp != null && !isNaN(point.orp)) {
          entry.orp = Math.round(point.orp);
          entry.orpTs = point.timestamp;
          entry.orpG = g;
        }
        if (point.temperature != null && !isNaN(point.temperature)) {
          entry.temperature = point.temperature;
          entry.tempTs = point.timestamp;
          entry.tempG = g;
        }
      });

      // Si aujourd'hui absent de l'historique mais données temps réel disponibles, l'injecter
      // feature-061 : mesures non représentatives (filtration arrêtée) → null (temp. incluse, it. 2)
      if (!dayMap.has(todayKey) && latestSensorData) {
        const s = latestSensorData;
        const liveOk = measuresAreValid(s);
        const liveTs = Math.floor(Date.now() / 1000);
        const entry = emptyDay("Aujourd'hui");
        if (liveOk && s.ph != null) {
          entry.ph = Math.round(s.ph * 10) / 10;
          entry.phTs = liveTs;
          entry.phG = 0;
        }
        if (liveOk && s.orp != null) {
          entry.orp = Math.round(s.orp);
          entry.orpTs = liveTs;
          entry.orpG = 0;
        }
        if (liveOk && s.temperature != null) {
          entry.temperature = s.temperature;
          entry.tempTs = liveTs;
          entry.tempG = 0;
        }
        dayMap.set(todayKey, entry);
      }

      // Trier par date croissante — tri lexicographique direct sur YYYY-MM-DD
      const sortedDays = [...dayMap.entries()]
        .sort((a, b) => (a[0] < b[0] ? -1 : a[0] > b[0] ? 1 : 0));

      // Coupure de courbe sur les écrans détaillés (résolution : 1 point/jour) :
      // le seuil brut de 90 min (CHART_GAP_SECONDS) est inapplicable ici — deux
      // jours consécutifs sont toujours espacés de ~24 h. L'équivalent : un jour
      // calendaire entièrement absent entre deux jours présents → entrée null
      // insérée (uPlot lève le crayon, l'arrêt prolongé se voit).
      const dayKeyToUtc = (key) => {
        const [y, m, d] = key.split('-').map(Number);
        return Date.UTC(y, m - 1, d);
      };
      const entries = [];
      let prevKey = null;
      sortedDays.forEach(([key, e]) => {
        if (prevKey != null && (dayKeyToUtc(key) - dayKeyToUtc(prevKey)) > 86400000) {
          entries.push(emptyDay(''));
        }
        prevKey = key;
        entries.push(e);
      });

      // feature-061 : jours sans aucune mesure valide (filtration arrêtée) → null
      // conservé dans la série (trou uPlot) au lieu de sauter le jour.
      // Itération 2 : la température suit le même mécanisme que pH/ORP.
      if (tempChart) {
        tempChart.setPoints(entries.map(e => e.label), entries.map(e => e.temperature),
          entries.map(e => e.tempTs), entries.map(e => e.tempG));
      }

      if (phChart) {
        phChart.setPoints(entries.map(e => e.label), entries.map(e => e.ph),
          entries.map(e => e.phTs), entries.map(e => e.phG));
      }

      if (orpChart) {
        orpChart.setPoints(entries.map(e => e.label), entries.map(e => e.orp),
          entries.map(e => e.orpTs), entries.map(e => e.orpG));
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
      const st = chart.st;
      // Tenir ts/gran parallèles aux labels : point live = mesure datée de maintenant
      const nowTs = Math.floor(Date.now() / 1000);
      if (st.labels.length > 0 && st.labels[st.labels.length - 1] === todayLabel) {
        st.values[st.values.length - 1] = value;
        st.ts[st.ts.length - 1] = nowTs;
        st.gran[st.gran.length - 1] = 0;
      } else {
        st.labels.push(todayLabel);
        st.values.push(value);
        st.ts.push(nowTs);
        st.gran.push(0);
      }
      chart.render();
    }

    // feature-061 : mesure live non représentative (filtration arrêtée / stabilisation)
    // → ne pas pousser sur le point « Aujourd'hui » (la dernière mesure valide du jour reste).
    // Itération 2 : température incluse.
    const liveOk = measuresAreValid(sensorData);
    if (liveOk && tempChart && sensorData.temperature != null) {
      updateChart(tempChart, sensorData.temperature);
    }
    if (liveOk && !phCalibrationActive && phChart && sensorData.ph != null) {
      updateChart(phChart, Math.round(sensorData.ph * 10) / 10);
    }
    if (liveOk && !orpCalibrationActive && orpChart && sensorData.orp != null) {
      updateChart(orpChart, Math.round(sensorData.orp));
    }
  }

  let tempChart, phChart, orpChart;
  let phMiniChart = null, orpMiniChart = null, tempMiniChart = null;
  let currentHistoryRange = 'all';

  // ---------- State ----------
  let latestSensorData = null;
  // bug-dosage-fantome it.3 : ancre des décomptes du widget dosing-status.
  // Snapshot IMMUABLE pris à la réception des données (push WS OU poll /data) :
  // { atMs: Date.now(), ph/orp: { manualS, phaseMs } }. Les valeurs sont figées
  // ici car injectInterval décrémente *_inject_remaining_s en place sur l'objet
  // json — relire latestSensorData ferait décompter deux fois.
  let dosingAnchor = null;
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

  // feature-067 : variante de sendConfig() qui REMONTE le message d'erreur du
  // serveur. /save-config peut refuser une valeur (HTTP 400 + {"error":"..."}) —
  // il ne faut jamais laisser croire à l'utilisateur que c'est enregistré.
  async function sendConfigDetailed(data) {
    try {
      const res = await authFetch("/save-config", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify(data),
      });
      if (res.ok) return { ok: true, error: "", status: res.status, message: "" };
      let msg = "";
      let detail = "";
      try {
        const j = await res.json();
        if (j && j.error) msg = String(j.error);
        // v2.25.0 : certains refus portent une phrase d'explication complète en
        // plus du libellé court (`error`) — ex. le 409 du gate de sécurité pH.
        if (j && j.message) detail = String(j.message);
      } catch (e) { /* corps non JSON */ }
      return {
        ok: false,
        error: msg || `Enregistrement refusé (HTTP ${res.status}).`,
        status: res.status,
        message: detail,
      };
    } catch (e) {
      // Hors ligne / contrôleur injoignable : surtout ne pas afficher « réussi ».
      return {
        ok: false,
        error: "Contrôleur injoignable — modification non enregistrée.",
        status: 0,
        message: "",
      };
    }
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
      // feature-056 : la visibilité de la filtration est gérée par applyInstallMode() (mode d'installation)
      lighting: "lighting_feature_enabled",
      temperature: "temperature_enabled"
      // orp n'est plus un toggle binaire — la visibilité est gérée par updateOrpModeControls()
    };
    const contentMap = {
      filtration: "filtration-content",
      lighting: "lighting-content"
      // temperature : plusieurs cartes (stats/historique/calibration), géré ci-dessous —
      // la carte d'activation reste visible pour pouvoir réactiver la mesure.
    };
    const dashboardCardMap = {
      filtration: "dashboard-filtration-card",
      lighting: "dashboard-lighting-card",
      temperature: "dashboard-temperature-card"
    };

    const switchId = switchMap[feature];
    if (!switchId) return;

    const switchEl = $(`#${switchId}`);
    if (!switchEl) return;
    const enabled = switchEl.checked;

    const dashboardCardId = dashboardCardMap[feature];
    const dashboardCardEl = dashboardCardId ? $(`#${dashboardCardId}`) : null;
    if (dashboardCardEl) {
      dashboardCardEl.style.display = enabled ? "" : "none";
    }

    if (feature === "temperature") {
      // Pas de wrapper unique : masquer les cartes de mesure/calibration quand la
      // sonde est désactivée, tout en gardant la carte d'activation accessible.
      ["temperature-card-stats", "temperature-card-history"].forEach((id) => {
        const el = $(`#${id}`);
        if (el) el.style.display = enabled ? "" : "none";
      });
      if (!enabled) {
        const calCard = $("#temperature-card-calibration");
        if (calCard) calCard.style.display = "none";
      }
      return;
    }

    const contentId = contentMap[feature];
    const contentEl = contentId ? $(`#${contentId}`) : null;
    if (!contentEl) return;
    contentEl.style.display = enabled ? "block" : "none";
  }

  function updatePhModeControls() {
    const mode = getSegmented("ph_regulation_mode") || "automatic";
    const params = $("#ph-regulation-params");      // sous-bloc Auto
    const scheduled = $("#ph-params-scheduled");    // sous-bloc Programmée
    const inject = $("#ph-inject-section");         // Injection manuelle

    if (params)    params.style.display    = mode === "automatic"  ? "" : "none";
    if (scheduled) scheduled.style.display = mode === "scheduled"  ? "" : "none";
    if (inject)    inject.style.display    = mode === "manual"     ? "" : "none";
    // v2.26.0 (C5) : le bloc « rattrapage accumulé » dépend AUSSI de la
    // disponibilité de la donnée — sa visibilité est décidée en un seul endroit.
    updatePhIntegralDisplay();
    // feature-034 itération 3 (volet A) : le bouton Calibrer la sonde vit désormais
    // sous la rangée des badges (#ph-card-stats, toujours visible) → accessible
    // dans tous les modes par construction, sans gestion de display ici.
  }

  function updatePhControls() {
    updatePhModeControls();
  }

  // ---------- feature-067 : offset logiciel de mesure pH ----------
  // pH corrigé = pH brut EZO + offset.
  const PH_OFFSET_WARN1 = 0.15;  // palier 1 : avertissement de décalage de régulation
  const PH_OFFSET_WARN2 = 0.30;  // palier 2 : offset élevé, recalibration recommandée

  // ---------- v2.25.0 (B1) : bornes SERVIES PAR LE SERVEUR ----------
  // `/get-config` publie ph_target_min/_max/_step et ph_offset_min/_max/_step,
  // dérivés des constantes firmware (constants.h). Le client n'en code AUCUNE en
  // dur : élargir la plage ou passer de 4 à 5 positions se fait côté firmware,
  // sans retouche de l'UI. Aucun miroir de seuil de sécurité (plancher, plafond,
  // inhibition ORP) ne subsiste ici : ces seuils sont FIXES côté firmware et ne
  // dépendent plus de l'offset — l'UI n'a plus à les recalculer.
  //
  // Le repli ci-dessous n'est qu'une valeur de secours pour un firmware antérieur
  // qui ne servirait pas encore ces champs : jamais une autorité.
  const PH_BOUNDS_FALLBACK = Object.freeze({
    targetMin: 7.20, targetMax: 7.50, targetStep: 0.10,
    offsetMin: -0.50, offsetMax: 0.50, offsetStep: 0.10,
  });
  let _phBounds = Object.assign({}, PH_BOUNDS_FALLBACK);

  const _phNum = (v) => (typeof v === "number" && isFinite(v)) ? v : NaN;
  // Arrondi au centième : neutralise la dérive flottante des additions de pas
  // (0.1 + 0.2 = 0.30000000000000004).
  const _phRound = (v) => Math.round(v * 100) / 100;
  const PH_EPS = 1e-9;

  // Reprend les bornes de la config. Un lot incohérent (borne inversée, pas nul)
  // ou ABSENT est ignoré EN BLOC : on conserve les bornes déjà connues. C'est
  // indispensable — la trame de config du WebSocket ne transporte PAS ces bornes
  // (statiques, budget de trame) et ne doit donc pas les faire retomber sur le
  // repli entre deux appels à /get-config.
  function _applyPhBounds(cfg) {
    const next = Object.assign({}, _phBounds);
    const tMin = _phNum(cfg?.ph_target_min);
    const tMax = _phNum(cfg?.ph_target_max);
    const tStep = _phNum(cfg?.ph_target_step);
    if (isFinite(tMin) && isFinite(tMax) && isFinite(tStep) && tMin < tMax && tStep > 0) {
      next.targetMin = tMin; next.targetMax = tMax; next.targetStep = tStep;
    }
    const oMin = _phNum(cfg?.ph_offset_min);
    const oMax = _phNum(cfg?.ph_offset_max);
    const oStep = _phNum(cfg?.ph_offset_step);
    if (isFinite(oMin) && isFinite(oMax) && isFinite(oStep) && oMin < oMax && oStep > 0) {
      next.offsetMin = oMin; next.offsetMax = oMax; next.offsetStep = oStep;
    }
    _phBounds = next;
  }

  // Positions du contrôle segmenté de consigne, calculées depuis les bornes.
  // Le nombre de positions est borné (garde-fou : un pas aberrant servi par un
  // firmware corrompu ne doit pas fabriquer des milliers de boutons).
  function _phTargetValues() {
    const { targetMin, targetMax, targetStep } = _phBounds;
    let n = Math.round((targetMax - targetMin) / targetStep);
    if (!isFinite(n) || n < 1) n = 1;
    if (n > 20) n = 20;
    const out = [];
    for (let i = 0; i <= n; i++) out.push(_phRound(targetMin + i * targetStep));
    return out;
  }

  // Offset actuellement appliqué par le firmware (config, pas le champ de saisie).
  function _phOffsetValue() {
    const v = window._config?.ph_offset;
    return (typeof v === "number" && isFinite(v)) ? v : 0;
  }

  // Formatage français : virgule décimale, 2 décimales, signe explicite si demandé.
  function _fmtPh(v, signed) {
    const s = Math.abs(v).toFixed(2).replace(".", ",");
    if (signed) return (v < 0 ? "−" : "+") + s;
    return (v < 0 ? "−" : "") + s;
  }

  // Avertissements imposés par la revue de sécurité chimique. Calculés sur la
  // valeur EN COURS DE SAISIE (pas sur la config enregistrée) : l'utilisateur doit
  // être prévenu AVANT d'enregistrer.
  function renderPhOffsetWarning() {
    const box = $("#ph_offset_warning");
    if (!box) return;
    const raw = $("#ph_offset")?.value;
    const off = parseFloat(raw);
    if (raw === "" || !isFinite(off) || Math.abs(off) < PH_OFFSET_WARN1) {
      box.style.display = "none";
      box.textContent = "";
      return;
    }
    // Consigne réelle de l'utilisateur : contrôle segmenté s'il est initialisé,
    // sinon valeur enregistrée, sinon borne basse de la plage servie.
    const fieldTarget = parseFloat($("#ph_target")?.value);
    const cfgTarget = window._config?.ph_target;
    const target = isFinite(fieldTarget) ? fieldTarget
                 : ((typeof cfgTarget === "number" && isFinite(cfgTarget))
                      ? cfgTarget : _phBounds.targetMin);
    const regulated = target - off;
    if (Math.abs(off) >= PH_OFFSET_WARN2) {
      // v2.25.0 : plus AUCUN seuil annoncé ici. Les seuils de sécurité sont
      // désormais FIXES et portent sur le pH AFFICHÉ (offset compris) : ils ne
      // dépendent plus de l'offset, il n'y a donc plus rien à recalculer côté
      // client — et donc plus de miroir de constante firmware à maintenir.
      box.textContent = `Offset élevé (${_fmtPh(off, true)} pH). Si cet offset est erroné, l'eau réelle `
        + `s'établira autour de ${_fmtPh(regulated, false)} au lieu de ${_fmtPh(target, false)}. `
        + `Au-delà de 0,30, la sonde doit être recalibrée plutôt que compensée. `
        + `Les seuils de sécurité du dosage portent sur le pH affiché (offset compris) : `
        + `un offset faux les déplace d'autant sur l'eau réelle.`;
    } else {
      box.textContent = `Attention : cet offset décale la régulation. Avec une consigne de `
        + `${_fmtPh(target, false)} et cet offset, le système régulera l'eau à un pH réel de `
        + `${_fmtPh(regulated, false)}. Un offset erroné entraîne un surdosage d'acide et une eau agressive. `
        + `Vérifiez votre mesure de référence avant d'enregistrer.`;
    }
    box.style.display = "";
  }

  // v2.25.0 — Aide sous le contrôle « pH cible ». La saisie hors plage étant
  // devenue IMPOSSIBLE (contrôle segmenté), ce texte n'est plus un avertissement
  // de validation : il rappelle la plage servie par le firmware et le référentiel
  // des seuils de sécurité. Le rouge est réservé aux ÉCHECS DE SAUVEGARDE (B5,
  // showPhTargetSaveError ci-dessous).
  function renderPhTargetHint() {
    const hint = $("#ph_target_hint");
    if (!hint) return;
    const { targetMin, targetMax, targetStep } = _phBounds;
    hint.style.color = "";
    hint.textContent = `Plage autorisée : ${_fmtPh(targetMin, false)} à ${_fmtPh(targetMax, false)}, `
      + `par pas de ${_fmtPh(targetStep, false)}. Les seuils de sécurité du dosage portent sur le pH `
      + `affiché (offset compris).`;
  }

  // B5 — Le hint rouge ne signale plus qu'un échec de SAUVEGARDE (contrôleur
  // injoignable, refus serveur). Effacé au prochain rendu du hint, donc dès que
  // l'utilisateur change de position ou qu'une sauvegarde réussit.
  function showPhTargetSaveError(msg) {
    const hint = $("#ph_target_hint");
    if (!hint) return;
    hint.style.color = "var(--danger)";
    hint.textContent = msg;
  }

  // Avertissement chimique attaché à la borne HAUTE de la plage : à cette
  // consigne le chlore est déjà nettement moins actif, et c'est précisément la
  // raison pour laquelle la plage s'arrête là. Aucun avertissement sur la borne
  // basse : 7,20 est une consigne d'exploitation normale (défaut du projet).
  function renderPhTargetWarning() {
    const box = $("#ph_target_warning");
    if (!box) return;
    const v = parseFloat($("#ph_target")?.value);
    if (isFinite(v) && v >= _phBounds.targetMax - PH_EPS) {
      box.textContent = `Consigne à la borne haute (${_fmtPh(_phBounds.targetMax, false)}) : à ce pH, `
        + `le chlore est nettement moins actif et la désinfection perd en efficacité. `
        + `Au-delà, le dosage de chlore serait suspendu — c'est pourquoi la plage s'arrête ici.`;
      box.hidden = false;
    } else {
      box.hidden = true;
      box.textContent = "";
    }
  }

  // ---------- v2.25.0 : consigne pH (contrôle segmenté généré) ----------
  const _phTargetButtons = () => $$("#ph_target_group .segmented__btn");

  // (Re)génère les positions depuis les bornes serveur. Idempotent : on ne
  // reconstruit que si la liste des valeurs a changé, pour ne pas détruire le
  // bouton qui a le focus à chaque trame de config.
  function buildPhTargetSegments() {
    const group = $("#ph_target_group");
    if (!group) return;
    const values = _phTargetValues();
    const signature = values.join(",");
    if (group.dataset.signature === signature) return;
    group.dataset.signature = signature;
    group.textContent = "";
    values.forEach((v) => {
      const btn = document.createElement("button");
      btn.type = "button";
      btn.className = "segmented__btn";
      btn.setAttribute("role", "radio");
      btn.setAttribute("aria-checked", "false");
      btn.tabIndex = -1;  // roving tabindex : la position active passera à 0
      btn.dataset.value = String(v);       // format machine (point décimal)
      btn.textContent = _fmtPh(v, false);  // affichage français (virgule)
      group.appendChild(btn);
    });
    // Une reconstruction efface la sélection : la rétablir TOUT DE SUITE depuis le
    // porteur de valeur, y compris si l'utilisateur était en train de modifier —
    // « aucune position sélectionnée » est un état interdit (B7).
    const current = parseFloat($("#ph_target")?.value);
    if (isFinite(current)) setPhTarget(current, true);
  }

  // Sélectionne la position correspondant à `value` et met à jour le porteur de
  // valeur caché. `silent` = écriture programmatique (chargement de config) : ni
  // `input` ni `change` ne sont émis, donc aucun « dirty » implicite (B6).
  function setPhTarget(value, silent) {
    const input = $("#ph_target");
    const btns = _phTargetButtons();
    if (!input || btns.length === 0) return;
    // Position la PLUS PROCHE : une consigne héritée hors grille (7,35 d'une
    // configuration antérieure) doit s'afficher quelque part — « aucune position
    // sélectionnée » est un état interdit (B7).
    const v = (typeof value === "number" && isFinite(value)) ? value : _phBounds.targetMin;
    let best = btns[0];
    let bestDelta = Infinity;
    btns.forEach((b) => {
      const d = Math.abs(parseFloat(b.dataset.value) - v);
      if (d < bestDelta) { bestDelta = d; best = b; }
    });
    btns.forEach((b) => {
      const on = (b === best);
      b.classList.toggle("is-active", on);
      b.setAttribute("aria-checked", on ? "true" : "false");
      b.tabIndex = on ? 0 : -1;
    });
    input.value = best.dataset.value;
    renderPhTargetHint();
    renderPhTargetWarning();
    renderPhOffsetWarning();  // la consigne entre dans le calcul de l'avertissement
    if (!silent) {
      // Les consommateurs historiques (trackDirtyState, updatePhControls) écoutent
      // `input`/`change` sur #ph_target : une écriture `.value =` ne les déclenche
      // PAS, il faut émettre explicitement.
      input.dispatchEvent(new Event("input", { bubbles: true }));
      input.dispatchEvent(new Event("change", { bubbles: true }));
    }
  }

  // Clic + clavier (délégation : survit à la regénération des positions).
  // Mêmes touches que les onglets Réglages : ←/→/↑/↓ déplacent ET sélectionnent,
  // Début/Fin vont aux bornes.
  function bindPhTargetControl() {
    const group = $("#ph_target_group");
    if (!group) return;
    // Contrôleur injoignable au chargement : le contrôle doit malgré tout avoir
    // des positions ET une position sélectionnée (état « aucune » interdit). Les
    // bornes de repli seront remplacées par celles du serveur dès /get-config.
    buildPhTargetSegments();
    if (!$("#ph_target_group .segmented__btn.is-active")) {
      setPhTarget(_phBounds.targetMin, true);
    }
    group.addEventListener("click", (e) => {
      const btn = e.target.closest(".segmented__btn");
      if (!btn || !group.contains(btn)) return;
      setPhTarget(parseFloat(btn.dataset.value), false);
    });
    group.addEventListener("keydown", (e) => {
      const btns = _phTargetButtons();
      const i = btns.indexOf(e.target);
      if (i < 0 || btns.length === 0) return;
      let target = null;
      if (e.key === "ArrowRight" || e.key === "ArrowDown") {
        target = btns[(i + 1) % btns.length];
      } else if (e.key === "ArrowLeft" || e.key === "ArrowUp") {
        target = btns[(i - 1 + btns.length) % btns.length];
      } else if (e.key === "Home") {
        target = btns[0];
      } else if (e.key === "End") {
        target = btns[btns.length - 1];
      }
      if (!target) return;
      e.preventDefault();
      setPhTarget(parseFloat(target.dataset.value), false);
      target.focus();
    });
  }

  // ---------- v2.25.0 : offset pH (incrémenteur) ----------
  function _phOffsetFromField() {
    const v = parseFloat($("#ph_offset")?.value);
    return isFinite(v) ? v : 0;
  }

  // Arrondi au pas + bornage. Sert à la MIGRATION (B6) : une configuration
  // antérieure a pu enregistrer 0,47 ; on affiche 0,50 sans marquer le formulaire
  // modifié — l'arrondi persistant est fait côté firmware.
  function _phSnapOffset(v) {
    if (typeof v !== "number" || !isFinite(v)) return 0;  // B7 : absent → 0,00
    const { offsetMin, offsetMax, offsetStep } = _phBounds;
    const snapped = _phRound(Math.round(v / offsetStep) * offsetStep);
    return clamp(snapped, offsetMin, offsetMax);
  }

  // Écrit l'offset dans le porteur caché et rafraîchit tout l'incrémenteur.
  // `silent` : voir setPhTarget().
  function setPhOffset(value, silent) {
    const input = $("#ph_offset");
    if (!input) return;
    const { offsetMin, offsetMax, offsetStep } = _phBounds;
    const raw = (typeof value === "number" && isFinite(value)) ? value : 0;
    const v = _phRound(clamp(raw, offsetMin, offsetMax)) || 0;  // `|| 0` neutralise -0
    input.value = v.toFixed(2);  // format machine : 2 décimales, point décimal

    const valueEl = $("#ph_offset_value");
    if (valueEl) {
      const txt = _fmtPh(v, true);
      valueEl.textContent = txt;
      valueEl.classList.toggle("is-neutral", v === 0);  // B7 : 0,00 = aucune correction
      valueEl.setAttribute("aria-valuemin", String(offsetMin));
      valueEl.setAttribute("aria-valuemax", String(offsetMax));
      valueEl.setAttribute("aria-valuenow", String(v));
      valueEl.setAttribute("aria-valuetext", txt + " pH");
    }

    const dec = $("#ph_offset_dec");
    const inc = $("#ph_offset_inc");
    const pas = _fmtPh(offsetStep, false);
    if (dec) {
      dec.disabled = (v <= offsetMin + PH_EPS);
      dec.setAttribute("aria-label", `Diminuer l'offset de ${pas}`);
    }
    if (inc) {
      inc.disabled = (v >= offsetMax - PH_EPS);
      inc.setAttribute("aria-label", `Augmenter l'offset de ${pas}`);
    }
    // `.btn:disabled` porte `pointer-events:none` : un bouton désactivé SOUS le
    // focus le laisserait retomber sur <body> et la navigation clavier serait
    // perdue. On rapatrie donc le focus sur la valeur (elle-même focusable).
    if (valueEl && document.activeElement &&
        ((dec && dec.disabled && document.activeElement === dec) ||
         (inc && inc.disabled && document.activeElement === inc))) {
      valueEl.focus();
    }

    const reset = $("#ph_offset_reset");
    if (reset) reset.hidden = (v === 0);

    renderPhOffsetWarning();
    if (!silent) {
      input.dispatchEvent(new Event("input", { bubbles: true }));
      input.dispatchEvent(new Event("change", { bubbles: true }));
    }
  }

  // Incrément d'un pas. L'arrondi au centième est fait AVANT le bornage pour
  // éviter la dérive flottante cumulée (−0,5 + 0,1 × 5 ≠ 0 sans lui).
  function stepPhOffset(direction) {
    setPhOffset(_phRound(_phOffsetFromField() + direction * _phBounds.offsetStep), false);
  }

  // Boutons + clavier de l'incrémenteur. PAS d'auto-répétition à l'appui long :
  // l'offset est un réglage de sécurité chimique, chaque pas doit être un acte.
  function bindPhOffsetStepper() {
    // État initial cohérent (0,00 neutre, boutons armés) avant la première
    // réponse de /get-config.
    setPhOffset(_phOffsetFromField(), true);
    $("#ph_offset_dec")?.addEventListener("click", () => stepPhOffset(-1));
    $("#ph_offset_inc")?.addEventListener("click", () => stepPhOffset(+1));
    $("#ph_offset_reset")?.addEventListener("click", () => {
      setPhOffset(0, false);
      $("#ph_offset_value")?.focus();  // le bouton disparaît : ne pas perdre le focus
    });
    $("#ph_offset_value")?.addEventListener("keydown", (e) => {
      const k = e.key;
      if (k === "ArrowUp" || k === "ArrowRight" || k === "PageUp") {
        e.preventDefault(); stepPhOffset(+1);
      } else if (k === "ArrowDown" || k === "ArrowLeft" || k === "PageDown") {
        e.preventDefault(); stepPhOffset(-1);
      } else if (k === "Home") {
        e.preventDefault(); setPhOffset(_phBounds.offsetMin, false);
      } else if (k === "End") {
        e.preventDefault(); setPhOffset(_phBounds.offsetMax, false);
      }
    });
  }

  // Un contrôle est « occupé » si l'utilisateur y a le focus OU si son bouton
  // Sauvegarder est actif (modification non enregistrée). Dans les deux cas, la
  // trame de config poussée par le WebSocket ne doit RIEN y écraser.
  //
  // CORRECTIF v2.25.0 : le test historique `document.activeElement !== input` est
  // devenu TOUJOURS VRAI depuis que le porteur de valeur est un input CACHÉ (il
  // n'est jamais focusable) — la trame écrasait donc la saisie en cours. On teste
  // le CONTENEUR visible, pas le porteur.
  function _phControlBusy(containerSelector, saveBtnSelector) {
    const container = $(containerSelector);
    const active = document.activeElement;
    if (container && active && container.contains(active)) return true;
    const btn = $(saveBtnSelector);
    return !!(btn && !btn.disabled);
  }

  function bindPhOffsetSave() {
    const input = $("#ph_offset");
    const btn = $("#ph_offset_save_btn");
    const errEl = $("#ph_offset_error");
    if (!input || !btn) return;

    const setBtn = (label, spinner, disabled, status) => {
      btn.disabled = disabled;
      btn.classList.remove("btn--primary", "btn--ok", "btn--danger");
      btn.classList.add(status === "success" ? "btn--ok" : status === "error" ? "btn--danger" : "btn--primary");
      btn.innerHTML = spinner ? `<span class="btn__spinner"></span><span>${label}</span>` : label;
    };
    const showErr = (msg) => {
      if (!errEl) return;
      if (msg) { errEl.textContent = msg; errEl.style.display = ""; }
      else { errEl.textContent = ""; errEl.style.display = "none"; }
    };

    trackDirtyState("#ph_offset_save_btn", ["ph_offset"]);
    input.addEventListener("input", () => { showErr(""); renderPhOffsetWarning(); });
    input.addEventListener("change", renderPhOffsetWarning);
    bindPhOffsetStepper();  // v2.25.0 : incrémenteur (± 0,10) + clavier + remise à zéro

    btn.addEventListener("click", async () => {
      if (btn.disabled) return;
      // v2.25.0 : plus aucune validation locale de plage. L'incrémenteur ne peut
      // produire qu'un multiple du pas dans les bornes servies par le serveur ;
      // une valeur illisible dans le porteur caché retombe sur 0,00 (neutre).
      const value = _phOffsetFromField();
      showErr("");
      setBtn("Sauvegarde...", true, true, "default");
      const res = await sendConfigDetailed({ ph_offset: _phRound(value) });
      if (res.ok) {
        setBtn("Sauvegarde réussie", false, true, "success");
        loadConfig();
        setTimeout(() => setBtn("Sauvegarder", false, true, "default"), 2000);
      } else {
        // B5 — Ce message ne signale plus qu'un ÉCHEC DE SAUVEGARDE (contrôleur
        // injoignable, refus serveur) : ne JAMAIS laisser croire que la valeur
        // affichée est enregistrée.
        showErr((res.message || res.error || "Enregistrement refusé.") +
                " L'offset affiché n'a pas été enregistré.");
        setBtn("Erreur", false, false, "error");
        setTimeout(() => setBtn("Sauvegarder", false, false, "default"), 2000);
      }
    });
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
    const phValue = parseFloat($("#ph_target")?.value);  // porteur caché du segmenté
    const orpValue = parseFloat($("#orp_target")?.value || "650");

    const phPumpValue = parseInt($("#ph_pump")?.value || "1", 10);
    const orpPumpValue = parseInt($("#orp_pump")?.value || "2", 10);

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
      // v2.25.0 : le porteur caché #ph_target ne contient qu'une valeur de la
      // GRILLE servie par le serveur (contrôle segmenté) — il n'y a plus de
      // saisie libre à filtrer, donc plus de risque de faire échouer (400) une
      // auto-save d'un réglage sans rapport (éclairage, filtration...) avec une
      // consigne en cours de frappe. Repli défensif si le contrôle n'est pas
      // encore construit : consigne enregistrée, puis borne basse de la plage.
      ph_target: isFinite(phValue)
        ? phValue
        : (typeof window._config?.ph_target === "number" && isFinite(window._config.ph_target)
            ? window._config.ph_target
            : _phBounds.targetMin),
      orp_target: isNaN(orpValue) ? 650 : orpValue,
      // v2.25.0 (B1) : ph_regulation_mode et ph_enabled VOLONTAIREMENT absents du
      // payload — même patron que feature-074 (pumpN_max_duty_pct). Le mode a son
      // PROPRE chemin de sauvegarde (clic sur le segmented, plus bas) ; le réémettre
      // ici avec un repli EN DUR sur "automatic" faisait rouvrir la régulation pH
      // depuis un onglet périmé (window._config absent ou obsolète). Incident du
      // 2026-08-25 : une bascule de sécurité annulée et sa trace effacée sur une
      // simple consultation de l'app. ph_enabled est en outre un dérivé pur du mode
      // et n'est plus accepté en écriture par /save-config.
      ph_daily_target_ml: parseInt($("#ph_daily_target_ml")?.value ?? "0", 10) || 0,
      ph_pump: isNaN(phPumpValue) ? 1 : phPumpValue,
      orp_regulation_mode: window._config?.orp_regulation_mode || "automatic",
      orp_enabled: (window._config?.orp_regulation_mode || "automatic") !== "manual",
      orp_daily_target_ml: parseInt($("#orp_daily_target_ml")?.value ?? "0", 10) || 0,
      orp_pump: isNaN(orpPumpValue) ? 2 : orpPumpValue,
      // feature-074 : pumpN_max_duty_pct VOLONTAIREMENT absent du payload —
      // l'amplitude est gelée (UI retirée) et son omission immunise contre les
      // sauvegardes d'onglets périmés qui écrasaient la valeur serveur.
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
    };
  }

  // Séparé de collectConfig() pour éviter qu'un save() ORP/pH
  // n'écrase accidentellement le mode de filtration en cours.
  function collectFiltrationConfig() {
    return {
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

    // v2.25.0 (B1) : bornes de réglage servies par le serveur, JAMAIS codées côté
    // client. À appliquer AVANT de (re)construire les positions de la consigne.
    _applyPhBounds(cfg);
    buildPhTargetSegments();
    renderPhTargetHint();

    // Consigne pH — écriture SILENCIEUSE (pas de dispatch) : lire la config ne
    // doit jamais marquer le formulaire modifié. B7 : consigne absente ou
    // invalide → borne basse ; « aucune position sélectionnée » est interdit.
    if (!_phControlBusy("#ph_target_group", "#ph_regulation_save_btn")) {
      setPhTarget(typeof cfg.ph_target === "number" ? cfg.ph_target : _phBounds.targetMin, true);
    }
    $("#orp_target").value = typeof cfg.orp_target === "number" ? cfg.orp_target : 650;

    // feature-067 : offset de mesure pH. Ne jamais écraser une modification en
    // cours (la config est aussi poussée par le WebSocket toutes les quelques
    // secondes) — cf. _phControlBusy() pour le correctif du test de focus.
    // B6 : un offset hérité hors pas (0,47) est affiché arrondi au plus proche,
    // SANS activer le bouton Sauvegarder (l'arrondi persistant est déjà fait
    // côté firmware). B7 : offset absent → 0,00 (état neutre).
    if (!_phControlBusy("#ph-offset-stepper", "#ph_offset_save_btn")) {
      setPhOffset(_phSnapOffset(cfg.ph_offset), true);
    }
    renderPhOffsetWarning();

    setSegmented("ph_regulation_mode", cfg.ph_regulation_mode || "automatic");
    updatePhModeControls();
    // v2.20.3 (C5) : trace persistée d'une bascule AUTOMATIQUE en mode manuel
    renderPhAutoManualBanner(cfg);
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

    // feature-074 : amplitude gelée — plus de curseur. La ligne lecture seule
    // (amplitude réelle + débit + provenance usine/mesuré) et le pré-remplissage
    // du champ de saisie avancée sont gérés par updatePumpCalibrationDisplay.
    updatePumpCalibrationDisplay(cfg);

    $("#ph_limit").value = typeof cfg.ph_limit_minutes === "number" ? cfg.ph_limit_minutes : 5;
    $("#orp_limit").value = typeof cfg.orp_limit_minutes === "number" ? cfg.orp_limit_minutes : 10;
    $("#ph_daily_limit").value = typeof cfg.max_ph_ml_per_day === "number" ? cfg.max_ph_ml_per_day : 300;
    $("#orp_daily_limit").value = typeof cfg.max_chlorine_ml_per_day === "number" ? cfg.max_chlorine_ml_per_day : 500;
    // feature-060 : garde anti-cycling configurable (clamp serveur 10-60, défaut 48)
    if ($("#max_cycles_per_day")) $("#max_cycles_per_day").value = typeof cfg.max_cycles_per_day === "number" ? cfg.max_cycles_per_day : 48;
    // feature-056 : mode d'installation (remplace regulation_mode + filtration_enabled)
    applyInstallMode(cfg.install_mode || "managed");
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

    // feature-035 : chip d'état de calibration température (remplace l'ancien callout/entête)
    updateTempCalChip();

    // Filtration
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

    // Éclairage — programmation (bug-ui-eclairage-refresh-ws : rafraîchissement
    // temps réel sur message WS config, miroir de la filtration ci-dessus).
    if (!lightingDirty) {
      const lightMode = $("#lighting_schedule_mode");
      if (lightMode && cfg.lighting_schedule_enabled !== undefined) {
        lightMode.value = cfg.lighting_schedule_enabled ? "enabled" : "disabled";
        const lightSettings = $("#lighting-schedule-settings");
        if (lightSettings) lightSettings.style.display = cfg.lighting_schedule_enabled ? "" : "none";
      }
      if (cfg.lighting_start_time) $("#lighting_start_time").value = cfg.lighting_start_time;
      if (cfg.lighting_end_time) $("#lighting_end_time").value = cfg.lighting_end_time;
    }

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

    // Development panel
    if ($("#sensor_logs_enabled")) {
      $("#sensor_logs_enabled").checked = cfg.sensor_logs_enabled === true;
    }
    if ($("#debug_logs_enabled")) {
      $("#debug_logs_enabled").checked = cfg.debug_logs_enabled === true;
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
  let lightingDirty = false;   // Idem pour la section Programmation éclairage (bug-ui-eclairage-refresh-ws)

  // ========== BADGES CAPTEURS ==========

  // Règle de composition du badge multi-états (feature-012 N2).
  // Chaque état candidat est décrit par { priority, variant, text } :
  //   priority 0 = capteur indisponible (danger) — feature-022 (valeur null/NaN)
  //   priority 1 = hors plage (danger)
  //   priority 2 = limite journalière atteinte
  //   priority 3 = cumul journalier ≥ 75 %
  //   priority 4 = stock faible
  // - MAXIMUM 2 segments affichés, séparés par « · » : au-delà de 2 candidats,
  //   on garde les 2 plus prioritaires (priority la plus faible d'abord).
  // - Variant du badge : hiérarchie danger > warning > info (feature-061 :
  //   'info' = état normal informatif, filtration arrêtée / stabilisation).
  function composeSensorBadge(candidates) {
    if (!candidates.length) return { variant: null, text: '' };
    const kept = [...candidates].sort((a, b) => a.priority - b.priority).slice(0, 2);
    const variant = kept.some(s => s.variant === 'danger') ? 'danger'
      : kept.some(s => s.variant === 'warning') ? 'warning' : 'info';
    return { variant, text: kept.map(s => s.text).join(' · ') };
  }

  // feature-061 : validité des mesures pH/ORP côté firmware (filtration arrêtée
  // ou purge de la chambre de mesure en cours). Champs absents (firmware plus
  // ancien) → mesures considérées valides (rétrocompat, rien ne change).
  function measuresAreValid(data) {
    if (!data) return true;
    if (data.water_present === false) return false;
    if (data.measures_valid === false) return false;
    return true;
  }

  // feature-061 : segments d'état mesure partagés dashboard / pages pH-ORP.
  // Recalculés à chaque push WS (le firmware décrémente measures_stab_remaining_s),
  // AUCUN tick JS seconde. `sensor` réservé pour d'éventuels textes différenciés.
  function measureStateSegments(sensor, data) {
    if (!data) return []; // boot : pas de faux positif avant le 1er message WS
    if (data.water_present === false) {
      // Signal externe périmé = anomalie (warning) ; arrêt franc = info
      return data.filtration_state_stale
        ? [{ priority: 1, variant: 'warning', text: 'Signal filtration périmé' }]
        : [{ priority: 1, variant: 'info', text: 'Filtration arrêtée' }];
    }
    if (data.water_present === true && data.measures_valid === false) {
      const s = data.measures_stab_remaining_s;
      const text = (s != null && s >= 60)
        ? `Stabilisation : ~${Math.ceil(s / 60)} min`
        : 'Stabilisation : < 1 min';
      return [{ priority: 1, variant: 'info', text }];
    }
    return [];
  }

  // Segment « Cumul XX % » (feature-012 N1) : affiché à partir de 75 % du
  // quota journalier (seuil aligné sur updateDailyDoseDisplay), masqué quand
  // la limite est atteinte (le segment « Limite journalière atteinte » prend
  // le relais) ou quand la limite n'est pas configurée.
  function dailyCumulSegment(dailyMl, maxMl, limitReached) {
    if (limitReached) return null;
    if (dailyMl == null || isNaN(dailyMl)) return null;
    if (!maxMl || maxMl <= 0) return null;
    // Même arithmétique que updateDailyDoseDisplay (arrondi des entrées, plafond 100 %)
    const pct = Math.min(100, Math.round((Math.round(dailyMl) / Math.round(maxMl)) * 100));
    if (pct < 75) return null;
    return { priority: 3, variant: 'warning', text: `Cumul ${pct} %` };
  }

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

    // feature-061 : mesures invalides (filtration arrêtée / stabilisation) —
    // gate les segments « Capteur indisponible » et « hors plage » (on ne peut
    // pas juger une sonde ni une valeur quand l'eau ne circule pas).
    const measuresOk = measuresAreValid(latestSensorData);

    // Badge pH — liste de candidats, composés par composeSensorBadge()
    const phSegments = [];
    phSegments.push(...measureStateSegments('ph', latestSensorData));
    // feature-022 : capteur indisponible (valeur null ou NaN). Le test
    // latestSensorData !== null évite le faux positif au boot : updateSensorBadges
    // est appelé depuis loadConfig avant le 1er message WS.
    if (latestSensorData !== null && measuresOk && (latestSensorData.ph == null || isNaN(latestSensorData.ph))) {
      phSegments.push({ priority: 0, variant: 'danger', text: 'Capteur indisponible' });
    }
    if (measuresOk && latestSensorData?.ph != null && !isNaN(latestSensorData.ph)) {
      const ph = latestSensorData.ph;
      const phTarget = window._config?.ph_target ?? 7.2;
      const phLow = +(phTarget - 0.2).toFixed(1);
      const phHigh = +(phTarget + 0.2).toFixed(1);
      if (ph < phLow || ph > phHigh) {
        phSegments.push({ priority: 1, variant: 'danger',
          text: ph < phLow ? `Trop bas (${ph.toFixed(1)})` : `Trop élevé (${ph.toFixed(1)})` });
      }
    }
    if (latestSensorData?.ph_limit_reached) {
      phSegments.push({ priority: 2, variant: 'warning', text: 'Limite journalière atteinte' });
    }
    { const cumul = dailyCumulSegment(
        latestSensorData?.ph_daily_ml,
        window._config?.max_ph_ml_per_day ?? 0,
        latestSensorData?.ph_limit_reached === true);
      if (cumul) phSegments.push(cumul);
    }
    { const rem = latestSensorData?.ph_remaining_ml;
      const thr = latestSensorData?.ph_alert_threshold_ml;
      if (rem != null && thr != null && rem <= thr && thr > 0) {
        phSegments.push({ priority: 4, variant: 'warning',
          text: `Stock faible (${(rem / 1000).toFixed(1)} L)` });
      }
    }
    const phBadge = composeSensorBadge(phSegments);
    setBadge('ph-sensor-badge', phBadge.variant, phBadge.text);

    // Badge ORP — mêmes règles de composition
    const orpSegments = [];
    orpSegments.push(...measureStateSegments('orp', latestSensorData));
    // feature-022 : capteur indisponible (cf. commentaire du bloc pH ci-dessus)
    if (latestSensorData !== null && measuresOk && (latestSensorData.orp == null || isNaN(latestSensorData.orp))) {
      orpSegments.push({ priority: 0, variant: 'danger', text: 'Capteur indisponible' });
    }
    if (measuresOk && latestSensorData?.orp != null && !isNaN(latestSensorData.orp)) {
      const orp = Math.round(latestSensorData.orp);
      const orpTarget = window._config?.orp_target ?? 650;
      const orpLow = Math.round(orpTarget - 150);
      const orpHigh = Math.round(orpTarget + 150);
      if (orp < orpLow || orp > orpHigh) {
        orpSegments.push({ priority: 1, variant: 'danger',
          text: orp < orpLow ? `Trop bas (${orp} mV)` : `Trop élevé (${orp} mV)` });
      }
    }
    if (latestSensorData?.orp_limit_reached) {
      orpSegments.push({ priority: 2, variant: 'warning', text: 'Limite journalière atteinte' });
    }
    { const cumul = dailyCumulSegment(
        latestSensorData?.orp_daily_ml,
        window._config?.max_chlorine_ml_per_day ?? 0,
        latestSensorData?.orp_limit_reached === true);
      if (cumul) orpSegments.push(cumul);
    }
    { const rem = latestSensorData?.orp_remaining_ml;
      const thr = latestSensorData?.orp_alert_threshold_ml;
      if (rem != null && thr != null && rem <= thr && thr > 0) {
        orpSegments.push({ priority: 4, variant: 'warning',
          text: `Stock faible (${(rem / 1000).toFixed(1)} L)` });
      }
    }
    const orpBadge = composeSensorBadge(orpSegments);
    setBadge('orp-sensor-badge', orpBadge.variant, orpBadge.text);

    // Badge Température — capteur indisponible (bug-ui-valeurs-figees) + segments
    // d'état mesure (feature-061 it. 2), calqué sur les blocs pH/ORP ci-dessus
    // (mêmes gardes anti-faux-positif : boot et mesures invalides).
    const tempSegments = [];
    tempSegments.push(...measureStateSegments('temperature', latestSensorData));
    if (latestSensorData !== null && measuresOk && (latestSensorData.temperature == null || isNaN(latestSensorData.temperature))) {
      tempSegments.push({ priority: 0, variant: 'danger', text: 'Capteur indisponible' });
    }
    const tempBadge = composeSensorBadge(tempSegments);
    setBadge('temperature-sensor-badge', tempBadge.variant, tempBadge.text);

    // feature-061 : badges des pages pH/ORP — alimentés par les SEULS segments
    // d'état mesure (pas les segments dosage/stock, déjà portés ailleurs sur la page).
    const phPageBadge = composeSensorBadge(measureStateSegments('ph', latestSensorData));
    setBadge('ph-page-badge', phPageBadge.variant, phPageBadge.text);
    const orpPageBadge = composeSensorBadge(measureStateSegments('orp', latestSensorData));
    setBadge('orp-page-badge', orpPageBadge.variant, orpPageBadge.text);
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

  // feature-056 : applique le mode d'installation à l'UI (masquage conditionnel de la
  // filtration). managed → filtration pilotée (widgets + programmation + nav visibles).
  // powered/external → filtration non pilotée (widgets/programmation/nav masqués + note).
  // external → bloc « État du signal filtration » visible.
  function applyInstallMode(mode) {
    const m = (mode === "powered" || mode === "external") ? mode : "managed";
    const managed = m === "managed";

    // Sélection du radio correspondant (sans déclencher d'événement)
    const radio = document.querySelector(`input[name="install_mode"][value="${m}"]`);
    if (radio) radio.checked = true;

    const dashCard = $("#dashboard-filtration-card");
    if (dashCard) dashCard.style.display = managed ? "" : "none";

    const filtContent = $("#filtration-content");
    if (filtContent) filtContent.style.display = managed ? "block" : "none";

    const navFilt = $("#nav-filtration");
    if (navFilt) navFilt.style.display = managed ? "" : "none";

    const note = $("#filtration-unmanaged-note");
    if (note) note.style.display = managed ? "none" : "";

    const extStatus = $("#install-ext-status");
    if (extStatus) extStatus.style.display = (m === "external") ? "" : "none";

    if (m === "external") fillExternalTips();
    updateExternalStatePill();
  }

  // feature-056 : renseigne le bloc d'aide external avec le host et le topic MQTT réels.
  function fillExternalTips() {
    const urlEl = $("#tips-http-url");
    if (urlEl) urlEl.textContent = `${location.protocol}//${location.host}/filtration/external-state`;
    const base = (window._config && window._config.topic)
      || ($("#mqtt_topic") && $("#mqtt_topic").value)
      || "pool/sensors";
    const fullTopic = `${base}/filtration_external_state/set`;
    document.querySelectorAll(".tips-topic").forEach((el) => { el.textContent = fullTopic; });
  }

  // feature-056 : pill reflétant l'état du signal filtration externe (mode external).
  // Source : WS sensor_data (water_present / filtration_state_stale / filtration_ext_*).
  function updateExternalStatePill() {
    const pill = $("#install-ext-pill");
    if (!pill) return;
    const data = latestSensorData || {};
    const known = data.filtration_ext_known === true;
    const stale = data.filtration_state_stale === true;
    const water = data.water_present === true;
    const ageS = Number.isFinite(data.filtration_ext_age_s) ? data.filtration_ext_age_s : 0;

    let text, cls;
    if (!known) {
      text = "Aucun signal"; cls = "mid";
    } else if (stale) {
      const mins = Math.max(1, Math.round(ageS / 60));
      text = `Périmé · ${mins} min`; cls = "bad";
    } else if (water) {
      text = `Actif · ${ageS} s`; cls = "ok";
    } else {
      text = "Arrêtée"; cls = "mid";
    }
    pill.textContent = text;
    pill.className = "pill " + cls;
  }

  // feature-056 : bindings du panneau Installation (radios + bouton Sauvegarder).
  function bindInstallMode() {
    document.querySelectorAll('input[name="install_mode"]').forEach((r) => {
      r.addEventListener("change", () => {
        if (r.checked) applyInstallMode(r.value);  // masquage immédiat, persistance au save
      });
    });
    $("#install_save_btn")?.addEventListener("click", async () => {
      const sel = document.querySelector('input[name="install_mode"]:checked');
      const mode = sel ? sel.value : "managed";
      const btn = $("#install_save_btn");
      if (btn) btn.disabled = true;
      const ok = await sendConfig({ install_mode: mode });
      if (btn) btn.disabled = false;
      if (ok) {
        if (window._config) window._config.install_mode = mode;
        applyInstallMode(mode);
        updateStatusCards();
        showToast("Mode d'installation enregistré", "success");
        loadConfig();
      } else {
        showToast("Erreur lors de la sauvegarde", "error");
      }
    });
  }

  function predictFiltrationRunning(cfg) {
    // feature-056 : le relais n'est piloté qu'en mode « managed ». Dans les autres
    // modes, la filtration n'est pas commandée par le PoolController → jamais ON.
    if (!cfg) return false;
    const im = cfg.install_mode || window._config?.install_mode;
    if (im && im !== "managed") return false;
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

  // bug-dosage-fantome it.3 : le widget distingue « session active » (commandé)
  // de « pompe alimentée » (pouls effectif). Décomptes recalculés depuis
  // dosingAnchor (valeur ancrée + horloge locale), JAMAIS extrapolés au-delà de
  // la fin de période : à l'échéance sans nouvelle donnée → ellipsis, jamais de
  // bascule optimiste vers « Injection en cours », jamais 0 s ni négatif.
  // UN SEUL setInterval 1 s partagé pH+ORP (pattern injectInterval), actif
  // uniquement quand un état à décompte est affiché.
  let dosingTickTimer = null;
  const _dosingStatusArgs = { ph: null, orp: null };   // derniers args (re-rendu par le ticker)
  const _dosingCountdownShown = { ph: false, orp: false };
  const DOSING_NBSP = " ";  // espace insécable avant l'unité (« 42 s »)

  const fmtDosingCountdown = (s) => {
    if (s >= 60) {
      const m = Math.floor(s / 60), sec = s % 60;
      return `${m}${DOSING_NBSP}min ${String(sec).padStart(2, "0")}${DOSING_NBSP}s`;
    }
    return `${s}${DOSING_NBSP}s`;
  };

  function _dosingTick() {
    ["ph", "orp"].forEach((s) => {
      const a = _dosingStatusArgs[s];
      if (a) updateDosingStatus(s, a.isActive, a.stopReason);
    });
  }

  function updateDosingStatus(sensor, isActive, stopReason) {
    const el = $(`#${sensor}-dosing-status`);
    if (!el) return;
    _dosingStatusArgs[sensor] = { isActive, stopReason };

    const data = latestSensorData || {};
    const anchor = dosingAnchor;
    const elapsedMs = anchor ? Math.max(0, Date.now() - anchor.atMs) : 0;
    const DOT = '<span class="dosing-status__dot" aria-hidden="true"></span>';
    let countdownShown = false;

    // Applique texte + classes en retirant les classes d'état inutilisées.
    const show = (html, stateCls) => {
      el.innerHTML = html;
      el.classList.add("is-visible");
      ["is-active", "is-waiting", "is-fault"].forEach((c) => el.classList.toggle(c, c === stateCls));
    };

    // Priorité 1 : injection MANUELLE minutée (valeur ancrée à la réception).
    const manualBase = anchor ? anchor[sensor].manualS : (data[`${sensor}_inject_remaining_s`] ?? 0);
    const manualS = manualBase - Math.floor(elapsedMs / 1000);
    const session = data[`${sensor}_pulse_session`];
    const periodMs = data.pulse_period_ms;
    if (manualBase > 0 && manualS > 0) {
      show(DOT + "Injection manuelle : " + fmtDosingCountdown(manualS), "is-active");
      countdownShown = true;
    } else if (session === true && Number.isFinite(periodMs) && periodMs > 0) {
      // Priorités 2-4 : session d'impulsions (dosage en proportionnel de temps).
      // Phase locale = phase ancrée + temps écoulé, clampée à la fin de période.
      const pulseMs = data[`${sensor}_pulse_ms`] ?? 0;
      const phaseMs = Math.min((anchor ? anchor[sensor].phaseMs : 0) + elapsedMs, periodMs);
      const endS = Math.ceil((periodMs - phaseMs) / 1000);  // jamais 0 tant que phase < période
      if (pulseMs > 0 && phaseMs < pulseMs) {
        // Priorité 2 : pouls en cours — la pompe est réellement alimentée.
        // Ticker maintenu ACTIF (revue it.3) : quand la phase ancrée atteint
        // pulseMs, la bascule RESTRICTIVE vers « Prochaine impulsion » se fait
        // localement, sans attendre la trame suivante. Le sens inverse
        // (attente → injection) reste interdit sans donnée serveur.
        show(DOT + "Injection en cours", "is-active");
        countdownShown = true;
      } else if (pulseMs > 0) {
        // Priorité 3 : repos entre deux pouls — décompte jusqu'à la fin de période.
        show(DOT + (endS > 0 ? "Prochaine impulsion : " + fmtDosingCountdown(endS)
                             : "Prochaine impulsion…"), "is-waiting");
        countdownShown = endS > 0;
      } else {
        // Priorité 4 : deadband — période sans pouls planifié (demande < pouls min).
        show(DOT + (endS > 0 ? "Dosage en attente : " + fmtDosingCountdown(endS)
                             : "Dosage en attente…"), "is-waiting");
        countdownShown = endS > 0;
      }
    } else if (session === undefined && isActive) {
      // Rétrocompat firmware sans champs pulse_* : comportement historique.
      show(DOT + "Injection en cours", "is-active");
    } else if (stopReason) {
      // Priorité 5 : motif d'arrêt (getDosingStopReason + dedupDosingReason).
      el.textContent = stopReason;
      el.classList.add("is-visible");
      el.classList.remove("is-active", "is-waiting");
      // v2.25.1 : ton d'alerte SI ET SEULEMENT SI le texte affiché est bien le
      // libellé d'une PANNE de mesure (sonde figée / muette). Les états
      // transitoires attendus (préchauffage, porte de validité fermée, quotas)
      // gardent le ton neutre — c'est toute la distinction que ce correctif pose.
      const b = _doseBlockInfo(data, sensor);
      el.classList.toggle("is-fault", b.tone === "fault" && stopReason === b.label);
    } else {
      // Priorité 6 : rien à afficher.
      el.textContent = "";
      el.classList.remove("is-visible", "is-active", "is-waiting", "is-fault");
    }

    _dosingCountdownShown[sensor] = countdownShown;
    if (_dosingCountdownShown.ph || _dosingCountdownShown.orp) {
      if (!dosingTickTimer) dosingTickTimer = setInterval(_dosingTick, 1000);
    } else if (dosingTickTimer) {
      clearInterval(dosingTickTimer);
      dosingTickTimer = null;
    }
  }

  function getDosingStopReason(data, config, sensor) {
    const enabled = sensor === "ph" ? config.ph_enabled : config.orp_enabled;
    if (!enabled) return "Régulation désactivée";

    // feature-012 N3 : stabilisation PAR POMPE (champs feature-006) — une
    // calibration ORP ne doit pas afficher « Stabilisation » sur la carte pH,
    // et inversement. Fallback sur le champ global (max des 2) pour compat
    // avec un firmware plus ancien qui ne pousserait pas les champs par pompe.
    const remainS = data[`${sensor}_stab_remaining_s`] ?? (data.stabilization_remaining_s || 0);
    if (remainS > 0) {
      const m = Math.floor(remainS / 60);
      const s = remainS % 60;
      return m > 0 ? `Stabilisation : ${m} min ${String(s).padStart(2, '0')} s` : `Stabilisation : ${s} s`;
    }

    // Pause mélange hydraulique post-injection (feature-025) : rendue visible sur le
    // widget (v2.19.1). Compte à rebours via *_mix_remaining_s ; repli sur le booléen
    // *MixingDelayActive (firmware plus ancien, sans temps restant).
    const mixS = data[`${sensor}_mix_remaining_s`] || 0;
    if (mixS > 0) {
      const m = Math.floor(mixS / 60);
      const s = mixS % 60;
      return m > 0 ? `Pause mélange : ${m} min ${String(s).padStart(2, '0')} s` : `Pause mélange : ${s} s`;
    }
    if (data[`${sensor}MixingDelayActive`] === true) return "Pause mélange en cours";

    // feature-056 : présence d'eau = source UNIQUE (résolue par le mode d'installation).
    // Fallback sur filtration_running si le firmware ne pousse pas encore water_present.
    const waterPresent = (data.water_present !== undefined) ? data.water_present : data.filtration_running;
    if (waterPresent === false) {
      return data.filtration_state_stale ? "Signal filtration périmé" : "Filtration arrêtée";
    }

    if (sensor === "ph"  && data.ph_limit_reached)  return "Limite journalière atteinte";
    if (sensor === "orp" && data.orp_limit_reached)  return "Limite journalière atteinte";

    const usedMs    = sensor === "ph" ? (data.ph_used_ms  || 0) : (data.orp_used_ms  || 0);
    const limitMin  = sensor === "ph" ? (config.ph_limit_minutes || 0) : (config.orp_limit_minutes || 0);
    if (limitMin > 0 && usedMs >= limitMin * 60000) return "Limite de durée atteinte";

    // v2.25.1 : PORTE DE VALIDITÉ FERMÉE — traitée AVANT la cause de refus
    // firmware. Le badge capteur affiche déjà « Stabilisation : ~N min »
    // (measureStateSegments, measures_valid === false) ; descendre jusqu'au motif
    // de refus juxtaposait deux messages contradictoires sur la même carte.
    // On ne se taît QUE pour la famille « stabilisation des mesures » : une panne
    // (sonde figée / muette) ou tout autre motif reste dit, badge ou pas.
    const block = _doseBlockInfo(data, sensor);
    const blockEntry = DOSE_BLOCK_CODES[block.code];
    if (data.measures_valid === false && blockEntry && blockEntry.stab) return "";

    // v2.8.5 : remonter la cause de refus firmware non couverte ci-dessus
    // (anti-cycling cycles/jour, anti-rafale…). Vide en cas 2 « valeur OK »
    // (canDose ne refuse pas → code 0/null) → statut reste vide.
    if (block.label) return block.label;

    return "";  // Pas de refus firmware : valeur dans l'hystérésis (rien à doser)
  }

  // feature-061 : dé-duplication — quand le badge capteur affiche déjà
  // « Filtration arrêtée » / « Signal filtration périmé » (water_present === false,
  // segment garanti visible car priorité 1), masquer le motif équivalent du
  // dosing-status : le badge fait foi. Les autres motifs restent inchangés.
  // Fallback firmware ancien (water_present absent) : pas de badge → on garde le motif.
  function dedupDosingReason(reason, data) {
    if (data && data.water_present === false
        && (reason === "Filtration arrêtée" || reason === "Signal filtration périmé")) {
      return "";
    }
    return reason;
  }

  // feature-053 : carte Boost du tableau de bord. Consomme boost_active/boost_until
  // du WS sensor_data (fallback window._config, alimenté par le broadcast config).
  // Ne dépend PAS de latestSensorData → appelée avant l'early-return d'updateStatusCards.
  function updateBoostCard() {
    const badge = $("#detail-boost-status");
    if (!badge) return;
    const untilEl = $("#detail-boost-until");
    const startBtn = $("#detail-boost-start");
    const stopBtn = $("#detail-boost-stop");
    const data = latestSensorData || {};
    const cfg = window._config || {};
    const active = (data.boost_active != null) ? !!data.boost_active : !!cfg.boost_active;
    const until = (data.boost_until != null) ? data.boost_until : cfg.boost_until;
    const effectRow = $("#detail-boost-effect-row");
    const effectEl = $("#detail-boost-effect");
    const hintEl = $("#detail-boost-hint");
    // feature-055 : leviers réellement actifs (défaut true si champ absent, rétrocompat).
    // Champs toujours émis par le WS, valides même Boost inactif → hint prospectif honnête.
    const filtExt = data.boost_filtration_extended !== false;
    const chlorBoost = data.boost_chlorine_boosted !== false;
    let effectText, noEffect = false;
    if (filtExt && chlorBoost)      effectText = "Filtration prolongée + surchloration";
    else if (!filtExt && chlorBoost) effectText = "Surchloration seule";
    else if (filtExt && !chlorBoost) effectText = "Filtration prolongée seule";
    else { effectText = "Sans effet (filtration non gérée, ORP non automatique)"; noEffect = true; }
    if (active) {
      if (untilEl) {
        if (Number.isFinite(until) && until > 0) {
          const d = new Date(until * 1000);
          const p = (n) => String(n).padStart(2, "0");
          untilEl.textContent = `${p(d.getDate())}/${p(d.getMonth() + 1)} à ${p(d.getHours())}:${p(d.getMinutes())}`;
        } else {
          untilEl.textContent = "prochain minuit";
        }
      }
      badge.textContent = "Actif";
      badge.className = noEffect ? "state-badge state-badge--warn" : "state-badge state-badge--ok";
      if (effectEl) effectEl.textContent = effectText;
      if (effectRow) effectRow.style.display = "";
      // La ligne « Effet » fait foi quand actif : masquer le hint statique pour éviter la contradiction.
      if (hintEl) hintEl.style.display = "none";
      if (startBtn) startBtn.style.display = "none";
      if (stopBtn) stopBtn.style.display = "";
    } else {
      badge.textContent = "Inactif";
      badge.className = "state-badge state-badge--off";
      if (untilEl) untilEl.textContent = "—";
      if (effectRow) effectRow.style.display = "none";
      // Hint prospectif : décrit ce que fera réellement l'activation selon la config courante.
      if (hintEl) {
        let hintText;
        if (filtExt && chlorBoost)       hintText = "À l'activation : filtration prolongée + cible ORP relevée jusqu'à minuit.";
        else if (!filtExt && chlorBoost) hintText = "À l'activation : cible ORP relevée jusqu'à minuit (filtration non gérée par le contrôleur).";
        else if (filtExt && !chlorBoost) hintText = "À l'activation : filtration prolongée jusqu'à minuit (régulation ORP non automatique, chlore non relevé).";
        else                             hintText = "À l'activation : aucun effet (filtration non gérée par le contrôleur et régulation ORP non automatique).";
        hintEl.textContent = hintText;
        hintEl.style.display = "";
      }
      if (startBtn) startBtn.style.display = "";
      if (stopBtn) stopBtn.style.display = "none";
    }
  }

  function updateStatusCards() {
    updateBoostCard();  // feature-053 : indépendant de latestSensorData
    updateExternalStatePill();  // feature-056 : pill signal filtration externe
    if (!latestSensorData) return;

    const data = latestSensorData;
    const config = window._config || {};

    // === FILTRATION ===
    // Les cartes de filtration et éclairage compactes ont été supprimées et fusionnées avec les cartes détaillées

    // feature-061 : mesures non représentatives (filtration arrêtée / stabilisation)
    // → « -- » sur les valeurs pH/ORP ; la ligne « Cible » reste affichée.
    const measuresOk = measuresAreValid(data);

    // === pH COMPACT ===
    // bug-ui-valeurs-figees : valeur null/NaN → « -- » explicite (jamais de skip,
    // sinon l'ancienne valeur reste figée quand le capteur devient indisponible).
    const compactPh = $("#compact-ph");
    if (compactPh) {
      compactPh.textContent = (measuresOk && data.ph != null && !isNaN(data.ph))
        ? (Math.round(data.ph * 10) / 10).toFixed(1)
        : "--";
    }

    const compactPhTarget = $("#compact-ph-target");
    if (compactPhTarget && config.ph_target != null) {
      compactPhTarget.textContent = config.ph_target.toFixed(1);
    }

    updateDosingStatus("ph",  !!data.ph_dosing,  dedupDosingReason(getDosingStopReason(data, config, "ph"), data));

    // === ORP COMPACT ===
    const compactOrp = $("#compact-orp");
    if (compactOrp) {
      const orpText = (measuresOk && data.orp != null && !isNaN(data.orp)) ? String(Math.round(data.orp)) : "--";
      compactOrp.innerHTML = orpText + '<span class="compact-unit">mV</span>';
    }

    const compactOrpTarget = $("#compact-orp-target");
    if (compactOrpTarget && config.orp_target != null) {
      compactOrpTarget.textContent = Math.round(config.orp_target);
    }

    updateDosingStatus("orp", !!data.orp_dosing, dedupDosingReason(getDosingStopReason(data, config, "orp"), data));

    // === ÉCLAIRAGE ===
    if (data.lighting_enabled != null) updateLightingStatus(data.lighting_enabled);

    // === TEMPÉRATURE COMPACT ===
    // feature-061 it. 2 : mesure non représentative (filtration arrêtée) → « -- »
    const compactTemperature = $("#compact-temperature");
    if (compactTemperature) {
      const tempText = (measuresOk && data.temperature != null && !isNaN(data.temperature))
        ? (Math.round(data.temperature * 10) / 10).toFixed(1)
        : "--";
      compactTemperature.innerHTML = tempText + '<span class="compact-unit">°C</span>';
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
        showToast(`Erreur export historique : ${err.message || err}`, "error");
      } finally {
        exportBtn.disabled = false;
      }
    });

    importInput?.addEventListener("change", () => {
      if (importBtn) importBtn.disabled = !(importInput.files && importInput.files.length);
    });

    importBtn?.addEventListener("click", async () => {
      if (!(importInput.files && importInput.files.length)) return;
      if (!(await confirmDialog({
        title: "Importer l'historique",
        message: "Importer ce fichier va remplacer l'historique actuel.",
        confirmLabel: "Importer",
        danger: true
      }))) return;

      importBtn.disabled = true;
      importInput.disabled = true;

      try {
        const text = await importInput.files[0].text();
        const points = parseHistoryCsv(text);
        if (!points.length) {
          showToast("Aucune donnée valide trouvée dans le fichier.", "error");
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
        showToast(`Import terminé (${response?.count || points.length} points).`, "success");
        importInput.value = "";
        if (importBtn) importBtn.disabled = true;
      } catch (err) {
        showToast(`Erreur import historique : ${err.message || err}`, "error");
      } finally {
        importBtn.disabled = !(importInput.files && importInput.files.length);
        importInput.disabled = false;
      }
    });

    clearBtn?.addEventListener("click", async () => {
      if (!(await confirmDialog({
        title: "Supprimer l'historique",
        message: "Supprimer l'historique enregistré ?\n\nCette action est irréversible.",
        confirmLabel: "Supprimer",
        danger: true
      }))) return;
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
        showToast("Historique supprimé.", "success");
      } catch (err) {
        showToast(`Erreur suppression historique : ${err.message || err}`, "error");
      } finally {
        clearBtn.disabled = false;
      }
    });
  }

  function clearHistoryCharts() {
    [tempChart, phChart, orpChart].forEach(chart => {
      if (chart) chart.setPoints([], []);
    });
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

  // feature-011 : ligne « Débit calculé » des blocs Programmée (pH/ORP).
  // Valeur poussée par WS (ph/orp_scheduled_flow_ml_per_min) : float en mL/min,
  // ou null hors mode scheduled / hors plage de filtration / heure invalide.
  // La visibilité est déjà gérée par les blocs #xx-params-scheduled (mode Programmée
  // uniquement) — ici on ne fait que rafraîchir le texte à chaque push sensor_data.
  // NB : pas de « sur Y h restantes » côté client — l'heure du navigateur peut
  // différer de celle de l'ESP32, on n'affiche que le débit (source firmware).
  function updateScheduledFlowDisplay() {
    const data = latestSensorData || {};
    [["ph", "ph_scheduled_flow_ml_per_min"], ["orp", "orp_scheduled_flow_ml_per_min"]].forEach(([prefix, key]) => {
      const valueEl = $(`#${prefix}_scheduled_flow_value`);
      if (!valueEl) return;
      const lineEl = $(`#${prefix}_scheduled_flow_line`);
      const v = data[key];
      if (typeof v === "number" && !isNaN(v)) {
        valueEl.textContent = v.toLocaleString("fr-FR", { minimumFractionDigits: 1, maximumFractionDigits: 1 }) + " mL/min";
        if (lineEl) lineEl.removeAttribute("title");
      } else {
        valueEl.textContent = "—";
        if (lineEl) lineEl.title = "Hors plage de filtration ou données indisponibles";
      }
    });
  }

  // ---------- v2.26.0 (C5) : rattrapage accumulé par la régulation pH ----------
  //
  // Le firmware sert TROIS champs sur /data et le WebSocket :
  //   ph_integral_pct  → remplissage de la réserve, en % (0-100, valeur absolue) ;
  //   ph_integral      → l'intégrale brute, en pH·s (signée) ;
  //   ph_integral_term → sa contribution au débit demandé, en mL/min (signée).
  //
  // Le POURCENTAGE est affiché en premier : c'est la seule des trois grandeurs
  // lisible sans connaître le plafond de l'anti-windup — lequel VARIE avec la
  // vitesse de correction (lente / normale / rapide). Ce plafond n'est donc
  // JAMAIS recopié ici : le firmware sert le rapport déjà calculé. Toute
  // tentative de recalculer un pourcentage côté client rouvrirait le motif du
  // miroir de constante firmware, éliminé quatre fois dans ce projet.
  //
  // SÉMANTIQUE STRICTE des états (clé absente ≠ null ≠ 0), même contrat que
  // `phDoseBlockedCode` :
  //   nombre fini → valeur mesurée, 0 compris (0 % = réserve VIDE, affirmé) ;
  //   null        → le firmware dit explicitement « non calculable » (terme
  //                 intégral inactif) : on affiche « — », pas un zéro ;
  //   undefined   → INFORMATION ABSENTE (firmware antérieur à la 2.26.0) : le
  //                 bloc entier reste masqué. Afficher « 0 % » dans ce cas
  //                 mentirait sur une régulation dont on ne sait rien.
  //
  // Aucun seuil de saturation côté client (revue v2.26.0) : le verdict est servi
  // par le firmware via `ph_integral_saturated`. Une constante 95 recopiée ici
  // aurait été un 5ᵉ miroir de valeur firmware dans ce fichier — et surtout elle
  // aurait porté sur la MAUVAISE grandeur (remplissage au lieu de
  // demande/soutenable), sans jamais connaître la durée (2 h / 6 h).

  function updatePhIntegralDisplay() {
    const section = $("#ph-integral-section");
    if (!section) return;
    const pctEl = $("#ph_integral_pct_value");
    const detailEl = $("#ph_integral_detail");
    const warnEl = $("#ph_integral_warning");

    const data = latestSensorData || {};
    const pct = data.ph_integral_pct;

    // Firmware antérieur : rien à dire, donc rien à montrer.
    if (pct === undefined) { section.style.display = "none"; return; }

    // Le rattrapage n'évolue qu'en régulation automatique : l'exposer dans les
    // autres modes afficherait un reliquat figé, sans lecture possible.
    const mode = getSegmented("ph_regulation_mode") || "automatic";
    if (mode !== "automatic") { section.style.display = "none"; return; }
    section.style.display = "";

    const num = (v) => (typeof v === "number" && isFinite(v)) ? v : null;
    const p = num(pct);
    const raw = num(data.ph_integral);
    const term = num(data.ph_integral_term);

    if (pctEl) {
      pctEl.textContent = (p === null)
        ? "—"
        : p.toLocaleString("fr-FR", { minimumFractionDigits: 0, maximumFractionDigits: 0 }) + " %";
    }
    if (detailEl) {
      // Second rang, volontairement discret : la valeur brute et le débit
      // qu'elle réclame. |term| suffit — le signe est porté par le sens de
      // correction (pH- / pH+), déjà affiché plus haut dans la carte.
      const parts = [];
      if (raw !== null) {
        parts.push(Math.abs(raw).toLocaleString("fr-FR", { maximumFractionDigits: 0 }) + " pH·s");
      }
      if (term !== null) {
        parts.push(Math.abs(term).toLocaleString("fr-FR", { minimumFractionDigits: 1, maximumFractionDigits: 1 }) + " mL/min demandés");
      }
      detailEl.textContent = parts.length ? " (" + parts.join(" · ") + ")" : "";
    }
    if (warnEl) {
      // Le verdict est SERVI par le firmware (`ph_integral_saturated`), il n'est
      // PAS recalculé ici (revue v2.26.0). Motif : le seuil firmware porte sur
      // `demande / débit soutenable`, alors que `ph_integral_pct` mesure le
      // REMPLISSAGE de l'anti-windup — deux grandeurs distinctes. Une intégrale
      // au plafond qui tient la charge sans forcer est une régulation SAINE :
      // comparer le remplissage à 95 % afficherait « vérifiez le bidon » sur une
      // installation qui va bien, et c'est le faux positif qui décrédibilise
      // l'alerte. Le firmware seul connaît en outre la DURÉE (2 h / 6 h).
      // Clé absente (firmware antérieur à 2.26.0) ⇒ pas d'encart, jamais un
      // `false` inventé.
      const sat = latestSensorData?.ph_integral_saturated;
      const saturated = (sat === true);
      warnEl.hidden = !saturated;
      if (saturated) {
        warnEl.textContent =
          "Rattrapage au maximum : la régulation demande tout ce qu'elle peut sans que "
          + "le pH rejoigne sa consigne. Vérifiez le niveau du bidon, l'amorçage de la "
          + "pompe doseuse et le TAC de l'eau (une alcalinité élevée bloque le pH).";
      }
    }

    // ----- feature-085 (v2.28.0) : dose de base apprise (feedforward pH) -----
    // Deux champs servis à l'IDENTIQUE sur /data et le WebSocket :
    //   ph_ff_ml_per_min → débit de base appliqué à la session auto (mL/min) ;
    //   ph_ff_status     → code d'état FIGÉ côté firmware :
    //     0 inactif · 1 actif · 2 latch sécurité (pH sous consigne) ·
    //     3 historique < 3 j · 4 quota journalier ≥ 60 %.
    // MÊME contrat de présence que le reste du bloc : `ph_ff_status` absent
    // (undefined) = firmware antérieur à la 2.28.0 → la ligne reste masquée
    // (afficher « inactif » mentirait sur un firmware qui n'a pas de FF).
    // La valeur n'est affichée QUE pour le statut « actif » : dans tous les
    // autres états le débit vaut 0 par construction, et un « 0,00 mL/min »
    // se lirait comme une mesure. Jamais de NaN affiché (helper num()).
    const ffLine = $("#ph_ff_line");
    if (ffLine) {
      const st = data.ph_ff_status;
      if (typeof st !== "number" || !isFinite(st)) {
        ffLine.hidden = true;
      } else {
        ffLine.hidden = false;
        const valEl = $("#ph_ff_value");
        const stateEl = $("#ph_ff_state");
        const flow = num(data.ph_ff_ml_per_min);
        let valTxt = "—";
        let stateTxt = "";
        if (st === 1) {
          stateTxt = "active";
          if (flow !== null) {
            valTxt = flow.toLocaleString("fr-FR", { minimumFractionDigits: 2, maximumFractionDigits: 2 }) + " mL/min";
          }
        } else if (st === 2) {
          stateTxt = "suspendue (pH sous consigne)";
        } else if (st === 3) {
          stateTxt = "en apprentissage (historique < 3 j)";
        } else if (st === 4) {
          stateTxt = "suspendue (60 % du quota atteint)";
        }
        // st === 0 (inactif) ou code inconnu d'un firmware plus récent : tiret seul.
        if (valEl) valEl.textContent = valTxt;
        if (stateEl) stateEl.textContent = stateTxt ? " " + stateTxt : "";
      }
    }
  }

  // ========== SECTIONS DÉTAILLÉES ==========
  function updateDetailSections() {
    const config = window._config || {};

    // v2.26.0 (C5) : rattrapage accumulé — rendu EN TÊTE et sous try/catch, même
    // précaution que le statut MQTT dans _onWsSensorData. Motif : c'est le SEUL
    // point de décision de la visibilité du bloc, et il était appelé en DERNIER,
    // après quatre autres rafraîchisseurs. Une exception dans l'un d'eux (donnée
    // inattendue d'un firmware donné) laissait le bloc masqué EN SILENCE — sans
    // rien casser de visible par ailleurs, puisque les rubriques qui le
    // précèdent avaient déjà été mises à jour et que la ligne « débit calculé »
    // est de toute façon invisible hors mode Programmée. Un bloc neuf qui « ne
    // s'affiche jamais » est exactement la trace que laisserait ce défaut.
    try { updatePhIntegralDisplay(); } catch (e) { console.error('[ph-integral] rendu:', e); }

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

    // feature-011 : débit moyen calculé en mode Programmée (push WS)
    updateScheduledFlowDisplay();

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
      // Programmation désactivée → la ligne Horaires est masquée (afficher des
      // horaires inertes contredisait le mode affiché juste au-dessus).
      const schedRow = detailLightingSchedule.closest('.detail-row');
      if (config.lighting_schedule_enabled) {
        const startTime = config.lighting_start_time || '20:00';
        const endTime = config.lighting_end_time || '23:00';
        detailLightingSchedule.textContent = startTime + ' - ' + endTime;
        if (schedRow) schedRow.style.display = '';
      } else if (schedRow) {
        schedRow.style.display = 'none';
      }
    }
  }

  function bindDetailActions() {
    // Boutons filtration
    const startBtn = $("#detail-filtration-start");
    const stopBtn = $("#detail-filtration-stop");

    if (startBtn) {
      startBtn.addEventListener('click', async () => {
        const payload = { filtration_force_on: true, filtration_force_off: false };
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

    // Boutons Boost (feature-053)
    const boostStartBtn = $("#detail-boost-start");
    const boostStopBtn = $("#detail-boost-stop");

    if (boostStartBtn) {
      boostStartBtn.addEventListener('click', async () => {
        try {
          const response = await authFetch("/boost/start", { method: "POST" });
          if (response.ok) {
            const r = await response.json().catch(() => ({}));
            if (latestSensorData) {
              latestSensorData.boost_active = true;
              if (r.boost_until != null) latestSensorData.boost_until = r.boost_until;
              // feature-055 : affichage immédiat des leviers (avant le prochain WS ~5 s)
              if (r.filtration_extended != null) latestSensorData.boost_filtration_extended = r.filtration_extended;
              if (r.chlorine_boosted != null) latestSensorData.boost_chlorine_boosted = r.chlorine_boosted;
            }
            updateBoostCard();
            // feature-054 : toast adaptatif selon les leviers réellement actifs
            // (renvoyés par le firmware, source de vérité).
            const filtExt = r.filtration_extended !== false;   // défaut true si absent (rétrocompat)
            const chlorBoost = r.chlorine_boosted !== false;
            if (filtExt && chlorBoost) {
              showToast("Boost activé — vérifiez le taux de chlore avant la baignade.", "info");
            } else if (!filtExt && chlorBoost) {
              showToast("Boost activé (surchloration seule) : la filtration n'est pas gérée par PoolController. Vérifiez le taux de chlore avant la baignade.", "info");
            } else if (filtExt && !chlorBoost) {
              showToast("Boost activé (filtration prolongée seule) : la régulation ORP n'est pas en mode Automatique, le chlore n'est pas relevé.", "warning");
            } else {
              showToast("Boost sans effet : filtration non gérée et régulation ORP non automatique.", "warning");
            }
          } else if (response.status === 409) {
            const r = await response.json().catch(() => ({}));
            showToast(r.message || "Boost impossible (horloge non synchronisée).", "error");
          } else {
            showToast("Erreur lors de l'activation du Boost", "error");
          }
        } catch (e) {
          console.error('Erreur activation boost:', e);
          showToast("Erreur de connexion", "error");
        }
      });
    }

    if (boostStopBtn) {
      boostStopBtn.addEventListener('click', async () => {
        try {
          const response = await authFetch("/boost/stop", { method: "POST" });
          if (response.ok) {
            if (latestSensorData) { latestSensorData.boost_active = false; latestSensorData.boost_until = 0; }
            updateBoostCard();
            showToast("Boost désactivé", "success");
          } else {
            showToast("Erreur lors de l'arrêt du Boost", "error");
          }
        } catch (e) {
          console.error('Erreur arrêt boost:', e);
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

    // bug-ui-valeurs-figees : valeur null/NaN → « -- » explicite (jamais de skip)
    // feature-061 it. 2 : température gatée par la validité des mesures
    if (mTemp) {
      mTemp.textContent = (measuresAreValid(json) && json.temperature != null && !isNaN(json.temperature))
        ? json.temperature.toFixed(1) : "--";
    }
    if (mPh) {
      mPh.textContent = (json.ph != null && !isNaN(json.ph))
        ? (Math.round(json.ph * 10) / 10).toFixed(1) : "--";
    }
    if (mOrp) {
      mOrp.textContent = (json.orp != null && !isNaN(json.orp))
        ? String(Math.round(json.orp)) : "--";
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
  //   calPoints null/<0 → lecture en cours ; 0 → calibration requise ;
  //   1 → calibration 1/2 ; >=2 → diagnostic de pente (santé sonde).
  // bug-slope-boot-perdue (C1/C2) : trois états distincts au lieu du
  // fourre-tout « EZO indisponible » / « Pente non disponible » :
  //   1. calPoints null/<0  → transitoire de boot (cache -1 les premières
  //      secondes). Aucun champ de /data ou du WS ne distingue ce transitoire
  //      d'un vrai mutisme EZO (les deux donnent calPoints -1 et ph null) →
  //      choix architect : UN SEUL libellé, adouci en « Sonde pH : lecture en
  //      cours… », classe neutre. Un vrai mutisme finit de toute façon en
  //      « EZO pH » au journal + valeurs pH figées à « -- ».
  //   2. ageMs null (pente JAMAIS lue depuis le boot) et calPoints >= 2 →
  //      « Pente en cours de lecture… », classe neutre (PAS warning) : le
  //      firmware retente désormais automatiquement, résolu en < 2 min.
  //   3. phSlope* null MAIS ageMs non-null → pente invalidée après échecs
  //      répétés : vrai état d'erreur → « Pente non disponible », classe warn.
  function classifyPhProbe({ acid, base, zero, calPoints, ageMs }) {
    if (calPoints == null || calPoints < 0) {
      return { cls: 'unknown', label: 'Sonde pH : lecture en cours…' };
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
      // Âge connu = la pente a déjà été lue au moins une fois PUIS invalidée
      // (échecs répétés) → erreur réelle. Sinon : simple attente de la
      // première lecture de boot (le firmware la retente tout seul).
      const ageKnown = (ageMs != null && Number(ageMs) < PH_SLOPE_NEVER);
      if (!ageKnown) {
        return { cls: 'unknown', label: 'Pente en cours de lecture…' };
      }
      return { cls: 'warn', label: 'Pente non disponible' };
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
  // v2.25.1 : au-delà de cette fenêtre sans valeur brute, l'absence de lecture
  // cesse d'être un blip et devient « EZO indisponible » (2 rafraîchissements
  // /data de 30 s au minimum avant de condamner la sonde).
  const FILTER_RAW_BLIP_MS = 60000;
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
    // v2.20.3 (D5) : l'alarme firmware « filtre non prêt durablement » prime sur
    // TOUT le reste, y compris sur « EZO indisponible ». Elle signifie qu'il n'y a
    // plus AUCUNE régulation depuis > 5 min alors que la filtration tourne — c'est
    // le seul état que l'utilisateur doit voir immédiatement.
    if (json?.[prefix + 'FilterAlarm'] === true) {
      return { cls: 'bad', label: 'Régulation à l’arrêt' };
    }
    // v2.25.1 : le CODE de refus discrimine PANNE (sonde figée / muette) et état
    // transitoire (préchauffage, porte de validité fermée). Il passe avant le
    // test « brut invalide », qui condamnait l'EZO sans savoir pourquoi.
    const entry = DOSE_BLOCK_CODES[_doseBlockInfo(json, prefix).code];
    const short = entry && entry.chip;   // undefined si code absent / non qualifiant
    if (short && entry.tone === 'fault') return { cls: 'bad', label: short };
    // Remonté avant le test « brut invalide », qui le masquait (une sonde
    // instable finit souvent par rendre une lecture nulle).
    if (json?.[prefix + 'FilterUnstable'] === true) return { cls: 'bad', label: 'Capteur instable' };
    if (!_filterRawIsValid(raw)) {
      // Une lecture manquante alors qu'on en avait une il y a moins d'une minute
      // est un blip de bus, pas une panne établie : on nuance au lieu de conclure.
      const recent = st && st.lastRawAt && (Date.now() - st.lastRawAt) < FILTER_RAW_BLIP_MS;
      return recent ? { cls: 'warn', label: 'Lecture EZO manquante' }
                    : { cls: 'unknown', label: 'EZO indisponible' };
    }
    // Cause connue et NORMALE (préchauffage, mesures non représentatives) :
    // ton d'attente, jamais le registre visuel d'une panne.
    if (short) return { cls: 'warn', label: short };
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

  // ==========================================================================
  // v2.25.1 — Cause de refus de dosage : on traduit un CODE, plus jamais une phrase
  // ==========================================================================
  // L'ancienne version devinait la catégorie par includes() sur le texte français.
  // Deux défauts observés en production, symétriques et tous deux graves :
  //   - « mesure filtrée indisponible (NaN) » contenait « indispo » → affichage
  //     « EZO indisponible » pendant ~6 min au boot, sonde parfaitement saine ;
  //   - « warmup / EZO injoignable » contenait « warm » ET « ezo », « warm » testé
  //     d'abord → une VRAIE panne d'EZO s'affichait « Stabilisation du filtre »,
  //     c'est-à-dire une panne matérielle maquillée en état normal.
  // Le firmware publie désormais `ph/orpDoseBlockedCode` (entier) À CÔTÉ du texte.
  // Ces valeurs sont un CONTRAT DE SÉRIALISATION FIGÉ (jamais renumérotées, toute
  // nouvelle cause s'ajoute à la fin) — cf. enum DoseRefusal, src/dosing_logic.h.
  //
  // tone :
  //   'normal'  → état transitoire ATTENDU (préchauffage, porte de validité
  //               fermée, quota, mélange) : ton neutre/informatif ;
  //   'fault'   → PANNE (sonde figée, sonde muette, watchdog…) : ton d'alerte ;
  //   'unknown' → cause non attribuable (le firmware lui-même dit qu'il ne sait
  //               pas, ou code plus récent que cette page) : aveu d'ignorance.
  // chip : libellé court pour le chip d'état filtre (absent = pas d'incidence).
  // stab : cause appartenant à la famille « stabilisation des mesures », déjà
  //        annoncée par le badge capteur (cf. measureStateSegments) — sert à ne
  //        pas afficher deux messages concurrents côte à côte.
  const DOSE_BLOCK_CODE_GUARD = 250;
  const DOSE_BLOCK_CODES = {
    1:  { tone: 'fault',   label: 'Watchdog inactif — dosage suspendu' },
    2:  { tone: 'normal',  label: 'Filtration arrêtée' },
    3:  { tone: 'unknown', label: 'Mesure indisponible — cause indéterminée' },
    4:  { tone: 'unknown', label: 'Filtre de mesure non prêt — cause indéterminée' },
    5:  { tone: 'fault',   label: 'Capteur instable (mesures rejetées)', chip: 'Capteur instable' },
    6:  { tone: 'fault',   label: 'Calibration insuffisante' },
    7:  { tone: 'normal',  label: 'Stabilisation post-calibration' },
    8:  { tone: 'normal',  label: 'Mélange en cours' },
    9:  { tone: 'normal',  label: 'Régulation non automatique' },
    10: { tone: 'normal',  label: 'Limite journalière atteinte' },
    11: { tone: 'normal',  label: 'Limite horaire atteinte' },
    12: { tone: 'normal',  label: 'Limite cycles/jour atteinte — reprise à minuit' },
    13: { tone: 'normal',  label: 'Anti-rafale — patienter avant la prochaine injection' },
    14: { tone: 'normal',  label: 'Anti-rafale (15 min) — patienter avant la prochaine injection' },
    // Qualification de la garde 3 (lecture filtrée NaN)
    15: { tone: 'normal',  label: 'Stabilisation du filtre de mesure', chip: 'Stabilisation…', stab: true },
    16: { tone: 'fault',   label: 'Sonde figée — vérifier la sonde', chip: 'Sonde figée' },
    17: { tone: 'fault',   label: 'Plus aucune mesure acceptée — vérifier la sonde', chip: 'Sonde muette' },
    18: { tone: 'normal',  label: 'Mesures non représentatives (stabilisation)', chip: 'Mesures non représentatives', stab: true },
    // Qualification de la garde 3b (filtre non prêt)
    19: { tone: 'normal',  label: 'Stabilisation du filtre de mesure', chip: 'Stabilisation…', stab: true },
    20: { tone: 'fault',   label: 'Sonde figée — vérifier la sonde', chip: 'Sonde figée' },
    21: { tone: 'fault',   label: 'Plus aucune mesure acceptée — vérifier la sonde', chip: 'Sonde muette' },
    22: { tone: 'normal',  label: 'Mesures non représentatives (stabilisation)', chip: 'Mesures non représentatives', stab: true },
  };

  // Traduit le couple (code, texte) servi pour `prefix` en { code, label, tone }.
  // SÉMANTIQUE STRICTE des états du champ code (clé absente ≠ null ≠ 0) :
  //   0 ou null → aucun refus en cours, mesuré et affirmé par le firmware ;
  //   undefined → INFORMATION ABSENTE (firmware antérieur à la parité v2.25.1) :
  //               dans ce cas SEULEMENT, repli sur le texte serveur, affiché tel
  //               quel — aucune catégorie n'est déduite de son contenu ;
  //   code non répertorié → « cause inconnue (code N) ». Jamais le libellé
  //               voisin : un firmware plus récent que cette page doit produire
  //               un doute visible, pas une approximation plausible.
  function _doseBlockInfo(json, prefix) {
    if (!json) return { code: undefined, label: '', tone: 'none' };
    const code = json[prefix + 'DoseBlockedCode'];
    const raw = json[prefix + 'DoseBlockedReason'];
    const text = (typeof raw === 'string') ? raw.trim() : '';
    if (code === undefined) {
      return { code: undefined, label: text, tone: text ? 'unknown' : 'none' };
    }
    if (code === null || code === 0) return { code: 0, label: '', tone: 'none' };
    if (code === DOSE_BLOCK_CODE_GUARD) {
      // Garde de sécurité pH/ORP : message porteur de valeurs runtime (seuils,
      // mesure), rédigé en clair côté firmware et non énumérable → on cite.
      return { code, label: text || 'Sécurité pH/ORP active', tone: 'fault' };
    }
    const e = DOSE_BLOCK_CODES[code];
    if (e) return { code, label: e.label, tone: e.tone };
    return { code, label: `Cause inconnue (code ${code})`, tone: 'unknown' };
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
    // feature-067 : diagnostic pH — brut EZO, offset appliqué, corrigé (brut + offset).
    // Médiane et valeur filtrée sont déjà corrigées côté firmware.
    if (prefix === 'ph') {
      const off = _phOffsetValue();
      const offEl = $('#ph-fm-offset');
      const corEl = $('#ph-fm-corrected');
      if (offEl) offEl.textContent = (off >= 0 ? '+' : '') + off.toFixed(2);
      if (corEl) {
        const raw = json?.phRaw;
        corEl.textContent = _filterRawIsValid(raw) ? Number(raw + off).toFixed(2) : '--';
      }
    }
    if (medEl) medEl.textContent = _fmtFilterVal(prefix, json?.[prefix + 'Median']);
    if (filEl) filEl.textContent = _fmtFilterVal(prefix, json?.[prefix + 'Filtered']);
    if (rejEl) { const r = json?.[prefix + 'RejectedCount']; rejEl.textContent = (typeof r === 'number') ? String(r) : '--'; }
    if (ageEl) ageEl.textContent = _filterAgeText(prefix);
    if (rdyEl) {
      const ready = json?.[prefix + 'FilterReady'];
      rdyEl.textContent = ready === true ? 'Oui' : (ready === false ? 'Non' : '--');
    }

    const block = _doseBlockInfo(json, prefix);
    let reason = block.label;
    let tone = block.tone;
    if (!reason && json?.[prefix + 'MixingDelayActive'] === true) {
      reason = 'Mélange en cours';
      tone = 'normal';
    }
    if (blockedRow && blockedEl) {
      // Le ton d'alerte est RÉSERVÉ aux pannes : un préchauffage ou une porte de
      // validité fermée sont des états attendus et restent en ton neutre.
      blockedEl.classList.toggle('probe-modal__value--bad', tone === 'fault');
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

  // ---- v2.20.3 (D5) : bannière tableau de bord « régulation à l'arrêt » ----
  // Alimentée par les drapeaux firmware ph/orpFilterAlarm (/data + WebSocket).
  // Le firmware ne les arme QUE si la filtration tourne, que les mesures sont
  // représentatives et que le filtre reste non prêt > 5 min : aucun affichage
  // pendant une purge, un warmup ou une filtration à l'arrêt.
  function updateRegulationAlert(json) {
    const box = $("#regul-alert");
    if (!box) return;
    const phAlarm = json?.phFilterAlarm === true;
    const orpAlarm = json?.orpFilterAlarm === true;
    if (!phAlarm && !orpAlarm) { box.hidden = true; return; }

    const titleEl = $("#regul-alert-title");
    const textEl = $("#regul-alert-text");
    const cta = $("#regul-alert-cta");
    let quoi, cible;
    if (phAlarm && orpAlarm) { quoi = "pH et chlore"; cible = "#/ph"; }
    else if (phAlarm)        { quoi = "pH"; cible = "#/ph"; }
    else                     { quoi = "chlore"; cible = "#/orp"; }
    if (titleEl) titleEl.textContent = `Régulation ${quoi} à l’arrêt`;
    if (textEl) {
      textEl.textContent =
        `Le filtre de mesure est bloqué depuis plus de 5 minutes alors que la ` +
        `filtration tourne : aucun dosage ${quoi} n’est effectué. ` +
        `Ouvrez l’état du filtre pour diagnostiquer et le réarmer si besoin.`;
    }
    if (cta) cta.setAttribute('href', cible);
    box.hidden = false;
  }

  // ---- v2.20.3 (C5) : encart « bascule automatique pH → manuel » (page pH) ----
  // Trace PERSISTÉE côté firmware (ph_auto_manual_epoch / _reason dans /get-config).
  // Elle survit au redémarrage et au ring buffer du journal.
  // v2.25.0 (B3) : elle ne disparaît PLUS « en choisissant un mode » — tant qu'elle
  // est là, /save-config refuse (409) toute sortie du mode manuel. Le seul chemin
  // de sortie est le bouton d'acquittement ci-dessous (ph_auto_manual_ack), qui
  // efface la trace SANS réarmer la régulation : l'utilisateur choisit ensuite.
  function renderPhAutoManualBanner(cfg) {
    const box = $("#ph-auto-manual-banner");
    if (!box) return;
    const reason = (cfg && typeof cfg.ph_auto_manual_reason === 'string')
      ? cfg.ph_auto_manual_reason.trim() : '';
    const epoch = (cfg && typeof cfg.ph_auto_manual_epoch === 'number')
      ? cfg.ph_auto_manual_epoch : 0;
    if (!reason && !epoch) { box.hidden = true; return; }

    let quand = "date inconnue (horloge non synchronisée au moment de la bascule)";
    if (epoch > 0) {
      try {
        quand = new Date(epoch * 1000).toLocaleString('fr-FR');
      } catch (e) { quand = String(epoch); }
    }
    const textEl = $("#ph-auto-manual-text");
    if (textEl) {
      textEl.textContent =
        `Le ${quand}, l’appareil a basculé la régulation pH en mode manuel par ` +
        `sécurité. Cause : ${reason || 'non renseignée'}. ` +
        `Tant que cette alerte n’est pas acquittée, la régulation ne peut pas être ` +
        `réactivée : traitez d’abord la cause, puis utilisez le bouton ci-dessous. ` +
        `L’acquittement n’enclenche aucun dosage — il vous rend seulement le choix ` +
        `du mode.`;
    }
    box.hidden = false;
  }

  // v2.25.0 (B3) : acquittement DÉLIBÉRÉ de la bascule de sécurité pH.
  // N'envoie QUE ph_auto_manual_ack — jamais de mode : effacer la trace ne doit
  // pas réarmer la régulation (l'utilisateur reste en manuel et décide ensuite).
  function bindPhAutoManualAck() {
    const btn = $("#ph-auto-manual-ack");
    if (!btn) return;
    btn.addEventListener("click", async () => {
      if (btn.disabled) return;
      const label = btn.textContent;
      btn.disabled = true;
      btn.textContent = "Acquittement…";
      try {
        const r = await sendConfigDetailed({ ph_auto_manual_ack: true });
        if (r.ok) {
          showToast("Alerte acquittée — vous pouvez choisir un mode de régulation.", "success");
          await loadConfig();  // masque la bannière et resynchronise le segmented
        } else {
          showToast(r.error || "Acquittement refusé.", "error");
        }
      } finally {
        // Hors ligne comme succès : le bouton ne doit jamais rester figé.
        btn.disabled = false;
        btn.textContent = label;
      }
    });
  }

  // ---- v2.20.3 (D3) : réarmement manuel d'un filtre depuis son modal ----
  // Confirmation EXPLICITE exigée par pool-chemistry (P6) : l'utilisateur doit
  // être informé que la détection de sonde figée sera effacée.
  async function requestFilterReset(prefix) {
    const nom = prefix === 'ph' ? 'pH' : 'ORP';
    const ok = await confirmDialog({
      title: `Réarmer le filtre ${nom} ?`,
      message:
        `Cette action vide la fenêtre de mesure du filtre ${nom} et le fait ` +
        `repartir de zéro : le dosage automatique ${nom} restera bloqué environ ` +
        `une minute, le temps du réamorçage.\n\n` +
        `⚠ La détection de sonde figée et l’alarme de bruit électrique en cours ` +
        `seront EFFACÉES. Si la sonde est réellement défectueuse, le défaut sera ` +
        `de nouveau masqué et devra être re-détecté.\n\n` +
        `Un seul réarmement est autorisé par capteur toutes les 10 minutes.`,
      confirmLabel: 'Réarmer',
      cancelLabel: 'Annuler',
      danger: true,
    });
    if (!ok) return;

    const btn = $(`#${prefix}-filter-reset`);
    if (btn) { btn.disabled = true; btn.textContent = 'Réarmement…'; }
    try {
      const res = await authFetch(`/reset-sensor-filter?sensor=${prefix}`, { method: 'POST' });
      if (res.status === 429) {
        let wait = 0;
        try { wait = (await res.json())?.retry_after_s || 0; } catch (e) { /* corps vide */ }
        showToast(
          wait > 0
            ? `Réarmement refusé : réessayez dans ${Math.ceil(wait / 60)} min.`
            : `Réarmement refusé : un seul par capteur toutes les 10 minutes.`,
          'error');
        return;
      }
      if (res.status === 401 || res.status === 403) {
        showToast("Réarmement refusé : droits administrateur requis.", 'error');
        return;
      }
      if (!res.ok) { showToast(`Échec du réarmement du filtre ${nom}.`, 'error'); return; }
      showToast(`Filtre ${nom} en cours de réarmement (~1 min avant reprise du dosage).`, 'success');
      const modal = $(`#${prefix}-filter-modal`);
      if (modal && modal.open) modal.close();
    } catch (e) {
      // Hors ligne / ESP32 injoignable : l'UI ne doit jamais rester figée.
      console.error(`[${prefix}-filter] reset:`, e);
      showToast("Contrôleur injoignable — réarmement non envoyé.", 'error');
    } finally {
      if (btn) { btn.disabled = false; btn.textContent = `Réarmer le filtre ${nom}`; }
    }
  }

  function bindFilterChip(prefix) {
    const chip = $(`#${prefix}-filter-chip`);
    const modal = $(`#${prefix}-filter-modal`);
    const closeBtn = $(`#${prefix}-filter-close`);
    const resetBtn = $(`#${prefix}-filter-reset`);
    if (resetBtn) resetBtn.addEventListener('click', () => requestFilterReset(prefix));
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
        // bug-dosage-fantome it.3 : ancrage du calendrier d'impulsion sur
        // l'horloge LOCALE (les phases firmware sont relatives au début de
        // période, jamais un millis absolu). Voir la déclaration de dosingAnchor.
        dosingAnchor = {
          atMs: Date.now(),
          ph:  { manualS: json.ph_inject_remaining_s  ?? 0, phaseMs: json.ph_pulse_phase_ms  ?? 0 },
          orp: { manualS: json.orp_inject_remaining_s ?? 0, phaseMs: json.orp_pulse_phase_ms ?? 0 },
        };
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
        updatePumpCharacterizationBadges(json);  // badge « Débit non caractérisé »
        updateProductUI(json);
        _updateSondesChip();
        if (window._updateInjectButtons) window._updateInjectButtons(json);
        // feature-075 : expiration de session de l'assistant (champ flow_assist
        // présent uniquement dans /data — les trames WS sont ignorées par le hook)
        if (window._flowWizardOnData) window._flowWizardOnData(json);

        // Mettre à jour les sections détaillées
        updateDetailSections();

        // also update readouts in settings — Atlas EZO : la sonde retourne directement la valeur
        // calibrée (pas de "tension brute" à exposer). Pendant la calibration on affiche la
        // lecture pH/ORP live (3 décimales pour pH).
        const phCurrentValue = $("#ph_current_value");
        const phCurrentLabel = $("#ph_current_label");
        if (phCurrentValue) {
          if (phCurrentLabel) phCurrentLabel.textContent = "Valeur pH actuelle";
          // feature-061 : mesure non représentative (filtration arrêtée / stabilisation)
          // → « -- » (le badge #ph-page-badge explique pourquoi). Les lectures live de
          // calibration/diagnostic (filter-sub, updateCalibrationReadouts) ne sont PAS gatées.
          if (measuresAreValid(json) && json.ph != null && typeof json.ph === "number" && !isNaN(json.ph)) {
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
          // feature-061 : cf. bloc pH ci-dessus
          if (measuresAreValid(json) && json.orp != null && typeof json.orp === "number" && !isNaN(json.orp)) {
            orpCurrentValue.textContent = Math.round(json.orp) + " mV";
          } else {
            orpCurrentValue.textContent = "--";
          }
        }
        // Chip d'état filtre ORP (feature-025)
        updateFilterChip('orp', json);
        // v2.20.3 (D5) : bannière tableau de bord « régulation à l'arrêt »
        updateRegulationAlert(json);

        const tempCurrentValue = $("#temp_current_value");
        const tempCurrentLabel = $("#temp_current_label");
        const tempCalLiveValue = $("#temp_cal_live_value");
        {
          let tempText = "--";
          if (json.temperature != null && typeof json.temperature === "number" && !isNaN(json.temperature)) {
            tempText = json.temperature.toFixed(1) + " °C";
          }
          if (tempCurrentValue) {
            if (tempCurrentLabel) tempCurrentLabel.textContent = "Température actuelle";
            tempCurrentValue.textContent = tempText;
          }
          // feature-035 : lecture live dans la carte de calibration
          if (tempCalLiveValue) tempCalLiveValue.textContent = tempText;
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

  // feature-035 : chip d'état de calibration température (offset 1 point).
  // Lit window._config.temp_calibration_date + temp_calibration_offset, calcule l'âge côté JS.
  function updateTempCalChip() {
    const chip = $("#temp-cal-chip");
    const lbl = $("#temp-cal-chip-label");
    const header = $("#temp_cal_status_header");
    const cfg = window._config || {};
    const dateStr = cfg.temp_calibration_date;
    let cls, label, headerText;

    if (dateStr && dateStr !== "") {
      const d = new Date(dateStr);
      const ageMs = Date.now() - d.getTime();
      const ageDays = Math.floor(ageMs / 86400000);
      const ageMonths = Math.floor(ageDays / 30);
      if (ageMonths > 6) {
        cls = "chip--probe-warn";
        label = `Calibré · ancien (il y a ${ageMonths} mois)`;
      } else {
        cls = "chip--probe-good";
        if (ageDays > 30) {
          label = `Calibré · le ${d.toLocaleDateString("fr-FR", { day: "2-digit", month: "2-digit" })}`;
        } else if (ageDays >= 1) {
          label = `Calibré · il y a ${ageDays} j`;
        } else {
          label = "Calibré · aujourd'hui";
        }
      }
      const offset = cfg.temp_calibration_offset;
      headerText = "Calibré · " + d.toLocaleString("fr-FR");
      if (offset != null && !isNaN(offset)) headerText += ` (offset ${offset > 0 ? "+" : ""}${offset.toFixed(1)} °C)`;
    } else {
      cls = "chip--probe-unknown";
      label = "Non calibré";
      headerText = "Non calibré";
    }

    if (chip) chip.className = "chip chip--probe chip--cal " + cls;
    if (lbl) lbl.textContent = label;
    if (header) header.textContent = headerText;
  }

  // feature-035 : show/hide de la carte de calibration température (calqué sur showOrp/hideOrpCalibrationCard)
  function showTempCalibrationCard() {
    const enable = $("#temperature-card-enable");
    const hist = $("#temperature-card-history");
    const cal = $("#temperature-card-calibration");
    const calBtn = $("#temp_cal_trigger_btn");
    if (enable) enable.style.display = "none";
    if (hist) hist.style.display = "none";
    if (cal) cal.style.display = "";
    if (calBtn) calBtn.disabled = true;
    tempCalibrationStep = 0;
    const refInput = $("#temp_reference_value");
    if (refInput) refInput.value = "";
    updateTempCalibrationSteps();
  }
  function hideTempCalibrationCard() {
    const enable = $("#temperature-card-enable");
    const hist = $("#temperature-card-history");
    const cal = $("#temperature-card-calibration");
    const calBtn = $("#temp_cal_trigger_btn");
    if (enable) enable.style.display = "";
    if (hist) hist.style.display = "";
    if (cal) cal.style.display = "none";
    if (calBtn) calBtn.disabled = false;
    tempCalibrationStep = 0;
    const refInput = $("#temp_reference_value");
    if (refInput) refInput.value = "";
    updateTempCalibrationSteps();
    calBtn?.focus();
  }

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

    // feature-035 : déclencheur / fermeture de la carte de calibration
    $("#temp_cal_trigger_btn")?.addEventListener("click", () => {
      showTempCalibrationCard();
      $("#temp_step1")?.scrollIntoView?.({ behavior: "smooth", block: "nearest" });
    });
    $("#temp_cal_close_btn")?.addEventListener("click", () => {
      hideTempCalibrationCard();
    });

    cancelBtn?.addEventListener("click", async () => {
      if (!(await confirmDialog({
        title: "Annuler la calibration",
        message: "Annuler la calibration en cours ?"
      }))) return;
      tempCalibrationStep = 0;
      if (refInput) refInput.value = "";
      updateTempCalibrationSteps();
      loadConfig();
    });

    startBtn?.addEventListener("click", async () => {
      if (tempCalibrationStep === 0) {
        tempCalibrationStep = 1;
        updateTempCalibrationSteps();
      } else if (tempCalibrationStep === 1) {
        tempCalibrationStep = 2;
        updateTempCalibrationSteps();
        if (refInput) refInput.focus();
      } else if (tempCalibrationStep === 2) {
        const referenceValue = parseFloat(refInput.value);
        if (isNaN(referenceValue) || referenceValue < -10 || referenceValue > 50) {
          showToast("Température de référence invalide (-10 à 50 °C)", "error");
          return;
        }
        tempCalibrationStep = 3;
        updateTempCalibrationSteps();
      } else if (tempCalibrationStep === 3) {
        const referenceValue = parseFloat(refInput.value);
        if (isNaN(referenceValue) || referenceValue < -10 || referenceValue > 50) {
          showToast("Température de référence invalide (-10 à 50 °C)", "error");
          tempCalibrationStep = 2;
          updateTempCalibrationSteps();
          return;
        }

        if (!latestSensorData || isNaN(latestSensorData.temperature) || latestSensorData.temperature == null) {
          showToast("Aucune donnée capteur disponible (/data).", "error");
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
          const tempCalLiveValue = $("#temp_cal_live_value");
          if (tempCalLiveValue) tempCalLiveValue.textContent = correctedTemp.toFixed(1) + " °C";

          // Invalider le timestamp pour forcer le rechargement
          lastSensorDataLoadTime = 0;

          // Rafraîchir la config pour mettre à jour la date de calibration (met aussi à jour le chip)
          await loadConfig();
          updateTempCalibrationSteps();
          // feature-035 : chip « Calibré » + retour à l'état nominal
          updateTempCalChip();
          hideTempCalibrationCard();

          // Recharger les données après un court délai pour se resynchroniser avec le backend
          setTimeout(async () => {
            await loadSensorData();
          }, 2000);
        } catch (err) {
          showToast("Erreur calibration température : " + err.message, "error");
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
    // feature-067 : la calibration EZO travaille EXCLUSIVEMENT sur le brut — plus
    // aucun repli sur `ph` (qui porte désormais la valeur corrigée par l'offset).
    const phRaw = (typeof latestSensorData?.phRaw === "number" && !isNaN(latestSensorData.phRaw)) ? latestSensorData.phRaw : null;
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
    // bug-dosage-fantome it.3 : ce OU manuel/auto est un verrou fonctionnel
    // voulu (pas de calibration pendant une injection, quelle qu'elle soit),
    // pas de la conflation d'affichage — CONSERVÉ tel quel.
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
    // feature-067 : INVARIANT — l'écran de calibration EZO n'affiche QUE le brut
    // (`phRaw`). Le repli sur `json.ph` a été retiré : ce champ porte maintenant la
    // valeur corrigée par l'offset logiciel, qui fausserait la lecture d'étalon.
    const phRaw = (typeof json.phRaw === "number" && !isNaN(json.phRaw)) ? json.phRaw : null;
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
      // bug-dosage-fantome it.3 : OU manuel/auto = verrou fonctionnel voulu
      // (pas de calibration pendant une injection), pas de la conflation
      // d'affichage — CONSERVÉ tel quel.
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
      // bug-dosage-fantome it.3 : OU manuel/auto = verrou fonctionnel voulu
      // (pas de calibration pendant une injection), pas de la conflation
      // d'affichage — CONSERVÉ tel quel.
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
      // v2.19.0 : la seule mémoire qui compte est celle réellement allouable
      // (pool 8 bits). `free_heap` (mesure brute historique) englobe une zone
      // technique — IRAM accessible en 32 bits seulement — qu'aucune allocation
      // ordinaire ne peut utiliser : elle passe en note secondaire.
      const heapKb = (v) => (Number.isFinite(v) ? `${Math.round(v / 1024)} KB` : null);
      const heapUsableFree = heapKb(data.free_heap_8bit);
      const heapUsableBlock = heapKb(data.max_alloc_8bit);
      const heapRawFree = heapKb(data.free_heap);
      const heapEl = $("#sys_free_heap");
      const heapNoteEl = $("#sys_heap_raw");
      if (heapEl) {
        if (heapUsableFree) {
          heapEl.textContent = heapUsableBlock
            ? `${heapUsableFree} · plus gros bloc : ${heapUsableBlock}`
            : heapUsableFree;
        } else {
          // Firmware antérieur à 2.19.0 : seule la mesure brute existe
          heapEl.textContent = heapRawFree || "—";
        }
      }
      if (heapNoteEl) {
        // Note affichée uniquement quand les deux mesures sont disponibles :
        // sinon la comparaison n'a pas de sens (et surtout, pas de "undefined")
        const showHeapNote = Boolean(heapUsableFree && heapRawFree);
        heapNoteEl.textContent = showHeapNote
          ? `Mesure brute : ${heapRawFree}, dont une zone technique inutilisable pour les allocations courantes.`
          : "";
        heapNoteEl.style.display = showHeapNote ? "" : "none";
      }
      // feature-066 : PSRAM (module WROVER) — psram_size = 0 sur module WROOM
      if ($("#sys_psram")) {
        $("#sys_psram").textContent = data.psram_size
          ? `${Math.round(data.psram_free / 1024)} / ${Math.round(data.psram_size / 1024)} KB libres`
          : "absente";
      }
      // Sans PSRAM : mise à jour en ligne (TLS GitHub) impossible → carte neutralisée
      applyOnlineUpdateAvailability(data.psram_size);
      $("#sys_flash_size").textContent = data.flash_size ? `${Math.round(data.flash_size / (1024 * 1024))} MB` : "—";

      if (data.sketch_size && data.ota_partition_size) {
        const used = Math.round(data.sketch_size / 1024);
        const total = Math.round(data.ota_partition_size / 1024);
        const pct = Math.round((data.sketch_size / data.ota_partition_size) * 100);
        $("#sys_app_usage").textContent = `${used} KB / ${total} KB (${pct}%)`;
      } else {
        $("#sys_app_usage").textContent = "—";
      }

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

  // Traduit les erreurs d'intégrité OTA (feature-026) en message français.
  // Retourne null si l'erreur n'est pas liée à l'intégrité.
  function integrityErrorMessage(errJson, isFs) {
    const code = errJson?.error;
    if (code === "integrity_digest_missing" || code === "integrity_digest_invalid") {
      return "Empreinte d'intégrité absente ou invalide — mise à jour refusée par sécurité.";
    }
    if (code === "integrity_mismatch") {
      return (
        "Échec de la vérification d'intégrité — image refusée, version actuelle conservée." +
        (isFs ? " Réinstallez le filesystem." : "")
      );
    }
    return null;
  }

  // Codes machine des refus 503 de la garde TLS sortante (/check-update,
  // /download-update). Contrat firmware ↔ front : on teste TOUJOURS le code,
  // jamais le texte du message (qui reste libre d'évoluer côté firmware).
  const OTA_CODE_PSRAM_REQUIRED = "psram_required"; // PSRAM ABSENTE → permanent
  const OTA_CODE_PSRAM_LOW = "psram_low";           // PSRAM saturée → temporaire
  const OTA_CODE_TLS_LOW_MEMORY = "tls_low_memory"; // heap dégradé → temporaire

  // La mise à jour en ligne exige de la PSRAM (session TLS GitHub, cf. garde
  // firmware 503 code=psram_required). Sur un module qui en est dépourvu,
  // l'indisponibilité est PERMANENTE : on neutralise la carte « Mise à jour en
  // ligne (GitHub) » dès la lecture de /get-system-info plutôt que de laisser
  // l'utilisateur buter sur un refus. Le toast reste le filet de sécurité si la
  // requête part quand même. Les refus TEMPORAIRES (psram_low, tls_low_memory)
  // ne neutralisent rien : l'utilisateur doit pouvoir réessayer.
  let onlineUpdateBlocked = false;

  function applyOnlineUpdateAvailability(psramSize) {
    if (typeof psramSize !== "number") return; // info absente : on ne touche à rien
    const blocked = psramSize === 0;
    const notice = $("#online_update_unavailable");
    const checkBtn = $("#check_update_btn");
    const installBtn = $("#install_update_btn");
    if (notice) notice.style.display = blocked ? "block" : "none";
    if (blocked) {
      if (checkBtn) checkBtn.disabled = true;
      if (installBtn) installBtn.disabled = true;
    } else if (onlineUpdateBlocked && checkBtn) {
      checkBtn.disabled = false; // matériel compatible : on lève un blocage précédent
    }
    onlineUpdateBlocked = blocked;
  }

  function bindGithubUpdate() {
    const checkBtn = $("#check_update_btn");
    const installBtn = $("#install_update_btn");

    const info = $("#github_update_info");
    const progress = $("#github_update_progress");
    const bar = $("#github_update_progress_bar");
    const status = $("#github_update_status");
    const barWrap = bar ? bar.parentElement : null;

    checkBtn?.addEventListener("click", async () => {
      checkBtn.disabled = true;
      checkBtn.textContent = "Vérification…";
      if (info) info.style.display = "none";
      if (installBtn) installBtn.disabled = true;
      if (progress) progress.style.display = "none";
      if (barWrap) barWrap.style.display = "";

      try {
        const res = await authFetch("/check-update");
        let data = null;
        if (!res.ok) {
          try {
            data = await res.json();
          } catch (_) {}
          const errMsg = data?.error || "Erreur vérification";
          const err = new Error(errMsg);
          err.code = data?.code; // ex. "psram_required" (503) — contrat machine
          throw err;
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
          // Fail-closed (feature-026) : sans empreinte d'intégrité, installation refusée
          if (!data.firmware_digest || !data.filesystem_digest) {
            installBtn.disabled = true;
            if (progress) progress.style.display = "block";
            if (barWrap) barWrap.style.display = "none";
            status.textContent =
              "Cette release ne fournit pas d'empreinte d'intégrité — installation refusée (sécurité).";
          } else {
            installBtn.disabled = !data.firmware_url;
          }
        } else {
          $("#update_available_msg").style.display = "none";
          $("#no_update_msg").style.display = "block";
          $("#no_update_msg").innerHTML = "<strong>✓ Vous avez la dernière version</strong>";
          $("#release_info").style.display = "none";
          installBtn.disabled = true;
        }
      } catch (e) {
        const msg = String(e?.message || "");
        if (e?.code === OTA_CODE_PSRAM_REQUIRED) {
          // Indisponibilité matérielle permanente, pas une panne : message du
          // firmware TEL QUEL, sans préfixe "Erreur", en tonalité informative.
          // Le code garantit désormais l'ABSENCE de PSRAM → neutralisation OK.
          applyOnlineUpdateAvailability(0);
          showToast(msg, "info");
        } else if (e?.code === OTA_CODE_PSRAM_LOW || e?.code === OTA_CODE_TLS_LOW_MEMORY) {
          // Indisponibilité TEMPORAIRE (PSRAM saturée ou heap dégradé) : on
          // informe sans neutraliser la carte ni afficher le callout « pas de
          // PSRAM ». L'état réel vient de /get-system-info, pas d'une valeur
          // inventée ici.
          showToast(msg || "Mise à jour en ligne momentanément indisponible, réessayez.", "info");
          loadSystemInfo();
        } else if (msg.includes("System time not synchronized")) {
          showToast("Heure non synchronisée — active le NTP ou règle l'heure avant de vérifier les mises à jour.", "error");
        } else {
          showToast("Erreur vérification mise à jour : " + (msg || "vérifie la connexion Internet de l'ESP32."), "error");
        }
      } finally {
        checkBtn.textContent = "Vérifier";
        // Reste désactivé si le matériel ne permet pas la mise à jour en ligne.
        checkBtn.disabled = onlineUpdateBlocked;
      }
    });

    installBtn?.addEventListener("click", async () => {
      if (!latestRelease || !latestRelease.firmware_url || !latestRelease.filesystem_url) {
        showToast("Aucune mise à jour disponible.", "info");
        return;
      }
      if (!latestRelease.firmware_digest || !latestRelease.filesystem_digest) {
        showToast("Cette release ne fournit pas d'empreinte d'intégrité — installation refusée (sécurité).", "error");
        return;
      }

      if (!(await confirmDialog({
        title: "Installer la mise à jour",
        message: `Installer ${latestRelease.latest_version}\n\n• Filesystem puis firmware\n• Ne pas couper l'alimentation`,
        confirmLabel: "Installer"
      }))) {
        return;
      }

      checkBtn.disabled = true;
      installBtn.disabled = true;

      progress.style.display = "block";
      if (barWrap) barWrap.style.display = "";
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
          body:
            "url=" + encodeURIComponent(latestRelease.filesystem_url) +
            "&digest=" + encodeURIComponent(latestRelease.filesystem_digest) +
            "&restart=false",
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
          const integrityMsg = integrityErrorMessage(fsJson, true);
          if (integrityMsg) {
            const err = new Error(integrityMsg);
            err.integrity = true;
            err.httpStatus = fsRes.status;
            throw err;
          }
          // httpStatus marque une réponse serveur (4xx/5xx) : le firmware a répondu,
          // ce n'est pas une coupure réseau. Voir l'étape 2 pour l'usage.
          const err = new Error(fsJson?.error || "Erreur téléchargement filesystem");
          err.httpStatus = fsRes.status;
          err.code = fsJson?.code; // ex. "psram_required" (503)
          throw err;
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
            body:
              "url=" + encodeURIComponent(latestRelease.firmware_url) +
              "&digest=" + encodeURIComponent(latestRelease.firmware_digest) +
              "&restart=true",
          });

          clearInterval(t);

          let fwJson = null;
          if (!fwRes.ok) {
            try {
              fwJson = await fwRes.json();
            } catch (_) {}
            const integrityMsg = integrityErrorMessage(fwJson, false);
            if (integrityMsg) {
              const err = new Error(integrityMsg);
              err.integrity = true;
              err.httpStatus = fwRes.status;
              throw err;
            }
            const err = new Error(fwJson?.error || "Erreur téléchargement firmware");
            err.httpStatus = fwRes.status;
            err.code = fwJson?.code; // ex. "psram_required" (503)
            throw err;
          }
          fwJson = await fwRes.json();
          fwSuccess = fwJson.status === "success";
        } catch (fwErr) {
          clearInterval(t);
          const errMsg = String(fwErr?.message || "");
          // Le firmware a RÉPONDU (4xx/5xx, ex. 503 psram_required ou mémoire
          // insuffisante) : l'image n'a pas été installée, ce n'est pas un
          // redémarrage. On propage l'erreur au lieu d'annoncer un succès.
          if (fwErr?.httpStatus !== undefined) {
            throw fwErr;
          }
          if (errMsg.includes("System time not synchronized") || fwErr?.integrity) {
            throw fwErr; // Re-throw time sync and integrity errors
          }
          // Coupure réseau pure (fetch rejeté, aucune réponse) : attendue quand
          // l'ESP32 redémarre après le flash → on considère l'installation OK.
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
        if (e?.code === OTA_CODE_PSRAM_REQUIRED) {
          // Matériel sans PSRAM : indisponibilité normale, pas un échec de mise
          // à jour → message firmware tel quel, sans préfixe, tonalité neutre.
          applyOnlineUpdateAvailability(0);
          if (barWrap) barWrap.style.display = "none";
          status.textContent = msg;
          showToast(msg, "info");
        } else if (e?.code === OTA_CODE_PSRAM_LOW || e?.code === OTA_CODE_TLS_LOW_MEMORY) {
          // Refus TEMPORAIRE : on n'affirme pas l'absence de PSRAM et on laisse
          // le bouton réutilisable (le finally/`onlineUpdateBlocked` inchangé).
          const info = msg || "Mise à jour en ligne momentanément indisponible, réessayez.";
          if (barWrap) barWrap.style.display = "none";
          status.textContent = info;
          showToast(info, "info");
          loadSystemInfo();
        } else if (e?.integrity) {
          status.textContent = "✗ " + msg;
        } else if (msg.includes("System time not synchronized")) {
          showToast("Heure non synchronisée — active le NTP ou règle l'heure avant de lancer la mise à jour.", "error");
        } else {
          showToast("Erreur mise à jour : " + e.message, "error");
        }
        checkBtn.disabled = onlineUpdateBlocked;
      }
    });
  }

  // Manual update (/update)

  // Libellés des types, alignés sur les <option> de #update_type
  const UPDATE_TYPE_LABELS = {
    firmware: "Firmware (.bin)",
    filesystem: "Système de fichiers (.bin)",
  };

  // Déduit le type d'image du NOM du fichier. Retourne "firmware",
  // "filesystem" ou null (indéterminé → aucune supposition affichée).
  // ORDRE IMPOSÉ : filesystem testé EN PREMIER. Le nom nu `littlefs.bin`
  // produit par build_fs.sh doit matcher — c'était le trou qui a permis
  // d'envoyer une image FS déclarée « firmware » (bug 2.20.1).
  function detectUpdateType(name) {
    const n = String(name || "").toLowerCase();
    if (!n) return null;
    // `fs.bin` doit être ANCRÉ : un simple endsWith classerait `refs.bin`,
    // `defs.bin` ou `mytoolfs.bin` en filesystem et pré-réglerait le type
    // silencieusement. Miroir de kFsSuffixes dans src/ota_type_logic.h :
    // toute évolution touche les DEUX côtés.
    if (n.includes("littlefs") || n.includes("spiffs") || n.includes("filesystem") ||
        /(?:^|[.\-_])fs\.bin$/.test(n)) {
      return "filesystem";
    }
    if (n.includes("firmware")) return "firmware";
    return null;
  }

  // Extrait la cause lisible d'un corps de réponse d'échec ("FAIL: <cause>").
  function parseUpdateFailure(responseText, httpStatus) {
    let cause = String(responseText || "").trim();
    cause = cause.replace(/^FAIL\s*:?\s*/i, "").trim();
    if (cause.length > 200) cause = cause.slice(0, 200);
    if (!cause) {
      cause = httpStatus ? `erreur HTTP ${httpStatus} — voir le Journal` : "cause inconnue — voir le Journal";
    }
    return cause;
  }

  function bindManualUpdate() {
    const file = $("#update_file");
    const btn = $("#update_btn");
    const type = $("#update_type");
    const notice = $("#update_type_notice");

    // Type déduit du fichier courant (null = indéterminé ou aucun fichier)
    let detectedType = null;

    // Rendu de la notice. display:block est posé AVANT l'écriture du texte,
    // dans une frame séparée : la région live doit exister dans l'arbre
    // d'accessibilité avant sa mutation, sinon rien n'est annoncé.
    function showNotice(kind, text) {
      if (!notice) return;
      notice.className = kind === "warn" ? "callout callout--warn" : "callout callout--info";
      notice.style.display = "block";
      requestAnimationFrame(() => {
        notice.textContent = text;
      });
    }

    function hideNotice() {
      if (!notice) return;
      notice.textContent = "";
      notice.style.display = "none";
    }

    function fileName() {
      return file && file.files && file.files.length ? file.files[0].name : "";
    }

    function typeLabel(value) {
      return UPDATE_TYPE_LABELS[value] || value;
    }

    // Met la notice à jour selon la cohérence courante fichier ↔ type.
    function refreshNotice() {
      const name = fileName();
      if (!name || !detectedType) {
        hideNotice();  // Indéterminé = silence total
        return;
      }
      if (detectedType === type.value) {
        showNotice("info",
          `Type réglé sur « ${typeLabel(type.value)} » d'après le nom du fichier (${name}). Modifiable si besoin.`);
      } else {
        showNotice("warn",
          `Le type sélectionné (« ${typeLabel(type.value)} ») ne correspond pas au nom du fichier (${name}). ` +
          `Une image envoyée avec le mauvais type échoue, ou écrit dans la mauvaise partition.`);
      }
    }

    file?.addEventListener("change", () => {
      btn.disabled = !(file.files && file.files.length);
      detectedType = detectUpdateType(fileName());
      if (!detectedType) {
        hideNotice();
        return;
      }
      type.value = detectedType;  // Pré-réglage, toujours modifiable
      refreshNotice();
    });

    type?.addEventListener("change", refreshNotice);

    btn?.addEventListener("click", async () => {
      if (!(file.files && file.files.length)) return;

      // Garde-fou SOUPLE : jamais de blocage dur, un clic suffit à passer outre.
      if (detectedType && detectedType !== type.value) {
        const ok = await confirmDialog({
          title: "Type de mise à jour incohérent",
          message: `Le fichier « ${fileName()} » ressemble à une image « ${typeLabel(detectedType)} ».\n\n` +
                   `Vous avez sélectionné le type « ${typeLabel(type.value)} ».\n\n` +
                   `Envoyer quand même ?`,
          confirmLabel: "Envoyer quand même",
          cancelLabel: "Annuler",
          danger: true,
        });
        if (!ok) return;  // Sortie silencieuse, aucun état modifié
      }

      const formData = new FormData();
      formData.append("update_type", type.value);
      formData.append("update_file", file.files[0]);

      const progressWrap = $("#update_progress");
      const bar = $("#update_progress_bar");
      const status = $("#update_status");

      progressWrap.style.display = "block";
      bar.style.width = "0%";
      bar.textContent = "0%";
      bar.classList.remove("error");
      bar.setAttribute("aria-valuenow", "0");
      status.classList.remove("is-error");  // Purge l'échec précédent
      status.textContent = "Préparation…";

      btn.disabled = true;
      file.disabled = true;
      type.disabled = true;

      const xhr = new XMLHttpRequest();
      let lastAnnounced = -1;  // Dernier palier de 10 % annoncé

      xhr.upload.addEventListener("progress", (e) => {
        if (!e.lengthComputable) return;
        const percent = Math.round((e.loaded / e.total) * 100);
        bar.style.width = percent + "%";
        bar.textContent = percent + "%";
        bar.setAttribute("aria-valuenow", String(percent));
        // #update_status est une région aria-live polite : n'écrire que par pas
        // de 10 %, sinon le lecteur d'écran est saturé d'annonces.
        const step = percent >= 100 ? 100 : Math.floor(percent / 10) * 10;
        if (step !== lastAnnounced) {
          lastAnnounced = step;
          status.textContent = percent < 100 ? `Envoi: ${step}%` : "Envoi terminé, application en cours…";
        }
      });

      xhr.addEventListener("load", () => {
        if (xhr.status === 200 && xhr.responseText.trim() === "OK") {
          bar.style.width = "100%";
          bar.textContent = "100%";
          bar.setAttribute("aria-valuenow", "100");
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
          bar.setAttribute("aria-valuenow", "100");
          status.textContent = "✗ Échec : " + parseUpdateFailure(xhr.responseText, xhr.status);
          status.classList.add("is-error");
          btn.disabled = false;
          file.disabled = false;
          type.disabled = false;
        }
      });

      xhr.addEventListener("error", () => {
        status.textContent = "✗ Erreur réseau";
        status.classList.add("is-error");
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
      // Pagination transparente (bug-crash-getlogs-badalloc) : /get-logs renvoie
      // des pages bornées (~30 entrées) + un flag `truncated`. On boucle avec
      // ?since=<dernier timestamp reçu> tant que truncated === true. Gardes
      // anti-boucle : max 8 itérations ET curseur strictement croissant.
      // Les modes complet et incrémental partagent la même boucle (seul le
      // curseur de départ diffère).
      let cursor = (incremental && lastLogTimestamp > 0) ? lastLogTimestamp : 0;
      let collected = [];
      let uptimeMs = null;
      for (let i = 0; i < 8; i++) {
        const url = cursor > 0 ? `/get-logs?since=${cursor}` : "/get-logs";
        const res = await authFetch(url);
        const data = await res.json();
        if (uptimeMs == null && data.uptime_ms != null) uptimeMs = data.uptime_ms;

        const lines = Array.isArray(data) ? data : data.logs || [];
        collected = collected.concat(lines);

        let maxTs = cursor;
        lines.forEach(entry => {
          const ts = typeof entry === 'string' ? 0 : (entry.timestamp || 0);
          if (ts > maxTs) maxTs = ts;
        });

        if (data.truncated !== true) break;   // dernière page atteinte
        if (maxTs <= cursor) break;           // curseur figé → jamais de boucle infinie
        cursor = maxTs;
      }

      // Initialiser _bootEpochMs depuis uptime_ms si pas encore connu via WebSocket
      if (_bootEpochMs == null && uptimeMs != null) {
        _bootEpochMs = Date.now() - uptimeMs;
      }

      if (incremental) {
        // En mode incrémental, ajouter les nouveaux logs reçus
        if (collected.length > 0) {
          allLogEntries = [...allLogEntries, ...collected];
          if (allLogEntries.length > 500) allLogEntries = allLogEntries.slice(-500);
        }
      } else {
        // En mode complet, remplacer tous les logs
        allLogEntries = collected;
        lastLogTimestamp = 0;
      }

      // Mettre à jour le dernier timestamp
      collected.forEach(entry => {
        const ts = typeof entry === 'string' ? 0 : (entry.timestamp || 0);
        if (ts > lastLogTimestamp) lastLogTimestamp = ts;
      });

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
        if (!(await confirmDialog({
          title: "Réinitialiser les sondes",
          message: "Réinitialiser l'identification des deux sondes ?\n\nVous devrez les ré-identifier ensuite.",
          confirmLabel: "Réinitialiser",
          danger: true
        }))) return;
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

  // ========== TRACE SIGNAUX CAPTEURS (feature-057) ==========
  // Charge /debug/sensor_trace à l'activation de l'onglet Avancé et sur clic
  // « Recharger ». Pas de polling permanent. 2 graphes uPlot (pH, ORP), stats
  // min/max/Δ calculées en ignorant les null.
  let _tracePhChart = null;
  let _traceOrpChart = null;
  let _traceDispPhChart = null;
  let _traceDispOrpChart = null;
  let _traceLoading = false;

  // Seuils tracés sur les graphes de dispersion (feature-072) : dispersion_max
  // de /debug/sensor_filter_state si chargée, sinon repli constantes JS —
  // le tracé ne dépend jamais du succès du fetch route (invariant 071).
  const _dispThresholds = { ph: null, orp: null };
  const kPhDispersionSeuilFallback = 0.5;   // = kPhFilterMaxDispersion (constants.h)
  const kOrpDispersionSeuilFallback = 100;  // = kOrpFilterMaxDispersion (constants.h)

  function _dispSeuil(kind) {
    return _dispThresholds[kind] ?? (kind === "ph" ? kPhDispersionSeuilFallback : kOrpDispersionSeuilFallback);
  }

  const _fmtVirgule = (v, decimals) => v.toFixed(decimals).replace(".", ",");

  function _initTraceCharts() {
    if (_tracePhChart || typeof uPlot === "undefined") return;
    const phHost = $("#trace_ph_chart");
    const orpHost = $("#trace_orp_chart");
    if (phHost) {
      _tracePhChart = createTraceChart(phHost, [
        { label: "pH brut", color: "#a78bfa" },
        { label: "pH lissé", color: "#7c3aed" }
      ]);
    }
    if (orpHost) {
      _traceOrpChart = createTraceChart(orpHost, [
        { label: "ORP brut", color: "#34d399" },
        { label: "ORP lissé", color: "#059669" }
      ]);
    }
    // Graphes dispersion (feature-072) : tracé linéaire + points visibles
    // (PAS de spline : un point valide isolé entre deux trous doit se voir),
    // ligne de seuil pointillée exclue du tooltip, échelle Y incluant le seuil.
    const dispPhHost = $("#trace_disp_ph_chart");
    const dispOrpHost = $("#trace_disp_orp_chart");
    if (dispPhHost) {
      _traceDispPhChart = createTraceChart(dispPhHost, [
        { label: "Dispersion pH", color: "#d97706", linear: true, points: true,
          tooltip: (v) => `Dispersion : ${_fmtVirgule(v, 3)}` },
        { label: "Seuil", color: "#dc2626", width: 1, dash: [6, 6], linear: true, noTooltip: true }
      ], { yInclude: _dispSeuil("ph") });
    }
    if (dispOrpHost) {
      _traceDispOrpChart = createTraceChart(dispOrpHost, [
        { label: "Dispersion ORP", color: "#d97706", linear: true, points: true,
          tooltip: (v) => `Dispersion : ${_fmtVirgule(v, 1)}` },
        { label: "Seuil", color: "#dc2626", width: 1, dash: [6, 6], linear: true, noTooltip: true }
      ], { yInclude: _dispSeuil("orp") });
    }
  }

  function _traceMinMax(arr) {
    let mn = null, mx = null;
    for (const v of arr) {
      if (v == null || isNaN(v)) continue;
      if (mn == null || v < mn) mn = v;
      if (mx == null || v > mx) mx = v;
    }
    return { min: mn, max: mx };
  }

  function _setTraceStat(id, arr, decimals, withDelta = true) {
    const el = $(id);
    if (!el) return;
    const { min, max } = _traceMinMax(arr);
    if (min == null || max == null) {
      el.textContent = "—";
      return;
    }
    let txt = `min ${min.toFixed(decimals)} · max ${max.toFixed(decimals)}`;
    // « Δ 25 min » : crête-à-crête sur TOUT le buffer (~300 pts ≈ 25 min), dérive
    // lente comprise — sans rapport avec la dispersion du drapeau unstable.
    if (withDelta) txt += ` · Δ 25 min ${(max - min).toFixed(decimals)}`;
    el.textContent = txt;
  }

  // ---- Dispersion du drapeau `unstable` (feature-071) ----
  // Valeur instantanée publiée par /debug/sensor_filter_state (v2.20.6) :
  // amplitude max−min de la fenêtre médiane pleine, la grandeur qui décide
  // de l'inhibition du dosage.
  let _filterStateLoading = false;

  function _setDispersionRow(id, st, decimals) {
    const el = $(id);
    if (!el) return;
    el.textContent = "";
    // Piège docs/API.md §Marge d'instabilité : dispersion null + unstable=false
    // signifie « aucun jugement rendu », PAS « stable ». On ne juge que si
    // dispersion est un nombre.
    if (!st || typeof st.dispersion !== "number") {
      const note = document.createElement("span");
      note.className = "muted";
      note.textContent = "— · pas de jugement (circulation arrêtée ou fenêtre en cours de remplissage)";
      el.appendChild(note);
      return;
    }
    const seuil = (typeof st.dispersion_max === "number") ? ` / ${st.dispersion_max}` : "";
    el.appendChild(document.createTextNode(`${st.dispersion.toFixed(decimals)}${seuil} `));
    const pill = document.createElement("span");
    pill.className = st.unstable ? "pill bad" : "pill ok";
    pill.textContent = st.unstable ? "INSTABLE" : "stable";
    el.appendChild(pill);
  }

  async function loadFilterState() {
    if (_filterStateLoading) return;
    _filterStateLoading = true;
    try {
      const res = await authFetch("/debug/sensor_filter_state");
      if (!res.ok) throw new Error(`HTTP ${res.status}`);
      const json = await res.json();
      _setDispersionRow("#trace_disp_ph", json.ph, 3);
      _setDispersionRow("#trace_disp_orp", json.orp, 1);
      // Seuils des graphes de dispersion (feature-072) : mémorise dispersion_max
      // et ré-applique au graphe déjà tracé (légende + échelle). Pas de
      // chaînage avec loadSensorTrace : une panne de l'un n'affecte pas l'autre.
      if (typeof json.ph?.dispersion_max === "number") _dispThresholds.ph = json.ph.dispersion_max;
      if (typeof json.orp?.dispersion_max === "number") _dispThresholds.orp = json.orp.dispersion_max;
      _applyDispThreshold("ph", _traceDispPhChart, "#trace_disp_ph_seuil", (v) => _fmtVirgule(v, 2));
      _applyDispThreshold("orp", _traceDispOrpChart, "#trace_disp_orp_seuil", (v) => `${v} mV`);
    } catch (e) {
      console.error("[trace] filter state load failed:", e);
      // Panne isolée : seules ces deux lignes se dégradent, le reste de la
      // carte (trace, stats) reste fonctionnel.
      for (const id of ["#trace_disp_ph", "#trace_disp_orp"]) {
        const el = $(id);
        if (el) el.textContent = "indisponible";
      }
    } finally {
      _filterStateLoading = false;
    }
  }

  // Reconstruction de la dispersion firmware sur la trace affichée (indicatif).
  // Fenêtre glissante de 13 points = kSensorFilterCentralWindow (constants.h).
  // Une fenêtre est écartée si elle contient un trou (null/NaN) ou un saut
  // consécutif ≥ spikeReject : le firmware rejette ces pics AVANT la fenêtre
  // médiane, la trace brute les contient encore. Précédent réel : sans ce
  // filtre, faux max 5,425 sur un transitoire de redémarrage de filtration
  // là où le firmware voyait 0,308.
  function _traceDispersionStats(values, spikeReject) {
    const kWindow = 13; // = kSensorFilterCentralWindow (firmware)
    if (!Array.isArray(values) || values.length < kWindow) return null;
    const dispersions = [];
    // Série alignée sur values (feature-072) : series[i+12] = dispersion de la
    // fenêtre [i, i+12] (point au temps du dernier échantillon), null pour les
    // 12 premiers indices et les fenêtres écartées → mêmes xs que les graphes
    // brut/lissé, et les trous restent des trous (transitoire rejeté).
    const series = new Array(values.length).fill(null);
    for (let i = 0; i + kWindow <= values.length; i++) {
      let mn = Infinity, mx = -Infinity, valide = true;
      for (let j = i; j < i + kWindow; j++) {
        const v = values[j];
        if (v == null || isNaN(v)) { valide = false; break; }
        if (j > i && Math.abs(v - values[j - 1]) >= spikeReject) { valide = false; break; }
        if (v < mn) mn = v;
        if (v > mx) mx = v;
      }
      if (valide) {
        const d = mx - mn;
        dispersions.push(d);
        series[i + kWindow - 1] = d;
      }
    }
    if (dispersions.length === 0) return null;
    dispersions.sort((a, b) => a - b);
    const mid = Math.floor(dispersions.length / 2);
    const median = (dispersions.length % 2 === 1)
      ? dispersions[mid]
      : (dispersions[mid - 1] + dispersions[mid]) / 2;
    return { median, max: dispersions[dispersions.length - 1], windows: dispersions.length, series };
  }

  function _setTraceDispRecon(id, stats, decimals) {
    const el = $(id);
    if (!el) return;
    el.textContent = stats
      ? `médiane ${stats.median.toFixed(decimals)} · max ${stats.max.toFixed(decimals)}`
      : "—";
  }

  // Alimente un graphe de dispersion (feature-072). stats null (< 13 points,
  // ou aucune fenêtre valide) → bloc masqué, pas d'erreur. La ligne de seuil
  // est une série constante alignée sur xs.
  function _renderDispChart(chart, blockId, xs, stats, seuil) {
    const block = $(blockId);
    if (!block) return;
    if (!chart || !stats) { block.hidden = true; return; }
    chart.setYInclude(seuil);
    chart.setData(xs, [stats.series, xs.map(() => seuil)]);
    block.hidden = false;
  }

  // Ré-applique le seuil (route ou repli) : span de légende, puis échelle Y et
  // ligne pointillée si le graphe est déjà tracé — u.data[1] (la dispersion)
  // est conservée telle quelle.
  function _applyDispThreshold(kind, chart, spanId, fmt) {
    const seuil = _dispSeuil(kind);
    const span = $(spanId);
    if (span) span.textContent = fmt(seuil);
    if (chart && chart.st.xs.length) {
      chart.setYInclude(seuil);
      chart.setData(chart.st.xs, [chart.u.data[1], chart.st.xs.map(() => seuil)]);
    }
  }

  async function loadSensorTrace() {
    if (_traceLoading) return;
    _traceLoading = true;
    const status = $("#trace_status");
    const charts = $("#trace_charts");
    const countEl = $("#trace_count");
    try {
      // Route paginée (bug-crash-sensor-trace-badalloc) : on agrège les pages
      // séquentiellement jusqu'à couvrir `count` (total buffer). Borne dure de
      // 12 itérations (300 échantillons / 50 par page = 6 en nominal).
      const samples = [];
      let offset = 0;
      for (let i = 0; i < 12; i++) {
        const res = await authFetch(`/debug/sensor_trace?offset=${offset}`);
        if (res.status === 503) {
          if (status) { status.textContent = "Mémoire fragmentée, réessayez."; status.hidden = false; }
          if (charts) charts.hidden = true;
          return;
        }
        if (!res.ok) throw new Error(`HTTP ${res.status}`);
        const json = await res.json();
        const page = Array.isArray(json.samples) ? json.samples : [];
        samples.push(...page);
        const returned = Number(json.returned ?? page.length);
        const total = Number(json.count ?? 0);
        if (returned === 0 || total === 0) break;  // buffer vide ou fin de page
        offset += returned;
        if (offset >= total) break;                // trace complète
      }

      if (countEl) countEl.textContent = String(samples.length);

      if (samples.length === 0) {
        if (status) { status.textContent = "Aucune donnée dans le buffer."; status.hidden = false; }
        if (charts) charts.hidden = true;
        return;
      }

      _initTraceCharts();

      const num = (v) => (v == null || isNaN(v)) ? null : v;
      const t0 = samples[0].t ?? 0;
      // Secondes relatives, arrondies à 0,1 s
      const xs = samples.map(s => Math.round((((s.t ?? 0) - t0) / 100)) / 10);
      const phRaw = samples.map(s => num(s.phRaw));
      const phFilt = samples.map(s => num(s.phFiltered));
      const orpRaw = samples.map(s => num(s.orpRaw));
      const orpFilt = samples.map(s => num(s.orpFiltered));
      const temp = samples.map(s => num(s.tempEau));

      _tracePhChart?.setData(xs, [phRaw, phFilt]);
      _traceOrpChart?.setData(xs, [orpRaw, orpFilt]);

      _setTraceStat("#trace_stats_ph_raw", phRaw, 2);
      _setTraceStat("#trace_stats_ph_filtered", phFilt, 2);
      _setTraceStat("#trace_stats_orp_raw", orpRaw, 1);
      _setTraceStat("#trace_stats_orp_filtered", orpFilt, 1);
      _setTraceStat("#trace_stats_temp", temp, 1, false);

      // Dispersion reconstruite sur la trace (fenêtre 13, seuils de rejet
      // dupliqués du firmware : kPhFilterSpikeReject = 1.0 pH,
      // kOrpFilterSpikeReject = 300 mV — constants.h).
      const phDispStats = _traceDispersionStats(phRaw, 1.0);
      const orpDispStats = _traceDispersionStats(orpRaw, 300.0);
      _setTraceDispRecon("#trace_disp_recon_ph", phDispStats, 3);
      _setTraceDispRecon("#trace_disp_recon_orp", orpDispStats, 1);

      // Courbes de dispersion (feature-072) — même axe X que les graphes du dessus
      _renderDispChart(_traceDispPhChart, "#trace_disp_ph_block", xs, phDispStats, _dispSeuil("ph"));
      _renderDispChart(_traceDispOrpChart, "#trace_disp_orp_block", xs, orpDispStats, _dispSeuil("orp"));

      if (status) status.hidden = true;
      if (charts) charts.hidden = false;
    } catch (e) {
      console.error("[trace] load failed:", e);
      if (status) { status.textContent = "Trace indisponible (appareil injoignable)."; status.hidden = false; }
      if (charts) charts.hidden = true;
    } finally {
      _traceLoading = false;
    }
  }

  function bindSensorTrace() {
    // loadFilterState() est volontairement une promesse séparée, jamais
    // await-ée ni chaînée à loadSensorTrace() : une panne de l'une ne doit
    // pas dégrader l'autre.
    $("#trace_reload_btn")?.addEventListener("click", () => { loadSensorTrace(); loadFilterState(); });

    $("#trace_clear_btn")?.addEventListener("click", async () => {
      if (!(await confirmDialog({
        title: "Vider le buffer",
        message: "Vider le buffer de trace des signaux capteurs ?\n\nLes données enregistrées seront perdues.",
        confirmLabel: "Vider",
        danger: true
      }))) return;
      try {
        const res = await authFetch("/debug/sensor_trace_clear", { method: "POST" });
        if (!res.ok) throw new Error(`HTTP ${res.status}`);
        showToast("Buffer vidé", "success");
        await loadSensorTrace();
      } catch (e) {
        console.error("[trace] clear failed:", e);
        showToast("Échec du vidage du buffer", "error");
      }
    });

    // Chargement à l'activation de l'onglet Avancé uniquement (pas de polling permanent)
    const onRouteChange = () => {
      if (_isPanelDevActive()) { loadSensorTrace(); loadFilterState(); }
    };
    window.addEventListener("hashchange", onRouteChange);
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
      if (!(await confirmDialog({
        title: "Effacer les logs",
        message: "Effacer tous les logs côté ESP32 ?\n\nLa mémoire RAM ET le fichier persistant seront vidés.\nCette action est irréversible.",
        confirmLabel: "Effacer",
        danger: true
      }))) return;
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
        showToast("Erreur lors de la sauvegarde MQTT.", "error");
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
        lightingDirty = false;  // bug-ui-eclairage-refresh-ws : édition sauvegardée
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

  // bug-dosage-fantome it.2 (W4c/d) : duty d'exploitation d'une pompe (miroir de
  // getPumpAmplitudeDuty côté firmware : pct × 255 / 100, division entière).
  // C'est à CE duty que la mesure du débit doit être faite pour que la calibration serve.
  function pumpAmplitudeDuty(pct) {
    return Math.floor((pct * 255) / 100);
  }

  // feature-074 : ligne LECTURE SEULE par pompe (le curseur d'amplitude est
  // retiré) — « Amplitude d'impulsion : X % — débit Y ml/min (valeur usine|mesuré) ».
  // X = pumpN_max_duty_pct RÉEL de get-config (peut différer de 50 si hérité),
  // provenance selon pumpN_flow_weighed. Pré-remplit aussi le champ unique de
  // saisie avancée avec le débit courant — jamais pendant une saisie en cours,
  // la config étant aussi poussée par le WebSocket (patron ph_offset).
  function updatePumpCalibrationDisplay(cfg) {
    [1, 2].forEach((n) => {
      // PIÈGE (même leçon que la parité /data des pentes, 2.21.2) : la config
      // arrive aussi par la trame WS, dont le contrat peut être en retard sur
      // /get-config. Clé ABSENTE = pas d'information, JAMAIS « zéro » — sinon
      // le premier push WS écrase le pré-remplissage et affiche « débit non
      // caractérisé » alors que la calibration existe. On ne touche à rien.
      if (cfg[`pump${n}_pulse_flow_ml_per_min`] === undefined) return;
      const calDuty = typeof cfg[`pump${n}_calibrated_duty`] === "number" ? cfg[`pump${n}_calibrated_duty`] : 0;
      const calFlow = typeof cfg[`pump${n}_pulse_flow_ml_per_min`] === "number" ? cfg[`pump${n}_pulse_flow_ml_per_min`] : 0;
      const ampPct = typeof cfg[`pump${n}_max_duty_pct`] === "number" ? cfg[`pump${n}_max_duty_pct`] : 50;
      const weighed = cfg[`pump${n}_flow_weighed`] === true;
      const flowEl = $(`#pump${n}_cal_flow`);
      // Calibration effacée (cal_duty=0) : VIDER le champ (retour au
      // placeholder) — sinon l'ancienne mesure périmée resterait affichée.
      if (flowEl && document.activeElement !== flowEl) flowEl.value = calFlow > 0 ? calFlow : "";
      const info = $(`#pump${n}_cal_info`);
      if (info) {
        info.textContent = (calDuty > 0 && calFlow > 0)
          ? `Amplitude d'impulsion : ${ampPct} % — débit ${calFlow.toFixed(1)} ml/min (${weighed ? "mesuré" : "valeur usine"})`
          : `Amplitude d'impulsion : ${ampPct} % — débit non caractérisé`;
      }
    });
  }

  // bug-dosage-fantome it.2 (W4b) : badge « Débit non caractérisé — mesure
  // requise » par pompe PHYSIQUE. Les drapeaux de /data sont par CHIMIE — on
  // les route vers la pompe affectée (ph_pump/orp_pump). Champs absents
  // (payload WS sans ces clés, firmware plus ancien) → on conserve l'état
  // affiché, pas de scintillement ni d'UI figée.
  // feature-074 : ce badge est RESTREINT au cas cal_duty=0 (effacement explicite
  // / NVS hérité, seul état encore fail-closed) — la distinction « valeur usine
  // vs mesuré » relève de la ligne lecture seule, jamais de ce badge.
  function updatePumpCharacterizationBadges(data) {
    if (!data || data.ph_pump_flow_uncharacterized === undefined) return;
    const cfg = window._config || {};
    const phPump = cfg.ph_pump === 2 ? 2 : 1;
    [1, 2].forEach((n) => {
      const badge = $(`#pump${n}_uncal_badge`);
      if (!badge) return;
      const flag = (phPump === n)
        ? data.ph_pump_flow_uncharacterized === true
        : data.orp_pump_flow_uncharacterized === true;
      badge.hidden = !flag;
    });
  }

  function bindPumpConfigSave() {
    // feature-074 : plus de curseurs d'amplitude — seuls ph_pump/orp_pump
    // rendent le formulaire « sale ».
    trackDirtyState("#pump_config_save_btn", ["ph_pump", "orp_pump"]);
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
      // feature-074 : pumpN_max_duty_pct VOLONTAIREMENT absent (amplitude gelée,
      // immunité anti-onglets-périmés). pump_max_flow_ml_per_min : SUPPRIMÉ
      // (bug-dosage-fantome it.2) — le débit vient de la mesure par pompe.
      const cfg = {
        ph_pump:  parseInt($("#ph_pump")?.value || "1", 10),
        orp_pump: parseInt($("#orp_pump")?.value || "2", 10),
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

    // feature-074 : enregistrement du débit mesuré par pompe (« saisie avancée »
    // depuis feature-075) — champ UNIQUE « débit mesuré (ml/min) ». Le duty
    // calibré n'est PLUS envoyé : le serveur le déduit de l'amplitude courante
    // et pose le drapeau « mesuré ». Validation client miroir (débit ∈ ]0..200],
    // vide/0/NaN refusés) ; le serveur reste l'autorité (refus HTTP 400 remonté
    // par sendConfigDetailed). L'assistant guidé (bindFlowWizard) réutilise ce
    // même chemin /save-config à l'étape « Volume recueilli ».
    [1, 2].forEach((n) => {
      const calBtn = $(`#pump${n}_cal_save_btn`);
      if (!calBtn) return;
      calBtn.addEventListener("click", async () => {
        if (calBtn.disabled) return;
        const flowRaw = ($(`#pump${n}_cal_flow`)?.value || "").trim();
        const flow = parseFloat(flowRaw.replace(",", "."));
        if (flowRaw === "" || isNaN(flow) || flow <= 0 || flow > 200) {
          showToast("Débit mesuré invalide : saisir une valeur supérieure à 0 et au plus 200 ml/min.", "error");
          return;
        }
        calBtn.disabled = true;
        const payload = {};
        payload[`pump${n}_pulse_flow_ml_per_min`] = flow;
        const r = await sendConfigDetailed(payload);
        calBtn.disabled = false;
        if (r.ok) {
          showToast(`Débit pompe ${n} enregistré : ${flow.toFixed(1)} ml/min.`, "success");
          // Rafraîchit la ligne lecture seule depuis get-config : l'indication
          // passe à « mesuré ».
          setTimeout(loadConfig, 500);
          setTimeout(() => loadSensorData({ force: true, source: "pump-cal" }), 800);
        } else {
          showToast(r.error || "Erreur lors de l'enregistrement du débit.", "error");
        }
      });
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

    // Débit majorant à duty 255 pour la pompe `n` (1|2), SERVI par /data
    // (flow_assist.flow255[n-1]) — plus de miroir en dur de la constante
    // firmware (kPumpFlow255MlPerMin) : une valeur de sécurité n'a pas de copie
    // dans le navigateur, sinon l'UI annonce une durée que la pompe ne tient pas.
    // PIÈGE : clé absente ≠ tableau vide ≠ zéro. Seul un nombre fini > 0 est une
    // information ; tout le reste retombe sur le repli.
    // REPLI = valeurs des firmwares ANTÉRIEURS à la fusion de flotte (61 pompe 1,
    // 67 pompe 2), qui ne publient pas flow255 et dosent bien à ces débits-là.
    // Ne PAS l'aligner sur 74 : ce serait mentir sur un firmware ancien.
    const FLOW_255_FALLBACK = { 1: 61.0, 2: 67.0 };
    const flow255For = (pumpNum) => {
      const served = latestSensorData?.flow_assist?.flow255;
      const v = Array.isArray(served) ? served[pumpNum - 1] : undefined;
      return (typeof v === "number" && isFinite(v) && v > 0) ? v : FLOW_255_FALLBACK[pumpNum];
    };
    // Calcule la durée d'injection (secondes) pour un volume donné (mL).
    // bug-dosage-fantome it.2 (W3) : miroir best-effort de planManualInject
    // (web_routes_control.cpp) — le modèle linéaire duty↔débit est mort, plus
    // JAMAIS d'interpolation : amplitude == duty calibré → débit mesuré ; sinon
    // la pompe est forcée à 255 et le débit majorant SERVI s'applique.
    // Le firmware fait autorité.
    const calcInjectDurationS = (product, volumeMl) => {
      const cfg = window._config;
      if (!cfg) return null;
      const pumpNum = (product === "ph" ? cfg.ph_pump : cfg.orp_pump) === 2 ? 2 : 1;
      const dutyPct = pumpNum === 1 ? (cfg.pump1_max_duty_pct ?? 50) : (cfg.pump2_max_duty_pct ?? 50);
      const calDuty = pumpNum === 1 ? (cfg.pump1_calibrated_duty ?? 0) : (cfg.pump2_calibrated_duty ?? 0);
      const calFlow = pumpNum === 1 ? (cfg.pump1_pulse_flow_ml_per_min ?? 0) : (cfg.pump2_pulse_flow_ml_per_min ?? 0);
      const ampDuty = pumpAmplitudeDuty(dutyPct);
      const flow = (calDuty > 0 && ampDuty === calDuty && calFlow > 0)
        ? calFlow
        : flow255For(pumpNum);
      return Math.max(1, Math.round((volumeMl / flow) * 60));
    };

    // Miroir de kManualInjectMaxDurationS (constants.h) : une injection manuelle
    // est plafonnée à 10 min côté firmware. Au-delà, le volume demandé est écrêté.
    const MAX_INJECT_DURATION_S = 600;

    // Met à jour le hint "≈ Xmin" sous le champ volume. Reflète le plafond de
    // durée : si le volume demandé dépasse ce qu'une injection peut délivrer,
    // afficher la durée/volume RÉELS (plafonnés) + inviter à répéter.
    const updateInjectHint = (product) => {
      const mlInput = $(`#${product}_inject_ml`);
      const hint = $(`#${product}_inject_hint`);
      if (!mlInput || !hint) return;
      const vol = parseFloat(mlInput.value);
      if (!vol || vol <= 0) { hint.textContent = ""; return; }
      const rawDur = calcInjectDurationS(product, vol);
      if (rawDur == null) { hint.textContent = ""; return; }
      if (rawDur > MAX_INJECT_DURATION_S) {
        // Volume > max par injection : durée écrêtée à 10 min, volume effectif proportionnel.
        const effMl = Math.round(vol * MAX_INJECT_DURATION_S / rawDur);
        hint.textContent = `≈ ${fmtInjectRemaining(MAX_INJECT_DURATION_S)} — plafonné à ~${effMl} mL par injection (répéter pour le reste)`;
      } else {
        hint.textContent = `≈ ${fmtInjectRemaining(rawDur)}`;
      }
    };

    // feature-006 : messages français des codes de refus 409 du firmware
    const INJECT_REFUSAL_MESSAGES = {
      watchdog_inactive: "Injection refusée : watchdog inactif (sécurité).",
      filtration_off: "Injection refusée : la filtration doit être active avant d'injecter (sécurité chimique : pas de circulation = surdosage local).",
      stabilization_in_progress: "Injection refusée : stabilisation post-calibration en cours.",
      already_injecting: "Injection refusée : une injection manuelle est déjà en cours — l'arrêter avant d'en relancer une.",
      daily_limit: "Injection refusée : limite journalière atteinte.",
      hourly_limit: "Injection refusée : limite horaire d'injection atteinte — réessayer plus tard.",
      max_cycles: "Injection refusée : nombre maximal de démarrages de pompe atteint pour aujourd'hui.",
      burst_limit: "Injection refusée : trop de démarrages rapprochés (anti-rafale) — patienter avant de réessayer.",
      flow_uncharacterized: "Injection refusée : débit de la pompe non caractérisé — effectuez la mesure du débit (Réglages → Avancé → Configuration des pompes)."
    };

    // feature-006 : raison de blocage CONNUE côté client (miroir best-effort des
    // gardes firmware, pour désactiver le bouton de façon proactive). Le firmware
    // reste l'autorité (409 sinon). Sans données (WS déconnecté), on ne bloque
    // PAS : le bouton reste cliquable et le firmware tranche.
    const getInjectBlockReason = (data, product) => {
      if (!data) return "";
      const config = window._config || {};
      if (data[`${product}_limit_reached`] === true) {
        return "Limite journalière atteinte — injection impossible jusqu'à minuit";
      }
      // Stabilisation PAR POMPE (miroir de la garde firmware) : une calibration
      // ORP ne doit pas bloquer le bouton pH, et inversement. Fallback sur le
      // champ global (max des 2) pour compat avec un firmware plus ancien.
      const stabS = data[`${product}_stab_remaining_s`] ?? (data.stabilization_remaining_s || 0);
      if (stabS > 0) {
        return `Stabilisation post-calibration en cours — réessayer dans ${fmtInjectRemaining(stabS)}`;
      }
      // feature-056 : garde présence d'eau = source UNIQUE (data.water_present).
      // Fallback filtration_running si champ absent (firmware plus ancien).
      const running = filtrationRunningOverride !== null
        ? filtrationRunningOverride
        : (data.filtration_running === true);
      const waterPresent = (data.water_present !== undefined) ? data.water_present : running;
      if (waterPresent === false) {
        return data.filtration_state_stale
          ? "Signal filtration périmé — injection impossible (sécurité chimique)"
          : "Filtration arrêtée — injection impossible (sécurité chimique)";
      }
      return "";
    };

    // ======================================================================
    // v2.25.x — Butée pH BRUTE et injection MANUELLE
    // ======================================================================
    // La butée de sécurité porte sur le pH BRUT (mesure capteur, offset NON
    // appliqué) : c'est le seul signal qui reste fiable quand l'offset est faux,
    // puisqu'un offset erroné rend le pH affiché rassurant. Le chemin
    // d'injection MANUELLE ignore délibérément cette butée (échappatoire
    // opérateur, décision produit) : le firmware obéit. L'UI n'a donc RIEN à
    // bloquer — elle doit seulement dire, AVANT le clic, que la sécurité est
    // active, pourquoi, et que l'injection passera outre.
    //
    // Les seuils (6,20 / 8,10) ne sont JAMAIS reconstruits ici : la v2.25.0 a
    // supprimé tous les miroirs de constantes firmware. On consomme les booléens
    // (« la butée mord ») et la chaîne de cause, servis à l'identique par /data
    // et par le WebSocket.

    // true = la butée brute mord pour ce produit, false = elle ne mord pas,
    // null = INFORMATION ABSENTE (firmware antérieur à la parité /data ↔ WS).
    // L'absence n'est PAS convertie en false : elle a son propre état, pour ne
    // jamais laisser croire à un « pas de blocage » qui n'a pas été mesuré.
    const rawGuardBites = (data, product) => {
      if (!data) return null;
      const v = data[product === "ph" ? "phRawGuardBlocked" : "orpPhRawGuardBlocked"];
      if (v === true) return true;
      if (v === false) return false;
      return null;
    };

    // Nom du produit réellement injecté (le libellé pH dépend du type de
    // correction configuré : acide pH− ou base pH+).
    const injectProductLabel = (product) => {
      if (product === "orp") return "du chlore";
      return (window._config?.ph_correction_type === "ph_plus")
        ? "de la base (pH+)" : "de l’acide (pH−)";
    };

    // Cause exacte telle que rapportée par l'appareil. Seule normalisation :
    // la virgule décimale française (le firmware sérialise avec un point).
    // Aucun seuil recalculé, aucune reformulation du fond.
    const rawGuardCause = (data, product) => {
      // v2.25.1 : la cause passe par la table code → libellé (code 250 = garde de
      // sécurité → le texte firmware, déjà rédigé en clair, est cité tel quel).
      const r = _doseBlockInfo(data, product).label;
      return r ? String(r).replace(/(\d)\.(\d)/g, "$1,$2") : "";
    };

    // Avertissement in situ, au-dessus du bouton Injecter. Rendu depuis
    // _updateInjectButtons, donc alimenté par l'UNIQUE chemin loadSensorData
    // (charge /data comme trame WebSocket) : les deux sources servant ces
    // champs, l'encart ne clignote pas d'un rafraîchissement à l'autre.
    const renderInjectRawGuardWarning = (data) => {
      ["ph", "orp"].forEach(product => {
        const box = $(`#${product}_inject_raw_guard_warning`);
        if (!box) return;
        const titleEl = $(`#${product}_inject_raw_guard_title`);
        const textEl = $(`#${product}_inject_raw_guard_text`);
        // false ET null → aucun avertissement. Un encart permanent (affiché
        // « par précaution » quand l'info manque) serait ignoré et détruirait
        // la valeur de signal de l'encart réel.
        if (rawGuardBites(data, product) !== true) {
          box.hidden = true;
          if (titleEl) titleEl.textContent = "";
          if (textEl) textEl.textContent = "";
          return;
        }
        const cause = rawGuardCause(data, product);
        if (titleEl) {
          titleEl.textContent = "Sécurité pH brut active — l’injection manuelle passera outre. ";
        }
        if (textEl) {
          textEl.textContent =
            `La régulation automatique refuse d’injecter ${injectProductLabel(product)}.`
            + (cause ? ` Cause signalée par l’appareil : « ${cause} ».` : "")
            + ` Cette butée porte sur le pH brut — la mesure du capteur, sans l’offset :`
            + ` c’est le seul repère fiable si l’offset pH est mal réglé, car le pH affiché`
            + ` peut alors sembler normal alors que la mesure ne l’est pas.`
            + ` L’eau n’est donc pas forcément à ce pH.`
            + ` Lancer une injection manuelle contournera cette sécurité et injectera quand même.`
            + ` Avant de continuer, contrôlez le pH au réactif (trousse DPD ou photomètre)`
            + ` et vérifiez l’offset pH${product === "ph" ? " (carte « Correction de la mesure pH »)"
                                                          : " sur la page pH"}.`;
        }
        box.hidden = false;
      });
    };

    // Appelé à chaque push sensor_data pour mettre à jour les boutons
    window._updateInjectButtons = (data) => {
      // Avertissement butée brute : rendu AVANT et INDÉPENDAMMENT de l'état du
      // bouton — il reste visible pendant une injection en cours (la sécurité
      // mord toujours) et n'entre jamais en concurrence avec #*_inject_block_hint,
      // qui porte les blocages réels (limite journalière, filtration…).
      renderInjectRawGuardWarning(data);
      // bug-dosage-fantome it.3 : harmonisation dans le sens RESTRICTIF — le
      // verrou du bouton Calibrer inclut le dosage AUTO (ph_dosing) comme
      // l'ORP le faisait déjà. Verrou fonctionnel voulu (pas de calibration
      // pendant une injection), pas de la conflation d'affichage.
      const phInjecting = (data["ph_inject_remaining_s"] ?? 0) > 0 || (data["ph_dosing"] === true);
      const orpInjecting = (data["orp_inject_remaining_s"] ?? 0) > 0 || (data["orp_dosing"] === true);
      const phCalBtn = $("#ph_cal_trigger_btn");
      const orpCalBtn = $("#orp_cal_trigger_btn");

      ["ph", "orp"].forEach(product => {
        const remaining = data[`${product}_inject_remaining_s`] ?? 0;
        const btn = $(`#${product}_inject_btn`);
        const mlInput = $(`#${product}_inject_ml`);
        const blockHint = $(`#${product}_inject_block_hint`);
        if (!btn) return;

        if (remaining > 0) {
          btn.textContent = `⏹ Arrêter — ${fmtInjectRemaining(remaining)}`;
          // Le stop est un coupe-circuit opérateur : JAMAIS désactivé
          btn.disabled = false;
          btn.title = "";
          if (blockHint) blockHint.textContent = "";
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
          // feature-006 : blocage proactif quand l'état connu l'exige (limite
          // journalière, stabilisation, filtration arrêtée hors mode continu)
          const blocked = getInjectBlockReason(data, product);
          btn.disabled = !!blocked;
          btn.title = blocked;
          if (blockHint) blockHint.textContent = blocked;
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

      // v2.25.x — Confirmation quand la butée pH BRUTE mord pour CE produit.
      // Placée APRÈS la branche d'arrêt : le stop est un coupe-circuit opérateur,
      // il n'est jamais conditionné. Placée AVANT le retour visuel optimiste,
      // pour ne pas laisser le bouton sur « Démarrage… » pendant la modale.
      //
      // Pourquoi une modale et pas un second clic sur le bouton : ce bouton est
      // à double fonction (Injecter / Arrêter). Y greffer un état transitoire
      // « confirmer ? » rendrait sa signification dépendante d'un minutage
      // invisible et exposerait le coupe-circuit à un clic malheureux. La modale
      // est de plus le motif déjà utilisé pour les actions à conséquence, elle
      // focalise « Annuler » par défaut, et elle peut restituer la cause serveur
      // complète et le réflexe attendu — ce qu'un libellé de bouton ne peut pas.
      // Elle n'est PAS une barrière : annulable, et affichée uniquement quand la
      // butée mord réellement (jamais quand l'information est absente).
      if (rawGuardBites(latestSensorData, product) === true) {
        const cause = rawGuardCause(latestSensorData, product);
        const ok = await confirmDialog({
          title: "Sécurité pH brut active",
          message:
            `La régulation automatique refuse d’injecter ${injectProductLabel(product)}.`
            + (cause ? `\n\nCause signalée par l’appareil : ${cause}.` : "")
            + `\n\nCette butée porte sur le pH brut (mesure du capteur, sans l’offset).`
            + ` Un offset pH mal réglé en est la cause la plus probable : l’eau n’est pas`
            + ` forcément à ce pH. Le bon réflexe est de contrôler le pH au réactif`
            + ` (trousse DPD ou photomètre) avant d’injecter.`
            + `\n\nEn continuant, l’injection manuelle contournera cette sécurité :`
            + ` le produit sera injecté malgré le refus de l’automatisme.`,
          confirmLabel: "Injecter quand même",
          cancelLabel: "Annuler",
          danger: true
        });
        if (!ok) return;
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

        // feature-006 : le firmware renvoie un 409 JSON structuré
        // {"error":"<code>","message":"<fr>","seconds_remaining"?,"remaining_ml"?}
        // Fallback texte si la réponse n'est pas du JSON (firmware ancien / proxy).
        let errorMsg = "Erreur lors du démarrage de l'injection";
        if (resp) {
          const body = await resp.text().catch(() => "");
          let err = null;
          try { err = JSON.parse(body); } catch (_) { /* réponse non JSON */ }
          if (err && (err.error || err.message)) {
            errorMsg = INJECT_REFUSAL_MESSAGES[err.error] || err.message || errorMsg;
            if (err.error === "stabilization_in_progress" && err.seconds_remaining > 0) {
              errorMsg += ` Réessayer dans ${fmtInjectRemaining(err.seconds_remaining)}.`;
            }
            if (err.error === "daily_limit" && err.remaining_ml != null) {
              errorMsg += ` Reste disponible aujourd'hui : ${Math.round(err.remaining_ml)} mL.`;
            }
          } else if (resp.status === 409) {
            // Ancien format text/plain : seule cause 409 possible = filtration
            errorMsg = "Injection refusée : la filtration doit être active avant d'injecter (sécurité chimique : pas de circulation = surdosage local).";
          } else if (body && body.length < 200) {
            errorMsg = body;
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

  // ==========================================================================
  // feature-075 : assistant de mesure du débit des pompes doseuses
  // ==========================================================================
  // Carte UNIQUE #pump-flow-wizard paramétrée par pompe, plein écran (classe
  // is-flow-wizard sur #view-settings, pattern calibration pH). Les marches de
  // pompe (purge/mesure) sont MINUTÉES SERVEUR (/flow-assist/start) : le compte
  // à rebours ci-dessous est purement LOCAL, resynchronisé par UN GET /data à
  // l'échéance (aucun nouveau rythme réseau). Perte réseau : la pompe s'arrête
  // seule à la fin du temps (minuterie serveur) — l'UI le signale, le bouton
  // Arrêter reste actif. beforeunload SEULEMENT pendant une marche.
  function bindFlowWizard() {
    const card = $("#pump-flow-wizard");
    if (!card) return;

    const wiz = {
      pump: 0,            // 1|2
      step: 0,
      running: null,      // null | "purge" | "measure"
      sessionActive: false,  // une session serveur a été ouverte (1er start réussi)
      endAt: 0,           // échéance locale (Date.now)
      timer: null,
      purgesLeft: 3,
      purgeS: 60,         // durées SERVIES (flow_assist.purge_s / measure_s)
      measureS: 120,
      measureDurationS: 120,  // durée réelle de la mesure achevée (conversion)
    };

    const steps = Array.from(card.querySelectorAll("#flow-wizard-steps > .step"));
    const cancelBtn = $("#flow-wizard-cancel-btn");
    const offlineNote = $("#flow-wizard-offline");
    const expiredNote = $("#flow-wizard-expired");
    const purgeBtn = $("#flow-wizard-purge-btn");
    const purgeStopBtn = $("#flow-wizard-purge-stop-btn");
    const purgeReadout = $("#flow-wizard-purge-readout");
    const purgeCountdown = $("#flow-wizard-purge-countdown");
    const repurgeBtn = $("#flow-wizard-repurge-btn");
    const next1Btn = $("#flow-wizard-next-1");
    const measureBtn = $("#flow-wizard-measure-btn");
    const measureStopBtn = $("#flow-wizard-measure-stop-btn");
    const measureReadout = $("#flow-wizard-measure-readout");
    const measureCountdown = $("#flow-wizard-measure-countdown");
    const measureAborted = $("#flow-wizard-measure-aborted");
    const volumeInput = $("#flow-wizard-volume");
    const conversionEl = $("#flow-wizard-conversion");
    const plausibilityEl = $("#flow-wizard-plausibility");
    const saveBtn = $("#flow-wizard-save-btn");

    const fmtMSS = (s) => `${Math.floor(s / 60)}:${String(s % 60).padStart(2, "0")}`;

    // Messages français des refus 409 des routes /flow-assist/* (les codes des
    // gardes d'injection manuelle sont partagés avec /ph|orp/inject/start).
    const FLOW_ASSIST_REFUSALS = {
      assist_busy: "Un assistant de mesure est déjà en cours sur l'autre pompe.",
      purge_limit: "Quota de purges atteint (3 par session) — passez à la suite ou quittez l'assistant.",
      already_injecting: "Une marche de pompe est déjà en cours — l'arrêter d'abord.",
      invalid_amplitude: "Amplitude d'exploitation nulle — mesure impossible.",
      filtration_off: "Marche refusée : la filtration doit être active (sécurité chimique).",
      watchdog_inactive: "Marche refusée : watchdog inactif (sécurité).",
      stabilization_in_progress: "Marche refusée : stabilisation post-calibration en cours.",
      daily_limit: "Marche refusée : limite journalière de dosage atteinte (la mesure compte dans le budget de sécurité).",
      hourly_limit: "Marche refusée : limite horaire d'injection atteinte — réessayer plus tard.",
      max_cycles: "Marche refusée : nombre maximal de démarrages de pompe atteint pour aujourd'hui.",
      burst_limit: "Marche refusée : trop de démarrages rapprochés (anti-rafale) — patienter.",
      maintenance_mode: "Marche refusée : mode maintenance actif — dosage suspendu.",
    };

    const beforeUnload = (e) => { e.preventDefault(); e.returnValue = ""; };

    function setEntryButtons(disabled) {
      [1, 2].forEach((n) => {
        const b = $(`#pump${n}_measure_btn`);
        if (b) b.disabled = disabled;
      });
    }

    function gotoStep(i) {
      wiz.step = i;
      steps.forEach((el, idx) => {
        el.classList.remove("is-active", "is-completed", "is-upcoming");
        if (idx < i) el.classList.add("is-completed");
        else if (idx === i) el.classList.add("is-active");
        else el.classList.add("is-upcoming");
      });
      // Annuler visible aux étapes 1-5 ; à l'étape finale, « Terminer » quitte.
      if (cancelBtn) cancelBtn.hidden = (i === 5);
      steps[i]?.scrollIntoView?.({ behavior: "smooth", block: "nearest" });
    }

    function updateRepurgeUi() {
      if (repurgeBtn) {
        repurgeBtn.hidden = !(wiz.purgesLeft > 0);
        repurgeBtn.textContent = `Purger à nouveau (${wiz.purgesLeft} restante${wiz.purgesLeft > 1 ? "s" : ""})`;
      }
    }

    // ---------- Compte à rebours local (resynchro /data à l'échéance) ----------
    function stopTimer() {
      if (wiz.timer) { clearInterval(wiz.timer); wiz.timer = null; }
    }

    function tick() {
      const remain = Math.max(0, Math.ceil((wiz.endAt - Date.now()) / 1000));
      const out = wiz.running === "purge" ? purgeCountdown : measureCountdown;
      if (out) out.textContent = fmtMSS(remain);
      // Perte réseau (bannière app--offline posée par setNetStatus) : signaler
      // que la minuterie SERVEUR arrêtera la pompe seule. Arrêter reste actif.
      if (offlineNote) {
        offlineNote.hidden = !document.querySelector(".app")?.classList.contains("app--offline");
      }
      if (remain <= 0) {
        stopTimer();
        verifyRunEnd(wiz.running);
      }
    }

    function startTimer(durS) {
      wiz.endAt = Date.now() + durS * 1000;
      tick();
      wiz.timer = setInterval(tick, 500);
    }

    // Échéance locale atteinte : VÉRIFICATION d'état serveur (un seul GET /data).
    // Si le serveur annonce encore du temps (horloge client en avance), on se
    // recale ; hors ligne, on considère la marche finie (le serveur a arrêté).
    // Revue feature-075 : consomme flow_assist.aborted — une marche interrompue
    // par le SERVEUR (perte de filtration) ne doit JAMAIS être présentée comme
    // réussie : une mesure partielle mènerait à un débit SOUS-estimé enregistré
    // → sur-injection systématique.
    async function verifyRunEnd(phase) {
      let fa = null;
      try {
        const d = await loadSensorData({ force: true, source: "flow-wizard" });
        fa = d && d.flow_assist;
      } catch (e) { /* hors ligne : la minuterie serveur a fait foi */ }
      if (fa && fa.active && fa.phase === phase && fa.remaining_s > 0) {
        startTimer(fa.remaining_s);
        return;
      }
      const aborted = !!(fa && fa.aborted);
      onRunEnded(phase, !aborted, aborted);
    }

    // `completed` : la marche est allée au bout ; `aborted` : interrompue par le
    // SERVEUR (perte de filtration) — distinct de l'arrêt utilisateur (stopRun).
    function onRunEnded(phase, completed, aborted = false) {
      wiz.running = null;
      stopTimer();
      window.removeEventListener("beforeunload", beforeUnload);
      if (offlineNote) offlineNote.hidden = true;
      if (phase === "purge") {
        if (purgeReadout) purgeReadout.hidden = true;
        if (purgeStopBtn) purgeStopBtn.hidden = true;
        if (aborted) {
          // Purge avortée par le serveur : signalement + retour au bouton Purger.
          showToast("Purge interrompue par le contrôleur (filtration arrêtée) — relancez la purge quand l'eau circule.", "error");
          if (purgeBtn) {
            purgeBtn.hidden = false;
            purgeBtn.textContent = `Purger (${wiz.purgeS} s)`;
          }
          if (next1Btn) next1Btn.hidden = true;
          if (repurgeBtn) repurgeBtn.hidden = true;
          return;
        }
        if (purgeBtn) purgeBtn.hidden = true;
        if (next1Btn) next1Btn.hidden = false;
        updateRepurgeUi();
      } else if (phase === "measure") {
        if (measureReadout) measureReadout.hidden = true;
        if (measureStopBtn) measureStopBtn.hidden = true;
        if (completed && !aborted) {
          if (measureAborted) measureAborted.hidden = true;
          // wiz.measureDurationS = durée SERVIE au démarrage (réponse start)
          updateConversion();
          gotoStep(4);
          setTimeout(() => volumeInput?.focus?.(), 300);
        } else {
          // Arrêt anticipé (utilisateur OU serveur) : valeur non fiable — relancer.
          if (aborted) {
            showToast("Mesure interrompue par le contrôleur (filtration arrêtée).", "error");
          }
          if (measureAborted) measureAborted.hidden = false;
          if (measureBtn) {
            measureBtn.hidden = false;
            measureBtn.textContent = `Relancer la mesure (${wiz.measureS} s)`;
          }
        }
      }
    }

    // ---------- Marches minutées serveur ----------
    async function startPhase(phase) {
      const btn = phase === "purge" ? purgeBtn : measureBtn;
      if (btn) btn.disabled = true;
      let resp = null;
      try {
        resp = await authFetch("/flow-assist/start", {
          method: "POST",
          headers: { "Content-Type": "application/x-www-form-urlencoded" },
          body: `pump=${wiz.pump}&phase=${phase}`,
        });
      } catch (e) { /* réseau : géré ci-dessous */ }
      if (btn) btn.disabled = false;
      if (!resp || !resp.ok) {
        let msg = "Contrôleur injoignable — la pompe n'a pas démarré.";
        if (resp) {
          const err = await resp.json().catch(() => null);
          msg = (err && (FLOW_ASSIST_REFUSALS[err.error] || err.message)) ||
                `Démarrage refusé (HTTP ${resp.status}).`;
        }
        showToast(msg, "error");
        return;
      }
      const j = await resp.json().catch(() => ({}));
      const durS = (typeof j.duration_s === "number" && j.duration_s > 0)
        ? j.duration_s
        : (phase === "purge" ? wiz.purgeS : wiz.measureS);
      if (typeof j.purges_left === "number") wiz.purgesLeft = j.purges_left;
      wiz.running = phase;
      wiz.sessionActive = true;  // session serveur ouverte (suivi d'expiration)
      if (expiredNote) expiredNote.hidden = true;
      window.addEventListener("beforeunload", beforeUnload);
      if (phase === "purge") {
        if (purgeBtn) purgeBtn.hidden = true;
        if (repurgeBtn) repurgeBtn.hidden = true;
        if (next1Btn) next1Btn.hidden = true;
        if (purgeReadout) purgeReadout.hidden = false;
        if (purgeStopBtn) purgeStopBtn.hidden = false;
      } else {
        wiz.measureDurationS = durS;
        if (measureBtn) measureBtn.hidden = true;
        if (measureAborted) measureAborted.hidden = true;
        if (measureReadout) measureReadout.hidden = false;
        if (measureStopBtn) measureStopBtn.hidden = false;
      }
      startTimer(durS);
    }

    // Coupe-circuit opérateur : JAMAIS désactivé. Même hors ligne on tente la
    // requête ; en cas d'échec la minuterie serveur arrête de toute façon.
    async function stopRun() {
      const phase = wiz.running;
      if (!phase) return;
      stopTimer();
      try { await authFetch("/flow-assist/stop", { method: "POST" }); } catch (e) { /* minuterie serveur */ }
      onRunEnded(phase, false);
    }

    // ---------- Conversion volume → débit (étape 5) ----------
    function readVolume() {
      const raw = (volumeInput?.value || "").trim().replace(",", ".");
      const v = parseFloat(raw);
      return (raw !== "" && !isNaN(v) && v > 0) ? v : null;
    }

    function updateConversion() {
      const vol = readVolume();
      if (!conversionEl) return;
      if (vol == null) {
        conversionEl.textContent = "";
        if (plausibilityEl) plausibilityEl.hidden = true;
        return;
      }
      const flow = vol / (wiz.measureDurationS / 60);
      conversionEl.textContent =
        `${vol} ml en ${fmtMSS(wiz.measureDurationS)} → ≈ ${flow.toFixed(1)} ml/min`;
      // Vraisemblance NON bloquante (attendu ~20-35 ml/min pour ces pompes).
      if (plausibilityEl) plausibilityEl.hidden = (flow >= 20 && flow <= 35);
    }

    async function saveMeasuredFlow() {
      // Filet (re-revue) : si la mesure a été interrompue par le SERVEUR alors
      // que le client était hors ligne à l'échéance (verifyRunEnd passé par le
      // catch sans connaître `aborted`), refuser la saisie — un débit partiel
      // enregistré serait SOUS-estimé → sur-injection.
      if (latestSensorData?.flow_assist?.aborted === true) {
        showToast("Mesure interrompue par le contrôleur — la valeur ne serait pas fiable. Relancez la mesure.", "error");
        if (measureAborted) measureAborted.hidden = false;
        if (measureBtn) {
          measureBtn.hidden = false;
          measureBtn.textContent = `Relancer la mesure (${wiz.measureS} s)`;
        }
        gotoStep(3);
        return;
      }
      const vol = readVolume();
      if (vol == null) {
        showToast("Volume invalide : saisissez le volume lu sur le récipient (ml).", "error");
        return;
      }
      const flow = Math.round((vol / (wiz.measureDurationS / 60)) * 10) / 10;
      if (flow <= 0 || flow > 200) {
        showToast("Débit hors bornes (au plus 200 ml/min) — vérifiez la lecture.", "error");
        return;
      }
      if (saveBtn) saveBtn.disabled = true;
      // Chemin /save-config de la feature-074 : le serveur déduit le duty
      // calibré de l'amplitude courante et pose le drapeau « mesuré ».
      const payload = {};
      payload[`pump${wiz.pump}_pulse_flow_ml_per_min`] = flow;
      const r = await sendConfigDetailed(payload);
      if (saveBtn) saveBtn.disabled = false;
      if (!r.ok) {
        showToast(r.error || "Erreur lors de l'enregistrement du débit.", "error");
        return;
      }
      const doneText = $("#flow-wizard-done-text");
      if (doneText) {
        doneText.textContent =
          `Débit enregistré : ${flow.toFixed(1)} ml/min (mesuré). Rebranchez le tuyau d'injection sur le point d'injection.`;
      }
      gotoStep(5);
      setTimeout(loadConfig, 500);
      setTimeout(() => loadSensorData({ force: true, source: "flow-wizard-save" }), 800);
    }

    // ---------- Ouverture / fermeture ----------
    // Réinitialisation des états d'étapes (libellés paramétrés sur les durées
    // servies) — factorisée : ouverture ET reprise après expiration de session.
    function resetStepsUi() {
      if (purgeBtn) { purgeBtn.hidden = false; purgeBtn.disabled = false; purgeBtn.textContent = `Purger (${wiz.purgeS} s)`; }
      if (purgeStopBtn) purgeStopBtn.hidden = true;
      if (purgeReadout) purgeReadout.hidden = true;
      if (repurgeBtn) repurgeBtn.hidden = true;
      if (next1Btn) next1Btn.hidden = true;
      if (measureBtn) { measureBtn.hidden = false; measureBtn.disabled = false; measureBtn.textContent = `Lancer la mesure (${wiz.measureS} s)`; }
      if (measureStopBtn) measureStopBtn.hidden = true;
      if (measureReadout) measureReadout.hidden = true;
      if (measureAborted) measureAborted.hidden = true;
      if (volumeInput) volumeInput.value = "";
      if (conversionEl) conversionEl.textContent = "";
      if (plausibilityEl) plausibilityEl.hidden = true;
      if (offlineNote) offlineNote.hidden = true;
    }

    function openFlowWizard(n) {
      const cfg = window._config || {};
      const phPump = cfg.ph_pump === 2 ? 2 : 1;
      wiz.pump = n;
      // Durées servies par /data si déjà connues (sinon miroirs par défaut)
      const fa = latestSensorData?.flow_assist;
      if (fa) {
        if (fa.purge_s > 0) wiz.purgeS = fa.purge_s;
        if (fa.measure_s > 0) wiz.measureS = fa.measure_s;
      }
      wiz.purgesLeft = (fa && fa.active && fa.pump === n && typeof fa.purges_left === "number")
        ? fa.purges_left : 3;
      wiz.measureDurationS = wiz.measureS;
      wiz.running = null;
      wiz.sessionActive = false;

      const title = $("#flow-wizard-title");
      if (title) title.textContent = `Mesure du débit — Pompe ${n} (${phPump === n ? "pH" : "Chlore"})`;
      const budget = $("#flow-wizard-budget");
      if (budget) {
        // Débit majorant SERVI par /data (flow_assist.flow255[n-1]) — plus de
        // miroir en dur (revue).
        // Repli PAR POMPE et non générique (revue v2.25.2) : un repli à 61 pour
        // les deux sous-annonçait le budget de 10 % sur la pompe 2, dont les
        // firmwares ANTÉRIEURS à la fusion de flotte dosent bien à 67.
        // Logique dupliquée volontairement plutôt qu'appelée via flow255For() :
        // celle-ci est déclarée dans un autre bloc (const = portée de bloc).
        const servi = fa && Array.isArray(fa.flow255) ? fa.flow255[n - 1] : undefined;
        const f = (typeof servi === "number" && isFinite(servi) && servi > 0)
          ? servi : (n === 2 ? 67.0 : 61.0);
        budget.textContent =
          `≈ ${Math.ceil(f * wiz.purgeS / 60)} ml par purge, ≈ ${Math.ceil(f * wiz.measureS / 60)} ml pour la mesure`;
      }
      resetStepsUi();
      if (expiredNote) expiredNote.hidden = true;

      gotoStep(0);
      card.hidden = false;
      $("#view-settings")?.classList.add("is-flow-wizard");
      setEntryButtons(true);  // une pompe à la fois : entrées neutralisées
      card.scrollIntoView?.({ behavior: "smooth", block: "start" });
    }

    function closeFlowWizard() {
      stopTimer();
      wiz.running = null;
      wiz.sessionActive = false;
      window.removeEventListener("beforeunload", beforeUnload);
      card.hidden = true;
      $("#view-settings")?.classList.remove("is-flow-wizard");
      setEntryButtons(false);
    }

    // Revue feature-075 : détection de l'EXPIRATION de session côté serveur
    // (timeout 15 min) pendant que l'assistant est ouvert. Appelé par
    // loadSensorData sur chaque /data reçu (aucun nouveau rythme réseau).
    // Hors marche (wiz.running) : l'échéance est gérée par verifyRunEnd.
    // À l'étape finale (5), le débit est déjà enregistré — l'expiration ne
    // change rien (« Terminer » reste un exit idempotent).
    window._flowWizardOnData = (json) => {
      const fa = json && json.flow_assist;
      if (!fa || card.hidden || wiz.running || !wiz.sessionActive) return;
      if (fa.active === false && wiz.step < 5) {
        wiz.sessionActive = false;
        resetStepsUi();
        gotoStep(0);
        if (expiredNote) expiredNote.hidden = false;
      }
    };

    // Sortie : POST /flow-assist/exit (arrêt pompe + rétablissement du dosage
    // auto côté serveur). Sans confirmation — l'action est réversible et le
    // timeout serveur couvre de toute façon l'abandon.
    async function exitWizard(toastMsg, toastType) {
      try { await authFetch("/flow-assist/exit", { method: "POST" }); } catch (e) { /* timeout serveur en filet */ }
      closeFlowWizard();
      if (toastMsg) showToast(toastMsg, toastType);
    }

    // ---------- Liaisons ----------
    [1, 2].forEach((n) => {
      $(`#pump${n}_measure_btn`)?.addEventListener("click", () => openFlowWizard(n));
      // « Saisie avancée » : dépliant discret du champ direct de la feature-074
      const toggle = $(`#pump${n}_adv_toggle`);
      const panel = $(`#pump${n}_adv`);
      toggle?.addEventListener("click", () => {
        const open = toggle.getAttribute("aria-expanded") === "true";
        toggle.setAttribute("aria-expanded", String(!open));
        if (panel) panel.hidden = open;
      });
    });

    $("#flow-wizard-next-0")?.addEventListener("click", () => gotoStep(1));
    next1Btn?.addEventListener("click", () => gotoStep(2));
    $("#flow-wizard-next-2")?.addEventListener("click", () => gotoStep(3));
    purgeBtn?.addEventListener("click", () => startPhase("purge"));
    repurgeBtn?.addEventListener("click", () => startPhase("purge"));
    purgeStopBtn?.addEventListener("click", stopRun);
    measureBtn?.addEventListener("click", () => startPhase("measure"));
    measureStopBtn?.addEventListener("click", stopRun);
    volumeInput?.addEventListener("input", updateConversion);
    saveBtn?.addEventListener("click", saveMeasuredFlow);
    cancelBtn?.addEventListener("click", () =>
      exitWizard("Mesure annulée — dosage automatique rétabli.", "info"));
    $("#flow-wizard-finish-btn")?.addEventListener("click", () =>
      exitWizard("Mesure du débit terminée — dosage automatique rétabli.", "success"));
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
      // v2.25.0 : plus de garde-fou de saisie sur la consigne pH — le contrôle
      // segmenté ne peut produire qu'une valeur de la plage servie par le
      // serveur. La validation serveur reste, elle, en place (défense en
      // profondeur pour l'API et MQTT).
      setBtn("Sauvegarde...", true, true, "default");
      // sendConfigDetailed : un refus serveur porte un message explicite, il ne
      // doit pas se réduire à un bouton « Erreur » muet.
      const r = await sendConfigDetailed(collectConfig());
      if (r.ok) {
        setBtn("Sauvegarde réussie", false, true, "success");
        if (sensor === "ph") renderPhTargetHint();  // efface un éventuel échec précédent
        setTimeout(() => setBtn(defaultLabel, false, true, "default"), 2000);
      } else {
        const msg = r.message || r.error || "Enregistrement refusé.";
        showToast(msg, "error");
        // B5 : le hint rouge ne signale plus qu'un échec de SAUVEGARDE.
        if (sensor === "ph") {
          showPhTargetSaveError(msg + " La consigne affichée n'a pas été enregistrée.");
        }
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

    // feature-056 : le toggle « Gérer la filtration » est remplacé par le mode
    // d'installation (panel Installation). La visibilité filtration est pilotée
    // par applyInstallMode().
    bindInstallMode();
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
      lightingDirty = true;  // bug-ui-eclairage-refresh-ws : ne pas écraser une édition en cours
      const scheduleSettings = $("#lighting-schedule-settings");
      if (scheduleSettings) {
        scheduleSettings.style.display = $("#lighting_schedule_mode").value === "enabled" ? "" : "none";
      }
    });
    $("#lighting_start_time")?.addEventListener("change", () => { lightingDirty = true; });
    $("#lighting_end_time")?.addEventListener("change", () => { lightingDirty = true; });
    bindLightingManualSave();

    // pH / ORP regulation — sauvegarde immédiate sur le sélecteur de mode
    initSegmented("ph_regulation_mode");
    $$(`#ph_regulation_mode .segmented__btn`).forEach(btn => {
      btn.addEventListener("click", async () => {
        // v2.25.0 (B4) : le serveur peut REFUSER (409) — bascule de sécurité pH
        // non acquittée. Sans revert, l'UI resterait bloquée sur un mode que
        // l'appareil n'a jamais adopté, et l'utilisateur n'aurait aucune issue.
        const previousMode = window._config?.ph_regulation_mode || "manual";
        const mode = btn.dataset.value;
        updatePhModeControls();
        if (window._config) { window._config.ph_regulation_mode = mode; window._config.ph_enabled = (mode !== "manual"); }
        const r = await sendConfigDetailed({ ph_regulation_mode: mode });
        if (r.ok) { loadConfig(); return; }
        // Revert de l'override optimiste (config + segmented) sur l'état serveur.
        if (window._config) {
          window._config.ph_regulation_mode = previousMode;
          window._config.ph_enabled = (previousMode !== "manual");
        }
        setSegmented("ph_regulation_mode", previousMode);
        updatePhModeControls();
        if (r.status === 409) {
          // On NE propose PAS d'acquitter depuis ce message : l'acquittement doit
          // rester un acte lu et réfléchi (bouton de la bannière, qui affiche la
          // cause), pas un réflexe pour débloquer un clic refusé.
          showToast(r.message || r.error, "error");
          const banner = $("#ph-auto-manual-banner");
          if (banner && !banner.hidden && typeof banner.scrollIntoView === "function") {
            banner.scrollIntoView({ behavior: "smooth", block: "center" });
          }
        } else {
          showToast(r.error || "Changement de mode refusé.", "error");
        }
        // Resynchronisation sur l'état RÉEL du contrôleur (et bannière à jour).
        loadConfig();
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
    bindPhOffsetSave();  // feature-067 : carte « Correction de la mesure pH »
    bindPhTargetControl();  // v2.25.0 : contrôle segmenté de la consigne pH
    // La consigne pH entre dans le calcul de l'avertissement d'offset → resynchroniser.
    $("#ph_target")?.addEventListener("input", renderPhOffsetWarning);
    // v2.25.0 : rappel de la plage sous le contrôle.
    $("#ph_target")?.addEventListener("input", renderPhTargetHint);
    // Onglet Régulation : initialisation segmented + save dédié
    initSegmented("regulation_speed");
    $("#regulation_save_btn")?.addEventListener("click", async () => {
      // feature-056 : regulation_mode retiré (remplacé par le mode d'installation).
      const delay = parseInt($("#stabilization_delay_min")?.value ?? "5", 10);
      const speed = getSegmented("regulation_speed") || "normal";
      const phLimit = parseInt($("#ph_limit")?.value ?? "5", 10);
      const phDaily = parseFloat($("#ph_daily_limit")?.value ?? "300");
      const orpLimit = parseInt($("#orp_limit")?.value ?? "10", 10);
      const orpDaily = parseFloat($("#orp_daily_limit")?.value ?? "500");
      const maxCycles = parseInt($("#max_cycles_per_day")?.value ?? "48", 10);
      const ok = await sendConfig({
        stabilization_delay_min: isNaN(delay) ? 5 : Math.min(60, Math.max(0, delay)),
        regulation_speed: speed,
        ph_limit_minutes:        isNaN(phLimit)  ? 5  : Math.min(60, Math.max(1, phLimit)),
        max_ph_ml_per_day:       isNaN(phDaily)  ? 300 : Math.max(0, phDaily),
        orp_limit_minutes:       isNaN(orpLimit) ? 10 : Math.min(60, Math.max(1, orpLimit)),
        max_chlorine_ml_per_day: isNaN(orpDaily) ? 500 : Math.max(0, orpDaily),
        // feature-060 : pré-clamp UI indicatif — le serveur reste l'autorité (clampCyclesPerDay)
        max_cycles_per_day:      isNaN(maxCycles) ? 48 : Math.min(60, Math.max(10, maxCycles)),
      });
      if (ok) { showToast("Enregistré", "success"); loadConfig(); }
      else showToast("Erreur lors de la sauvegarde", "error");
    });

    // Temperature feature
    $("#temperature_enabled")?.addEventListener("change", () => {
      updateFeatureVisibility("temperature");
      updateFiltrationControls();  // Mode auto dépend de la température
      save();
    });

    // Development panel - Sensor logs / Debug logs
    $("#sensor_logs_enabled")?.addEventListener("change", save);
    $("#debug_logs_enabled")?.addEventListener("change", save);
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
      if (!(await confirmDialog({
        title: "Effacer le coredump",
        message: "Effacer le coredump ?\n\nL'analyse du crash actuel sera définitivement perdue.",
        confirmLabel: "Effacer",
        danger: true
      }))) return;
      try {
        const resp = await authFetch("/coredump", { method: "DELETE" });
        if (!resp.ok) { showToast("Erreur lors de l'effacement", "error"); return; }
        showToast("Coredump effacé", "success");
        await loadCoredumpInfo();
      } catch (e) {
        showToast("Erreur lors de l'effacement", "error");
      }
    });

    // Logo topbar → blur immédiat pour éviter le focus ring persistant sur iOS Safari
    $("#topbar-home-link")?.addEventListener("click", (e) => {
      e.currentTarget.blur();
    });

    // Logout button
    $("#logout-btn")?.addEventListener("click", async (e) => {
      e.preventDefault();
      if (!(await confirmDialog({
        title: "Déconnexion",
        message: "Voulez-vous vraiment vous déconnecter ?",
        confirmLabel: "Se déconnecter"
      }))) return;
      // Clear session storage
      sessionStorage.removeItem('authToken');
      // Redirect to login page
      window.location.href = '/login.html';
    });

    // Reboot button
    $("#reboot_btn")?.addEventListener("click", async () => {
      if (!(await confirmDialog({
        title: "Redémarrer",
        message: "Voulez-vous vraiment redémarrer ?\n\nL'appareil sera indisponible pendant environ 10 secondes.",
        confirmLabel: "Redémarrer"
      }))) {
        return;
      }

      try {
        const res = await authFetch("/reboot", {
          method: "POST"
        });

        if (res.ok) {
          showToast("Redémarrage en cours — patientez environ 10 secondes puis actualisez la page.", "info");
          // Désactiver le bouton pour éviter les clics multiples
          $("#reboot_btn").disabled = true;
          $("#reboot_btn").textContent = "Redémarrage en cours...";
        } else {
          showToast("Erreur lors du redémarrage de l'ESP32", "error");
        }
      } catch (error) {
        showToast("Erreur de connexion : " + error.message, "error");
      }
    });

    // Factory reset button
    $("#factory_reset_btn")?.addEventListener("click", async () => {
      if (!(await confirmDialog({
        title: "Réinitialisation du système",
        message: "Cette action est irréversible.\n\nToutes les données seront supprimées :\n- Configuration (pH, ORP, MQTT, Wi-Fi…)\n- Calibrations\n- Historique des produits\n\nLe système redémarrera sur l'assistant de configuration.",
        confirmLabel: "Réinitialiser",
        danger: true
      }))) {
        return;
      }
      // Double confirmation
      if (!(await confirmDialog({
        title: "Dernière confirmation",
        message: "Voulez-vous vraiment effacer toutes les données ?",
        confirmLabel: "Tout effacer",
        danger: true
      }))) {
        return;
      }
      try {
        const btn = $("#factory_reset_btn");
        btn.disabled = true;
        btn.textContent = "Réinitialisation…";
        const res = await authFetch("/factory-reset", { method: "POST" });
        if (res.ok) {
          showToast("Réinitialisation effectuée — l'appareil redémarre. Connectez-vous au Wi-Fi de l'ESP32 pour l'assistant de configuration.", "success");
        } else {
          showToast("Erreur lors de la réinitialisation.", "error");
          btn.disabled = false;
          btn.textContent = "Réinitialiser";
        }
      } catch {
        showToast("Erreur de connexion.", "error");
        const btn = $("#factory_reset_btn");
        if (btn) { btn.disabled = false; btn.textContent = "Réinitialiser"; }
      }
    });
  }

  // ---------- Security bindings ----------
  function bindSecurity() {
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
        showToast("Veuillez saisir votre mot de passe actuel.", "error");
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
        showToast(`Le nouveau mot de passe doit contenir : ${missing.join(', ')}`, "error");
        return;
      }

      // Validation: passwords match
      if (newPassword !== confirmPassword) {
        showToast("Les nouveaux mots de passe ne correspondent pas.", "error");
        return;
      }

      // Validation: new password must be different
      if (currentPassword === newPassword) {
        showToast("Le nouveau mot de passe doit être différent du mot de passe actuel.", "error");
        return;
      }

      if (!(await confirmDialog({
        title: "Modifier le mot de passe",
        message: "Voulez-vous vraiment modifier le mot de passe administrateur ?",
        confirmLabel: "Modifier"
      }))) {
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
            showToast("L'ancien mot de passe est incorrect.", "error");
          } else {
            showToast("Erreur : " + (error.error || "Échec du changement de mot de passe"), "error");
          }
          return;
        }

        const data = await res.json();

        // Update the token in sessionStorage
        if (data.token) {
          sessionStorage.setItem('authToken', data.token);
        }

        showToast("Mot de passe modifié avec succès !", "success");
        // Clear all password fields and reset validation UI
        $("#auth_current_password").value = "";
        $("#auth_password").value = "";
        $("#auth_password_confirm").value = "";
        updatePasswordFeedback();
      } catch (error) {
        showToast("Erreur de connexion : " + error.message, "error");
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
        fill: false,
        referenceLines: {
          max: PH_MAX,
          min: PH_MIN,
          axisMin: PH_AXIS_MIN_DEFAULT,
          axisMax: PH_AXIS_MAX_DEFAULT,
          zoneColor: PH_ZONE_COLOR,
          lineColor: PH_LINE_COLOR
        }
      });
    }
    if (orpChartCanvas) {
      orpChart = createLineChart(orpChartCanvas, "#10b981", "ORP", {
        integerOnly: true,
        fill: false,
        referenceLines: {
          max: ORP_MAX,
          min: ORP_MIN,
          axisMin: ORP_AXIS_MIN_DEFAULT,
          axisMax: ORP_AXIS_MAX_DEFAULT,
          zoneColor: ORP_ZONE_COLOR,
          lineColor: ORP_LINE_COLOR
        }
      });
    }

    bindDetailCharts();

    bindUI();
    bindConfirmModal();
    bindHistoryBackup();
    bindDetailActions();
    bindAutosave();
    bindPhCalibration();
    bindPhProbeChip();
    bindPhAutoManualAck();  // v2.25.0 : acquittement de la bascule de sécurité pH
    bindFilterChip('ph');
    bindFilterChip('orp');
    bindOrpCalibration();
    bindTempCalibration();
    bindWifi();
    bindMqttManualSave();
    bindFiltrationManualSave();
    bindPumpConfigSave();
    bindFlowWizard();
    bindTimeManualSave();
    bindSecurity();
    bindGithubUpdate();
    bindManualUpdate();
    bindLogs();
    bindSondesOnewire();
    bindSensorTrace();

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

    // Chargement initial des mini charts (journée en cours, depuis minuit local)
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
        scheduleSettings.style.display = config.lighting_schedule_enabled ? "" : "none";
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
        const payload = { filtration_force_on: true, filtration_force_off: false };
        try {
          const result = await sendConfig(payload);
          if (result) {
            filtrationRunningOverride = true;
            updateFiltrationControls();
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
