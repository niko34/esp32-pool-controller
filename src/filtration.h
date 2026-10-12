#ifndef FILTRATION_H
#define FILTRATION_H

#include <Arduino.h>
#include "config.h"
#include "schedule_logic.h"
#include "filtration_resume_logic.h"

struct FiltrationRuntime {
  bool running = false;
  unsigned long startedAtMs = 0;
  unsigned long lastStoppedAtMs = 0;   // Horodatage du dernier arrêt (0 = jamais arrêtée)
  // feature-158 : secondes CRÉDITÉES par une reprise après redémarrage
  // (condition 4 du gate pool-chemistry — ordres d'écriture imposés, voir
  // filtration.cpp). Posée UNIQUEMENT par begin() (condition 3 : consommation
  // UNE SEULE FOIS) ; tout démarrage ultérieur du même uptime (Boost compris)
  // la remet à 0. Additionnée à l'écoulé par filtrationElapsedMs() — jamais
  // fusionnée dans startedAtMs, pour que l'ordre d'écriture reste observable.
  unsigned long carryMs = 0;
};

// bug-126 — planning calculé à l'ARRÊT d'un cycle, promu au passage de minuit.
// 100 % RAM, JAMAIS persisté (ADR-0079) : un reboot avant minuit fait
// repartir la journée sur le planning de la veille — dégradation d'un jour,
// auto-réparatrice (le cycle suivant recalcule un pending à son propre arrêt).
// ⚠️ La perte la PLUS LOURDE des deux n'est pas celle-ci mais celle de
// `_haveRepresentativeMeasure` (membre privé, plus bas) : un reboot
// RÉ-AUTORISE l'amorçage — le premier planning après redémarrage peut
// reposer sur moins de kFiltrationMinRepresentativeSamples échantillons.
struct PendingSchedule {
  bool  valid = false;
  int   startMin = -1;
  int   endMin = -1;
  float refTempC = NAN;                // journal
  uint16_t sampleCount = 0;            // journal
  float refMinC = NAN, refMaxC = NAN;  // journal (dispersion de la moyenne)
  char  computedOnDate[9] = {};        // "YYYYMMDD" local de l'arrêt producteur
  // Instantané de BASE (arbitrage A) : état du planning courant à l'instant
  // du calcul. Toute divergence à la promotion = l'utilisateur (ou MQTT) est
  // passé par un chemin d'écriture entre-temps — le pending est alors jeté.
  // Un INSTANTANÉ attrape tout chemin d'écriture, y compris ceux non
  // énumérés ; une liste d'appels à invalider se périmerait au premier
  // chemin ajouté.
  char  basisStart[6] = {};
  char  basisEnd[6] = {};
  // 8 o : couvre le plus long mode reconnu, "manual" (6 + NUL = 7) — une
  // valeur de 7+ caractères serait tronquée SILENCIEUSEMENT et le pending
  // jeté à chaque promotion sans que rien ne le dise (revue P2, corrections
  // mineures).
  char  basisMode[8] = {};
};

class FiltrationManager {
private:
  FiltrationRuntime state;
  bool relayState = false;

