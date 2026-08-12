#include <Arduino.h>
#include <new>       // std::set_new_handler
#include <atomic>
#include <esp_heap_caps.h>  // heap_caps_register_failed_alloc_callback
#include "oom_guard.h"
#include "oom_guard_logic.h"
#include "constants.h"
#include "logger.h"

// =============================================================================
// État partagé handler/callback ↔ loop (bug-oom-accept-http-durcissement)
// =============================================================================
// Le new_handler peut être invoqué depuis N'IMPORTE QUELLE tâche (async_tcp,
// mqttTask, loopTask...) et le callback heap_caps depuis l'INTÉRIEUR de
// l'allocateur : tout l'état partagé est atomique, aucun mutex, aucune alloc,
// aucun systemLogger dans ces chemins (réentrance allocateur interdite).
// seq_cst par défaut : coût négligeable (chemins froids), raisonnement simple.

static std::atomic<void*>    sOomReserve{nullptr};            // Réserve d'urgence (nullptr = consommée/absente)
static std::atomic<uint8_t>  sOomState{kOomStateDepleted};    // Machine à états (oom_guard_logic.h)
static std::atomic<uint32_t> sOomReleaseCount{0};             // Épisodes de libération de la réserve
static std::atomic<uint32_t> sOomFailedAllocCount{0};         // Échecs heap_caps cumulés (diagnostic)
static std::atomic<uint32_t> sOomFailedAllocLastSize{0};      // Taille demandée du dernier échec

// new_handler (contrat libstdc++ : appelé quand malloc échoue dans operator
// new, puis le new est RE-TENTÉ). UNIQUEMENT : extraction atomique de la
// réserve + free() + état + compteur. Si la réserve est déjà consommée →
// désenregistrement (le new suivant lève bad_alloc = comportement d'avant le
// parachute, coredump en filet ultime). Le log CRITICAL est émis en DIFFÉRÉ
// depuis oomGuardLoopTick().
static void oomNewHandler() {
  void* reserve = sOomReserve.exchange(nullptr);
  OomHandlerAction action = oomOnAllocFailed(reserve != nullptr);
  if (action.freeReserve) {
    free(reserve);
    sOomReleaseCount.fetch_add(1);
  }
  if (action.unregisterHandler) {
    std::set_new_handler(nullptr);
  }
  sOomState.store(action.nextState);
}

// Callback diagnostic heap_caps (COMPLÉMENT d'attribution) : appelé depuis
// l'intérieur de l'allocateur à CHAQUE échec d'allocation (malloc C compris,
// pas seulement operator new). Mode FLAG-SEUL : pas de free, pas de log —
// drainé par la loop (WARN one-shot throttlé).
static void oomFailedAllocHook(size_t size, uint32_t caps, const char* function_name) {
  (void)caps;
  (void)function_name;
  sOomFailedAllocLastSize.store((uint32_t)size);
  sOomFailedAllocCount.fetch_add(1);
}

void initOomGuard() {
  // Réserve allouée au plus TÔT du setup : bloc contigu pris avant toute
  // fragmentation. Pas de log ici (systemLogger.begin() pas encore appelé) —
  // le setup journalise l'armement juste après l'init du logger.
  void* reserve = malloc(kOomReserveBytes);
  if (reserve != nullptr) {
    sOomReserve.store(reserve);
    sOomState.store(kOomStateArmed);
  } else {
    // Improbable (boot) : la loop tentera l'armement via le chemin réarmement.
    sOomState.store(kOomStateReleased);
  }
  std::set_new_handler(oomNewHandler);
  heap_caps_register_failed_alloc_callback(oomFailedAllocHook);
}

bool oomGuardReserveArmed() {
  return sOomReserve.load() != nullptr;
}

void oomGuardLoopTick() {
  unsigned long now = millis();

  // 1) Log CRITICAL différé « réserve OOM consommée » — one-shot par épisode
  //    (drain du compteur posé par le handler). Pas de flushToDisk() ici : en
  //    pleine famine une écriture LittleFS aggraverait la pression ; le flush
  //    one-shot de checkHeapHealth() (kLowHeapWarnBytes) couvre la persistance.
  static uint32_t sLoggedReleaseCount = 0;
  uint32_t releases = sOomReleaseCount.load();
  if (oomShouldLogRelease(releases, sLoggedReleaseCount)) {
    sLoggedReleaseCount = releases;
    systemLogger.critical("Réserve OOM consommée (épisode n°" + String(releases) +
                          ") — famine heap absorbée par le parachute (" +
                          String(kOomReserveBytes) + " o libérés), maxBloc=" +
                          String(ESP.getMaxAllocHeap()) + " o, libre=" +
                          String(ESP.getFreeHeap()) + " o");
  }

  // 2) WARN différé des échecs d'allocation heap_caps (diagnostic
  //    d'attribution du crash du 11/08) — throttlé kOomFailWarnThrottleMs.
  static uint32_t sLoggedFailedCount = 0;
  static unsigned long sLastFailWarnMs = 0;
  uint32_t fails = sOomFailedAllocCount.load();
  if (fails != sLoggedFailedCount &&
      (sLastFailWarnMs == 0 || now - sLastFailWarnMs >= kOomFailWarnThrottleMs)) {
    systemLogger.warning("Allocation heap échouée ×" +
                         String(fails - sLoggedFailedCount) +
                         " (dernière taille demandée=" +
                         String(sOomFailedAllocLastSize.load()) + " o, cumul=" +
                         String(fails) + ")");
    sLoggedFailedCount = fails;
    sLastFailWarnMs = now;
  }

  // 3) Réarmement à cadence lente (hystérésis kOomRearmMinMaxAllocBytes).
  static unsigned long sLastRearmCheckMs = 0;
  if (now - sLastRearmCheckMs < kOomRearmCheckIntervalMs) return;
  sLastRearmCheckMs = now;

  // Critère = pointeur de réserve (pas l'état) : converge même si une course
  // handler/réarmement a laissé un état Armed avec réserve nulle (cf. logique).
  if (!oomShouldRearm(sOomReserve.load() == nullptr, ESP.getMaxAllocHeap(),
                      kOomRearmMinMaxAllocBytes)) {
    return;
  }
  void* reserve = malloc(kOomReserveBytes);
  if (reserve == nullptr) return;  // maxBloc a bougé entre-temps : retente au prochain tick

  // Ordre voulu : pointeur AVANT état. Si le handler court entre les deux
  // (nouvelle famine immédiate), il consomme la nouvelle réserve via exchange
  // — jamais de double free ni de fuite. Exchange en ceinture-bretelles : si
  // une réserve résiduelle existait (course théorique), elle est libérée.
  void* previous = sOomReserve.exchange(reserve);
  if (previous != nullptr) free(previous);
  sOomState.store(kOomStateArmed);
  // Ré-enregistrement inconditionnel (idempotent) : couvre l'état Depleted où
  // le handler s'était désenregistré.
  std::set_new_handler(oomNewHandler);
  systemLogger.info("Parachute OOM réarmé : réserve de " + String(kOomReserveBytes) +
                    " o ré-allouée (maxBloc=" + String(ESP.getMaxAllocHeap()) + " o)");
}
