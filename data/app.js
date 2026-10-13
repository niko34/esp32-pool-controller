(() => {
  "use strict";

  // ---------- Utils ----------
  const $ = (sel, root = document) => root.querySelector(sel);
  const $$ = (sel, root = document) => Array.from(root.querySelectorAll(sel));
  const clamp = (n, a, b) => Math.max(a, Math.min(b, n));

  // Segmented value controls (ex: regulation_speed)
  // feature-153 (B1) : aria-pressed tenu à jour pour TOUT contrôle segmenté
  // (pas seulement le nouveau sélecteur de cadence) — ajout rétrocompatible,
  // ne change aucun comportement visuel existant.
  function setSegmented(id, value) {
    const container = $(`#${id}`);
    if (!container) return;
    container.querySelectorAll(".segmented__btn").forEach(btn => {
      const active = btn.dataset.value === value;
      btn.classList.toggle("is-active", active);
      btn.setAttribute("aria-pressed", active ? "true" : "false");
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
      // feature-138 (B2) : #dose-block-banner péremption — la classe pose l'opacité
      // (app.css), et révèle un texte explicite (.dose-block-banner__stale-note,
      // caché par défaut) : l'opacité seule n'explique rien à un lecteur d'écran.
      //
      // feature-148 (B4, cas limite « WebSocket déconnecté ») — comportement VOULU,
      // écrit ici parce qu'il ne se lit nulle part ailleurs : quand la liaison tombe,
      // le badge de carte RESTE MUET sur une cause portée par le bandeau. Ce n'est
      // pas un oubli. Le bandeau n'efface rien — il garde sa ligne, l'estompe et
      // révèle sa note de péremption : la cause reste donc affichée AVEC sa réserve
      // de fraîcheur. La faire réapparaître dans le badge recréerait une seconde
      // voix QUI A L'AIR FRAÎCHE, le badge n'ayant, lui, aucune note de péremption.
      // ⚠️ Cet invariant repose sur un fait du flux, pas sur une garde : badge et
      // bandeau lisent la MÊME trame (`latestSensorData`) dans la MÊME passe
      // synchrone de loadSensorData(). Scinder ce rafraîchissement en deux cycles
      // casserait le silence sans que rien ne le signale.
      ['#ph-card-stats', '#orp-card-stats', '#dose-block-banner'].forEach(sel => {
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

  // feature-088 : options ajoutées — messageNode (contenu DOM construit via
  // createElement, JAMAIS innerHTML) pour les récapitulatifs riches (gras sur
  // les lignes qui changent) et confirmDisabled (bouton Confirmer inactif,
  // ex. « rien à restaurer »). `message` reste requis : c'est le fallback texte.
  function confirmDialog({ title, message, messageNode = null, confirmLabel = 'Confirmer', cancelLabel = 'Annuler', danger = false, confirmDisabled = false } = {}) {
    const modal = $("#confirm-modal");
    if (!modal || typeof modal.showModal !== 'function') {
      // Fallback navigateur sans <dialog> : on ne supprime JAMAIS une confirmation
      // de sécurité → window.confirm() natif.
      if (confirmDisabled) { window.alert(message); return Promise.resolve(false); }
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
      // white-space: pre-line (CSS .confirm-modal__message). messageNode :
      // arbre DOM déjà construit (textContent interne), substitué au texte.
      if (messageEl) {
        messageEl.textContent = message || '';
        if (messageNode) { messageEl.textContent = ''; messageEl.appendChild(messageNode); }
      }
      if (okBtn) {
        okBtn.textContent = confirmLabel;
        okBtn.disabled = !!confirmDisabled;
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

  // ---------- feature-088 : restauration des valeurs d'usine de régulation ----------
  // Les VALEURS D'USINE ne sont JAMAIS codées ici : elles viennent exclusivement
  // de l'objet regulation_defaults servi par /get-config (source unique
  // firmware). Ce module ne porte que les libellés/unités d'affichage.
  const kRegulationResetFields = [
    { key: "ph_target",               label: "Consigne pH",                          fmt: (v) => `${Math.round(Number(v) * 100) / 100} pH` },
    { key: "orp_target",              label: "Consigne ORP",                         fmt: (v) => `${Math.round(Number(v))} mV` },
    { key: "ph_limit_minutes",        label: "pH — durée max d'injection",           fmt: (v) => `${v} min/h` },
    { key: "orp_limit_minutes",       label: "ORP — durée max d'injection",          fmt: (v) => `${v} min/h` },
    // feature-091 : stabilization_delay_min retiré (purge adaptative, plancher/plafond
    // firmware) — regulation_defaults ne porte plus que 7 réglages.
    { key: "ph_daily_target_ml",     label: "Volume quotidien pH (mode programmé)", fmt: (v) => `${v} mL/j` },
    { key: "orp_daily_target_ml",     label: "Volume quotidien ORP (mode programmé)",fmt: (v) => `${v} mL/j` },
    { key: "regulation_speed",        label: "Vitesse de correction",                fmt: (v) => ({ slow: "Lente", normal: "Normale", fast: "Rapide" }[v] || String(v)) },
  ];

  // Cache des valeurs d'usine : la trame WS "config" remplace window._config
  // sans porter regulation_defaults (parité partielle) — le cache évite que le
  // bouton clignote et que le dialogue perde ses valeurs de référence.
  let _regulationDefaults = null;

  function _regValueChanged(key, cur, def) {
    // Valeur courante inconnue → considérée comme modifiée (la restauration l'écrira)
    if (cur == null || def == null) return def != null;
    if (key === "regulation_speed") return String(cur) !== String(def);
    const c = Number(cur), d = Number(def);
    if (!isFinite(c) || !isFinite(d)) return true;
    return Math.abs(c - d) > 0.005; // tolérance flottants (pH à 2 décimales)
  }

  // Construit le récapitulatif avant/après : { node (DOM), text (fallback), changes }
  function _buildRegulationResetSummary(cfg, defs) {
    const node = document.createElement("div");
    const lines = [];
    let changes = 0;
    kRegulationResetFields.forEach((f) => {
      const cur = cfg[f.key];
      const def = defs[f.key];
      const changed = _regValueChanged(f.key, cur, def);
      if (changed) changes++;
      const curStr = cur == null ? "—" : f.fmt(cur);
      const defStr = def == null ? "—" : f.fmt(def);
      const row = document.createElement("div");
      row.textContent = `${f.label} : ${curStr} → ${defStr}`;
      // Mise en évidence discrète des lignes qui changent réellement
      if (changed) row.style.fontWeight = "600";
      else row.classList.add("muted");
      node.appendChild(row);
      lines.push(row.textContent);
    });
    const foot = document.createElement("div");
    foot.style.marginTop = "10px";
    foot.textContent = changes === 0
      ? "Tous les paramètres sont déjà aux valeurs d'usine."
      : `${changes} paramètre${changes > 1 ? "s" : ""} ${changes > 1 ? "seront modifiés" : "sera modifié"} (en gras). Les calibrations, modes et limites de sécurité ne sont pas touchés.`;
    node.appendChild(foot);
    lines.push("", foot.textContent);
    return { node, text: lines.join("\n"), changes };
  }

  async function _onRegulationResetClick() {
    const btn = $("#regulation_reset_btn");
    if (!btn || btn.disabled) return;
    btn.disabled = true;
    try {
      // Valeurs courantes FRAÎCHES : window._config peut avoir été remplacé par
      // une trame WS partielle (sans regulation_speed ni regulation_defaults).
      try { await loadConfig(); }
      catch (e) { showToast("Impossible de lire la configuration — réessayez", "error"); return; }
      const cfg = window._config || {};
      const defs = cfg.regulation_defaults;
      if (!defs) { showToast("Fonction indisponible sur ce firmware", "error"); return; }

      const { node, text, changes } = _buildRegulationResetSummary(cfg, defs);
      const ok = await confirmDialog({
        title: "Restaurer les valeurs par défaut",
        message: text,
        messageNode: node,
        confirmLabel: "Restaurer",
        confirmDisabled: changes === 0,
      });
      if (!ok) return;

      let res;
      try {
        res = await authFetch("/reset-regulation-defaults", { method: "POST" });
      } catch (e) {
        if (e && e.message === "Authentication required") {
          showToast("Session expirée — reconnectez-vous", "error"); // redirection login déjà déclenchée
        } else {
          showToast("Erreur réseau — réessayez", "error");
        }
        return;
      }
      if (!res.ok) { showToast(`Erreur serveur (${res.status}) — réessayez`, "error"); return; }

      // Restauration appliquée côté firmware : recharger la config pour
      // rafraîchir TOUS les champs de la page (aucun état à moitié appliqué).
      try {
        await loadConfig();
        showToast("Valeurs d'usine restaurées", "success");
      } catch (e) {
        showToast("Valeurs d'usine restaurées — rechargez la page pour actualiser l'affichage", "success");
      }
    } finally {
      btn.disabled = false;
    }
  }

  // Appelé par loadConfig() : met en cache les valeurs d'usine si présentes,
  // les réinjecte dans une config partielle (trame WS), et pilote la
  // visibilité du bouton (masqué face à un firmware antérieur à 2.31.0).
  function syncRegulationDefaults(cfg) {
    if (cfg.regulation_defaults) _regulationDefaults = cfg.regulation_defaults;
    else {
      // bug-089 : `regulation_defaults` reste volontairement HTTP-seul (dérivable,
      // ~703 o de trame économisés). La trame WS porte désormais `pool_size` et
      // syncPoolSize a déjà réinjecté le cache `by_size` : on RECONSTRUIT la
      // colonne courante au lieu de resservir un cache périmé après un changement
      // de taille reçu par WebSocket.
      const derived = cfg.regulation_defaults_by_size && cfg.pool_size
        ? cfg.regulation_defaults_by_size[cfg.pool_size] : null;
      if (derived) _regulationDefaults = derived;
      if (_regulationDefaults) cfg.regulation_defaults = _regulationDefaults;
    }
    const block = $("#regulation_reset_block");
    if (block) block.hidden = !cfg.regulation_defaults;
  }

  // ---------- feature-090 : taille du bassin → présélection des défauts ----------
  // La taille ne fait que PRÉSÉLECTIONNER des réglages : c'est le firmware qui
  // applique les défauts de la nouvelle taille sur POST {pool_size}. L'UI
  // n'envoie JAMAIS les 7 paramètres elle-même.
  // Caches : la trame WS "config" remplace window._config sans porter
  // pool_size/regulation_defaults_by_size/regulation_speed (parité partielle,
  // même mécanique que _regulationDefaults ci-dessus).
  let _poolSizeCache = null;
  let _regulationDefaultsBySizeCache = null;
  let _regulationSpeedCache = null;

  // Appelé par loadConfig() AVANT setSegmented("regulation_speed", ...) :
  // réinjecte les champs absents d'une config partielle, synchronise le
  // sélecteur et pilote la visibilité (B4 — rétrocompat ancien firmware :
  // pool_size absent ⇒ sélecteur masqué ET vitesse ré-affichée).
  function syncPoolSize(cfg) {
    if (typeof cfg.pool_size === "string" && cfg.pool_size) _poolSizeCache = cfg.pool_size;
    else if (_poolSizeCache) cfg.pool_size = _poolSizeCache;
    if (cfg.regulation_defaults_by_size) _regulationDefaultsBySizeCache = cfg.regulation_defaults_by_size;
    else if (_regulationDefaultsBySizeCache) cfg.regulation_defaults_by_size = _regulationDefaultsBySizeCache;
    if (typeof cfg.regulation_speed === "string" && cfg.regulation_speed) _regulationSpeedCache = cfg.regulation_speed;
    else if (_regulationSpeedCache) cfg.regulation_speed = _regulationSpeedCache;

    const supported = typeof cfg.pool_size === "string" && !!cfg.pool_size;
    const sizeBlock = $("#pool_size_block");
    if (sizeBlock) sizeBlock.hidden = !supported;
    // B2 : la vitesse de correction disparaît de l'UI quand la taille pilote
    // les défauts — le réglage reste dans l'API, la NVS et MQTT.
    const speedBlock = $("#regulation_speed_block");
    if (speedBlock) speedBlock.hidden = supported;
    // Pendant le dialogue de changement, ne pas écraser la surbrillance
    // optimiste de la taille CIBLE (loadConfig frais + trames WS périodiques).
    if (supported && !_poolSizeDialogBusy) setSegmented("pool_size", cfg.pool_size);
  }

  let _poolSizeDialogBusy = false;

  async function _onPoolSizeClick(target) {
    if (_poolSizeDialogBusy) return;
    _poolSizeDialogBusy = true;
    const previous = window._config?.pool_size || _poolSizeCache || "medium";
    try {
      if (target === previous) { setSegmented("pool_size", previous); return; }
      // Valeurs courantes FRAÎCHES (même précaution que le reset 088) : la
      // colonne de gauche du dialogue doit refléter l'état réel du contrôleur.
      try { await loadConfig(); }
      catch (e) { showToast("Impossible de lire la configuration — réessayez", "error"); setSegmented("pool_size", previous); return; }
      const cfg = window._config || {};
      const defs = cfg.regulation_defaults_by_size?.[target];
      if (!defs) { showToast("Fonction indisponible sur ce firmware", "error"); setSegmented("pool_size", previous); return; }
      // La taille a pu changer ailleurs (autre onglet) pendant le rechargement.
      if (target === (cfg.pool_size || previous)) { setSegmented("pool_size", target); return; }
      setSegmented("pool_size", target);  // maintien de la surbrillance cible

      // Dialogue avant/après (mécanisme 088) : les 7 paramètres, dont la
      // vitesse de correction — elle change avec la taille et n'est plus
      // visible ailleurs, l'utilisateur DOIT le voir ici.
      const { node } = _buildRegulationResetSummary(cfg, defs);
      const labels = { small: "moins de 20 m³", medium: "20 à 60 m³", large: "plus de 60 m³" };
      const head = document.createElement("div");
      head.style.marginBottom = "10px";
      head.textContent = `Bassin de ${labels[target] || target} : les réglages de régulation seront remplacés par les valeurs recommandées pour cette taille.`;
      node.insertBefore(head, node.firstChild);
      const ok = await confirmDialog({
        title: "Changer la taille du bassin",
        message: "",
        messageNode: node,
        confirmLabel: "Appliquer",
      });
      if (!ok) { setSegmented("pool_size", previous); return; }

      // pool_size SEUL : le firmware applique les défauts, journalise et retune.
      const r = await sendConfigDetailed({ pool_size: target });
      if (!r.ok) {
        setSegmented("pool_size", previous);
        showToast(r.message || r.error || "Erreur lors de la sauvegarde", "error");
        return;
      }
      try {
        await loadConfig();
        // syncPoolSize ne touche pas au sélecteur tant que le flux est actif
        // (_poolSizeDialogBusy) : alignement explicite sur l'état serveur.
        setSegmented("pool_size", window._config?.pool_size || target);
        showToast("Taille du bassin enregistrée — réglages recommandés appliqués", "success");
      } catch (e) {
        setSegmented("pool_size", target);
        showToast("Taille enregistrée — rechargez la page pour actualiser l'affichage", "success");
      }
    } finally {
      _poolSizeDialogBusy = false;
    }
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
    // 🔴 CORRIGÉ APRÈS TESTS (feature-156) : toute page encore en mode réglage est
    // RAMENÉE À SON ÉTAT NORMAL avant de changer de vue.
    // Sans ça : entrer en mode sur pH, partir par le MENU (sans cliquer le lien de
    // retour), revenir sur pH ⇒ la page restait AMPUTÉE — `is-flow-maintenance`
    // n'était jamais retirée, donc débit/régulation/historique restaient masqués.
    // Et deux pages pouvaient rester en mode SIMULTANÉMENT, chaque vue portant sa
    // classe indépendamment, aucune n'étant nettoyée.
    // ⚠️ On appelle exitMaintenance(page, false) — silencieux, SANS restauration du
    // défilement ni du focus — et NON requestExitMaintenance() :
    // ce dernier ouvre une confirmation si une calibration est en vol, et une
    // modale qui s'ouvre pendant un changement de route piégerait l'utilisateur
    // dans une page qu'il est déjà en train de quitter.
    _exitAllMaintenanceModes();
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

    // feature-096 : la mémoire 7 jours du temps d'atteinte de cible n'est relue
    // qu'à l'ENTRÉE des vues pH et ORP (puis au changement d'état ou de jour,
    // cf. updateTimeToTargetDisplay). JAMAIS en polling : l'anneau NVS n'est
    // écrit que deux fois par jour et par régulation.
    if (routeObj.view === "/ph" || routeObj.view === "/orp") {
      fetchKpiHistory();
    }

    // Mobile: close sidebar after navigation
    closeSidebar();
    perf?.end();
  }

  // ---------- Charts (Dashboard) ----------
  // Bande du graphique pH : plage idéale de l'eau (littérature — poolplanet,
  // optimum 7,4), distincte de la plage de consigne 7,20-7,50 et des seuils de
  // garde — Option B, spec 083. L'ORP n'a AUCUNE bande : pas de plage
  // universelle (dépend du pH, du stabilisant, de la sonde) — consigne seule.
  const PH_IDEAL_MIN = 7.2;
  const PH_IDEAL_MAX = 7.6;
  const PH_AXIS_MIN_DEFAULT = 6;
  const PH_AXIS_MAX_DEFAULT = 9;
  const PH_ZONE_COLOR = 'rgba(239, 68, 68, 0.08)';
  const PH_LINE_COLOR = 'rgba(239, 68, 68, 0.7)';

  const ORP_AXIS_MIN_DEFAULT = 500;
  const ORP_AXIS_MAX_DEFAULT = 900;
  // Ligne de consigne (ph_target / orp_target servis par /get-config et le WS)
  const TARGET_LINE_COLOR = 'rgba(59, 130, 246, 0.85)';
  const TARGET_LINE_DASH = [6, 6];
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

  // ---------- Préférences d'interface persistées (namespace poolctl.ui.*) ----------
  // localStorage peut lever À LA LECTURE COMME À L'ÉCRITURE (Safari en navigation
  // privée, stockage désactivé par politique) : tout est enveloppé, et un objet en
  // mémoire sert de repli. Un échec ne remonte JAMAIS à l'utilisateur — la
  // préférence vaut alors pour la session seulement.
  const _uiPrefFallback = {};

  function uiPrefGet(key, defaultValue) {
    try {
      const v = window.localStorage.getItem(key);
      if (v != null) return v;
    } catch (e) { /* stockage indisponible : repli mémoire */ }
    return (key in _uiPrefFallback) ? _uiPrefFallback[key] : defaultValue;
  }

  function uiPrefSet(key, value) {
    _uiPrefFallback[key] = value;
    try {
      window.localStorage.setItem(key, value);
    } catch (e) { /* stockage indisponible : la préférence ne survit pas au rechargement */ }
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

  // ---------- Dosage historisé : un CHAMP DE BITS, jamais un booléen (bug-098) ----------
  // Depuis la 2.47.0, la clé `dosing` est servie en ENTIER — et les DEUX
  // surfaces portent les MÊMES trois bits : /get-history relaie le masque
  // verbatim, exactement celui du fichier interne /history.json.
  //   0x01 = dosage pH · 0x02 = dosage ORP · 0x04 = dosage, voie indéterminée
  // Valeurs donc observées : 0, 1, 2, 3 et 4 (jusqu'à 7 en théorie). Le 0x04
  // marque les points écrits par les firmwares <= 2.46, dont on sait qu'il y a
  // eu dosage mais pas sur quelle voie.
  // On teste des BITS, jamais l'égalité : un bit inconnu d'un firmware plus
  // récent doit rester sans effet ici.
  const DOSE_PH = 0x01;
  const DOSE_ORP = 0x02;
  const DOSE_UNKNOWN = 0x04;
  const DOSE_MASK = DOSE_PH | DOSE_ORP | DOSE_UNKNOWN;

  // Normalisation de ce qui arrive du réseau OU d'un fichier importé.
  // POURQUOI un `true` ne peut PAS se lire « pH » : un booléen dit qu'il y a EU
  // dosage, il ne dit RIEN de la voie. Le lire 0x01 inventerait une information
  // absente (et accuserait la pompe de pH d'injections de chlore) — c'est
  // exactement le défaut que bug-098 corrige côté firmware, on ne le
  // réintroduit pas côté client. Un booléen vrai vaut donc 0x04.
  function dosingMaskFromApi(v) {
    if (typeof v === 'number' && isFinite(v)) return Math.max(0, Math.trunc(v)) & DOSE_MASK;
    if (typeof v === 'string') {
      const t = v.trim().toLowerCase();
      if (t === '') return 0;
      const n = Number(t);
      if (isFinite(n)) return Math.max(0, Math.trunc(n)) & DOSE_MASK;
      return (t === 'true' || t === 'yes' || t === 'y' || t === 'on') ? DOSE_UNKNOWN : 0;
    }
    return v === true ? DOSE_UNKNOWN : 0;
  }

  // Lignes d'infobulle : le marqueur ne porte JAMAIS seul l'information.
  function dosingLabels(mask) {
    const out = [];
    if (mask & DOSE_PH) out.push('Dosage pH');
    if (mask & DOSE_ORP) out.push('Dosage chlore');
    if (mask & DOSE_UNKNOWN) out.push('Dosage (voie inconnue)');
    return out;
  }

  // Rendus des marqueurs : trois formes DISTINCTES (couleur + rang + plein/creux).
  // pH au rang du bas, chlore au rang du dessus, voie inconnue en creux à cheval
  // sur les deux — on doit pouvoir dire quelle pompe a dosé, d'un coup d'œil.
  const DOSE_MARK_PH = { color: 'rgba(146,64,14,0.85)', row: 0, hollow: false };
  const DOSE_MARK_ORP = { color: 'rgba(37,99,235,0.85)', row: 1, hollow: false };
  const DOSE_MARK_UNKNOWN = { color: 'rgba(89,95,107,0.8)', row: 2, hollow: true };

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

  // feature-147 (correctif) : mini-graphiques du tableau de bord ENTIÈREMENT
  // retirés (pH/ORP puis Température) — décision utilisateur (« avec cet
  // affichage, le graphique n'est plus utile », puis « oui, je veux que ça
  // suive » pour la Température). createMiniLineChart/updateMiniChart/
  // appendMiniChart/applyMiniChartFallbackScale/miniRawSeries/miniDoseSeries/
  // loadMiniChartData/miniChartPointLabel/startOfTodaySeconds/getMiniChartRGB/
  // getMiniChartColor/hasFiniteValue n'avaient plus aucun appelant : supprimés
  // (code mort), pas masqués. createLineChart (pages de détail) et la trace de
  // debug (feature-057) restent inchangés, l'infrastructure uPlot partagée
  // (observeChart, makeUplotCursor, tooltipPlugin, CHART_GAP_SECONDS, DOSE_*…)
  // est conservée car ils en dépendent encore.

  // Usine des graphiques détail (Température / pH / ORP)
  // referenceLines : { max, min, zoneColor, lineColor } → lignes + zones hors
  // plage (bandes uPlot). targetLine : { color, dash } → ligne de consigne
  // horizontale, mutable via setTarget() sans recréer le graphe (feature-083).
  // axisMin/axisMax (requis si referenceLines ou targetLine) : bornes Y par
  // défaut, étendues dynamiquement via calculateAxisLimits (données + consigne).
  function createLineChart(host, color, label, options = {}) {
    if (!host || typeof uPlot === 'undefined') return null;
    const {
      integerOnly = false, fill = true,
      referenceLines = null, targetLine = null,
      axisMin = null, axisMax = null
    } = options;

    const hasBands = referenceLines != null;
    const hasTarget = targetLine != null;
    const dynamicY = hasBands || hasTarget; // bornes Y pilotées par calculateAxisLimits
    // Index de la série de mesure (la DERNIÈRE : après les 4 séries de zones et
    // l'éventuelle ligne de consigne) — pilote le curseur et le tooltip
    const measureIdx = 1 + (hasBands ? 4 : 0) + (hasTarget ? 1 : 0);
    const st = {
      labels: [],
      values: [],
      ts: [],   // timestamp du dernier point contribuant à chaque entrée (tooltip)
      gran: [], // granularité de ce point (0/1/2)
      // Consigne : null tant que non servie (firmware antérieur) → aucune ligne
      // tracée, jamais de valeur inventée (clé absente ≠ 0 — spec 083)
      target: null,
      yMin: dynamicY ? axisMin : null,
      yMax: dynamicY ? axisMax : null
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

    // Ordre des séries : x, [4 séries de zones], [ligne de consigne], mesure —
    // les zones restent aux index 1-4 (bands uPlot figées sur [1,2]/[3,4])
    const series = [{}];
    if (hasBands) series.push(boundLine(), refLine(), refLine(), boundLine());
    if (hasTarget) series.push({
      scale: 'y',
      stroke: targetLine.color,
      width: 2,
      dash: targetLine.dash,
      points: { show: false }
    });
    series.push(measureSeries);

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

    const initialData = series.map(() => []);
    const u = new uPlot(opts, initialData, host);
    observeChart(host, u);

    return {
      u,
      st,
      // Reconstruit les données columnar + bornes Y dynamiques (les zones suivent)
      render() {
        const xs = st.labels.map((_, i) => i);
        if (dynamicY) {
          // Bornes Y : données + consigne — la ligne de consigne reste toujours
          // visible dans le cadre (spec 083)
          const forLimits = st.target != null ? st.values.concat(st.target) : st.values;
          const limits = calculateAxisLimits(forLimits, axisMin, axisMax);
          st.yMin = limits.min;
          st.yMax = limits.max;
          const constant = (v) => Array(xs.length).fill(v);
          const data = [xs];
          if (hasBands) {
            data.push(
              constant(limits.max),          // borne haute de la zone haute
              constant(referenceLines.max),  // ligne max
              constant(referenceLines.min),  // ligne min
              constant(limits.min)           // borne basse de la zone basse
            );
          }
          // Consigne null → série entièrement null : uPlot ne trace rien
          if (hasTarget) data.push(constant(st.target));
          data.push(st.values);
          u.setData(data);
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
      },
      // feature-083 : consigne mutable sans recréer le graphe. Les options uPlot
      // sont capturées par closure → on mute l'état partagé st.target PUIS on
      // re-rend via setData. Normalisation à 2 décimales : /get-config sert un
      // arrondi, le WS le float brut — sans elle, chaque trame provoquerait un
      // re-render cosmétique. Valeur absente/non finie → ignorée : une trame
      // incomplète n'efface jamais une ligne existante, et rien n'est inventé.
      setTarget(v) {
        if (!hasTarget || typeof v !== 'number' || !isFinite(v)) return;
        const norm = Math.round(v * 100) / 100;
        if (norm === st.target) return;
        st.target = norm;
        this.render();
      }
    };
  }

  // Graphe uPlot à N séries pour la trace des signaux capteurs (feature-057) —
  // DÉJÀ générique en nombre de séries malgré le nom historique (corrigé en
  // revue feature-153 : ne pas ajouter de second helper pour la vue Journée,
  // cette même fonction sert les deux cadences). Axe X numérique (secondes
  // relatives par défaut, ou toute autre unité via opts.xFormat — ex. index de
  // points horodatés pour la vue Journée). Les null créent des trous (uPlot
  // les gère).
  // seriesDefs : [{ label, color }, ...] + options par série (feature-072,
  // rétro-compatibles — défauts = comportement historique) :
  //   width (défaut 2), dash (pointillés), linear (paths linéaires au lieu de
  //   spline — un point valide isolé entre deux trous doit rester visible),
  //   points (point visible taille 4), noTooltip (exclu du tooltip et du
  //   curseur), tooltip(v) → texte custom.
  // opts.yInclude : seuil mutable TOUJOURS inclus dans l'échelle Y
  // (jamais d'autorange qui zoomerait sous le seuil).
  // opts.xFormat(v) : formatte un tick/une valeur d'axe X (défaut `${v}s`,
  // feature-153 : la vue Journée y branche un libellé horaire).
  // opts.extraPlugins : plugins uPlot additionnels, ajoutés après le tooltip
  // (feature-153 : marqueur de bascule de granularité, B15).
  function createTraceChart(host, seriesDefs, opts = {}) {
    if (!host || typeof uPlot === 'undefined') return null;
    const st = { xs: [], yInclude: opts.yInclude ?? null };
    const xFmt = typeof opts.xFormat === 'function' ? opts.xFormat : (v) => `${v}s`;

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
          values: (u, splits) => splits.map(v => (v != null ? xFmt(v) : ''))
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
          title: (u, idx) => (st.xs[idx] != null ? xFmt(st.xs[idx]) : ''),
          rows: (u, idx) => seriesDefs.map((sd, si) => {
            if (sd.noTooltip) return null;
            const v = u.data[si + 1][idx];
            if (v == null || isNaN(v)) return null;
            return { color: sd.color, text: sd.tooltip ? sd.tooltip(v) : `${sd.label}: ${v}` };
          }).filter(Boolean)
        }),
        ...(Array.isArray(opts.extraPlugins) ? opts.extraPlugins : [])
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

  // =============================================================================
  // feature-153 — logique PURE de la carte « Signaux capteurs » (D3) : ces trois
  // fonctions sont EXTRAITES telles quelles par le harnais de test
  // (test/trace_logic_vectors.js, patron sha256_vectors.js) et évaluées sous
  // Node. Elles doivent donc rester SANS dépendance au DOM ni à authFetch —
  // pures, nommées, signatures FIGÉES (décision architecte D3). Ne pas les
  // renommer, ne pas les rendre impures.
  // =============================================================================

  // Regroupe des points d'historique par fenêtre temporelle de `windowMs`
  // (même unité que `p.timestamp` — appelant responsable de la cohérence :
  // la vue Journée convertit les secondes epoch de /get-history en
  // millisecondes avant l'appel, feature-153/B3). Chaque point d'entrée porte
  // au moins `timestamp` ; les champs numériques (`ph`, `orp`, `temperature`,
  // …) sont MOYENNÉS en ignorant les `null`/non-finis — un groupe sans aucune
  // valeur finie pour un champ rend `null` (jamais 0, jamais d'interpolation,
  // B5). `granularity` du point de sortie = la plus grossière du groupe.
  function groupHistoryPoints(points, windowMs) {
    if (!Array.isArray(points) || !Number.isFinite(windowMs) || windowMs <= 0) return [];
    const buckets = new Map();
    const order = [];
    for (const p of points) {
      if (!p || !Number.isFinite(p.timestamp)) continue;
      const key = Math.floor(p.timestamp / windowMs);
      if (!buckets.has(key)) { buckets.set(key, []); order.push(key); }
      buckets.get(key).push(p);
    }
    order.sort((a, b) => a - b);
    const numericKeys = ['ph', 'orp', 'temperature'];
    return order.map(key => {
      const group = buckets.get(key);
      const out = { timestamp: key * windowMs + Math.floor(windowMs / 2) };
      for (const field of numericKeys) {
        let sum = 0, n = 0;
        for (const p of group) {
          const v = p[field];
          if (v == null || typeof v !== 'number' || !Number.isFinite(v)) continue;
          sum += v; n++;
        }
        out[field] = n > 0 ? sum / n : null;
      }
      let gran = 0;
      for (const p of group) {
        const g = Number.isFinite(p.granularity) ? p.granularity : 0;
        if (g > gran) gran = g;
      }
      out.granularity = gran;
      return out;
    });
  }

  // Min / max / moyenne d'un tableau de valeurs (null/non-finis ignorés,
  // JAMAIS traités comme 0). `n` = nombre de valeurs finies retenues :
  //   n === 0 → min/max/moyenne tous `null` (aucune valeur, pas un extremum à
  //             zéro) ; n === 1 → min = max = moyenne = l'unique valeur.
  function seriesStats(values) {
    if (!Array.isArray(values)) return { min: null, max: null, avg: null, n: 0 };
    let min = null, max = null, sum = 0, n = 0;
    for (const v of values) {
      if (v == null || typeof v !== 'number' || !Number.isFinite(v)) continue;
      if (min == null || v < min) min = v;
      if (max == null || v > max) max = v;
      sum += v;
      n++;
    }
    return { min, max, avg: n > 0 ? sum / n : null, n };
  }

  // Durée couverte par `sampleCount` échantillons à la cadence `intervalMs` —
  // JAMAIS codée en dur (ADR-0073 : la cadence de lecture a déjà changé une
  // fois, cf. feature-152). Entrées invalides/non positives → 0, jamais une
  // valeur inventée.
  function traceWindowDurationMs(sampleCount, intervalMs) {
    const n = Number(sampleCount);
    const iv = Number(intervalMs);
    if (!Number.isFinite(n) || !Number.isFinite(iv) || n <= 0 || iv <= 0) return 0;
    return n * iv;
  }

  // Formattage FR d'une durée en millisecondes ("16 min 40 s", "8 h 0 min",
  // "0 s"). Pure, mais hors du périmètre D3 (pas une des trois signatures
  // figées) — simple mise en forme, aucune décision.
  function _formatTraceDurationFr(ms) {
    if (!Number.isFinite(ms) || ms <= 0) return '0 s';
    const totalSec = Math.round(ms / 1000);
    const h = Math.floor(totalSec / 3600);
    const m = Math.floor((totalSec % 3600) / 60);
    const s = totalSec % 60;
    const parts = [];
    if (h > 0) parts.push(`${h} h`);
    if (h > 0 || m > 0) parts.push(`${m} min`);
    parts.push(`${s} s`);
    return parts.join(' ');
  }

  // Plugin uPlot (feature-153, B15) : marque la bascule de granularité
  // (RAW → horaire → quotidien) par un trait vertical pointillé + une
  // étiquette discrète, dessinés dans le hook `draw` — le STYLE des courbes
  // elles-mêmes reste intact (ne pas le modifier : un style de courbe changé
  // se confondrait avec un trou de mesure, qui est précisément ce que B5 rend
  // lisible). `getGranularity()` retourne le tableau de granularités (0/1/2)
  // aligné sur les x actuellement affichés.
  function granularityMarkerPlugin(getGranularity) {
    return {
      hooks: {
        draw(u) {
          const gran = getGranularity();
          if (!Array.isArray(gran) || gran.length === 0) return;
          const ctx = u.ctx;
          ctx.save();
          let prev = gran[0];
          for (let i = 1; i < gran.length; i++) {
            const g = gran[i];
            if (g === prev) continue;
            const x = u.valToPos(u.data[0][i], 'x', true);
            ctx.beginPath();
            ctx.setLineDash([4, 4]);
            ctx.strokeStyle = 'rgba(107,114,128,0.6)';
            ctx.lineWidth = 1;
            ctx.moveTo(x, u.bbox.top);
            ctx.lineTo(x, u.bbox.top + u.bbox.height);
            ctx.stroke();
            const label = granularityLabel(g);
            if (label) {
              ctx.setLineDash([]);
              ctx.fillStyle = 'rgba(107,114,128,0.9)';
              ctx.font = `11px ${UPLOT_FONT_STACK}`;
              ctx.textBaseline = 'top';
              ctx.fillText(label, x + 4, u.bbox.top + 2);
            }
            prev = g;
          }
          ctx.restore();
        }
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
      // bug-101 : on retient le total SEULEMENT quand la plage est « tout »,
      // sinon on afficherait un compte partiel dans la confirmation de
      // remplacement. Aucune requête supplémentaire n'est faite pour l'obtenir.
      if (range === 'all') _historyAllCount = history.length;

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
    // feature-146 : Démarrer/Arrêter changent désormais le MODE — en mode
    // Arrêt, « Démarrer » est précisément la sortie du mode : plus jamais grisés.
    if (startBtn) startBtn.disabled = false;
    if (stopBtn) stopBtn.disabled = false;

    // feature-146 : le hint décrit les quatre modes en permanence (plus de
    // masquage conditionnel — il n'est plus spécifique au mode Auto).
    if (modeHint) modeHint.style.display = "";

    if (mode === "manual") {
      // Programme : horaires éditables
      if (scheduleFields) scheduleFields.style.display = "";
      start.disabled = false;
      end.disabled = false;
      start.classList.remove("is-computed");
      end.classList.remove("is-computed");
    } else if (mode === "auto") {
      // Auto : horaires calculés (lecture seule)
      if (scheduleFields) scheduleFields.style.display = "";
      start.disabled = true;
      end.disabled = true;
      start.classList.add("is-computed");
      end.classList.add("is-computed");
    } else {
      // Marche ou Arrêt : pas d'horaires
      if (scheduleFields) scheduleFields.style.display = "none";
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
        // ⚠️ CORRIGÉ (feature-156) : cette branche posait
        // `calCard.style.display = "none"`. Deux défauts.
        // (1) Un style inline ÉCRASE la règle CSS du mode réglage — le piège même
        //     que ce lot a éliminé partout ailleurs.
        // (2) Il était COLLANT : aucune branche ne le retirait à la réactivation
        //     de la sonde, donc la carte d'étalonnage n'aurait PLUS JAMAIS pu
        //     s'afficher, mode réglage compris.
        // La visibilité de cette carte appartient désormais au mode seul. Si la
        // sonde est désactivée alors qu'on est dedans, on en SORT proprement —
        // rester dans un écran d'étalonnage pour une sonde qu'on vient d'éteindre
        // n'a pas de sens.
        const tempView = $("#view-temperature");
        if (tempView && tempView.classList.contains("is-flow-maintenance")) {
          exitMaintenance("temperature", false);
        }
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
    // feature-092 : la carte « Débit d'injection pH » dépend AUSSI du mode ET de
    // la disponibilité de la donnée — sa visibilité est décidée en un seul
    // endroit (updatePhDosingCard, qui pilote également la capacité journalière
    // et l'alerte de sous-dimensionnement).
    updatePhDosingCard();
    // feature-034 itération 3 (volet A) : le bouton Calibrer la sonde vit désormais
    // sous la rangée des badges (#ph-card-stats, toujours visible) → accessible
    // dans tous les modes par construction, sans gestion de display ici.
  }

  function updatePhControls() {
    updatePhModeControls();
  }

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

  // ---------- feature-155 : accord de la mesure pH sur un test manuel ----------
  // Remplace l'ancien réglage d'un offset abstrait (feature-067) : l'utilisateur
  // AMÈNE un nombre affiché (le pas-à-pas est RÉAFFECTÉ, pas supprimé) à la
  // valeur relevée sur son test. Condition 1 du gate pool-chemistry : c'est le
  // FIRMWARE qui calcule l'écart, avec SA mesure à l'instant de la réception
  // (POST /tune-ph-offset) — tout ce qui suit ici n'est qu'un APERÇU, jamais la
  // décision. Le mot « offset » ne doit apparaître dans AUCUN texte visible.

  // Repli de secours (firmware antérieur qui ne servirait pas encore ces deux
  // champs) : jamais une autorité, même convention que PH_BOUNDS_FALLBACK.
  const PH_TUNE_CONFIG_FALLBACK = Object.freeze({
    calPointsMin: 2,
    minFiltrationMs: 600000,  // 10 min — kPhTuneMinFiltrationMs (constants.h)
  });
  let _phTuneCalPointsMin = PH_TUNE_CONFIG_FALLBACK.calPointsMin;
  let _phTuneMinFiltrationMs = PH_TUNE_CONFIG_FALLBACK.minFiltrationMs;

  // ph_cal_points_min / ph_tune_min_filtration_ms : HTTP-seules (comme
  // ph_offset_min/_max/_step ci-dessus) — absentes des trames WS, jamais
  // retombées sur le repli entre deux /get-config.
  function _applyPhTuneConfig(cfg) {
    const cpm = _phNum(cfg?.ph_cal_points_min);
    if (isFinite(cpm) && cpm >= 1) _phTuneCalPointsMin = cpm;
    const mfm = _phNum(cfg?.ph_tune_min_filtration_ms);
    if (isFinite(mfm) && mfm >= 0) _phTuneMinFiltrationMs = mfm;
  }

  // feature-156 (B11/A2) : bannière PERSISTANTE « Correction À REVOIR après
  // l'étalonnage » — pilotée par le champ firmware `ph_offset_stale` (servi par
  // /get-config ET la trame WS "config", donc lu ici depuis `cfg`, jamais
  // recalculé côté client, ADR-0100). Verdict : correction antérieure au dernier
  // étalonnage ET non nulle — survit à un rechargement ET à un redémarrage,
  // contrairement au conseil transitoire de la carte Calibration
  // (#ph_cal_final_advice).
  // ⚠️ DEUX CORRECTIONS DE CE COMMENTAIRE (revue de code) :
  //  1. il citait « correction DÉSACTIVÉE par l'étalonnage », le libellé que le
  //     gate pool-chemistry a BANNI : la correction n'est pas désactivée, elle
  //     RESTE APPLIQUÉE et continue d'orienter le dosage tant que l'utilisateur
  //     n'a pas tranché. Ne pas le ressusciter, même en commentaire ;
  //  2. il affirmait que `ph_offset_stale` « n'existe pas encore côté firmware » —
  //     c'était vrai pendant quelques heures, ça ne l'est plus : le champ est
  //     servi des deux côtés. Un firmware ANTÉRIEUR rendra `undefined`, et la
  //     bannière restera masquée : compatible en avant, rien à changer.
  function renderPhOffsetStaleBanner(cfg) {
    const banner = $("#ph_tune_stale_banner");
    if (!banner) return;
    // ⚠️ `isFinite` N'EST PAS REDONDANT avec `typeof === "number"` : en
    // JavaScript, `typeof NaN === "number"`. Sans lui, un `ph_offset` non fini
    // passerait la garde, et comme `NaN !== 0` est VRAI la bannière
    // s'afficherait en annonçant « NaN pH ». Garde alignée sur celle de
    // renderPhTuneRemoveRow(), qui lit le MÊME champ (2026-10-12) — deux
    // lectures d'une même donnée ne doivent pas avoir deux forces de garde.
    const offset = (typeof cfg?.ph_offset === "number" && isFinite(cfg.ph_offset)) ? cfg.ph_offset : 0;
    const stale = (cfg?.ph_offset_stale === true) && (offset !== 0);
    if (!stale) { banner.hidden = true; return; }
    const valueEl = $("#ph_tune_stale_value");
    // feature-157 : seul le format du nombre change (une décimale) — le texte
    // figé autour (imposé par le gate) reste intact.
    if (valueEl) valueEl.textContent = _fmtPh(offset, true);
    banner.hidden = false;
  }

  function bindPhOffsetStaleBanner() {
    $("#ph_tune_stale_cancel_btn")?.addEventListener("click", async () => {
      const res = await sendConfigDetailed({ ph_offset: 0 });
      if (res.ok) { showToast("Correction annulée", "success"); loadConfig(); }
      else showToast("Échec de l'annulation — " + (res.message || res.error), "error");
    });
    $("#ph_tune_stale_keep_btn")?.addEventListener("click", async () => {
      const current = (typeof window._config?.ph_offset === "number") ? window._config.ph_offset : 0;
      // ⚠️ RE-ENVOYER LA VALEUR COURANTE N'EST PAS UNE REDONDANCE, NE PAS
      // « SIMPLIFIER » EN UN SIMPLE MASQUAGE CLIENT : ce POST re-estampille
      // côté firmware l'instant de pose de la correction et ÉTEINT DURABLEMENT
      // le verdict « caduque » (cf. feature-156, A1/A2). Sans lui, « Garder »
      // ne ferait que fermer la bannière dans cet onglet — elle réapparaîtrait
      // au prochain rechargement, ou sur un autre appareil connecté.
      // ⛔ Ne PAS passer par /tune-ph-offset : il exige 10 min de filtration et
      // une sonde déjà étalonnée — absurde pour RETIRER une correction, et
      // cette route n'a jamais à s'appliquer ici puisque la valeur ne change pas.
      const res = await sendConfigDetailed({ ph_offset: current });
      // Condition mineure du gate pool-chemistry : « conservée » seul laisserait
      // croire que le sujet est clos. La correction a été posée AVANT l'étalonnage
      // et n'a pas été revérifiée — le toast le rappelle au moment du choix.
      if (res.ok) { showToast("Correction conservée — vérifiez-la par un test fiable", "success"); loadConfig(); }
      else showToast("Échec — " + (res.message || res.error), "error");
    });
  }

  // feature-157 : bouton « Supprimer la correction », distinct du pas-à-pas.
  // MASQUAGE MUTUEL avec la bannière « correction caduque » ci-dessus : si
  // elle est affichée, elle seule agit (ses deux boutons couvrent déjà le cas
  // de la suppression) — pas de doublon visuel (B3).
  function renderPhTuneRemoveRow(cfg) {
    const label = $("#ph_tune_remove_label");
    const btn = $("#ph_tune_remove_btn");
    const hint = $("#ph_tune_remove_hint");
    const valueEl = $("#ph_tune_remove_value");
    if (!label || !btn || !hint) return;
    // Offset absent ou non numérique => traité comme 0 => ligne et bouton
    // masqués (jamais un bouton qui supprimerait une correction inconnue).
    const offset = (typeof cfg?.ph_offset === "number" && isFinite(cfg.ph_offset)) ? cfg.ph_offset : 0;
    const staleBannerShown = (cfg?.ph_offset_stale === true) && (offset !== 0);
    const show = (offset !== 0) && !staleBannerShown;
    label.hidden = !show;
    btn.hidden = !show;
    hint.hidden = !show;
    if (valueEl) valueEl.textContent = _fmtPh(offset, true);
  }

  // ⛔ Ne PAS passer par /tune-ph-offset : il exige 10 min de filtration et une
  // sonde étalonnée — absurde pour RETIRER une correction. /save-config est le
  // contrat de stockage, déjà validé (gate pool-chemistry, gate 2 feature-156).
  // ⛔ Aucune confirmation (verdict du gate) : geste réversible qui va vers le
  // défaut sûr (mesure nue de la sonde).
  function bindPhTuneRemoveRow() {
    $("#ph_tune_remove_btn")?.addEventListener("click", async () => {
      // sendConfigDetailed et PAS sendConfig : « réessayez » invite à répéter un
      // geste qui échouera à l'identique si le contrôleur est injoignable. Le
      // refus de PLAGE est hors d'atteinte ici (0 est toujours valide), mais la
      // panne de liaison, elle, arrive — et elle doit être NOMMÉE.
      const res = await sendConfigDetailed({ ph_offset: 0 });
      // Échec : la correction reste, aucun message de succès (B7/tests exigés
      // par le gate).
      if (res.ok) {
        showToast("Correction supprimée — la mesure affichée est celle de la sonde.", "success");
        loadConfig();
      } else {
        showToast("Échec de la suppression — " + (res.message || res.error), "error");
      }
    });
  }

  // Excursion du pas-à-pas autour de la lecture appareil (ergonomie — PAS un
  // seuil de sécurité, celui-ci reste ph_offset_min/_max, servi par le serveur
  // et utilisé dans evaluatePhTunePreview ci-dessous). ±1,00 autour de la
  // lecture, la zone acceptable (±0,50) en étant la moitié centrale ; au-delà
  // le pas-à-pas continue mais l'écran passe en refus expliqué. Écrêtée à
  // l'échelle physique universelle du pH [0, 14].
  const PH_TUNE_RANGE = 1.00;
  const PH_TUNE_SCALE_FLOOR = 0;
  const PH_TUNE_SCALE_CEIL = 14;

  // Aperçu PUR de l'accord — ADR-0099 : pure, nommée, SANS DOM ni authFetch,
  // extractible par ancre + profondeur d'accolades. AUCUN littéral numérique
  // hors 0 et 1 dans ce corps : les seuils (pas, bornes) viennent des
  // PARAMÈTRES, jamais recopiés en dur (ADR-0100) — ils sont servis par
  // /get-config (ph_offset_step, ph_offset_min, ph_offset_max).
  //
  // ⚠️ `phValueRaw` DOIT être la valeur OFFICIELLE pH, échelle BRUTE (champ
  // `phValueRaw` de /data et du WS, cf. sensors.getPhValueRaw()) — JAMAIS
  // `ph`/`phFiltered`/`phMedian`, déjà CORRIGÉS de l'offset courant (Piège 2,
  // spec feature-155) : partir d'une valeur déjà corrigée ajouterait l'écart à
  // un écart déjà appliqué, défaut invisible à offset 0,00.
  //
  // Travaille en NOMBRE DE PAS ENTIER (et non en comparaison flottante sur une
  // borne exacte) : ±0,50 étant un multiple du pas, cela règle le même piège
  // que l'arrondi firmware (roundPhOffsetToStep) sans epsilon arbitraire.
  //
  // Retour :
  //   - mesureIndisponible : true si phValueRaw n'est pas un nombre fini —
  //     aucun autre champ n'est alors signifiant ;
  //   - ecart : (valeurAmenee − phValueRaw) arrondi au pas le plus proche,
  //     SANS bornage (un dépassement doit être VISIBLE, pas absorbé) ;
  //   - horsBornes : true si `ecart` sort de [offsetMin, offsetMax] ;
  //   - valeurAtteinte : phValueRaw + ecart — la valeur que l'appareil va
  //     RÉELLEMENT atteindre après arrondi (Piège 1), jamais `valeurAmenee`.
  function evaluatePhTunePreview(valeurAmenee, phValueRaw, offsetStep, offsetMin, offsetMax) {
    if (typeof phValueRaw !== "number" || !isFinite(phValueRaw)) {
      return { mesureIndisponible: true, ecart: null, horsBornes: false, valeurAtteinte: null };
    }
    const pas = (typeof offsetStep === "number" && isFinite(offsetStep) && offsetStep > 0) ? offsetStep : 1;
    // ⚠️ LE TEST DE BORNE PORTE SUR L'ÉCART **DEMANDÉ**, PAS SUR L'ÉCART ARRONDI.
    // C'est le MÊME contrat que le firmware (src/ph_tune_logic.h) : il compare
    // `phReference - phValueRaw` aux bornes AVANT tout arrondi.
    // 🔴 CORRIGÉ : la version précédente comparait des NOMBRES DE PAS ENTIERS
    // (`Math.round(ecart/pas)` contre `Math.round(offsetMax/pas)`). Pour tout
    // écart demandé dans ]0,50 ; 0,55], l'aperçu arrondissait à 5 pas, concluait
    // « dans la plage » et ACTIVAIT le bouton — pendant que le serveur, lui,
    // refusait en 400. Un bouton actif qui mène à un refus est un bouton qui
    // ment. Trouvé par le vecteur de COHÉRENCE APERÇU ↔ SERVEUR, pas par la
    // relecture : les deux moitiés étaient justes séparément.
    const ecartDemande = valeurAmenee - phValueRaw;
    const horsBornes = (ecartDemande > offsetMax) || (ecartDemande < offsetMin);
    // L'arrondi ne sert qu'à PRÉDIRE la valeur atteinte, jamais à juger la borne.
    // ⚠️ `Math.round` NE REPRODUIT PAS `roundf` du firmware sur les NÉGATIFS :
    // JS arrondit les demis vers +∞ (Math.round(-3.5) === -3), le C les arrondit
    // À L'OPPOSÉ DE ZÉRO (roundf(-3.5f) === -4). Un écart négatif tombant pile
    // à mi-pas (ex. −0,35) aurait donc donné −0,30 à l'écran et −0,40 dans
    // l'appareil. Encore le même défaut : l'aperçu et le verdict qui divergent.
    // On reproduit explicitement la règle du C.
    const enPas = ecartDemande / pas;
    const enPasArrondi = (enPas < 0) ? -Math.round(-enPas) : Math.round(enPas);
    const ecart = enPasArrondi * pas;
    const valeurAtteinte = phValueRaw + ecart;
    return { mesureIndisponible: false, ecart, horsBornes, valeurAtteinte };
  }

  // Seul endroit, hors de la fonction pure ci-dessus, où l'arrondi au dixième
  // est fait (affichage de la position de départ — livrable #3).
  function _phTuneRoundTenth(v) {
    return Math.round(v * 10) / 10;
  }

  let _phTuneValue = null;              // valeur AMENÉE par l'utilisateur (ex. 7.4)
  let _phTuneTouched = false;           // true dès le 1er appui +/- : plus de resynchronisation auto (#3)
  let _phTuneSubmitBusy = false;        // requête POST en cours : renderPhTuneCard() ne touche plus au bouton

  function _phTuneLiveRaw() {
    const v = latestSensorData?.phValueRaw;
    return (typeof v === "number" && isFinite(v)) ? v : NaN;
  }

  // Rendu complet de la carte — appelé à CHAQUE trame (#4, référence vivante)
  // depuis updateDetailSections(), et après chaque appui +/- ou fin de requête.
  //
  // ⚠️ LIMITATION DOCUMENTÉE (filtration) : le firmware ne sert (encore) nulle
  // part l'instant RÉEL de démarrage du cycle de filtration courant — seule la
  // route POST /tune-ph-offset le connaît, côté serveur, au moment même de la
  // décision (filtration.filtrationStartedAtMs(), web_routes_config.cpp). Cet
  // écran n'a donc qu'une ANCRE LOCALE : le premier instant où CE client a vu
  // filtration_running passer à true. Cette ancre ne peut que SOUS-ESTIMER le
  // temps réel écoulé (jamais le sur-estimer) : l'aperçu ne peut donc jamais
  // annoncer « prêt » avant que le firmware ne le soit réellement — seul le
  // sens inverse (annoncer « pas encore » alors que c'est déjà bon, borné à la
  // durée depuis l'ouverture de cet onglet) est possible. La décision RÉELLE
  // reste de toute façon côté serveur (condition 1 du gate) : un clic sur
  // « Corriger » encore trop tôt aux yeux du firmware affichera son message
  // exact, avec son décompte EXACT à lui.
  function renderPhTuneCard() {
    const valueEl = $("#ph_tune_value");
    if (!valueEl) return;  // carte absente de cette vue : rien à faire

    const data = latestSensorData || {};
    const phValueRaw = _phTuneLiveRaw();

    // Ce que l'appareil ANNONCE aujourd'hui (même nombre que la carte
    // « Lecture pH », #ph_current_value — échelle CORRIGÉE, offset courant
    // compris) : sert à la fois de position de départ du pas-à-pas (#3, zéro
    // appui = aucun changement) ET de terme de comparaison de la ligne de
    // résultat ci-dessous (#2). Repli sur phValueRaw si `ph` est indisponible
    // (offset courant alors inconnu, raw est le seul nombre qui reste vrai).
    const announced = (typeof data.ph === "number" && isFinite(data.ph)) ? data.ph
                     : (isFinite(phValueRaw) ? phValueRaw : NaN);
    if (!_phTuneTouched) {
      _phTuneValue = isFinite(announced) ? _phTuneRoundTenth(announced) : null;
    }

    // Suivi LOCAL de la filtration (cf. limitation documentée en en-tête).
    const running = data.filtration_running === true;
    // 🔴 CORRECTIF POST-LIVRAISON : la durée écoulée est SERVIE par le firmware
    // (`filtration_elapsed_s`), elle n'est plus devinée ici.
    // L'ancienne version posait un repère LOCAL au premier rendu où elle voyait la
    // filtration tourner (`_phTuneFiltrationSinceMs = Date.now()`) : elle supposait
    // donc qu'elle VENAIT de démarrer. Conséquence constatée en production — pompe
    // en marche depuis 2 h 30, écran affichant « encore 10 min », bouton désactivé,
    // et décompte qui REPARTAIT À ZÉRO À CHAQUE CHARGEMENT DE PAGE. Le serveur,
    // lui, aurait accepté : il lit `filtrationStartedAtMs()`.
    // ⚠️ 5e occurrence du même motif dans ce projet : un client qui RECALCULE ce
    // que le firmware sait déjà (ADR-0100, et les quatre défauts de feature-155).
    // Champ absent (firmware antérieur) ⇒ 0 ⇒ l'écran reste prudent et bloque,
    // jamais l'inverse : on ne débloque pas sur une information qu'on n'a pas.
    const elapsedS = data.filtration_elapsed_s;
    const filtrationElapsedMs = (running && typeof elapsedS === "number" && isFinite(elapsedS))
      ? Math.max(0, elapsedS * 1000) : 0;

    const calPoints = (typeof data.phCalPoints === "number") ? data.phCalPoints : null;

    const preview = evaluatePhTunePreview(
      _phTuneValue != null ? _phTuneValue : 0,
      phValueRaw,
      _phBounds.offsetStep, _phBounds.offsetMin, _phBounds.offsetMax
    );

    // --- Pas-à-pas : affichage + bornes d'excursion.
    const dispValue = _phTuneValue != null ? _phTuneValue : phValueRaw;
    const hasDisp = isFinite(dispValue);
    valueEl.textContent = hasDisp ? _fmtPh(dispValue, false) : "--";
    const rangeMin = isFinite(phValueRaw) ? Math.max(PH_TUNE_SCALE_FLOOR, phValueRaw - PH_TUNE_RANGE) : PH_TUNE_SCALE_FLOOR;
    const rangeMax = isFinite(phValueRaw) ? Math.min(PH_TUNE_SCALE_CEIL, phValueRaw + PH_TUNE_RANGE) : PH_TUNE_SCALE_CEIL;
    valueEl.setAttribute("aria-valuemin", String(rangeMin));
    valueEl.setAttribute("aria-valuemax", String(rangeMax));
    valueEl.setAttribute("aria-valuenow", hasDisp ? String(dispValue) : String(rangeMin));
    valueEl.setAttribute("aria-valuetext", hasDisp ? (_fmtPh(dispValue, false) + " pH") : "mesure indisponible");

    const dec = $("#ph_tune_dec"), inc = $("#ph_tune_inc");
    const pas = _fmtPh(_phBounds.offsetStep, false);
    if (dec) { dec.disabled = !hasDisp || dispValue <= rangeMin + PH_EPS; dec.setAttribute("aria-label", `Diminuer de ${pas}`); }
    if (inc) { inc.disabled = !hasDisp || dispValue >= rangeMax - PH_EPS; inc.setAttribute("aria-label", `Augmenter de ${pas}`); }

    // --- Ligne de résultat (#2) : « Mesure actuelle » = ce que l'appareil
    // ANNONCE aujourd'hui (même échelle corrigée que #ph_current_value, pas
    // phValueRaw — sinon un offset déjà non nul ferait croire à un changement
    // là où il n'y en a aucun, à l'exact endroit où zéro appui doit afficher
    // zéro changement). « après validation » reste TOUJOURS la valeur
    // RÉELLEMENT atteinte après arrondi, jamais la valeur demandée (Piège 1).
    const resultEl = $("#ph_tune_result");
    if (resultEl) {
      if (!isFinite(announced) || !isFinite(phValueRaw) || _phTuneValue == null) {
        resultEl.textContent = "";
      } else {
        resultEl.textContent = `Mesure actuelle : ${_fmtPh(announced, false)} → `
          + `après validation : ${_fmtPh(preview.valeurAtteinte, false)}`;
      }
    }

    // --- Les cinq états (design arrêté) : prêt + 4 refus, dans le MÊME ORDRE
    // qu'evaluatePhTune (src/ph_tune_logic.h) pour qu'un même jeu d'entrées
    // produise TOUJOURS le même verdict ici et côté serveur. Textes IMPOSÉS
    // par le gate, mot pour mot.
    let blockedText = "";
    if (!isFinite(phValueRaw)) {
      blockedText = "L'appareil n'a pas de mesure assez récente. Attendez qu'une valeur "
        + "s'affiche, puis recommencez.";
    } else if (!running || filtrationElapsedMs < _phTuneMinFiltrationMs) {
      const remainingMin = Math.ceil((_phTuneMinFiltrationMs - filtrationElapsedMs) / 60000);
      blockedText = "La filtration doit tourner depuis 10 minutes pour que l'eau testée "
        + "soit celle qui passe devant la sonde. Possible dans " + remainingMin + " min.";
    } else if (calPoints == null || calPoints < _phTuneCalPointsMin) {
      blockedText = "La sonde n'a pas encore ses deux points de référence. Faites d'abord "
        + "l'étalonnage (tampon 7 puis tampon 4), puis revenez ici.";
    } else if (preview.horsBornes) {
      blockedText = "Trop loin de ce que l'appareil mesure. Refaites le test (bandelettes non "
        + "périmées, lecture à la bonne échelle) et vérifiez l'étalonnage de la "
        + "sonde avant de recommencer.";
    }

    const blockedEl = $("#ph_tune_blocked");
    if (blockedEl) {
      if (blockedText) { blockedEl.textContent = blockedText; blockedEl.hidden = false; }
      else { blockedEl.textContent = ""; blockedEl.hidden = true; }
    }
    const submitBtn = $("#ph_tune_submit_btn");
    // ⚠️ `!_phTuneTouched` EST UNE GARDE, PAS UN CONFORT (relevé en revue).
    // Sans lui, « zéro appui » pouvait DÉPLACER l'offset d'un pas : la position
    // de départ est `round10(data.ph)` — la valeur ANNONCÉE, arrondie au
    // dixième — alors que l'écart se calcule contre `phValueRaw`, la valeur
    // BRUTE non arrondie. Quand l'annoncée tombe sur un demi-dixième (brut
    // 7,150 + offset 0,30 = 7,450 → départ affiché 7,5), l'écart demandé vaut
    // 0,35 → 0,40 appliqué : +0,10 d'offset SANS AUCUNE ACTION de
    // l'utilisateur, et une réécriture NVS à chaque clic.
    // C'est le même défaut que les trois autres de ce lot : confondre la valeur
    // AFFICHÉE (arrondie) avec la valeur qui SERT AU CALCUL (brute).
    if (submitBtn && !_phTuneSubmitBusy) submitBtn.disabled = !!blockedText || !_phTuneTouched;
  }

  // Un appui = un acte (#2 du gate, hérité de feature-067) : pas d'auto-répétition.
  function stepPhTune(direction) {
    _phTuneTouched = true;
    const phValueRaw = _phTuneLiveRaw();
    const base = _phTuneValue != null ? _phTuneValue
               : (isFinite(phValueRaw) ? _phTuneRoundTenth(phValueRaw) : 0);
    let next = _phRound(base + direction * _phBounds.offsetStep);
    const lo = isFinite(phValueRaw) ? Math.max(PH_TUNE_SCALE_FLOOR, phValueRaw - PH_TUNE_RANGE) : PH_TUNE_SCALE_FLOOR;
    const hi = isFinite(phValueRaw) ? Math.min(PH_TUNE_SCALE_CEIL, phValueRaw + PH_TUNE_RANGE) : PH_TUNE_SCALE_CEIL;
    _phTuneValue = clamp(next, lo, hi);
    renderPhTuneCard();
  }

  function bindPhTuneStepper() {
    $("#ph_tune_dec")?.addEventListener("click", () => stepPhTune(-1));
    $("#ph_tune_inc")?.addEventListener("click", () => stepPhTune(+1));
    $("#ph_tune_value")?.addEventListener("keydown", (e) => {
      const k = e.key;
      if (k === "ArrowUp" || k === "ArrowRight" || k === "PageUp") {
        e.preventDefault(); stepPhTune(+1);
      } else if (k === "ArrowDown" || k === "ArrowLeft" || k === "PageDown") {
        e.preventDefault(); stepPhTune(-1);
      } else if (k === "Home" || k === "End") {
        e.preventDefault();
        _phTuneTouched = true;
        const phValueRaw = _phTuneLiveRaw();
        const lo = isFinite(phValueRaw) ? Math.max(PH_TUNE_SCALE_FLOOR, phValueRaw - PH_TUNE_RANGE) : PH_TUNE_SCALE_FLOOR;
        const hi = isFinite(phValueRaw) ? Math.min(PH_TUNE_SCALE_CEIL, phValueRaw + PH_TUNE_RANGE) : PH_TUNE_SCALE_CEIL;
        _phTuneValue = (k === "Home") ? lo : hi;
        renderPhTuneCard();
      }
    });
  }

  // POST /tune-ph-offset : porte la VALEUR AMENÉE, jamais un écart déjà
  // calculé (condition 1 du gate — cf. en-tête de ce bloc). Le message du
  // serveur fait AUTORITÉ sur un refus (#9) : affiché tel quel, jamais réécrit.
  function bindPhTuneSubmit() {
    const btn = $("#ph_tune_submit_btn");
    const errEl = $("#ph_tune_error");
    if (!btn) return;

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
    const idle = () => { _phTuneSubmitBusy = false; setBtn("Corriger", false, false, "default"); renderPhTuneCard(); };

    btn.addEventListener("click", async () => {
      if (btn.disabled || _phTuneValue == null) return;
      _phTuneSubmitBusy = true;
      showErr("");
      setBtn("Correction...", true, true, "default");
      try {
        const res = await authFetch("/tune-ph-offset", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({ ph_reference: _phTuneValue }),
        });
        if (res.ok) {
          setBtn("Corrigé", false, true, "success");
          _phTuneTouched = false;  // reprend la lecture vivante au prochain rendu
          loadConfig();
          setTimeout(idle, 2000);
        } else {
          let msg = "Correction refusée.";
          try { const j = await res.json(); if (j && j.error) msg = String(j.error); } catch (e) { /* corps non JSON */ }
          showErr(msg);
          setBtn("Erreur", false, false, "error");
          setTimeout(idle, 2000);
        }
      } catch (e) {
        showErr("Contrôleur injoignable — correction non appliquée.");
        setBtn("Erreur", false, false, "error");
        setTimeout(idle, 2000);
      }
    });

    // feature-156 (B10) : #ph_tune_goto_cal_btn est SUPPRIMÉ — la carte visée
    // (#ph-card-calibration) est désormais juste au-dessus dans le même mode
    // réglage, plus besoin d'un bouton pour y faire défiler.

    bindPhTuneStepper();
    renderPhTuneCard();
  }

  // ---------- feature-154 : seuils de SANTÉ pH servis, jamais codés en dur ----------
  // classifyPhCriterion() est la SEULE fonction de jugement sain/hors/indisponible
  // pour les trois critères pH (pente acide, pente base, décalage zéro) — miroir
  // exact d'isPhProbeHealthy() (src/ph_guard_logic.h) : une pente ou un zéro HORS
  // de [min, max] est rendu « hors », l'intérieur (bornes incluses) est « sain ».
  // Les pentes s'évaluent sur (slopeMin, slopeMax), le zéro sur (-zeroMax,
  // +zeroMax) : UNE SEULE fonction pour les deux, le zéro négatif devient un cas
  // naturel (pas de Math.abs — les pentes sont déjà des pourcentages positifs).
  //
  // Rend 'indisponible' AVANT tout autre test si la valeur ou les bornes ne sont
  // pas exploitables : le vert est structurellement INATTEIGNABLE sans seuils
  // reçus du serveur (ADR-0100 : un seuil de santé est servi, jamais réécrit
  // côté client — aucun repli ici, contrairement à PH_BOUNDS_FALLBACK ci-dessus,
  // qui ne dégrade qu'une borne d'IHM ; un repli sur un seuil de santé
  // INNOCENTE une sonde).
  //
  // Pure, nommée, sans DOM ni authFetch : extractible par ancre + profondeur
  // d'accolades (ADR-0099). Aucun littéral numérique dans le corps hors 0 et 1 :
  // c'est ce que le harnais de test assertera (feature-154, architect).
  function classifyPhCriterion(value, minInclusive, maxInclusive) {
    // isFinite couvre NaN ET les deux infinis. ⚠️ `isNaN` seul NE SUFFIT PAS :
    // Infinity est un `number` qui n'est pas NaN, il passait donc la garde et
    // finissait comparé normalement (Infinity > max ⇒ 'hors'). Pas un faux vert
    // — une pastille rouge s'affichait quand même — mais un écart au contrat :
    // une valeur non représentable n'est pas une mesure hors norme, c'est une
    // mesure ABSENTE. Trouvé par le harnais de vecteurs, pas par la relecture.
    if (value == null || typeof value !== "number" || !isFinite(value)) return 'indisponible';
    if (!isFinite(minInclusive) || !isFinite(maxInclusive)) return 'indisponible';
    if (minInclusive > maxInclusive) return 'indisponible';
    if (value < minInclusive || value > maxInclusive) return 'hors';
    return 'sain';
  }

  // Seuils de santé pH reçus de /get-config (ph_slope_healthy_min_pct,
  // ph_slope_healthy_max_pct, ph_zero_healthy_max_mv — source constants.h,
  // MÊMES constantes que isPhProbeHealthy() côté firmware).
  // AUCUNE VALEUR INVENTÉE : tant qu'aucune réponse valide n'a jamais été
  // reçue, le store reste null (jamais PH_BOUNDS_FALLBACK — un repli sur un
  // seuil de SANTÉ innocenterait une sonde) et classifyPhCriterion() rend
  // 'indisponible' pour les trois critères ET le chip global (condition C2 du
  // gate pool-chemistry : vert seulement si les trois critères sont mesurés
  // et dans la plage).
  // ⚠️ Mais UNE FOIS une réponse valide reçue, un lot ABSENT ou INCOHÉRENT est
  // ignoré EN BLOC et la DERNIÈRE borne connue est CONSERVÉE — même mécanique
  // que _applyPhBounds ci-dessus. Ces trois clés sont HTTP-only
  // (kHttpOnlyKeys, test_native_ws_config_parity : « app.js conserve la
  // dernière borne reçue », texte normatif du test) : la trame WS "config" NE
  // LES PORTE JAMAIS (budget de trame), y compris le push INITIAL envoyé par
  // le serveur juste après que le client vient de faire un /get-config
  // complet au démarrage. Réinitialiser à null à chaque frame WS ferait
  // retomber les 3 pastilles sur « indisponible » quelques secondes après
  // CHAQUE chargement de page — une régression, pas une prudence.
  let _phHealth = null;

  function _applyPhHealth(cfg) {
    const slopeMin = _phNum(cfg?.ph_slope_healthy_min_pct);
    const slopeMax = _phNum(cfg?.ph_slope_healthy_max_pct);
    const zeroMax = _phNum(cfg?.ph_zero_healthy_max_mv);
    if (isFinite(slopeMin) && isFinite(slopeMax) && isFinite(zeroMax)
        && slopeMin < slopeMax && zeroMax > 0) {
      _phHealth = { slopeMin, slopeMax, zeroMax };
    }
    // sinon : lot absent/incohérent → _phHealth INCHANGÉ (conserve la
    // dernière valeur connue, ou reste null si aucune n'a jamais été reçue).
    //
    // Un modal déjà ouvert avant la réponse de /get-config affichait sinon
    // trois pastilles "indisponible" figées jusqu'à la prochaine trame WS
    // (jusqu'à 30 s) : re-rendre tout de suite si l'utilisateur regarde déjà
    // l'écran, et rafraîchir le chip global avec les mêmes seuils.
    const modal = $("#ph-probe-modal");
    if (modal && modal.open) _renderPhProbeModalValues(latestSensorData || {});
    updatePhProbeChip(latestSensorData || {});
  }

  // ---------- feature-147 : jauges pH / ORP (représentation graphique) ----------
  // Gate pool-chemistry, condition C1 : l'échelle trace UNIQUEMENT le pH
  // CORRIGÉ (`data.ph`, déjà affiché en compact-value) — jamais un mélange
  // brut/corrigé. Bornes de DESSIN uniquement (jamais légendées comme seuils) :
  // miroir de kPhFloorRawHard/kPhCeilingRawHard (src/constants.h ~1648/1669).
  // Non servies par /get-config (purement graphiques) : hors budget trame.
  const PH_GAUGE_DRAW_MIN = 6.20;
  const PH_GAUGE_DRAW_MAX = 8.10;

  // Fonction pure : valeur pH corrigée → pourcentage [0,100], ou null si
  // absente/NaN (⇒ curseur masqué, jamais de position fausse).
  function phGaugePosition(value) {
    if (typeof value !== "number" || !isFinite(value)) return null;
    const pct = (value - PH_GAUGE_DRAW_MIN) / (PH_GAUGE_DRAW_MAX - PH_GAUGE_DRAW_MIN) * 100;
    return Math.max(0, Math.min(100, pct));
  }

  // Repères discrets NON COLORÉS (gate C2) — UNE SEULE source : ces deux
  // constantes, jamais recopiées en %. Miroir de kPhLowWarn/kPhOrpInhibit
  // (src/constants.h). updatePhGauge() les pose via phGaugePosition().
  const PH_GAUGE_MARK_LOW = 7.00;   // « Eau agressive »
  const PH_GAUGE_MARK_HIGH = 7.60;  // « Chlore inefficace »

  // Chantier simplification (gate pool-chemistry GO) : phGaugeStateText()
  // (« Trop bas »/« Idéal »/« Trop haut ») a été SUPPRIMÉE — plus aucun
  // appelant depuis que updatePhGauge() ne les affiche plus. _phBounds reste
  // utilisé par le dégradé (le vert suit la fenêtre cible réelle).

  // feature-147 : ORP — échelle fixe 0-1000 mV (choix du designer, aucune
  // constante firmware à mirer ici). Dégradé ÉVALUATIF mais ASYMÉTRIQUE
  // (verdict gate pool-chemistry, 2e passage) : le rouge est relatif à LA
  // CONSIGNE (comme le vert du pH), jamais une bande fixe en mV — un ORP
  // « correct » n'existe pas dans l'absolu, mais « loin de votre cible » est
  // vrai par construction. Aucune couleur au-dessus de la consigne (neutre) :
  // la chimie est asymétrique, un ORP élevé n'est pas dangereux dans la plage
  // affichable. Voir updateOrpGauge().
  const ORP_GAUGE_MIN = 0;
  const ORP_GAUGE_MAX = 1000;

  function orpGaugePosition(value) {
    if (typeof value !== "number" || !isFinite(value)) return null;
    const pct = (value - ORP_GAUGE_MIN) / (ORP_GAUGE_MAX - ORP_GAUGE_MIN) * 100;
    return Math.max(0, Math.min(100, pct));
  }

  // Demi-largeur de la bande verte du dégradé — CONSTANTE UNIQUE, déjà en
  // production (badge capteur ORP plus bas). Chantier simplification (gate
  // GO) : orpGaugeStateText() (« Sous consigne »/« Dans la plage »/
  // « Au-dessus ») a été SUPPRIMÉE — plus aucun appelant. Perte actée et non
  // compensée : l'étendue ± ORP_GAUGE_BAND_MV n'est plus écrite nulle part
  // (tolérance d'affichage, pas un seuil chimique). La constante elle-même
  // reste : updateOrpGauge() la réutilise pour poser le dégradé.
  const ORP_GAUGE_BAND_MV = 150;

  // Met à jour la jauge pH (curseur + dégradé de zones + légende). Le dégradé
  // utilise _phBounds (live, servi par /get-config) : jamais de magie en dur
  // pour la frontière Idéal/Trop haut-bas.
  function updatePhGauge(value, measuresOk) {
    const gaugeEl = $("#ph-gauge");
    const track = $("#ph-gauge-track");
    const cursor = $("#ph-gauge-cursor");
    const stateEl = $("#ph-gauge-state");
    const tickLow = $("#ph-gauge-tick-low");
    const tickHigh = $("#ph-gauge-tick-high");
    if (!gaugeEl || !track || !cursor || !stateEl) return;
    const pMin = phGaugePosition(_phBounds.targetMin);
    const pMax = phGaugePosition(_phBounds.targetMax);
    if (pMin !== null && pMax !== null) {
      // Correctif retour utilisateur (« pas très joli, dégradé continu comme
      // la carte de référence ») : rouge → orange → jaune → vert → bleu, SANS
      // bord net. Le vert est CENTRÉ sur la fenêtre idéale réelle (pMin-pMax,
      // lue depuis /get-config via _phBounds) : si la consigne change, le
      // vert suit. Arrêts chauds (pWarm1/pWarm2) dérivés de pMin — AUCUN
      // pourcentage en dur. (Le texte d'état normal a depuis été retiré —
      // chantier simplification ci-dessous — seul le dégradé reste ici.)
      const pWarm1 = pMin / 3;
      const pWarm2 = (pMin * 2) / 3;
      track.style.background =
        `linear-gradient(to right, var(--gauge-low) 0%, ` +
        `var(--gauge-low-mid) ${pWarm1}%, var(--gauge-low-mid2) ${pWarm2}%, ` +
        `var(--gauge-ok) ${pMin}%, var(--gauge-ok) ${pMax}%, ` +
        `var(--gauge-high) 100%)`;
    }
    // Revue code-reviewer : positions dérivées de phGaugePosition(), jamais
    // recopiées en % dans le HTML (une valeur, un seul endroit).
    const pLow = phGaugePosition(PH_GAUGE_MARK_LOW);
    if (tickLow && pLow !== null) tickLow.style.left = pLow + "%";
    const pHigh = phGaugePosition(PH_GAUGE_MARK_HIGH);
    if (tickHigh && pHigh !== null) tickHigh.style.left = pHigh + "%";
    gaugeEl.classList.toggle("gauge--stale", !measuresOk);
    if (!measuresOk) {
      // EXCEPTION BLOQUANTE (gate pool-chemistry, chantier simplification) :
      // ce texte-là SURVIT — en stale, la piste n'est que grisée/désaturée
      // (.gauge--stale), l'avertissement serait sinon porté par la seule
      // couleur. Seul le mot d'état du fonctionnement NORMAL disparaît
      // ci-dessous (même motif que updateTempGauge(), précédent à suivre).
      cursor.hidden = true;
      stateEl.textContent = "Mesures non représentatives";
      stateEl.className = "gauge__legend-state gauge__legend-state--stale";
      return;
    }
    // Chantier simplification (demande utilisateur, GO gate pool-chemistry) :
    // « Trop bas »/« Idéal »/« Trop haut » retirés — la valeur chiffrée, la
    // cible chiffrée et les deux repères verbalisés disent déjà ce que la
    // couleur montre ; la position du curseur n'est pas une couleur.
    stateEl.textContent = "";
    stateEl.className = "gauge__legend-state";
    const pos = phGaugePosition(value);
    if (pos === null) {
      cursor.hidden = true;
      return;
    }
    cursor.hidden = false;
    cursor.style.left = pos + "%";
  }

  // Met à jour la jauge ORP (curseur mesure + marqueur consigne + légende).
  function updateOrpGauge(value, target, measuresOk) {
    const gaugeEl = $("#orp-gauge");
    const track = $("#orp-gauge-track");
    const cursor = $("#orp-gauge-cursor");
    const targetMarker = $("#orp-gauge-target");
    const stateEl = $("#orp-gauge-state");
    if (!gaugeEl || !cursor || !targetMarker || !stateEl) return;
    // Gate pool-chemistry GO SOUS CONDITIONS : dégradé ÉVALUATIF comme le pH,
    // mais ASYMÉTRIQUE — la chimie l'est. Rouge en s'éloignant SOUS la
    // consigne (aucun stabilisant n'explique un ORP très bas) ; vert sur
    // target ± ORP_GAUGE_BAND_MV (MÊME constante que le texte d'état,
    // condition 2) ; NEUTRE au-dessus (préféré au gate) — rien en chimie ne
    // rend un ORP élevé dangereux dans la plage affichable, la régulation
    // cesse simplement de doser. Positions dérivées de orpGaugePosition() à
    // chaque rendu (condition 1) : si la consigne change, le dégradé suit.
    if (track) {
      const pLow = (typeof target === "number" && isFinite(target))
        ? orpGaugePosition(target - ORP_GAUGE_BAND_MV) : null;
      const pHigh = (typeof target === "number" && isFinite(target))
        ? orpGaugePosition(target + ORP_GAUGE_BAND_MV) : null;
      if (pLow !== null && pHigh !== null) {
        const pLowMid = pLow / 2;
        track.style.background =
          `linear-gradient(to right, var(--gauge-low) 0%, ` +
          `var(--gauge-low-mid) ${pLowMid}%, var(--gauge-ok) ${pLow}%, ` +
          `var(--gauge-ok) ${pHigh}%, var(--gauge-track-bg) 100%)`;
        // Au-dessus de la consigne : FONDU progressif du vert vers la piste
        // neutre, et non une cassure nette. L'arrêt était posé DEUX FOIS à
        // ${pHigh}% (vert puis gris), ce qui créait une frontière franche
        // visible — signalée par l'utilisateur. Le verdict du gate
        // pool-chemistry est respecté à l'identique : AUCUNE couleur d'alerte
        // au-dessus de la consigne, seulement un retour au neutre.
      }
    }
    gaugeEl.classList.toggle("gauge--stale", !measuresOk);
    const tPos = orpGaugePosition(target);
    if (tPos === null) {
      targetMarker.hidden = true;
    } else {
      targetMarker.hidden = false;
      targetMarker.style.left = tPos + "%";
    }
    if (!measuresOk) {
      // EXCEPTION BLOQUANTE (gate pool-chemistry, chantier simplification) :
      // ce texte-là SURVIT — cf. commentaire équivalent dans updatePhGauge().
      cursor.hidden = true;
      stateEl.textContent = "Mesures non représentatives";
      stateEl.className = "gauge__legend-state gauge__legend-state--stale";
      return;
    }
    // Chantier simplification : « Sous consigne »/« Dans la plage »/
    // « Au-dessus » retirés (GO gate — « loin de la cible » est déjà dit par
    // la valeur chiffrée + le marqueur de consigne ; perte actée et non
    // compensée : l'étendue de la bande verte ± ORP_GAUGE_BAND_MV n'est plus
    // écrite nulle part, c'est une tolérance d'affichage, pas un seuil
    // chimique — ne pas ajouter de texte ici pour la réintroduire).
    stateEl.textContent = "";
    stateEl.className = "gauge__legend-state";
    const pos = orpGaugePosition(value);
    if (pos === null) {
      cursor.hidden = true;
      return;
    }
    cursor.hidden = false;
    cursor.style.left = pos + "%";
  }

  // feature-147 (correctif, parité « ça suit ») : jauge Température — mêmes
  // valeurs RÉELLES du firmware que les ticks pH, jamais inventées. Domaine
  // 0-40°C : kFiltrationRefTempMaxC = 40 est la borne de panne de sonde
  // (src/constants.h:2762) — bornes de DESSIN uniquement, jamais légendées
  // comme un seuil de panne. Un seul repère à 4°C = kFiltrationRefTempMinC
  // (src/constants.h:2761, hivernage : plus de planning auto en dessous).
  const TEMP_GAUGE_MIN = 0;
  const TEMP_GAUGE_MAX = 40;
  const TEMP_GAUGE_MARK_LOW = 4;

  // Dégradé DESCRIPTIF (demande utilisateur) — convention du thermomètre,
  // froid→chaud, AUCUN jugement de valeur : contrairement au pH (dégradé
  // ÉVALUATIF, le vert suit la fenêtre cible), aucune température d'eau n'est
  // « idéale » dans l'absolu — c'est une préférence de confort. D'où l'absence
  // de vert ici, et des variables CSS dédiées (--gauge-temp-*), distinctes de
  // celles du pH pour ne pas laisser croire à la même grammaire de couleur.
  // Pivots de teinte (°C) en TEMP_GAUGE_HUE_* : repères visuels du dégradé,
  // PAS des seuils — n'importe quel couple aurait convenu, celui-ci centre la
  // transition sur la plage de confort usuelle d'une piscine.
  const TEMP_GAUGE_HUE_COOL = 15;
  const TEMP_GAUGE_HUE_WARM = 25;

  function tempGaugePosition(value) {
    if (typeof value !== "number" || !isFinite(value)) return null;
    const pct = (value - TEMP_GAUGE_MIN) / (TEMP_GAUGE_MAX - TEMP_GAUGE_MIN) * 100;
    return Math.max(0, Math.min(100, pct));
  }

  // measuresOk : RÉUTILISE measuresAreValid() — la température est déjà
  // traitée comme non représentative filtration arrêtée (feature-061 it.2,
  // cf. compact-temperature ci-dessus) : même garde, pas une nouvelle
  // hypothèse.
  function updateTempGauge(value, measuresOk) {
    const gaugeEl = $("#temp-gauge");
    const track = $("#temp-gauge-track");
    const cursor = $("#temp-gauge-cursor");
    const tickLow = $("#temp-gauge-tick-low");
    const stateEl = $("#temp-gauge-state");
    if (!gaugeEl || !cursor || !tickLow) return;
    // Arrêts dérivés de tempGaugePosition() — AUCUN pourcentage en dur.
    const pCool = tempGaugePosition(TEMP_GAUGE_HUE_COOL);
    const pWarm = tempGaugePosition(TEMP_GAUGE_HUE_WARM);
    if (track && pCool !== null && pWarm !== null) {
      track.style.background =
        `linear-gradient(to right, var(--gauge-temp-cold) 0%, ` +
        `var(--gauge-temp-cool) ${pCool}%, var(--gauge-temp-warm) ${pWarm}%, ` +
        `var(--gauge-temp-hot) 100%)`;
    }
    const pLow = tempGaugePosition(TEMP_GAUGE_MARK_LOW);
    if (pLow !== null) tickLow.style.left = pLow + "%";
    gaugeEl.classList.toggle("gauge--stale", !measuresOk);
    if (!measuresOk) {
      cursor.hidden = true;
      if (stateEl) {
        stateEl.textContent = "Mesures non représentatives";
        stateEl.className = "gauge__legend-state gauge__legend-state--stale";
      }
      return;
    }
    if (stateEl) { stateEl.textContent = ""; stateEl.className = "gauge__legend-state"; }
    const pos = tempGaugePosition(value);
    if (pos === null) {
      cursor.hidden = true;
      return;
    }
    cursor.hidden = false;
    cursor.style.left = pos + "%";
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

  // Formatage français : virgule décimale, 1 décimale (règle produit du
  // 2026-10-10), signe explicite si demandé.
  // ⚠️ feature-157 : la frontière est l'AFFICHAGE. Cette fonction n'arrondit
  // QUE la représentation textuelle — elle ne doit JAMAIS être utilisée pour
  // produire une valeur réinjectée dans un calcul ou envoyée au serveur.
  // La fenêtre d'état du filtre est EXCLUE de cette règle : elle utilise
  // _fmtFilterVal(), une fonction distincte, non touchée ici.
  function _fmtPh(v, signed) {
    const s = Math.abs(v).toFixed(1).replace(".", ",");
    if (signed) return (v < 0 ? "−" : "+") + s;
    return (v < 0 ? "−" : "") + s;
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

  function updateOrpModeControls() {
    const mode = getSegmented("orp_regulation_mode") || "automatic";
    const params = $("#orp-regulation-params");      // sous-bloc Automatique
    const scheduled = $("#orp-params-scheduled");    // sous-bloc Programmée
    const inject = $("#orp-inject-section");         // Injection manuelle

    if (params)    params.style.display    = mode === "automatic"  ? "" : "none";
    if (scheduled) scheduled.style.display = mode === "scheduled"  ? "" : "none";
    if (inject)    inject.style.display    = mode === "manual"     ? "" : "none";
    // feature-092 : miroir de la page pH — la carte « Débit d'injection ORP »
    // dépend du mode ET de la donnée, décidés en un seul endroit.
    updateOrpDosingCard();
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

    // feature-083 : lignes de consigne des graphiques détail pH/ORP — mise à
    // jour sans rechargement, quelle que soit la source du changement (UI, API,
    // MQTT) : chaque sauvegarde recharge /get-config ici, et les trames WS
    // "config" passent aussi par loadConfig. setTarget ignore les valeurs
    // absentes/non finies et ne re-rend que si la consigne a changé.
    phChart?.setTarget(cfg.ph_target);
    orpChart?.setTarget(cfg.orp_target);

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
    // feature-155 : constantes de l'accord pH sur un test manuel (calPoints
    // minimal, délai minimal de filtration) — même convention que ci-dessus.
    _applyPhTuneConfig(cfg);
    // feature-156 (B11/A2) : bannière persistante « correction caduque ».
    renderPhOffsetStaleBanner(cfg);
    // feature-157 : ligne + bouton « Supprimer la correction ».
    renderPhTuneRemoveRow(cfg);
    buildPhTargetSegments();
    renderPhTargetHint();
    // feature-154 : seuils de SANTÉ pH (pente/zéro) — distincts des bornes de
    // réglage ci-dessus. Alimente les 3 pastilles du modal + le chip global.
    _applyPhHealth(cfg);

    // Consigne pH — écriture SILENCIEUSE (pas de dispatch) : lire la config ne
    // doit jamais marquer le formulaire modifié. B7 : consigne absente ou
    // invalide → borne basse ; « aucune position sélectionnée » est interdit.
    if (!_phControlBusy("#ph_target_group", "#ph_regulation_save_btn")) {
      setPhTarget(typeof cfg.ph_target === "number" ? cfg.ph_target : _phBounds.targetMin, true);
    }
    $("#orp_target").value = typeof cfg.orp_target === "number" ? cfg.orp_target : 650;

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
    // feature-091 : plus de champ « délai avant injection » — l'attente après
    // un arrêt d'eau est fixe (firmware) ; la ligne « Dernière pause » est
    // nourrie par latestSensorData (updateMeasuresPurgeDisplay), pas par la config.
    // feature-090 : réinjection des champs absents d'une config partielle
    // (pool_size, regulation_defaults_by_size, regulation_speed) + sélecteur de
    // taille + visibilité taille/vitesse — AVANT le setSegmented de la vitesse,
    // pour que celui-ci lise la valeur réinjectée et jamais le défaut "normal".
    syncPoolSize(cfg);
    setSegmented("regulation_speed", cfg.regulation_speed || "normal");
    // feature-088 : cache des valeurs d'usine + visibilité du bouton de restauration
    syncRegulationDefaults(cfg);
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

  // Règle de composition du badge multi-états (feature-012 N2, revue chantier
  // simplification — retour utilisateur : « les messages contextuels sont en
  // bas, jamais en haut. La couleur du badge dépendra du niveau d'alerte »).
  // Chaque état candidat est décrit par { priority, variant, text }.
  // ⚠️ PLUS DE PLAFOND sur ce qui compte : couper à 2 segments était un choix
  // de densité, pas de sécurité — masquer une alerte quand plusieurs choses
  // vont mal en même temps est exactement le défaut qu'on traque.
  //   - TOUS les segments 'danger'/'warning' s'affichent, empilés, jamais coupés.
  //   - Un SEUL segment 'info' (neutre), le plus prioritaire, UNIQUEMENT si
  //     aucun danger/warning n'existe pour ce capteur.
  //   - Rien à signaler ⇒ badge masqué (comme avant).
  function composeSensorBadge(candidates) {
    const bySeverity = (a, b) => a.priority - b.priority;
    // Revue code-reviewer : condition EXPLICITE (pas `!== 'info'`) — un
    // `variant` absent ne doit jamais compter comme alerte par accident.
    const alerts = candidates.filter(s => s.variant === 'danger' || s.variant === 'warning').sort(bySeverity);
    const neutrals = candidates.filter(s => s.variant === 'info').sort(bySeverity);
    const kept = alerts.length ? alerts : neutrals.slice(0, 1);
    if (!kept.length) return { variant: null, segments: [] };
    const variant = kept.some(s => s.variant === 'danger') ? 'danger'
      : kept.some(s => s.variant === 'warning') ? 'warning' : 'info';
    // `quiet` (revue code-reviewer, accessibilité) : le décompte d'injection
    // se réécrit à la seconde (dosingCountdownSegment) — s'il est annoncé par
    // le aria-live du badge à cette cadence, c'est une ré-annonce hostile à
    // chaque tick. setBadge() l'exclut de la région active via aria-hidden.
    return { variant, segments: kept.map(s => ({ text: s.text, quiet: !!s.quiet })) };
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
  // Recalculés à chaque push WS, AUCUN tick JS seconde. `sensor` réservé pour
  // d'éventuels textes différenciés.
  // feature-139 (v2.62.0, correctif code-reviewer 2026-10-08, D7) : le texte
  // appuyé sur la durée ÉCOULÉE (« Stabilisation depuis N min ») a été retiré
  // — il ne décrit plus rien (plus d'analyse de stabilité dans cet appareil)
  // et c'est exactement le vocabulaire que l'utilisateur a dit ne plus
  // comprendre. Remplacé par `_measuresWaitText(data)` : MÊME source que le
  // bandeau de blocage / la pastille (code 18), pour qu'un seul endroit du
  // code puisse se tromper sur ce délai, pas trois.
  // `water_present === true` est garanti dans cette branche (cf. condition
  // ci-dessous) : la sous-branche « filtration arrêtée » de
  // _measuresWaitText() ne peut pas s'y déclencher, aucune contradiction.
  function measureStateSegments(sensor, data) {
    if (!data) return []; // boot : pas de faux positif avant le 1er message WS
    if (data.water_present === false) {
      // Signal externe périmé = anomalie (warning) ; arrêt franc = info
      return data.filtration_state_stale
        ? [{ priority: 1, variant: 'warning', text: 'Signal filtration périmé' }]
        : [{ priority: 1, variant: 'info', text: 'Filtration arrêtée' }];
    }
    if (data.water_present === true && data.measures_valid === false) {
      // Le plafond 90 min (motif 2) OUVRE la porte (measures_valid passe à true) :
      // il n'a donc jamais sa place ici. Revue v2.34.0 : une branche « non
      // atteinte » testée ici ne se déclenchait qu'à la purge SUIVANTE, tant
      // que le motif 2 restait mémorisé — texte faux.
      // feature-139 (v2.62.0) : le motif 2 n'est plus JAMAIS émis par le
      // firmware (code réservé, non réattribué) — cette branche reste
      // défensive mais ne se déclenche plus en pratique. Le bandeau dédié
      // #measures_purge_timeout_banner[_orp] a été retiré (alerte MQTT
      // measures_purge_timeout supprimée côté firmware).
      return [{ priority: 1, variant: 'info', text: _measuresWaitText(data) }];
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

  // Texte « Trop bas/élevé » — factorisé (pH et ORP ne diffèrent que par la
  // tolérance, l'arrondi et l'unité).
  function outOfRangeText(value, target, tol, round, unit) {
    if (value == null || isNaN(value)) return null;
    const v = round(value), lo = round(target - tol), hi = round(target + tol);
    if (v < lo) return `Trop bas (${v}${unit})`;
    if (v > hi) return `Trop élevé (${v}${unit})`;
    return null;
  }

  // Revue code-reviewer (facteur commun pH/ORP — les deux blocs d'appel
  // étaient un quasi copier-coller d'une trentaine de lignes) : limites
  // (journalière/durée/horaire/cycles), règle de COMBINAISON (limite atteinte
  // ET mesure hors plage sur le MÊME capteur ⇒ danger — sinon warning), et le
  // reste de la classification firmware DOSE_BLOCK_CODES non encore sourcé
  // ailleurs.
  function dosingLimitSegments(sensor, rangeText) {
    const data = latestSensorData || {};
    const segs = [];

    const dailyLimit = !!data[`${sensor}_limit_reached`];
    const block = _doseBlockInfo(data, sensor);
    const hourlyLimit = block.code === 11;
    const cyclesLimit = block.code === 12;
    // Revue code-reviewer : un calcul client « durationLimit » (config
    // `*_limit_minutes` + `*_used_ms`) a existé ici — RETIRÉ, pas neutralisé.
    // Vérifié côté firmware (src/dosing_logic.cpp:173 :
    // `in.hourlyLimitMs > 0 && in.usedMs >= in.hourlyLimitMs`) : c'est
    // EXACTEMENT le même test que le code 11 (hourlyLimit ci-dessus), déjà
    // servi des DEUX côtés /data et WS. Le garder aurait affiché DEUX
    // segments pour UN seul fait (« Limite de durée atteinte » ET « Limite
    // horaire atteinte ») — le doublon que DOSE_BLOCK_CODE_ALREADY_SOURCED
    // existe pour interdire. Bénéfice en prime : plus de lecture de
    // `*_used_ms`, le champ non parié /data↔WS qui motivait le correctif
    // précédent — le contournement disparaît avec sa cause.
    const anyLimit = dailyLimit || hourlyLimit || cyclesLimit;
    const alertVariant = (rangeText && anyLimit) ? 'danger' : 'warning';

    if (rangeText) segs.push({ priority: 1, variant: alertVariant, text: rangeText });
    // Source unique pour chaque fait de limite — un seul endroit le pousse.
    [[dailyLimit, 'Limite journalière atteinte'],
     [hourlyLimit, DOSE_BLOCK_CODES[11].label], [cyclesLimit, DOSE_BLOCK_CODES[12].label]]
      .forEach(([on, text]) => { if (on) segs.push({ priority: 2, variant: alertVariant, text }); });

    // Reste de la classification firmware (DOSE_BLOCK_CODES), faits non
    // encore sourcés ailleurs (DOSE_BLOCK_CODE_ALREADY_SOURCED) : watchdog,
    // calibration insuffisante, stabilisation post-cal, mélange en cours,
    // régulation non automatique, anti-rafale, sonde figée/muette, sécurité
    // pH/ORP active…
    // feature-148 (B1) : second garde-fou, orthogonal au Set ci-dessus — une
    // cause EFFECTIVEMENT portée par le bandeau du tableau de bord
    // (doseBlockBannerCarries(), lit le même plan pur que le bandeau) ne doit
    // pas être répétée ici, pour N'IMPORTE QUEL code, pas seulement ceux déjà
    // recensés dans le Set. Voir le commentaire du Set, quelques lignes plus
    // bas, pour la distinction entre les deux questions.
    if (block.label && !DOSE_BLOCK_CODE_ALREADY_SOURCED.has(block.code) && !doseBlockBannerCarries(sensor, data)) {
      const v = doseBlockVariant(block.tone, block.code);
      segs.push({ priority: (v === 'danger') ? 0.5 : 1.5, variant: v, text: block.label });
    }
    return segs;
  }

  // Facteur commun pH/ORP (revue code-reviewer) : assemble TOUS les segments
  // d'un capteur — mesure d'état, capteur indisponible, limites/combinaison
  // (dosingLimitSegments), cumul/stock, décomptes. `dailyMlKey`/`maxMlKey`/
  // `remKey`/`thrKey` : seuls les NOMS DE CHAMP diffèrent entre pH et ORP (et
  // ne suivent pas tous le même gabarit, ex. max_chlorine_ml_per_day) — d'où
  // ces paramètres plutôt qu'un simple préfixe `${sensor}_`.
  function buildSensorSegments(sensor, target, tol, round, unit, dailyMlKey, maxMlKey, remKey, thrKey) {
    const segs = measureStateSegments(sensor, latestSensorData);
    const v = latestSensorData?.[sensor];
    const ok = latestSensorData !== null && measuresAreValid(latestSensorData);
    if (ok && (v == null || isNaN(v))) {
      segs.push({ priority: 0, variant: 'danger', text: 'Capteur indisponible' });
    }
    segs.push(...dosingLimitSegments(sensor, ok ? outOfRangeText(v, target, tol, round, unit) : null));
    const cumul = dailyCumulSegment(latestSensorData?.[dailyMlKey], window._config?.[maxMlKey] ?? 0,
      latestSensorData?.[`${sensor}_limit_reached`] === true);
    if (cumul) segs.push(cumul);
    const rem = latestSensorData?.[remKey], thr = latestSensorData?.[thrKey];
    if (rem != null && thr != null && rem <= thr && thr > 0) {
      segs.push({ priority: 4, variant: 'warning', text: `Stock faible (${(rem / 1000).toFixed(1)} L)` });
    }
    // Décompte d'injection — relocalisé tel quel (segment neutre, faible
    // priorité). Décompte stabilisation/pause mélange — RESTAURÉ tel quel
    // (coordinateur) : pas de libellé figé, un décompte ne se déduit de rien.
    const countdown = dosingCountdownSegment(sensor);
    const stabOrMix = dosingStabOrMixSegment(sensor);
    _dosingCountdownShown[sensor] = !!(countdown?.live || stabOrMix?.live);
    if (countdown) segs.push(countdown);
    if (stabOrMix) segs.push(stabOrMix);
    return segs;
  }

  function updateSensorBadges() {
    // Chantier simplification : rendu en LISTE de <span> (un par segment),
    // plus une seule chaîne jointe par « · » (illisible à 320 px dès deux
    // textes longs). `.sensor-badge--stacked` (empilement vertical) actif
    // seulement à partir de 2 segments.
    // Revue code-reviewer :
    // [SÉCURITÉ] `segments[].text` peut porter le texte BRUT du firmware
    // (_doseBlockInfo() renvoie json.*DoseBlockedReason tel quel quand le
    // code est absent, ou le code-garde 250) — `textContent` uniquement,
    // JAMAIS `innerHTML` (même garantie que l'ancien `el.textContent = stopReason`).
    // [ACCESSIBILITÉ] Mise à jour INCRÉMENTALE des <span> existants, pas de
    // clear+rebuild : le décompte d'injection réécrit SON span à la seconde
    // (aria-hidden, cf. `quiet`) — reconstruire tout le sous-arbre à chaque
    // tick réannoncerait le badge ENTIER via aria-live, quiet ou pas.
    function setBadge(id, variant, segments) {
      const el = $(`#${id}`);
      if (!el) return;
      if (!variant || !segments || !segments.length) {
        el.style.display = 'none';
        el.className = 'sensor-badge';
        el.innerHTML = '';
        return;
      }
      el.className = `sensor-badge sensor-badge--${variant}` +
        (segments.length > 1 ? ' sensor-badge--stacked' : '');
      const kids = el.children;
      segments.forEach((seg, i) => {
        const span = kids[i] || el.appendChild(document.createElement('span'));
        if (span.textContent !== seg.text) span.textContent = seg.text;
        if (seg.quiet) span.setAttribute('aria-hidden', 'true');
        else span.removeAttribute('aria-hidden');
      });
      while (el.children.length > segments.length) el.lastChild.remove();
      el.style.display = '';
    }

    // feature-061 : mesures invalides (filtration arrêtée / stabilisation) —
    // gate les segments température « Capteur indisponible »/« figée » plus
    // bas (ph/orp : même gate, désormais interne à buildSensorSegments()).
    const measuresOk = measuresAreValid(latestSensorData);

    // Badge pH — assemblé par buildSensorSegments() (factorisé avec ORP)
    const phSegments = buildSensorSegments('ph', window._config?.ph_target ?? 7.2, 0.2, v => +v.toFixed(1), '',
      'ph_daily_ml', 'max_ph_ml_per_day', 'ph_remaining_ml', 'ph_alert_threshold_ml');
    const phBadge = composeSensorBadge(phSegments);
    setBadge('ph-sensor-badge', phBadge.variant, phBadge.segments);

    // Badge ORP — même assemblage. Règle de combinaison : cf. commentaire de
    // dosingLimitSegments() — c'est elle qui romprait le silence de la dérive
    // ORP en cours (510-540 mV stable jusqu'au 28/09, puis décroissance
    // monotone jusqu'à 74 mV, minimum −46 : jamais « Capteur indisponible »,
    // un « Trop bas » orange pendant dix jours sans cette règle).
    const orpSegments = buildSensorSegments('orp', window._config?.orp_target ?? 650, 150, Math.round, ' mV',
      'orp_daily_ml', 'max_chlorine_ml_per_day', 'orp_remaining_ml', 'orp_alert_threshold_ml');
    const orpBadge = composeSensorBadge(orpSegments);
    setBadge('orp-sensor-badge', orpBadge.variant, orpBadge.segments);

    // Démarrage/arrêt du ticker 1 s — une seule fois par rendu, après les deux
    // capteurs (plusieurs décomptes indépendants peuvent être « live » à la fois).
    if (_dosingCountdownShown.ph || _dosingCountdownShown.orp) {
      if (!dosingTickTimer) dosingTickTimer = setInterval(_dosingTick, 1000);
    } else if (dosingTickTimer) {
      clearInterval(dosingTickTimer);
      dosingTickTimer = null;
    }

    // Badge Température — capteur indisponible (bug-ui-valeurs-figees) + segments
    // d'état mesure (feature-061 it. 2), calqué sur les blocs pH/ORP ci-dessus
    // (mêmes gardes anti-faux-positif : boot et mesures invalides).
    const tempSegments = [];
    tempSegments.push(...measureStateSegments('temperature', latestSensorData));
    if (latestSensorData !== null && measuresOk && (latestSensorData.temperature == null || isNaN(latestSensorData.temperature))) {
      tempSegments.push({ priority: 0, variant: 'danger', text: 'Capteur indisponible' });
    }
    // ---- feature-068 (B1) : état de la sonde de température d'EAU ----
    // Trois états DISTINCTS, et l'ordre entre eux est l'arbitrage du verdict
    // pool-chemistry du 07/09/2026, pas un détail de présentation :
    //   1. valeur absente ou invalide → « Capteur indisponible » (segment
    //      ci-dessus, priorité 0). PRIME sur « figée », étant plus sévère : une
    //      sonde qui ne mesure plus rien ne peut pas être « figée ». C'est aussi
    //      ce qui empêche un badge « figée » de rester à l'écran après un
    //      débranchement en cours d'épisode.
    //   2. sonde eau non identifiée   → on n'affiche JAMAIS « eau figée » : les
    //      deux états ne sont pas actionnables de la même façon (ici, aller
    //      identifier la sonde ; là, remplacer le capteur).
    //   3. figée                      → informatif, uniquement quand une sonde
    //      « eau » est bien identifiée et rend une valeur.
    //
    // POURQUOI `sondes_identified` ET NON `temperature_raw`, qui serait le
    // discriminant naturel : `temperature_raw` est WS-SEUL (dette déclarée dans
    // la sentinelle de parité). S'en servir ici aurait rejoué le piège n° 1 du
    // projet — `latestSensorData` est REMPLACÉ en entier par chaque charge /data,
    // le champ y disparaîtrait toutes les ~30 s, et le badge aurait CLIGNOTÉ
    // entre « figée » et « non identifiée ». `sondes_identified` et
    // `sondes_detected` sont servis des DEUX côtés : la condition retenue est
    // exactement celle de la pastille #sondes-chip existante (identification
    // incomplète avec au moins une sonde sur le bus), donc l'appareil dit déjà
    // la même chose au même moment, au même endroit.
    // LIMITE ASSUMÉE : `sondes_identified` couvre les DEUX rôles (eau ET
    // circuit). Dans une installation où seule la sonde « circuit » manque, la
    // sonde « eau » fonctionne et le badge « figée » sera SUPPRIMÉ à tort. C'est
    // un défaut CONSERVATEUR — on perd un badge informatif, on n'en invente pas
    // un faux — et le sens inverse est impossible : le rôle « eau » non assigné
    // rend toujours `sondes_identified` faux.
    const sondesIdentified = latestSensorData?.sondes_identified;
    const sondesDetected = latestSensorData?.sondes_detected;
    const waterRoleUnknown = sondesIdentified === false &&
      typeof sondesDetected === 'number' && sondesDetected >= 1;
    const tempValued = latestSensorData?.temperature != null && !isNaN(latestSensorData.temperature);
    if (latestSensorData !== null && measuresOk && waterRoleUnknown && tempValued) {
      tempSegments.push({ priority: 0, variant: 'danger', text: 'Sonde eau non identifiée' });
    }
    // « Sonde température figée » — WARNING et non DANGER, volontairement :
    // l'erreur de pH induite par une compensation figée a été chiffrée à
    // ≈ 0,001 pH, soit 2 à 3 ordres de grandeur sous la bande morte de la
    // régulation. Rien n'est inhibé ici, et rien ne doit l'être : le badge
    // informe, il ne déclenche pas. Le gate `measuresOk` évite le faux positif
    // évident — filtration arrêtée, eau immobile dans la canalisation, la sonde
    // peut légitimement rendre la même valeur pendant une demi-heure ; c'est la
    // même garde que les badges pH/ORP ci-dessus.
    if (measuresOk && !waterRoleUnknown && tempValued &&
        latestSensorData?.tempFrozenAlarm === true) {
      tempSegments.push({ priority: 1, variant: 'warning', text: 'Sonde température figée' });
    }
    const tempBadge = composeSensorBadge(tempSegments);
    setBadge('temperature-sensor-badge', tempBadge.variant, tempBadge.segments);
    // Page Température : MÊME composition que la carte du tableau de bord. Les
    // segments de dosage/stock n'existent pas pour la température, il n'y a donc
    // rien à retirer (contrairement aux pages pH/ORP, qui n'affichent que les
    // segments d'état mesure).
    setBadge('temperature-page-badge', tempBadge.variant, tempBadge.segments);

    // feature-061 : badges des pages pH/ORP — alimentés par les SEULS segments
    // d'état mesure (pas les segments dosage/stock, déjà portés ailleurs sur la page).
    const phPageBadge = composeSensorBadge(measureStateSegments('ph', latestSensorData));
    setBadge('ph-page-badge', phPageBadge.variant, phPageBadge.segments);
    const orpPageBadge = composeSensorBadge(measureStateSegments('orp', latestSensorData));
    setBadge('orp-page-badge', orpPageBadge.variant, orpPageBadge.segments);
  }

  // ========== ÉCRAN PRODUITS ==========

  // feature-110 : mise en forme du RELEVÉ (V, N). Une valeur absente rend
  // « -- » et JAMAIS 0 : au changement de bidon, un zéro affiché à la place
  // d'une valeur manquante serait relevé comme un vrai zéro, et fausserait la
  // conclusion sur la nature du biais de comptage.
  function fmtInjectedMl(v) {
    const n = Number(v);
    if (v == null || !isFinite(n)) return '--';
    return Math.round(n).toLocaleString('fr-FR') + ' mL';
  }

  function fmtPumpStarts(v) {
    const n = Number(v);
    if (v == null || !isFinite(n)) return '--';
    return Math.round(n).toLocaleString('fr-FR');
  }

  function updateProductUI(data) {
    function updateCard(prefix, remainingMl, containerMl, thresholdMl) {
      // feature-143 : `containerMl` absent (donnée non servie) ne doit JAMAIS
      // produire un chiffre inventé — `--` et barre à 0 %, c'est tout.
      const hasContainer = containerMl != null && containerMl > 0;
      const pct = hasContainer ? Math.max(0, Math.min(100, (remainingMl / containerMl) * 100)) : 0;
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
      if (totEl) totEl.textContent = hasContainer ? (containerMl / 1000).toFixed(1) : '--';

      if (status) {
        const isLow = thresholdMl > 0 && remainingMl <= thresholdMl;
        status.textContent = isLow ? 'Stock faible' : 'OK';
        status.className = 'state-badge ' + (isLow ? 'state-badge--warn' : 'state-badge--on');
      }
    }

    if (data.ph_remaining_ml != null) {
      updateCard('ph', data.ph_remaining_ml, data.ph_container_ml, data.ph_alert_threshold_ml ?? 2000);
    }
    if (data.orp_remaining_ml != null) {
      updateCard('orp', data.orp_remaining_ml, data.orp_container_ml, data.orp_alert_threshold_ml ?? 2000);
    }

    // feature-110 : relevé (V, N). Ces deux clés ne sont PAS dans la trame
    // capteurs (seule la trame « config » et /get-config les portent) : on ne
    // touche donc au DOM que si la clé est présente, sinon chaque trame
    // capteurs effacerait le relevé une seconde après son affichage.
    ['ph', 'orp'].forEach((prefix) => {
      const injected = data[`${prefix}_total_injected_ml`];
      const starts = data[`${prefix}_starts`];
      if (injected != null) {
        const el = $(`#product-${prefix}-injected`);
        if (el) el.textContent = fmtInjectedMl(injected);
      }
      if (starts != null) {
        const el = $(`#product-${prefix}-starts`);
        if (el) el.textContent = fmtPumpStarts(starts);
      }
    });
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
    // feature-143 : le champ de niveau (`*_level_l`) n'est JAMAIS pré-rempli
    // depuis `cfg.*_container_ml` — le pré-remplir rechargerait le malentendu
    // « il y a déjà un nombre, il doit être bon ». Il reste vide, placeholder
    // seul, à chaque affichage.
    const phAlert = $(`#ph_alert_threshold_l`);
    const orpAlert = $(`#orp_alert_threshold_l`);
    if (phAlert && cfg.ph_alert_threshold_ml != null) phAlert.value = (cfg.ph_alert_threshold_ml / 1000).toFixed(1);
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
      orp_alert_threshold_ml: cfg.orp_alert_threshold_ml,
      // feature-110 : relevé (V, N) — présent dans /get-config et dans la
      // trame WS « config », absent de la trame capteurs.
      ph_total_injected_ml: cfg.ph_total_injected_ml,
      ph_starts: cfg.ph_starts,
      orp_total_injected_ml: cfg.orp_total_injected_ml,
      orp_starts: cfg.orp_starts
    });
  }

  function setupProductScreen() {
    // Chantier simplification 2026-10-09 : le bouton « Enregistrer » disparaît
    // au profit d'un autosave au `change` du champ. C'est un ÉCART ASSUMÉ au
    // motif du projet (bindAutosave(), app.js ~L.11068, exclut explicitement
    // les champs numériques : « pas d'auto-save, bouton Sauvegarder
    // uniquement ») — pas une mise en cohérence avec lui. L'écart est justifié
    // ici par la nature du réglage : un seuil d'alerte n'a aucune conséquence
    // immédiate (contrairement à une consigne de régulation), le risque d'un
    // envoi non désiré est donc négligeable. Retour visuel porté par le champ
    // (.field.is-valid / .field__success), pas par un toast.
    async function saveProduct(prefix) {
      const input = $(`#${prefix}_alert_threshold_l`);
      const fieldEl = input?.closest('.field');
      const alertL = parseFloat(input?.value);
      if (isNaN(alertL)) {
        showToast('Valeur invalide', 'error');
        if (fieldEl) { fieldEl.classList.remove('is-valid'); fieldEl.classList.add('is-error'); }
        return;
      }
      const payload = {
        [`${prefix}_alert_threshold_ml`]: alertL * 1000
      };
      const ok = await sendConfig(payload);
      if (ok) {
        if (fieldEl) {
          fieldEl.classList.remove('is-error');
          fieldEl.classList.add('is-valid');
          clearTimeout(fieldEl._successTimer);
          fieldEl._successTimer = setTimeout(() => fieldEl.classList.remove('is-valid'), 2000);
        }
        loadConfig().catch(() => {});
      } else {
        if (fieldEl) { fieldEl.classList.remove('is-valid'); fieldEl.classList.add('is-error'); }
        showToast('Erreur lors de la sauvegarde', 'error');
      }
    }

    // Rafale d'écritures flash évitée : un input[type=number] émet un `change`
    // PAR PAS quand on utilise les flèches ↑↓ ou le spinner tactile — sans
    // debounce, chaque clic déclencherait un POST /save-config et une écriture
    // flash. 600 ms : l'utilisateur a le temps d'enchaîner plusieurs clics
    // avant le premier envoi.
    const _productSaveTimers = {};
    function debounceSaveProduct(prefix) {
      clearTimeout(_productSaveTimers[prefix]);
      _productSaveTimers[prefix] = setTimeout(() => saveProduct(prefix), 600);
    }

    // feature-110 : le couple (volume injecté, démarrages pompe) ne sert qu'à la
    // mise à jour du niveau — et c'est ce geste même qui le détruit. La
    // confirmation le présente donc à l'instant exact où il va disparaître,
    // sans rien à mémoriser au préalable. Retourne { node, text } ; `text` doit
    // porter TOUTES LES VALEURS EN CLAIR, car c'est le seul contenu affiché par
    // le repli window.confirm() des navigateurs sans <dialog>.
    // feature-143 : `levelL` est la nouvelle ligne de tête — le niveau qui va
    // être posé, affiché AVANT le relevé qu'il efface.
    function _buildProductResetSummary(prefix, cfg, levelL) {
      const vTxt = fmtInjectedMl(cfg[`${prefix}_total_injected_ml`]);
      const nTxt = fmtPumpStarts(cfg[`${prefix}_starts`]);
      const anomN = Number(cfg[`${prefix}_anom`] ?? 0);
      const anomMl = Number(cfg[`${prefix}_anom_ml`] ?? 0);
      const levelTxt = Number(levelL).toLocaleString('fr-FR', { minimumFractionDigits: 1, maximumFractionDigits: 1 });

      const node = document.createElement('div');
      const lines = [];

      const levelP = document.createElement('p');
      levelP.style.margin = '0 0 10px';
      const levelStrong = document.createElement('strong');
      levelStrong.textContent = `Nouveau niveau : ${levelTxt} L`;
      levelP.appendChild(levelStrong);
      node.appendChild(levelP);
      lines.push(levelStrong.textContent, '');

      const intro = document.createElement('p');
      intro.style.margin = '0 0 10px';
      intro.textContent = "Dernière chance de noter ces valeurs : elles seront remises à zéro.";
      node.appendChild(intro);
      lines.push(intro.textContent, '');

      // Mêmes libellés que la carte : le relevé doit se reconnaître d'un coup d'œil.
      const kv = document.createElement('div');
      kv.className = 'kv kv--compact';
      [['Volume injecté', vTxt], ['Démarrages pompe', nTxt]].forEach(([k, v]) => {
        const row = document.createElement('div');
        row.className = 'kv__row';
        const kEl = document.createElement('span');
        kEl.className = 'kv__k';
        kEl.textContent = k;
        const vEl = document.createElement('span');
        vEl.className = 'kv__v';
        vEl.textContent = v;
        row.appendChild(kEl);
        row.appendChild(vEl);
        kv.appendChild(row);
        lines.push(`${k} : ${v}`);
      });
      node.appendChild(kv);

      // Hygiène du relevé : affichée UNIQUEMENT si elle est non nulle. Un « 0 »
      // permanent apprendrait à ignorer la zone, et l'avertissement passerait
      // inaperçu le jour où il invalide vraiment le relevé.
      if (anomN > 0 || anomMl > 0) {
        const warn = document.createElement('div');
        warn.className = 'callout callout--warn';
        warn.textContent = `Relevé peu fiable : ${fmtPumpStarts(anomN)} démarrages (${fmtInjectedMl(anomMl)}) ont eu lieu hors comptage normal.`;
        node.appendChild(warn);
        lines.push('', warn.textContent);
      }

      const foot = document.createElement('p');
      foot.style.margin = '10px 0 0';
      foot.textContent = "Elles repartiront de zéro avec ce niveau comme point de départ.";
      node.appendChild(foot);
      lines.push('', foot.textContent);

      return { node, text: lines.join('\n') };
    }

    async function updateLevel(prefix) {
      const label = prefix === 'ph' ? 'pH' : 'Chlore';
      const levelL = parseFloat($(`#${prefix}_level_l`)?.value);
      if (isNaN(levelL) || levelL <= 0) {
        showToast('Indiquez une quantité valide, supérieure à 0 L.', 'error');
        return;
      }

      // Config FRAÎCHE obligatoire : window._config peut provenir d'une trame WS
      // « config » PARTIELLE, et les compteurs d'anomalie (*_anom, *_anom_ml) n'y
      // sont JAMAIS — ils sont HTTP-seuls. Sans ce rechargement, le relevé serait
      // incomplet à l'instant même où il est détruit.
      try {
        await loadConfig();
      } catch (e) {
        showToast('Impossible de lire la configuration avant la mise à jour — réessayez.', 'error');
        return;
      }

      const { node, text } = _buildProductResetSummary(prefix, window._config || {}, levelL);
      const ok = await confirmDialog({
        title: `Mettre à jour le niveau — ${label}`,
        message: text,
        messageNode: node,
        confirmLabel: 'Mettre à jour',
        danger: true
      });
      // Action IRRÉVERSIBLE : on ne repart que sur un « oui » explicite.
      if (ok !== true) return;

      const saved = await sendConfig({
        [`${prefix}_container_ml`]: levelL * 1000,
        [`${prefix}_reset_container`]: true
      });
      if (saved) {
        showToast(`Niveau ${prefix === 'ph' ? 'pH' : 'chlore'} mis à jour`, 'success');
        loadConfig().catch(() => {});
      } else {
        showToast('Erreur lors de la mise à jour', 'error');
      }
    }

    $('#ph_alert_threshold_l')?.addEventListener('change', () => debounceSaveProduct('ph'));
    $('#orp_alert_threshold_l')?.addEventListener('change', () => debounceSaveProduct('orp'));
    $('#product-ph-update-level')?.addEventListener('click', () => updateLevel('ph'));
    $('#product-orp-update-level')?.addEventListener('click', () => updateLevel('orp'));

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
    // feature-146 : mode Marche — la pompe tourne tant que l'utilisateur ne
    // change pas de mode, sans dépendre du créneau.
    if (mode === 'force') return true;
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
      // Parité avec isMinutesInRange(equalMeansAlways=false) côté firmware
      // (src/schedule_logic.cpp) : start==end signifie « jamais », pas « toujours ».
      if (startMin === endMin) return false;
      if (startMin < endMin) return nowMin >= startMin && nowMin < endMin;
      return nowMin >= startMin || nowMin < endMin;
    }
    return false;
  }

  function updateFiltrationBadges() {
    const config = window._config || {};
    const state = getFiltrationState(config, latestSensorData || {});
    const modeLabels = { auto: 'Auto', manual: 'Programme', force: 'Marche', off: 'Arrêt' };

    // Revue coordinateur 2026-10-09 : le mode dit ce qui est DEMANDÉ, l'état
    // dit ce que la pompe FAIT — les deux divergent légitimement (ex. Auto
    // hors créneau = mode Auto + pompe arrêtée, fonctionnement normal). Le
    // badge d'état (id/classes INCHANGÉS) est restauré dans l'en-tête, à côté
    // du texte de mode — les 4 boutons ne remplacent que les anciens
    // Démarrer/Arrêter, pas cette information.
    const dashBadge = $("#detail-filtration-status");
    if (dashBadge) {
      dashBadge.textContent = state.text;
      dashBadge.className = 'state-badge ' + state.class;
    }
    // Texte de MODE, toujours "Mode X" (jamais remplacé par l'état : le badge
    // ci-dessus porte déjà "Hors gel"). L'exception --warn est conservée en
    // renfort visuel sur ce texte pendant la pause anti-gel, sans dupliquer le
    // mot "Hors gel" une seconde fois.
    const dashLabel = $("#detail-filtration-mode");
    if (dashLabel) {
      dashLabel.textContent = 'Mode ' + (modeLabels[config.filtration_mode] || 'Auto');
      dashLabel.classList.toggle('card__status-mode--warn', state.frost);
    }
    // Card "Contrôle manuel" (page Filtration, hors périmètre feature-147) :
    // badge .pill placé dans .card__head — INCHANGÉ, mis à jour depuis le
    // MÊME `state` que le badge du tableau de bord ci-dessus (synchronisés).
    const headBadge = $("#filtration-current-status");
    if (headBadge) {
      headBadge.textContent = state.text;
      headBadge.className = 'pill ' + state.pillClass;
    }
    updateFiltrationModeButtons(config.filtration_mode);
  }

  // feature-147 : grille 2x2 de la carte Filtration (tableau de bord). Bouton
  // actif = btn--primary + aria-pressed="true" ; aucun bouton jamais
  // désactivé (en Arrêt, "Auto" est la sortie — feature-146).
  function updateFiltrationModeButtons(mode) {
    const activeMode = mode || 'auto';
    ['auto', 'manual', 'force', 'off'].forEach((m) => {
      const btn = $(`#filtration-mode-btn-${m}`);
      if (!btn) return;
      const active = m === activeMode;
      btn.classList.toggle('btn--primary', active);
      btn.classList.toggle('btn--ghost', !active);
      btn.setAttribute('aria-pressed', active ? 'true' : 'false');
    });
  }

  function getFiltrationState(config, data) {
    const isRunning = filtrationRunningOverride !== null ? filtrationRunningOverride : (data && data.filtration_running);
    const temp = data && data.temperature;

    if (!config) return { text: 'Chargement...', class: 'state-badge--off', pillClass: '', frost: false };

    // Température hors gel — `frost` est l'ÉTAT testé par les appelants (pas
    // le texte affiché, revue code-reviewer : un libellé reformulé un jour ne
    // doit pas casser silencieusement la logique qui en dépend).
    if (temp != null && temp < 5.0 && isRunning) {
      return { text: 'Hors gel', class: 'state-badge--warn', pillClass: 'mid', frost: true };
    }

    return {
      text: isRunning ? 'En marche' : 'Arrêtée',
      class: isRunning ? 'state-badge--ok' : 'state-badge--off',
      pillClass: isRunning ? 'ok' : 'bad',
      frost: false
    };
  }

  // Chantier de simplification (règle utilisateur : « les messages
  // contextuels sont en bas, jamais en haut. La couleur du badge dépendra du
  // niveau d'alerte ») — le widget séparé #<sensor>-dosing-status DISPARAÎT.
  // getDosingStopReason() et dedupDosingReason() sont SUPPRIMÉES : tous les
  // motifs qu'elles portaient migrent dans phSegments/orpSegments
  // (updateSensorBadges()), via measureStateSegments() (déjà en place),
  // les flags *_limit_reached (déjà en place) et la classification générique
  // DOSE_BLOCK_CODES (doseBlockVariant(), DOSE_BLOCK_CODE_ALREADY_SOURCED —
  // cf. définitions près de DOSE_BLOCK_CODES). Un seul emplacement, une seule
  // source par fait.
  // ⚠️ Simplification assumée en cours de route : les décomptes dynamiques
  // "Stabilisation : M min S s" / "Pause mélange : M min S s" (calculés ici
  // auparavant) sont remplacés par les libellés STATIQUES déjà portés par
  // DOSE_BLOCK_CODES (codes 7/8 : « Stabilisation post-calibration » /
  // « Mélange en cours ») — même fait, sans le compte à rebours à la seconde.
  // Seul LE décompte de l'INJECTION (ci-dessous, bug-dosage-fantome) est
  // explicitement préservé tel quel par la demande.
  //
  // bug-dosage-fantome it.3 : le segment distingue « session active »
  // (commandé) de « pompe alimentée » (pouls effectif). Décomptes recalculés
  // depuis dosingAnchor (valeur ancrée + horloge locale), JAMAIS extrapolés
  // au-delà de la fin de période : à l'échéance sans nouvelle donnée →
  // ellipsis, jamais de bascule optimiste vers « Injection en cours », jamais
  // 0 s ni négatif. UN SEUL setInterval 1 s partagé pH+ORP, actif uniquement
  // quand un décompte est affiché — relance désormais updateSensorBadges()
  // en entier (le segment vit dans le badge, plus dans un widget à part).
  let dosingTickTimer = null;
  const _dosingCountdownShown = { ph: false, orp: false };
  const DOSING_NBSP = " ";  // espace insécable avant l'unité (« 42 s »)

  const fmtDosingCountdown = (s) => {
    if (s >= 60) {
      const m = Math.floor(s / 60), sec = s % 60;
      return `${m}${DOSING_NBSP}min ${String(sec).padStart(2, "0")}${DOSING_NBSP}s`;
    }
    return `${s}${DOSING_NBSP}s`;
  };

  function _dosingTick() {
    updateSensorBadges();
  }

  // Segment NEUTRE de faible priorité (demande explicite : « ne supprime pas
  // le décompte du dosage, relocalise-le ») — même cascade de priorités que
  // l'ancien widget, simplement rendue comme candidat de composeSensorBadge()
  // au lieu d'un <div> à part.
  function dosingCountdownSegment(sensor) {
    const data = latestSensorData || {};
    const anchor = dosingAnchor;
    const elapsedMs = anchor ? Math.max(0, Date.now() - anchor.atMs) : 0;
    let text = null, countdownShown = false;

    // Priorité 1 : injection MANUELLE minutée (valeur ancrée à la réception).
    const manualBase = anchor ? anchor[sensor].manualS : (data[`${sensor}_inject_remaining_s`] ?? 0);
    const manualS = manualBase - Math.floor(elapsedMs / 1000);
    const session = data[`${sensor}_pulse_session`];
    const periodMs = data.pulse_period_ms;
    if (manualBase > 0 && manualS > 0) {
      text = "Injection manuelle : " + fmtDosingCountdown(manualS);
      countdownShown = true;
    } else if (session === true && Number.isFinite(periodMs) && periodMs > 0) {
      // Priorités 2-4 : session d'impulsions (dosage en proportionnel de temps).
      // Phase locale = phase ancrée + temps écoulé, clampée à la fin de période.
      const pulseMs = data[`${sensor}_pulse_ms`] ?? 0;
      const phaseMs = Math.min((anchor ? anchor[sensor].phaseMs : 0) + elapsedMs, periodMs);
      const endS = Math.ceil((periodMs - phaseMs) / 1000);  // jamais 0 tant que phase < période
      if (pulseMs > 0 && phaseMs < pulseMs) {
        // Priorité 2 : pouls en cours — la pompe est réellement alimentée.
        text = "Injection en cours";
        countdownShown = true;
      } else if (pulseMs > 0) {
        // Priorité 3 : repos entre deux pouls — décompte jusqu'à la fin de période.
        text = endS > 0 ? "Prochaine impulsion : " + fmtDosingCountdown(endS) : "Prochaine impulsion…";
        countdownShown = endS > 0;
      } else {
        // Priorité 4 : deadband — période sans pouls planifié (demande < pouls min).
        text = endS > 0 ? "Dosage en attente : " + fmtDosingCountdown(endS) : "Dosage en attente…";
        countdownShown = endS > 0;
      }
    } else if (session === undefined && data[`${sensor}_dosing`]) {
      // Rétrocompat firmware sans champs pulse_* : comportement historique.
      text = "Injection en cours";
    }

    // quiet: countdownShown — SEUL le texte qui change réellement à la
    // seconde (revue code-reviewer) doit être exclu de l'annonce aria-live
    // (setBadge) ; un `quiet: true` inconditionnel rendrait aussi muets les
    // états qui NE tickent pas (« Prochaine impulsion… », « Injection en
    // cours » en rétrocompat sans pulse_*), au pire cas seuls à l'écran.
    return text ? { priority: 9, variant: 'info', text, live: countdownShown, quiet: countdownShown } : null;
  }


  // RESTAURÉ sur décision du coordinateur (le lot précédent l'avait remplacé
  // par les libellés statiques DOSE_BLOCK_CODES[7]/[8] — refusé). Décompte à
  // la seconde, PAS un libellé figé : c'est précisément le défaut corrigé ce
  // matin sur le code 15 (« un message sans fin annoncée ne vaut rien »),
  // et la demande la plus constante de l'utilisateur. Segment NEUTRE de
  // faible priorité (ux-ui-designer : « relocalisé tel quel, pas supprimé,
  // pas tranché pour un futur chantier »). Texte et calcul INCHANGÉS depuis
  // l'ancien getDosingStopReason().
  // live:false sur les 3 retours — fidèle à l'ancien comportement : ce
  // décompte n'a JAMAIS été ancré sur l'horloge locale (pas de dosingAnchor
  // ici, à la différence de l'injection) ; il affiche la dernière valeur
  // serveur, rafraîchie au prochain /data ou push WS, pas à la seconde.
  function dosingStabOrMixSegment(sensor) {
    const data = latestSensorData || {};
    const fmt = (label, secs) => {
      const m = Math.floor(secs / 60), s = secs % 60;
      return (m > 0 ? `${label} : ${m} min ${String(s).padStart(2, '0')} s` : `${label} : ${s} s`);
    };
    // feature-012 N3 : stabilisation PAR POMPE — fallback sur le champ global
    // (firmware antérieur sans champs par pompe).
    const remainS = data[`${sensor}_stab_remaining_s`] ?? (data.stabilization_remaining_s || 0);
    if (remainS > 0) return { priority: 0.8, variant: 'info', text: fmt('Stabilisation', remainS), live: false };
    // Pause mélange hydraulique post-injection (feature-025) ; repli sur le
    // booléen *MixingDelayActive (firmware plus ancien, sans temps restant).
    const mixS = data[`${sensor}_mix_remaining_s`] || 0;
    if (mixS > 0) return { priority: 0.8, variant: 'info', text: fmt('Pause mélange', mixS), live: false };
    if (data[`${sensor}MixingDelayActive`] === true) {
      return { priority: 0.8, variant: 'info', text: 'Pause mélange en cours', live: false };
    }
    return null;
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
    // feature-146 : le Boost surchlore l'eau qui circule — impossible en mode
    // Arrêt (seul mode exclu ; Auto/Programme/Marche l'autorisent tous).
    // Miroir client de config.cpp:startBoost() (revue code-reviewer) : en
    // Powered/External (feature-056) le mode de filtration est INERTE — l'eau
    // circule en permanence hors du contrôle du PoolController, un "off"
    // résiduel ne doit donc pas griser le Boost à tort.
    const installMode = cfg.install_mode || 'managed';
    const filtrationOff = installMode === 'managed' && (cfg.filtration_mode || '').toLowerCase() === 'off';
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
        if (filtrationOff) {
          hintText = "Indisponible en mode Arrêt : sans circulation, la surchloration ne peut pas se répartir dans le bassin.";
        } else if (filtExt && chlorBoost)       hintText = "À l'activation : filtration prolongée + cible ORP relevée jusqu'à minuit.";
        else if (!filtExt && chlorBoost) hintText = "À l'activation : cible ORP relevée jusqu'à minuit (filtration non gérée par le contrôleur).";
        else if (filtExt && !chlorBoost) hintText = "À l'activation : filtration prolongée jusqu'à minuit (régulation ORP non automatique, chlore non relevé).";
        else                             hintText = "À l'activation : aucun effet (filtration non gérée par le contrôleur et régulation ORP non automatique).";
        hintEl.textContent = hintText;
        hintEl.style.display = "";
      }
      if (startBtn) {
        startBtn.style.display = "";
        startBtn.disabled = filtrationOff;
      }
      if (stopBtn) stopBtn.style.display = "none";
    }
  }

  // feature-140 : sous-bloc Chloration de secours du widget « Modes
  // temporaires ». Consomme rescue_until/rescue_dosing (parité /data ↔ WS,
  // trame sensor_data) et orp_regulation_mode (trame WS "config" / /get-config,
  // via window._config — PAS latestSensorData, ce champ n'est publié que là).
  // Rétrocompat : champs absents (firmware ancien) ⇒ secours traité inactif,
  // aucune erreur affichée (même patron que boost_filtration_extended !== false).
  //
  // ⚠️ LIBELLÉS L1/L2/L3/L4/L5/L6/L7/L8 IMPOSÉS MOT À MOT par le gate
  // pool-chemistry (feature-140) — ne JAMAIS les reformuler, abréger ou
  // tronquer sans repasser ce gate. Précédent feature-138 : une version
  // tronquée avait perdu l'avertissement sur les chloramines.
  // rescue_lift (0-3, parité /data ↔ WS) remplace l'ancienne heuristique de
  // transition active→inactive : 1 = levée automatique (30 min de mesures
  // fiables, SEUL cas où L6 est vrai) ; 2 = expiration naturelle des 48h ;
  // 3 = arrêt manuel (bouton, MQTT, bascule Manuel). Fenêtre glissante de 90 s
  // côté firmware (3× la cadence /data) : ce compteur mémorise la dernière
  // valeur déjà annoncée pour ne toaster qu'une fois par événement.
  let _rescueLastAnnouncedLift = 0;

  // Volume quotidien EFFECTIF de secours, reflet client de la condition C3 du
  // gate pool-chemistry (feature-140/141) : min(cfg.rescue_ml, limite
  // utilisateur si > 0, 2000). feature-141 : la source n'est plus la
  // constante kRescueChlorineDailyMl mais le volume réellement ARMÉ, exposé
  // en parité stricte /get-config ↔ trame WS "config" (cfg.rescue_ml). Sans
  // ce calcul, une limite utilisateur inférieure au volume armé rendrait L5
  // inatteignable côté client alors que le firmware plafonne déjà en dessous
  // — angle mort que ce widget existe pour fermer. ⚠️ Les LIBELLÉS L5a/L5b
  // restent IMPOSÉS mot à mot : seuls le SEUIL de déclenchement et le volume
  // affiché sont dérivés ici, jamais le texte lui-même.
  function _rescueEffectiveDailyMl(cfg) {
    // feature-141 (correctif B6) : ce `let vol = 1500;` était un VRAI BUG — la
    // fonction alimente le bandeau actif, recalculé à chaque /data, pas
    // seulement à l'ouverture du dialogue. Correct tant que le volume armé
    // valait exactement 1500 ; faux dès qu'il devient variable (bassin
    // « petit », ou saisie > 1500). Repli 1500 UNIQUEMENT si cfg.rescue_ml est
    // absent (rétrocompat firmware ancien) — jamais une valeur qui prétend
    // remplacer le clamp serveur.
    let vol = (typeof cfg.rescue_ml === 'number' && cfg.rescue_ml > 0) ? cfg.rescue_ml : 1500;
    const userLimit = cfg.max_orp_ml_per_day;
    if (typeof userLimit === 'number' && userLimit > 0 && userLimit < vol) vol = userLimit;
    const hardCap = 2000; // miroir de kRescueChlorineDailyHardCapMl (constants.h)
    if (vol > hardCap) vol = hardCap;
    return vol;
  }

  // Causes « armé mais non dosant » dérivées côté client de champs /data déjà
  // existants (aucun champ WS dédié). Priorité IMPOSÉE par le gate :
  // L3 (filtration) > L4 (pH) > L5 (plafond).
  function _rescueReasonText(data, cfg) {
    if (data.water_present === false) {
      // L3
      return "Sans effet pour l'instant : filtration à l'arrêt, aucune injection possible.";
    }
    const ph = data.phFiltered; // pH CORRIGÉ (brut + offset) — c'est ce que l'utilisateur reconnaît
    if (typeof ph === 'number' && Number.isFinite(ph)) {
      // ⚠️ Piège BRUT/CORRIGÉ récurrent sur ce projet (ADR-0039) : le firmware
      // inhibe la chloration sur le pH FILTRÉ EN ÉCHELLE BRUTE
      // (getPhFilteredRaw(), hors offset) — PAS sur phFiltered (corrigé).
      // Les deux ne coïncident QUE si ph_offset == 0. Pour que ce callout ne
      // reproduise pas l'angle mort qu'il existe pour fermer (badge « en
      // attente » sans cause affichée si le seuil brut est franchi mais pas
      // le corrigé, ou l'inverse), on RETRANCHE l'offset avant de comparer au
      // seuil : NE PAS « corriger » ce signe, c'est délibéré.
      const offset = (cfg && typeof cfg.ph_offset === 'number') ? cfg.ph_offset : 0;
      const phRaw = ph - offset;
      if (phRaw > 7.6) {
        // L4 — gabarit, %s = pH CORRIGÉ (phFiltered) à 1 décimale — règle
        // produit du 2026-10-10, GO du gate pool-chemistry : le centième est
        // du bruit de mesure (filtré ±0,08 sur 10 min), une décimale suffit
        // pour toute valeur MESURÉE (deux décimales restent réservées aux
        // réglages et aux écrans de diagnostic).
        // 🔒 Toujours verrouillé : seul le TEST de seuil passe en échelle
        // brute (offset retranché, ligne 4006), l'AFFICHAGE reste en échelle
        // corrigée, la seule que l'utilisateur reconnaît sur le reste du
        // tableau de bord — délibéré et contre-intuitif, ne pas « corriger »
        // ce signe.
        // Ce texte est recalculé à CHAQUE RENDU : il ne se périme pas, à la
        // différence des causes de garde latchées du firmware (objet du
        // reste de cette spec).
        return `Chloration suspendue : pH ${ph.toFixed(1)} — le chlore y est peu actif. Faire baisser le pH d'abord.`;
      }
    }
    const effectiveDailyMl = _rescueEffectiveDailyMl(cfg || {});
    if (typeof data.orp_daily_ml === 'number' && data.orp_daily_ml >= effectiveDailyMl) {
      // L5a/L5b — PAIRE de libellés IMPOSÉS par le gate pool-chemistry, repris
      // MOT À MOT : ne JAMAIS les reformuler, abréger ou tronquer sans
      // repasser ce gate. feature-141 : le seuil de bascule entre les deux
      // n'est plus « < 1500 » mais « volume effectif < volume demandé »
      // (cfg.rescue_ml, repli 1500 si absent — rétrocompat firmware ancien).
      // Modifier l'un sans l'autre casse la cohérence du raisonnement : la
      // conclusion « le facteur limitant n'est pas le chlore » (L5a) n'est
      // valide QUE si le plein DEMANDÉ a été atteint — plafonné par la limite
      // utilisateur (L5b), cette conclusion serait fausse et détournerait
      // l'utilisateur du seul correctif réel (relever sa limite ou le volume
      // demandé).
      const requestedMl = (typeof (cfg || {}).rescue_ml === 'number' && cfg.rescue_ml > 0) ? cfg.rescue_ml : 1500;
      if (effectiveDailyMl < requestedMl) {
        // L5b — gabarit, %s = volume effectif puis volume demandé, ENTIERS, sans décimale.
        return `Volume quotidien atteint (${Math.round(effectiveDailyMl)} mL), plafonné par votre limite journalière. Reprise demain. Ce volume est inférieur aux ${Math.round(requestedMl)} mL que vous avez demandés : on ne peut pas en conclure que le chlore n'est pas le facteur limitant.`;
      }
      // L5a — gabarit, %s = volume effectif, ENTIER, sans décimale.
      return `Volume quotidien atteint (${Math.round(effectiveDailyMl)} mL). Reprise demain. Si l'ORP ne monte pas malgré ce volume, le facteur limitant n'est pas le chlore.`;
    }
    return null;
  }

  // feature-140 (v2.55.3, gate pool-chemistry) : l'état et le bouton Arrêter
  // de la Chloration de secours ont quitté le widget « Modes temporaires »
  // pour devenir un bandeau PLEINE PAGE (#rescue-active-banner), affiché
  // UNIQUEMENT quand rescue_until > 0, placé AVANT #dose-block-banner (le
  // secours RÉPOND au blocage, il ne s'y ajoute pas). Le widget ne contient
  // plus aucune trace du mode (DOM retiré, cf. index.html) : L1 disparaît de
  // l'écran avec lui — admissible, le L2 révisé en porte désormais
  // l'avertissement. Ni masquable ni repliable : pas de bouton de fermeture.
  function updateRescueCard() {
    const banner = $("#rescue-active-banner");
    if (!banner) return;
    const textEl = $("#rescue-active-banner-text");
    const data = latestSensorData || {};
    const cfg = window._config || {};

    const until = Number.isFinite(data.rescue_until) ? data.rescue_until : 0;
    const active = until > 0;
    const dosing = active && !!data.rescue_dosing;

    // feature-140 : rescue_lift — raison de FIN du mode (0 = aucune fin
    // récente). Indépendant de la visibilité du bandeau : un événement de
    // levée doit être annoncé même si le bandeau vient de se masquer.
    // Rétrocompat : champ absent (firmware ancien) ⇒ 0, aucun toast, aucune
    // erreur (même patron que rescue_until/rescue_dosing).
    const lift = Number.isFinite(data.rescue_lift) ? data.rescue_lift : 0;
    if (lift !== 0 && lift !== _rescueLastAnnouncedLift) {
      if (lift === 1) {
        // L6 — libellé IMPOSÉ par le gate pool-chemistry, repris MOT À MOT.
        showToast(
          "Chloration de secours levée automatiquement : les mesures ORP sont de nouveau fiables depuis 30 minutes. La régulation automatique a repris.",
          "info"
        );
      } else if (lift === 2) {
        // L9 — libellé IMPOSÉ par le gate pool-chemistry, repris MOT À MOT :
        // ne JAMAIS le reformuler, abréger ou tronquer sans repasser ce gate.
        showToast(
          "Chloration de secours terminée : les 48 h maximum sont écoulées. Rien ne prouve que l'eau est redevenue fiable. La régulation automatique a déjà repris selon votre configuration : contrôlez le chlore libre au réactif DPD et le pH avant de lui faire confiance, et réarmez la chloration de secours si les sondes restent douteuses.",
          "warning"
        );
      }
      // lift === 3 (arrêt manuel) : aucun toast, l'utilisateur vient d'agir.
      _rescueLastAnnouncedLift = lift;
    } else if (lift === 0) {
      _rescueLastAnnouncedLift = 0;
    }

    if (!active) {
      banner.hidden = true;
      return;
    }

    const d = new Date(until * 1000);
    const p = (n) => String(n).padStart(2, "0");
    const untilText = `${p(d.getDate())}/${p(d.getMonth() + 1)} à ${p(d.getHours())}:${p(d.getMinutes())}`;

    // ⚠️ PAIRE de libellés IMPOSÉS par le gate pool-chemistry (feature-140,
    // 2026-10-06), opposables au même titre que L1-L9 : ne JAMAIS les
    // reformuler, abréger ou tronquer sans repasser ce gate.
    let text;
    if (dosing) {
      // Variante A — actif ET dosant. %s = échéance JJ/MM à HH:MM.
      banner.className = "rescue-active-banner rescue-active-banner--info";
      text = `Chloration de secours active jusqu'au ${untilText} : le chlore est injecté à volume fixe, sans contrôle de l'ORP.`;
    } else {
      // Variante B — actif mais en attente. 1er %s = échéance, 2nd %s = cause
      // reprise MOT À MOT parmi L3/L4/L5a/L5b (priorité L3 > L4 > L5 inchangée,
      // cf. _rescueReasonText).
      banner.className = "rescue-active-banner rescue-active-banner--warning";
      const reason = _rescueReasonText(data, cfg) || "";
      text = `Chloration de secours active jusqu'au ${untilText}, mais le chlore ne coule pas actuellement. ${reason}`;
    }

    // Recommandation du gate pool-chemistry (feature-140, 2026-10-06), déménagée
    // du widget (v2.55.4) : phrase ajoutée aux variantes A et B — PAS un texte
    // séparé. ⚠️ Libellé IMPOSÉ MOT À MOT : c'est le seul endroit de
    // l'application qui dit que la filtration prime sur le chlore (contre-pied
    // du réflexe « ça ne monte pas, j'en remets »), gardé CONDITIONNEL pour ne
    // pas devenir du décor — affichée UNIQUEMENT quand la filtration n'est PAS
    // déjà prolongée (Boost inactif, ou boost_filtration_extended === false).
    const boostActive = (data.boost_active != null) ? !!data.boost_active : !!cfg.boost_active;
    const filtExtAvailable = data.boost_filtration_extended !== false;
    const filtrationAlreadyExtended = boostActive && filtExtAvailable;
    if (!filtrationAlreadyExtended) {
      text += " Suggestion : activez aussi le Boost pour prolonger la filtration — sur une eau chargée en matière organique, c'est le levier le plus utile, davantage que le chlore lui-même.";
    }

    if (textEl) textEl.textContent = text;
    banner.hidden = false;
  }

  // feature-141 (B1) : borne max de CONFORT pour le champ de saisie du
  // dialogue d'activation — PAS une garde. Même mirroir que le clamp serveur
  // (conditions C3/C4 du gate pool-chemistry) : min(limite journalière
  // utilisateur si > 0, kRescueChlorineDailyHardCapMl). Le clamp qui fait foi
  // est côté serveur (/rescue/start) — voir B9 pour l'annonce d'un écart.
  function _rescueVolumeInputMax(cfg) {
    let max = 2000; // miroir de kRescueChlorineDailyHardCapMl (constants.h)
    const userLimit = cfg.max_orp_ml_per_day;
    if (typeof userLimit === 'number' && userLimit > 0 && userLimit < max) max = userLimit;
    return max;
  }

  // feature-140 (v2.55.2) : activation de la chloration de secours — appelée
  // DEPUIS LE BANDEAU UNIQUEMENT (le bouton Activer du widget a été retiré du
  // DOM, cf. updateRescueCard ci-dessus). La confirmation L2 reste un passage
  // obligé, quel que soit l'appelant.
  // feature-141 : le volume quotidien n'est plus une constante — il est saisi
  // ici, pré-rempli depuis cfg.rescue_ml (déjà résolu côté serveur par la
  // chaîne de pré-remplissage). Ergonomie IMPOSÉE par le gate pool-chemistry :
  // l'utilisateur doit pouvoir valider SANS RIEN LIRE ni toucher au champ.
  async function _activateRescueChlorine() {
    const cfg = window._config || {};

    // Rétrocompat : cfg.rescue_ml absent/nul (firmware ancien) ⇒ repli local
    // 1500, jamais 0 — même principe que la condition bloquante 2 du gate.
    const prefill = (typeof cfg.rescue_ml === 'number' && Number.isFinite(cfg.rescue_ml) && cfg.rescue_ml > 0)
      ? Math.round(cfg.rescue_ml) : 1500;
    const maxBound = _rescueVolumeInputMax(cfg);

    const node = document.createElement('div');

    // L2 — confirmation IMPOSÉE par le gate pool-chemistry (feature-141,
    // révision du texte feature-140), reprise MOT À MOT : ne pas reformuler
    // sans nouveau passage du gate. La mention « plafonné à 1500 mL » a
    // disparu : 1500 n'est plus qu'un défaut, les vraies bornes sont
    // ailleurs (C3/C4, inchangées).
    const introP = document.createElement('p');
    introP.style.margin = '0 0 12px';
    introP.textContent = "Le dosage se fera à l'aveugle, sans contrôle de l'ORP, pendant 48 h au maximum. Le volume indiqué ci-dessous sera réparti sur la journée, sans jamais dépasser votre limite journalière. Le pH n'est jamais dosé dans ce mode. Il maintient une désinfection minimale — il ne traite pas la cause : quand l'eau est chargée en matière organique, seul le retrait mécanique des dépôts (brossage, aspiration, nettoyage du filtre) fait baisser cette charge. Désactivez-le dès que les sondes sont de nouveau fiables.";
    node.appendChild(introP);

    // B1/B3 — champ de volume + libellé/aide IMPOSÉS mot à mot par le gate.
    // Pré-rempli, modifiable, borné pour le confort (le clamp qui fait foi
    // est serveur, cf. _rescueVolumeInputMax et B9 plus bas).
    const fieldLabel = document.createElement('label');
    fieldLabel.style.cssText = 'display:flex;align-items:center;gap:6px;font-size:0.9em;flex-wrap:wrap;margin:0';
    fieldLabel.appendChild(document.createTextNode('Volume de chlore par jour (mL)'));
    const volumeInput = document.createElement('input');
    volumeInput.type = 'number';
    // feature-141 (recommandation gate 2) : quand la limite journalière de
    // l'utilisateur est INFÉRIEURE au plancher de 100, min et max s'inversent et
    // l'attribut devient incohérent. Le clamp serveur fait foi de toute façon,
    // mais un champ qui s'affiche min=100 max=60 est une incohérence visible.
    volumeInput.min = String(Math.min(100, maxBound));
    volumeInput.max = String(maxBound);
    volumeInput.step = '1';
    volumeInput.value = String(prefill);
    volumeInput.style.cssText = 'width:90px;padding:4px 6px;border:1px solid var(--border);border-radius:6px;background:var(--input-bg);color:var(--text);font-size:1em';
    fieldLabel.appendChild(volumeInput);
    node.appendChild(fieldLabel);

    const aideP = document.createElement('p');
    aideP.className = 'muted small';
    aideP.style.margin = '4px 0 0';
    aideP.textContent = "Pré-rempli d'après votre configuration. En cas de doute, laissez la valeur proposée.";
    node.appendChild(aideP);

    // B2 — avertissement IMPOSÉ mot à mot, révélé dynamiquement dès que la
    // saisie dépasse 1500 mL (1500 n'est plus un plafond, mais le seuil où le
    // bénéfice chimique cesse d'être démontré — gate pool-chemistry
    // feature-141). textContent uniquement : messageNode ne passe jamais par
    // innerHTML (cf. confirmDialog).
    const warnDiv = document.createElement('div');
    warnDiv.className = 'callout callout--warn';
    warnDiv.style.margin = '10px 0 0';
    warnDiv.hidden = true;
    warnDiv.textContent = "Au-delà de 1500 mL par jour, le bénéfice n'est pas démontré : quand l'eau est chargée en matière organique, l'ORP ne remonte pas tant que le point de rupture n'est pas atteint — augmenter le volume n'y change rien.";
    node.appendChild(warnDiv);

    const updateWarn = () => {
      const v = parseInt(volumeInput.value, 10);
      warnDiv.hidden = !(Number.isFinite(v) && v > 1500);
    };
    volumeInput.addEventListener('input', updateWarn);
    updateWarn();

    const ok = await confirmDialog({
      title: "Activer la chloration de secours ?",
      // Fallback window.confirm() natif (navigateur sans <dialog>) : pas de
      // champ éditable possible, le volume pré-rempli est envoyé tel quel.
      message: `${introP.textContent}\n\nVolume proposé : ${prefill} mL.`,
      messageNode: node,
      confirmLabel: 'Activer',
      cancelLabel: 'Annuler',
    });
    if (!ok) return;

    // ⚠️ Lire la valeur de l'input APRÈS confirmDialog : le nœud reste vivant
    // dans cette closure (feature-141). NaN ⇒ n'envoie rien, le serveur résout
    // lui-même la chaîne de pré-remplissage (condition bloquante 2 du gate :
    // jamais 0 mL).
    const submittedRaw = parseInt(volumeInput.value, 10);
    const hasSubmitted = Number.isFinite(submittedRaw) && submittedRaw > 0;
    const url = hasSubmitted ? `/rescue/start?ml=${submittedRaw}` : "/rescue/start";

    try {
      const response = await authFetch(url, { method: "POST" });
      if (response.ok) {
        const r = await response.json().catch(() => ({}));
        if (latestSensorData) {
          latestSensorData.rescue_until = (r.rescue_until != null) ? r.rescue_until : latestSensorData.rescue_until;
        }
        // Reflet optimiste du volume réellement armé (parité /get-config ↔
        // WS « config ») : évite d'attendre le prochain cycle pour que le
        // bandeau (_rescueEffectiveDailyMl) et L5a/L5b soient justes.
        if (window._config && r.rescue_ml != null) window._config.rescue_ml = r.rescue_ml;
        // B9 — un clamp n'est jamais silencieux : si le serveur a résolu un
        // volume différent de la saisie, l'utilisateur doit le savoir.
        if (hasSubmitted && r.rescue_ml != null && r.rescue_ml !== submittedRaw) {
          showToast(`Volume ramené à ${r.rescue_ml} mL par vos limites de sécurité (valeur saisie : ${submittedRaw} mL).`, "warning");
        } else {
          showToast("Chloration de secours activée.", "info");
        }
        updateRescueCard();
      } else if (response.status === 409) {
        const r = await response.json().catch(() => ({}));
        // L8 / refus horloge non synchronisée : message renvoyé tel quel par le firmware.
        showToast(r.message || "Activation impossible.", "error");
      } else {
        showToast("Erreur lors de l'activation de la chloration de secours", "error");
      }
    } catch (e) {
      console.error('Erreur activation chloration de secours:', e);
      showToast("Erreur de connexion", "error");
    }
  }

  function updateStatusCards() {
    updateBoostCard();  // feature-053 : indépendant de latestSensorData
    updateRescueCard(); // feature-140 : indépendant de latestSensorData (bandeau #rescue-active-banner)
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

    // feature-147 : jauge pH — trace le pH CORRIGÉ (data.ph, déjà affiché
    // ci-dessus en compact-value), jamais une valeur brute (gate C1).
    updatePhGauge((data.ph != null && !isNaN(data.ph)) ? data.ph : NaN, measuresOk);

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

    // feature-147 : jauge ORP — mesure + consigne, aucune zone colorée/nommée
    // (gate : un ORP « correct » n'existe pas dans l'absolu).
    updateOrpGauge((data.orp != null && !isNaN(data.orp)) ? data.orp : NaN,
                   (typeof config.orp_target === "number") ? config.orp_target : NaN,
                   measuresOk);

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

    // feature-147 (correctif) : jauge Température — même garde measuresOk que
    // la valeur compacte ci-dessus (filtration arrêtée ⇒ non représentative).
    updateTempGauge((data.temperature != null && !isNaN(data.temperature)) ? data.temperature : NaN, measuresOk);
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
    // bug-098 — DEUX formats de dosage, et il faut pouvoir les distinguer :
    //  · `dosing_mask` (exports >= 2.47.0) : CHAMP DE BITS 0x01 pH / 0x02 ORP /
    //    0x04 voie indéterminée — valeurs 0, 1, 2, 3 ou 4 (7 en théorie).
    //    Colonne au nom neuf EXPRÈS : dans l'ancien format, la
    //    colonne `dosing` valait déjà « 1 » pour dire « il y a eu dosage ». Avec
    //    le même nom, un « 1 » serait indécidable — pH ou voie inconnue ? — et le
    //    lire « pH » accuserait la pompe d'acide d'injections de chlore. C'est
    //    précisément le défaut que bug-098 corrige : on ne le réintroduit pas ici.
    //  · `dosing` (exports <= 2.46) : booléen. Il dit qu'il y a EU dosage, jamais
    //    lequel → 0x04, voie indéterminée.
    const idxDosingMask = headers.indexOf("dosing_mask");
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
        dosing: (idxDosingMask >= 0)
          ? dosingMaskFromApi(toValue(getCol(idxDosingMask)))
          : (idxDosing >= 0 && toBool(getCol(idxDosing)) ? DOSE_UNKNOWN : 0),
        granularity: idxGranularity >= 0 ? Math.max(0, parseInt(getCol(idxGranularity), 10) || 0) : 0
      });
    }

    return result;
  }

  // ================= bug-101 : import d'historique par LOTS =================
  // La route /history/import tamponne le corps entier avant d'appeler son
  // callback : son plafond est donc une contrainte MÉMOIRE de l'appareil, pas
  // un caprice. Le découpage vit ici, chez le client, ce qui évite d'agrandir
  // une allocation contiguë sur une carte en famine 8 bits (bug-080).
  const kHistoryImportChunkBudgetBytes = 7168;  // < 8192 accepté par l'appareil
  // Enveloppe mesurée sur le mode le plus LONG : le budget doit tenir quel que
  // soit le mode réellement envoyé.
  const kHistoryImportEnvelope = '{"mode":"replace","history":[]}';

  let _byteLenEncoder = null;
  function jsonByteLength(str) {
    if (_byteLenEncoder === null) {
      _byteLenEncoder = (typeof TextEncoder === 'function') ? new TextEncoder() : false;
    }
    if (_byteLenEncoder) return _byteLenEncoder.encode(str).length;
    // Repli sans TextEncoder : comptage UTF-8 explicite. On compte des OCTETS,
    // pas des code units — c'est ce que mesure Content-Length côté appareil.
    let n = 0;
    for (let i = 0; i < str.length; i++) {
      const c = str.charCodeAt(i);
      if (c < 0x80) n += 1;
      else if (c < 0x800) n += 2;
      else if (c >= 0xD800 && c <= 0xDBFF) { n += 4; i++; }
      else n += 3;
    }
    return n;
  }

  // Découpage par BUDGET D'OCTETS MESURÉS, jamais par nombre de points.
  // POURQUOI : le poids réel d'un point va de 110 à 149 octets selon les
  // décimales que rend JSON.stringify (7.9 contre 7.123456789012345). Un
  // découpage « par 145 points » repasserait le plafond au premier fichier à
  // décimales longues — c'est exactement le mur que ce correctif retire.
  function chunkHistoryPoints(points, budgetBytes = kHistoryImportChunkBudgetBytes) {
    const overhead = jsonByteLength(kHistoryImportEnvelope);
    const chunks = [];
    let cur = [];
    let curBytes = 0;
    for (const point of (points || [])) {
      const size = jsonByteLength(JSON.stringify(point));
      // +1 = la virgule de séparation, comptée seulement si le lot est déjà entamé
      const add = size + (cur.length ? 1 : 0);
      if (cur.length && overhead + curBytes + add > budgetBytes) {
        chunks.push(cur);
        cur = [point];
        curBytes = size;
      } else {
        cur.push(point);
        curBytes += add;
      }
    }
    if (cur.length) chunks.push(cur);
    // Un point seul plus gros que le budget partirait seul dans son lot et
    // serait refusé par l'appareil : structurellement impossible. Le pire cas
    // MESURÉ est 148 o (fixture history-worst-bytes.csv) et le maximum
    // THÉORIQUE 172 o — squelette de clés 97 o + 3 nombres de 25 o, 25 étant la
    // longueur JSON maximale d'un double. Soit 41 fois moins que le budget. Et
    // l'échec serait de toute façon visible et reprenable — jamais silencieux.
    return chunks;
  }

  // Total de points détenus par l'appareil, tel que la dernière lecture
  // « range=all » l'a montré. null = inconnu → la confirmation le dit ainsi
  // plutôt que d'inventer un chiffre.
  let _historyAllCount = null;

  // État de reprise, volontairement en mémoire de PAGE : l'appareil ne connaît
  // aucune session d'import. Chaque lot est autonome (mode + points), donc une
  // coupure au lot 3 sur 6 laisse un historique PARTIEL MAIS COHÉRENT — jamais
  // un historique vide.
  let _historyImportResume = null;  // { chunks, index, mode, points }

  function historyImportUi() {
    return {
      input: $("#history_import_file"),
      replaceBtn: $("#history_import_replace_btn"),
      appendBtn: $("#history_import_append_btn"),
      resumeBtn: $("#history_import_resume_btn"),
      abandonBtn: $("#history_import_abandon_btn"),
      wrap: $("#history_import_progress"),
      bar: $("#history_import_progress_bar"),
      status: $("#history_import_status")
    };
  }

  function setHistoryImportProgress(done, totalChunks, text) {
    const ui = historyImportUi();
    if (ui.wrap) ui.wrap.style.display = '';
    const pct = totalChunks ? Math.round((done / totalChunks) * 100) : 0;
    if (ui.bar) {
      ui.bar.style.width = `${pct}%`;
      ui.bar.textContent = `${pct}%`;
      ui.bar.setAttribute('aria-valuenow', String(pct));
    }
    if (ui.status) ui.status.textContent = text;
  }

  // Envoi SÉQUENTIEL, jamais concurrent : deux lots en vol se disputeraient le
  // tampon de requête et le mutex de l'historique.
  async function sendHistoryChunks(state) {
    const totalChunks = state.chunks.length;
    let lastTotal = null;
    let totalDropped = 0;
    while (state.index < totalChunks) {
      const i = state.index;
      // mode « replace » au PREMIER lot seulement : les suivants complètent,
      // sinon chaque lot effacerait le précédent (history.cpp : memoryBuffer =
      // dataPoints). C'est le piège qui détruisait l'historique de ceux qui
      // découpaient à la main.
      const mode = (i === 0) ? state.mode : 'append';
      setHistoryImportProgress(i, totalChunks, `Envoi du lot ${i + 1} sur ${totalChunks}…`);
      let res;
      try {
        res = await authFetch("/history/import", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({ mode, history: state.chunks[i] })
        });
      } catch (err) {
        throw new Error(`réseau interrompu au lot ${i + 1}/${totalChunks} (${err.message || err})`);
      }
      let body = null;
      try { body = await res.json(); } catch (_) {}
      if (!res.ok) {
        if (res.status === 413) {
          throw new Error(`lot ${i + 1}/${totalChunks} refusé comme trop volumineux — rechargez la page (une version en cache de l'interface envoie des lots hors gabarit)`);
        }
        if (res.status === 503) {
          throw new Error(`lot ${i + 1}/${totalChunks} refusé : mémoire insuffisante sur l'appareil, réessayez dans quelques minutes`);
        }
        throw new Error(body?.error || `lot ${i + 1}/${totalChunks} refusé (HTTP ${res.status})`);
      }
      if (typeof body?.total === 'number') lastTotal = body.total;
      if (typeof body?.dropped === 'number') totalDropped += body.dropped;
      state.index = i + 1;
    }
    setHistoryImportProgress(totalChunks, totalChunks, `${totalChunks} lot(s) envoyé(s).`);
    return { lastTotal, totalDropped };
  }

  // Source UNIQUE de l'état des boutons d'import. Deux conditions, dans cet
  // ordre : une reprise en attente verrouille les deux imports (voir le motif
  // dans le finally de runHistoryImport), sinon c'est la présence d'un fichier
  // qui décide.
  function refreshHistoryImportButtons() {
    const ui = historyImportUi();
    const pending = _historyImportResume !== null;
    const hasFile = !!(ui.input && ui.input.files && ui.input.files.length);
    if (ui.replaceBtn) ui.replaceBtn.disabled = pending || !hasFile;
    if (ui.appendBtn) ui.appendBtn.disabled = pending || !hasFile;
    if (ui.resumeBtn) {
      ui.resumeBtn.style.display = pending ? '' : 'none';
      ui.resumeBtn.disabled = !pending;
    }
    if (ui.abandonBtn) {
      ui.abandonBtn.style.display = pending ? '' : 'none';
      ui.abandonBtn.disabled = !pending;
    }
  }

  async function runHistoryImport(state) {
    const ui = historyImportUi();
    [ui.replaceBtn, ui.appendBtn, ui.resumeBtn, ui.abandonBtn]
      .forEach((b) => { if (b) b.disabled = true; });
    if (ui.input) ui.input.disabled = true;
    if (ui.resumeBtn) ui.resumeBtn.style.display = 'none';
    if (ui.abandonBtn) ui.abandonBtn.style.display = 'none';
    try {
      const { lastTotal, totalDropped } = await sendHistoryChunks(state);
      _historyImportResume = null;
      let msg = `Import terminé : ${state.points} point(s) envoyé(s)`;
      if (lastTotal !== null) {
        msg += `, ${lastTotal} conservé(s) sur l'appareil`;
        _historyAllCount = lastTotal;
      }
      msg += '.';
      if (totalDropped > 0) {
        // Une perte n'est JAMAIS muette : c'est le défaut d'origine de bug-101.
        // ⚠️ FORMULATION EXACTE, à ne pas « simplifier » en « points refusés » :
        // le compteur rendu par l'appareil est celui des points ÉVINCÉS DE
        // L'HISTORIQUE pour tenir dans les plafonds, soit les PLUS ANCIENS de
        // chaque granularité. Ce ne sont pas nécessairement les points du
        // fichier : importer 20 points récents dans un historique plein évince
        // 20 points anciens déjà présents. Dire « écartés du fichier » ferait
        // croire à l'utilisateur que sa sauvegarde n'est pas passée.
        showToast(`${msg} Attention : ${totalDropped} point(s) les plus anciens ont été évincés de l'historique pour tenir dans les plafonds de l'appareil.`, 'error');
      } else {
        showToast(msg, 'success');
      }
      if (ui.input) ui.input.value = '';
      // Succès : plus de reprise en attente, les deux imports redeviennent
      // disponibles via refreshHistoryImportButtons() dans le finally.
    } catch (err) {
      _historyImportResume = state;
      setHistoryImportProgress(state.index, state.chunks.length,
        `Interrompu après ${state.index} lot(s) sur ${state.chunks.length} — les points déjà envoyés sont conservés.`);
      if (ui.resumeBtn) { ui.resumeBtn.style.display = ''; ui.resumeBtn.disabled = false; }
      if (ui.abandonBtn) { ui.abandonBtn.style.display = ''; ui.abandonBtn.disabled = false; }
      showToast(`Erreur import historique : ${err.message || err}`, 'error');
    } finally {
      if (ui.input) ui.input.disabled = false;
      // ⚠️ CHEMIN DE PERTE DE DONNÉES — ne pas « simplifier » en réactivant
      // inconditionnellement. Tant qu'une reprise est EN ATTENTE, « Remplacer »
      // et « Compléter » restent désactivés : un clic sur « Remplacer » à cet
      // instant écraserait les lots déjà passés, c'est-à-dire détruirait l'import
      // partiel exactement quand l'utilisateur cherche à le récupérer après une
      // coupure. Seul « Reprendre » est offert, avec « Abandonner la reprise »
      // comme sortie explicite — sinon l'état serait sans issue.
      refreshHistoryImportButtons();
    }
  }

  // mode : 'replace' (destructif) ou 'append' (fusion)
  async function startHistoryImport(mode) {
    const ui = historyImportUi();
    if (!(ui.input && ui.input.files && ui.input.files.length)) return;

    let points;
    try {
      points = parseHistoryCsv(await ui.input.files[0].text());
    } catch (err) {
      showToast(`Lecture du fichier impossible : ${err.message || err}`, 'error');
      return;
    }
    if (!points.length) {
      showToast("Aucune donnée valide trouvée dans le fichier.", "error");
      return;
    }

    const chunks = chunkHistoryPoints(points);
    let biggest = 0;
    chunks.forEach((c) => {
      biggest = Math.max(biggest, jsonByteLength(JSON.stringify({ mode: 'replace', history: c })));
    });

    // La limite et le découpage sont annoncés AVANT l'envoi : l'autre moitié de
    // bug-101 était que la limite n'était écrite nulle part.
    const node = document.createElement('div');
    const line = (text, strong) => {
      const p = document.createElement('p');
      if (strong) {
        const b = document.createElement('strong');
        b.textContent = text;      // textContent, jamais innerHTML
        p.appendChild(b);
      } else {
        p.textContent = text;
      }
      node.appendChild(p);
    };
    if (mode === 'replace') {
      line(_historyAllCount === null
        ? "L'historique actuel de l'appareil sera intégralement remplacé."
        : `Les ${_historyAllCount} points actuels seront perdus.`, true);
    } else {
      line("Les créneaux communs seront écrasés par le fichier ; le reste de l'historique est conservé.", true);
    }
    line(`Fichier : ${points.length} point(s), envoyés en ${chunks.length} lot(s) de ${biggest} octets au plus (l'appareil refuse au-delà de 8192 octets par requête).`);
    if (chunks.length > 1) {
      line("Si la liaison coupe en cours de route, l'import est reprenable au lot interrompu ; les points déjà envoyés restent en place.");
    }

    const fallback = (mode === 'replace')
      ? `Les points actuels seront perdus.\n\nFichier : ${points.length} point(s) en ${chunks.length} lot(s).`
      : `Les créneaux communs seront écrasés par le fichier.\n\nFichier : ${points.length} point(s) en ${chunks.length} lot(s).`;

    if (!(await confirmDialog({
      title: (mode === 'replace') ? "Remplacer l'historique" : "Compléter l'historique",
      message: fallback,
      messageNode: node,
      confirmLabel: (mode === 'replace') ? "Remplacer" : "Compléter",
      danger: (mode === 'replace')
    }))) return;

    await runHistoryImport({ chunks, index: 0, mode, points: points.length });
  }

  function bindHistoryBackup() {
    const exportBtn = $("#history_export_btn");
    const importInput = $("#history_import_file");
    const replaceBtn = $("#history_import_replace_btn");
    const appendBtn = $("#history_import_append_btn");
    const resumeBtn = $("#history_import_resume_btn");
    const abandonBtn = $("#history_import_abandon_btn");
    const clearBtn = $("#history_clear_btn");

    exportBtn?.addEventListener("click", async () => {
      exportBtn.disabled = true;
      try {
        const res = await authFetch("/get-history?range=all");
        if (!res.ok) throw new Error("export failed");
        const data = await res.json();
        const rows = [
          ["datetime", "ph", "orp", "temperature", "filtration", "dosing",
           "dosing_mask", "granularity"].join(",")
        ];

        (data.history || []).forEach((point) => {
          // `dosing` reste le booléen d'héritage (tableurs et fichiers déjà
          // exportés), `dosing_mask` porte la voie réellement dosée.
          const mask = dosingMaskFromApi(point.dosing);
          const row = [
            formatDateTimeFromEpoch(point.timestamp),
            point.ph ?? "",
            point.orp ?? "",
            point.temperature ?? "",
            point.filtration ? 1 : 0,
            mask ? 1 : 0,
            mask,
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
      // Nouveau fichier choisi : la reprise de l'import précédent n'a plus de
      // sens (elle porterait sur les lots de l'ancien fichier). C'est donc AUSSI
      // une sortie de l'état « reprise en attente » — mais délibérée, pas un
      // effet de bord d'un bouton qui serait resté cliquable.
      _historyImportResume = null;
      const wrap = $("#history_import_progress");
      if (wrap) wrap.style.display = 'none';
      refreshHistoryImportButtons();
    });

    replaceBtn?.addEventListener("click", () => { startHistoryImport('replace'); });
    appendBtn?.addEventListener("click", () => { startHistoryImport('append'); });
    resumeBtn?.addEventListener("click", () => {
      if (_historyImportResume) runHistoryImport(_historyImportResume);
    });
    abandonBtn?.addEventListener("click", async () => {
      const state = _historyImportResume;
      if (!state) return;
      const sent = state.index;
      if (!(await confirmDialog({
        title: "Abandonner la reprise",
        message: `Les ${sent} lot(s) déjà envoyés RESTENT dans l'historique de l'appareil : rien n'est effacé.\n\nSeuls les ${state.chunks.length - sent} lot(s) restants ne seront pas envoyés.`,
        confirmLabel: "Abandonner",
        cancelLabel: "Continuer la reprise"
      }))) return;
      _historyImportResume = null;
      const wrap = $("#history_import_progress");
      if (wrap) wrap.style.display = 'none';
      refreshHistoryImportButtons();
      showToast(`Reprise abandonnée — les ${sent} lot(s) déjà importés sont conservés.`, 'info');
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

  // ========== feature-092 : cartes « Débit d'injection » pH et ORP ==========
  //
  // Une seule surface répond à la question que l'utilisateur se pose vraiment :
  // « la régulation demande combien, composé de quoi, et est-ce qu'elle
  // l'applique en ce moment — sinon pourquoi ». Ces cartes REMPLACENT trois
  // présentations empilées au fil des versions (bloc « Rattrapage accumulé »
  // v2.26.0, ligne « Dose de base apprise » feature-085, capacités + bandeau
  // « Dosage au maximum autorisé » feature-086) et les DEUX points de décision
  // `hidden` qui allaient avec — updatePhIntegralDisplay() et
  // updatePhActuatorLimitDisplay() sont SUPPRIMÉES, pas laissées mortes.
  //
  // Champs consommés, servis à l'IDENTIQUE sur /data et le WebSocket (parité
  // stricte : un champ WS-seul ne disparaît pas, il CLIGNOTE toutes les 30 s
  // quand la charge /data remplace latestSensorData — piège n° 1 du projet,
  // trois récidives en trois versions) :
  //   ph_ff_ml_per_min          → contribution du feedforward RÉELLEMENT servie
  //                               (0 hors session, 0 en régime de maintien) ;
  //   ph_ff_learned_ml_per_min  → la dose APPRISE, stable, connue même à froid ;
  //   ph_ff_status              → 0 inactif · 1 actif · 2 latch sécurité ·
  //                               3 historique < 3 j · 4 quota ≥ 60 % ;
  //   ph_p_term_ml_per_min      → terme proportionnel, SIGNÉ ;
  //   ph_i_term_ml_per_min      → terme intégral (rattrapage), en mL/min ;
  //   ph_integral_pct           → remplissage de la réserve (note de la ligne) ;
  //   ph_integral_saturated     → verdict de saturation SERVI (callout) ;
  //   ph_demand_ml_per_min      → la DEMANDE : ce que la pompe commande ;
  //   ph_sustainable_ml_per_min → plafond de débit MOYEN imposé par la limite
  //                               horaire (null = pompe non caractérisée) ;
  //   ph_limited_by_hourly_budget · ph_undersized · ph_daily_max_ml → 086 ;
  //   ph_pump_flow_uncharacterized → fiabilité des débits (pied de carte) ;
  //   ph_pulse_session · phMixingDelayActive · ph_mix_remaining_s ·
  //   phDoseBlockedCode / phDoseBlockedReason → pastille d'état.
  // Miroir ORP sans feedforward ni intégrale : le chlore est régulé en P PUR —
  // mais AVEC le cas bridé (orp_limited_by_hourly_budget) et le pied de fiabilité
  // (orp_pump_flow_uncharacterized), servis eux aussi.
  //
  // ⚠️ IL N'EXISTE AUCUN CHAMP « APPLIQUÉ » DE DÉBIT — et il n'en existera pas.
  // Le total affiché est donc la DEMANDE. Le firmware borne à l'amplitude de la
  // pompe, JAMAIS au débit soutenable : la limite horaire agit EN AVAL, par
  // refus. Afficher min(demande, soutenable) comme un débit servi
  // SOUS-ESTIMERAIT de ~17 % ce que le bassin reçoit dans les dix minutes qui
  // suivent (verdict pool-chemistry feature-092). Aucun min(), aucune somme de
  // contrôle, aucune valeur absolue n'est calculé ici : le plafond descend en
  // NOTE du total, et le seul calcul toléré est la soustraction documentée dans
  // _doseCoupleNote().
  //
  // SÉMANTIQUE STRICTE des états (clé absente ≠ null ≠ 0), contrat commun à
  // toute la page : nombre fini → valeur affirmée, 0,00 COMPRIS (une charge
  // médiane nulle est une observation légitime, jamais un tiret) ; null → le
  // firmware dit « non calculable » → « — » et la raison en note ; undefined →
  // information ABSENTE (firmware antérieur) → carte entière masquée, aucune
  // erreur console, et aucun des blocs retirés ne réapparaît.

  const _doseNum = (v) => (typeof v === "number" && isFinite(v)) ? v : null;

  // fr-FR, 2 décimales, unité collée. Le signe « − » est U+2212 (pas un trait
  // d'union) et il est porté par la VALEUR SERVIE : les champs de débit sont
  // signés et un « −3,00 mL/min » sur la ligne Écart est VOULU — il enseigne
  // l'asymétrie de l'actionneur acide. Aucune valeur absolue n'est prise ici :
  // masquer un signe servi reviendrait à inventer une grandeur.
  function _fmtDoseFlow(v) {
    return (v < 0 ? "−" : "")
      + Math.abs(v).toLocaleString("fr-FR", { minimumFractionDigits: 2, maximumFractionDigits: 2 })
      + " mL/min";
  }

  // Écrit une ligne de la carte : valeur seule dans .kv__v, libellé + note dans
  // .kv__k (la note est un <span class="kv__note"> INTERNE au libellé).
  function _setDoseCell(idBase, value, note) {
    const v = $(`#${idBase}-value`);
    if (v) v.textContent = value;
    const n = $(`#${idBase}-note`);
    if (n) n.textContent = note || "";
  }

  // ---- Bornage du couple (P + I) : la note qui remplace toute sommation ----
  //
  // L'égalité de référence n'est PAS `demande = ff + p + i`. Le firmware borne
  // le COUPLE (P + I) à zéro AVANT d'ajouter le feedforward : avec ff = 1,41 et
  // p = −3,00, il injecte bien 1,41 mL/min. L'égalité vraie est donc
  //   demande = coupleServi + ffServi
  // où `coupleServi` se RECONSTITUE par soustraction — aucune régulation n'est
  // recalculée côté client, et le couple borné n'est JAMAIS réparti entre P et
  // I selon une règle inventée. Quand le couple brut diffère du couple servi,
  // la ligne le NOMME : un trait de total sous des nombres qui ne s'additionnent
  // pas, SANS note, serait le pire des deux mondes.
  //
  // Seuil 0,01 IMPÉRATIF (pas un `!==`) : les champs sont servis arrondis à deux
  // décimales, une égalité stricte faux-positiverait d'un centime.
  //
  // La note se pose sur la ligne Rattrapage côté pH ; côté ORP, qui n'a pas de
  // ligne Rattrapage, sur la ligne Écart.
  function _doseCoupleNote(data, prefix) {
    const demand = _doseNum(data[`${prefix}_demand_ml_per_min`]);
    const p = _doseNum(data[`${prefix}_p_term_ml_per_min`]);
    const ff = (prefix === "ph") ? _doseNum(data.ph_ff_ml_per_min) : 0;
    const i = (prefix === "ph") ? _doseNum(data.ph_i_term_ml_per_min) : 0;
    if (demand === null || p === null || ff === null || i === null) return "";
    const served = demand - ff;
    const rawCouple = p + i;
    if (Math.abs(rawCouple - served) <= 0.01) return "";
    return (rawCouple < 0)
      ? "plancher appliqué · couple retenu 0,00 mL/min"
      : "couple non servi · maintien de session";
  }

  // ---- Ligne « Écart courant » — point d'entrée UNIQUE de cette ligne -------
  // Valeur = terme proportionnel BRUT ET SIGNÉ. null (mesure invalide : filtre
  // non prêt, purge, NaN) → « — », la cause est portée par la pastille.
  function _doseSpreadCell(data, prefix) {
    const p = _doseNum(data[`${prefix}_p_term_ml_per_min`]);
    return {
      value: (p === null) ? "—" : _fmtDoseFlow(p),
      // Côté ORP il n'y a pas de ligne Rattrapage : le bornage du couple
      // s'annonce ici. Côté pH il s'annonce sur la ligne Rattrapage.
      note: (prefix === "ph") ? "" : _doseCoupleNote(data, prefix),
    };
  }

  // ---- Ligne « Dose de base apprise » (pH) ---------------------------------
  // La VALEUR est la contribution appliquée (`ph_ff_ml_per_min`), pas la dose
  // apprise : c'est ce que la pompe sert, et c'est ce qui rend le total
  // additionnable. La dose APPRISE, elle, descend en note — c'est elle qui
  // manquait avant feature-092 : hors session, l'utilisateur ne pouvait pas
  // distinguer « rien d'appris » de « 1,41 appris, en attente ».
  // « en attente » est placé AVANT le chiffre : l'œil doit lire la cause avant
  // la valeur, sinon un zéro se lit comme une panne. Point le plus délicat de
  // la carte — ne pas réordonner ces textes, ils sont validés tels quels.
  // Aucune note quand la dose apprise n'est pas un nombre fini : il n'y a
  // rien à appliquer, donc rien à annoncer.
  function _doseFfCell(data) {
    const applied = _doseNum(data.ph_ff_ml_per_min);
    const learned = _doseNum(data.ph_ff_learned_ml_per_min);
    const st = data.ph_ff_status;
    // Statut 3 (historique < 3 j) : la dose n'est pas encore une grandeur, la
    // valeur reste un tiret même si le firmware sert un 0 de contribution.
    const value = (st === 3 || applied === null) ? "—" : _fmtDoseFlow(applied);
    let note = "";
    if (learned !== null) {
      const l = _fmtDoseFlow(learned);
      if (st === 0) {
        note = `en attente — dose apprise ${l}, appliquée à la prochaine session`;
      } else if (st === 2) {
        note = `suspendue (pH sous consigne, mesure invalide ou réglages modifiés) — dose apprise ${l} conservée`;
      } else if (st === 3) {
        note = "en apprentissage (historique < 3 j)";
      } else if (st === 4) {
        note = `suspendue (60 % du quota atteint) — dose apprise ${l} conservée`;
      }
      // st === 1 (active, en session) : la valeur parle d'elle-même, pas de note.
      // Statut inconnu d'un firmware plus récent : pas de note inventée.
    }
    return { value, note };
  }

  // ---- Pastille d'état : cinq priorités, trois couleurs --------------------
  // SÉMANTIQUE DE COULEUR : vert = pompe commandée, gris = état normal, rouge =
  // PANNE uniquement. Pas d'ambre — une pause n'est pas un avertissement. Aucun
  // glyphe « ● / ○ » : indiscernables à 12 px et ils créeraient une seconde
  // sémantique de vert à 20 px des pastilles du calendrier 7 jours (décision
  // ux-ui-designer). Seule zone aria-live de la carte.
  // Compte à rebours UNIQUEMENT sur la pause mélange : champ servi et borné,
  // il répond exactement à la question posée. Pas de durée écoulée de session
  // (aucun champ, et un compteur qui tourne n'est que du bruit).
  //   1. cause de refus de ton `fault` (panne sonde) → rouge ;
  //   2. session d'impulsions en cours → vert ;
  //   3. pause mélange → gris + minutes restantes ;
  //   4. cause de refus de ton `normal` (filtration arrêtée, quota…) → gris ;
  //   5. aucune cause, pas de session → gris, qualifié « à la consigne » SI le
  //      terme P vaut EXACTEMENT 0. `null` ne qualifie pas : un P null signale
  //      un gain nul (régulateur inerte) ou une mesure invalide, pas un pH à la
  //      consigne — l'affirmer serait un contresens ;
  //   6. aucune cause, pas de session, terme P NON nul → « En attente » SEUL.
  //      C'est le cas trompeur : la porte de session statue sur le signal LENT
  //      (moyenne 10 min), la composition sur l'erreur INSTANTANÉE — avec la
  //      dispersion pH de cet appareil, la demande peut être non nulle porte
  //      fermée. La ligne Demande porte alors la note qui l'explique (cf.
  //      updatePhDosingCard) : un chiffre sans motif de non-application est
  //      exactement le vide que cette carte existe pour combler.
  // RETOURNE l'état retenu ('fault' | 'session' | 'mixing' | 'blocked' |
  // 'setpoint' | 'plain' | 'unknown') — 'plain' EST le 6ᵉ cas.
  function _renderDosePill(prefix, data) {
    const el = $(`#${prefix}-dosing-pill`);
    if (!el) return "";
    const info = _doseBlockInfo(data, prefix);
    const entry = DOSE_BLOCK_CODES[info.code];
    let tone = info.tone;
    // DOSE_BLOCK_CODES reste la source UNIQUE des libellés de refus : la
    // pastille lit un libellé COURT (`pill`/`chip`) et retombe sur `label`
    // quand il n'y en a pas — les libellés des codes 2 et 10-14 sont déjà
    // assez courts pour une pastille.
    let short = (entry && (entry.pill || entry.chip)) || info.label || "";
    // feature-139 (v2.62.0, lot B) : code 18 — `info.label` porte déjà le
    // texte DÉTERMINISTE (_measuresWaitText), il doit primer sur `entry.chip`
    // (repli statique hors-ligne de la table).
    if (info.code === 18) short = info.label;
    if (info.code === DOSE_BLOCK_CODE_GUARD) {
      // Sentinelle 250 : cause de GARDE, message porteur de valeurs runtime →
      // trop long pour une pastille (le modal filtre garde le texte complet).
      short = (entry && entry.pill) || "Sécurité pH/ORP active";
      if (prefix === "orp" && entry && entry.pillOrp) {
        // Côté chlore, la garde 250 = inhibition sur pH élevé. C'est un état
        // d'ATTENTE normal, pas une panne : gris, jamais ambre ni rouge
        // (verdict pool-chemistry feature-092).
        short = entry.pillOrp;
        tone = entry.toneOrp || "normal";
      }
    }
    const mixing = (data[`${prefix}MixingDelayActive`] === true);
    const remain = _doseNum(data[`${prefix}_mix_remaining_s`]);
    let cls = "";
    let txt;
    let state;
    if (tone === "fault" && short) {
      cls = "bad";
      txt = "Dosage suspendu · " + short;
      state = "fault";
    } else if (data[`${prefix}_pulse_session`] === true) {
      cls = "ok";
      txt = "Injection en cours";
      state = "session";
    } else if (mixing) {
      txt = (remain === null)
        ? "Pause mélange"
        : `Pause mélange · ${Math.ceil(Math.max(0, remain) / 60)} min`;
      state = "mixing";
    } else if (tone === "normal" && short) {
      txt = "En attente · " + short;
      state = "blocked";
    } else if (data[`${prefix}_p_term_ml_per_min`] === 0) {
      txt = "En attente · " + ((prefix === "ph") ? "pH" : "ORP") + " à la consigne";
      state = "setpoint";
    } else {
      // Ton `unknown` (code plus récent que cette page) ou terme P non nul sans
      // refus : on dit l'attente, on ne devine pas la cause.
      txt = "En attente";
      // 'plain' = 6ᵉ cas, AUCUNE cause de refus servie ; 'unknown' = code non
      // répertorié (la cause existe, cette page ne sait pas la nommer) — la note
      // de la ligne Demande ne doit parler QUE du premier.
      state = (info.code === 0 || info.code === undefined) ? "plain" : "unknown";
    }
    el.className = "pill" + (cls ? " " + cls : "");
    el.textContent = txt;
    return state;
  }

  // ---- Pieds de carte communs aux deux cartes ------------------------------
  // Pied « limite horaire » — REMPLACE le bandeau #ph_budget_banner (retiré), et
  // sert AUSSI la carte ORP : la régulation chlore sature son quota horaire
  // quotidiennement (journal de production), le cas bridé n'y est pas théorique.
  // Phrase validée par pool-chemistry : elle dit que la pompe sert la demande
  // PUIS s'arrête, et non qu'elle débite en continu à un débit réduit. Le verdict
  // est SERVI par le firmware ({prefix}_limited_by_hourly_budget) — aucune
  // comparaison demande/plafond côté client. Les nombres viennent de /get-config
  // et de la trame, JAMAIS d'une valeur en dur ; sans la limite configurée la
  // phrase serait creuse → on masque.
  function _renderDoseBudgetFoot(prefix, limited, sustainable) {
    const foot = $(`#${prefix}-dose-budget-foot`);
    if (!foot) return;
    const limitMin = _doseNum(window._config?.[`${prefix}_limit_minutes`]);
    const show = limited && sustainable !== null && limitMin !== null;
    foot.hidden = !show;
    if (show) {
      foot.textContent = "La limite horaire (" + limitMin.toLocaleString("fr-FR")
        + " min/h) autorise " + _fmtDoseFlow(sustainable) + " en moyenne sur l'heure : "
        + "la pompe sert la demande, puis s'arrête jusqu'à l'heure suivante.";
    }
  }

  // Pied « débit non caractérisé » : sans pesée, TOUS les débits de la carte
  // reposent sur le débit d'usine de la pompe. C'est une qualification de la
  // FIABILITÉ des nombres affichés, elle ne doit pas disparaître — elle vivait
  // avant dans la ligne « Débit soutenable » du bloc capacités (retirée).
  // Texte statique dans index.html, seul `hidden` est piloté ici. Clé absente
  // (firmware antérieur) ⇒ pied masqué, jamais un `false` inventé.
  function _renderDoseUncharacterizedFoot(prefix, data) {
    const foot = $(`#${prefix}-dose-uncharacterized-foot`);
    if (!foot) return;
    foot.hidden = (data[`${prefix}_pump_flow_uncharacterized`] !== true);
  }

  // ---- Carte pH : SEUL point de décision de son `hidden` -------------------
  function updatePhDosingCard() {
    const data = latestSensorData || {};

    // (1) Capacité journalière (colonne des stats) et alerte de
    // sous-dimensionnement (juste sous la carte) : ces deux éléments vivent HORS
    // de la carte, mais leur `hidden` était piloté par
    // updatePhActuatorLimitDisplay(), supprimée — il passe donc ici. MÊME
    // contrat de présence qu'avant (ph_limited_by_hourly_budget booléen) et,
    // comme avant, INDÉPENDANT du mode de régulation : un maximum journalier
    // reste une capacité lisible en mode manuel.
    const served086 = (typeof data.ph_limited_by_hourly_budget === "boolean");
    const capBlock = $("#ph_capacity_block");
    if (capBlock) {
      capBlock.hidden = !served086;
      const maxEl = $("#ph_daily_max_value");
      const dailyMax = _doseNum(data.ph_daily_max_ml);
      if (maxEl) maxEl.textContent = (dailyMax !== null)
        ? Math.round(dailyMax).toLocaleString("fr-FR") + " mL/j"
        : "Indisponible";
    }
    const undersizedBanner = $("#ph_undersized_banner");
    if (undersizedBanner) {
      undersizedBanner.hidden = !(served086 && data.ph_undersized === true);
    }

    // (2) La carte. Deux conditions, un seul endroit : mode automatique (le
    // feedforward et le rattrapage n'évoluent que là ; ailleurs on afficherait
    // un reliquat figé) et champ sentinelle servi (firmware ≥ feature-092).
    const card = $("#ph-card-dosing");
    if (!card) return;
    const mode = getSegmented("ph_regulation_mode") || "automatic";
    if (mode !== "automatic" || data.ph_demand_ml_per_min === undefined) {
      card.hidden = true;
      return;
    }
    card.hidden = false;

    // La pastille est rendue AVANT les lignes : son verdict (6ᵉ cas) conditionne
    // la note de la ligne Demande, qui doit expliquer un chiffre non appliqué.
    const pillState = _renderDosePill("ph", data);

    const ff = _doseFfCell(data);
    _setDoseCell("ph-dose-ff", ff.value, ff.note);

    const spread = _doseSpreadCell(data, "ph");
    _setDoseCell("ph-dose-p", spread.value, spread.note);

    // Rattrapage : la réserve (%) est SERVIE par le firmware, jamais recalculée
    // ici (le plafond de l'anti-windup varie avec la vitesse de correction — le
    // recopier rouvrirait le motif du miroir de constante, éliminé cinq fois
    // dans ce projet). Le « N pH·s » a disparu : jargon sans usage pour la
    // cible. Le bornage du couple, s'il y a lieu, s'annonce EN TÊTE de note :
    // c'est lui qui justifie l'arithmétique du total.
    const iTerm = _doseNum(data.ph_i_term_ml_per_min);
    const pct = _doseNum(data.ph_integral_pct);
    const iNotes = [];
    const couple = _doseCoupleNote(data, "ph");
    if (couple) iNotes.push(couple);
    if (pct !== null) {
      iNotes.push("réserve " + pct.toLocaleString("fr-FR", { maximumFractionDigits: 0 }) + " %");
    }
    _setDoseCell("ph-dose-i", (iTerm === null) ? "—" : _fmtDoseFlow(iTerm), iNotes.join(" · "));

    // Total — la DEMANDE en gras (jamais un « appliqué » : cf. l'avertissement
    // en tête de section). Le plafond descend en note ; plafond null (pompe non
    // caractérisée) → AUCUNE note, on ne bavarde pas sur ce qu'on ignore.
    const demand = _doseNum(data.ph_demand_ml_per_min);
    const sust = _doseNum(data.ph_sustainable_ml_per_min);
    const limited = (data.ph_limited_by_hourly_budget === true);
    const totalNotes = [];
    // CAUSE DE NON-APPLICATION D'ABORD, rappel du plafond ensuite : c'est la
    // question que l'utilisateur se pose en regardant le chiffre. La note n'est
    // posée QUE dans le 6ᵉ cas de la pastille (« En attente » sans suffixe :
    // aucun refus servi, hors bande morte, pas de session) et sur une demande
    // non nulle — ailleurs elle diluerait la note du plafond, qui vit sur la
    // MÊME ligne. Formulation volontairement SANS code de porte : les `kPhGate*`
    // sont un contrat interne, non sérialisé (les exposer demanderait un ADR).
    // Ce qu'elle dit est vrai et suffisant : la porte décide sur une moyenne
    // glissante, la composition affichée est instantanée.
    if (pillState === "plain" && demand !== null && Math.abs(demand) > 0.005) {
      totalNotes.push("demande instantanée : la régulation attend une mesure stabilisée avant d'ouvrir une session");
    }
    if (sust !== null) {
      totalNotes.push(limited
        ? "apport moyen possible " + _fmtDoseFlow(sust) + " en moyenne sur l'heure"
        : "sous le maximum de " + _fmtDoseFlow(sust));
    }
    _setDoseCell("ph-dose-total", (demand === null) ? "—" : _fmtDoseFlow(demand), totalNotes.join(" · "));

    _renderDoseBudgetFoot("ph", limited, sust);
    _renderDoseUncharacterizedFoot("ph", data);
    // (pastille déjà rendue en tête : son verdict alimente la note du total)

    // Callout « rattrapage au maximum » (v2.26.0), déplacé au pied de la carte,
    // TEXTE ET RÈGLE INCHANGÉS : le verdict est SERVI par le firmware
    // (`ph_integral_saturated`), il n'est pas recalculé ici — le seuil firmware
    // porte sur `demande / soutenable`, pas sur le remplissage de l'anti-windup,
    // et le firmware seul connaît la durée (2 h / 6 h). Clé absente ⇒ pas
    // d'encart, jamais un `false` inventé.
    const warnEl = $("#ph_integral_warning");
    if (warnEl) {
      const saturated = (data.ph_integral_saturated === true);
      warnEl.hidden = !saturated;
      if (saturated) {
        warnEl.textContent =
          "Rattrapage au maximum : la régulation demande tout ce qu'elle peut sans que "
          + "le pH rejoigne sa consigne. Vérifiez le niveau du bidon, l'amorçage de la "
          + "pompe doseuse et le TAC de l'eau (une alcalinité élevée bloque le pH).";
      }
    }
  }

  // ---- Carte ORP : deux lignes, même composant -----------------------------
  // Le chlore est régulé en P PUR (Ki = 0, pas de feedforward) : il n'y a ni
  // ligne « Dose de base », ni ligne « Rattrapage » — et c'est précisément ce
  // que la carte montre au passage. Ce n'est pas un repli : la carte est
  // complète pour ce qu'elle régule.
  function updateOrpDosingCard() {
    const data = latestSensorData || {};
    const card = $("#orp-card-dosing");
    if (!card) return;
    const mode = getSegmented("orp_regulation_mode") || "automatic";
    if (mode !== "automatic" || data.orp_demand_ml_per_min === undefined) {
      card.hidden = true;
      return;
    }
    card.hidden = false;

    const spread = _doseSpreadCell(data, "orp");
    _setDoseCell("orp-dose-p", spread.value, spread.note);

    // PAS de note « demande instantanée » ici, à la différence de la carte pH :
    // le signal lent et la porte de session de feature-094 (phSessionGate,
    // moyenne glissante 10 min) n'existent QUE pour le pH — il n'y a pas
    // d'orpSessionGate. Côté chlore, la décision lit la mesure filtrée
    // INSTANTANÉE, la même que la composition : une demande non nulle sans
    // session relève de la bande morte d'hystérésis, pas d'une attente de
    // stabilisation. Reprendre la phrase du pH y serait FAUX.

    // Total et plafond : MÊME règle que la carte pH, y compris le cas bridé — le
    // quota horaire du chlore est atteint tous les jours en production.
    const demand = _doseNum(data.orp_demand_ml_per_min);
    const sust = _doseNum(data.orp_sustainable_ml_per_min);
    const limited = (data.orp_limited_by_hourly_budget === true);
    let totalNote = "";
    if (sust !== null) {
      totalNote = limited
        ? "apport moyen possible " + _fmtDoseFlow(sust) + " en moyenne sur l'heure"
        : "sous le maximum de " + _fmtDoseFlow(sust);
    }
    _setDoseCell("orp-dose-total", (demand === null) ? "—" : _fmtDoseFlow(demand), totalNote);

    _renderDoseBudgetFoot("orp", limited, sust);
    _renderDoseUncharacterizedFoot("orp", data);
    _renderDosePill("orp", data);
  }

  // ========== feature-096 (v2.37.0) : temps d'atteinte de cible ==========
  //
  // HUIT champs servis à l'IDENTIQUE sur /data et le WebSocket (parité stricte,
  // même passage côté firmware) :
  //   {ph|orp}_ttt_state → 0 pas de départ aujourd'hui · 1 en cours ·
  //                        2 atteinte · 3 non atteinte (4-255 réservés) ;
  //   {ph|orp}_ttt_min   → minutes de FILTRATION (état 1 : écoulées ;
  //                        2 : temps d'atteinte ; 3 : durée totale) ;
  //   {ph|orp}_ttt_e0    → écart initial signé à t0 (null = indisponible) ;
  //   {ph|orp}_ttt_flag  → CHAMP DE BITS, jamais une énumération : une journée
  //                        peut cumuler plusieurs circonstances, et les bits
  //                        libres d'un firmware plus récent doivent rester sans
  //                        effet ici. On teste donc des bits, jamais l'égalité.
  //
  // Le contrat NE PORTE PAS d'écart résiduel : seul `e0` existe. Le détail dit
  // « écart initial » — n'inventez pas un « à l'arrêt » que le firmware
  // n'expose pas.
  //
  // La mémoire 7 jours vient de `GET /get-kpi` (jamais du WS) : elle n'est
  // relue qu'à l'entrée des vues pH/ORP, puis au changement d'état ou de jour.
  // JAMAIS en polling — c'est une donnée qui bouge deux fois par jour.
  const TTT_LONG_MIN = 120;  // seuil vert/ambré des pastilles : 2 h

  // Libellés des bits de drapeau. Table EXTENSIBLE : un bit inconnu (firmware
  // plus récent) est simplement ignoré, il ne casse ni la phrase ni la note.
  const TTT_FLAG_LABELS = [
    [0x01, "consigne modifiée"],
    [0x02, "boost chlore"],
    // feature-139 (v2.62.0, balayage de vocabulaire du 2026-10-08) : ne plus
    // dire « stabilisation » — ce bit décrit un jour où la mesure a été
    // indisponible (attente post-coupure d'eau), vocabulaire SIMPLE.
    [0x04, "mesure indisponible"],
    [0x08, "reprise après coupure"],
    [0x10, "régulation non automatique"],
    // bug-106 : le compteur de filtration a RECULÉ pendant la tenue du jour
    // (effacement NVS pool-daily, restauration d'usine ou de configuration).
    // La durée archivée ne mesure alors plus rien — ce n'est pas « cible non
    // atteinte », c'est « pas de mesure ».
    [0x20, "compteur de filtration remis à zéro"],
  ];

  // Bit du champ de bits `f` signalant la remise à zéro du compteur (miroir de
  // kTttFlagCounterReset côté firmware). Isolé dans une constante : il est testé
  // hors de la table des libellés, pour retirer la journée du vocabulaire
  // « bonne journée ».
  const TTT_FLAG_COUNTER_RESET = 0x20;

  // Vrai si la durée de cette entrée a été invalidée par un recul du compteur.
  function tttCounterReset(entry) {
    return !!entry && typeof entry.f === "number" && (entry.f & TTT_FLAG_COUNTER_RESET) !== 0;
  }

  function tttFlagCauses(flags) {
    if (typeof flags !== "number" || !isFinite(flags) || flags <= 0) return [];
    return TTT_FLAG_LABELS.filter(([bit]) => (flags & bit) !== 0).map(([, label]) => label);
  }

  // « 45 min » (< 60), « 3 h 53 » (minutes sur 2 chiffres), « 5 h » (reste nul).
  // Espaces INSÉCABLES : une durée ne doit jamais se couper en fin de ligne.
  function fmtLong(min) {
    if (typeof min !== "number" || !isFinite(min) || min < 0) return "—";
    const m = Math.round(min);
    if (m < 60) return `${m} min`;
    const h = Math.floor(m / 60);
    const r = m % 60;
    return r === 0 ? `${h} h` : `${h} h ${String(r).padStart(2, "0")}`;
  }

  // Forme courte des pastilles : 4 caractères, chiffres tabulaires (« 3h53 »).
  function fmtShort(min) {
    if (typeof min !== "number" || !isFinite(min) || min < 0) return "—";
    const m = Math.round(min);
    return `${Math.floor(m / 60)}h${String(m % 60).padStart(2, "0")}`;
  }

  // Écart initial signé. pH : 2 décimales, virgule française. ORP : entier + mV.
  // Signe TOUJOURS explicite, moins typographique U+2212 (jamais le trait
  // d'union, qui se confond avec une puce à 11 px).
  function fmtE0(v, unit) {
    if (typeof v !== "number" || !isFinite(v)) return null;
    const isPh = (unit === "ph");
    const abs = Math.abs(v);
    const txt = isPh
      ? abs.toLocaleString("fr-FR", { minimumFractionDigits: 2, maximumFractionDigits: 2 })
      : Math.round(abs).toLocaleString("fr-FR");
    const sign = (v < 0) ? "−" : "+";
    return sign + txt + (isPh ? "" : " mV");
  }

  // Mémoire 7 jours (réponse de /get-kpi), + traces de la dernière relecture.
  let _kpiDays = null;          // null = jamais chargé · [] = chargé et vide
  // Dernière tentative en échec (hors ligne ou 503 « mémoire indisponible »).
  // REVUE (mineur) : ce drapeau n'est PAS réarmé par un rendu. Après un échec,
  // l'historique ne se recharge donc qu'à la ré-entrée dans la vue ou au
  // changement d'état/de jour — jamais en boucle. C'est délibéré : la mémoire
  // 7 jours ne change au plus que deux fois par jour et par régulation,
  // repolluer le réseau pour elle n'apporterait rien.
  let _kpiFailed = false;
  let _kpiInFlight = false;
  let _kpiSig = "";             // signature « états + jour » de la dernière relecture
  let _kpiSuppressFetch = false; // rendu déclenché PAR la relecture : ne pas reboucler

  async function fetchKpiHistory() {
    if (_kpiInFlight) return;
    _kpiInFlight = true;
    try {
      const res = await authFetch(`/get-kpi?t=${Date.now()}`, { cache: "no-store" });
      if (!res.ok) throw new Error("kpi " + res.status);
      const json = await res.json();
      _kpiDays = Array.isArray(json.days) ? json.days : [];
      _kpiFailed = false;
    } catch (e) {
      // Offline, 503 « mémoire indisponible » ou JSON illisible : on le DIT
      // (légende remplacée), on ne fabrique pas une bande vide qui se lirait
      // « aucune journée enregistrée ».
      _kpiFailed = true;
      _kpiDays = null;
    } finally {
      _kpiInFlight = false;
      // Le re-rendu qui suit RAFRAÎCHIT la bande et met la signature à jour ; il
      // ne doit pas relancer une seconde requête (la donnée vient d'arriver).
      _kpiSuppressFetch = true;
      try { updateTimeToTargetDisplay(); } catch (_) { /* rendu défensif */ }
      _kpiSuppressFetch = false;
    }
  }

  // Date lisible d'une journée (AAAAMMJJ) : « Vendredi 4 septembre ».
  // Noms EN DUR, comme dans loadHistoricalData : toLocaleDateString("fr-FR")
  // lance une SyntaxError sur certaines versions de Safari (précédent déjà
  // rencontré et commenté dans ce fichier). Une phrase d'aria-label ne peut pas
  // dépendre d'une API qui jette.
  const TTT_JOURS_FR = ["Dimanche", "Lundi", "Mardi", "Mercredi", "Jeudi", "Vendredi", "Samedi"];
  const TTT_MOIS_FR = ["janvier", "février", "mars", "avril", "mai", "juin",
                       "juillet", "août", "septembre", "octobre", "novembre", "décembre"];

  function tttDayLabel(d) {
    if (typeof d !== "number" || d < 19700101) return null;
    const y = Math.floor(d / 10000);
    const mo = Math.floor(d / 100) % 100;
    const da = d % 100;
    if (mo < 1 || mo > 12 || da < 1 || da > 31) return null;
    const dt = new Date(y, mo - 1, da, 12, 0, 0, 0);
    if (isNaN(dt.getTime())) return null;
    // « 1er », pas « 1 » : la phrase est lue à voix haute par les lecteurs d'écran.
    return `${TTT_JOURS_FR[dt.getDay()]} ${da === 1 ? "1er" : da} ${TTT_MOIS_FR[mo - 1]}`;
  }

  // Phrase COMPLÈTE d'une pastille (title ET aria-label identiques) : la couleur
  // ne porte jamais seule l'information, le lecteur d'écran doit tout entendre.
  function tttDaySentence(label, entry, unit) {
    if (!entry) return `${label} : pas de donnée.`;
    const e0 = fmtE0(entry.e0, unit);
    const e0Txt = (e0 === null) ? "écart initial indisponible" : `écart initial ${e0}`;
    let core;
    if (tttCounterReset(entry)) {
      // bug-106 : la journée s'archive en « cible atteinte » avec 0 minute quand
      // le compteur de filtration a reculé. Raconter « déjà à la cible au
      // départ » serait un mensonge FAVORABLE — la durée n'a pas été mesurée.
      // Le drapeau, lui, nomme la cause dans la queue de la phrase.
      core = `durée non mesurable, ${e0Txt}`;
    } else if (entry.s === 2) {
      core = (entry.m === 0)
        ? `déjà à la cible au départ, ${e0Txt}`
        : `cible atteinte en ${fmtLong(entry.m)} de circulation, ${e0Txt}`;
    } else if (entry.s === 3) {
      core = `cible non atteinte après ${fmtLong(entry.m)} de circulation, ${e0Txt}`;
    } else if (entry.s === 1) {
      core = `journée en cours, ${fmtLong(entry.m)} de circulation depuis le départ, ${e0Txt}`;
    } else {
      core = "aucun départ enregistré";
    }
    const causes = tttFlagCauses(entry.f);
    const tail = causes.length
      ? ` Jour non comparable : ${causes.join(" · ")}.`
      : "";
    return `${label} : ${core}.${tail}`;
  }

  // Rendu des SEPT emplacements — toujours sept, du plus ancien (gauche) au plus
  // récent (droite). Un historique court n'efface pas les cases : le vide EST
  // une information (« pas de jugement rendu »), pas un trou à masquer.
  function renderTttDays(container, noteEl, captionEl, days, live, unit) {
    if (!container) return;
    container.textContent = "";
    if (noteEl) { noteEl.hidden = true; noteEl.textContent = ""; }
    if (_kpiFailed) {
      // Bande VIDÉE et légende explicite ; la ligne de valeur, elle, est
      // conservée (elle vient de /data ou du WS, pas de /get-kpi).
      if (captionEl) captionEl.textContent = "Historique indisponible";
      return;
    }
    if (captionEl) captionEl.textContent = "7 derniers jours — le plus récent à droite";

    // ALIGNEMENT PAR DATE, jamais par ordre du tableau.
    // -------------------------------------------------
    // Les sept emplacements sont les sept dates ATTENDUES (J-6 … aujourd'hui) ;
    // chaque entrée de /get-kpi est placée par ÉGALITÉ DE DATE. Un simple
    // reverse() aligné à droite décalerait d'un cran tout ce qui se trouve à
    // droite d'un trou — et il y aura des trous : appareil éteint une journée,
    // ou journée où l'heure n'était pas synchronisée (le firmware REFUSE alors
    // d'archiver une entrée datée faux, c'est voulu). Or chaque pastille NOMME
    // son jour dans son title et son aria-label : un décalage ferait annoncer
    // la mauvaise date. Un indicateur qui se trompe de jour est pire qu'un
    // indicateur absent — on lui fait confiance.
    // Ici un trou reste un trou À SA PLACE, et les jours présents gardent leur
    // date exacte.
    const byDate = new Map();
    if (Array.isArray(days)) {
      days.forEach((e) => { if (e && typeof e.d === "number") byDate.set(e.d, e); });
    }
    // Dates attendues : Date locale reculée par setDate(), qui gère seul les
    // bascules de mois, d'année et d'heure d'été. Aucun décalage recalculé à la
    // main (soustraire 86 400 000 ms casse aux changements d'heure), et même
    // source de date locale que le reste de l'UI (loadHistoricalData).
    const slots = new Array(7).fill(null);
    const slotDate = new Array(7).fill(0);   // AAAAMMJJ attendu de l'emplacement
    const slotBack = new Array(7).fill(0);   // ancienneté en jours (6 … 0)
    for (let i = 0; i < 7; i++) {
      const back = 6 - i;
      const d = new Date();
      d.setHours(12, 0, 0, 0);               // midi : à l'abri des sauts DST
      d.setDate(d.getDate() - back);
      slotDate[i] = d.getFullYear() * 10000 + (d.getMonth() + 1) * 100 + d.getDate();
      slotBack[i] = back;
      slots[i] = byDate.get(slotDate[i]) || null;
    }

    // CHOIX DÉLIBÉRÉ, non prescrit par le design — ne pas le prendre pour un
    // bug : le dernier emplacement (aujourd'hui) porte l'état VIVANT (/data +
    // WS) dès qu'un chronomètre existe, et non l'entrée « en cours » archivée
    // en NVS à t0. Cette entrée-là est figée à 0 minute jusqu'à la clôture :
    // la pastille afficherait « 0h00 » — qui se lit « déjà à la cible » — sous
    // une ligne disant « en cours — 1 h 10 ». L'écrasement garde la bande
    // cohérente avec la ligne qui la surmonte.
    if (live && typeof live.s === "number" && live.s !== 0) {
      slots[6] = { d: slotDate[6], s: live.s, m: live.m, e0: live.e0, f: live.f };
    }

    let anyFlag = false;
    const causesSeen = [];
    for (let i = 0; i < 7; i++) {
      const e = slots[i];
      const cell = document.createElement("div");
      cell.className = "ttt__day";
      cell.setAttribute("role", "listitem");

      let txt;
      if (!e) {
        cell.classList.add("ttt__day--none");
        txt = "·";
      } else if (tttCounterReset(e)) {
        // bug-106 — POURQUOI ce cas passe AVANT l'état : une journée dont le
        // compteur de filtration a reculé s'archive en s = 2 (cible atteinte)
        // avec m = 0. Sans ce test, fmtShort(0) rendrait « 0h00 » en
        // .ttt__day--good, parce que 0 <= TTT_LONG_MIN : l'ABSENCE de mesure
        // s'afficherait comme la MEILLEURE journée possible, et tirerait vers
        // le bas toute moyenne de comparaison. Ce n'est pas un échec non plus
        // (pas de .ttt__day--long) : on réemploie le vocabulaire des journées
        // sans entrée — pointillé estompé, aucune durée affichée.
        cell.classList.add("ttt__day--none");
        txt = "—";
      } else if (e.s === 2) {
        cell.classList.add((e.m <= TTT_LONG_MIN) ? "ttt__day--good" : "ttt__day--long");
        txt = fmtShort(e.m);
      } else if (e.s === 3) {
        txt = "—";  // non atteinte : aucune classe modificatrice, le style de
                    // base de .ttt__day est déjà le gris voulu (revue : il n'y
                    // a PAS de classe « --miss » dans app.css, ne pas en croire
                    // l'existence sur la foi d'un commentaire)
      } else if (e.s === 1) {
        cell.classList.add("ttt__day--pending");
        txt = fmtShort(e.m);
      } else {
        cell.classList.add("ttt__day--pending");
        txt = "—";
      }

      const causes = e ? tttFlagCauses(e.f) : [];
      if (causes.length) {
        anyFlag = true;
        cell.classList.add("ttt__day--flag");
        txt += "*";  // l'astérisque DOUBLE les hachures : jamais la couleur seule
        causes.forEach((c) => { if (!causesSeen.includes(c)) causesSeen.push(c); });
      }
      cell.textContent = txt;

      // Libellé : les deux cas partent de la DATE ATTENDUE de l'emplacement,
      // jamais de la position d'une entrée dans le tableau. Journée présente →
      // sa date en toutes lettres ; journée absente → son ancienneté (« Il y a
      // 6 jours »), qui décrit exactement le même jour.
      const back = slotBack[i];
      const rel = (back === 0) ? "Aujourd'hui" : (back === 1) ? "Hier" : `Il y a ${back} jours`;
      const label = e ? (tttDayLabel(slotDate[i]) || rel) : rel;
      const sentence = tttDaySentence(label, e, unit);
      cell.title = sentence;
      cell.setAttribute("aria-label", sentence);
      container.appendChild(cell);
    }

    // Note visible SOUS la bande, affichée seulement s'il y a au moins un
    // drapeau sur les sept jours ; elle énumère les causes RÉELLEMENT présentes.
    if (noteEl && anyFlag) {
      noteEl.hidden = false;
      noteEl.textContent = `* Jours non comparables : ${causesSeen.join(", ")}.`;
    }
  }

  // Aplatissement d'une charge /get-kpi pour UNE voie.
  // ---------------------------------------------------
  // /get-kpi sert chaque journée avec une sous-structure PAR RÉGULATION
  // ({"d":20260906,"ph":{"s":2,"m":356,…},"orp":{…}}), alors que
  // renderTttDays() lit s/m/e0/f au PREMIER niveau de l'entrée. Sans cette
  // conversion, e.s vaut undefined et les sept pastilles tombent dans le cas
  // par défaut (« en cours », « — ») — bug-111.
  function tttDaysForChannel(days, prefix) {
    // POURQUOI cette garde : null ne signifie pas « aucune journée », il
    // signifie « on ne sait pas » (échec de /get-kpi, cf. fetchKpiHistory).
    // renderTttDays s'appuie dessus pour écrire « Historique indisponible ».
    // Un .map() non gardé rendrait un tableau, donc une bande vide sous la
    // légende « 7 derniers jours » — qui se lit « rien d'enregistré ». On
    // remplacerait un aveu d'ignorance par un mensonge.
    if (!Array.isArray(days)) return days;
    const out = [];
    days.forEach((e) => {
      if (!e || typeof e.d !== "number") return;
      const c = e[prefix];
      // Voie absente de l'entrée : on ne pousse RIEN, on ne fabrique pas un
      // objet vide. byDate ne trouvera pas cette date et l'emplacement restera
      // un trou (« · ») À SA PLACE — la bande étant alignée par date, retirer
      // une entrée ne décale aucune autre (cf. commentaire ci-dessus).
      if (!c || typeof c !== "object") return;
      out.push({ d: e.d, s: c.s, m: c.m, e0: c.e0, f: c.f });
    });
    return out;
  }

  // Rendu d'UNE voie. Une seule implémentation, paramétrée par l'unité.
  function renderTttChannel(prefix, unit, data) {
    const block = $(`#${prefix}-ttt-block`);
    if (!block) return null;

    // Sentinelle de présence (même patron que updatePhDosingCard) : un firmware
    // antérieur à la 2.37.0 ne sert pas ces clés → bloc MASQUÉ, jamais un « — »
    // qui ferait croire à un indicateur en panne.
    const state = data[`${prefix}_ttt_state`];
    if (typeof state !== "number" || !isFinite(state)) { block.hidden = true; return null; }
    block.hidden = false;

    const min = data[`${prefix}_ttt_min`];
    const e0v = data[`${prefix}_ttt_e0`];
    const flags = data[`${prefix}_ttt_flag`];
    const e0 = fmtE0(e0v, unit);
    const e0Txt = (e0 === null) ? "écart initial indisponible" : `écart initial ${e0}`;

    let value = "—";
    let detail = "";
    if (state === 2) {
      if (min === 0) {
        value = "déjà à la cible au départ";
        detail = e0Txt;
      } else {
        value = `en ${fmtLong(min)}`;
        detail = `de circulation — ${e0Txt}`;
      }
    } else if (state === 1) {
      value = `en cours — ${fmtLong(min)}`;
      detail = `de circulation depuis le départ du jour — ${e0Txt}`;
    } else if (state === 3) {
      value = "non atteinte";
      detail = `après ${fmtLong(min)} de circulation — ${e0Txt}`;
    } else {
      // État 0 : deux situations très différentes. L'heure système tranche.
      if (data.time_synced === false) {
        value = "indicateur suspendu";
        detail = "heure système non synchronisée — aucune mesure enregistrée";
      } else {
        value = "en attente du départ";
        detail = "le chronomètre démarre au premier relevé valide, filtration en marche";
      }
    }

    // Drapeau sur le JOUR COURANT : la valeur reste inchangée, le détail porte
    // le suffixe. Plusieurs causes simultanées sont jointes par « · ».
    const causes = tttFlagCauses(flags);
    if (causes.length && state !== 0) {
      detail += ` — jour non comparable (${causes.join(" · ")})`;
    }

    const valEl = $(`#${prefix}-ttt-value`);
    const detEl = $(`#${prefix}-ttt-detail`);
    if (valEl) valEl.textContent = value;
    if (detEl) detEl.textContent = detail ? " " + detail : "";

    renderTttDays($(`#${prefix}-ttt-days`), $(`#${prefix}-ttt-note`), $(`#${prefix}-ttt-caption`),
                  tttDaysForChannel(_kpiDays, prefix),
                  { s: state, m: min, e0: e0v, f: flags }, unit);
    return state;
  }

  // Point d'entrée UNIQUE, appelé là où updatePhDosingCard() l'est : après
  // une charge /data ET après une trame WS `sensor_data` (les deux passent par
  // loadSensorData → updateDetailSections).
  function updateTimeToTargetDisplay() {
    const data = latestSensorData || {};
    const sPh = renderTttChannel("ph", "ph", data);
    const sOrp = renderTttChannel("orp", "orp", data);
    if (sPh === null && sOrp === null) return;  // firmware antérieur : rien à faire

    // Relecture de la mémoire 7 jours : au premier rendu, puis UNIQUEMENT au
    // changement d'un état ou de jour civil. Jamais de polling — l'anneau NVS
    // n'est écrit que deux fois par jour et par régulation.
    const sig = `${sPh}|${sOrp}|${new Date().toDateString()}`;
    const changed = (sig !== _kpiSig);
    _kpiSig = sig;
    if (_kpiSuppressFetch) return;
    if (changed || (_kpiDays === null && !_kpiFailed)) fetchKpiHistory();
  }

  // feature-091 : rendu de la ligne « Dernière pause » (Réglages > Régulation)
  // — l'arrêt d'injection qui suit un arrêt d'eau, le temps que l'eau neuve
  // arrive jusqu'aux sondes (même texte d'aide que le bloc ci-dessus dans
  // index.html). Champs firmware servis à l'IDENTIQUE sur /data et WS (noms
  // INCHANGÉS, contrat public) :
  //   measures_purge_last_duration_s → durée de la dernière pause (s) ;
  //   measures_purge_last_end        → motif de fin : 0 aucune, 1 normale,
  //                                    3 dispensée (arrêt d'eau STRICTEMENT
  //                                    < kWaterStopGraceS = 180 s / 3 min,
  //                                    borne STRICTE : à 180 s pile, PAS de
  //                                    dispense — ne pas recopier l'ancien
  //                                    seuil kShortOutageS = 10 s, supprimé).
  // Sentinelle de présence : measures_purge_last_end numérique. Absent
  // (firmware antérieur) → « — », rien d'inventé.
  // feature-139 (v2.62.0, correctif code-reviewer du 2026-10-08) : vocabulaire
  // VISIBLE purgé de « purge »/« stabilité »/« dispensée » (seul le NOM du
  // champ firmware conserve ces mots, jamais affichés tels quels). Motifs 2
  // (plafond 90 min) et 4 (relancée) RETIRÉS de l'émission firmware — le
  // mécanisme de re-purge et l'analyse de stabilité ont disparu, et l'alerte
  // MQTT measures_purge_timeout qu'ils signalaient aussi n'existe plus. Les
  // bandeaux #measures_purge_timeout_banner[_orp] sont retirés du DOM pour la
  // même raison. Les `case` 2/4 ci-dessous restent en lecture DÉFENSIVE (un
  // firmware ancien ou un rollback peut encore les émettre) : texte NEUTRE et
  // COURT, qui ne prétend décrire ni un mécanisme vivant ni un jargon interne
  // (« motif hérité » a été jugé incompréhensible, correctif du 2026-10-08).
  function updateMeasuresPurgeDisplay(data) {
    data = data || {};
    const lastEl = $("#measures_purge_last");
    const end = data.measures_purge_last_end;
    if (typeof end !== "number" || !isFinite(end)) {
      if (lastEl) { lastEl.textContent = "—"; lastEl.style.color = ""; }
      return;
    }
    const dur = data.measures_purge_last_duration_s;
    const min = (typeof dur === "number" && isFinite(dur)) ? Math.round(dur / 60) : null;
    const minTxt = (min === null) ? "—" : `${min} min`;
    let txt = "—";
    switch (end) {
      case 1: txt = `${minTxt} — terminée normalement`; break;
      case 2: txt = `${minTxt} — terminée`; break;
      case 3: txt = "pas d'attente — coupure d'eau de moins de 3 minutes"; break;
      case 4: txt = `${minTxt} — terminée`; break;
      default: txt = "—";
    }
    if (lastEl) {
      lastEl.textContent = txt;
      lastEl.style.color = (end === 2) ? "var(--warn)" : "";
    }
  }

  // ========== SECTIONS DÉTAILLÉES ==========
  function updateDetailSections() {
    const config = window._config || {};

    // feature-092 : cartes « Débit d'injection » pH et ORP — rendu EN TÊTE et
    // sous try/catch, même précaution que le statut MQTT dans _onWsSensorData.
    // Motif (hérité de la v2.26.0) : c'est le SEUL point de décision de la
    // visibilité de ces cartes, et il était appelé en DERNIER, après quatre
    // autres rafraîchisseurs. Une exception dans l'un d'eux (donnée inattendue
    // d'un firmware donné) laissait le bloc masqué EN SILENCE — sans rien casser
    // de visible par ailleurs. Une carte neuve qui « ne s'affiche jamais » est
    // exactement la trace que laisserait ce défaut. updatePhDosingCard() pilote
    // AUSSI la capacité journalière et l'alerte de sous-dimensionnement, qui
    // vivent hors de la carte (ex-updatePhActuatorLimitDisplay).
    try { updatePhDosingCard(); } catch (e) { console.error('[ph-dosage] rendu:', e); }
    try { updateOrpDosingCard(); } catch (e) { console.error('[orp-dosage] rendu:', e); }

    // feature-096 : temps d'atteinte de cible (pages pH ET ORP) — appelé
    // exactement là où updatePhDosingCard() l'est, donc après une charge
    // /data ET après une trame WS `sensor_data` (les deux passent ici via
    // loadSensorData). Même précaution try/catch : un rendu qui lance ne doit
    // jamais masquer les rubriques suivantes en silence.
    try { updateTimeToTargetDisplay(); } catch (e) { console.error('[ttt] rendu:', e); }

    // feature-091 : durée/motif de la dernière pause — même source
    // (latestSensorData : /data ET WS), même précaution try/catch.
    try { updateMeasuresPurgeDisplay(latestSensorData); } catch (e) { console.error('[purge] rendu:', e); }

    // feature-155 : carte « Correction manuelle de la mesure » — référence
    // VIVANTE (#4), rafraîchie à chaque trame comme les rendus ci-dessus.
    try { renderPhTuneCard(); } catch (e) { console.error('[ph-tune] rendu:', e); }

    // === FILTRATION ===
    // feature-147 : le texte de mode ("Mode Auto"/.../"Hors gel") est géré par
    // updateFiltrationBadges() ci-dessous (un seul calcul, header de carte).
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

    // feature-146 : Démarrer/Arrêter changent désormais le MODE — en mode
    // Arrêt, « Démarrer » est précisément la sortie du mode : plus jamais grisés.

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

  // feature-146 : les boutons Démarrer/Arrêter (tableau de bord ET réglages)
  // ne font plus qu'une chose — changer le MODE de filtration (Marche/Arrêt).
  // Les champs filtration_force_on/_off ont disparu du firmware ; un seul
  // chemin sert désormais les DEUX paires de boutons.
  async function setFiltrationRunMode(mode) {
    const payload = { filtration_mode: mode };
    // feature-147 : la grille 2x2 appelle désormais ce chemin pour les 4 modes
    // (auto/manual compris, plus seulement force/off). Textes force/off
    // INCHANGÉS (non-régression page Filtration) ; auto/manual ajoutés.
    const modeLabels = { auto: 'Auto', manual: 'Programme', force: 'Marche', off: 'Arrêt' };
    const label = modeLabels[mode] || mode;
    try {
      const result = await sendConfig(payload);
      if (!result) {
        const errorMsg = mode === "force" ? "Erreur lors du démarrage"
          : mode === "off" ? "Erreur lors de l'arrêt"
          : `Erreur lors du passage en mode ${label}`;
        showToast(errorMsg, "error");
        return false;
      }
      // feature-147 : prédiction partagée avec bindFiltrationManualSave() —
      // correcte pour les 4 modes (force/off inclus), contrairement à l'ancien
      // `mode === "force"` qui ignorait auto/manual.
      filtrationRunningOverride = predictFiltrationRunning(Object.assign({}, window._config, payload));
      if (window._config) window._config.filtration_mode = mode;
      const modeSelect = $("#filtration_mode");
      if (modeSelect) modeSelect.value = mode;
      updateFiltrationControls();
      updateFiltrationBadges();
      updateDetailSections();
      updateStatusCards();
      const successMsg = mode === "force" ? "Filtration démarrée"
        : mode === "off" ? "Filtration arrêtée"
        : `Mode ${label} activé`;
      showToast(successMsg, "success");
      loadConfig();
      setTimeout(() => loadSensorData({ force: true, source: "filtration-mode" }), 500);
      return true;
    } catch (e) {
      console.error('Erreur changement de mode filtration:', e);
      showToast("Erreur de connexion", "error");
      return false;
    }
  }

  function bindDetailActions() {
    // feature-147 : 4 boutons de mode (grille 2x2) remplacent Démarrer/Arrêter
    // — même chemin setFiltrationRunMode() existant, aucun second chemin.
    ['auto', 'manual', 'force', 'off'].forEach((m) => {
      const btn = $(`#filtration-mode-btn-${m}`);
      if (btn) btn.addEventListener('click', () => setFiltrationRunMode(m));
    });

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
            updateRescueCard(); // feature-140 : la suggestion Boost du bandeau secours dépend aussi de l'état du Boost
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
            updateRescueCard(); // feature-140 : la suggestion Boost du bandeau secours dépend aussi de l'état du Boost
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

    // Bouton Arrêter de la Chloration de secours (feature-140), décalqué du
    // Boost. v2.55.3 : vit désormais DANS #rescue-active-banner (plus dans le
    // widget « Modes temporaires », qui ne porte plus aucune trace du mode).
    // L'activation se fait uniquement depuis le bandeau de blocage
    // (_activateRescueChlorine, cf. _renderDoseBlockRow).
    const rescueStopBtn = $("#rescue-active-banner-stop");

    if (rescueStopBtn) {
      rescueStopBtn.addEventListener('click', async () => {
        try {
          const response = await authFetch("/rescue/stop", { method: "POST" });
          if (response.ok) {
            if (latestSensorData) { latestSensorData.rescue_until = 0; latestSensorData.rescue_dosing = false; }
            updateRescueCard();
            showToast("Chloration de secours désactivée", "success");
          } else {
            showToast("Erreur lors de l'arrêt de la chloration de secours", "error");
          }
        } catch (e) {
          console.error('Erreur arrêt chloration de secours:', e);
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
    // feature-154 (B4) : plus qu'UN SEUL jugement de santé dans l'application —
    // classifyPhCriterion() contre les seuils SERVIS (_phHealth), plus aucun
    // seuil recopié ici (85/90/95/15/30 disparus). B8 : un zéro null/NaN est
    // 'indisponible', JAMAIS 0 mV — un module EZO qui ne rapporte que deux
    // champs (atlas_ezo.cpp:271, condition C1 du gate) laisserait sinon un
    // faux vert PERMANENT sur un critère jamais mesuré.
    const zeroNum = (zero == null || isNaN(zero)) ? null : Number(zero);
    const acidJudg = _phHealth
      ? classifyPhCriterion(acidNum, _phHealth.slopeMin, _phHealth.slopeMax) : 'indisponible';
    const baseJudg = _phHealth
      ? classifyPhCriterion(baseNum, _phHealth.slopeMin, _phHealth.slopeMax) : 'indisponible';
    const zeroJudg = _phHealth
      ? classifyPhCriterion(zeroNum, -_phHealth.zeroMax, _phHealth.zeroMax) : 'indisponible';

    // C3 (gate pool-chemistry, vocabulaire BLOQUANT) : « Sonde à remplacer »
    // disparaît — il a accusé une sonde SAINE le 2026-08-25 (décalage externe)
    // et récidivé le 2026-10-11 (jonction de référence, membrane intacte). Le
    // libellé retenu par l'utilisateur n'accuse AUCUNE pièce et renvoie au
    // détail (3 pastilles) pour savoir lequel des trois critères est en cause.
    if (acidJudg === 'hors' || baseJudg === 'hors' || zeroJudg === 'hors') {
      return { cls: 'bad', label: 'Calibré · critère hors norme' };
    }
    // C2 (gate, bloquant) : le chip ne peut être vert QUE si les TROIS
    // critères sont mesurés ET dans la plage. Un seul 'indisponible' interdit
    // le vert — jamais un faux vert sur un critère non mesuré.
    if (acidJudg === 'indisponible' || baseJudg === 'indisponible' || zeroJudg === 'indisponible') {
      return { cls: 'unknown', label: 'Calibré · critère indisponible' };
    }
    return { cls: 'good', label: 'Calibré · sain' };
  }

  // feature-138 (B1/B2) : helper d'âge PARTAGÉ — il était écrit deux fois
  // (ici et dans _filterAgeText) et le sera une troisième fois pour la durée
  // de blocage du dosage (bandeau #dose-block-banner) : on ne lisse ni ne
  // moyenne, la durée est toujours recalculée depuis l'horodatage source.
  // `preposition` porte le verbe complet ("il y a", "relevé il y a", "bloqué
  // depuis"...) ; `instantMs`/`instantText`, si fournis, figent les toutes
  // premières secondes (jamais utilisé pour une durée de BLOCAGE : "bloqué
  // depuis à l'instant" n'aurait pas de sens).
  function _elapsedText(ms, preposition, instantMs = null, instantText = "à l'instant") {
    if (ms == null || !isFinite(ms) || ms < 0) return null;
    if (instantMs != null && ms < instantMs) return instantText;
    const sec = Math.floor(ms / 1000);
    if (sec < 60) return `${preposition} ${sec} s`;
    const minutes = Math.floor(sec / 60);
    if (minutes < 60) return `${preposition} ${minutes} min`;
    const hours = Math.floor(minutes / 60);
    const remMin = minutes - hours * 60;
    if (hours < 24) {
      return remMin === 0 ? `${preposition} ${hours} h` : `${preposition} ${hours} h ${remMin} min`;
    }
    const days = Math.floor(hours / 24);
    const remHours = hours - days * 24;
    return remHours === 0 ? `${preposition} ${days} j` : `${preposition} ${days} j ${remHours} h`;
  }

  // Met en forme l'âge en libellé lisible : "il y a 14 h", "il y a 12 min", "jamais"
  function formatProbeAge(ageMs) {
    if (ageMs == null || ageMs >= PH_SLOPE_NEVER) return 'jamais';
    return _elapsedText(ageMs, 'il y a', 60000);
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

    // Reset les classes de variantes uniquement. feature-154 (B4) : 'warn2'
    // retiré — classifyPhProbe() ne le produit plus (dégradé 4 niveaux disparu
    // pour la santé pH), mais la classe CSS .chip--probe-warn2 reste définie
    // (sert au chip ORP / chip--cal).
    chip.classList.remove(
      'chip--probe-good', 'chip--probe-warn',
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

    // feature-025 : ligne brute discrète (brut/maj) de la carte « Lecture pH ».
    updateFilterSub('ph', json);

    // Mettre à jour le contenu du modal si ouvert
    const modal = $("#ph-probe-modal");
    if (modal && modal.open) _renderPhProbeModalValues(json);
  }

  // feature-154 (B1) : glyphe + libellé .sr-only par état de pastille. Glyphe
  // en aria-hidden, le sens est porté par .sr-only (classe déjà présente,
  // app.css:3424) — jamais la couleur seule (daltonisme rouge-vert).
  const PH_PASTILLE_GLYPH = { sain: '✓', hors: '✕', indisponible: '–' };
  const PH_PASTILLE_SR = { sain: 'sain', hors: 'hors norme', indisponible: 'indisponible' };

  // Rend une pastille à partir d'un état classifyPhCriterion(), ou AUCUNE
  // pastille (élément masqué via [hidden]) si state est null — cas « sonde
  // non calibrée » : la valeur affiche déjà « -- ». Non interactive (pas de
  // tabindex/role) : purement informative, ne conditionne aucun dosage (C6).
  function _renderPhPastille(el, state) {
    if (!el) return;
    if (state == null) {
      el.hidden = true;
      el.className = 'probe-pastille';
      el.innerHTML = '';
      return;
    }
    el.hidden = false;
    el.className = `probe-pastille probe-pastille--${state}`;
    el.innerHTML = `<span aria-hidden="true">${PH_PASTILLE_GLYPH[state]}</span><span class="sr-only">${PH_PASTILLE_SR[state]}</span>`;
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

    const acidPastille = $("#ph-probe-acid-pastille");
    const basePastille = $("#ph-probe-base-pastille");
    const zeroPastille = $("#ph-probe-zero-pastille");

    if (calPoints != null && calPoints < 2) {
      acidEl.textContent = '--';
      baseEl.textContent = '--';
      zeroEl.textContent = '--';
      // B1 (table ux-ui-designer) : sonde non calibrée → AUCUNE pastille.
      _renderPhPastille(acidPastille, null);
      _renderPhPastille(basePastille, null);
      _renderPhPastille(zeroPastille, null);
    } else {
      acidEl.textContent = fmtPct(acid);
      baseEl.textContent = fmtPct(base);
      zeroEl.textContent = fmtMv(zero);

      // feature-154 (B2/B8) : classement par critère contre les seuils SERVIS
      // (_phHealth). Aucun repli : _phHealth null ⇒ 'indisponible' pour les
      // trois (classifyPhCriterion le garantit structurellement).
      const acidNum = (acid == null || isNaN(acid)) ? null : Number(acid);
      const baseNum = (base == null || isNaN(base)) ? null : Number(base);
      const zeroNum = (zero == null || isNaN(zero)) ? null : Number(zero);
      _renderPhPastille(acidPastille, _phHealth
        ? classifyPhCriterion(acidNum, _phHealth.slopeMin, _phHealth.slopeMax) : 'indisponible');
      _renderPhPastille(basePastille, _phHealth
        ? classifyPhCriterion(baseNum, _phHealth.slopeMin, _phHealth.slopeMax) : 'indisponible');
      _renderPhPastille(zeroPastille, _phHealth
        ? classifyPhCriterion(zeroNum, -_phHealth.zeroMax, _phHealth.zeroMax) : 'indisponible');
    }
    ageEl.textContent = formatProbeAge(ageMs);

    // feature-152 (B6) : 3 champs de diagnostic de la chaîne de mesure,
    // absorbés ici (plus de modal « filtre » séparé) — valeur brute et valeur
    // RETENUE (celle qui décide, cf. getPhFiltered côté firmware), + fraîcheur.
    const fmRawEl = $("#ph-fm-raw");
    if (fmRawEl) fmRawEl.textContent = _fmtFilterVal('ph', json?.phRaw);
    const fmFilEl = $("#ph-fm-filtered");
    if (fmFilEl) fmFilEl.textContent = _fmtFilterVal('ph', json?.phFiltered);
    const fmAgeEl = $("#ph-fm-age");
    if (fmAgeEl) fmAgeEl.textContent = _filterAgeText('ph');
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

  // ---- feature-025 (réduit par feature-152) : ligne brute + modal diagnostic pH/ORP ----
  // La valeur principale (#xx_current_value) affiche déjà la valeur OFFICIELLE
  // (json.ph/json.orp). Les chips et modal « état filtre » séparés ont disparu
  // (plus d'état bloquant à diagnostiquer : measures_valid/le badge capteur
  // couvrent déjà la montée en régime, cf. measureStateSegments()). Il ne reste
  // que la ligne brute discrète et trois champs de diagnostic (brut, valeur
  // retenue, âge), absorbés dans le modal sonde de chaque voie.
  const _lastRawAt = { ph: 0, orp: 0 };

  function _filterRawIsValid(v) { return v != null && !(typeof v === 'number' && isNaN(v)); }

  function _fmtFilterVal(prefix, v) {
    if (!_filterRawIsValid(v)) return '--';
    return prefix === 'orp' ? `${Math.round(v)} mV` : Number(v).toFixed(2);
  }

  function _filterAgeText(prefix) {
    const lastAt = _lastRawAt[prefix];
    if (!lastAt) return '--';
    const age = Date.now() - lastAt;
    if (age < 3600000) return _elapsedText(age, 'il y a', 10000);
    return formatProbeAge(age);
  }

  // feature-152 (B6) : « médiane » retiré de cette ligne — étape interne du
  // lissage, pas un fait à lire en permanence sur l'écran principal.
  function _renderFilterSub(prefix, json) {
    const el = $(`#${prefix}-filter-sub`);
    if (!el) return;
    const raw = _fmtFilterVal(prefix, json?.[prefix + 'Raw']);
    el.textContent = `brut ${raw} · maj ${_filterAgeText(prefix)}`;
  }

  // Met à jour la ligne brute discrète (brut/maj) + l'horodatage interne de
  // fraîcheur. Appelé depuis loadSensorData après chaque push WS / fetch /data.
  function updateFilterSub(prefix, json) {
    const raw = json?.[prefix + 'Raw'];
    if (_filterRawIsValid(raw)) _lastRawAt[prefix] = Date.now();
    _renderFilterSub(prefix, json);
  }

  // ==========================================================================
  // v2.25.1 — Cause de refus de dosage : on traduit un CODE, plus jamais une phrase
  // ==========================================================================
  // L'ancienne version devinait la catégorie par includes() sur le texte français.
  // Deux défauts observés en production, symétriques et tous deux graves :
  //   - « mesure filtrée indisponible (NaN) » contenait « indispo » → affichage
  //     « EZO indisponible » pendant ~6 min au boot, sonde parfaitement saine ;
  //   - « warmup / EZO injoignable » contenait « warm » ET « ezo », « warm » testé
  //     d'abord → une VRAIE panne d'EZO s'affichait comme une mise en place
  //     normale des mesures, c'est-à-dire une panne matérielle maquillée en
  //     état normal.
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
  // chip : libellé court pour la pastille « Débit d'injection » (absent = pas d'incidence).
  // stab : cause appartenant à la famille « stabilisation des mesures », déjà
  //        annoncée par le badge capteur (cf. measureStateSegments) — sert à ne
  //        pas afficher deux messages concurrents côte à côte.
  //
  // feature-139 (v2.62.0, lot B puis correctif gate du 2026-10-08) : le code
  // 18 (ReadingNaNGateClosed, commun à pH et ORP depuis feature-152) —
  // l'ATTENTE avant la première mesure jugée fiable après une coupure
  // d'eau/filtration — passe d'un libellé STATIQUE qui ne parlait que
  // d'instabilité à un texte DÉTERMINISTE qui ANNONCE SA FIN.
  // ⚠️ Correctif gate (2e passage, 2026-10-08) : un PREMIER calcul côté client
  // (`600 − measures_stab_elapsed_s`, mirroir local de kPurgeFloorMs) MENTAIT
  // d'un facteur 10 sur le cas le plus fréquent de ce lot — un cycle DISPENSÉ
  // (coupure courte) a `elapsed` figé à 0, mais la porte rouvre en 60 s, car
  // c'est un AUTRE plancher qui domine alors (gel du filtre de mesure), pas
  // celui de la purge. Le firmware est le SEUL à connaître le max des deux
  // plafonds : il expose désormais `measures_dose_available_in_s` (secondes,
  // parité /data ↔ WS), et ce texte ne calcule plus rien — il LIT ce champ.
  // AUCUNE constante de plancher n'est dupliquée ici : c'est exactement cette
  // duplication qui a produit le défaut corrigé ci-dessus.
  // Vocabulaire SIMPLE à dessein (retour utilisateur : « stabilisation » /
  // « purge » / « représentativité » jugés incompréhensibles) : on dit ce qui
  // se passe et quand ça finit, jamais plus.
  // ⚠️ Correctif code-reviewer (3e occurrence du même défaut dans ce lot,
  // 2026-10-08) : filtration ARRÊTÉE ⇒ le code 18 (ReadingNaNGateClosed) reste
  // émis EN PERMANENCE, et `measures_dose_available_in_s` ne peut PLUS être un
  // nombre fixe (il n'y a pas de date de reprise tant que rien ne circule) :
  // le firmware OMET désormais ce champ dans ce cas précis (pas de sentinelle
  // numérique — la marge WS ne le permet pas). TROIS cas à distinguer, pas
  // deux : champ présent > 0, champ présent === 0, champ ABSENT. Le cas
  // « absent » lui-même se sépare en DEUX causes selon `water_present` :
  //   - `water_present === false` (champ fiable depuis feature-061, bien
  //     antérieur à `measures_dose_available_in_s`) ⇒ c'est la filtration
  //     arrêtée qui explique l'omission, délai réellement inconnu ;
  //   - toute autre valeur (true / absent) ⇒ l'omission ne peut venir que d'un
  //     firmware antérieur à ce champ (filtration par ailleurs active ou
  //     indéterminée) : repli générique, jamais de calcul inventé.
  // Ce texte NE CONTREDIT PAS le reste de l'écran : measureStateSegments()
  // affiche déjà « Filtration arrêtée » avec PRIORITÉ sur toute cause de
  // dosage quand water_present === false (chantier simplification : c'est
  // désormais la SEULE source de ce texte, le badge capteur ayant absorbé
  // l'ancien widget dosing-status) — ce texte n'apparaît donc que là où ce
  // segment ne s'affiche pas (pastille, chip filtre, bandeau de blocage),
  // sans jamais se contredire.
  function _measuresWaitText(json) {
    const remainingS = json && json.measures_dose_available_in_s;
    if (typeof remainingS === 'number' && isFinite(remainingS) && remainingS >= 0) {
      if (remainingS <= 0) return 'Reprise automatique imminente';
      const remainingMin = Math.max(1, Math.ceil(remainingS / 60));
      return `Reprise automatique dans ${remainingMin} min`;
    }
    if (json && json.water_present === false) {
      return 'Reprise à la remise en route de la filtration';
    }
    // Firmware antérieur à ce champ (filtration par ailleurs active ou
    // indéterminée) : repli statique correct, jamais de calcul sur une
    // valeur invalide.
    return 'Mesure en pause, reprise automatique à venir';
  }
  const DOSE_BLOCK_CODE_GUARD = 250;
  // feature-152 (refonte de la chaîne de mesure) : codes 4 (FilterNotReady) et
  // 5 (FilterUnstable) RETIRÉS — les deux gardes séparées (disponibilité et
  // fiabilité du signal) ont disparu, la VALEUR OFFICIELLE encapsule désormais
  // directement warmup/sonde figée/péremption/porte fermée (NaN dans tous ces
  // cas). Codes
  // 19-22 RETIRÉS aussi : ORP partage désormais EXACTEMENT les mêmes codes
  // 15-18 que pH (garde unique isnan(reading), qualifiée par le même état de
  // filtre pour les deux voies). Ces six numéros ne sont JAMAIS recyclés — un
  // appareil non mis à jour afficherait l'ancien libellé sur un code réattribué.
  const DOSE_BLOCK_CODES = {
    1:  { tone: 'fault',   label: 'Watchdog inactif — dosage suspendu' },
    2:  { tone: 'normal',  label: 'Filtration arrêtée' },
    3:  { tone: 'unknown', label: 'Mesure indisponible — cause indéterminée' },
    // 4, 5 : RETIRÉS (feature-152), ne jamais recycler — voir note ci-dessus.
    6:  { tone: 'fault',   label: 'Calibration insuffisante' },
    7:  { tone: 'normal',  label: 'Stabilisation post-calibration' },
    8:  { tone: 'normal',  label: 'Mélange en cours' },
    9:  { tone: 'normal',  label: 'Régulation non automatique' },
    10: { tone: 'normal',  label: 'Limite journalière atteinte' },
    11: { tone: 'normal',  label: 'Limite horaire atteinte' },
    12: { tone: 'normal',  label: 'Limite cycles/jour atteinte — reprise à minuit' },
    13: { tone: 'normal',  label: 'Anti-rafale — patienter avant la prochaine injection' },
    14: { tone: 'normal',  label: 'Anti-rafale (15 min) — patienter avant la prochaine injection' },
    // Qualification de la garde 3 (lecture officielle NaN), COMMUNE à pH et
    // ORP depuis feature-152 (une seule garde isnan(reading), qualifiée par
    // l'état interne du filtre). Durée DÉTERMINISTE et connue d'avance
    // (kSensorFilterMedianWindow échantillons × cadence de lecture) — dite en
    // ordre de grandeur, PAS en engagement ferme (une sonde muette prolonge
    // l'attente), donc AUCUN champ /data ou WS à ajouter, et surtout AUCUN
    // décompte fabriqué côté client (un rechargement le remettrait à zéro et
    // mentirait).
    15: { tone: 'normal',  label: 'Les mesures se mettent en place — environ une minute', chip: 'Mesures en place', stab: true },
    16: { tone: 'fault',   label: 'Sonde figée — vérifier la sonde', chip: 'Sonde figée' },
    17: { tone: 'fault',   label: 'Plus aucune mesure acceptée — vérifier la sonde', chip: 'Sonde muette' },
    // feature-139 (v2.62.0, lot B) : label/chip ci-dessous = REPLI STATIQUE
    // seulement (firmware antérieur à measures_stab_elapsed_s, ou appel qui
    // n'aurait pas accès à `json`) — l'affichage normal passe par
    // _measuresWaitText(json), voir les 2 points d'appel (_doseBlockInfo,
    // _renderDosePill) : ne pas reformuler ici sans reformuler là-bas.
    18: { tone: 'normal',  label: 'Reprise automatique à venir', chip: 'Reprise à venir', stab: true },
    // feature-092 — sentinelle 250 (cause de GARDE) : le message firmware porte
    // des valeurs runtime (seuils, mesure) et reste cité tel quel ; la pastille
    // des cartes « Débit d'injection » a besoin d'un libellé COURT, il vit donc
    // ici, dans la source unique.
    // `chip` : libellé court consommé par _renderDosePill() (pastille des
    // cartes « Débit d'injection »), PAS un chip d'état filtre (retiré).
    // `pillOrp` / `toneOrp` : côté chlore, cette garde est l'inhibition sur pH
    // élevé. État d'ATTENTE normal, GRIS — jamais ambre, jamais rouge (verdict
    // pool-chemistry feature-092 : un écart de chlore pendant un pH haut n'est
    // pas une panne, c'est de la chimie).
    250: {
      tone: 'fault', label: 'Sécurité pH/ORP active', pill: 'Sécurité pH/ORP active',
      pillOrp: 'pH trop haut, chlore peu actif', toneOrp: 'normal',
    },
  };

  // Chantier simplification — correspondance (tone, code) → variante du badge
  // capteur, CÔTÉ JS UNIQUEMENT. Ne touche PAS à DOSE_BLOCK_CODES ci-dessus
  // (sert aussi au chip filtre et à la pill « Débit d'injection »). Verdict du
  // gate pool-chemistry, après arbitrage :
  //   - fault/unknown → danger (le capteur ou la mesure est en cause) ;
  //   - 13/14 (anti-rafale) → info, TOUJOURS, même si tone='normal' générique :
  //     « c'est le métronome de la boucle, il se déclenche plusieurs fois par
  //     heure sur un appareil sain ; les passer orange viderait l'orange de
  //     son sens » — refusé explicitement par le gate ;
  //   - reste du tone 'normal' → info (marche attendue).
  // Revue code-reviewer : la branche 10/11/12 → warning a été RETIRÉE — morte
  // (ces 3 codes sont dans DOSE_BLOCK_CODE_ALREADY_SOURCED, jamais passés
  // ici ; 11/12 sont traités explicitement, en combinaison, dans
  // dosingLimitSegments()).
  function doseBlockVariant(tone, code) {
    if (tone === 'fault' || tone === 'unknown') return 'danger';
    if (code === 13 || code === 14) return 'info';
    return (tone === 'warning') ? 'warning' : 'info';
  }

  // feature-148 (B3) : ce `Set` répond à UNE SEULE question, et son nom prête
  // à confusion avec une autre, voisine mais distincte — les deux coexistent
  // et NE SE REMPLACENT PAS :
  //   - DOSE_BLOCK_CODE_ALREADY_SOURCED (ici) : « ce fait est déjà poussé
  //     ailleurs DANS LE BADGE, par un chemin dédié qui le dit mieux (décompte
  //     à la seconde, flag déjà testé…) » — une question STATIQUE, par code,
  //     vraie quelle que soit la page.
  //   - doseBlockBannerCarries() (définie plus bas, à côté du bandeau) :
  //     « cette cause est-elle EFFECTIVEMENT affichée par le bandeau du
  //     TABLEAU DE BORD en ce moment ? » — une question DYNAMIQUE, qui dépend
  //     de la vue (le bandeau n'existe que sur le tableau de bord, jamais sur
  //     /ph ou /orp) et de l'état courant (fusion, carte masquée…).
  // Invariant qui rend la seconde question sûre sans filtre par code : SEULES
  // les lignes 3123/3133 (badges pH/ORP du tableau de bord) appellent
  // buildSensorSegments() → dosingLimitSegments() → doseBlockBannerCarries().
  // Aucune page dédiée n'y passe (vérifié : /ph et /orp n'appellent que
  // measureStateSegments(), cf. commentaire plus bas et feature-148 §« audit
  // corrigé »). Ne JAMAIS étendre ce `Set` pour fermer le trou de feature-148
  // — c'est doseBlockBannerCarries() qui le ferme, précisément parce qu'il
  // n'a besoin d'AUCUNE liste de codes.
  //
  // Faits déjà sourcés ailleurs par un chemin dédié — SOURCE UNIQUE, on ne les
  // repousse pas une seconde fois depuis la classification générique
  // DOSE_BLOCK_CODES (sous peine de dupliquer le même fait avec un texte
  // différent, le défaut qu'on a passé la journée à corriger) :
  //   2  Filtration arrêtée        → measureStateSegments() (water_present)
  //   3  Mesure indisponible (cause indéterminée) → subsumé par « Capteur
  //      indisponible » (valeur ph/orp null), même fait pour l'utilisateur
  //   7  Stabilisation post-calibration → dosingStabOrMixSegment() : décompte
  //      à la seconde RESTAURÉ (refus coordinateur du libellé statique figé —
  //      exactement le défaut corrigé ce matin sur le code 15, et la
  //      demande la plus constante de l'utilisateur : dire combien de temps).
  //   8  Mélange en cours           → idem, « Pause mélange : M min S s ».
  //   10 Limite journalière        → flag *_limit_reached (updateSensorBadges)
  //   11 Limite horaire            → traité explicitement (règle de combinaison)
  //   12 Limite cycles/jour        → idem
  //   15,18 (famille « stab »)     → measureStateSegments() (measures_valid)
  const DOSE_BLOCK_CODE_ALREADY_SOURCED = new Set([2, 3, 7, 8, 10, 11, 12, 15, 18]);

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
      //
      // ⚠️ Le TON dépend de la VOIE, et l'ignorer crie à la panne sur un cas
      // bénin. Côté chlore, cette garde n'est pas un défaut : c'est l'inhibition
      // sur pH indisponible ou trop haut — transitoire au réamorçage du filtre,
      // normale en régulation. La table porte déjà `toneOrp` pour ce code ; il
      // n'était lu que par le rendu des pastilles, pas par le bandeau, qui
      // affichait donc un bandeau ROUGE pendant la stabilisation suivant un
      // simple redémarrage (constaté en production le 2026-10-05, v2.55.0).
      const g = DOSE_BLOCK_CODES[code];
      const guardTone = (prefix === 'orp' && g && g.toneOrp) ? g.toneOrp : 'fault';
      return { code, label: text || 'Sécurité pH/ORP active', tone: guardTone };
    }
    // feature-139 (v2.62.0, lot B) : code 18 — libellé DÉTERMINISTE calculé
    // depuis measures_stab_elapsed_s (cf. _measuresWaitText ci-dessus), jamais
    // le texte statique de la table (devenu simple repli hors-ligne).
    if (code === 18) {
      const e = DOSE_BLOCK_CODES[code];
      return { code, label: _measuresWaitText(json), tone: e ? e.tone : 'normal' };
    }
    const e = DOSE_BLOCK_CODES[code];
    if (e) return { code, label: e.label, tone: e.tone };
    return { code, label: `Cause inconnue (code ${code})`, tone: 'unknown' };
  }

  // ---- feature-138 (B2/B3) : bandeau #dose-block-banner, hors panneau dev ----
  // Réutilise STRICTEMENT la taxonomie DOSE_BLOCK_CODES / _doseBlockInfo()
  // ci-dessus : aucune nouvelle classification.
  // Code 2 = « Filtration arrêtée » (cf. DOSE_BLOCK_CODES) : cause bénigne au
  // sens chimique, condition C6 du gate pool-chemistry pour B3.
  const DOSE_BLOCK_FILTRATION_STOPPED_CODE = 2;
  // kDoseBlockedForMaxS côté firmware (constants.h) : au-delà, la durée réelle
  // n'est plus connue, l'UI rend « ≥ 24 h », jamais une estimation.
  const DOSE_BLOCKED_FOR_MAX_S = 86400;

  function _doseBlockDurationText(durationS) {
    if (typeof durationS !== 'number' || !isFinite(durationS) || durationS < 0) return '—';
    if (durationS >= DOSE_BLOCKED_FOR_MAX_S) return 'bloqué depuis ≥ 24 h';
    return _elapsedText(durationS * 1000, 'bloqué depuis') || '—';
  }

  // null si la voie n'est pas bloquée.
  function _doseBlockRowData(json, prefix) {
    const info = _doseBlockInfo(json, prefix);
    if (!info || info.tone === 'none') return null;
    const raw = json?.[prefix + 'DoseBlockedForS'];
    const durationS = (typeof raw === 'number' && isFinite(raw)) ? raw : null;
    return { prefix, code: info.code, label: info.label, tone: info.tone, durationS };
  }

  // feature-138 (B3) — GO SOUS CONDITIONS pool-chemistry (C1 à C6, 2026-10-04).
  // LIEN DE NAVIGATION PUR : ne touche JAMAIS
  // ph/orp_regulation_mode (basculer ce sélecteur persiste la config et COUPE
  // la régulation automatique — cf. le handler de #ph_regulation_mode). Aucune
  // présélection de volume/durée/produit (C1) : on se contente de naviguer vers
  // #/ph ou #/orp après une confirmation EXPLICITE et distincte de celle du
  // bouton d'injection (C2) qui porte les libellés imposés (C3).
  async function _openManualInjectShortcut(prefix) {
    const isOrp = prefix === 'orp';
    const title = isOrp
      ? "Ouvrir l’injection manuelle — chlore (ORP)"
      : "Ouvrir l’injection manuelle — pH";
    // Libellés IMPOSÉS MOT À MOT par le gate pool-chemistry de feature-138
    // (condition C3, verdict du 2026-10-04). NE PAS reformuler, NE PAS abréger :
    // une première version tronquée avait perdu l'avertissement sur les
    // chloramines, l'instruction « n'augmentez pas la dose » et le rappel des
    // limites. Toute évolution de ce texte exige un nouveau passage du gate.
    const message = isOrp
      ? "La mesure est actuellement déclarée non représentative.\n\n" +
        "L’appareil a refusé le dosage automatique parce qu’il ne fait plus confiance à la mesure ORP — pas parce que l’eau serait correcte. " +
        "L’injection manuelle n’applique aucune garde de stabilité : elle vous obéira sur une mesure que l’appareil juge fausse. " +
        "C’est volontaire — c’est vous qui jugez de la représentativité.\n\n" +
        "Avant d’injecter, contrôlez le chlore libre au réactif (DPD) et le pH.\n\n" +
        "En eau chargée en matière organique (après de fortes pluies, un apport de dépôts, un nettoyage de fond), l’ORP ne remonte pas tant que le point de rupture n’est pas atteint : " +
        "poursuivre la consigne peut former des chloramines et vider un bidon sans effet mesurable. " +
        "Si le DPD montre du chlore libre alors que l’ORP reste bas, le problème n’est pas un manque de chlore — n’augmentez pas la dose.\n\n" +
        "Les limites journalière et horaire restent appliquées."
      : "La mesure est actuellement déclarée non représentative.\n\n" +
        "L’appareil a refusé le dosage automatique pH parce qu’il ne fait plus confiance à la mesure. " +
        "L’injection manuelle n’applique aucune garde de stabilité.\n\n" +
        "Une injection d’acide ne se reprend pas : si la mesure est fausse et le pH réellement bas, vous l’enfoncez. " +
        "Contrôlez le pH au réactif (rouge de phénol ou photomètre) et vérifiez l’offset pH avant toute injection.\n\n" +
        "Les limites journalière et horaire restent appliquées.";
    if (!(await confirmDialog({ title, message, confirmLabel: 'Ouvrir', cancelLabel: 'Annuler' }))) return;
    location.hash = isOrp ? '#/orp' : '#/ph';
  }

  function _renderDoseBlockRow(container, { label, tone, durationText, shortcutPrefix, shortcutCode, rescueActive }) {
    container.innerHTML = '';
    container.classList.remove(
      'dose-block-banner__row--normal', 'dose-block-banner__row--fault', 'dose-block-banner__row--unknown'
    );
    container.classList.add(`dose-block-banner__row--${tone}`);
    // role="alert" réservé au ton fault : ne pas sur-alerter sur du bénin
    // (filtration arrêtée la nuit est le cas NORMAL).
    container.setAttribute('role', tone === 'fault' ? 'alert' : 'status');
    if (tone === 'fault') container.removeAttribute('aria-live');
    else container.setAttribute('aria-live', 'polite');

    const dot = document.createElement('span');
    dot.className = 'dose-block-banner__dot';
    dot.setAttribute('aria-hidden', 'true');
    container.appendChild(dot);

    const body = document.createElement('div');
    body.className = 'dose-block-banner__body';
    const strong = document.createElement('strong');
    strong.className = 'dose-block-banner__label';
    strong.textContent = label;
    const dur = document.createElement('span');
    dur.className = 'dose-block-banner__duration';
    dur.textContent = durationText;
    body.appendChild(strong);
    body.appendChild(dur);
    container.appendChild(body);

    // feature-140 (B4) : chaînage vers la chloration de secours, ligne ORP
    // UNIQUEMENT (le secours ne concerne jamais le pH) — même liste blanche
    // DOSE_BLOCK_SHORTCUT_CODES que le raccourci ci-dessous, sans extension.
    // Placé AVANT le bouton secondaire existant (btn--primary ici, c'est
    // l'action recommandée — contrairement au raccourci d'injection manuelle).
    // ⚠️ Le widget « Modes temporaires » n'affiche plus d'état/bouton secours
    // du tout (v2.55.3, bandeau dédié #rescue-active-banner) — le clic ici
    // déclenche donc DIRECTEMENT la confirmation L2 via
    // _activateRescueChlorine(), qui reste un passage obligé.
    // ⚠️ v2.55.3 (gate pool-chemistry) : si le secours tourne DÉJÀ
    // (rescueActive), l'offre L7 et son bouton DISPARAISSENT — proposer
    // d'armer ce qui tourne déjà est l'incohérence que le gate a fermée. Le
    // texte de la ligne et le bouton « Ouvrir l'injection manuelle » restent
    // inchangés.
    // ⚠️ Correctif gate pool-chemistry (3e passage, 2026-10-09, feature-146) :
    // pendant une attente COURTE et AUTO-RÉSOLUTIVE (transitoires 15/18 —
    // fenêtre de lissage en remplissage ou attente de circulation), l'appareil
    // ne doit RIEN proposer : ni raccourci d'injection manuelle, ni chloration
    // de secours. Il annonce la cause et la durée, point. Les DEUX offres sont
    // donc restreintes au seul état INDÉTERMINÉ, sans fin annoncée (3
    // ReadingNaN) — `shortcutPrefix`/`shortcutCode` ne sont désormais produits
    // que pour ce code (canShortcut, plus bas, filtre sur
    // DOSE_BLOCK_SHORTCUT_CODES = [3]) : aucune exclusion locale à empiler ici,
    // la restriction est faite UNE fois, à la source.
    if (shortcutPrefix === 'orp' && !rescueActive) {
      const cfg = window._config || {};
      const isManual = (cfg.orp_regulation_mode || 'automatic') === 'manual';
      const rescueHint = document.createElement('p');
      rescueHint.className = 'dose-block-banner__rescue-hint';
      if (isManual) {
        // L8 — libellé IMPOSÉ par le gate pool-chemistry, repris MOT À MOT.
        rescueHint.textContent = "Chloration de secours indisponible : la régulation du chlore est en mode Manuel. Repassez en Automatique ou Programmée pour l'utiliser.";
        container.appendChild(rescueHint);
      } else {
        // L7 — libellé IMPOSÉ par le gate pool-chemistry (2e passage,
        // 2026-10-08, feature-139), repris MOT À MOT. Remplace le texte
        // antérieur (feature-140) : ne s'affiche plus que pour les vraies
        // pannes de mesure (code 3 — jamais 15/18, exclus ci-dessus).
        rescueHint.textContent = "Le chlore n'est plus dosé : l'appareil n'a plus de mesure fiable, et rien ne dit quand elle reviendra. La chloration de secours injecte un volume fixe, sans tenir compte de la mesure — à réserver aux pannes de sonde qui durent.";
        container.appendChild(rescueHint);

        const rescueBtn = document.createElement('button');
        rescueBtn.type = 'button';
        rescueBtn.className = 'btn btn--primary dose-block-banner__cta';
        rescueBtn.textContent = 'Activer la chloration de secours';
        rescueBtn.addEventListener('click', () => { _activateRescueChlorine(); });
        container.appendChild(rescueBtn);
      }
    }

    if (shortcutPrefix) {
      const btn = document.createElement('button');
      btn.type = 'button';
      // btn--secondary à dessein (C4) : jamais présenté comme l'action recommandée.
      btn.className = 'btn btn--secondary dose-block-banner__cta';
      btn.textContent = 'Ouvrir l’injection manuelle';
      btn.addEventListener('click', () => _openManualInjectShortcut(shortcutPrefix));
      container.appendChild(btn);
    }
    container.hidden = false;
  }

  // feature-148 (B1-B4) : plan de rendu du bandeau, PUR (aucun DOM). Le
  // bandeau (updateDoseBlockBanner, juste après) ne fait plus AUCUN choix :
  // il lit ce plan et rend STRICTEMENT ce qu'il décrit. C'est ce qui rend
  // `plan.carries` opposable — `carries.ph`/`carries.orp` ne disent PAS « le
  // code est dans telle liste », ils disent « cette ligne sera effectivement
  // à l'écran » : la garantie que `doseBlockBannerCarries()` (plus bas) peut
  // s'y fier pour faire taire le badge sans jamais faire taire une cause que
  // rien n'affiche réellement.
  function _doseBlockBannerPlan(json) {
    const ph = _doseBlockRowData(json, 'ph');
    const orp = _doseBlockRowData(json, 'orp');
    // feature-140 (v2.55.3) : secours DÉJÀ actif ⇒ supprime l'offre L7/bouton
    // d'activation sur la ligne ORP (sinon : proposer d'armer ce qui tourne
    // déjà). Le texte et le bouton d'injection manuelle restent inchangés.
    const rescueActive = Number.isFinite(json?.rescue_until) && json.rescue_until > 0;

    if (!ph && !orp) {
      return { visible: false, rowPh: null, rowOrp: null, carries: { ph: false, orp: false } };
    }

    // LISTE DES CODES « ATTENTE DE MESURE » — 3e passage pool-chemistry,
    // 2026-10-09, feature-146 ; réduite de SIX à TROIS codes par feature-152
    // (codes 4/19/22 retirés — ORP partage désormais les mêmes codes 15/18 que
    // pH). Ces codes partagent la même cause générique (mesure pH/ORP non
    // exploitable), mais se répartissent en DEUX familles bien distinctes du
    // point de vue de l'utilisateur :
    //   - 3  ReadingNaN            état INDÉTERMINÉ, sans fin annoncée
    //   - 15 ReadingNaNWarmup      TRANSITOIRE, borné et annoncé à l'écran
    //   - 18 ReadingNaNGateClosed  TRANSITOIRE, borné et annoncé à l'écran
    // Cette liste ne sert plus qu'à la FUSION pH→ORP (plus bas) : c'est elle
    // qui détermine si la conséquence ORP (code 250) partage la cause racine
    // pH. Elle ne doit JAMAIS déterminer un raccourci ou une offre de secours
    // — un transitoire court et auto-résolutif ne doit RIEN proposer, cf.
    // DOSE_BLOCK_SHORTCUT_CODES ci-dessous.
    // ⚠️ Toute addition à cette liste exige un passage du gate pool-chemistry.
    const DOSE_BLOCK_MEASURE_WAIT_CODES = [3, 15, 18];

    // LISTE BLANCHE des causes ouvrant une OFFRE (raccourci d'injection
    // manuelle ET chloration de secours, gate pool-chemistry 3e passage,
    // 2026-10-09, feature-146 — incident de production : un bandeau proposait
    // encore ces deux offres sur un transitoire de 24 s qui s'est résolu seul).
    // RESTREINTE au seul état INDÉTERMINÉ, sans fin annoncée — les transitoires
    // (15, 18) sont retirés des DEUX offres : proposer un geste sur une
    // attente qui se termine seule n'aide personne, et le texte L7 (« rien ne
    // dit quand elle reviendra ») y serait un mensonge.
    // L'injection manuelle reste pleinement ATTEIGNABLE sur ce code — par
    // #ph-inject-section / #orp-inject-section directement, sans raccourci —
    // on retire un raccourci, pas un recours.
    //   3  ReadingNaN       lecture filtrée NaN, état indéterminé
    // Les PANNES (16, 17) sont déjà écartées par le ton `fault`, et un code
    // inconnu l'est par la liste blanche elle-même — on ne peut alors pas
    // exclure « filtration arrêtée ».
    // ⚠️ Toute addition à cette liste exige un passage du gate pool-chemistry.
    const DOSE_BLOCK_SHORTCUT_CODES = [3];
    const canShortcut = (row) => !!row
      && (row.tone === 'normal' || row.tone === 'unknown')
      && row.code !== DOSE_BLOCK_FILTRATION_STOPPED_CODE
      && DOSE_BLOCK_SHORTCUT_CODES.includes(row.code);

    // Fusion CAUSE RACINE pH → CONSÉQUENCE ORP (production 2026-10-05, v2.55.0,
    // redémarrage OTA) : « Mesures non représentatives (stabilisation) » (pH)
    // et « Sécurité pH/ORP active » / pH trop haut (ORP, code 250) sont la
    // MÊME cause, pas deux causes — le code 250 n'est qu'une conséquence du pH
    // indisponible. Ce cas se produit à CHAQUE redémarrage et reprise de
    // filtration : c'est le scénario le plus fréquent du bandeau.
    //
    // CONDITION DE FUSION, non négociable, codée EXPLICITEMENT (pas par
    // approximation sur les textes) : orp.code === DOSE_BLOCK_CODE_GUARD (250)
    // ET ph.code ∈ DOSE_BLOCK_MEASURE_WAIT_CODES ({3,15,18} — les codes
    // d'attente de mesure, pas la liste restreinte du raccourci/secours
    // ci-dessus). Toute AUTRE valeur côté ORP (quota journalier, inhibition sur
    // pH haut en régulation stable, pompe non caractérisée…) est une cause
    // INDÉPENDANTE ⇒ deux lignes, SANS EXCEPTION : fusionner là masquerait un
    // second problème réel, ce qui serait pire que la redondance.
    //
    // ⚠️ Libellé IMPOSÉ MOT À MOT par le gate pool-chemistry (2e passage,
    // 2026-10-08, feature-139), opposable au même titre que L1-L9 : ne JAMAIS
    // le reformuler, abréger ou tronquer, et ne JAMAIS élargir la condition de
    // fusion ci-dessus sans un nouveau passage du gate. Remplace le texte
    // antérieur (feature-140, 2026-10-06) : le premier annonçait la fin de
    // façon vague (« dès que... ») ; `ph.label` porte désormais le texte
    // déterministe (_measuresWaitText, « Reprise automatique dans N min »),
    // la fin n'est donc annoncée QU'UNE fois, pas deux.
    // feature-148 : fusion ⇒ carries = { ph:true, orp:true } dans LES DEUX cas
    // ci-dessous (cause racine ET même-code). La ligne fusionnée RACONTE les
    // deux voies à la fois ; laisser le badge ORP (ou pH) répéter l'une
    // d'elles romprait exactement ce que la fusion visait à éviter.
    if (ph && orp && orp.code === DOSE_BLOCK_CODE_GUARD && DOSE_BLOCK_MEASURE_WAIT_CODES.includes(ph.code)) {
      const durations = [ph.durationS, orp.durationS].filter((v) => v != null);
      // « bloqué depuis » = la PLUS ANCIENNE des deux durées : la ligne décrit
      // la cause RACINE (pH), afficher l'âge de la conséquence (ORP, souvent
      // bien plus courte — 32 s contre 2 min observées en production) sous-
      // estimerait la durée réelle pendant laquelle le dosage chlore est
      // suspendu.
      const durationS = durations.length ? Math.max(...durations) : null;
      return {
        visible: true,
        rowPh: {
          // %s = ph.label, repris TEL QUEL depuis DOSE_BLOCK_CODES (non reformulé).
          label: `${ph.label} — dosage du pH et du chlore en pause tous les deux, le temps que l'eau neuve arrive jusqu'aux sondes.`,
          tone: 'normal', // jamais fault — cohérent avec toneOrp (code 250, voie ORP)
          durationText: _doseBlockDurationText(durationS),
          // Le chaînage feature-140 reste attaché à la ligne fusionnée. Mais
          // depuis le 3e passage (feature-146), DOSE_BLOCK_SHORTCUT_CODES ([3])
          // est plus ÉTROITE que DOSE_BLOCK_MEASURE_WAIT_CODES (fusion) :
          // canShortcut(ph) ne vaut `true` QUE pour 3 (indéterminé, sans fin
          // annoncée) — sur 15/18 (transitoires bornés et annoncés),
          // shortcutPrefix/shortcutCode ressortent `null` et ni le bouton
          // « Ouvrir l'injection manuelle » ni l'offre de secours (L7/bouton,
          // _renderDoseBlockRow) ne s'affichent. Voulu : rien à proposer sur une
          // attente qui se résout seule.
          shortcutPrefix: canShortcut(ph) ? 'orp' : null,
          shortcutCode: canShortcut(ph) ? ph.code : null,
          rescueActive,
        },
        rowOrp: null,
        carries: { ph: true, orp: true },
      };
    }

    // Fusion UNIQUEMENT si même code ET ton normal — JAMAIS pour fault/unknown :
    // une panne par voie doit rester lisible individuellement.
    if (ph && orp && ph.code === orp.code && ph.tone === 'normal' && orp.tone === 'normal') {
      const durations = [ph.durationS, orp.durationS].filter((v) => v != null);
      const durationS = durations.length ? Math.min(...durations) : null;
      return {
        visible: true,
        rowPh: {
          label: `${ph.label} (pH et chlore)`,
          tone: 'normal',
          durationText: _doseBlockDurationText(durationS),
          // C4 : ligne fusionnée → raccourci par défaut sur l'ORP uniquement.
          shortcutPrefix: canShortcut(ph) ? 'orp' : null,
          shortcutCode: canShortcut(ph) ? ph.code : null,
          rescueActive,
        },
        rowOrp: null,
        carries: { ph: true, orp: true },
      };
    }

    // Lignes indépendantes : `carries` suit EXACTEMENT la présence de chaque
    // ligne dans le plan — pas une relecture de `ph`/`orp` bruts, pour que le
    // badge et le bandeau restent d'accord même si un futur appel ajoute une
    // condition ici sans la répercuter plus bas.
    return {
      visible: true,
      rowPh: ph ? {
        label: ph.label, tone: ph.tone,
        durationText: _doseBlockDurationText(ph.durationS),
        shortcutPrefix: canShortcut(ph) ? 'ph' : null,
        rescueActive,
      } : null,
      rowOrp: orp ? {
        label: orp.label, tone: orp.tone,
        durationText: _doseBlockDurationText(orp.durationS),
        shortcutPrefix: canShortcut(orp) ? 'orp' : null,
        shortcutCode: canShortcut(orp) ? orp.code : null,
        rescueActive,
      } : null,
      carries: { ph: !!ph, orp: !!orp },
    };
  }

  // Bandeau d'état de blocage du dosage (B2), hors panneau développeur.
  // Absent si rien n'est bloqué — pas de bandeau permanent qui s'ignore.
  // feature-148 : NE DÉCIDE PLUS RIEN — lit _doseBlockBannerPlan() et rend
  // STRICTEMENT ce qu'il décrit. Point de vigilance du relecteur (feature-148) :
  // si une seule décision de rendu revenait ici (un `hidden` posé hors plan, un
  // retour anticipé), `plan.carries` mentirait et le silence du badge
  // deviendrait faux. Toute évolution du rendu se fait dans le plan, jamais ici.
  function updateDoseBlockBanner(json) {
    const box = $("#dose-block-banner");
    const rowPh = $("#dose-block-row-ph");
    const rowOrp = $("#dose-block-row-orp");
    if (!box || !rowPh || !rowOrp) return;

    const plan = _doseBlockBannerPlan(json);

    if (!plan.visible) {
      box.hidden = true;
      rowPh.hidden = true;
      rowOrp.hidden = true;
      return;
    }

    if (plan.rowPh) _renderDoseBlockRow(rowPh, plan.rowPh); else rowPh.hidden = true;
    if (plan.rowOrp) _renderDoseBlockRow(rowOrp, plan.rowOrp); else rowOrp.hidden = true;

    box.hidden = false;
  }

  // feature-148 (B1-B4) — garde de présence DOM + plan. Un prédicat purement
  // logique dirait « le bandeau porte cette voie » même sur une page sans les
  // trois nœuds (`#dose-block-banner` n'existe QUE sur le tableau de bord,
  // cf. feature-148 §« Correction d'une affirmation de l'audit » : les pages
  // /ph et /orp n'appellent jamais buildSensorSegments()) — d'où la garde de
  // présence DOM, en plus du plan, avant de faire taire le badge.
  // En cas de doute, on duplique : si l'un des trois nœuds manque, ou si le
  // plan ne couvre pas `prefix`, la fonction renvoie `false` et le badge PARLE
  // — jamais l'inverse.
  function doseBlockBannerCarries(prefix, json) {
    if (!$("#dose-block-banner") || !$("#dose-block-row-ph") || !$("#dose-block-row-orp")) return false;
    return !!_doseBlockBannerPlan(json).carries?.[prefix];
  }

  // feature-152 (B6) : les 3 champs survivants (brut, valeur retenue, dernière
  // mesure) sont absorbés dans le modal sonde de chaque voie — pH dans
  // #ph-probe-modal (cf. _renderPhProbeModalValues), ORP dans #orp-filter-modal
  // réutilisé/retitré (ci-dessous). Médiane, compteur de rejets, indicateur de
  // disponibilité et dosage bloqué ont disparu avec le mécanisme qu'ils
  // décrivaient (plus d'état bloquant à diagnostiquer, plus de réarmement
  // manuel).
  function _renderOrpProbeModalValues(json) {
    const rawEl = $('#orp-fm-raw');
    if (!rawEl) return;
    const filEl = $('#orp-fm-filtered');
    const ageEl = $('#orp-fm-age');
    rawEl.textContent = _fmtFilterVal('orp', json?.orpRaw);
    if (filEl) filEl.textContent = _fmtFilterVal('orp', json?.orpFiltered);
    if (ageEl) ageEl.textContent = _filterAgeText('orp');
  }

  function bindOrpCalChip() {
    const chip = $('#orp-cal-chip');
    const modal = $('#orp-filter-modal');
    const closeBtn = $('#orp-filter-close');
    if (!chip || !modal) return;

    if (typeof modal.showModal !== 'function') {
      chip.setAttribute('aria-disabled', 'true');
      chip.style.cursor = 'default';
      return;
    }

    chip.addEventListener('click', () => {
      _renderOrpProbeModalValues(latestSensorData || {});
      try { modal.showModal(); } catch (e) { console.error('[orp-probe] showModal:', e); }
    });
    chip.addEventListener('keydown', (e) => {
      if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); chip.click(); }
    });
    if (closeBtn) closeBtn.addEventListener('click', () => modal.close());
    modal.addEventListener('click', (e) => { if (e.target === modal) modal.close(); });
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

        // feature-148 (code-reviewer, point de robustesse) : AVANT le badge,
        // volontairement — doseBlockBannerCarries() (lu par updateSensorBadges
        // via dosingLimitSegments) doit lire un bandeau déjà à jour pour CETTE
        // trame. Si une exception survenait entre les deux appels (plus bas :
        // updateDetailSections, _flowWizardOnData…), l'ancien ordre aurait pu
        // figer le bandeau sur une trame périmée pendant que le ticker 1 s
        // continuait de faire taire le badge sur la trame courante — un
        // silence sans porteur, exactement ce que cette spec ferme. Ne pas
        // remettre « dans l'ordre logique » (rendu global avant badges) sans
        // revérifier cet invariant.
        updateDoseBlockBanner(json);
        // Mettre à jour les badges et cartes status
        updateSensorBadges();
        updateStatusCards();
        updatePumpCharacterizationBadges(json);  // badge « Débit non caractérisé »
        updateProductUI(json);
        _updateSondesChip();
        if (window._updateInjectButtons) window._updateInjectButtons(json);
        // feature-075 : expiration de session de l'assistant. bug-082 : le hook
        // EST appelé sur les trames WS — elles portent désormais flow_assist
        // (flow255, aborted). Ce sont sa garde `fa.active === undefined` et
        // l'état de session, servi par /data SEUL, qui les font sortir tôt.
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
        // (met aussi à jour la ligne brute et le modal diagnostic, cf. updateFilterSub)
        updatePhProbeChip(json);

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
        // feature-025 : ligne brute discrète (brut/maj) de la carte « Lecture ORP »
        // + rafraîchissement du modal sonde ORP si ouvert.
        updateFilterSub('orp', json);
        const orpProbeModal = $('#orp-filter-modal');
        if (orpProbeModal && orpProbeModal.open) _renderOrpProbeModalValues(json);

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

    // feature-156 : déclencheur / fermeture du mode réglage (ex-carte de
    // calibration seule) — cf. enterMaintenance/requestExitMaintenance.
    $("#temp_cal_trigger_btn")?.addEventListener("click", () => {
      enterMaintenance("temperature");
    });
    $("#temp_cal_close_btn")?.addEventListener("click", () => {
      requestExitMaintenance("temperature");
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
          exitMaintenance("temperature");

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
    // feature-159 (code-reviewer, correctif bloquant) : `pts` INCONNU
    // (/data pas encore répondu, ou hors ligne) n'est PAS la même chose que
    // « non calibré » — fail-CLOSED, pas fail-open. Distinct de
    // `phUnreachable` (-1, EZO explicitement injoignable).
    const phKnown = _calIsPtsKnown(phPts);

    // Bouton + hint pH (priorité : EZO injoignable > état inconnu > état calibration).
    // Le garde injection est appliqué après (prioritaire).
    const phBtn = $("#ph_cal_trigger_btn");
    if (phBtn) phBtn.disabled = !phKnown;
    const phHint = $("#ph_cal_hint");
    if (phHint) {
      if (phUnreachable) phHint.textContent = "EZO pH injoignable — vérifiez le câblage I²C et l'alimentation.";
      else if (!phKnown) phHint.textContent = "État de calibration inconnu — données non reçues";
      else if (phPartial) phHint.textContent = "Régulation auto inhibée (1/2 point).";
      else if (phPts === 0) phHint.textContent = "Régulation auto inhibée tant que non calibré.";
      else phHint.textContent = "";
    }

    // Status header de la carte Calibration pH
    const phHeader = $("#ph_cal_status_header");
    if (phHeader) {
      // feature-159 (contre-visite, règle C9 du gate) : « Aucun point calibré »
      // était affirmé alors que le compteur est INCONNU (pas encore reçu). Le
      // hint disait déjà « inconnu » pendant que l'en-tête, lui, assertait.
      // Dire ce qu'on a vérifié, jamais un jugement.
      if (phUnreachable) phHeader.textContent = "EZO pH injoignable";
      else if (!_calIsPtsKnown(phPts)) phHeader.textContent = "État de calibration inconnu";
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
      // feature-159 (code-reviewer, correctif bloquant) : même défaut que pH —
      // `orpPts` inconnu (null/undefined) armait le bouton à tort (seul `-1`
      // désactivait). `_calIsPtsKnown` traite les deux comme non autorisants.
      const orpKnown = _calIsPtsKnown(orpPts);
      setCalChip("orp", {
        cls: "chip--probe-unknown",
        label: ezoDown ? "EZO indisponible" : "Calibration —",
        btnDisabled: !orpKnown,
        hint: ezoDown ? "EZO ORP injoignable — vérifiez le câblage I²C."
            : (!orpKnown ? "État de calibration inconnu — données non reçues" : ""),
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

    // feature-159 (code-reviewer, correctif bloquant, partie b) : auto-
    // réparation UNE SEULE FOIS — voir le commentaire de `_calPtsKnownOnce`.
    // Si l'écran a été ouvert pendant que `/data` n'avait pas encore répondu
    // (ou hors ligne), `resetCalStepper()` a désactivé tous les boutons du
    // stepper (fail-closed) ; dès que `pts` devient numérique pour la
    // première fois, on recalcule l'acquisition réelle et on réarme les
    // boutons avant de laisser `_calRefreshFrozenButtons()` (ci-dessous)
    // refigure ceux réellement déjà calibrés.
    if (!_calPtsKnownOnce.ph && typeof json.phCalPoints === "number") {
      _calPtsKnownOnce.ph = true;
      Object.assign(_calFrozen.ph, _calPointsAcquired("ph", json.phCalPoints));
      ["btn-cal-ph-mid", "btn-cal-ph-low"].forEach(id => { const b = $("#" + id); if (b) b.disabled = false; });
    }
    if (!_calPtsKnownOnce.orp && typeof json.orpCalPoints === "number") {
      _calPtsKnownOnce.orp = true;
      Object.assign(_calFrozen.orp, _calPointsAcquired("orp", json.orpCalPoints));
      { const b = $("#btn-cal-orp"); if (b) b.disabled = false; }
    }

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

  // ---------- feature-034 / bug-160 : moteur stepper + statistique de stabilité ----------
  // ⚠️ DÉFAUT CORRIGÉ (bug-160, chiffrage gate pool-chemistry) : l'ancien seuil
  // (0,9 pH / fenêtre 20 s, `max - min`) annonçait « Stable » à 0,8-1,1 pH de
  // l'équilibre réel. `max - min` est une ÉTENDUE : elle CROÎT avec le nombre
  // d'échantillons et mélange bruit et dérive — LA STATISTIQUE CHANGE, pas
  // seulement le nombre : dérive MÉDIANE moitié récente / moitié ancienne,
  // gardée par un garde-fou `G` sur les temps RÉELLEMENT observés (pas la
  // fenêtre nominale). Voir `_calStabilityVerdict` ci-dessous.
  //
  // Table CHAQUE constante déclare sa SOURCE et son ARITHMÉTIQUE (ADR-0098),
  // pour qu'une session future RECONTRÔLE au lieu de CROIRE — verdict
  // pool-chemistry GO/17 conditions (C1-C8) puis GATE 2. Getters : DÉRIVÉS,
  // jamais un second nombre à tenir synchronisé à la main.
  const CAL_STAB = {
    // Cadence DÉGRADÉE, sans WebSocket (cf. loadSensorData, section C9) : 30 s.
    SLOW_CADENCE_MS: 30000,
    // Pente de Nernst à 25 °C : S = 2,303·R·T/F ≈ 59,16 mV/pH (ADR-0074).
    NERNST_MV_PER_PH: 59.16,
    // Produit — lot B (lecture/stabilité) : nombre MINIMAL d'échantillons
    // avant qu'un verdict puisse seulement être ENVISAGÉ (coupure au
    // COMPTAGE, cf. _calStabilityVerdict ; distincte du garde-fou temporel
    // `G ≥ G_MIN`, qui protège un autre régime — gate P2). SOURCE UNIQUE :
    // référencée ici ET par la purge souple de `pushStabilitySample` — avant
    // ce correctif (revue bloquante), le même « 6 » vivait en DEUX littéraux
    // indépendants, susceptibles de diverger silencieusement.
    MIN_SAMPLES: 6,
    // Produit — lot B : facteur de marge du plafond d'âge DUR (HARD_AGE_MS),
    // au-delà du simple WINDOW_MS. Valeur ARBITRAIRE mais DÉCLARÉE (ADR-0098
    // exige une source, pas nécessairement une dérivation physique) : assez
    // large pour ne jamais purger prématurément une sonde lente avant que la
    // purge SOUPLE (qui, elle, protège n ≥ MIN_SAMPLES) n'ait eu l'occasion
    // d'agir ; borné quand même pour éviter une fuite mémoire/pertinence si
    // une session reste ouverte des heures.
    HARD_AGE_FACTOR: 4,
    ph: {
      // Produit — kPhGuardReleaseHysteresis (constants.h) : hystérésis de
      // relâche de garde DÉJÀ choisie pour ce projet, pas un nombre neuf.
      THRESHOLD: 0.05,
      // Produit — kPhHighMargin (constants.h).
      RESIDUAL_MAX: 0.10,
      // Matériel — Atlas EZO pH, 1-2 min avant `Cal` ; ⚠️ majoration de
      // conception NON relevée sur datasheet (cf. constants.h,
      // CAL_STAB_TAU_MAX_MS_PH) — SURESTIMER est fail-safe (agrandit WINDOW,
      // donc DURCIT le critère) ; TOUTE BAISSE exige un relevé fabricant.
      TAU_MAX_MS: 120000,
      // Dérivée : réserve de rétention = τmax moins la cadence dégradée, pour
      // écarter les échantillons trop anciens pour appartenir au MÊME
      // transitoire d'électrode sans black-outer la cadence la plus lente.
      get WINDOW_MS() { return this.TAU_MAX_MS - CAL_STAB.SLOW_CADENCE_MS; },
      // Plafond d'âge DUR (section 2, lot B) : jamais un échantillon plus
      // vieux que HARD_AGE_FACTOR × WINDOW, quel que soit le nombre restant.
      get HARD_AGE_MS() { return this.WINDOW_MS * CAL_STAB.HARD_AGE_FACTOR; },
      // Dérivé — AUCUN degré de liberté : G_MIN = SEUIL / RÉSIDUEL_MAX, PAR
      // PRODUIT (revue bloquante : une version antérieure dérivait le G_MIN
      // PARTAGÉ depuis `ph` SEUL — un futur changement du ratio ORP ne
      // l'aurait pas suivi). Si un vecteur échoue, ce n'est JAMAIS ce seuil
      // qu'il faut assouplir (⛔ ADR-0098) : c'est `G ≥ G_MIN` qui est
      // violée — seule issue : allonger la fenêtre ou la cadence, toutes
      // deux PLUS STRICTES.
      get G_MIN() { return this.THRESHOLD / this.RESIDUAL_MAX; },
    },
    orp: {
      // Physique — 0,05 × pente de Nernst. ⚠️ ÉCRIRE L'EXPRESSION, jamais un
      // littéral (3.0 REFUSÉ par le gate — « exactement le mouvement
      // qu'ADR-0098 interdit, et la même faute que celle qu'on corrige »).
      get THRESHOLD() { return 0.05 * CAL_STAB.NERNST_MV_PER_PH; },
      get RESIDUAL_MAX() { return 0.10 * CAL_STAB.NERNST_MV_PER_PH; },
      // Matériel — majoration de conception (non relevée sur datasheet,
      // idem pH) ; plus courte que la pose normale d'une sonde redox.
      TAU_MAX_MS: 180000,
      get WINDOW_MS() { return this.TAU_MAX_MS - CAL_STAB.SLOW_CADENCE_MS; },
      get HARD_AGE_MS() { return this.WINDOW_MS * CAL_STAB.HARD_AGE_FACTOR; },
      // PROPRE au produit (même raison que ph.G_MIN ci-dessus) — par
      // construction IDENTIQUE à ph.G_MIN (le facteur de Nernst s'annule) ;
      // `test/cal_stability_vectors.js` épingle cette égalité EXPLICITEMENT,
      // pas seulement par coïncidence de lecture.
      get G_MIN() { return this.THRESHOLD / this.RESIDUAL_MAX; },
    },
  };
  // Borné en NOMBRE (section 2) : indépendant de la cadence/fenêtre, évite
  // une fuite mémoire si une session reste ouverte des heures.
  const CAL_STAB_MAX_SAMPLES = 256;
  // Diagnostic (section 4, verdict ergonome, P3 du gate) — produit :
  // kStabilizationDurationPhMs (constants.h), appliqué aux DEUX produits par
  // décision du gate : kStabilizationDurationOrpMs (180 000 ms) serait PLUS
  // COURTE que la pose normale d'une sonde redox, donc inadaptée ici.
  const CAL_STAB_DIAG_MS = 300000;
  // Dérivé de CAL_STAB_DIAG_MS — jamais un second littéral « 5 » découplé
  // (revue bloquante) : si la durée de diagnostic change un jour, le texte
  // T-D1/T-D2 (verbatim ci-dessous) la reflète automatiquement. AUJOURD'HUI,
  // vaut exactement 5 — le verbatim imposé reste donc inchangé.
  const CAL_STAB_DIAG_MIN = CAL_STAB_DIAG_MS / 60000;
  // Textes T-D1 (pH) / T-D2 (ORP) — VERBATIM (coordinateur, correctif du
  // 2026-10-13 : la première rédaction de ce lot était une PARAPHRASE, pas
  // une recopie de la spec). DEUX LIGNES dans le MÊME bloc — jamais de renvoi
  // vers une pente côté ORP (étalonnage à un seul point, pas de pente
  // possible : les deux textes NE SONT PAS symétriques). ⛔ L'icône ⚠ N'EST
  // PAS dans le texte (tranché par ux-ui-designer) — posée par CSS/classe
  // `.is-diag` comme pour les autres états, jamais concaténée à la chaîne.
  // ⚠️ `D`/`S` dans la ligne 1 sont les valeurs RÉALISÉES du verdict COURANT
  // (C5) — jamais un exemple recopié en dur (0,12 pH/45 s et 4 mV/75 s dans la
  // spec sont des gabarits de FORMAT, pas des littéraux à reprendre).
  function _calDiagLines(product, verdict) {
    const fmtD = product === "ph"
      ? verdict.D.toFixed(2).replace(".", ",") + " pH"
      : Math.round(verdict.D) + " mV";
    const sSec = Math.max(0, Math.round(verdict.S / 1000));
    const line1 = "Toujours instable après " + CAL_STAB_DIAG_MIN + " min (écart " + fmtD + " sur " + sSec + " s).";
    const line2 = product === "ph"
      ? "Vous pouvez calibrer quand même. Une dérive qui dure est aussi un signe d'usure de la sonde : vérifiez la pente dans « État sonde pH » après l'étalonnage."
      : "Vous pouvez calibrer quand même. Une dérive qui dure est aussi un signe d'usure de la sonde redox.";
    return [line1, line2];
  }

  // Verdict de stabilité — fonction PURE (ADR-0017/0099) : aucun DOM, aucune
  // horloge interne (`nowMs` fourni par l'appelant — `performance.now()` côté
  // navigateur, un nombre arbitraire dans les vecteurs natifs). `samples`
  // DOIT être trié par `t` croissant (invariant tenu par pushStabilitySample,
  // pas revérifié ici : une fonction pure ne masque pas un appelant qui le
  // romprait).
  //
  // Coupure en DEUX moitiés PAR COMPTAGE (floor(n/2), médian écarté si
  // impair) ET garde-fou `G` sur les temps RÉELLEMENT observés : les DEUX
  // sont imposés, ils tuent des mutants différents (gate, P2) — une coupure
  // au comptage SANS `G` laisserait passer une RAFALE (beaucoup
  // d'échantillons en peu de temps) ; une coupure au temps SANS `G`
  // laisserait passer une cadence MIXTE.
  //
  // D = |médiane(moitié récente) − médiane(moitié ancienne)|
  // S = séparation RÉALISÉE des temps médians (pas la fenêtre nominale)
  // a = âge RÉALISÉ de la médiane récente (pas la cadence nominale)
  // G = e^(a/τmax) · (e^(S/τmax) − 1) ⇒ résiduel garanti R = D/G
  //
  // Verdict RENDU seulement si n ≥ 6 ET G ≥ G_MIN ; sinon MUET. `stable` ⟺
  // `D ≤ SEUIL`.
  function _calStabilityVerdict(product, samples, nowMs) {
    const cfg = CAL_STAB[product];
    const n = samples.length;
    if (n < CAL_STAB.MIN_SAMPLES) return { state: "muted", D: null, S: null, G: null, n };

    const half = Math.floor(n / 2);
    const oldHalf = samples.slice(0, half);
    const recentHalf = samples.slice(n - half);

    const median = (arr, key) => {
      const vals = arr.map(key).sort((a, b) => a - b);
      const mid = Math.floor(vals.length / 2);
      return vals.length % 2 ? vals[mid] : (vals[mid - 1] + vals[mid]) / 2;
    };

    const oldMedianV = median(oldHalf, (s) => s.v);
    const recentMedianV = median(recentHalf, (s) => s.v);
    const oldMedianT = median(oldHalf, (s) => s.t);
    const recentMedianT = median(recentHalf, (s) => s.t);

    const D = Math.abs(recentMedianV - oldMedianV);
    const S = recentMedianT - oldMedianT; // séparation RÉALISÉE
    const a = nowMs - recentMedianT;      // âge RÉALISÉ de la médiane récente

    const tau = cfg.TAU_MAX_MS;
    const G = Math.exp(a / tau) * (Math.exp(S / tau) - 1);

    if (G < cfg.G_MIN) return { state: "muted", D, S, G, n };

    const stable = D <= cfg.THRESHOLD;
    return { state: stable ? "stable" : "unstable", D, S, G, n };
  }

  // Vide le tampon de stabilité ET son compagnon de session (section 2/4) —
  // SEUL point qui les remet à zéro ENSEMBLE, pour qu'aucun site d'appel ne
  // puisse oublier l'un des deux (c'était exactement le risque d'une remise
  // à zéro dupliquée quatre fois).
  function _calResetStabBuffer(product) {
    _calStab[product] = [];
    _calStabSessionStart[product] = null;
  }

  // Affiche `text` (chaîne OU tableau de lignes, cf. diagnostic T-D1/T-D2)
  // dans `el` SANS jamais aplatir un retour à la ligne via `textContent` sur
  // plusieurs lignes (section 3/4 du lot) — un enfant par ligne.
  function _calRenderStabilityLines(el, text) {
    if (Array.isArray(text)) {
      el.textContent = "";
      text.forEach((line) => {
        const div = document.createElement("div");
        div.textContent = line;
        el.appendChild(div);
      });
    } else {
      el.textContent = text;
    }
  }

  // Indices d'étapes du stepper que l'utilisateur fait avancer manuellement.
  const CAL_MANUAL_STEPS = { ph: [0, 2], orp: [0, 1] };
  const CAL_STEP_COUNT   = { ph: 5, orp: 4 };

  const _calStab = { ph: [], orp: [] };      // [{t, v}] fenêtre glissante (t = performance.now(), monotone)
  const _calActiveIdx = { ph: 0, orp: 0 };   // index de l'étape active (pour figer les étapes franchies)
  // bug-160 (section 4) : instant du PREMIER échantillon du tampon courant —
  // `null` tant qu'aucun échantillon n'a encore été poussé depuis le dernier
  // reset. Compagnon DÉDIÉ de `_calStab` (se remet à zéro AVEC lui, cf. tous
  // les sites qui vident `_calStab[product]`) : NE DOIT PAS dépendre de
  // `_calStab[product][0].t`, que la purge de rétention fait avancer en
  // continu (sans quoi le diagnostic ne se déclencherait jamais, cf. gate).
  const _calStabSessionStart = { ph: null, orp: null };

  // feature-159 (code-reviewer, correctif bloquant) : SEULE définition de
  // « pts est-il une mesure exploitable ? ». Un `pts` non numérique
  // (undefined/null — /data pas encore répondu, ou hors ligne) ou négatif
  // (-1, EZO injoignable) ne vaut PAS « non calibré » : c'est un INCONNU,
  // qui ne doit RIEN autoriser (fail-closed, jamais fail-open). Utilisée par
  // `_calPointsAcquired` ci-dessous ET par `renderCalibrationStatus` (bouton
  // + hint du déclencheur, hors stepper) — une SEULE expression, jamais
  // dupliquée, pour qu'un futur élargissement de cette frontière (ex. un
  // malheureux `>= -1`) soit visible aux DEUX appelants et tué par le même
  // harnais.
  function _calIsPtsKnown(pts) {
    return (typeof pts === "number") && pts >= 0;
  }

  // feature-159 (code-reviewer, correctif bloquant) : fonction PURE, sans DOM,
  // qui décide quels points sont RÉELLEMENT acquis d'après le firmware
  // (`phCalPoints`/`orpCalPoints`). Extraite pour que `resetCalStepper()` et
  // l'auto-réparation de `updateCalibrationReadouts()` partagent la MÊME
  // décision — et pour être couverte par un harnais
  // (`test/cal_points_acquired_vectors.js`, ADR-0099 : le texte est EXTRAIT
  // de ce fichier, jamais recopié).
  function _calPointsAcquired(product, pts) {
    const known = _calIsPtsKnown(pts);
    if (product === "ph") {
      return { mid: known && pts >= 1, low: known && pts >= 2 };
    }
    return { orp: known && pts >= 1 };
  }

  // feature-159 (C5) : SEULE source de vérité pour « ce point est déjà calibré »,
  // peuplée depuis le firmware (phCalPoints/orpCalPoints) à resetCalStepper() et
  // mise à jour à chaque succès. Contrairement à `_calActiveIdx` (position du
  // stepper DANS CETTE SESSION, remise à 0 à chaque réouverture), `_calFrozen`
  // survit à la réouverture de l'écran — c'est exactement ce qui manquait : le
  // figeage ne dépendait que de `_calActiveIdx`, remis à zéro par
  // resetCalStepper() sans consulter le firmware.
  const _calFrozen = { ph: { mid: false, low: false }, orp: { orp: false } };
  // feature-159 (code-reviewer, correctif bloquant, partie b) : auto-réparation
  // de l'état « inconnu » fixé ci-dessus par `resetCalStepper()`, au PREMIER
  // `pts` numérique reçu après l'ouverture de l'écran — et seulement là.
  // Sans ceci, un écran ouvert pendant que `/data` n'avait pas encore répondu
  // restait bloqué (tous les boutons désactivés) pour toute la session, même
  // une fois la donnée arrivée : une impasse. ⚠️ Ne JAMAIS recalculer au-delà
  // de la première fois : après un clic « Recommencer », le firmware annonce
  // ENCORE le point comme calibré le temps que l'EZO exécute le nouveau `Cal`
  // — un recalcul permanent re-figerait le bouton aussitôt et annulerait le
  // geste que ce lot fournit.
  const _calPtsKnownOnce = { ph: false, orp: false };
  // État d'ÉCHEC persistant par étape (feature-159, C9) : un message tant qu'un
  // nouvel essai ou un succès ne l'a pas remplacé/effacé — jamais un simple
  // toast volatile. `null` = pas d'échec en attente.
  const _calFailure = { ph: { mid: null, low: null }, orp: { orp: null } };

  function _calSetFailure(product, step, message) {
    _calFailure[product][step] = message;
    renderStability(product);
  }
  // `step` omis → efface TOUTES les étapes du produit (nouvelle session).
  function _calClearFailure(product, step) {
    if (step) { _calFailure[product][step] = null; }
    else { Object.keys(_calFailure[product]).forEach(k => { _calFailure[product][k] = null; }); }
    renderStability(product);
  }

  function pushStabilitySample(product, value) {
    // ⚠️ Horloge MONOTONE (section 2) : `performance.now()`, PAS `Date.now()`
    // — un recalage NTP ou un changement de fuseau cassait la purge dans les
    // DEUX sens (bug-160, gate P2).
    const now = performance.now();
    const arr = _calStab[product];
    // Premier échantillon du tampon courant : amorce le minuteur de
    // diagnostic (section 4) — compagnon DÉDIÉ, jamais réutilisé depuis
    // `arr[0].t` (cf. commentaire de `_calStabSessionStart`).
    if (_calStabSessionStart[product] == null) _calStabSessionStart[product] = now;
    arr.push({ t: now, v: value });

    // Borné en NOMBRE (section 2), indépendant de l'âge.
    while (arr.length > CAL_STAB_MAX_SAMPLES) arr.shift();

    const cfg = CAL_STAB[product];
    // Plafond d'âge DUR : jamais un échantillon plus vieux que
    // HARD_AGE_FACTOR × WINDOW, quel que soit le nombre restant (contrairement
    // à la purge souple ci-dessous).
    const hardCutoff = now - cfg.HARD_AGE_MS;
    while (arr.length && arr[0].t < hardCutoff) arr.shift();

    // Purge SOUPLE : au-delà de WINDOW, SAUF s'il resterait moins de
    // MIN_SAMPLES échantillons — une cadence dégradée (30 s sans WebSocket)
    // doit pouvoir s'accumuler au-delà de WINDOW le temps d'atteindre
    // n ≥ MIN_SAMPLES, sinon : état MUET PERMANENT, le pire résultat
    // (contraintes techniques de la spec). SOURCE UNIQUE avec le seuil
    // utilisé par `_calStabilityVerdict` (CAL_STAB.MIN_SAMPLES) — avant ce
    // correctif (revue bloquante), le même « 6 » vivait ici en second
    // littéral indépendant.
    const softCutoff = now - cfg.WINDOW_MS;
    while (arr.length > CAL_STAB.MIN_SAMPLES && arr[0].t < softCutoff) arr.shift();
  }

  function renderStability(product) {
    // Cible UNIQUEMENT le libellé de stabilité de l'étape calibrer ACTIVE (mid=1, low=3, ORP=2).
    // Les étapes franchies gardent leur libellé figé (comme leur lecture).
    let el = null, step = null;
    if (product === "ph") {
      if (_calActiveIdx.ph === 1) { el = $("#cal-ph-mid-stability"); step = "mid"; }
      else if (_calActiveIdx.ph === 3) { el = $("#cal-ph-low-stability"); step = "low"; }
    } else if (_calActiveIdx.orp === 2) {
      el = $("#cal-orp-stability"); step = "orp";
    }
    if (!el) return;

    // bug-160 : SIX états, pour la LISIBILITÉ — jamais comme support d'un
    // verrou (le bouton n'est jamais `disabled` pour instabilité). Priorité
    // d'affichage, un seul message à la fois, codée EXPLICITEMENT : en cours
    // (T4) > échec persistant (T5) > diagnostic (section 4) >
    // muet/instable/stable (T1-T3). Icônes TOUTES différentes
    // (⏳ ○ ● ↻ ✕ ⚠) : la distinction ne repose jamais sur la seule couleur
    // (daltonisme).
    el.classList.remove("is-busy", "is-failed", "is-diag", "is-stable");

    if (_calAwait && _calAwait.product === product && _calAwait.step === step) {
      _calRenderStabilityLines(el, "↻ Calibration en cours — ne retirez pas la sonde du tampon");
      el.classList.add("is-busy");
      return;
    }
    const failMsg = step && _calFailure[product][step];
    if (failMsg) {
      _calRenderStabilityLines(el, "✕ " + failMsg);
      el.classList.add("is-failed");
      return;
    }

    const arr = _calStab[product];
    const nowMs = performance.now();
    const verdict = _calStabilityVerdict(product, arr, nowMs);

    // Diagnostic (section 4, verdict ergonome P3) : 5 min après le PREMIER
    // échantillon du TAMPON COURANT, et seulement si l'étalonnage n'est
    // TOUJOURS pas confirmé stable — cède la priorité dès que « Stable »
    // redevient vrai (le mécanisme GARDE RAISON sur une sonde dégradée, seule
    // la NATURE du message change, jamais le critère, cf. gate P3).
    // ⚠️ `verdict.D != null` EXCLUT le seul cas muet où D/S n'existent pas
    // (n < 6 — réseau dégradé au point de ne pas avoir 6 échantillons après
    // 5 min) : le texte T-D1/T-D2 cite un écart RÉALISÉ (C5), jamais une
    // valeur absente. Ce cas rarissime retombe sur T1 (« lecture en cours »),
    // qui reste honnête (on n'a toujours pas assez de données).
    const sessionStart = _calStabSessionStart[product];
    const diagDue = sessionStart != null
      && (nowMs - sessionStart) >= CAL_STAB_DIAG_MS
      && verdict.state !== "stable"
      && verdict.D != null;
    if (diagDue) {
      _calRenderStabilityLines(el, _calDiagLines(product, verdict));
      el.classList.add("is-diag");
      return;
    }

    if (verdict.state === "muted") {
      // T1 — état MUET : TEXTE INCHANGÉ pour TOUS les cas muets (n < 6 OU
      // G < G_MIN, gate) — l'utilisateur n'a pas besoin de distinguer trop
      // tôt de cadence dégradée, l'action est la même (attendre).
      _calRenderStabilityLines(el, "⏳ Lecture de la sonde en cours — ne calibrez pas encore");
      return;
    }
    if (verdict.state === "stable") {
      _calRenderStabilityLines(el, "● Stable — vous pouvez calibrer");
      el.classList.add("is-stable");
      return;
    }
    // instable (section 3) : le nombre affiché EST celui qui décide, DURÉE
    // COMPRISE — les deux valeurs viennent du MÊME objet verdict, jamais une
    // seconde valeur recalculée pour l'affichage (⛔ C5 : ce serait
    // exactement le défaut Δ60 s/Δ20 s reconstitué sous une autre forme).
    const fmt = product === "ph"
      ? verdict.D.toFixed(2).replace(".", ",") + " pH"
      : Math.round(verdict.D) + " mV";
    const sSec = Math.max(0, Math.round(verdict.S / 1000));
    _calRenderStabilityLines(el, "○ La mesure bouge encore — attendez (écart " + fmt + " sur " + sSec + " s)");
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
    // feature-156 : conseil post-étalonnage — TRANSITOIRE, accroché à l'étape
    // finale du pas-à-pas pH (idx 4). Seul point de décision de sa visibilité :
    // jamais persisté, disparaît dès qu'on quitte cette étape (reset, sortie du
    // mode réglage). ORP/Température n'ont pas ce conseil (pas de carte de
    // correction derrière).
    if (product === "ph") {
      const advice = $("#ph_cal_final_advice");
      if (advice) advice.hidden = (idx !== 4);
    }
    // bug-160 (section 7, C9) : cadence /data GARANTIE à 5 s UNIQUEMENT
    // pendant une étape « Calibrer » ACTIVE (mid=1/low=3 pH, orp=2) — le
    // pouvoir de rejet du bruit de la statistique de stabilité est en 1/√n,
    // une cadence dégradée (30 s) le prive de données pendant de longues
    // minutes. Revient à la cadence normale dès qu'on quitte cette étape.
    _calStabSyncPolling();
  }

  // feature-159 (C5) : défaut REPRODUIT puis corrigé — cette fonction réactivait
  // INCONDITIONNELLEMENT tous les boutons « Calibrer » et remettait
  // `_calActiveIdx` à 0 sans jamais consulter `phCalPoints`/`orpCalPoints` (la
  // seule vérité firmware). Un point déjà calibré redevenait donc cliquable à
  // chaque réouverture de l'écran. Reste toujours à l'étape visuelle 0 (jamais
  // de saut direct à l'étape « terminé ») : ⚠️ ne JAMAIS en déduire un saut vers
  // l'étape 4 quand les 2 points sont déjà faits — ça réveillerait le conseil
  // transitoire #ph_cal_final_advice (feature-156) alors qu'aucun étalonnage
  // n'a eu lieu dans CETTE session.
  function resetCalStepper(product) {
    _calResetStabBuffer(product);
    _calClearFailure(product); // nouvelle session : pas d'échec persistant d'une ouverture passée
    setCalStepState(product, 0);

    const pts = product === "ph" ? latestSensorData?.phCalPoints : latestSensorData?.orpCalPoints;
    // feature-159 (code-reviewer, correctif bloquant) : `pts` inconnu
    // (undefined/null — /data pas encore répondu, ou hors ligne) ne doit PAS
    // se comporter comme « non calibré » (fail-open découvert en revue) : la
    // même décision pure que l'auto-réparation ci-dessous (`_calPointsAcquired`).
    Object.assign(_calFrozen[product], _calPointsAcquired(product, pts));
    const known = _calIsPtsKnown(pts);

    const btnIds = product === "ph" ? ["btn-cal-ph-mid", "btn-cal-ph-low"] : ["btn-cal-orp"];
    // EZO injoignable (T13) OU état INCONNU (ni l'un ni l'autre n'autorise un
    // geste) : n'armer AUCUN bouton — un geste qui échouerait à coup sûr, ou
    // dont on ne sait même pas s'il écraserait un point déjà fait, n'est pas
    // un recours. Sinon, repartir de « non figé » : le figeage réel des points
    // déjà acquis est appliqué juste après par `_calRefreshFrozenButtons()`,
    // qui lui ne fait QUE désactiver (jamais réactiver) — d'où la remise à
    // `false` explicite ici.
    btnIds.forEach(id => { const b = $("#" + id); if (b) b.disabled = !known; });
    _calRefreshFrozenButtons();
    renderStability(product);
  }

  // Avance manuelle (boutons « C'est fait → »).
  function advanceCalStepper(product, fromIdx) {
    const active = $$(`#${product}-cal-steps .step.is-active`)[0];
    if (!active || parseInt(active.dataset.step, 10) !== fromIdx) return;
    setCalStepState(product, fromIdx + 1);
    _calResetStabBuffer(product); // repartir d'une fenêtre propre à chaque changement de solution
    renderStability(product);
    focusActiveStep(product);
  }

  // Marque une étape « calibrer » comme faite après succès EZO et passe à la suivante.
  function completeCalStep(product, idx) {
    setCalStepState(product, idx + 1);
    _calResetStabBuffer(product);
    renderStability(product);
    focusActiveStep(product);
  }

  function focusActiveStep(product) {
    const active = $$(`#${product}-cal-steps .step.is-active`)[0];
    const btn = active ? active.querySelector("button, input") : null;
    if (btn) { try { btn.focus(); } catch (_) {} }
  }

  function bindStepperControls(product) {
    $$(`#${product}-cal-stepper .cal-step-advance`).forEach(btn => {
      btn.addEventListener("click", () => advanceCalStepper(product, parseInt(btn.dataset.advance, 10)));
    });
  }

  // ==========================================================================
  // bug-160 (section 7, C9) : cadence /data GARANTIE pendant une étape
  // « Calibrer » active — le pouvoir de rejet du bruit de la statistique de
  // stabilité est en 1/√n ; une cadence dégradée (30 s sans WebSocket) prive
  // la fenêtre de données pendant de longues minutes. ⚠️ Budget
  // `kMaxRequestsPerMinute` (constants.h) = 120 req/min/IP : ce ticker coûte
  // au plus 60000/kCalStabPollMs = 12 req/min, moins d'un dixième du quota —
  // DIT, pas supposé.
  // ==========================================================================
  const kCalStabPollMs = 5000;
  let _calStabPollTimer = null;

  function _calStabActiveStepNow() {
    return _calActiveIdx.ph === 1 || _calActiveIdx.ph === 3 || _calActiveIdx.orp === 2;
  }

  function _calStabSyncPolling() {
    const shouldPoll = _calStabActiveStepNow();
    if (shouldPoll && !_calStabPollTimer) {
      _calStabPollTimer = setInterval(() => {
        // Pendant l'attente de confirmation d'un `Cal` (_calAwait), le ticker
        // 1,5 s de `_calStartPolling()` prend déjà le relais — éviter de
        // doubler le trafic.
        if (_calAwait) return;
        loadSensorData({ force: true, source: "cal-stability" });
      }, kCalStabPollMs);
    } else if (!shouldPoll && _calStabPollTimer) {
      clearInterval(_calStabPollTimer);
      _calStabPollTimer = null;
    }
  }

  function _calStabStopPolling() {
    if (_calStabPollTimer) { clearInterval(_calStabPollTimer); _calStabPollTimer = null; }
  }

  // ==========================================================================
  // bug-160 (volet 2, REFONTE 2026-10-13) — bascule de la régulation en
  // Manuel à l'entrée en calibration, rétablie par un GESTE HUMAIN (bouton
  // « Fermer » ou liens de retour). Remplace ENTIÈREMENT l'ancien mécanisme de
  // bail auto-expirant (battement de cœur 60 s, plafond 30 min, période
  // réfractaire — abandonnés, gate pool-chemistry GATE 5 : « le bail n'est pas
  // regretté, il échouait en silence du mauvais côté »).
  // Contrat firmware (web_routes_calibration.cpp) :
  //   POST /calibration/enter → { success, ph_switched, orp_switched }
  //   POST /calibration/exit  → { success, restored, ph_mode?, orp_mode? }
  // ⛔ AUCUNE ROUTE DE RELÂCHE : /calibration/exit est le SEUL chemin de sortie
  // (appelé par le bouton de fermeture ET par les deux liens de retour, gate
  // 5/C5 — « même action »). Idempotentes des deux côtés.
  // ==========================================================================
  let _calSwitchOccurred = false; // bascule réalisée (pH et/ou ORP) pour la session ouverte

  function _calSwitchCloseLabel() {
    return _calSwitchOccurred ? "Fermer et rétablir la régulation" : "Fermer";
  }

  function _calSwitchSetCloseBtnLabel(page, label) {
    const cfg = MAINTENANCE_PAGES[page];
    const btn = cfg && $("#" + cfg.closeBtnId);
    if (btn) btn.textContent = label;
  }

  // Appelé par onEnter (geste humain, seul capable d'ouvrir une session côté
  // firmware). Best-effort : si le réseau est indisponible, on ne PEUT PAS
  // savoir si le firmware a basculé — par sécurité on suppose que oui (le
  // bouton affiche l'action de rétablissement ; /calibration/exit est de
  // toute façon idempotente si aucune session n'a réellement été ouverte).
  async function _calSwitchEnter(page) {
    _calSwitchOccurred = false;
    _calSwitchSetCloseBtnLabel(page, "Fermer");
    try {
      const res = await authFetch("/calibration/enter", { method: "POST" });
      const data = res.ok ? await res.json().catch(() => null) : null;
      _calSwitchOccurred = !!(data && (data.ph_switched || data.orp_switched));
    } catch (e) {
      _calSwitchOccurred = true; // hypothèse prudente, cf. commentaire ci-dessus
    }
    _calSwitchSetCloseBtnLabel(page, _calSwitchCloseLabel());
    _renderCalSwitchBanner(page);
  }

  // Appelé par exitMaintenance() — SUCCÈS OU NON, geste humain uniquement
  // (bouton de fermeture ou liens de retour, jamais automatique).
  async function _calSwitchExit(page) {
    _calSwitchOccurred = false;
    _calSwitchSetCloseBtnLabel(page, "Fermer");
    try {
      await authFetch("/calibration/exit", { method: "POST" });
    } catch (e) {
      // Best-effort : rien d'autre à tenter côté client — pas de route de
      // relâche alternative (invariant hérité du volet 2). La régulation
      // reste en Manuel jusqu'à ce que l'appareil soit de nouveau joignable
      // et qu'un nouvel appel (réouverture de l'écran) y parvienne — c'est le
      // compromis assumé par la refonte : un défaut VISIBLE (mode Manuel
      // affiché) plutôt qu'une minuterie invisible.
    }
  }

  // ==========================================================================
  // bug-160 (volet 2) — bandeau : PATRON des cartes/liens `.flow-maintenance`
  // (hidden par défaut, basculé en fonction de l'état), pas un mécanisme
  // d'affichage neuf. Un SEUL bandeau par page, texte FIXE imposé par le gate
  // pool-chemistry (GATE 5) — porté directement par le HTML (data/index.html),
  // ce bandeau ne fait que basculer sa visibilité.
  // ==========================================================================
  function _renderCalSwitchBanner(page) {
    // No-op pour toute page SANS bandeau (température) — ⚠️ garde EXPLICITE :
    // un ternaire par défaut aurait fait retomber "temperature" sur l'élément
    // ORP et l'aurait manipulé à tort.
    if (page !== "ph" && page !== "orp") return;
    const id = page === "ph" ? "ph-cal-switch-banner" : "orp-cal-switch-banner";
    const el = $("#" + id);
    if (!el) return;
    const view = $("#" + MAINTENANCE_PAGES[page].viewId);
    // ⚠️ DEUX conditions, pas une (relevé en revue, 2026-10-13) : le bandeau
    // affirme « la régulation a été mise en Manuel ». Le gater sur le seul mode
    // réglage le ferait MENTIR quand /calibration/enter a échoué (503 mutex) —
    // il annoncerait une bascule pendant que le bouton affiche « Fermer »,
    // c'est-à-dire l'absence de bascule. Un texte qui affirme ce qui n'a pas eu
    // lieu est un défaut, pas une approximation.
    const inMaintenance = !!(view && view.classList.contains("is-flow-maintenance"));
    el.hidden = !(inMaintenance && _calSwitchOccurred);
  }

  // ==========================================================================
  // feature-156 : mode réglage (calibration + correction), mécanisme GÉNÉRIQUE
  // ==========================================================================
  // Remplace show/hide{Ph,Orp,Temp}CalibrationCard (style inline qui écrasait
  // silencieusement la règle CSS `.is-flow-maintenance` — SUPPRIMÉS, pas
  // doublés). Une seule paire de fonctions, paramétrée par page : pas trois
  // implémentations jumelles. La règle CSS générique (app.css) masque déjà
  // tout enfant direct de la vue non marqué `.flow-maintenance` dès que la
  // classe `is-flow-maintenance` est posée sur la vue — ici on ne fait que (a)
  // sortir les cartes/liens `.flow-maintenance` de leur `hidden` par défaut,
  // (b) gérer scroll/focus/sous-titre/annonce, (c) les hooks par page.
  const MAINTENANCE_PAGES = {
    ph: {
      viewId: "view-ph", triggerBtnId: "ph_cal_trigger_btn", closeBtnId: "ph_cal_close_btn",
      backTopId: "ph-maintenance-back-top", backBottomId: "ph-maintenance-back-bottom",
      announceId: "ph-maintenance-announce", label: "pH",
      cardIds: ["ph-card-calibration", "ph-card-tune"],
      subMaintenance: "Calibration de la sonde et correction de la mesure",
      // bug-160 (volet 2, REFONTE) : POST /calibration/enter ÉMIS ICI
      // UNIQUEMENT (geste humain, seul capable de basculer la régulation
      // en Manuel côté firmware).
      onEnter: () => { resetCalStepper("ph"); renderCalibrationStatus(); _calSwitchEnter("ph"); },
      // POST /calibration/exit sur sortie, succès ou non ; arrêt du ticker
      // C9 (volet 1) même si l'étape active n'a pas été quittée proprement
      // (quitte-par-le-menu, cf. _exitAllMaintenanceModes).
      onExit: () => { phCalibrationActive = false; updatePhModeControls(); _calSwitchExit("ph"); _calStabStopPolling(); },
    },
    orp: {
      viewId: "view-orp", triggerBtnId: "orp_cal_trigger_btn", closeBtnId: "orp_cal_close_btn",
      backTopId: "orp-maintenance-back-top", backBottomId: "orp-maintenance-back-bottom",
      announceId: "orp-maintenance-announce", label: "ORP",
      cardIds: ["orp-card-calibration"],
      subMaintenance: null,
      onEnter: () => { resetCalStepper("orp"); renderCalibrationStatus(); _calSwitchEnter("orp"); },
      onExit: () => { orpCalibrationActive = false; updateOrpModeControls(); _calSwitchExit("orp"); _calStabStopPolling(); },
    },
    temperature: {
      viewId: "view-temperature", triggerBtnId: "temp_cal_trigger_btn",
      backTopId: "temperature-maintenance-back-top", backBottomId: "temperature-maintenance-back-bottom",
      announceId: "temperature-maintenance-announce", label: "Température",
      cardIds: ["temperature-card-calibration"],
      subMaintenance: null,
      onEnter: () => {
        tempCalibrationStep = 0;
        const r = $("#temp_reference_value"); if (r) r.value = "";
        updateTempCalibrationSteps();
      },
      onExit: () => {
        tempCalibrationStep = 0;
        const r = $("#temp_reference_value"); if (r) r.value = "";
        updateTempCalibrationSteps();
      },
    },
  };
  const _maintenanceScrollY = {};   // page -> window.scrollY mémorisé à l'entrée (B7)
  const _maintenanceSubSaved = {};  // page -> texte normal du sous-titre, restauré au retour

  function _maintenanceSubEl(page) {
    const view = $("#" + MAINTENANCE_PAGES[page].viewId);
    return view ? view.querySelector(".settingsHead__sub") : null;
  }

  function enterMaintenance(page) {
    const cfg = MAINTENANCE_PAGES[page];
    if (!cfg) return;
    const view = $("#" + cfg.viewId);
    if (!view || view.classList.contains("is-flow-maintenance")) return;

    _maintenanceScrollY[page] = window.scrollY;
    cfg.cardIds.forEach(id => { const el = $("#" + id); if (el) el.hidden = false; });
    const backTop = $("#" + cfg.backTopId);
    const backBottom = $("#" + cfg.backBottomId);
    if (backTop) backTop.hidden = false;
    if (backBottom) backBottom.hidden = false;
    if (cfg.subMaintenance) {
      const sub = _maintenanceSubEl(page);
      if (sub) { _maintenanceSubSaved[page] = sub.textContent; sub.textContent = cfg.subMaintenance; }
    }
    cfg.onEnter?.();
    view.classList.add("is-flow-maintenance");
    // bug-160 (volet 2) : rendu IMMÉDIAT du bandeau — la classe
    // `is-flow-maintenance` vient d'être posée. No-op pour les pages sans
    // bandeau (température).
    _renderCalSwitchBanner(page);

    const announce = $("#" + cfg.announceId);
    if (announce) announce.textContent = `Mode réglage — ${cfg.label}`;

    window.scrollTo(0, 0);
    backTop?.focus();
  }

  // `restoreView` (défaut true) : replace le défilement et rend le focus au point
  // d'entrée. On le passe à FALSE quand la sortie est provoquée par un CHANGEMENT
  // DE VUE (cf. _exitAllMaintenanceModes) : on s'en va ailleurs, replacer le
  // défilement de la page quittée se verrait comme un soubresaut, et lui rendre le
  // focus le VOLERAIT à la page d'arrivée.
  function exitMaintenance(page, restoreView = true) {
    const cfg = MAINTENANCE_PAGES[page];
    if (!cfg) return;
    const view = $("#" + cfg.viewId);
    if (!view) return;

    view.classList.remove("is-flow-maintenance");
    // bug-160 (volet 2) : masquer IMMÉDIATEMENT le bandeau — même patron que
    // les cartes ci-dessous.
    _renderCalSwitchBanner(page);
    cfg.cardIds.forEach(id => { const el = $("#" + id); if (el) el.hidden = true; });
    const backTop = $("#" + cfg.backTopId);
    const backBottom = $("#" + cfg.backBottomId);
    if (backTop) backTop.hidden = true;
    if (backBottom) backBottom.hidden = true;
    if (cfg.subMaintenance) {
      const sub = _maintenanceSubEl(page);
      if (sub && _maintenanceSubSaved[page] != null) { sub.textContent = _maintenanceSubSaved[page]; delete _maintenanceSubSaved[page]; }
    }
    cfg.onExit?.();

    const triggerBtn = $("#" + cfg.triggerBtnId);
    const y = _maintenanceScrollY[page] || 0;
    // Les cartes redeviennent `hidden` → la page n'a pas encore retrouvé sa
    // hauteur normale au moment de cet appel : restaurer le scroll APRÈS le
    // prochain repaint (B7), sinon la position cible n'existe pas encore.
    if (restoreView) {
      requestAnimationFrame(() => window.scrollTo(0, y));
      triggerBtn?.focus();
    }
  }

  // Ramène TOUTES les pages encore en mode réglage à leur état normal. Appelée à
  // chaque changement de vue (cf. showView), car on peut quitter une page par le
  // MENU sans passer par son lien de retour.
  // Idempotente, et `restoreView=false` : on part ailleurs.
  function _exitAllMaintenanceModes() {
    let left = false;
    Object.keys(MAINTENANCE_PAGES).forEach(page => {
      const cfg = MAINTENANCE_PAGES[page];
      const view = cfg && $("#" + cfg.viewId);
      if (view && view.classList.contains("is-flow-maintenance")) { exitMaintenance(page, false); left = true; }
    });
    // 🔴 LE FRÈRE DU DÉFAUT DE NAVIGATION (trouvé en revue de code).
    // `_calEndAwait()` n'était appelé que par requestExitMaintenance() — le lien
    // de retour. Quitter par le MENU pendant une calibration en vol (exactement
    // le scénario du premier défaut, une couche plus bas) laissait donc
    // `_calPollTimer` TOURNER et `_calAwait` posé ; au retour, `onEnter →
    // resetCalStepper()` s'exécutait pendant qu'une attente périmée possédait
    // encore le bouton. L'ancien hidePhCalibrationCard() l'annulait — la
    // généralisation avait perdu ce geste en chemin.
    if (left) _calEndAwait();
  }

  // Sortie demandée (lien de retour ou bouton « Fermer ») — reste TOUJOURS
  // actif pendant une calibration pH/ORP en vol (_calAwait), mais demande
  // confirmation : la sonde est alors dans un état intermédiaire. Sortir
  // n'annule QUE l'attente CLIENT (_calEndAwait()), jamais la commande déjà
  // envoyée à l'EZO (impossible à annuler côté matériel). Retourne `true` si
  // la sortie a eu lieu (permet aux appelants de n'agir qu'en cas de succès).
  async function requestExitMaintenance(page) {
    if (_calAwait && _calAwait.product === page) {
      const leave = await confirmDialog({
        title: "Calibration en cours",
        message: "Une calibration est en cours — la quitter maintenant ?",
        confirmLabel: "Quitter quand même",
        cancelLabel: "Continuer",
        danger: true,
      });
      if (!leave) return false;
      _calEndAwait();
    }
    exitMaintenance(page);
    return true;
  }

  function bindMaintenanceMode() {
    Object.keys(MAINTENANCE_PAGES).forEach((page) => {
      const cfg = MAINTENANCE_PAGES[page];
      const back = () => { requestExitMaintenance(page); };
      $("#" + cfg.backTopId)?.addEventListener("click", back);
      $("#" + cfg.backBottomId)?.addEventListener("click", back);
    });
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

  // Désactive les boutons « Calibrer » des étapes déjà franchies (figeage) : soit
  // dépassées DANS CETTE SESSION (`_calActiveIdx`), soit déjà acquises d'après le
  // firmware (`_calFrozen`, cf. resetCalStepper — feature-159/C5). N'ACTIVE
  // jamais : ce côté reste porté explicitement par les appelants (resetCalStepper,
  // _calEndAwait, les liens « Recommencer ») pour ne jamais réactiver un bouton
  // dont l'attente réseau est encore en vol.
  // Gère aussi la visibilité des liens « Recommencer » (A3) : affichés
  // uniquement pour un point RÉELLEMENT acquis (`_calFrozen`), jamais pour une
  // simple étape dépassée dans la session (la friction d'une confirmation n'a de
  // sens que si le point est vraiment calibré).
  function _calRefreshFrozenButtons() {
    const phMidFrozen = _calActiveIdx.ph > 1 || _calFrozen.ph.mid;
    const phLowFrozen = _calActiveIdx.ph > 3 || _calFrozen.ph.low;
    const orpFrozen   = _calActiveIdx.orp > 2 || _calFrozen.orp.orp;
    { const b = $("#btn-cal-ph-mid"); if (b && phMidFrozen) b.disabled = true; }
    { const b = $("#btn-cal-ph-low"); if (b && phLowFrozen) b.disabled = true; }
    { const b = $("#btn-cal-orp");    if (b && orpFrozen) b.disabled = true; }
    { const l = $("#cal-recal-ph-mid"); if (l) l.hidden = !_calFrozen.ph.mid; }
    { const l = $("#cal-recal-ph-low"); if (l) l.hidden = !_calFrozen.ph.low; }
    { const l = $("#cal-recal-orp");    if (l) l.hidden = !_calFrozen.orp.orp; }
  }

  // `isFailure` (bug-160, section 2) : un ÉCHEC confirmé (EZO injoignable,
  // délai dépassé) vide AUSSI le tampon de stabilité de l'étape concernée —
  // la sonde est peut-être en train d'être retirée/reconnectée, mélanger ces
  // échantillons à ceux qui suivront fausserait D/S/G. `false` par défaut :
  // un succès ou une sortie d'écran ne sont PAS des échecs (leurs propres
  // chemins vident déjà la fenêtre pour une autre raison, cf. completeCalStep).
  function _calEndAwait(isFailure = false) {
    if (_calPollTimer) { clearInterval(_calPollTimer); _calPollTimer = null; }
    if (_calAwait) {
      const a = _calAwait;
      _calAwait = null; // libérer AVANT de toucher le DOM (évite toute ré-entrance)
      const btn = $("#" + a.btnId);
      if (btn) { btn.disabled = false; btn.textContent = a.originalLabel; }
      if (a.otherBtnId) { const o = $("#" + a.otherBtnId); if (o) o.disabled = false; }
      if (a.roId) { const r = $("#" + a.roId); if (r) r.classList.remove("is-pulsing"); }
      if (isFailure) _calResetStabBuffer(a.product);
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
      // feature-159 (C9) : _calEndAwait() D'ABORD — il libère `_calAwait`, sans
      // quoi renderStability() (appelé par _calSetFailure) verrait encore
      // l'attente en vol et afficherait T4 au lieu de T5. ⚠️ Ne jamais affirmer
      // que la sonde est hors service : à ce stade, l'appareil sait seulement
      // qu'elle ne répond pas MAINTENANT.
      _calEndAwait(true);
      _calSetFailure(a.product, a.step, "Calibration non confirmée — sonde injoignable. Vérifiez le câblage, puis réessayez.");
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
      // feature-159 : ce point devient une vérité PERSISTANTE (survit à une
      // réouverture de l'écran, cf. resetCalStepper/C5) — et tout échec
      // antérieur sur cette étape est caduc.
      _calFrozen[a.product][a.step] = true;
      _calClearFailure(a.product, a.step);
      if (a.product === "ph") {
        completeCalStep("ph", a.step === "mid" ? 1 : 3);
        // feature-156 (A2/B11) : un étalonnage réussi peut rendre la correction
        // en vigueur CADUQUE côté firmware (ph_offset_stale) — recharger la
        // config fait apparaître la bannière sans attendre un rechargement de
        // page. Pas nécessaire côté ORP (pas de correction).
        loadConfig();
      } else completeCalStep("orp", 2);
      _calEndAwait();
      return;
    }
    if (elapsed > CAL_AWAIT_TIMEOUT_MS) {
      showToast("Calibration " + a.stepLabel + " : délai dépassé — vérifier la sonde", "warning");
      // feature-159 (C9) : ⚠️ ne jamais affirmer que la calibration précédente
      // est intacte — sur un délai dépassé, l'appareil NE SAIT PAS si l'EZO a
      // exécuté la commande ou non.
      _calEndAwait(true);
      _calSetFailure(a.product, a.step, "Calibration non confirmée — délai dépassé. Vérifiez le câblage, puis réessayez.");
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
    // feature-159 (contre-visite) : passe par _calIsPtsKnown — c'était la
    // TROISIÈME écriture de la même règle, et le commentaire de cette fonction
    // promettait « une seule expression, jamais dupliquée ». Une garantie écrite
    // plus forte que le code est pire que pas de garantie du tout.
    const baseline = _calIsPtsKnown(latestSensorData?.phCalPoints)
                   ? latestSensorData.phCalPoints : 0;
    try {
      const res = await authFetch("/calibrate_ph", {
        method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify({ step })
      });
      if (!res.ok) {
        let errMsg = "HTTP " + res.status;
        try { const j = await res.json(); if (j?.error) errMsg = j.error; } catch (_) {}
        showToast("Erreur calibration " + stepLabel + " : " + errMsg, "error");
        // feature-159 (C9) : échec persistant même quand la requête échoue
        // AVANT toute attente réseau (_calAwait n'a jamais existé ici).
        _calSetFailure("ph", step, "Calibration non confirmée — " + errMsg + ". Vérifiez le câblage, puis réessayez.");
        return;
      }
      showToast("Calibration " + stepLabel + " envoyée — exécution EZO en cours…", "info");
      if (btn) { btn.disabled = true; btn.textContent = "Calibration en cours…"; }
      const other = $("#" + otherBtnId); if (other) other.disabled = true;
      const ro = $("#" + roId); if (ro) ro.classList.add("is-pulsing");
      _calAwait = { product: "ph", step, baseline, startMs: Date.now(), btnId, otherBtnId, roId, originalLabel, stepLabel };
      renderStability("ph"); // T4 immédiat, sans attendre le prochain /data
      _calStartPolling();
    } catch (e) {
      showToast("Erreur réseau : " + (e?.message || e), "error");
      _calSetFailure("ph", step, "Calibration non confirmée — erreur réseau. Vérifiez le câblage, puis réessayez.");
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
    // feature-159 (contre-visite) : idem pH — quatrième site de la même règle.
    const baseline = _calIsPtsKnown(latestSensorData?.orpCalPoints)
                   ? latestSensorData.orpCalPoints : 0;
    try {
      const res = await authFetch("/calibrate_orp", {
        method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify({ reference })
      });
      if (!res.ok) {
        let errMsg = "HTTP " + res.status;
        try { const j = await res.json(); if (j?.error) errMsg = j.error; } catch (_) {}
        showToast("Erreur calibration ORP : " + errMsg, "error");
        _calSetFailure("orp", "orp", "Calibration non confirmée — " + errMsg + ". Vérifiez le câblage, puis réessayez.");
        return;
      }
      showToast("Calibration ORP envoyée — exécution EZO en cours…", "info");
      if (btn) { btn.disabled = true; btn.textContent = "Calibration en cours…"; }
      const ro = $("#" + roId); if (ro) ro.classList.add("is-pulsing");
      _calAwait = { product: "orp", step: "orp", baseline, startMs: Date.now(), btnId, otherBtnId: null, roId, originalLabel, stepLabel, reference };
      renderStability("orp"); // T4 immédiat, sans attendre le prochain /data
      _calStartPolling();
    } catch (e) {
      showToast("Erreur réseau : " + (e?.message || e), "error");
      _calSetFailure("orp", "orp", "Calibration non confirmée — erreur réseau. Vérifiez le câblage, puis réessayez.");
    }
  }

  // feature-159 (A3/C11) : geste « Recommencer », MÉCANISME GÉNÉRIQUE — dégèle un
  // point déjà calibré et replace le stepper à l'étape de rinçage/réglage qui le
  // précède, pour repartir proprement (fenêtre de stabilité vidée, bouton
  // réarmé). Utilisé par pH 4.0 et ORP (points « simples », C3) ; le point pH
  // 7.0 ne le réutilise PAS tel quel car il doit en plus emporter le point 4.0
  // avec lui (asymétrie matérielle — cf. bindPhCalibration).
  function _calRestartPoint(product, step, targetIdx) {
    _calFrozen[product][step] = false;
    _calClearFailure(product, step);
    _calResetStabBuffer(product);
    setCalStepState(product, targetIdx);
    const btnId = product === "ph" ? (step === "mid" ? "btn-cal-ph-mid" : "btn-cal-ph-low") : "btn-cal-orp";
    const b = $("#" + btnId); if (b) b.disabled = false;
    _calRefreshFrozenButtons();
    renderStability(product);
    focusActiveStep(product);
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
      enterMaintenance("ph"); // feature-156 : ouvre le mode réglage (calibration + correction)
    });

    // Bouton "Fermer" → quitte le mode réglage (feature-156)
    const closeBtn = $("#ph_cal_close_btn");
    closeBtn?.addEventListener("click", () => {
      requestExitMaintenance("ph");
    });

    // Boutons de calibration
    $("#btn-cal-ph-mid")?.addEventListener("click", () => calibratePh("mid"));
    $("#btn-cal-ph-low")?.addEventListener("click", () => calibratePh("low"));

    // feature-159 (A3/C3) : « Recommencer » — ASYMÉTRIQUE. Le 7.0 efface aussi
    // le 4.0 sur l'EZO (datasheet Atlas, docs/subsystems/sensors.md:202) : fait
    // matériel, pas une préférence d'interface → confirmation franche avant de
    // reperdre un point qu'on ne voulait pas toucher. Le 4.0 n'a pas cet effet
    // de bord → lien simple (_calRestartPoint), pas de confirmation.
    $("#btn-recal-ph-mid")?.addEventListener("click", async () => {
      const ok = await confirmDialog({
        title: "Recalibrer le point 7.0",
        message: "Recalibrer ce point efface aussi le point 4.00 sur la sonde : les deux seront à refaire. Il n'existe pas de retour en arrière.",
        confirmLabel: "Refaire les 2 points",
        cancelLabel: "Annuler",
        danger: true,
      });
      if (!ok) return;
      _calFrozen.ph.low = false; // emporté par Cal,mid (C3)
      _calClearFailure("ph", "low");
      { const b = $("#btn-cal-ph-low"); if (b) b.disabled = false; }
      _calRestartPoint("ph", "mid", 0);
    });
    $("#btn-recal-ph-low")?.addEventListener("click", () => _calRestartPoint("ph", "low", 2));

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
      enterMaintenance("orp"); // feature-156 : ouvre le mode réglage (calibration seule, pas de correction ORP)
    });

    const closeBtn = $("#orp_cal_close_btn");
    closeBtn?.addEventListener("click", () => {
      requestExitMaintenance("orp");
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

    // feature-159 (A3/A5) : ORP traité IDENTIQUEMENT à pH 4.0 — un seul point,
    // donc rien d'autre à effacer (C3) : lien simple, pas de confirmation.
    $("#btn-recal-orp")?.addEventListener("click", () => _calRestartPoint("orp", "orp", 1));

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

  // ---------------------------------------------------------------------
  // SHA-256 pur JS (feature-118) : crypto.subtle exige un contexte sécurisé
  // (HTTPS ou localhost). L'appareil sert du HTTP en clair sur une IP de
  // LAN — crypto.subtle y est donc INDISPONIBLE, sans repli possible.
  // Hachage incrémental par tranches de 256 Kio pour ne jamais charger
  // l'image entière en mémoire. Vecteurs de test : test/sha256_vectors.js.
  // Ce que ça prouve : l'image reçue est celle envoyée (intégrité du
  // transfert) — PAS qui l'a produite. Pas une signature.
  // ---------------------------------------------------------------------
  const SHA256_K = new Uint32Array([
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2,
  ]);

  function createSha256() {
    const h = new Uint32Array([0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19]);
    const w = new Uint32Array(64);
    const block = new Uint8Array(64);
    let blockLen = 0;
    let total = 0;

    function compress(buf, off) {
      for (let i = 0; i < 16; i++) {
        w[i] = (buf[off + i * 4] << 24) | (buf[off + i * 4 + 1] << 16) | (buf[off + i * 4 + 2] << 8) | buf[off + i * 4 + 3];
      }
      for (let i = 16; i < 64; i++) {
        const x = w[i - 15], y = w[i - 2];
        const s0 = ((x >>> 7) | (x << 25)) ^ ((x >>> 18) | (x << 14)) ^ (x >>> 3);
        const s1 = ((y >>> 17) | (y << 15)) ^ ((y >>> 19) | (y << 13)) ^ (y >>> 10);
        w[i] = (w[i - 16] + s0 + w[i - 7] + s1) | 0;
      }
      let a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
      for (let i = 0; i < 64; i++) {
        const S1 = ((e >>> 6) | (e << 26)) ^ ((e >>> 11) | (e << 21)) ^ ((e >>> 25) | (e << 7));
        const ch = (e & f) ^ (~e & g);
        const t1 = (hh + S1 + ch + SHA256_K[i] + w[i]) | 0;
        const S0 = ((a >>> 2) | (a << 30)) ^ ((a >>> 13) | (a << 19)) ^ ((a >>> 22) | (a << 10));
        const maj = (a & b) ^ (a & c) ^ (b & c);
        const t2 = (S0 + maj) | 0;
        hh = g; g = f; f = e; e = (d + t1) | 0; d = c; c = b; b = a; a = (t1 + t2) | 0;
      }
      h[0] = (h[0] + a) | 0; h[1] = (h[1] + b) | 0; h[2] = (h[2] + c) | 0; h[3] = (h[3] + d) | 0;
      h[4] = (h[4] + e) | 0; h[5] = (h[5] + f) | 0; h[6] = (h[6] + g) | 0; h[7] = (h[7] + hh) | 0;
    }

    return {
      update(bytes) {
        total += bytes.length;
        let i = 0;
        if (blockLen) {
          const need = Math.min(64 - blockLen, bytes.length);
          block.set(bytes.subarray(0, need), blockLen);
          blockLen += need;
          i = need;
          if (blockLen === 64) { compress(block, 0); blockLen = 0; }
        }
        for (; i + 64 <= bytes.length; i += 64) compress(bytes, i);
        if (i < bytes.length) {
          block.set(bytes.subarray(i), 0);
          blockLen = bytes.length - i;
        }
      },
      hex() {
        const bitLenHi = Math.floor(total / 0x20000000);
        const bitLenLo = (total * 8) >>> 0;
        const pad = new Uint8Array(blockLen < 56 ? 64 - blockLen : 128 - blockLen);
        pad[0] = 0x80;
        pad[pad.length - 8] = (bitLenHi >>> 24) & 0xff;
        pad[pad.length - 7] = (bitLenHi >>> 16) & 0xff;
        pad[pad.length - 6] = (bitLenHi >>> 8) & 0xff;
        pad[pad.length - 5] = bitLenHi & 0xff;
        pad[pad.length - 4] = (bitLenLo >>> 24) & 0xff;
        pad[pad.length - 3] = (bitLenLo >>> 16) & 0xff;
        pad[pad.length - 2] = (bitLenLo >>> 8) & 0xff;
        pad[pad.length - 1] = bitLenLo & 0xff;
        this.update(pad);
        let out = "";
        for (let i = 0; i < 8; i++) out += (h[i] >>> 0).toString(16).padStart(8, "0");
        return out;
      },
    };
  }

  // Hachage incrémental par tranches de 256 Kio (jamais le fichier entier en mémoire).
  async function sha256OfFile(file, onProgress) {
    const hasher = createSha256();
    const CHUNK = 262144;
    for (let off = 0; off < file.size; off += CHUNK) {
      const slice = file.slice(off, Math.min(off + CHUNK, file.size));
      const buf = await slice.arrayBuffer();
      hasher.update(new Uint8Array(buf));
      if (onProgress) onProgress(Math.min(off + CHUNK, file.size) / file.size);
    }
    return hasher.hex();
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

      const selectedFile = file.files[0];

      const progressWrap = $("#update_progress");
      const bar = $("#update_progress_bar");
      const status = $("#update_status");

      progressWrap.style.display = "block";
      bar.style.width = "0%";
      bar.textContent = "0%";
      bar.classList.remove("error");
      bar.setAttribute("aria-valuenow", "0");
      status.classList.remove("is-error");  // Purge l'échec précédent
      status.textContent = "Calcul de l'empreinte…";

      btn.disabled = true;
      file.disabled = true;
      type.disabled = true;

      // Empreinte SHA-256 calculée EN LOCAL (crypto.subtle indisponible : HTTP
      // clair sur IP de LAN, contexte non sécurisé). Elle prouve l'intégrité
      // du transfert — l'image reçue est celle envoyée — pas qui l'a produite.
      // Pas de repli silencieux : un échec de calcul bloque l'envoi, à l'écran.
      let sha256Hex;
      try {
        sha256Hex = await sha256OfFile(selectedFile);
      } catch (e) {
        bar.style.width = "100%";
        bar.classList.add("error");
        bar.setAttribute("aria-valuenow", "100");
        status.textContent = "✗ Impossible de calculer l'empreinte d'intégrité (" + e.message +
          "). Réessayez, ou rechargez la page.";
        status.classList.add("is-error");
        btn.disabled = false;
        file.disabled = false;
        type.disabled = false;
        return;
      }

      // ORDRE IMPOSÉ : "sha256" avant "update_file". Le firmware lit les
      // champs PENDANT le parsing de la partie fichier — un champ placé
      // après le fichier se lit comme absent (security-reviewer, feature-118).
      const formData = new FormData();
      formData.append("sha256", sha256Hex);
      formData.append("update_type", type.value);
      formData.append("update_file", selectedFile);

      status.textContent = "Préparation…";

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

  // ========== TRACE SIGNAUX CAPTEURS (feature-057, refondue feature-153) ==========
  // Deux cadences au choix (sélecteur #trace_rate_select) :
  //  - « fast » (Haute cadence) : /debug/sensor_trace (tampon RAM, volatil),
  //    trois courbes par voie (brut / médiane / lissé) ;
  //  - « day » (Journée)        : /get-history (persisté), UNE courbe par voie
  //    (valeur officielle), points fins regroupés par 10 min sur les points
  //    RAW, moyennes horaires servies telles quelles au-delà (B3).
  // Chargé à l'activation de l'onglet Avancé et sur clic « Recharger ». Pas de
  // polling permanent — aucune des deux sources n'a besoin d'un temps réel ici.
  let _traceMode = "fast";            // "fast" | "day"
  let _tracePhChart = null;
  let _traceOrpChart = null;
  let _traceChartsMode = null;        // cadence pour laquelle les graphes actuels ont été construits
  let _traceLoading = false;
  // Journée uniquement : labels d'axe X et granularité par point affiché,
  // tenus à jour avant chaque setData (closures lues par xFormat / le marqueur
  // de bascule, B15) — jamais une durée fixe, toujours dérivés des points réels.
  const _traceDayLabels = [];
  const _traceDayGranularity = [];
  // Journée (B3) : regroupement des points fins par fenêtre de 10 minutes.
  const TRACE_DAY_GROUP_WINDOW_MS = 10 * 60 * 1000;

  function _destroyTraceCharts() {
    const phHost = $("#trace_ph_chart");
    const orpHost = $("#trace_orp_chart");
    if (_tracePhChart) { unobserveChart(phHost); _tracePhChart.u.destroy(); _tracePhChart = null; }
    if (_traceOrpChart) { unobserveChart(orpHost); _traceOrpChart.u.destroy(); _traceOrpChart = null; }
    if (phHost) phHost.innerHTML = "";
    if (orpHost) orpHost.innerHTML = "";
    _traceChartsMode = null;
  }

  // (Re)construit les graphes pH/ORP pour la cadence demandée — les deux
  // cadences n'ont ni le même nombre de séries ni le même axe X, donc un
  // changement de cadence détruit et recrée (uPlot ne permet pas de changer le
  // nombre de séries d'une instance existante).
  // ---- Palette des courbes « Signaux capteurs » (étude ux-ui-designer, 2026-10-11) ----
  // SOURCE UNIQUE : ces quatre constantes sont lues par le TRACÉ (_ensureTraceCharts)
  // ET par les pastilles de la rangée de statistiques (_renderTraceStatsRows). Les hex
  // étaient dupliqués littéralement aux deux endroits : changer l'un sans l'autre
  // désynchronisait silencieusement la légende du tracé.
  // Le gris est PARTAGÉ par pH et ORP : « brut » a le même signifiant dans toute la
  // carte. Le violet et le vert sont ceux DÉJÀ utilisés par la vue Journée et par les
  // pages de détail — aucune couleur d'identité nouvelle n'est introduite ici.
  const TRACE_COLOR_RAW        = "#64748b";  // gris neutre — contraste 4,76:1 sur fond carte
  const TRACE_COLOR_MEDIAN     = "#d97706";  // ambre        — 3,19:1 (seuil graphique WCAG : 3:1)
  const TRACE_COLOR_PH_SMOOTH  = "#7c3aed";  // violet pH    — 5,71:1, identique à la vue Journée
  const TRACE_COLOR_ORP_SMOOTH = "#059669";  // vert ORP     — 3,77:1, identique à la vue Journée
  // ⚠️ Pas de thème sombre dans ce projet (vérifié : aucune règle `prefers-color-scheme`
  // dans app.css). Si un jour il en apparaît un, le gris et l'ambre sont à revérifier sur
  // fond sombre, et les deux teintes foncées devront probablement être éclaircies.

  function _ensureTraceCharts(mode) {
    if (_tracePhChart && _traceChartsMode === mode) return;
    _destroyTraceCharts();
    const phHost = $("#trace_ph_chart");
    const orpHost = $("#trace_orp_chart");
    if (mode === "fast") {
      // PALETTE (étude ux-ui-designer, 2026-10-11) — corrige un défaut signalé par
      // l'utilisateur sur la version livrée : « les couleurs des données brut et
      // lissés sont tellement proches qu'on ne voit rien ». Brut et lissé étaient
      // deux NUANCES DE LA MÊME TEINTE, et c'est justement la paire qu'on compare.
      // Trois canaux de distinction, jamais la couleur seule (accessibilité) :
      //   brut    gris neutre, trait FIN, plein    → « signal agité, pas une décision »
      //   médiane ambre,       trait moyen, POINTILLÉ → « étape de calcul »
      //   lissé   identité pH/ORP, trait ÉPAIS, tracé EN DERNIER → « c'est lui qui décide »
      // Le pointillé est le filet NON-COULEUR : sous daltonisme rouge-vert, ambre et
      // vert peuvent se rapprocher — le style de trait, lui, ne dépend d'aucune teinte.
      // ⚠️ NE PAS retirer `dash` même si la palette suffit sur un écran de bureau :
      // au bord du bassin, en plein soleil, le contraste perçu s'effondre.
      // ⚠️ Les hex sont repris TELS QUELS pour les pastilles de légende plus bas
      // (_renderTraceStatsRows) : les changer ici seulement désynchroniserait la
      // légende du tracé. D'où les constantes partagées ci-dessous.
      if (phHost) _tracePhChart = createTraceChart(phHost, [
        { label: "pH brut",    color: TRACE_COLOR_RAW,       width: 1,   dash: null },
        { label: "pH médiane", color: TRACE_COLOR_MEDIAN,    width: 1.5, dash: [5, 3] },
        { label: "pH lissé",   color: TRACE_COLOR_PH_SMOOTH, width: 2.5, dash: null }
      ]);
      if (orpHost) _traceOrpChart = createTraceChart(orpHost, [
        { label: "ORP brut",    color: TRACE_COLOR_RAW,        width: 1,   dash: null },
        { label: "ORP médiane", color: TRACE_COLOR_MEDIAN,     width: 1.5, dash: [5, 3] },
        { label: "ORP lissé",   color: TRACE_COLOR_ORP_SMOOTH, width: 2.5, dash: null }
      ]);
    } else {
      // Math.round : uPlot peut produire des splits NON ENTIERS quand les points
      // sont peu nombreux (axe indexé). Sans arrondi, `_traceDayLabels[2.5]` est
      // `undefined` et le tick s'affiche VIDE — un axe à trous se lit comme un
      // défaut de données alors que les données sont là (relevé en revue).
      const xFormat = (v) => _traceDayLabels[Math.round(v)] ?? "";
      const getGran = () => _traceDayGranularity;
      if (phHost) _tracePhChart = createTraceChart(phHost, [
        { label: "pH", color: "#7c3aed" }
      ], { xFormat, extraPlugins: [granularityMarkerPlugin(getGran)] });
      if (orpHost) _traceOrpChart = createTraceChart(orpHost, [
        { label: "ORP", color: "#059669" }
      ], { xFormat, extraPlugins: [granularityMarkerPlugin(getGran)] });
    }
    _traceChartsMode = mode;
  }

  // Offset pH courant (feature-153, B11/A5) : la trace est enregistrée BRUTE
  // (`_phFilter.median()`, jamais `getPhMedian()` — ADR-0039, l'offset
  // s'applique en sortie des accesseurs). Absent/invalide → null, JAMAIS une
  // valeur fabriquée (0 serait une correction silencieuse, pas une absence).
  function _currentPhOffsetOrNull() {
    const v = window._config?.ph_offset;
    return (typeof v === "number" && Number.isFinite(v)) ? v : null;
  }

  function _fmtHourMinute(ms) {
    const d = new Date(ms);
    return `${String(d.getHours()).padStart(2, "0")}:${String(d.getMinutes()).padStart(2, "0")}`;
  }

  // Trois messages DISTINCTS (B10) : jamais le même texte pour des causes
  // différentes — un lecteur en diagonale doit les distinguer sans relire.
  function _renderTraceMessage(kind, text) {
    const emptyEl = $("#trace_empty");
    const errEl = $("#trace_error");
    const chartsEl = $("#trace_charts");
    if (emptyEl) emptyEl.hidden = true;
    if (errEl) errEl.hidden = true;
    if (chartsEl) chartsEl.hidden = true;
    if (kind === "empty") {
      if (emptyEl) { emptyEl.textContent = text; emptyEl.hidden = false; }
    } else {
      // "error" (appareil injoignable), "heap" (503 de /debug/sensor_trace :
      // garde heap réelle) et "busy" (503 de /get-history : mutex de
      // l'historique) partagent le même style d'alerte (callout--warn) mais
      // JAMAIS le même texte (B10). Les deux 503 ont des CAUSES DIFFÉRENTES —
      // les confondre envoie chercher un problème de mémoire qui n'existe pas.
      if (errEl) { errEl.textContent = text; errEl.hidden = false; }
    }
  }

  function _showTraceCharts() {
    const emptyEl = $("#trace_empty");
    const errEl = $("#trace_error");
    const chartsEl = $("#trace_charts");
    if (emptyEl) emptyEl.hidden = true;
    if (errEl) errEl.hidden = true;
    if (chartsEl) chartsEl.hidden = false;
  }

  // Texte d'intro PAR POSITION (B12e) : le tampon est volatil, l'historique
  // persiste — ce ne sont pas la même promesse, le texte ne doit pas le taire.
  function _renderTraceIntro(mode, meta) {
    const el = $("#trace_intro");
    if (!el) return;
    if (mode === "fast") {
      const dur = traceWindowDurationMs(meta.count, meta.intervalMs);
      el.textContent = `Trace haute cadence (un point par cycle capteur) : ${meta.count || 0} point(s) `
        + `enregistré(s), soit ${_formatTraceDurationFr(dur)} de couverture. Volatile : perdue au `
        + `redémarrage de l'appareil.`;
    } else {
      el.textContent = "Vue Journée (historique persisté, survit à un redémarrage) : points fins "
        + "regroupés par 10 minutes sur les dernières heures, puis moyennes horaires au-delà.";
    }
  }

  // Rappelle l'échelle d'affichage (B11) : jamais de valeur fabriquée si
  // l'offset est indisponible — la vue Journée est TOUJOURS corrigée (la
  // correction est faite par le firmware, point de vérité ADR-0039).
  function _renderTraceScaleNote(mode, offsetOk) {
    const el = $("#trace_scale_note");
    if (!el) return;
    if (mode === "day") {
      el.textContent = "Échelle corrigée (offset pH appliqué) — comparable au tableau de bord.";
      return;
    }
    el.textContent = offsetOk
      ? "Échelle corrigée (offset pH appliqué) — comparable à la vue Journée et à un test manuel."
      : "Échelle BRUTE (offset pH indisponible pour l'instant) — pas encore comparable à un test manuel.";
  }

  // Un seul sous-titre au-dessus du bloc de statistiques (B13), jamais répété
  // par ligne — sa durée se recalcule, elle ne se code pas en dur.
  function _setTraceStatsSubtitle(durationMs) {
    const el = $("#trace_stats_subtitle");
    if (!el) return;
    el.textContent = `Sur la fenêtre affichée (${_formatTraceDurationFr(durationMs)}) :`;
  }

  function _formatSeriesStatsText(stats, decimals) {
    if (!stats || stats.n === 0) return "—";
    const f = (v) => v.toFixed(decimals);
    return `min ${f(stats.min)} · max ${f(stats.max)} · moy ${f(stats.avg)}`;
  }

  // Une ligne par courbe affichée, TOUJOURS — une courbe sans valeur encore
  // disponible (médiane au tout début d'une fenêtre, feature-153 B14) garde sa
  // ligne avec "—", elle ne s'évapore pas (une ligne qui disparaît se lit
  // comme un défaut).
  function _renderTraceStatsRows(container, rows) {
    if (!container) return;
    container.textContent = "";
    rows.forEach(r => {
      const row = document.createElement("div");
      row.className = "kv__row";
      const k = document.createElement("span");
      k.className = "kv__k";
      const swatch = document.createElement("span");
      swatch.className = "trace-legend__swatch";
      swatch.style.background = r.color;
      k.appendChild(swatch);
      k.appendChild(document.createTextNode(r.label));
      const v = document.createElement("span");
      v.className = "kv__v";
      v.textContent = _formatSeriesStatsText(r.stats, r.decimals);
      row.appendChild(k);
      row.appendChild(v);
      container.appendChild(row);
    });
  }

  // ---- Cadence « Haute cadence » : /debug/sensor_trace (tampon RAM) ----
  async function _loadTraceFast() {
    // Route paginée (bug-crash-sensor-trace-badalloc) : on agrège les pages
    // séquentiellement jusqu'à couvrir `count` (total buffer). Borne dérivée
    // du couple (count, returned) réellement servi par l'API (B9) — jamais
    // recalée sur un couple figé (300/50 hier, 200/38 aujourd'hui, un autre
    // demain).
    const samples = [];
    let offset = 0;
    let total = 0;
    let intervalMs = 0;
    let maxIterations = 1;
    for (let i = 0; i < maxIterations; i++) {
      const res = await authFetch(`/debug/sensor_trace?offset=${offset}`);
      if (res.status === 503) {
        _renderTraceMessage("heap", "Mémoire de l'appareil fragmentée, réessayez dans un instant.");
        return;
      }
      if (!res.ok) throw new Error(`HTTP ${res.status}`);
      const json = await res.json();
      const page = Array.isArray(json.samples) ? json.samples : [];
      samples.push(...page);
      const returned = Number(json.returned ?? page.length);
      total = Number(json.count ?? 0);
      if (Number.isFinite(json.interval_ms)) intervalMs = json.interval_ms;
      if (i === 0) maxIterations = returned > 0 ? Math.ceil(total / returned) : 1;
      if (returned === 0 || total === 0) break;  // buffer vide ou fin de page
      offset += returned;
      if (offset >= total) break;                // trace complète
    }

    const countEl = $("#trace_count");
    if (countEl) { countEl.hidden = false; countEl.textContent = String(samples.length); }

    _renderTraceIntro("fast", { count: total, intervalMs });

    if (samples.length === 0) {
      _renderTraceMessage("empty", "Aucune donnée dans le buffer de trace.");
      return;
    }

    _ensureTraceCharts("fast");

    const offsetPh = _currentPhOffsetOrNull();
    const addOffset = (arr) => offsetPh == null ? arr : arr.map(v => (v == null ? null : v + offsetPh));
    const num = (v) => (v == null || isNaN(v)) ? null : v;
    const t0 = samples[0].t ?? 0;
    // Secondes relatives, arrondies à 0,1 s — l'axe reste bâti sur les
    // horodatages réels, jamais sur un pas supposé constant.
    const xs = samples.map(s => Math.round((((s.t ?? 0) - t0) / 100)) / 10);
    const phRaw = addOffset(samples.map(s => num(s.phRaw)));
    const phMed = addOffset(samples.map(s => num(s.phMedian)));
    const phFilt = addOffset(samples.map(s => num(s.phFiltered)));
    const orpRaw = samples.map(s => num(s.orpRaw));
    const orpMed = samples.map(s => num(s.orpMedian));
    const orpFilt = samples.map(s => num(s.orpFiltered));

    _tracePhChart?.setData(xs, [phRaw, phMed, phFilt]);
    _traceOrpChart?.setData(xs, [orpRaw, orpMed, orpFilt]);

    _renderTraceScaleNote("fast", offsetPh != null);
    _setTraceStatsSubtitle(traceWindowDurationMs(samples.length, intervalMs));
    _renderTraceStatsRows($("#trace_stats_rows"), [
      // Mêmes constantes que les courbes ci-dessus : la pastille de légende DOIT
      // être la couleur du tracé, sinon la rangée de statistiques ment.
      { color: TRACE_COLOR_RAW,        label: "pH brut",    stats: seriesStats(phRaw),  decimals: 2 },
      { color: TRACE_COLOR_MEDIAN,     label: "pH médiane", stats: seriesStats(phMed),  decimals: 2 },
      { color: TRACE_COLOR_PH_SMOOTH,  label: "pH lissé",   stats: seriesStats(phFilt), decimals: 2 },
      { color: TRACE_COLOR_RAW,        label: "ORP brut",    stats: seriesStats(orpRaw),  decimals: 1 },
      { color: TRACE_COLOR_MEDIAN,     label: "ORP médiane", stats: seriesStats(orpMed),  decimals: 1 },
      { color: TRACE_COLOR_ORP_SMOOTH, label: "ORP lissé",   stats: seriesStats(orpFilt), decimals: 1 }
    ]);

    _showTraceCharts();
  }

  // ---- Cadence « Journée » : /get-history (persisté) ----
  async function _loadTraceDay() {
    const res = await authFetch("/get-history?range=24h");
    if (res.status === 503) {
      // ⚠️ CORRIGÉ EN REVUE (feature-153) : ce 503 n'a RIEN à voir avec le heap.
      // Il vient de `history.snapshotFiltered()` qui n'a pas pu prendre son mutex
      // (`web_routes_data.cpp` : « historique occupé, réessayez »). Le texte
      // « mémoire fragmentée » — correct pour /debug/sensor_trace, qui lui a bien
      // une garde heap — était recopié ici et MENTAIT sur la cause. B10 exige
      // trois messages DISTINCTS *et VRAIS* : un message faux est pire qu'un
      // message générique, il envoie chercher un problème qui n'existe pas.
      _renderTraceMessage("busy", "Historique momentanément occupé, réessayez dans un instant.");
      return;
    }
    if (!res.ok) throw new Error(`HTTP ${res.status}`);
    const json = await res.json();
    const history = Array.isArray(json.history) ? json.history : [];

    const countEl = $("#trace_count");
    if (countEl) countEl.hidden = true;  // compteur de tampon : sans objet hors Haute cadence

    _renderTraceIntro("day", {});

    if (history.length === 0) {
      _renderTraceMessage("empty", "Aucune donnée dans l'historique.");
      return;
    }

    _ensureTraceCharts("day");

    const num = (v) => (v == null || isNaN(v)) ? null : v;
    // feature-152 : point.ph / point.orp PORTENT DÉJÀ la valeur officielle
    // (getPhFiltered()/getOrpFiltered(), corrigée côté pH) — une seule courbe
    // par voie (B3). point.phFiltered/orpFiltered (NAN/"null" depuis
    // feature-152) sont IGNORÉS ici : tolérés sans planter, jamais lus comme
    // une seconde courbe.
    // Timestamps /get-history en SECONDES epoch → convertis en millisecondes
    // pour groupHistoryPoints (même unité que windowMs, B3).
    const rawPts = history.filter(p => (p.granularity ?? 0) === 0)
      .map(p => ({ timestamp: p.timestamp * 1000, ph: num(p.ph), orp: num(p.orp), granularity: 0 }));
    const aggPts = history.filter(p => (p.granularity ?? 0) !== 0)
      .map(p => ({ timestamp: p.timestamp * 1000, ph: num(p.ph), orp: num(p.orp), granularity: p.granularity }));
    const grouped = groupHistoryPoints(rawPts, TRACE_DAY_GROUP_WINDOW_MS);
    const merged = grouped.concat(aggPts).sort((a, b) => a.timestamp - b.timestamp);

    if (merged.length === 0) {
      _renderTraceMessage("empty", "Aucune donnée dans l'historique.");
      return;
    }

    _traceDayLabels.length = 0;
    _traceDayLabels.push(...merged.map(p => _fmtHourMinute(p.timestamp)));
    _traceDayGranularity.length = 0;
    _traceDayGranularity.push(...merged.map(p => p.granularity));

    const xs = merged.map((_, i) => i);
    const phVals = merged.map(p => p.ph);
    const orpVals = merged.map(p => p.orp);

    _tracePhChart?.setData(xs, [phVals]);
    _traceOrpChart?.setData(xs, [orpVals]);

    _renderTraceScaleNote("day", true);
    const firstTs = merged[0].timestamp, lastTs = merged[merged.length - 1].timestamp;
    _setTraceStatsSubtitle(lastTs - firstTs);
    _renderTraceStatsRows($("#trace_stats_rows"), [
      { color: "#7c3aed", label: "pH", stats: seriesStats(phVals), decimals: 2 },
      { color: "#059669", label: "ORP", stats: seriesStats(orpVals), decimals: 1 }
    ]);

    _showTraceCharts();
  }

  async function loadSensorTrace() {
    if (_traceLoading) return;
    _traceLoading = true;
    try {
      if (_traceMode === "day") await _loadTraceDay();
      else await _loadTraceFast();
    } catch (e) {
      console.error("[trace] load failed:", e);
      _renderTraceMessage("error", "Trace indisponible (appareil injoignable).");
    } finally {
      _traceLoading = false;
    }
  }

  function bindSensorTrace() {
    setSegmented("trace_rate_select", _traceMode);

    $$("#trace_rate_select .segmented__btn").forEach(btn => {
      btn.addEventListener("click", () => {
        const mode = btn.dataset.value;
        if (mode === _traceMode || (mode !== "fast" && mode !== "day")) return;
        _traceMode = mode;
        setSegmented("trace_rate_select", mode);
        // B12c : « Vider le buffer » agit sur la RAM — sans effet observable
        // hors Haute cadence, donc trompeur à y laisser visible.
        const clearBtn = $("#trace_clear_btn");
        if (clearBtn) clearBtn.hidden = (mode !== "fast");
        loadSensorTrace();
      });
    });

    $("#trace_reload_btn")?.addEventListener("click", () => loadSensorTrace());

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
      if (_isPanelDevActive()) loadSensorTrace();
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
    // bug-082 : le repli n'est PAS un alias de 74, et ne doit jamais l'être.
    //   1. Sur firmware ANCIEN (avant la fusion de flotte), la pompe débite
    //      réellement 61 / 67 : annoncer 74 raccourcirait l'estimation de 18 a
    //      21 % sous la marche réelle. L'utilisateur voit la pompe tourner
    //      « trop longtemps », en déduit un défaut et RELANCE — la sur-injection
    //      vient de cette répétition, pas du premier pouls.
    //   2. Sur firmware À JOUR, aligner le repli sur 74 rendrait la présence du
    //      champ INDISCERNABLE de son absence : ce bug (5e récidive du motif de
    //      parité) deviendrait invisible pour toujours, et la 6e récidive
    //      muette. Le repli distinct EST le détecteur.
    const FLOW_255_FALLBACK = { 1: 61.0, 2: 67.0 };
    let _flow255ShapeWarned = false;
    const flow255For = (pumpNum) => {
      // flow255 est un TABLEAU de 2 éléments (index 0 = pompe 1), jamais un
      // scalaire : un nombre servi directement serait ignoré, volontairement —
      // on ne devine pas à quelle pompe il appartient.
      const fa = latestSensorData?.flow_assist;
      const served = fa?.flow255;
      const v = Array.isArray(served) ? served[pumpNum - 1] : undefined;
      const ok = (typeof v === "number" && isFinite(v) && v > 0);
      // Repli atteint ALORS QUE flow_assist est servi = la forme a changé côté
      // firmware. On le dit une fois en console : un repli silencieux est
      // exactement ce qui a rendu ce bug invisible pendant une version entière.
      if (!ok && fa && !_flow255ShapeWarned) {
        _flow255ShapeWarned = true;
        console.warn("flow_assist.flow255 : forme inattendue, repli 61/67 appliqué", served);
      }
      return ok ? v : FLOW_255_FALLBACK[pumpNum];
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
            + ` et vérifiez l’offset pH${product === "ph" ? " (carte « Correction manuelle de la mesure »)"
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

    // bug-082 : la trame WS ne porte que flow255 et aborted ; les champs de
    // SESSION (purge_s, measure_s, purges_left, active, phase) restent servis
    // par /data SEULS. Comme latestSensorData est REMPLACÉ à chaque trame WS,
    // les lire sans rafraîchissement forcé donne `undefined` la quasi-totalité
    // du temps : l'assistant retomberait en permanence sur ses miroirs 60/120/3
    // — le 4e cas du motif de parité (« miroir client en dur »), qu'on refuse de
    // réintroduire. Un GET /data à l'ouverture (précédent : verifyRunEnd) est le
    // seul moment où ces champs sont fiables ; il n'ajoute aucun rythme réseau.
    async function openFlowWizard(n) {
      const cfg = window._config || {};
      const phPump = cfg.ph_pump === 2 ? 2 : 1;
      wiz.pump = n;
      wiz.measureDurationS = wiz.measureS;
      wiz.running = null;
      wiz.sessionActive = false;

      const title = $("#flow-wizard-title");
      if (title) title.textContent = `Mesure du débit — Pompe ${n} (${phPump === n ? "pH" : "Chlore"})`;
      const budget = $("#flow-wizard-budget");
      const next0Btn = $("#flow-wizard-next-0");

      // Revue bug-082 : la carte s'affiche AVANT le rafraîchissement. Le GET
      // /data peut durer jusqu'à 10 s (timeout de loadSensorData) : attendre
      // pour ouvrir donnerait un bouton « Mesurer » apparemment mort. L'attente
      // est signalée LÀ OÙ ELLE PORTE — la ligne de budget, dont la valeur
      // dépend de la réponse — et « continuer » est neutralisé le temps de la
      // requête, pour qu'on ne puisse pas avancer sur des durées de repli.
      resetStepsUi();
      if (expiredNote) expiredNote.hidden = true;
      gotoStep(0);
      card.hidden = false;
      $("#view-settings")?.classList.add("is-flow-wizard");
      setEntryButtons(true);  // une pompe à la fois : entrées neutralisées
      card.scrollIntoView?.({ behavior: "smooth", block: "start" });
      if (budget) budget.textContent = "lecture des paramètres en cours…";
      if (next0Btn) next0Btn.disabled = true;

      await loadSensorData({ force: true, source: "flow-wizard" });
      if (next0Btn) next0Btn.disabled = false;

      // Durées servies par /data (rafraîchies juste au-dessus ; miroirs en repli)
      const fa = latestSensorData?.flow_assist;
      if (fa) {
        if (fa.purge_s > 0) wiz.purgeS = fa.purge_s;
        if (fa.measure_s > 0) wiz.measureS = fa.measure_s;
      }
      wiz.purgesLeft = (fa && fa.active && fa.pump === n && typeof fa.purges_left === "number")
        ? fa.purges_left : 3;
      wiz.measureDurationS = wiz.measureS;
      // Réétiquetage des boutons (« Purger (60 s) »…) avec les durées SERVIES :
      // le premier appel, avant la requête, portait encore les miroirs.
      resetStepsUi();
      if (budget) {
        // Débit majorant SERVI par /data (flow_assist.flow255[n-1]) — plus de
        // miroir en dur (revue).
        // Repli PAR POMPE et non générique (revue v2.25.2) : un repli à 61 pour
        // les deux sous-annonçait le budget de 10 % sur la pompe 2, dont les
        // firmwares ANTÉRIEURS à la fusion de flotte dosent bien à 67.
        // Logique dupliquée volontairement plutôt qu'appelée via flow255For() :
        // celle-ci est déclarée dans un autre bloc (const = portée de bloc).
        // bug-082 : `fa` vient du GET /data forcé en tête de openFlowWizard —
        // c'est ce qui rend cette ligne de budget juste (74 ml/min servis, soit
        // ≈ 74 ml par purge) au lieu des ≈ 61 sous-annoncés quand la lecture
        // tombait sur une trame WS. Le consentement de l'opérateur porte
        // désormais sur le volume réellement injecté.
        // flow255 est un TABLEAU de 2 éléments : un scalaire est ignoré.
        const servi = fa && Array.isArray(fa.flow255) ? fa.flow255[n - 1] : undefined;
        const f = (typeof servi === "number" && isFinite(servi) && servi > 0)
          ? servi : (n === 2 ? 67.0 : 61.0);
        budget.textContent =
          `≈ ${Math.ceil(f * wiz.purgeS / 60)} ml par purge, ≈ ${Math.ceil(f * wiz.measureS / 60)} ml pour la mesure`;
      }
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
      // bug-082 — NE PAS SUPPRIMER : garde préventive, donc sans effet visible
      // aujourd'hui. Ce hook est appelé AUSSI sur les trames WebSocket
      // (loadSensorData source:'ws'), qui portent désormais flow_assist —
      // flow255 et aborted, mais PAS `active`. Le jour où une trame WS publiera
      // un `active` partiel ou par défaut, chaque trame reçue serait lue comme
      // une expiration de session : retour à l'étape 0 et purges relancées,
      // éventuellement pompe en marche. On n'agit donc que sur une source qui
      // porte VRAIMENT l'état de session : clé absente ≠ session finie.
      if (fa.active === undefined) return;
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
    bindPhTuneSubmit();  // feature-155 : carte « Correction manuelle de la mesure »
    bindPhOffsetStaleBanner();  // feature-156 (B11) : bannière « correction caduque »
    bindPhTuneRemoveRow();  // feature-157 : bouton « Supprimer la correction »
    bindPhTargetControl();  // v2.25.0 : contrôle segmenté de la consigne pH
    // v2.25.0 : rappel de la plage sous le contrôle.
    $("#ph_target")?.addEventListener("input", renderPhTargetHint);
    // Onglet Régulation : initialisation segmented + save dédié
    initSegmented("regulation_speed");
    // feature-090 : sélecteur de taille du bassin (dialogue avant/après 088).
    // initSegmented rend le clic optimiste ; _onPoolSizeClick confirme ou revert.
    initSegmented("pool_size");
    $$(`#pool_size .segmented__btn`).forEach(btn => {
      btn.addEventListener("click", () => { _onPoolSizeClick(btn.dataset.value); });
    });
    $("#regulation_save_btn")?.addEventListener("click", async () => {
      // feature-056 : regulation_mode retiré (remplacé par le mode d'installation).
      // feature-091 : stabilization_delay_min retiré (purge adaptative) — la clé
      // n'est plus émise ; le firmware l'ignore de toute façon.
      // feature-090 (B2) : quand la taille du bassin pilote les défauts, le
      // segmented vitesse est MASQUÉ — on réémet alors la valeur COURANTE du
      // contrôleur (window._config, complétée par le cache anti-trame-WS-
      // partielle de syncPoolSize), jamais un défaut ni un segmented invisible.
      // Face à un ancien firmware (pool_size absent), le segmented est visible
      // et reste la source du choix utilisateur en attente.
      const speedHidden = !!(window._config && window._config.pool_size);
      const speed = speedHidden
        ? (window._config.regulation_speed || getSegmented("regulation_speed") || "normal")
        : (getSegmented("regulation_speed") || "normal");
      const phLimit = parseInt($("#ph_limit")?.value ?? "5", 10);
      const phDaily = parseFloat($("#ph_daily_limit")?.value ?? "300");
      const orpLimit = parseInt($("#orp_limit")?.value ?? "10", 10);
      const orpDaily = parseFloat($("#orp_daily_limit")?.value ?? "500");
      const maxCycles = parseInt($("#max_cycles_per_day")?.value ?? "48", 10);
      const ok = await sendConfig({
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
    // feature-088 : restauration des valeurs d'usine de régulation
    $("#regulation_reset_btn")?.addEventListener("click", _onRegulationResetClick);

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
      // feature-083 : bande = plage idéale de l'eau (7,2-7,6) + ligne de
      // consigne ph_target (tracée à réception de la config — jamais inventée)
      phChart = createLineChart(phChartCanvas, "#8b5cf6", "pH", {
        fill: false,
        axisMin: PH_AXIS_MIN_DEFAULT,
        axisMax: PH_AXIS_MAX_DEFAULT,
        referenceLines: {
          max: PH_IDEAL_MAX,
          min: PH_IDEAL_MIN,
          zoneColor: PH_ZONE_COLOR,
          lineColor: PH_LINE_COLOR
        },
        targetLine: { color: TARGET_LINE_COLOR, dash: TARGET_LINE_DASH }
      });
    }
    if (orpChartCanvas) {
      // feature-083 : plus de bande ORP (aucune plage universelle) — ligne de
      // consigne orp_target seule
      orpChart = createLineChart(orpChartCanvas, "#10b981", "ORP", {
        integerOnly: true,
        fill: false,
        axisMin: ORP_AXIS_MIN_DEFAULT,
        axisMax: ORP_AXIS_MAX_DEFAULT,
        targetLine: { color: TARGET_LINE_COLOR, dash: TARGET_LINE_DASH }
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
    bindOrpCalChip();
    bindOrpCalibration();
    bindTempCalibration();
    bindMaintenanceMode();  // feature-156 : liens de retour (haut/bas) des trois pages
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

    // feature-147 (correctif) : plus aucun mini-graphique de tableau de bord
    // (pH/ORP/Température retirés) — createMiniLineChart/loadMiniChartData et
    // leur polling 5 min disparaissent avec le dernier appelant.

    // Load historical data to populate charts
    const historyPerf = debugStart("loadHistoricalData");
    await loadHistoricalData('all').catch(() => {});
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
  // feature-146 : mêmes boutons que le tableau de bord (#detail-filtration-*),
  // même chemin partagé setFiltrationRunMode() — changer le mode, rien d'autre.
  function setupFiltrationManualControl() {
    const startBtn = $("#filtration-manual-start");
    const stopBtn = $("#filtration-manual-stop");

    if (startBtn) {
      startBtn.addEventListener("click", () => setFiltrationRunMode("force"));
    }

    if (stopBtn) {
      stopBtn.addEventListener("click", () => setFiltrationRunMode("off"));
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