  // bug-126 — accumulation de la moyenne de température du cycle courant et
  // planning en attente (~64 o de RAM statique).
  PendingSchedule      _pending;
  // Initialiseur EXPLICITE, et non `= {}` ni rien du tout (revue R2). L'objet
  // global `filtration` (filtration.cpp) est à durée de stockage STATIQUE : ses
  // membres sans initialiseur sont zéro-initialisés de toute façon, donc retirer
  // `= {}` ne change rien — `mean` vaudrait 0.0f, en violation du contrat
  // « NaN de mean <=> count == 0 » (schedule_logic.h), et 0.0 °C est une valeur
  // d'apparence PHYSIQUE, hors bornes basses, qui déclencherait à tort une alerte.
  // Seul cet initialiseur l'empêche. resetCycleTempAccumulator() en tête de
  // begin() le double (revue P2, M2) et couvre en plus les instances NON statiques
  // — un harnais de test, une instance locale — que begin() n'a pas forcément vues.
  CycleTempAccumulator _cycleTemp{NAN, NAN, NAN, 0};
  uint32_t             _lastTempSampleMs = 0;             // cadence, wrap-safe
  // Le drapeau appartient à la MESURE, pas à l'écriture (revue P2, B1) : armé
  // en tête du cas Ok de writePendingSchedule(), AVANT le test de fenêtre
  // identique — sinon un régime stable (même fenêtre chaque jour, chemin
  // NORMAL) le laisserait `false` indéfiniment et un cycle écourté à un seul
  // échantillon pourrait ensuite écrire un planning différent. Un reboot
  // RÉ-AUTORISE l'amorçage (RAM) : voir le commentaire de PendingSchedule.
  bool                 _haveRepresentativeMeasure = false;
  char                 _lastPromotedDay[9] = {};           // idempotence (une promotion/jour)
  // Throttle du journal "promotion différée" : sans lui, un mode Marche à
  // cheval sur minuit journaliserait à CHAQUE tour de loop tant que la pompe
  // tourne (des heures). Même convention que kMutexTimeoutWarnThrottleMs.
  unsigned long        _lastPendingDeferredLogMs = 0;
  // P2-C : ensureTimesValid() peut réparer une plage nulle depuis begin(),
  // AVANT mqttManager.begin() (outQueue pas encore créée — publishAlert()
  // serait un no-op silencieux, même piège que main.cpp:399 pour l'alerte
  // history). On mémorise la réparation et on la publie au premier tour
  // d'update() où MQTT est connecté ; le systemLogger.critical(), lui, part
  // bien dès le boot.
  bool                 _scheduleRepairAlertPending = false;
  // feature-146 : migration "force" → Marche détectée au boot (begin()),
  // AVANT mqttManager.begin() — même contrainte que _scheduleRepairAlertPending
  // ci-dessus (outQueue pas encore créée). Publiée au premier tour d'update()
  // où MQTT est connecté.
  bool                 _forceModeMigrationAlertPending = false;
  // feature-146 (condition #3 du gate) : throttle du `critical` sur mode
  // inconnu rencontré en tête d'update() — même convention que les autres
  // throttles de ce fichier.
  unsigned long        _lastUnknownModeLogMs = 0;

  void sampleCycleTemperature(uint32_t nowMs, bool haveTime, int nowMin, int startMin, int endMin);
  void writePendingSchedule(bool haveTime);         // à l'ARRÊT — n'écrit JAMAIS filtrationCfg
  void promotePendingScheduleIfDue(bool haveTime);  // en TÊTE d'update()

  // feature-056 : état de la filtration EXTERNE (mode ExternalFiltration).
  // Triplet protégé par un spinlock portMUX (PAS un mutex FreeRTOS : écrit par
  // le handler HTTP et le callback MQTT async → un mutex bloquant y est proscrit).
  // Section critique MINIMALE (condition pool-chemistry #5) : on ne copie que les
  // 3 champs sous lock, l'évaluation et les logs se font HORS lock. Jamais persisté
  // en NVS (condition #3 : boot toujours OFF/known=false → fail-safe).
  mutable portMUX_TYPE _externalMux = portMUX_INITIALIZER_UNLOCKED;
  bool _externalOn = false;       // dernier état signalé
  uint32_t _externalLastMs = 0;   // millis() de réception du dernier signal
  bool _externalKnown = false;    // au moins un signal reçu depuis le boot

  bool getCurrentMinutesOfDay(int& minutes);
  int timeStringToMinutes(const String& value);
  bool isMinutesInRange(int now, int start, int end);

  // feature-158 : rafraîchit l'enregistrement RTC scellé qui porte la durée
  // MESURÉE du cycle de filtration courant à travers un reboot (patron
  // OutageRtcRecord de measure_validity.cpp). Appelé aux transitions
  // marche/arrêt et périodiquement (throttle _lastFiltrationRtcRefreshMs)
  // pendant la marche — JAMAIS de décision ici, une simple photo de l'état
  // courant (condition 2 du gate : filtrationRunning + installMode PROPRES à
  // la filtration, rien de déduit d'un verdict de continuité).
  void refreshFiltrationRtcRecord();
  unsigned long _lastFiltrationRtcRefreshMs = 0;  // cadence du rafraîchissement RTC

public:
  void begin();
  void update();

