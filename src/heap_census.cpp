#include "heap_census.h"

#include <Arduino.h>         // millis()
#include <atomic>
#include <cstring>           // memcpy — évite tout cast d'alignement direct (idiome oom_guard.cpp)
#include <esp_heap_caps.h>   // heap_caps_walk, walker_heap_into_t, walker_block_info_t
#include <esp_timer.h>       // esp_timer_get_time — C1 : JAMAIS le compteur de cycles CCOUNT

#include "heap_metrics.h"    // heapCaps8BitMask, heapFloorsRead (A2/I2 : lecture SANS parcours neuf)

// Idiome de heap_metrics.cpp:20-23 : épingle l'hypothèse structurelle au lieu
// de la supposer silencieusement. Canari non conforme à l'exécution => la
// valeur publiée est kHeapCensusAllocSizeUnknown (ADR-0072), jamais inventée.
#if !defined(CONFIG_HEAP_POISONING_LIGHT)
#error "kHeapPoisonHeadBytes=8 suppose CONFIG_HEAP_POISONING_LIGHT ; relire poison_head_t."
#endif

// =============================================================================
// heap_census — coquille impure (état, callbacks heap_caps_walk, chronomètre)
// =============================================================================
// Toute la doctrine (régime de verrouillage, protocole en deux temps, C1-C4,
// S4/S8/S9/S13) est écrite en tête de heap_census.h — ne pas la répéter ici,
// seulement l'appliquer.
//
// PRODUCTEUR UNIQUE assumé pour heapCensusRequest()/heapPeekRequest() : UNE
// SEULE tâche async_tcp exécute les handlers AsyncWebServer dans ce projet
// (convention déjà en place ailleurs, ex. le filtre de la garde 503 dans
// web_server.cpp, heap_caps_logic.h:122-124). Les paramètres de la requête EN
// ATTENTE sont donc écrits AVANT la CAS IDLE->REQUESTED (ordre RELEASE), que
// la loop observe en ACQUIRE (heapCensusServiceLoop) : aucune écriture
// déchirée possible SANS supposer de second appelant concurrent — hypothèse à
// revoir si ce projet passait un jour à plusieurs tâches async_tcp.
// =============================================================================

