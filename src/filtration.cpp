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
#include "filtration_resume_logic.h"  // feature-158 : reprise de la durée de filtration
#include <time.h>

// =============================================================================
// feature-158 — mémoire RTC de la durée de filtration (patron OutageRtcRecord,
// measure_validity.cpp) : survit à un reset logiciel/panic/watchdog, PAS à une
// coupure secteur (RAM RTC aléatoire ⇒ magic/CRC faux ⇒ fail-closed, cf.
// filtrationRtcIntact).
//
// ⚠️ LA FRONTIÈRE (verdict pool-chemistry, feature-158), à répéter ici :
// `measure_validity` est le PRODUCTEUR UNIQUE du verdict de continuité de
// l'eau. Ce fichier (filtration.*) en est un CONSOMMATEUR : il persiste une
// DONNÉE (les secondes mesurées ci-dessous), jamais un verdict, et n'en
// recalcule aucun — toute la décision « ce report est-il recevable ? » vit
// dans filtration_resume_logic.cpp, hors de ce fichier.
// =============================================================================
namespace {
RTC_NOINIT_ATTR FiltrationRtcRecord sFiltrationRtc;

// Cadence de rafraîchissement de la mémoire RTC pendant la marche (même ordre
// de grandeur que kOutageRtcRefreshMs, measure_validity.cpp : pas un seuil de
// continuité, une simple cadence d'écriture).
constexpr unsigned long kFiltrationRtcRefreshMs = 1000UL;
}  // namespace

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

  // feature-146 : migration "force" → Marche. Avant ce lot, ce jeton tombait
  // dans le `return false` par défaut de l'ancien decideFiltrationRun (pompe
  // ARRÊTÉE, un bug) ; il désigne désormais le mode Marche (pompe TOURNANTE)
  // — changement de comportement réel au boot, à signaler. outQueue n'existe
  // pas encore ici (avant mqttManager.begin()) : l'alerte MQTT est différée,
  // publiée au premier tour d'update() où MQTT est connecté (même patron que
  // _scheduleRepairAlertPending ci-dessous).
  FiltrationMode bootMode = parseFiltrationModeFrom(filtrationCfg.mode);
  if (bootMode == FiltrationMode::Marche) {
    systemLogger.warning("[Filtration] Mode 'force' au démarrage : filtration désormais ACTIVE (mode Marche)");
    _forceModeMigrationAlertPending = true;
  }

  // Calculer et appliquer l'état initial du relais immédiatement.
  // Pré-requis satisfaits dans main.cpp : config et heure RTC chargées avant cet appel.
  // Cela évite une coupure prolongée de la pompe de filtration au reboot :
  // sans ce calcul, le relais resterait LOW pendant tout le setup (WiFi inclus, jusqu'à ~18s)
  // avant que filtration.update() dans loop() ne le remette dans son état correct.
  // feature-056 : le relais n'est piloté que dans le mode ManagedFiltration.
  // feature-146 : la décision du boot délègue à decideFiltrationRun (logique
  // PURE, ADR-0017) — le cas "force" qui laissait la pompe arrêtée est
  // désormais inatteignable. Créneau de BASE (le Boost n'est jamais considéré
  // au tout premier tour : hors périmètre de ce lot, comportement inchangé).
  bool initialState = false;
  if (mqttCfg.installMode == InstallMode::ManagedFiltration) {
    int nowMinutes = 0;
    bool haveTime = getCurrentMinutesOfDay(nowMinutes);
    int startMin = timeStringToMinutes(filtrationCfg.start);
    int endMin = timeStringToMinutes(filtrationCfg.end);
    initialState = decideFiltrationRun(bootMode, haveTime, nowMinutes, startMin, endMin,
                                        /*currentlyRunning=*/false);
  }

  digitalWrite(kFiltrationRelayPin, initialState ? HIGH : LOW);
  relayState = initialState;
  // ⚠️ CONDITION 4 DU GATE — ORDRE SIGNIFICATIF, TROISIÈME SITE.
  // `running` reste FAUX tant que le couple (carryMs, startedAtMs) n'est pas
  // posé : un lecteur de filtrationElapsedMs() qui verrait `running == true`
  // avec `startedAtMs == 0` obtiendrait `millis() - 0`, c'est-à-dire L'UPTIME —
  // une SUR-ESTIMATION, la seule direction que le gate interdit (« sous-estimer
  // est sans danger, sur-estimer lève une garde de dosage »).
  // ⚠️ Ce défaut EXACT a déjà été relevé en revue sur le site voisin
  // (feature-155, démarrage dans update()). Il avait été corrigé là-bas et
  // reproduit ici : les TROIS sites suivent désormais le même ordre. La fenêtre
  // était inatteignable aujourd'hui — aucune tâche ni serveur web n'existe
  // encore à cet instant de setup() — mais s'en remettre à l'ordre de setup()
  // est exactement ce que la condition 13 refuse de laisser silencieux.
  state.running = false;
  // feature-158 (A1) : le journal ne prétend plus « maintenue active » avant
  // d'avoir regardé si une reprise s'applique — la ligne ci-dessous reste
  // neutre, la décision de continuité est journalisée séparément juste après.
  systemLogger.info(initialState ? "Gestionnaire de filtration initialisé (filtration active)"
                                  : "Gestionnaire de filtration initialisé");

  if (initialState) {
    // =========================================================================
    // feature-158 — consommation UNIQUE du verdict de continuité de l'eau
    // (condition 3 du gate pool-chemistry) : ICI, et nulle part ailleurs. Un
    // démarrage ultérieur du même uptime (Boost compris) ne repasse jamais
    // par begin() — il ne peut donc jamais consommer le report une seconde
    // fois (par construction, pas par un garde-fou ajouté).
    // =========================================================================
    OutageContinuityVerdict verdict;
    const bool verdictReady = measureValidity.bootContinuityVerdict(verdict);

    FiltrationResumeInputs resumeIn;
    resumeIn.record = filtrationRtcIntact(sFiltrationRtc) ? &sFiltrationRtc : nullptr;
    resumeIn.verdict = verdictReady ? &verdict : nullptr;
    resumeIn.installMode = mqttCfg.installMode;
    resumeIn.initialState = true;
    const FiltrationResumeResult resume = evaluateFiltrationResume(resumeIn);

    // Condition 13 du gate — l'ordre d'initialisation ne doit pas devenir une
    // dépendance silencieuse : si measureValidity.begin() n'a pas encore
    // tourné (setup() réordonné par erreur), on le dit BRUYAMMENT plutôt que
    // de laisser filtrationRunning paraître neuf par hasard.
    if (!verdictReady) {
      systemLogger.critical("[Filtration] Ordre d'initialisation invalide : measureValidity.begin() "
                             "n'a pas encore produit son verdict de continuité — reprise refusée (0 s recrédités)");
    }

    state.carryMs = resume.carryS * 1000UL;
    state.startedAtMs = millis();
    // A8 (gate) : protège la sentinelle "arrêt" (startedAtMs == 0, filtration.h)
    // — si l'ancre tombe PILE sur 0, on force 1 ms (sous-estimation sans danger).
    if (state.startedAtMs == 0) state.startedAtMs = 1;
    // Condition 4 : le couple est posé, SEULEMENT MAINTENANT on annonce la
    // marche. Ne jamais remonter cette ligne au-dessus des deux précédentes.
    state.running = true;

    if (resume.motif == FiltrationResumeMotif::kResumed) {
      // Condition 9 du gate — IMPOSÉ, les DEUX nombres à chaque fois.
      systemLogger.info("[Filtration] Reprise après redémarrage : " + String(resume.carryS) +
                         " s de filtration recréditées (coupure " + String(resume.coupureDureeS) +
                         " s, dispensée) — garde pH de 10 min NON réarmée");
    } else {
      systemLogger.info("[Filtration] Reprise à zéro (" +
                         String(filtrationResumeMotifLabel(resume.motif)) +
                         ") — la garde pH de 10 min s'applique");
    }

    refreshFiltrationRtcRecord();
  } else {
    // Filtration à l'arrêt au boot : aucun cycle en cours à porter à travers
    // un reboot ultérieur — le record reflète cet état (invalidation).
    refreshFiltrationRtcRecord();
  }
}