  bool isRunning() const { return state.running; }
  bool getRelayState() const { return relayState; }

  // feature-155 : instant millis() de démarrage du cycle de filtration COURANT
  // — sert aux gardes qui exigent un régime hydraulique établi depuis un délai
  // minimal (ex. accord de la mesure pH sur un test manuel,
  // kPhTuneMinFiltrationMs). 0 si la filtration est à l'arrêt (cf. isRunning()) :
  // NE PAS l'interpréter comme « démarrée à l'epoch », toujours tester
  // isRunning() avant de calculer une durée écoulée.
  unsigned long filtrationStartedAtMs() const { return state.startedAtMs; }

  // feature-158 (condition 7 du gate pool-chemistry, A4) : ACCESSEUR UNIQUE de
  // la durée écoulée du cycle courant — 0 si à l'arrêt. Remplace le ternaire
  // `isRunning() ? (millis() - filtrationStartedAtMs()) : 0` recopié jusque-là
  // aux trois sites consommateurs (web_routes_data.cpp, ws_manager.cpp,
  // web_routes_config.cpp) : le ternaire vit ICI, et ICI seulement. Additionne
  // l'écoulé en RAM (wrap-safe, soustraction non signée 32 bits) et le report
  // crédité par une reprise après redémarrage (state.carryMs, feature-158) —
  // jamais fusionné dans startedAtMs pour garder observables les deux grandeurs.
  // Arithmétique (wrap-safe à 2^32 ms) extraite dans computeFiltrationElapsedMs()
  // (filtration_resume_logic.{h,cpp}, module pur, prouvée par vecteurs natifs) —
  // seule la sentinelle `!running` reste ici, c'est de l'état, pas du calcul.
  unsigned long filtrationElapsedMs() const {
    if (!state.running) return 0;
    return (unsigned long)computeFiltrationElapsedMs(
        (uint32_t)millis(), (uint32_t)state.startedAtMs, (uint32_t)state.carryMs);
  }
  // Même accesseur, en secondes (consommateurs `/data` et WebSocket).
  uint32_t filtrationElapsedS() const {
    return (uint32_t)(filtrationElapsedMs() / 1000UL);
  }

  // feature-056 : signal d'état de la filtration externe. setExternalState est
  // NON bloquant (spinlock, horodatage millis() interne) — sûr depuis un handler
  // HTTP ou un callback MQTT async. getExternalState copie le triplet sous lock.
  void setExternalState(bool running);
  void getExternalState(bool& on, uint32_t& lastMs, bool& known) const;

  // feature-056 : résolution de la présence d'eau selon le mode d'installation.
  // SOURCE UNIQUE consommée par toutes les gardes de dosage (canDose, injection
  // manuelle, monitor injection). Fail-closed strict.
  WaterPresence resolveWaterPresence() const;

  // feature-086 (révisé feature-146) : durée PROGRAMMÉE de filtration du jour,
  // en minutes. LECTURE SEULE, aucune décision de dosage n'en dépend : sert au
  // maximum journalier délivrable affiché (getPhDailyMaxMl). Auto et Programme
  // suivent la plage [start, end) de filtrationCfg (passage minuit géré),
  // même prédicat que decideFiltrationRun (schedule_logic.cpp). Marche = 1440
  // (la pompe tourne en permanence tant que l'utilisateur ne change pas de
  // mode) — AFFICHAGE SEUL, ne borne aucun dosage (gate #4 : l'horizon de
  // dosage réparti suit la même règle, cf. pump_controller.cpp
  // filtrationDosingHorizonMinutes). −1 = indisponible : mode Arrêt (hors
  // planning) ou plage invalide (start == end, heure illisible). Appelable
  // depuis un handler web async (lit filtrationCfg.mode/start/end — course
  // bénigne d'un tour de loop, patron des getters existants).
  int scheduledMinutesToday() const;

  void computeAutoSchedule();
  void ensureTimesValid();
  void publishState();
};

extern FiltrationManager filtration;

#endif // FILTRATION_H
