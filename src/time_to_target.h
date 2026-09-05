#ifndef TIME_TO_TARGET_H
#define TIME_TO_TARGET_H

// =============================================================================
// time_to_target — INDICATEUR « temps d'atteinte de cible » (feature-096)
// =============================================================================
// Coquille impure de time_to_target_logic : collecte les entrées (grandeur de
// décision de chaque régulation, seuils vivants, compteur de filtration, jour
// civil), fait avancer la machine d'états pure, journalise les événements
// `[KPI]`, entretient la mémoire RTC et l'anneau NVS `pool-kpi`.
//
// SENS UNIQUE — INTENTION STRUCTURELLE
// ------------------------------------
// Ce fichier lit pump_controller ; pump_controller ne doit JAMAIS lire
// celui-ci. L'indicateur est un instrument de MESURE : refermer la boucle
// (adapter un gain, un seuil ou une limite sur le KPI) exigerait une spec et un
// nouveau passage par pool-chemistry. La garde n'est pas un `#error` — il
// casserait la compilation de time_to_target.cpp lui-même, qui doit inclure
// pump_controller.h pour lire la grandeur de décision — mais un test de grep
// BIDIRECTIONNEL : aucune occurrence de `time_to_target` ni de `ttt` dans
// src/pump_controller.{h,cpp}.
//
// CONTEXTES
//   - begin() et tick() : loopTask UNIQUEMENT (la NVS est bloquante, et
//     PumpController::getPhDecisionValue() est réservée à loopTask) ;
//   - getSnapshot() / copyHistory() / isMemoryUnavailable() : lisibles depuis
//     les handlers AsyncWebServer (copie sous spinlock portMUX, patron
//     measure_validity). /data, le WebSocket et /get-kpi servent CET
//     instantané mémorisé, jamais l'accesseur de pump_controller (C2 du gate).
// =============================================================================

#include <Arduino.h>

#include "time_to_target_logic.h"

// Instantané d'une voie, servi à /data + WS (parité obligatoire).
struct TttChannelSnapshot {
  uint8_t  state   = kTttStateIdle;  // 0 pas de départ / 1 en cours / 2 atteinte / 3 non
  uint16_t minutes = 0;              // minutes de FILTRATION (cf. état)
  float    e0      = NAN;            // écart initial signé (NAN = pas de départ)
  uint8_t  flags   = 0;              // champ de bits kTttFlag*
};

struct TttSnapshot {
  TttChannelSnapshot ph;
  TttChannelSnapshot orp;
  bool memoryUnavailable = false;  // C4 : NVS refusée — indicateur suspendu
};

class TimeToTargetManager {
public:
  // Relit l'anneau NVS et l'état vivant (RAM RTC, sinon reprise par la NVS).
  // À appeler APRÈS loadDailyCounters() (le compteur de filtration doit être
  // restauré) et après rtcManager.applyToSystem() (l'heure doit être connue).
  void begin();

  // Un tour de loop ; l'avancement effectif est cadencé à 1 s en interne.
  // loopTask UNIQUEMENT.
  void tick();

  // Instantané cohérent des deux voies (une seule section critique).
  TttSnapshot getSnapshot() const;

  // Copie de la mémoire 7 jours pour `GET /get-kpi`. Aucune lecture NVS : la
  // copie vient de la RAM, un handler async peut donc l'appeler.
  void copyHistory(TttHistory& out) const;

  // true = la NVS `pool-kpi` est indisponible : l'indicateur est suspendu
  // (état 0), sans aucune nouvelle tentative.
  bool isMemoryUnavailable() const;

private:
  void tickChannel(uint8_t chan, const TttInputs& in);
  void archive(uint8_t chan, const TttDay& entry, uint32_t t0FiltMin);
  void logEvent(uint8_t chan, const char* phase, uint16_t minutes, float e0,
                float residual, bool warn);
  void publishCache();
  void publishHistory();
  void restoreFromNvs(uint32_t today);

  // --- État interne (loopTask uniquement) ---
  TttHistory _history;                    // anneau 7 jours (image RAM de la NVS)
  TttLive    _live[kTttChannelCount] = {};
  uint32_t   _lastTickMs = 0;             // cadence 1 s
  uint32_t   _offSinceMs = 0;             // début de l'arrêt de filtration courant
  uint32_t   _lastDate = 0;               // jour civil du tick précédent
  bool       _filtOff = false;            // filtration arrêtée (fin de journée)
  bool       _memoryUnavailable = false;
  bool       _memoryWarned = false;       // le warn C4 n'est émis QU'UNE fois
  bool       _clockWarned = false;        // le warn « heure invalide » aussi
  // Un seul avertissement de départ refusé par voie et par journée : la garde
  // se réévalue à chaque seconde, la journaliser à chaque fois noierait le log.
  bool       _implausibleWarned[kTttChannelCount] = {false, false};

  // --- Miroirs lus par les handlers async (async_tcp) ---
  mutable portMUX_TYPE _mux = portMUX_INITIALIZER_UNLOCKED;
  TttSnapshot _cache;                     // état 0 au boot (conservateur)
  // Copie publiée de l'anneau : `_history` est muté hors section critique
  // (l'écriture NVS est bloquante et n'a rien à faire sous spinlock), le
  // miroir est publié APRÈS mutation, en une seule section critique.
  TttHistory _historyPub;
};

extern TimeToTargetManager timeToTarget;

#endif  // TIME_TO_TARGET_H
