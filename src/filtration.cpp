#include "filtration.h"
#include "config.h"
#include "constants.h"
#include "logger.h"
#include "sensors.h"
#include "mqtt_manager.h"
#include "pump_controller.h"
#include "measure_validity.h"
#include "dosing_logic.h"  // shouldRolloverByDate (bug-126)
#include "schedule_logic.h"
#include <time.h>

// bug-126 (arbitrage B) — selectFiltrationRefTemp() encode les rôles de sonde
// en uint8_t (0=Unknown, 1=Water, 2=Circuit) pour rester dans un module pur
// sans Arduino.h. Vérifie que l'enum C++ colle à cet encodage : un
// réordonnancement de SondeRole casserait le repli sans qu'aucun compilateur
// ne le signale autrement.
static_assert(static_cast<uint8_t>(SondeRole::Unknown) == 0 &&
                  static_cast<uint8_t>(SondeRole::Water) == 1 &&
                  static_cast<uint8_t>(SondeRole::Circuit) == 2,
              "Encodage SondeRole incompatible avec selectFiltrationRefTemp (schedule_logic).");

// Formate des minutes depuis minuit en chaîne "HH:MM" (helper local coquille).
static String minutesToTimeString(int minutes) {
  int h = (minutes / 60) % 24;
  int m = minutes % 60;
  char buffer[6];
  snprintf(buffer, sizeof(buffer), "%02d:%02d", h, m);
  return String(buffer);
}

FiltrationManager filtration;

void FiltrationManager::begin() {
  // bug-126 (M2, revue P2) : SEULE source de vérité de l'état initial de
  // l'accumulateur. `_cycleTemp` n'a plus de `= {}` en tête (filtration.h) —
  // un zéro-init donnerait mean=0.0f, en violation du contrat « NaN <=>
  // count==0 » (schedule_logic.h), et 0.0 °C est une valeur hors bornes
  // basses qui déclencherait à tort une alerte si jamais lue avant le
  // premier front montant.
  resetCycleTempAccumulator(_cycleTemp);

  pinMode(kFiltrationRelayPin, OUTPUT);
  ensureTimesValid();

  // Calculer et appliquer l'état initial du relais immédiatement.
  // Pré-requis satisfaits dans main.cpp : config et heure RTC chargées avant cet appel.
  // Cela évite une coupure prolongée de la pompe de filtration au reboot :
  // sans ce calcul, le relais resterait LOW pendant tout le setup (WiFi inclus, jusqu'à ~18s)
  // avant que filtration.update() dans loop() ne le remette dans son état correct.
  // feature-056 : le relais n'est piloté que dans le mode ManagedFiltration.
  bool initialState = false;
  if (mqttCfg.installMode == InstallMode::ManagedFiltration) {
    String mode = filtrationCfg.mode;
    mode.toLowerCase();
    if (mode != "off") {
      if (filtrationCfg.forceOn) {
        initialState = true;
      } else if (!filtrationCfg.forceOff && (mode == "manual" || mode == "auto")) {
        int nowMinutes = 0;
        if (getCurrentMinutesOfDay(nowMinutes)) {
          int startMin = timeStringToMinutes(filtrationCfg.start);
          int endMin = timeStringToMinutes(filtrationCfg.end);
          initialState = isMinutesInRange(nowMinutes, startMin, endMin);
        }
      }
    }
  }

  digitalWrite(kFiltrationRelayPin, initialState ? HIGH : LOW);
  relayState = initialState;
  state.running = initialState;
  if (initialState) {
    state.startedAtMs = millis();
    systemLogger.info("Gestionnaire de filtration initialisé (filtration maintenue active)");
  } else {
    systemLogger.info("Gestionnaire de filtration initialisé");
  }
}

