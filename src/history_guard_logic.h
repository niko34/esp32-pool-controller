#ifndef HISTORY_GUARD_LOGIC_H
#define HISTORY_GUARD_LOGIC_H

// =============================================================================
// history_guard_logic — Disjoncteur de boot-loop de l'init history PURE (v2.10.0)
// =============================================================================
// Logique de décision du disjoncteur de boot-loop autour de l'init de la
// partition history (feature-059). Une corruption littlefs peut provoquer une
// exception CPU non récupérable (IntegerDivideByZero dans lfs_alloc) à la
// première écriture → boot-loop. La seule défense viable est de détecter la
// boucle de panics ENTRE les boots (drapeau NVS posé avant montage, levé après
// init complète) et d'effacer la partition avant remontage. Extraite de
// history.cpp pour être testable en natif (sans ESP32, sans FreeRTOS, sans
// Arduino). Style ADR-0017, comme self_heal_logic / net_health_logic.
//
// INVARIANT : ce module ne fait QUE décider à partir de valeurs déjà collectées.
// La collecte (esp_reset_reason, lecture NVS crash_cnt) et les effets
// (esp_partition_erase_range, écriture NVS, systemLogger.critical) restent dans
// la coquille impure history.cpp.
//
// CONTRAINTE : pas d'Arduino.h, pas de FreeRTOS, pas de <String>/<FS.h>. On
// utilise l'en-tête C <stdint.h> (et non <cstdint>) pour compiler en natif.
// =============================================================================

#include <stdint.h>

// Cause du reset précédent, mappée depuis esp_reset_reason() par la coquille
// impure (switch dans history.cpp) — aucune dépendance à esp_system.h ici.
enum class HistoryResetKind : uint8_t {
  PowerOn = 0,  // ESP_RST_POWERON : mise sous tension
  Software,     // ESP_RST_SW : redémarrage logiciel (ESP.restart, OTA)
  External,     // ESP_RST_EXT : reset broche EN
  Brownout,     // ESP_RST_BROWNOUT : chute d'alimentation
  DeepSleep,    // ESP_RST_DEEPSLEEP : sortie de deep sleep
  Panic,        // ESP_RST_PANIC : exception CPU (le cas IntegerDivideByZero)
  IntWdt,       // ESP_RST_INT_WDT : watchdog d'interruption
  TaskWdt,      // ESP_RST_TASK_WDT : watchdog de tâche
  OtherWdt,     // ESP_RST_WDT : autre watchdog
  Other         // toute autre cause (inconnue, SDIO, ...)
};

// Verdict de l'évaluation du disjoncteur.
enum class HistoryGuardDecision : uint8_t {
  Proceed = 0,    // pas de drapeau OU reset propre → réarmer (compteur→1 posé) et monter
  CountCrash,     // drapeau + crash, sous seuil → compteur+1 posé, monter normalement
  ErasePartition  // drapeau + crash, seuil atteint → effacer AVANT montage, compteur→1 posé
};

// Entrées de la décision — toutes fournies par l'appelant (aucune API Arduino).
struct HistoryGuardInputs {
  uint8_t crashCount;      // crash_cnt lu en NVS ; 0 = pas de drapeau (init précédente complète)
  bool    resetWasCrash;   // résultat de isCrashResetKind() sur le reset précédent
  uint8_t eraseThreshold;  // kHistoryInitCrashThreshold
};

// Mapping pur : Panic, IntWdt, TaskWdt, OtherWdt → true ; le reste → false.
// Un POWERON/soft reset/brownout n'est PAS un crash : un drapeau resté posé
// dans ce cas est réarmé sans incrémenter (règle 2 ci-dessous).
bool isCrashResetKind(HistoryResetKind kind);

// Décision pure du disjoncteur. Ordre EXACT des règles (priorité décroissante) :
//   1. crashCount == 0              → Proceed (pas de drapeau : l'init précédente
//      s'est terminée proprement, ou premier boot).
//   2. !resetWasCrash               → Proceed (reset propre : réarmement sans compter).
//   3. crashCount >= eraseThreshold → ErasePartition (frontière >= : au moins
//      eraseThreshold crashs consécutifs ont eu lieu pendant la fenêtre d'init).
//   4. sinon                        → CountCrash.
HistoryGuardDecision evaluateHistoryGuard(const HistoryGuardInputs& in);

#endif // HISTORY_GUARD_LOGIC_H