namespace {

// ---------------------------------------------------------------------------
// Poisoning léger — layout brut, jamais exposé par l'IDF (interne à
// multi_heap_poisoning.c). Confirmé par désassemblage indépendant (0xABBA1234
// en .rodata de l'objet compilé, cf. constants.h).
// ---------------------------------------------------------------------------
struct PoisonHead {
  uint32_t canary;
  uint32_t allocSize;  // size_t == uint32_t sur cible 32 bits (ESP32)
};
static_assert(sizeof(PoisonHead) == kHeapPoisonHeadBytes,
              "poison_head_t suppose 8 o sur cible 32 bits (canary + size_t) ; "
              "revoir kHeapPoisonHeadBytes si la cible change.");

// ---------------------------------------------------------------------------
// État PARTAGÉ recensement (R2) + lecture (R3) — chronomètre et désactivation
// C1, plancher temporel S9 : LE MÊME coût O(blocs), donc LE MÊME budget.
// ---------------------------------------------------------------------------
std::atomic<uint32_t> sLastWalkMs{0};   // Horodatage loop du DERNIER parcours abouti (S9)
std::atomic<bool> sEverWalked{false};
std::atomic<uint32_t> sLastWalkUs{0};   // Durée du DERNIER parcours (C4)
std::atomic<uint32_t> sMaxWalkUs{0};    // Pire parcours depuis le boot (C4)
std::atomic<bool> sDisabled{false};     // C1 : désactivation DÉFINITIVE (jamais réarmée)

/**
 * @brief Chronomètre + auto-désactivation DÉFINITIVE (C1), PARTAGÉ R2/R3.
 *
 * Patron de oom_guard.cpp:408-418 (heapIntegrityAdmitNextRun -> sProbeDisabled) :
 * POST-HOC par construction — le dépassement a déjà eu lieu, le parcours EN
 * COURS reste publié (il a réellement eu lieu et ses résultats sont valides),
 * seuls les parcours SUIVANTS sont refusés. La base de calibrage est mouvante
 * (constants.h) : pas de réarmement, jamais.
 */
void noteWalkDuration(uint32_t durationUs) {
  sLastWalkUs.store(durationUs, std::memory_order_relaxed);
  uint32_t prevMax = sMaxWalkUs.load(std::memory_order_relaxed);
  if (durationUs > prevMax) sMaxWalkUs.store(durationUs, std::memory_order_relaxed);
  if (durationUs > kHeapCensusMaxDurationUs) {
    // C4 : le WARN + l'exposition sont laissés à un appelant HORS de ce
    // module (S13 interdit systemLogger ICI, y compris sur ce chemin
    // d'auto-désactivation). heapCensusIsDisabled()/heapCensusMaxWalkUs()
    // existent pour qu'un tel site externe (loop, hors chemin de requête
    // HTTP) puisse émettre ce WARN one-shot.
    sDisabled.store(true, std::memory_order_relaxed);
  }
}

// ---------------------------------------------------------------------------
// R2 — Recensement
// ---------------------------------------------------------------------------
std::atomic<uint8_t> sCensusState{kHeapCensusIdle};
std::atomic<uint32_t> sPendingUsedFrom{0};
std::atomic<uint32_t> sPendingUsedTo{0};
HeapCensusSnapshot sSnapshot;  // Écrit UNIQUEMENT par la loop en état REQUESTED (S8/I5)
std::atomic<uint32_t> sSeq{0};

/** @brief Contexte du callback de recensement (walk en cours, interruptions masquées). */
struct CensusWalkCtx {
  HeapCensusSnapshot* snap;
  uint32_t usedFrom;
  uint32_t usedTo;
  uint32_t freeFloor;      // Seuil de rejet courant (heapCensusKeptFloor), mis à jour après chaque insertion retenue
  uint32_t lastRegionStart;  // heapInfo.start de la DERNIÈRE région vue (détection de changement de région, B3)
  bool sawRegion;            // false tant qu'aucun bloc n'a été visité (pas encore de région connue)
};

/**
 * @brief Callback heap_caps_walk du recensement — COQUILLE, rien d'autre.
 *
 * ZÉRO log, ZÉRO String, ZÉRO allocation, ZÉRO appel non-inline. Un log ici
 * déclencherait systemLogger -> broadcastLog -> _heapAllowsPush ->
 * systemLogger (récursion documentée en tête de heap_metrics.h) — ET
 * s'exécuterait sous le verrou du tas, interruptions masquées (spinlock
 * INTER-CŒURS, cf. heap_census.h).
 *
 * S8 : ne déréférence blk.ptr[0..7] QUE si used == true (canari + alloc_size
 * du poisoning léger) — un bloc LIBRE ne porte aucun en-tête fiable à cette
 * adresse. La structure publiée (HeapCensusUsedEntry) ne contient AUCUN
 * champ d'octets de contenu.
 */
bool censusWalkCb(walker_heap_into_t heapInfo, walker_block_info_t block, void* userData) {
  CensusWalkCtx* ctx = static_cast<CensusWalkCtx*>(userData);
  HeapCensusSnapshot* s = ctx->snap;

  // B3 (revue code-reviewer) : heapInfo est « la région DANS LAQUELLE CE BLOC
  // se trouve » (esp_heap_caps.h:468-470), PAS le tas entier — heap_caps_walk
  // visite potentiellement PLUSIEURS régions portant les mêmes caps.
  // heapStart/heapEnd publient donc l'ENVELOPPE (min/max) de TOUTES les
  // régions vues, jamais un simple écrasement à chaque bloc (qui ne
  // publierait que la DERNIÈRE région, arbitraire, incohérente avec les
  // compteurs qui agrègent, eux, TOUTES les régions). regionCount compte les
  // changements de région (délimitation par egalité de heapInfo.start).
  const uint32_t regionStart = (uint32_t)heapInfo.start;
  const uint32_t regionEnd = (uint32_t)heapInfo.end;
  if (!ctx->sawRegion || regionStart != ctx->lastRegionStart) {
    ctx->sawRegion = true;
    ctx->lastRegionStart = regionStart;
    s->regionCount++;
  }
  if (s->regionCount == 1u) {
    s->heapStart = regionStart;
    s->heapEnd = regionEnd;
  } else {
    if (regionStart < s->heapStart) s->heapStart = regionStart;
    if (regionEnd > s->heapEnd) s->heapEnd = regionEnd;
  }

  const uint32_t addr = (uint32_t)(uintptr_t)block.ptr;
  const uint32_t size = (uint32_t)block.size;

  if (!block.used) {
    s->freeBlockCount++;
    // Rejet grossier en deux instructions (comparaison + return) : tout le
    // tri fin a lieu APRÈS le parcours, hors verrou (heapCensusKeptFloor).
    if (size <= ctx->freeFloor) return true;
    if (heapCensusTopNInsert(s->freeBlocks, &s->freeReturned, kHeapCensusMaxFreeBlocks, addr,
                              size)) {
      ctx->freeFloor = heapCensusKeptFloor(s->freeBlocks, s->freeReturned, kHeapCensusMaxFreeBlocks);
    }
    return true;
  }

  s->usedBlockCount++;
  if (!heapCensusInWindow(addr, ctx->usedFrom, ctx->usedTo)) return true;
  s->usedInWindowCount++;
  if (s->usedReturned >= kHeapCensusMaxUsedBlocks) {
    s->usedTruncated = true;  // I13 : troncature publiée, jamais silencieuse
    return true;
  }

  uint32_t allocSize = kHeapCensusAllocSizeUnknown;
  if (size >= kHeapPoisonHeadBytes) {
    PoisonHead head;
    memcpy(&head, block.ptr, sizeof(head));  // S8 : blk.ptr[0..7], used == true uniquement
    if (head.canary == kHeapPoisonCanaryValue) allocSize = head.allocSize;
  }
  s->usedBlocks[s->usedReturned] = HeapCensusUsedEntry{addr, size, allocSize};
  s->usedReturned++;
  return true;
}

}  // namespace