void FiltrationManager::ensureTimesValid() {
  String modeLower = filtrationCfg.mode;
  modeLower.toLowerCase();
  if (modeLower != "auto" && modeLower != "manual" && modeLower != "off" && modeLower != "force") {
    filtrationCfg.mode = "auto";
    // Corrections mineures (revue P2) : `modeLower` ne doit pas rester périmée
    // sur cette branche — le check C7 plus bas la réutilise telle quelle
    // (évite une 2e allocation String par tour de update()).
    modeLower = "auto";
  }

  auto normalize = [](String value, const char* fallback) {
    if (value.length() < 5 || value.charAt(2) != ':') return String(fallback);
    int hh = value.substring(0, 2).toInt();
    int mm = value.substring(3, 5).toInt();
    if (hh < 0 || hh > 23 || mm < 0 || mm > 59) return String(fallback);
    char buffer[6];
    snprintf(buffer, sizeof(buffer), "%02d:%02d", hh, mm);
    return String(buffer);
  };

  filtrationCfg.start = normalize(filtrationCfg.start, "08:00");
  filtrationCfg.end = normalize(filtrationCfg.end, "20:00");

  // bug-126 (condition C7 pool-chemistry) — mode AUTO uniquement : une plage
  // nulle (start == end) signifie « la filtration ne tourne plus jamais » en
  // silence (le cas 48°C sature computeAutoWindow à start==end). En mode
  // manual une plage nulle peut être VOULUE par l'utilisateur, on ne la
  // touche pas. Auto-limitant : une fois repliée sur 08:00-20:00, la
  // condition est fausse au tour suivant — aucune boucle de réparation.
  if (modeLower == "auto" && filtrationCfg.start == filtrationCfg.end) {
    filtrationCfg.start = "08:00";
    filtrationCfg.end = "20:00";
    // Le critical part immédiatement, y compris au boot (systemLogger est
    // déjà debout). L'alerte MQTT, elle, NE PEUT PAS : ensureTimesValid() est
    // aussi appelée depuis begin(), AVANT mqttManager.begin() — outQueue
    // n'existe pas encore et publishAlert() serait un no-op silencieux
    // (mqtt_manager.cpp:570), même piège que main.cpp:399 pour l'alerte
    // history (P2-C, revue). On mémorise la réparation ; update() la publie
    // au premier tour où MQTT est connecté.
    systemLogger.critical("[Filtration] Planning auto invalide (plage nulle) — repli sur 08:00-20:00");
    _scheduleRepairAlertPending = true;
  }
}

// bug-126 — voie « mise en service » : application IMMÉDIATE d'un planning
// auto, appelée par web_routes_config.cpp (mode qui vient de passer à "auto")
// et mqtt_manager.cpp (commande MQTT `filtration_mode=auto`). N'est PAS le
// chemin emprunté pendant la marche : celui-là accumule dans
// sampleCycleTemperature() et fige dans writePendingSchedule() à l'arrêt.
void FiltrationManager::computeAutoSchedule() {
  if (!filtrationCfg.mode.equalsIgnoreCase("auto")) return;

  // La garde de circulation évite de figer un planning sur l'eau stagnante
  // des tuyauteries dès le premier appel après un changement de mode.
  if (!measureValidity.isCirculationSettled()) return;

  // Arbitrage B (bug-126) : chemin nominal = sonde eau identifiée, offset de
  // calibration BORNÉ appliqué (bug-114). Repli de TOLÉRANCE uniquement si
  // aucun rôle n'est encore attribué (appareil neuf) — JAMAIS
  // sensors.getTemperature(), qui retomberait sur une sonde Circuit (mesure
  // du boîtier électronique, pas de l'eau).
  float referenceTemp = sensors.getWaterTemperature();
  if (isnan(referenceTemp)) {
    // Repli NON calibré (getSondeTempRaw, pas getWaterTemperature) : cohérent
    // avec l'arbitrage — aucun rôle n'est encore attribué, donc aucun offset
    // de calibration ne s'applique à cette sonde.
    uint8_t roles[kMaxDs18b20Sondes];
    float temps[kMaxDs18b20Sondes];
    for (uint8_t i = 0; i < kMaxDs18b20Sondes; i++) {
      roles[i] = static_cast<uint8_t>(sensors.getSondeRole(i));
      temps[i] = sensors.getSondeTempRaw(i);
    }
    if (!selectFiltrationRefTemp(roles, temps, kMaxDs18b20Sondes, referenceTemp)) {
      return;  // Pas de température exploitable : on conserve le planning actuel.
    }
  }

  // Une sonde figée ne doit pas produire de nouveau planning (ADR-0074) :
  // la valeur est constante, la « recalculer » ne ferait qu'user une
  // écriture NVS pour un résultat identique.
  if (sensors.isTemperatureFrozen()) return;

  // P2-A (revue) : bornes ASYMÉTRIQUES, comme resolveReferenceTemp. Sous
  // kFiltrationRefTempMinC = hivernage NORMAL (constants.h le dit lui-même),
  // PAS une panne de sonde : `info`, planning conservé, AUCUNE alerte. Un
  // `critical` + alerte quotidienne pendant tout l'hiver apprendrait à
  // l'utilisateur à ignorer le canal.
  if (referenceTemp < kFiltrationRefTempMinC) {
    systemLogger.info("[Filtration] Hivernage (" + String(referenceTemp, 1) +
                       "°C) : planning conservé");
    return;
  }
  if (!isPlausibleReferenceTemp(referenceTemp, kFiltrationRefTempMinC, kFiltrationRefTempMaxC)) {
    // B2 (revue) : le message MQTT tient sous ~53 o UTF-8 (budget réel du
    // transport, mqtt_manager.cpp:752-768), valeur numérique EN TÊTE — c'est
    // la seule info actionnable. Le texte long reste intégral dans le
    // systemLogger.critical() ci-dessus.
    systemLogger.critical("[Filtration] CRITIQUE — température de référence implausible (" +
                           String(referenceTemp, 1) + "°C) : planning conservé");
    mqttManager.publishAlert("filtration_temp_implausible",
        "Réf. " + String(referenceTemp, 1) + "°C hors bornes : planning conservé");
    return;
  }

  // Calcul du créneau (logique pure) puis formatage en "HH:MM" pour la config.
  ScheduleWindow w = computeAutoWindow(referenceTemp, kFiltrationPivotHour);
  int curStart = ::timeStringToMinutes(filtrationCfg.start.c_str());
  int curEnd = ::timeStringToMinutes(filtrationCfg.end.c_str());
  if (w.startMin == curStart && w.endMin == curEnd) return;  // fenêtre inchangée : rien à écrire

  filtrationCfg.start = minutesToTimeString(w.startMin);
  filtrationCfg.end = minutesToTimeString(w.endMin);
  ensureTimesValid();
  systemLogger.info("Planning auto: " + String(referenceTemp, 1) + "°C → " + filtrationCfg.start + "-" + filtrationCfg.end);
}

