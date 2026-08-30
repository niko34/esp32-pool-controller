#ifndef FILTRATION_H
#define FILTRATION_H

#include <Arduino.h>
#include "config.h"

struct FiltrationRuntime {
  bool running = false;
  bool scheduleComputedThisCycle = false;
  unsigned long startedAtMs = 0;
  unsigned long lastStoppedAtMs = 0;   // Horodatage du dernier arrêt (0 = jamais arrêtée)
  unsigned long forceOnStartMs = 0;    // Quand forceOn a été activé (0 = inactif)
  unsigned long forceOffStartMs = 0;   // Quand forceOff a été activé (0 = inactif)
};

class FiltrationManager {
private:
  FiltrationRuntime state;
  bool relayState = false;
  float _lastScheduledTemp = NAN;  // Température du dernier calcul auto (deadband 1°C)

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

public:
  void begin();
  void update();

  bool isRunning() const { return state.running; }
  bool getRelayState() const { return relayState; }

  // feature-056 : signal d'état de la filtration externe. setExternalState est
  // NON bloquant (spinlock, horodatage millis() interne) — sûr depuis un handler
  // HTTP ou un callback MQTT async. getExternalState copie le triplet sous lock.
  void setExternalState(bool running);
  void getExternalState(bool& on, uint32_t& lastMs, bool& known) const;

  // feature-056 : résolution de la présence d'eau selon le mode d'installation.
  // SOURCE UNIQUE consommée par toutes les gardes de dosage (canDose, injection
  // manuelle, monitor injection). Fail-closed strict.
  WaterPresence resolveWaterPresence() const;

  // feature-086 : durée PROGRAMMÉE de filtration du jour, en minutes (plage
  // [start, end) de filtrationCfg, passage minuit géré). LECTURE SEULE, aucune
  // décision de dosage n'en dépend : sert au maximum journalier délivrable
  // affiché (getPhDailyMaxMl). Modes `auto` ET `manual` — même prédicat que
  // decideFiltrationRun (schedule_logic.cpp) : les deux suivent la plage
  // start/end, `manual` = horaires fixés par l'utilisateur. −1 = indisponible :
  // mode `off`/`force` (hors planning) ou plage invalide (start == end, heure
  // illisible). Capacité ANNONCÉE fondée sur le planning : forceOn/boost hors
  // plage n'y entrent pas. Appelable depuis un handler web async (lit
  // filtrationCfg.mode/start/end — course bénigne d'un tour de loop, patron
  // des getters existants).
  int scheduledMinutesToday() const;

  void computeAutoSchedule();
  void ensureTimesValid();
  void publishState();
};

extern FiltrationManager filtration;

#endif // FILTRATION_H