HeapCensusRequestResult heapCensusRequest(uint32_t usedFrom, uint32_t usedTo) {
  if (sDisabled.load(std::memory_order_relaxed)) return kHeapCensusReqDisabled;
  if (sCensusState.load(std::memory_order_relaxed) != kHeapCensusIdle) return kHeapCensusReqBusy;
  if (!heapCensusMinIntervalElapsed((uint32_t)millis(), sLastWalkMs.load(std::memory_order_relaxed),
                                     sEverWalked.load(std::memory_order_relaxed),
                                     kHeapCensusMinIntervalMs)) {
    return kHeapCensusReqTooSoon;  // Réponse RAPIDE (429) — la loop revérifie quand même (S9)
  }

  // Producteur unique assumé (cf. doc en tête de fichier) : écrit AVANT la
  // CAS, publié par son ordre RELEASE.
  sPendingUsedFrom.store(usedFrom, std::memory_order_relaxed);
  sPendingUsedTo.store(usedTo, std::memory_order_relaxed);

  uint8_t expected = kHeapCensusIdle;
  if (!sCensusState.compare_exchange_strong(expected, kHeapCensusRequested,
                                             std::memory_order_release, std::memory_order_relaxed)) {
    return kHeapCensusReqBusy;
  }
  return kHeapCensusReqQueued;
}

HeapCensusState heapCensusGetState() {
  return (HeapCensusState)sCensusState.load(std::memory_order_acquire);
}