bool FiltrationManager::getCurrentMinutesOfDay(int& minutes) {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, 0)) return false;
  minutes = timeinfo.tm_hour * 60 + timeinfo.tm_min;
  return true;
}

int FiltrationManager::timeStringToMinutes(const String& value) {
  // Délègue à la fonction pure (schedule_logic).
  return ::timeStringToMinutes(value.c_str());
}

bool FiltrationManager::isMinutesInRange(int now, int start, int end) {
  // Délègue à la fonction pure (schedule_logic).
  return ::isMinutesInRange(now, start, end);
}

// feature-086 : durée PROGRAMMÉE de filtration du jour (minutes), −1 = indisponible.
// Le prédicat de mode est ALIGNÉ sur decideFiltrationRun (schedule_logic.cpp :
// `strcmp(mode, "manual") == 0 || strcmp(mode, "auto") == 0`) : dans ce projet,
// les modes `auto` ET `manual` suivent tous deux la plage start/end — `manual`
// signifie « horaires fixés par l'utilisateur », pas « sans planning ». Toute
// évolution de decideFiltrationRun doit être répercutée ici, sinon le maximum
// journalier annoncé divergerait silencieusement de la filtration réelle.
// `off`/`force` (hors planning) et plage invalide → −1 : rien n'est inventé.
// NUANCE ASSUMÉE : forceOn/forceOff/boost peuvent faire tourner la filtration
// HORS plage ; le maximum journalier reste une capacité ANNONCÉE fondée sur le
// planning (capacité stable), pas un décompte des minutes réelles.
// Le calcul délègue au parseur PUR ::timeStringToMinutes (schedule_logic) — la
// méthode privée homonyme n'est pas const, on appelle la fonction libre.
// La plage [start, end) est un intervalle fermé à gauche, ouvert à droite,
// même sémantique qu'isMinutesInRange (schedule_logic.cpp) — sa durée est
// exactement end − start (modulo minuit).
int FiltrationManager::scheduledMinutesToday() const {
  // Comparaison EXACTE (strcmp), comme decideFiltrationRun : un mode « Auto »
  // avec majuscule n'y suit pas le planning, il ne doit pas en annoncer un ici.
  const char* mode = filtrationCfg.mode.c_str();
  if (strcmp(mode, "auto") != 0 && strcmp(mode, "manual") != 0) return -1;
  const int startMin = ::timeStringToMinutes(filtrationCfg.start.c_str());
  const int endMin = ::timeStringToMinutes(filtrationCfg.end.c_str());
  // Plage illisible ou invalide (start == end = plage nulle côté filtration,
  // cf. isMinutesInRange) : rien à annoncer.
  if (startMin < 0 || endMin < 0 || startMin == endMin) return -1;
  if (startMin < endMin) return endMin - startMin;      // plage simple
  return (1440 - startMin) + endMin;                    // à cheval sur minuit
}