// feature-158 — photographie l'état COURANT (pas une décision) dans la
// mémoire RTC scellée : carryMs/startedAtMs/running viennent d'être posés par
// begin() ou update(), ce getter se contente de les refléter. Condition 2 du
// gate : filtrationRunning et installMode sont PROPRES à la filtration — rien
// n'est déduit d'un verdict de continuité d'un autre module.
void FiltrationManager::refreshFiltrationRtcRecord() {
  sFiltrationRtc.carryS = (uint32_t)filtrationElapsedS();
  sFiltrationRtc.filtrationRunning = state.running ? 1 : 0;
  sFiltrationRtc.installMode = static_cast<uint8_t>(mqttCfg.installMode);
  filtrationRtcSeal(sFiltrationRtc);
}

void FiltrationManager::ensureTimesValid() {
  // feature-146 (revue code-reviewer) : parseFiltrationModeFrom (config.h),
  // SANS allocation — remplace l'ex-`String modeLower` recopiée.
  FiltrationMode mode = parseFiltrationModeFrom(filtrationCfg.mode);
  if (mode == FiltrationMode::Inconnu) {
    // feature-146 (condition #3 du gate) : cesse d'être silencieux — un jeton
    // non reconnu (config corrompue, client legacy) est désormais journalisé.
    systemLogger.warning("[Filtration] Mode inconnu (\"" + filtrationCfg.mode + "\") — repli sur Auto");
    filtrationCfg.mode = "auto";
    // Corrections mineures (revue P2) : `mode` ne doit pas rester périmé sur
    // cette branche — le check C7 plus bas le réutilise tel quel.
    mode = FiltrationMode::Auto;
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
  if (mode == FiltrationMode::Auto && filtrationCfg.start == filtrationCfg.end) {
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
  if (parseFiltrationModeFrom(filtrationCfg.mode) != FiltrationMode::Auto) return;

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

// feature-086 (révisé feature-146) : durée PROGRAMMÉE de filtration du jour
// (minutes), −1 = indisponible. Auto et Programme suivent tous deux la plage
// start/end (même prédicat que decideFiltrationRun, schedule_logic.cpp) —
// Programme signifie « horaires fixés par l'utilisateur », pas « sans
// planning ». Marche = 1440 (gate #4) : AFFICHAGE SEUL, la pompe tourne en
// permanence tant que l'utilisateur ne change pas de mode — ne borne AUCUN
// dosage (l'horizon de dosage réparti suit la même règle indépendamment, cf.
// pump_controller.cpp filtrationDosingHorizonMinutes). Arrêt (hors planning)
// et plage invalide → −1 : rien n'est inventé.
// Le calcul délègue au parseur PUR ::timeStringToMinutes (schedule_logic) — la
// méthode privée homonyme n'est pas const, on appelle la fonction libre.
// La plage [start, end) est un intervalle fermé à gauche, ouvert à droite,
// même sémantique qu'isMinutesInRange (schedule_logic.cpp) — sa durée est
// exactement end − start (modulo minuit).
int FiltrationManager::scheduledMinutesToday() const {
  // feature-146, gate pool-chemistry (2e passage) : le mode est minusculisé
  // AVANT d'être interprété, via parseFiltrationModeFrom (SANS allocation),
  // exactement comme begin() et update(). Le commentaire précédent affirmait
  // l'inverse « comparaison EXACTE, cf. decideFiltrationRun » : c'était FAUX,
  // cette fonction recevant déjà la forme minuscule ailleurs. L'écart faisait
  // qu'un jeton « Force » rendait -1 ici et 1440-nowMin dans
  // filtrationDosingHorizonMinutes() le temps qu'ensureTimesValid() le
  // coerce. DEUX lectures du même état avec DEUX conventions : c'est
  // précisément la classe de défaut que ce lot supprime.
  const FiltrationMode mode = parseFiltrationModeFrom(filtrationCfg.mode);
  if (mode == FiltrationMode::Marche) return 1440;
  if (mode != FiltrationMode::Auto && mode != FiltrationMode::Programme) return -1;
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
  // feature-146 (condition #6 du gate) : migration "force" → Marche détectée
  // au boot (begin()) — alerte différée, même raison que ci-dessus (outQueue
  // pas encore créée à begin()).
  if (_forceModeMigrationAlertPending && mqttManager.isConnected()) {
    mqttManager.publishAlert("filtration_mode_force_migrated",
        "Mode 'force' : filtration désormais ACTIVE (mode Marche), et non plus arrêtée");
    _forceModeMigrationAlertPending = false;
  }

  // feature-056 : le relais filtration n'est piloté QUE dans le mode
  // « PoolController pilote la filtration » (ManagedFiltration). Dans les modes
  // PoweredByFiltration / ExternalFiltration, le relais reste INERTE (jamais
  // énergisé) et tout forçage (Boost / UI / MQTT) est SANS EFFET — signalé par un
  // warning throttlé. Le timer de stabilisation lié à la filtration est de facto
  // désactivé (le bloc d'arming ci-dessous n'est jamais atteint).
  if (mqttCfg.installMode != InstallMode::ManagedFiltration) {
    if (isBoostActive(time(nullptr))) {
      static unsigned long sLastForceWarnMs = 0;
      unsigned long nowMs = millis();
      if (sLastForceWarnMs == 0 || nowMs - sLastForceWarnMs >= kMutexTimeoutWarnThrottleMs) {
        systemLogger.warning("[Filtration] Boost ignoré : relais non piloté dans ce mode d'installation");
        sLastForceWarnMs = nowMs;
      }
    }
    if (state.running || relayState) {
      // Condition 4 du gate pool-chemistry (ordres d'écriture imposés, lecture
      // déchirée en async_tcp) : running → false AVANT de remettre à zéro
      // startedAtMs/carryMs — un lecteur concurrent qui voit encore
      // running==true doit toujours lire un couple qui SOUS-estime, jamais un
      // couple incohérent.
      state.running = false;
      state.startedAtMs = 0;
      state.carryMs = 0;
      refreshFiltrationRtcRecord();  // invalidation du record (feature-158)
      if (relayState) {
        digitalWrite(kFiltrationRelayPin, LOW);
        relayState = false;
        publishState();
      }
    }
    return;
  }

  ensureTimesValid();
  int nowMinutes = 0;
  bool haveTime = getCurrentMinutesOfDay(nowMinutes);

  // bug-126 (A3) — la promotion du planning en attente se fait EN TÊTE, avant
  // toute lecture de filtrationCfg.start/end pour la décision du tour : le
  // tour entier doit lire UN SEUL planning (le promu, s'il y en a un).
  promotePendingScheduleIfDue(haveTime);

  FiltrationMode mode = parseFiltrationModeFrom(filtrationCfg.mode);
  if (mode == FiltrationMode::Inconnu) {
    // feature-146 (condition #3 du gate) : ensureTimesValid() ci-dessus vient
    // de ramener tout jeton inconnu à Auto — l'atteindre ICI est anormal
    // (course entre lecture et coercition, config corrompue entre-temps).
    // `critical` (pas `warning`) ET throttlé : un signal fort, mais pas un
    // déni de service du journal si la course se répète chaque tour.
    unsigned long nowMs = millis();
    if (_lastUnknownModeLogMs == 0 || nowMs - _lastUnknownModeLogMs >= kMutexTimeoutWarnThrottleMs) {
      systemLogger.critical("[Filtration] Mode inconnu (\"" + filtrationCfg.mode + "\") après coercition — état courant conservé");
      _lastUnknownModeLogMs = nowMs;
    }
  }

  int startMin = timeStringToMinutes(filtrationCfg.start);
  int endMin = timeStringToMinutes(filtrationCfg.end);

  // feature-146 (2′, gate pool-chemistry) : passer en mode Arrêt EFFACE le
  // Boost en cours (pas seulement « ignoré ») — sinon un retour ultérieur en
  // mode Marche/Auto/Programme réélargirait le créneau et relèverait la
  // consigne ORP/le plafond chlore tout seul (isBoostActive() est lu
  // DIRECTEMENT par dosing_logic.cpp, hors de cette décision filtration).
  time_t nowEpoch = time(nullptr);
  if (isBoostActive(nowEpoch) && mode == FiltrationMode::Arret) {
    stopBoost();
    // (revue code-reviewer #4) : sans ce publish, le topic retenu HA reste à
    // ON après l'annulation — le WebSocket resynchronise (requestConfigBroadcast
    // ailleurs), HA non, tant que personne ne rappelle boost/set.
    mqttManager.publishBoostState();
    systemLogger.warning("[Filtration] Boost annulé : passage en mode Arrêt");
  }
  bool boostActive = isBoostActive(nowEpoch);

  // Créneau EFFECTIF (Boost = nowMin..minuit) : ne sert QU'À la décision qui
  // suit. Tous les autres consommateurs (échantillonnage de température,
  // planning annoncé, horizon de dosage réparti) restent sur le créneau de
  // BASE ci-dessus (ADR-0093) — `rw` n'est utilisé nulle part ailleurs.
  RunWindow rw = effectiveRunWindow(ScheduleWindow{startMin, endMin}, boostActive, nowMinutes);

  // Décision marche/arrêt (logique pure, ADR-0017). feature-146 : le Mode
  // Boost n'est plus une priorité à part dans la décision — il MODIFIE le
  // créneau (rw ci-dessus), consulté par TOUS les modes indifféremment (sans
  // effet pratique hors Auto/Programme : Marche ignore le créneau, et le
  // Boost ne peut pas être actif en mode Arrêt, cf. startBoost()/l'annulation
  // ci-dessus).
  bool runTarget = decideFiltrationRun(mode, haveTime, nowMinutes,
                                       rw.w.startMin, rw.w.endMin,
                                       state.running);

  bool wasRunning = state.running;
  if (!wasRunning && runTarget) {
    // ⚠️ ORDRE SIGNIFICATIF (relevé en revue, feature-155) : l'horodatage est
    // publié AVANT le drapeau. Dans l'ordre inverse, une requête
    // POST /tune-ph-offset intercalée entre les deux lisait `running = true`
    // avec `startedAtMs` encore à 0 ⇒ `elapsed = millis()`, donc la garde des
    // 10 minutes FRANCHIE au moment même où la filtration démarre. Fenêtre
    // microscopique, correctif gratuit : ne jamais annoncer « ça tourne » avant
    // de pouvoir dire « depuis quand ».
    // feature-158 (condition 3 du gate) : ce démarrage-ci n'est PAS begin() —
    // la consommation du report est UNIQUE et vit exclusivement là-bas (exclut
    // le Boost et tout redémarrage forcé par construction). carryMs posé à 0
    // AVANT startedAtMs (condition 4 : ordre d'écriture imposé).
    state.carryMs = 0;
    state.startedAtMs = millis();
    if (state.startedAtMs == 0) state.startedAtMs = 1;  // A8 : sentinelle "arrêt" protégée
    state.running = true;
    refreshFiltrationRtcRecord();
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
    // feature-158 : rafraîchit la mémoire RTC (cadence ~1 s, patron
    // measure_validity.cpp) pour que les secondes mesurées du cycle courant
    // restent à jour si un reboot survient PENDANT la marche.
    unsigned long nowMs = millis();
    if (_lastFiltrationRtcRefreshMs == 0 ||
        (uint32_t)(nowMs - _lastFiltrationRtcRefreshMs) >= kFiltrationRtcRefreshMs) {
      refreshFiltrationRtcRecord();
      _lastFiltrationRtcRefreshMs = nowMs;
    }
  }

  if (runNow && !runTarget) {
    // Condition 4 du gate (même ordre que la branche Powered/External
    // ci-dessus) : running → false, PUIS startedAtMs/carryMs à zéro, PUIS
    // invalidation du record — un arrêt réel ne doit JAMAIS se reprendre.
    state.running = false;
    state.startedAtMs = 0;
    state.carryMs = 0;
    refreshFiltrationRtcRecord();  // invalidation du record (feature-158)
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

  // feature-146 (condition #1 BLOQUANTE du gate) : SOURCE UNIQUE. L'ancien
  // terme `&& (filtrationCfg.mode != "off")` lisait le mode BRUT alors que la
  // décision ci-dessus lit le mode MINUSCULISÉ, et relisait filtrationCfg
  // APRÈS la décision — une course réelle avec les callbacks web/MQTT du même
  // tour de loop. `state.running` est déjà le résultat de decideFiltrationRun
  // (mode Arrêt y renvoie false) : le second terme était redondant ET faux.
  bool relayShouldBeOn = state.running;
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
  // (eau renouvelée), en plage de BASE (ferme un mode Marche nocturne, qui ne
  // doit pas polluer la moyenne — ADR-0093 : jamais le créneau élargi par le
  // Boost ici), sonde vivante.
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
  if (parseFiltrationModeFrom(filtrationCfg.mode) != FiltrationMode::Auto) return;

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
      // un passage en mode Arrêt) — le critère d'acceptation « un cycle
      // écourté ne pilote pas le lendemain » tombait.
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
  bool modeIsAuto = parseFiltrationModeFrom(filtrationCfg.mode) == FiltrationMode::Auto;

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
      // scénario mode Marche à cheval sur minuit).
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