namespace {

/** @brief REQUESTED -> READY du recensement (R2). Voir heapCensusServiceLoop(). */
void serviceCensus() {
  const uint32_t nowMs = (uint32_t)millis();

  // M1 (revue code-reviewer/security-reviewer) : un POST sans GET qui suit
  // (script interrompu, coupure WiFi, curl tué) laisserait R2 verrouillé en
  // READY INDÉFINIMENT — heapCensusRequest() refuse tout état != IDLE (409),
  // et la péremption n'était évaluée QUE dans heapCensusBeginRead(), jamais
  // atteinte si personne ne lit. Le redémarrage, seule échappatoire évidente,
  // est de toute façon interdit pendant la fenêtre d'observation en cours. La
  // loop réclame donc elle-même un READY PÉRIMÉ jamais lu : READY -> IDLE,
  // TROISIÈME transition pilotée par la loop (documentée en tête de
  // heap_census.h). La CAS gère la course avec un READY -> READING concurrent
  // côté route : si une lecture démarre entre le `load` et la CAS, la CAS
  // échoue simplement (l'état n'est plus READY), sans effet — la lecture qui
  // a gagné la course va son cours normalement.
  if (sCensusState.load(std::memory_order_acquire) == kHeapCensusReady &&
      heapCensusIsStale(nowMs, sSnapshot.tookAtMs, kHeapCensusStaleMs)) {
    uint8_t stale = kHeapCensusReady;
    sCensusState.compare_exchange_strong(stale, kHeapCensusIdle, std::memory_order_acq_rel);
  }

  if (sCensusState.load(std::memory_order_acquire) != kHeapCensusRequested) return;

  if (sDisabled.load(std::memory_order_relaxed)) {
    // Désactivé entre la requête et ce tour de loop (défense en profondeur) :
    // on abandonne SANS parcourir, retour à IDLE.
    uint8_t expected = kHeapCensusRequested;
    sCensusState.compare_exchange_strong(expected, kHeapCensusIdle, std::memory_order_acq_rel);
    return;
  }

  // S9 : REVÉRIFIÉ ICI, du côté qui consomme, sur l'horodatage ÉCRIT PAR LA
  // LOOP (sLastWalkMs) — pas seulement le contrôle rapide de la route.
  if (!heapCensusMinIntervalElapsed(nowMs, sLastWalkMs.load(std::memory_order_relaxed),
                                     sEverWalked.load(std::memory_order_relaxed),
                                     kHeapCensusMinIntervalMs)) {
    return;  // Pas encore l'heure : la requête reste REQUESTED, réessayée au tour suivant
  }

  const uint32_t usedFrom = sPendingUsedFrom.load(std::memory_order_relaxed);
  const uint32_t usedTo = sPendingUsedTo.load(std::memory_order_relaxed);

  sSnapshot.usedFrom = usedFrom;
  sSnapshot.usedTo = usedTo;
  sSnapshot.freeBlockCount = 0;
  sSnapshot.usedBlockCount = 0;
  sSnapshot.usedInWindowCount = 0;
  sSnapshot.freeReturned = 0;
  sSnapshot.usedReturned = 0;
  sSnapshot.usedTruncated = false;
  // B3 (revue code-reviewer) : SANS cette remise à zéro, un parcours SANS
  // AUCUN bloc (tas vide, cas d'école) republierait les bornes/regionCount du
  // recensement PRÉCÉDENT sous un nouveau `seq` — un mensonge par omission,
  // la faute même qu'ADR-0072 codifie.
  sSnapshot.heapStart = 0;
  sSnapshot.heapEnd = 0;
  sSnapshot.regionCount = 0;

  CensusWalkCtx ctx{&sSnapshot, usedFrom, usedTo, 0, 0, false};
  const int64_t startUs = esp_timer_get_time();  // C1 : JAMAIS CCOUNT
  heap_caps_walk(heapCaps8BitMask(), censusWalkCb, &ctx);
  const uint32_t durationUs = (uint32_t)(esp_timer_get_time() - startUs);
  noteWalkDuration(durationUs);

  sLastWalkMs.store(nowMs, std::memory_order_relaxed);
  sEverWalked.store(true, std::memory_order_relaxed);

  // A2/I2 : concordance avec fb/ab — LECTURE SEULE d'atomiques déjà
  // échantillonnés par heapFloorsSample() (loop, 2 s). Aucun heap_caps_get_info()
  // neuf sur ce chemin.
  HeapFloors floors = heapFloorsRead();
  const HeapFloorEntry& ref = floors.masks[kHeapFloorSlotInternal8];
  sSnapshot.floorFreeBlocks = ref.lastFreeBlocks;
  sSnapshot.floorUsedBlocks = ref.lastAllocatedBlocks;
  sSnapshot.concordanceVerdict = heapCensusCompare(sSnapshot.freeBlockCount, sSnapshot.usedBlockCount,
                                                    ref.lastFreeBlocks, ref.lastAllocatedBlocks);

  sSnapshot.tookAtMs = nowMs;
  sSnapshot.seq = sSeq.fetch_add(1, std::memory_order_relaxed) + 1u;

  uint8_t expected = kHeapCensusRequested;
  sCensusState.compare_exchange_strong(expected, kHeapCensusReady, std::memory_order_acq_rel);
}

}  // namespace