void FiltrationManager::update() {
  // bug-126 (P2-C, revue) : publie au premier tour où MQTT est CONNECTÉ
  // l'alerte différée par ensureTimesValid() (le critical du journal, lui,
  // est déjà parti au boot). Placé AVANT le gate d'installMode : la
  // réparation de plage nulle ne dépend pas du mode d'installation.
  if (_scheduleRepairAlertPending && mqttManager.isConnected()) {
    mqttManager.publishAlert("filtration_schedule_invalid", "Plage auto nulle : repli 08:00-20:00");
    _scheduleRepairAlertPending = false;
  }

  // feature-056 : le relais filtration n'est piloté QUE dans le mode
  // « PoolController pilote la filtration » (ManagedFiltration). Dans les modes
  // PoweredByFiltration / ExternalFiltration, le relais reste INERTE (jamais
  // énergisé) et tout forçage (Boost / UI / MQTT) est SANS EFFET — signalé par un
  // warning throttlé. Le timer de stabilisation lié à la filtration est de facto
  // désactivé (le bloc d'arming ci-dessous n'est jamais atteint).
  if (mqttCfg.installMode != InstallMode::ManagedFiltration) {
    if (filtrationCfg.forceOn || filtrationCfg.forceOff || isBoostActive(time(nullptr))) {
      static unsigned long sLastForceWarnMs = 0;
      unsigned long nowMs = millis();
      if (sLastForceWarnMs == 0 || nowMs - sLastForceWarnMs >= kMutexTimeoutWarnThrottleMs) {
        systemLogger.warning("[Filtration] Forçage ignoré : relais non piloté dans ce mode d'installation");
        sLastForceWarnMs = nowMs;
      }
    }
    if (state.running || relayState) {
      state.running = false;
      state.startedAtMs = 0;
      if (relayState) {
        digitalWrite(kFiltrationRelayPin, LOW);
        relayState = false;
        publishState();
      }
    }
    return;
  }

  // Expiration des forcages après 4 heures (sécurité si l'utilisateur oublie de désactiver)
  constexpr unsigned long kForceTimeoutMs = 4UL * 3600000UL;

  if (filtrationCfg.forceOn) {
    if (state.forceOnStartMs == 0) state.forceOnStartMs = millis();
    if (millis() - state.forceOnStartMs >= kForceTimeoutMs) {
      filtrationCfg.forceOn = false;
      state.forceOnStartMs = 0;
      systemLogger.warning("ForceOn expiré (timeout 4h), retour au mode normal");
    }
  } else {
    state.forceOnStartMs = 0;
  }

  if (filtrationCfg.forceOff) {
    if (state.forceOffStartMs == 0) state.forceOffStartMs = millis();
    if (millis() - state.forceOffStartMs >= kForceTimeoutMs) {
      filtrationCfg.forceOff = false;
      state.forceOffStartMs = 0;
      systemLogger.warning("ForceOff expiré (timeout 4h), retour au mode normal");
    }
  } else {
    state.forceOffStartMs = 0;
  }

  ensureTimesValid();
  int nowMinutes = 0;
  bool haveTime = getCurrentMinutesOfDay(nowMinutes);

  // bug-126 (A3) — la promotion du planning en attente se fait EN TÊTE, avant
  // toute lecture de filtrationCfg.start/end pour la décision du tour : le
  // tour entier doit lire UN SEUL planning (le promu, s'il y en a un).
  promotePendingScheduleIfDue(haveTime);

  String mode = filtrationCfg.mode;
  mode.toLowerCase();

  int startMin = timeStringToMinutes(filtrationCfg.start);
  int endMin = timeStringToMinutes(filtrationCfg.end);

  // Décision marche/arrêt (logique pure). `mode` est déjà en minuscules.
  // feature-053 : le Mode Boost force la filtration (turnover maximal) via un
  // chemin DÉDIÉ, prioritaire et indépendant du forceOn/kForceTimeoutMs.
  bool boostForce = isBoostActive(time(nullptr));
  bool runTarget = decideFiltrationRun(boostForce, mode.c_str(), filtrationCfg.forceOn,
                                       filtrationCfg.forceOff, haveTime,
                                       nowMinutes, startMin, endMin,
                                       state.running);

  bool wasRunning = state.running;
  if (!wasRunning && runTarget) {
    state.running = true;
    state.startedAtMs = millis();
    systemLogger.info("Démarrage filtration");
    // feature-091 (v2.34.0) : l'arming du timer de stabilisation au démarrage de
    // la filtration est RETIRÉ. La stabilisation après reprise de circulation est
    // portée par la purge adaptative de measureValidity (front montant de
    // présence d'eau → plancher kPurgeFloorMs + stabilité de la mesure) ; un
    // arrêt/redémarrage fugace y est traité par la logique pure, pas ici.
    // bug-126 (A4) — nouveau cycle : l'accumulateur de température repart de
    // zéro, la cadence d'échantillonnage aussi (wrap-safe : 0 = "jamais
    // échantillonné", cf. sampleCycleTemperature).
    resetCycleTempAccumulator(_cycleTemp);
    _lastTempSampleMs = 0;
  }

  bool runNow = state.running;

  // bug-126 (A4) — PLUS AUCUN recalcul du planning PENDANT la marche (c'était
  // le défaut : une fenêtre rétractée par une température en baisse pouvait
  // arrêter le cycle qui vient de la produire). On se contente d'accumuler un
  // échantillon de température ; le planning ne se fige qu'à l'arrêt
  // (writePendingSchedule) et ne s'applique qu'à minuit (promotePendingScheduleIfDue).
  if (runNow) {
    sampleCycleTemperature(millis(), haveTime, nowMinutes, startMin, endMin);
  }

  if (runNow && !runTarget) {
    state.running = false;
    state.startedAtMs = 0;
    systemLogger.info("Arrêt filtration");
    state.lastStoppedAtMs = millis();
    // bug-126 (A2/A5) — fige la moyenne du cycle qui vient de se terminer
    // dans le planning EN ATTENTE (jamais dans filtrationCfg directement :
    // écrire une fin postérieure à l'heure d'arrêt relancerait la pompe au
    // tour suivant, decideFiltrationRun étant une décision de NIVEAU).
    writePendingSchedule(haveTime);
    PumpController.clearStabilizationTimer();
  } else {
    state.running = runTarget;
  }

  bool relayShouldBeOn = state.running && (filtrationCfg.mode != "off");
  if (relayShouldBeOn != relayState) {
    digitalWrite(kFiltrationRelayPin, relayShouldBeOn ? HIGH : LOW);
    relayState = relayShouldBeOn;
    publishState();
  }
}

void FiltrationManager::publishState() {
  mqttManager.publishFiltrationState();
}

// =============================================================================
// bug-126 — planning auto calculé à l'ARRÊT, promu à minuit
// =============================================================================

// Échantillonne la température toutes les kFiltrationTempSampleIntervalMs
// (cadence wrap-safe, patron pump_controller.cpp:2301 — JAMAIS timeinfo.tm_min :
// un saut NTP ou le changement d'heure d'été duplique ou saute des échantillons).
void FiltrationManager::sampleCycleTemperature(uint32_t nowMs, bool haveTime, int nowMin,
                                                int startMin, int endMin) {
  if (_lastTempSampleMs != 0 &&
      (uint32_t)(nowMs - _lastTempSampleMs) < kFiltrationTempSampleIntervalMs) {
    return;
  }
  _lastTempSampleMs = nowMs;

  // C2 : les TROIS conditions sont cumulatives (voir schedule_logic.h) — purge
  // (eau renouvelée), en plage (ferme un forceOn nocturne), sonde vivante.
  bool inSchedule = haveTime && isMinutesInRange(nowMin, startMin, endMin);
  bool eligible = isSampleEligible(measureValidity.isCirculationSettled(), inSchedule,
                                    sensors.isTemperatureFrozen());

  // Arbitrage B (bug-126) : chemin nominal = sonde eau, repli de TOLÉRANCE
  // (appareil neuf) — JAMAIS sensors.getTemperature() (retomberait sur la
  // sonde Circuit = température du boîtier électronique).
  float sampleC = sensors.getWaterTemperature();
  if (isnan(sampleC)) {
    // Repli NON calibré (getSondeTempRaw) : aucun rôle attribué, donc aucun
    // offset de calibration à appliquer (cohérent avec computeAutoSchedule).
    uint8_t roles[kMaxDs18b20Sondes];
    float temps[kMaxDs18b20Sondes];
    for (uint8_t i = 0; i < kMaxDs18b20Sondes; i++) {
      roles[i] = static_cast<uint8_t>(sensors.getSondeRole(i));
      temps[i] = sensors.getSondeTempRaw(i);
    }
    if (!selectFiltrationRefTemp(roles, temps, kMaxDs18b20Sondes, sampleC)) {
      sampleC = NAN;
    }
  }

  accumulateCycleTemp(_cycleTemp, sampleC, eligible);
}