HeapCensusReadResult heapCensusBeginRead(const HeapCensusSnapshot** outSnap) {
  if (outSnap == nullptr) return kHeapCensusReadNotReady;
  uint8_t expected = kHeapCensusReady;
  if (!sCensusState.compare_exchange_strong(expected, kHeapCensusReading, std::memory_order_acquire,
                                             std::memory_order_relaxed)) {
    return (expected == kHeapCensusReading) ? kHeapCensusReadBusy : kHeapCensusReadNotReady;
  }
  if (heapCensusIsStale((uint32_t)millis(), sSnapshot.tookAtMs, kHeapCensusStaleMs)) {
    sCensusState.store(kHeapCensusIdle, std::memory_order_release);  // ne bloque pas le PROCHAIN recensement
    return kHeapCensusReadStale;
  }
  *outSnap = &sSnapshot;
  return kHeapCensusReadOk;
}

void heapCensusEndRead() {
  uint8_t expected = kHeapCensusReading;
  sCensusState.compare_exchange_strong(expected, kHeapCensusIdle, std::memory_order_release,
                                        std::memory_order_relaxed);
}

uint32_t heapCensusLastWalkUs() { return sLastWalkUs.load(std::memory_order_relaxed); }
uint32_t heapCensusMaxWalkUs() { return sMaxWalkUs.load(std::memory_order_relaxed); }
bool heapCensusIsDisabled() { return sDisabled.load(std::memory_order_relaxed); }

#if defined(POOL_HEAP_PEEK) && POOL_HEAP_PEEK == 1
// =============================================================================
// R3 — Lecture de contenu. NON LIVRÉ EN PRODUCTION (ADR-0081) : ce bloc entier
// n'existe dans le binaire QUE si cette macro est activée par une variable
// d'environnement au build (jamais DÉFINIE dans un fichier versionné,
// invariant I10 — voir docs/adr/0081 pour la commande exacte).
// =============================================================================