// Fige la moyenne du cycle qui vient de se terminer dans le planning EN
// ATTENTE — n'écrit JAMAIS filtrationCfg (seule promotePendingScheduleIfDue
// le fait, à minuit, pompe à l'arrêt).
void FiltrationManager::writePendingSchedule(bool haveTime) {
  // Corrections mineures (revue P2) : hors mode auto, chaque cycle produirait
  // un pending que promotePendingScheduleIfDue() jetterait ensuite
  // (NotAuto) — bruit de journal pur, et une raison d'armer
  // _haveRepresentativeMeasure à tort (le drapeau ne doit suivre que des
  // mesures faites EN mode auto).
  if (!filtrationCfg.mode.equalsIgnoreCase("auto")) return;

  // M3 (revue) : `computedOnDate` vient de time()/localtime_r plus bas. Sans
  // heure valide on obtiendrait "19700101" ; shouldRolloverByDate lirait ça
  // comme "jour différent" dès le premier passage à une date réelle ->
  // promotion IMMÉDIATE en pleine journée au lieu de minuit. Aujourd'hui ce
  // cas est INATTEIGNABLE (isSampleEligible exige déjà haveTime via
  // `inSchedule` : sans heure, aucun échantillon n'est retenu -> NoSample
  // plus bas) — mais ce couplage vit à trois fichiers de distance ; on le
  // rend explicite ici plutôt que de compter sur son effet de bord.
  if (!haveTime) {
    systemLogger.info("[Filtration] Planning en attente non écrit : heure indisponible");
    return;
  }

  time_t nowEpoch = time(nullptr);
  struct tm ti;
  localtime_r(&nowEpoch, &ti);
  char todayStr[9];
  strftime(todayStr, sizeof(todayStr), "%Y%m%d", &ti);

  // M4 (revue, trouvé indépendamment par pool-chemistry et code-reviewer) :
  // un pending existant DÉJÀ DÛ (computedOnDate antérieur à aujourd'hui) a
  // été DIFFÉRÉ par promotePendingScheduleIfDue() (PumpRunning, en tête de CE
  // MÊME tour) parce que la pompe tournait encore à minuit. Si on le laissait
  // écraser ici par la mesure du cycle qui vient de finir, son
  // computedOnDate redeviendrait "aujourd'hui" -> newDay redevient faux au
  // tour suivant -> la promotion différée n'aurait JAMAIS lieu, alors que le
  // journal a annoncé « différée ». NE PAS corriger en rappelant
  // promotePendingScheduleIfDue() ici : promouvoir une fenêtre qui contient
  // l'heure courante ferait redémarrer la pompe au tour suivant
  // (decideFiltrationRun est une décision de NIVEAU) — on rouvrirait un cycle
  // court juste après un arrêt.
  if (_pending.valid && shouldRolloverByDate(_pending.computedOnDate, todayStr)) {
    systemLogger.info("[Filtration] Planning en attente conservé : promotion déjà due (différée), non écrasé");
    return;
  }

  float t = NAN;
  RefTempVerdict verdict = resolveReferenceTemp(
      _cycleTemp, kFiltrationMinRepresentativeSamples,
      /*bootstrapAllowed=*/!_haveRepresentativeMeasure,
      kFiltrationRefTempMinC, kFiltrationRefTempMaxC, t);

  switch (verdict) {
    case RefTempVerdict::Ok: {
      // B1 (BLOQUANT, revue) : le drapeau appartient à la MESURE, pas à
      // l'ÉCRITURE — armé ICI, AVANT le test de fenêtre identique. En régime
      // établi (même fenêtre chaque jour), le retour "fenêtre inchangée"
      // ci-dessous est le chemin NORMAL : si l'armement vivait après lui, le
      // drapeau resterait `false` indéfiniment, bootstrapAllowed resterait
      // `true`, et le premier cycle produisant une fenêtre DIFFÉRENTE
      // pourrait le faire sur un seul échantillon (cycle tronqué à 12 min par
      // un forceOff) — le critère d'acceptation « un cycle écourté ne pilote
      // pas le lendemain » tombait.
      _haveRepresentativeMeasure = true;

      ScheduleWindow w = computeAutoWindow(t, kFiltrationPivotHour);
      int curStart = ::timeStringToMinutes(filtrationCfg.start.c_str());
      int curEnd = ::timeStringToMinutes(filtrationCfg.end.c_str());
      if (w.startMin == curStart && w.endMin == curEnd) {
        // Remplaçant de l'ancienne bande morte sur _lastScheduledTemp
        // (ADR-0074) : une fenêtre identique ne produit AUCUN pending, donc
        // aucune écriture NVS à la promotion.
        systemLogger.info("[Filtration] Planning en attente non écrit : fenêtre inchangée");
        return;
      }
      _pending.valid = true;
      _pending.startMin = w.startMin;
      _pending.endMin = w.endMin;
      _pending.refTempC = t;
      _pending.sampleCount = _cycleTemp.count;
      _pending.refMinC = _cycleTemp.minC;
      _pending.refMaxC = _cycleTemp.maxC;
      strlcpy(_pending.computedOnDate, todayStr, sizeof(_pending.computedOnDate));
      // Instantané de BASE (arbitrage A) : toute divergence à la promotion
      // jette le pending. strlcpy (pas strncpy, revue P2) : la terminaison
      // NUL ne dépend plus d'un invariant non écrit sur la longueur source.
      strlcpy(_pending.basisStart, filtrationCfg.start.c_str(), sizeof(_pending.basisStart));
      strlcpy(_pending.basisEnd, filtrationCfg.end.c_str(), sizeof(_pending.basisEnd));
      strlcpy(_pending.basisMode, filtrationCfg.mode.c_str(), sizeof(_pending.basisMode));
      systemLogger.info("[Filtration] Planning en attente : " + String(t, 1) + "°C (moy. " +
                         String(_cycleTemp.count) + " éch. en plage, min " +
                         String(_cycleTemp.minC, 1) + " max " + String(_cycleTemp.maxC, 1) +
                         ") → " + minutesToTimeString(w.startMin) + "-" +
                         minutesToTimeString(w.endMin));
      break;
    }
    case RefTempVerdict::TooCold:
      // P2-A (BLOQUANT, revue) : sous kFiltrationRefTempMinC = hivernage
      // NORMAL, PAS une panne de sonde — `info` seulement, AUCUNE alerte.
      // outTempC n'est écrit QUE sur Ok (ADR-0072) : on journalise la
      // moyenne brute de l'accumulateur, pas `t` (resté NaN).
      //
      // VARIANTE SAISONNIÈRE DE B1 (revue, suggestion retenue) : un cycle
      // hivernal assez long est une mesure REPRÉSENTATIVE — elle dit juste que
      // l'eau est froide. Si le drapeau n'était pas armé ici, un appareil mis en
      // service l'hiver ne l'armerait JAMAIS (bootstrapAllowed reste vrai, donc
      // TooShort est sauté, donc on atterrit ici même à 1 échantillon), et le
      // premier cycle de printemps pourrait fixer le planning sur un seul
      // échantillon. On arme donc sur le COMPTE, jamais sur le verdict.
      // Implausible n'arme pas, lui : cette mesure-là n'est pas digne de foi.
      if (_cycleTemp.count >= kFiltrationMinRepresentativeSamples) {
        _haveRepresentativeMeasure = true;
      }
      systemLogger.info("[Filtration] Planning en attente non écrit : hivernage (" +
                         String(_cycleTemp.mean, 1) + "°C)");
      break;
    case RefTempVerdict::Implausible:
      // B2 (BLOQUANT, revue) : message MQTT sous ~53 o UTF-8 (budget réel du
      // transport), valeur numérique EN TÊTE. Le texte long reste intégral
      // dans systemLogger.critical() — patron de la maison (dérogation
      // assumée au "pas de double log", C8 pool-chemistry : le silence est
      // ce qui rendait le cas 48°C dangereux).
      systemLogger.critical("[Filtration] CRITIQUE — température de référence implausible (" +
                             String(_cycleTemp.mean, 1) + "°C) : planning conservé");
      mqttManager.publishAlert("filtration_temp_implausible",
          "Réf. " + String(_cycleTemp.mean, 1) + "°C hors bornes : planning conservé");
      break;
    case RefTempVerdict::NoSample:
      systemLogger.info("[Filtration] Planning en attente non écrit : aucun échantillon");
      break;
    case RefTempVerdict::TooShort:
      systemLogger.info("[Filtration] Planning en attente non écrit : cycle trop court (" +
                         String(_cycleTemp.count) + " éch. < " +
                         String(kFiltrationMinRepresentativeSamples) + ")");
      break;
  }
}