namespace {

std::atomic<uint8_t> sPeekState{kHeapCensusIdle};
std::atomic<uint32_t> sPendingPeekAddr{0};
std::atomic<uint32_t> sPendingPeekLen{0};
HeapPeekSnapshot sPeekSnapshot;  // Écrit UNIQUEMENT par la loop en état REQUESTED
std::atomic<uint32_t> sPeekSeq{0};
std::atomic<uint32_t> sPeekBudgetSpent{0};  // Octets copiés cumulés depuis le boot (kHeapPeekMaxBytesPerBoot)

/** @brief Contexte du callback de lecture (walk en cours, interruptions masquées). */
struct PeekWalkCtx {
  uint32_t targetAddr;
  uint32_t requestedLen;
  bool found = false;
  bool used = false;
  uint32_t blockSize = 0;
  uint32_t allocSize = 0;
  bool canaryValid = false;
  bool stable = false;
  uint32_t copyLen = 0;
  uint8_t bytes[kHeapPeekMaxBytes] = {0};
};

/**
 * @brief Callback heap_caps_walk de la lecture — COQUILLE, rien d'autre.
 *
 * S4 : adresse validée en trouvant EXACTEMENT le bloc (block.ptr == addr) sur
 * un parcours FRAIS ; canari testé AVANT ET APRÈS la copie ; longueur clampée
 * par heapCensusPlanPeek() (PURE, aucun déréférencement) à
 * min(requestedLen, kHeapPeekMaxBytes, alloc_size DU BLOC DÉSIGNÉ).
 * I7 : aucun déréférencement hors du bloc trouvé par CE parcours — si
 * `addr` ne correspond à AUCUN block.ptr, rien n'est jamais lu.
 */
bool peekWalkCb(walker_heap_into_t /*heapInfo*/, walker_block_info_t block, void* userData) {
  PeekWalkCtx* ctx = static_cast<PeekWalkCtx*>(userData);
  if (ctx->found) return true;  // déjà localisé : on laisse le parcours se terminer sans y retoucher

  const uint32_t addr = (uint32_t)(uintptr_t)block.ptr;
  if (addr != ctx->targetAddr) return true;

  ctx->found = true;
  ctx->used = block.used;
  ctx->blockSize = (uint32_t)block.size;
  if (!block.used || block.size < kHeapPoisonHeadBytes) return true;  // S8 : pas d'en-tête fiable

  PoisonHead headBefore;
  memcpy(&headBefore, block.ptr, sizeof(headBefore));  // canari testé AVANT la copie (S4)
  ctx->canaryValid = (headBefore.canary == kHeapPoisonCanaryValue);
  if (!ctx->canaryValid) return true;
  ctx->allocSize = headBefore.allocSize;

  const HeapCensusPeekPlan plan = heapCensusPlanPeek(true, ctx->blockSize, ctx->allocSize,
                                                       ctx->requestedLen, kHeapPeekMaxBytes,
                                                       kHeapPoisonHeadBytes);
  if (!plan.ok) return true;

  const uint8_t* payload = static_cast<const uint8_t*>(block.ptr) + kHeapPoisonHeadBytes;
  memcpy(ctx->bytes, payload, plan.copyLen);
  ctx->copyLen = plan.copyLen;

  PoisonHead headAfter;
  memcpy(&headAfter, block.ptr, sizeof(headAfter));  // canari re-testé APRÈS la copie (S4)
  ctx->stable = (headAfter.canary == kHeapPoisonCanaryValue);
  return true;
}

/** @brief REQUESTED -> READY de la lecture (R3). Appelée depuis heapCensusServiceLoop(). */
void servicePeek() {
  const uint32_t nowMs = (uint32_t)millis();

  // M1 : même réclamation que serviceCensus() ci-dessus, IDEM pour R3 — un
  // POST /debug/heap_peek sans GET qui suit verrouillerait sinon la lecture
  // aussi longtemps que le recensement.
  if (sPeekState.load(std::memory_order_acquire) == kHeapCensusReady &&
      heapCensusIsStale(nowMs, sPeekSnapshot.tookAtMs, kHeapCensusStaleMs)) {
    uint8_t stale = kHeapCensusReady;
    sPeekState.compare_exchange_strong(stale, kHeapCensusIdle, std::memory_order_acq_rel);
  }

  if (sPeekState.load(std::memory_order_acquire) != kHeapCensusRequested) return;

  if (sDisabled.load(std::memory_order_relaxed)) {
    uint8_t expected = kHeapCensusRequested;
    sPeekState.compare_exchange_strong(expected, kHeapCensusIdle, std::memory_order_acq_rel);
    return;
  }
  // S9 : plancher PARTAGÉ avec R2 (même sLastWalkMs, même budget de 0,2/s
  // AJOUTÉ — constants.h).
  if (!heapCensusMinIntervalElapsed(nowMs, sLastWalkMs.load(std::memory_order_relaxed),
                                     sEverWalked.load(std::memory_order_relaxed),
                                     kHeapCensusMinIntervalMs)) {
    return;
  }

  const uint32_t addr = sPendingPeekAddr.load(std::memory_order_relaxed);
  const uint32_t len = sPendingPeekLen.load(std::memory_order_relaxed);

  PeekWalkCtx ctx{};
  ctx.targetAddr = addr;
  ctx.requestedLen = len;

  const int64_t startUs = esp_timer_get_time();  // C1 : JAMAIS CCOUNT
  heap_caps_walk(heapCaps8BitMask(), peekWalkCb, &ctx);
  const uint32_t durationUs = (uint32_t)(esp_timer_get_time() - startUs);
  noteWalkDuration(durationUs);

  sLastWalkMs.store(nowMs, std::memory_order_relaxed);
  sEverWalked.store(true, std::memory_order_relaxed);

  const bool eligible = ctx.found && ctx.used && ctx.canaryValid;
  uint32_t copyLen = eligible ? ctx.copyLen : 0u;

  // Budget cumulé (kHeapPeekMaxBytesPerBoot) : consommé sur la longueur
  // EFFECTIVEMENT copiée, jamais réservé à l'aveugle. Épuisé => clampé à 0,
  // jamais silencieusement (budgetClamped, I13).
  bool budgetClamped = false;
  if (copyLen > 0u) {
    const uint32_t spent = sPeekBudgetSpent.load(std::memory_order_relaxed);
    const uint32_t remaining = (spent >= kHeapPeekMaxBytesPerBoot) ? 0u : (kHeapPeekMaxBytesPerBoot - spent);
    if (copyLen > remaining) {
      copyLen = remaining;
      budgetClamped = true;
    }
    sPeekBudgetSpent.fetch_add(copyLen, std::memory_order_relaxed);
  }

  sPeekSnapshot.seq = sPeekSeq.fetch_add(1, std::memory_order_relaxed) + 1u;
  sPeekSnapshot.tookAtMs = nowMs;
  sPeekSnapshot.addr = addr;
  sPeekSnapshot.allocSize = eligible ? ctx.allocSize : kHeapCensusAllocSizeUnknown;
  sPeekSnapshot.len = copyLen;
  sPeekSnapshot.found = ctx.found && ctx.used;
  sPeekSnapshot.canaryValid = ctx.canaryValid;
  sPeekSnapshot.stable = ctx.stable;
  sPeekSnapshot.budgetClamped = budgetClamped;
  if (copyLen > 0u) memcpy(sPeekSnapshot.bytes, ctx.bytes, copyLen);

  uint8_t expected = kHeapCensusRequested;
  sPeekState.compare_exchange_strong(expected, kHeapCensusReady, std::memory_order_acq_rel);
}

}  // namespace

HeapPeekRequestResult heapPeekRequest(uint32_t addr, uint32_t len) {
  if (len == 0u) return kHeapPeekReqInvalidLen;
  if (sDisabled.load(std::memory_order_relaxed)) return kHeapPeekReqDisabled;
  if (heapPeekBudgetRemaining() == 0u) return kHeapPeekReqBudgetExhausted;
  if (sPeekState.load(std::memory_order_relaxed) != kHeapCensusIdle) return kHeapPeekReqBusy;
  if (!heapCensusMinIntervalElapsed((uint32_t)millis(), sLastWalkMs.load(std::memory_order_relaxed),
                                     sEverWalked.load(std::memory_order_relaxed),
                                     kHeapCensusMinIntervalMs)) {
    return kHeapPeekReqTooSoon;
  }

  sPendingPeekAddr.store(addr, std::memory_order_relaxed);
  sPendingPeekLen.store(len, std::memory_order_relaxed);

  uint8_t expected = kHeapCensusIdle;
  if (!sPeekState.compare_exchange_strong(expected, kHeapCensusRequested, std::memory_order_release,
                                           std::memory_order_relaxed)) {
    return kHeapPeekReqBusy;
  }
  return kHeapPeekReqQueued;
}

HeapCensusState heapPeekGetState() {
  return (HeapCensusState)sPeekState.load(std::memory_order_acquire);
}

HeapPeekReadResult heapPeekBeginRead(const HeapPeekSnapshot** outSnap) {
  if (outSnap == nullptr) return kHeapPeekReadNotReady;
  uint8_t expected = kHeapCensusReady;
  if (!sPeekState.compare_exchange_strong(expected, kHeapCensusReading, std::memory_order_acquire,
                                           std::memory_order_relaxed)) {
    return (expected == kHeapCensusReading) ? kHeapPeekReadBusy : kHeapPeekReadNotReady;
  }
  if (heapCensusIsStale((uint32_t)millis(), sPeekSnapshot.tookAtMs, kHeapCensusStaleMs)) {
    sPeekState.store(kHeapCensusIdle, std::memory_order_release);
    return kHeapPeekReadStale;
  }
  *outSnap = &sPeekSnapshot;
  return kHeapPeekReadOk;
}

void heapPeekEndRead() {
  uint8_t expected = kHeapCensusReading;
  sPeekState.compare_exchange_strong(expected, kHeapCensusIdle, std::memory_order_release,
                                      std::memory_order_relaxed);
}

uint32_t heapPeekBudgetRemaining() {
  const uint32_t spent = sPeekBudgetSpent.load(std::memory_order_relaxed);
  return (spent >= kHeapPeekMaxBytesPerBoot) ? 0u : (kHeapPeekMaxBytesPerBoot - spent);
}

#endif  // POOL_HEAP_PEEK

void heapCensusServiceLoop() {
  serviceCensus();
#if defined(POOL_HEAP_PEEK) && POOL_HEAP_PEEK == 1
  servicePeek();
#endif
}