// En TÊTE d'update() : bascule le planning en attente vers le planning
// courant si les conditions de promotion sont réunies (schedule_logic).
void FiltrationManager::promotePendingScheduleIfDue(bool haveTime) {
  if (!_pending.valid) return;  // évite un strftime inutile à chaque tour

  char todayStr[9] = {};
  bool newDay = false;
  if (haveTime) {
    time_t nowEpoch = time(nullptr);
    struct tm ti;
    localtime_r(&nowEpoch, &ti);
    strftime(todayStr, sizeof(todayStr), "%Y%m%d", &ti);
    newDay = shouldRolloverByDate(_pending.computedOnDate, todayStr);
  }
  bool alreadyPromotedToday = haveTime && strcmp(_lastPromotedDay, todayStr) == 0;
  bool basisUnchanged = (filtrationCfg.start == _pending.basisStart) &&
                        (filtrationCfg.end == _pending.basisEnd) &&
                        (filtrationCfg.mode == _pending.basisMode);
  bool modeIsAuto = filtrationCfg.mode.equalsIgnoreCase("auto");

  PromotionVerdict verdict = decideSchedulePromotion(_pending.valid, newDay, alreadyPromotedToday,
                                                     basisUnchanged, modeIsAuto, state.running);
  switch (verdict) {
    case PromotionVerdict::Promote: {
      // M1 (BLOQUANT, revue) : update() tourne dans DEUX contextes —
      // loopTask (main.cpp, SANS mutex) et web_routes_config.cpp (tâche
      // async_tcp, SOUS configMutex, pris en handleSaveConfig() avant
      // d'appeler filtration.update()). Sans prise ICI, la réaffectation de
      // filtrationCfg.start/end (String) pendant qu'un handler HTTP lit
      // filtrationCfg.start.c_str() est la même forme d'UAF que le bug
      // refreshTopics déjà corrigé dans ce dépôt — et l'écriture RAM +
      // saveMqttConfig() (NVS) ne seraient pas atomiques entre elles. Mutex
      // RÉCURSIF : saveMqttConfig() s'y imbrique sans changement, et l'appel
      // depuis web_routes_config.cpp (déjà sous configMutex) ré-entre sans
      // bloquer.
      if (xSemaphoreTakeRecursive(configMutex, pdMS_TO_TICKS(kConfigMutexTimeoutMs)) != pdTRUE) {
        // Throttle OBLIGATOIRE (revue R1) : l'état qui provoque l'échec persiste
        // par construction — newDay && !alreadyPromoted && basisUnchanged && auto
        // && !running reste vrai au tour suivant. Sans throttle, on journalise à
        // CHAQUE tour de loop() pendant toute la détention du mutex (handleSaveConfig
        // le tient de web_routes_config.cpp:444 à :1192, applyTimeConfig() comprise),
        // ce qui évince le tampon circulaire du journal au moment précis où il sert.
        static unsigned long sLastPromoteMutexWarnMs = 0;
        unsigned long nowMs = millis();
        if (sLastPromoteMutexWarnMs == 0 ||
            nowMs - sLastPromoteMutexWarnMs >= kMutexTimeoutWarnThrottleMs) {
          systemLogger.warning("[Filtration] Timeout configMutex — promotion reportée au tour suivant");
          sLastPromoteMutexWarnMs = nowMs;
        }
        break;  // pending CONSERVÉ, retenté au tour suivant (fail-safe)
      }
      filtrationCfg.start = minutesToTimeString(_pending.startMin);
      filtrationCfg.end = minutesToTimeString(_pending.endMin);
      _pending.valid = false;
      strlcpy(_lastPromotedDay, todayStr, sizeof(_lastPromotedDay));
      saveMqttConfig();
      // Capture DANS la section critique (revue) : relire filtrationCfg après le
      // Give ferait journaliser la valeur d'une écriture concurrente, donc une
      // AUTRE fenêtre que celle réellement promue — un journal qui ment sur ce
      // qu'il vient de faire est pire que pas de journal.
      String promuStart = filtrationCfg.start;
      String promuEnd = filtrationCfg.end;
      xSemaphoreGiveRecursive(configMutex);
      systemLogger.info("[Filtration] Planning promu : " + promuStart + "-" +
                         promuEnd + " (calculé le " + String(_pending.computedOnDate) + ")");
      publishState();
      break;
    }
    case PromotionVerdict::PumpRunning: {
      // Différée, JAMAIS sautée : sans ce throttle, le journal recevrait
      // cette ligne à chaque tour de loop tant que la pompe tourne (heures,
      // scénario forceOn manuel à cheval sur minuit).
      unsigned long nowMs = millis();
      if (_lastPendingDeferredLogMs == 0 ||
          nowMs - _lastPendingDeferredLogMs >= kMutexTimeoutWarnThrottleMs) {
        systemLogger.info("[Filtration] Planning en attente : promotion différée (pompe en marche)");
        _lastPendingDeferredLogMs = nowMs;
      }
      break;
    }
    case PromotionVerdict::BasisChanged:
      _pending.valid = false;
      systemLogger.info("[Filtration] Planning en attente jeté : les horaires ont été modifiés depuis son calcul");
      break;
    case PromotionVerdict::NotAuto:
      _pending.valid = false;
      systemLogger.info("[Filtration] Planning en attente jeté : mode non auto");
      break;
    case PromotionVerdict::NotNewDay:
    case PromotionVerdict::AlreadyPromoted:
    case PromotionVerdict::NoPending:
    default:
      break;  // silencieux (table de mapping bug-126)
  }
}

// =============================================================================
// feature-056 — état de la filtration externe (mode ExternalFiltration)
// =============================================================================

void FiltrationManager::setExternalState(bool running) {
  // Horodatage AVANT la section critique ; log APRÈS (section critique minimale).
  uint32_t now = (uint32_t)millis();
  portENTER_CRITICAL(&_externalMux);
  _externalOn = running;
  _externalLastMs = now;
  _externalKnown = true;
  portEXIT_CRITICAL(&_externalMux);
  systemLogger.info(String("[Filtration externe] État signalé : ") + (running ? "ON" : "OFF"));
}

void FiltrationManager::getExternalState(bool& on, uint32_t& lastMs, bool& known) const {
  portENTER_CRITICAL(&_externalMux);
  on = _externalOn;
  lastMs = _externalLastMs;
  known = _externalKnown;
  portEXIT_CRITICAL(&_externalMux);
}

WaterPresence FiltrationManager::resolveWaterPresence() const {
  WaterPresenceInputs in = {};
  in.mode = mqttCfg.installMode;
  in.filtrationCommandedOn = state.running;
  // Copie minimale du triplet externe sous lock ; âge calculé HORS lock,
  // wrap-safe (soustraction non signée, cohérente au passage 0xFFFFFFFF).
  bool on = false, known = false;
  uint32_t lastMs = 0;
  getExternalState(on, lastMs, known);
  uint32_t now = (uint32_t)millis();
  in.externalSignalOn = on;
  in.externalSignalKnown = known;
  in.externalSignalAgeMs = known ? (now - lastMs) : 0;
  in.externalStaleMs = kExternalFiltrationStaleMs;
  return resolveWaterPresent(in);
}
