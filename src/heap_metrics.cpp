#include "heap_metrics.h"

#include <atomic>
#include <esp_heap_caps.h>
#include <esp_timer.h>  // esp_timer_get_time — chronometrage du parcours (gate, condition 5)

#include "constants.h"  // kOomProbeBlockUnknown (sentinelle « plus gros bloc non renseigné »)
#include "oom_guard_logic.h"  // oomProbeUsesRegionScan (sélection de branche, PURE — ADR-0017)

// La sélection de branche de la sonde est décidée par un prédicat PUR (testable
// en natif, cf. ADR-0017), qui recopie un bit MALLOC_CAP_* sans inclure
// esp_heap_caps.h. Cette égalité est le seul point de contact : si l'IDF changeait
// la valeur du bit, c'est la COMPILATION du firmware qui casse, pas un test natif
// silencieusement devenu faux.
// L'assert épingle MALLOC_CAP_**INTERNAL**, et non plus MALLOC_CAP_SPIRAM :
// l'invariant qui protège n'est pas « ce masque demande-t-il de la PSRAM » mais
// « ce masque peut-il ATTEINDRE le tas PSRAM ». Épingler le bit SPIRAM est en
// partie ce qui a laissé passer le défaut bloquant relevé en revue (caps
// MALLOC_CAP_DEFAULT = 0x1000 pour tout malloc()/new ordinaire).
static_assert(kOomProbeInternalCapBit == (uint32_t)MALLOC_CAP_INTERNAL,
              "kOomProbeInternalCapBit doit valoir exactement MALLOC_CAP_INTERNAL : "
              "la borne a priori du pire cas PSRAM de la sonde en depend (seul un "
              "masque garantissant la RAM interne autorise le parcours O(blocs)).");

// Capacités du pool « réellement allouable » : mémoire INTERNE (pas de PSRAM,
// qui a ses propres contraintes) ET accessible en accès OCTET — c'est
// exactement ce qu'exige un malloc()/new ordinaire. Voir heap_metrics.h pour la
// raison pour laquelle MALLOC_CAP_INTERNAL seul est trompeur (IRAM 32 bits).
static constexpr uint32_t kHeapCaps8Bit = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;

uint32_t heapLargestFreeBlock8Bit() {
  return (uint32_t)heap_caps_get_largest_free_block(kHeapCaps8Bit);
}

uint32_t heapFree8Bit() {
  return (uint32_t)heap_caps_get_free_size(kHeapCaps8Bit);
}

uint32_t heapCaps8BitMask() {
  return kHeapCaps8Bit;
}

HeapProbe heapProbeForCaps(uint32_t caps) {
  HeapProbe p;
  if (oomProbeUsesRegionScan(caps)) {
    // Condition 2 du gate pool-chemistry (bug-080 B-2) : BORNE A PRIORI. Sur
    // 4 Mo de PSRAM, le parcours O(blocs) de `largest_free_block` n'a pas de
    // pire cas connu — et il s'exécute interruptions coupées. On ne SAUTE pas la
    // sonde (interdit : les 12 sites d'appel du hook doivent tous rendre un
    // verdict), on change d'ALGORITHME : agrégats O(régions), plus gros bloc
    // déclaré non renseigné. Le verdict du post-mortem sait le lire ainsi.
    p.freeBytes = (uint32_t)heap_caps_get_free_size(caps);
    p.largestBlockBytes = kOomProbeBlockUnknown;
    return p;
  }
  // UN seul parcours : heap_caps_get_info() livre les deux grandeurs.
  multi_heap_info_t info;
  heap_caps_get_info(&info, caps);
  p.freeBytes = (uint32_t)info.total_free_bytes;
  p.largestBlockBytes = (uint32_t)info.largest_free_block;
  return p;
}

HeapMetrics heapMetricsSnapshot() {
  // Quatre appels heap_caps_* consécutifs : chacun s'exécute interruptions
  // coupées, d'où le regroupement en un instantané unique plutôt que des appels
  // dispersés dans les appelants. Aucun log, aucune allocation ici.
  HeapMetrics m;
  m.free8 = (uint32_t)heap_caps_get_free_size(kHeapCaps8Bit);
  m.maxAlloc8 = (uint32_t)heap_caps_get_largest_free_block(kHeapCaps8Bit);
  m.freeInternal = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  m.maxAllocInternal = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
  return m;
}

// ---------------------------------------------------------------------------
// Suivi des creux (revue v2.19.0)
// ---------------------------------------------------------------------------
// Alimenté par heapRingSample() toutes les 2 s avec une valeur DÉJÀ mesurée :
// aucune mesure supplémentaire, aucun coût. Atomiques car la lecture (loopTask,
// trace de calibrage) et l'écriture (loopTask également aujourd'hui) doivent
// rester sûres si l'échantillonnage migrait un jour vers une autre tâche.
static std::atomic<uint32_t> sMinMaxAlloc8SinceBoot{UINT32_MAX};
static std::atomic<uint32_t> sMinMaxAlloc8Window{UINT32_MAX};

// `fetch_min` atomique : la RÈGLE vit dans heapAtomicFetchMin (heap_caps_logic.h,
// module PUR — ADR-0017), pour qu'un test natif puisse l'entrelacer avec un
// second écrivain scripté. Ici, seule reste la liaison à std::atomic et à
// l'ordre mémoire.
// POURQUOI PAS UN `load`-`compare`-`store` (condition 1 du gate pool-chemistry) :
// ce plancher est désormais alimenté par TROIS écrivains — la loop, le filtre de
// la garde 503 sur `async_tcp` (web_server.cpp) et la garde d'ouverture de
// fichiers depuis n'importe quelle tâche (fs_guard.cpp). Avec deux écrivains, un
// load-compare-store PERD un minimum au lieu de le reporter (A charge 30 Ko, B
// charge 30 Ko, A stocke 12 Ko, B stocke 20 Ko → le creux de 12 Ko a disparu).
// `relaxed` : ces planchers ne publient aucune donnée à travers eux (aucune
// dépendance mémoire à établir), et la boucle ne coûte un tour de plus que sous
// contention réelle.
static void heapFetchMinRelaxed(std::atomic<uint32_t>& target, uint32_t value) {
  heapAtomicFetchMin(target, value, std::memory_order_relaxed);
}


uint32_t heapMinMaxAlloc8SinceBoot() {
  return sMinMaxAlloc8SinceBoot.load(std::memory_order_relaxed);
}

uint32_t heapTakeMinMaxAlloc8Window() {
  // exchange : la fenêtre repart à vide pour la prochaine période de trace.
  // Course bénigne et documentée (revue) : un échantillon chargé juste avant
  // l'exchange et rangé juste après n'est PAS perdu — il est attribué à la
  // fenêtre SUIVANTE. Sans effet à l'échelle d'une fenêtre de 5 min
  // (≈ 150 échantillons à 2 s).
  return sMinMaxAlloc8Window.exchange(UINT32_MAX, std::memory_order_relaxed);
}

// ===========================================================================
// PLANCHERS GLISSANTS MULTI-MASQUES (feature-122, lot C-1a) — MESURE SEULE
// ===========================================================================
// Aucune garde ne lit ce qui suit : le refus 503 (web_server.cpp), la garde
// d'ouverture de fichiers (fs_guard.cpp) et le parachute OOM restent sur 0x0804
// et sur kStaticFileMinAllocBytes. Aucune réserve DMA n'est posée ici — c'est
// feature-123, avec son ADR.

// Les littéraux du module pur DOIVENT valoir exactement les combinaisons
// MALLOC_CAP_* de l'IDF : le seul point de contact entre le pur et l'IDF est
// épinglé ici, de sorte qu'un changement de bit casse la COMPILATION du
// firmware plutôt qu'un test natif silencieusement devenu faux (même idiome que
// kOomProbeInternalCapBit ci-dessus).
static_assert(kHeapMaskInternal8 == (uint32_t)(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
              "kHeapMaskInternal8 doit valoir MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT (0x0804) : "
              "c'est la metrique de reference des gardes du projet (ADR-0058).");
static_assert(kHeapMaskInternalDma8 ==
                  (uint32_t)(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT),
              "kHeapMaskInternalDma8 doit valoir INTERNAL|DMA|8BIT (0x080C) : c'est la "
              "signature exacte des allocations qui echouent en production (bug-080).");
static_assert(kHeapMaskDefault == (uint32_t)MALLOC_CAP_DEFAULT,
              "kHeapMaskDefault doit valoir MALLOC_CAP_DEFAULT (0x1000).");
static_assert(kHeapMaskInternal8Default ==
                  (uint32_t)(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT | MALLOC_CAP_DEFAULT),
              "kHeapMaskInternal8Default doit valoir INTERNAL|8BIT|DEFAULT (0x1804).");
static_assert(kHeapMaskSpiram == (uint32_t)MALLOC_CAP_SPIRAM,
              "kHeapMaskSpiram doit valoir MALLOC_CAP_SPIRAM (0x0400).");

// Ordre des emplacements = ordre des bits publiés (cf. kHeapFloorSlot*).
static constexpr uint32_t kFloorMasks[kHeapFloorMaskCount] = {
    kHeapMaskInternal8, kHeapMaskInternalDma8, kHeapMaskDefault,
    kHeapMaskInternal8Default, kHeapMaskSpiram};

// TOUR DE RÔLE : les masques à parcours O(blocs) AUTRES que la référence. Un
// seul est mesuré par échantillon → chacun toutes les 4 s à la cadence de 2 s.
static constexpr uint8_t kFloorWalkRing[] = {kHeapFloorSlotInternalDma8,
                                             kHeapFloorSlotInternal8Default};
static constexpr uint8_t kFloorWalkRingSize =
    (uint8_t)(sizeof(kFloorWalkRing) / sizeof(kFloorWalkRing[0]));
// Le budget déclaré = la référence 0x0804 + UN SEUL masque du tour de rôle, quel
// que soit le nombre de masques dans l'anneau (celui-ci ne fixe que la période
// de revisite : kFloorWalkRingSize × kHeapRingSampleIntervalMs, soit 4 s ici).
// Mesurer DEUX masques du tour de rôle par échantillon violerait le plafond du
// gate pool-chemistry (1 parcours O(blocs) par seconde) : c'est ce que cet
// assert interdit d'introduire par inadvertance.
static_assert(kHeapFloorWalksPerSample == 2,
              "Budget attendu : la reference 0x0804 PLUS un seul masque du tour de "
              "role par echantillon. Ne pas augmenter sans repasser le gate.");
static_assert(kFloorWalkRingSize >= 1 && kFloorWalkRingSize <= kHeapFloorMaskCount,
              "L'anneau du tour de role doit contenir au moins un masque et rester "
              "dans les emplacements declares.");

// Masques à chemin O(régions) (aucun bit INTERNAL → heapProbeForCaps prend les
// agrégats et déclare le plus gros bloc « non renseigné »). Coût sans parcours
// de blocs : mesurés à CHAQUE échantillon, donc à pleine résolution temporelle.
static constexpr uint8_t kFloorRegionSlots[] = {kHeapFloorSlotDefault,
                                                kHeapFloorSlotSpiram};
static_assert((kHeapMaskDefault & kOomProbeInternalCapBit) == 0u &&
                  (kHeapMaskSpiram & kOomProbeInternalCapBit) == 0u,
              "Les deux masques hors budget doivent etre ceux qu'oomProbeUsesRegionScan "
              "route vers les agregats : sans bit INTERNAL, pas de parcours O(blocs).");
static_assert((kHeapMaskInternal8 & kOomProbeInternalCapBit) != 0u &&
                  (kHeapMaskInternalDma8 & kOomProbeInternalCapBit) != 0u &&
                  (kHeapMaskInternal8Default & kOomProbeInternalCapBit) != 0u,
              "Les trois masques internes doivent bien couter un parcours O(blocs) : "
              "c'est ce qui justifie le tour de role.");

// Les trois tas de la dérivation, dans l'ordre des bits de HeapFloors::pred.
static constexpr uint32_t kPredHeapAllCaps[3] = {
    kHeapAllCapsInternalDram, kHeapAllCapsPsram, kHeapAllCapsDmaReserve};
static constexpr uint8_t kPredMaskCount = 4;  // les 4 masques de la table (hors SPIRAM)

// La table de sélection, réduite à 12 bits : bit (masque × 3 + tas).
// C'est la DÉRIVATION de la spec, calculée par la règle de l'IDF elle-même.
static constexpr uint16_t heapFloorsPredMask() {
  uint16_t bits = 0;
  for (uint8_t m = 0; m < kPredMaskCount; m++) {
    for (uint8_t h = 0; h < 3; h++) {
      if (heapMaskSelectsHeap(kPredHeapAllCaps[h], kFloorMasks[m])) {
        bits = (uint16_t)(bits | (uint16_t)(1u << (m * 3 + h)));
      }
    }
  }
  return bits;
}
// Valeur attendue = la table de feature-122, bit par bit :
//   0x0804 → DRAM oui, PSRAM non, réserve OUI  (bits 0, 2)
//   0x080C → DRAM oui, PSRAM non, réserve oui  (bits 3, 5)
//   0x1000 → DRAM oui, PSRAM OUI, réserve non  (bits 6, 7)
//   0x1804 → DRAM oui, PSRAM non, réserve non  (bit 9)
// Soit 0b0010'1110'1101 = 0x02ED. Ce static_assert est le garde-fou de la
// dérivation : remplacer `== caps` par `!= 0`, ou 0x1804 par 0x0804, casse la
// compilation.
static_assert(heapFloorsPredMask() == 0x02EDu,
              "La table de selection derivee a change : verifier heapMaskSelectsHeap, "
              "les masques et les all_caps AVANT de toucher a cette valeur.");

// Un emplacement de plancher. Écrivain UNIQUE : la loop (heapFloorsSample /
// heapFloorsRearmWindow). Lecteurs : la loop (trace agrégée) et le contexte web
// async (/get-system-info) — d'où les atomiques relâchées, qui suffisent : un
// lecteur ne peut voir qu'un échantillon voisin, jamais une valeur déchirée.
// Le load-compare-store est ici LÉGITIME (contrairement au plancher 0x0804
// historique, cf. heapAtomicFetchMin) parce qu'il n'y a qu'un écrivain.
namespace {
struct HeapFloorAtomics {
  std::atomic<uint32_t> lastFree{0};
  std::atomic<uint32_t> lastLargest{0};
  std::atomic<uint32_t> minFreeBoot{kHeapFloorEmpty};
  std::atomic<uint32_t> minLargestBoot{kHeapFloorEmpty};
  std::atomic<uint32_t> minFreeWindow{kHeapFloorEmpty};
  std::atomic<uint32_t> minLargestWindow{kHeapFloorEmpty};
  std::atomic<uint32_t> ctxAtMinFree{0};
  std::atomic<uint32_t> ctxAtMinLargest{0};
  std::atomic<uint32_t> samples{0};
};
HeapFloorAtomics sFloors[kHeapFloorMaskCount];
std::atomic<uint32_t> sFloorSampleCount{0};
std::atomic<uint32_t> sFloorCtxSeen{0};
// Dernier contexte composé par heapRingSample. La poussée FINE s'exécute sur
// `async_tcp` et ne peut pas composer le contexte elle-même (ce serait lire
// wsManager, PumpController et MQTT depuis async_tcp) : elle réutilise donc le
// dernier connu, à 2 s près. Approximation ASSUMÉE et bornée — sans elle, un
// creux de requête serait enregistré sans aucun contexte.
std::atomic<uint32_t> sFloorCtxLast{0};
// Jamais réarmé, à l'inverse de sFloorCtxSeen : cf. le commentaire de
// heapFloorsRead(). Le réarmement de fenêtre NE DOIT PAS le toucher.
std::atomic<uint32_t> sFloorCtxSeenBoot{0};
std::atomic<uint32_t> sHttpGetCount{0};
std::atomic<uint32_t> sWitnessBits{0};
// Chronométrage du parcours de référence 0x0804 (condition 5 du gate) :
// pire cas + histogramme dyadique pour la médiane. Écrivain UNIQUE (loop), donc
// load-compare-store légitime sur le max. 7 × 4 o = 28 o de compteurs.
std::atomic<uint32_t> sWalkMaxUs{0};
std::atomic<uint32_t> sWalkBuckets[kHeapWalkBucketCount] = {};

// Applique un plancher (depuis-boot et fenêtre) à travers la partie pure.
void heapFloorApply(std::atomic<uint32_t>& floorAtomic, std::atomic<uint32_t>* ctxAtomic,
                    uint32_t sampleBytes, uint8_t ctx) {
  HeapFloorSlot slot;
  slot.minBytes = floorAtomic.load(std::memory_order_relaxed);
  slot.ctxAtMin = ctxAtomic ? (uint8_t)ctxAtomic->load(std::memory_order_relaxed) : 0u;
  if (!heapFloorUpdate(slot, sampleBytes, ctx)) return;
  floorAtomic.store(slot.minBytes, std::memory_order_relaxed);
  if (ctxAtomic) ctxAtomic->store(slot.ctxAtMin, std::memory_order_relaxed);
}

// Range un échantillon dans l'emplacement d'un masque.
// Poussée FINE : seul le plus gros bloc est connu (aucun parcours pour le libre).
// Sans cette voie, les planchers par masque rateraient les creux de requête —
// défaut du 08/09/2026, cf. heapNoteMaxAlloc8Sample.
void heapFloorNoteFineLargest(uint8_t slot, uint32_t largest, uint8_t ctx) {
  if (slot >= kHeapFloorMaskCount) return;
  if (largest == kOomProbeBlockUnknown) return;
  HeapFloorAtomics& f = sFloors[slot];
  heapFloorApply(f.minLargestBoot, &f.ctxAtMinLargest, largest, ctx);
  heapFloorApply(f.minLargestWindow, nullptr, largest, ctx);
}

void heapFloorNoteSlot(uint8_t slot, const HeapProbe& probe, uint8_t ctx) {
  HeapFloorAtomics& f = sFloors[slot];
  f.lastFree.store(probe.freeBytes, std::memory_order_relaxed);
  f.lastLargest.store(probe.largestBlockBytes, std::memory_order_relaxed);
  heapFloorApply(f.minFreeBoot, &f.ctxAtMinFree, probe.freeBytes, ctx);
  heapFloorApply(f.minFreeWindow, nullptr, probe.freeBytes, ctx);
  // « Plus gros bloc NON RENSEIGNÉ » (masques O(régions)) : on ne l'inscrit
  // JAMAIS dans un plancher — ce serait un bloc de 4 194 240 o présenté comme
  // réel, exactement le genre de chiffre faux et crédible que ce dossier
  // combat. Le plancher reste kHeapFloorEmpty pour ces masques.
  if (probe.largestBlockBytes != kOomProbeBlockUnknown) {
    heapFloorApply(f.minLargestBoot, &f.ctxAtMinLargest, probe.largestBlockBytes, ctx);
    heapFloorApply(f.minLargestWindow, nullptr, probe.largestBlockBytes, ctx);
  }
  f.samples.fetch_add(1, std::memory_order_relaxed);
}
}  // namespace

// ⚠️ DÉFAUT CORRIGÉ LE 08/09/2026, RÉVÉLÉ PAR L'EXPÉRIENCE ELLE-MÊME.
// Cette poussée est la mesure FINE : elle est prise au moment d'une requête
// (web_server.cpp, filtre de la garde 503) ou d'une ouverture de fichier
// (fs_guard.cpp), donc à l'instant exact où la ressource est disputée. Or elle
// n'alimentait QUE les deux compteurs historiques, et JAMAIS les planchers par
// masque de feature-122.
//
// Conséquence mesurée sur cible : après 117 requêtes d'interface provoquées
// volontairement, le journal annonçait `minBoot=4084` (l'ancien compteur, nourri
// ici) tandis que `hf.i8.mb` (le plancher par masque, nourri seulement par
// l'échantillonnage à 2 s) annonçait 7 924. Deux mesures du MÊME plancher,
// divergentes de près du double — et c'est la seconde qui devait dimensionner la
// réserve DMA de feature-123, le lot irréversible. Un instrument qui sous-estime
// de 94 % le creux qu'il sert à mesurer.
//
// Le plancher du masque 0x0804 reçoit donc désormais cette valeur. AUCUN parcours
// n'est ajouté : `maxAlloc8` est déjà mesuré par l'appelant, on ne fait qu'un
// fetch_min de plus (condition 3 du gate pool-chemistry préservée).
//
// ⚠️ CE QUI RESTE GROSSIER, et qu'il faut savoir en lisant : les planchers de
// 0x080C et 0x1804 ne peuvent PAS recevoir cette valeur — elle mesure 0x0804.
// Les mesurer ici exigerait deux parcours de plus par requête, sur `async_tcp`,
// pendant la famine : exactement ce que la condition 2 du gate interdit. Ils
// restent donc à la résolution du tour de rôle (8 s depuis la v2.49.1).
// Atténuation : heaps(0x080C) et heaps(0x1804) sont des SOUS-ENSEMBLES de
// heaps(0x0804), donc le plancher fin de 0x0804 les BORNE SUPÉRIEUREMENT. En cas
// de divergence, c'est lui qui fait foi, et il est du bon côté.
// ⚠️ `ctxSurPlace` — AJOUTÉ LE 08/09/2026 À 23:17, SUR UNE MESURE.
// La reproduction volontaire d'un épisode 503 a fait descendre le plancher fin à
// 2 932 o, et l'a enregistré avec `ctxAtMin = 0x00` : AUCUN régime identifié,
// alors que le creux s'est produit en pleine rafale HTTP. Le plancher grossier,
// lui, avait correctement noté 0x02. La cause était l'approximation du contexte
// hérité — la poussée fine réutilisait le dernier contexte composé par
// `heapRingSample`, vieux de 2 s au plus, et a donc hérité d'un 0x00 périmé
// PRÉCISÉMENT à l'événement qui compte. Un plancher sans son régime ne répond
// pas à la question posée (« quel événement produit le creux ? »), et c'était
// toute la raison d'être de cet octet.
//
// Le correctif ne mesure ni ne lit rien de neuf : l'appelant du chemin HTTP SAIT
// qu'il est une requête. Il pose donc ce bit lui-même, fusionné (OU) avec le
// contexte échantillonné — lequel reste utile pour les régimes que l'appelant ne
// peut pas connaître : dosage, découverte MQTT, clients WebSocket.
void heapNoteMaxAlloc8Sample(uint32_t maxAlloc8, uint8_t ctxSurPlace) {
  heapFetchMinRelaxed(sMinMaxAlloc8SinceBoot, maxAlloc8);
  heapFetchMinRelaxed(sMinMaxAlloc8Window, maxAlloc8);
  // Le plancher par masque 0x0804. `freeBytes` n'est pas mesuré ici (ce serait
  // un parcours de plus) : on ne renseigne QUE le plus gros bloc.
  const uint8_t ctx =
      (uint8_t)(sFloorCtxLast.load(std::memory_order_relaxed) | ctxSurPlace);
  // Le régime CERTAIN rejoint aussi les accumulateurs : sans cela, un creux
  // correctement attribué au plancher ne serait pas compté dans `ctx`/`ctxb`.
  if (ctxSurPlace != 0) {
    sFloorCtxSeen.fetch_or(ctxSurPlace, std::memory_order_relaxed);
    sFloorCtxSeenBoot.fetch_or(ctxSurPlace, std::memory_order_relaxed);
  }
  heapFloorNoteFineLargest(kHeapFloorSlotInternal8, maxAlloc8, ctx);
}

HeapProbe heapFloorsSample(uint8_t ctx) {
  // 1) Masque de RÉFÉRENCE 0x0804 : UN parcours, qui remplace les DEUX appels
  //    (largest + free) que faisait heapRingSample. La valeur est rendue à
  //    l'appelant pour qu'il ne mesure pas une deuxième fois.
  //    CHRONOMÉTRÉ (condition 5 du gate pool-chemistry) : le plafond de coût doit
  //    s'adosser à une durée OBSERVÉE, pas au seuil d'autolimitation d'une autre
  //    sonde. Horloge = esp_timer_get_time() et NON le compteur de cycles
  //    CCOUNT : celui-ci est PAR CŒUR, et une migration de tâche entre les deux
  //    lectures rendrait la soustraction absurde (piège déjà rencontré,
  //    kOomProbeDurationInvalid). Deux lectures d'un compteur matériel par
  //    échantillon : le masquage d'interruptions du parcours ne l'affecte pas.
  const int64_t walkStartUs = esp_timer_get_time();
  const HeapProbe reference = heapProbeForCaps(kHeapMaskInternal8);
  const uint32_t walkUs = (uint32_t)(esp_timer_get_time() - walkStartUs);
  const uint32_t walkMax = sWalkMaxUs.load(std::memory_order_relaxed);
  if (walkUs > walkMax) sWalkMaxUs.store(walkUs, std::memory_order_relaxed);
  sWalkBuckets[heapWalkBucketIndex(walkUs)].fetch_add(1, std::memory_order_relaxed);
  heapFloorNoteSlot(kHeapFloorSlotInternal8, reference, ctx);

  // 2) Le créneau libéré porte UN masque O(blocs) en tour de rôle — mais plus à
  //    chaque échantillon : UN SUR kHeapFloorWalkEverySamples.
  //    MOTIF (condition 5 du gate pool-chemistry, déclenchée par le relevé du
  //    08/09/2026) : la durée réelle d'un parcours a été mesurée à 516 µs au pire
  //    et médiane <= 512 µs, contre les ~84 µs sur lesquels le gate avait
  //    raisonné et les 300 µs de sa condition. Elle prescrivait alors de porter
  //    le tour de rôle à 4 s. C'est fait ici, et le coût moyen retombe à
  //    1,5 parcours par échantillon (0,75/s) — sous le plafond, avec de la marge
  //    pour la première fois.
  //    ⚠️ La résolution du plancher 0x080C passe donc de 4 à 8 s. C'est le
  //    chiffre qui dimensionnera feature-123 : il faut le savoir en le lisant.
  //    Statiques locales : écrivain unique (loop).
  static uint8_t sWalkCursor = 0;
  static uint8_t sWalkDivider = 0;
  if (sWalkCursor >= kFloorWalkRingSize) sWalkCursor = 0;  // RAM corrompue → repli
  if (sWalkDivider >= kHeapFloorWalkEverySamples) sWalkDivider = 0;  // idem
  const bool walkThisSample = heapFloorWalkDue(sWalkDivider);
  sWalkDivider = heapFloorWalkNextDivider(sWalkDivider, kHeapFloorWalkEverySamples);
  if (walkThisSample) {
    const uint8_t walkSlot = kFloorWalkRing[sWalkCursor];
    sWalkCursor = heapMaskRoundRobinNext(sWalkCursor, kFloorWalkRingSize);
    heapFloorNoteSlot(walkSlot, heapProbeForCaps(kFloorMasks[walkSlot]), ctx);
  }

  // 3) Les masques à chemin O(régions) (0x1000 et SPIRAM) : pas de parcours de
  //    blocs, donc hors budget — mesurés à chaque échantillon.
  //    NOTE A6 : ESP.getFreePsram() n'est rien d'autre que
  //    heap_caps_get_free_size(MALLOC_CAP_SPIRAM) ; le plancher SPIRAM ci-dessous
  //    est donc bien le plancher de `psram_free`, sans second appel
  //    interruptions coupées.
  for (uint8_t i = 0; i < (uint8_t)(sizeof(kFloorRegionSlots) / sizeof(kFloorRegionSlots[0]));
       i++) {
    const uint8_t slot = kFloorRegionSlots[i];
    heapFloorNoteSlot(slot, heapProbeForCaps(kFloorMasks[slot]), ctx);
  }

  sFloorCtxSeen.fetch_or(ctx, std::memory_order_relaxed);
  sFloorCtxLast.store(ctx, std::memory_order_relaxed);
  sFloorCtxSeenBoot.fetch_or(ctx, std::memory_order_relaxed);
  sFloorSampleCount.fetch_add(1, std::memory_order_relaxed);
  return reference;
}

HeapFloors heapFloorsRead() {
  // Condition 2 du gate pool-chemistry : QUE des lectures d'atomiques déjà
  // rangées. AUCUN appel à heapProbeForCaps ici — quatre parcours de tas par
  // requête HTTP, sur `async_tcp`, seraient posés sur le chemin même de la
  // famine que ce lot cherche à mesurer.
  HeapFloors out;
  for (uint8_t i = 0; i < kHeapFloorMaskCount; i++) {
    HeapFloorAtomics& f = sFloors[i];
    out.masks[i].mask = kFloorMasks[i];
    out.masks[i].lastFree = f.lastFree.load(std::memory_order_relaxed);
    out.masks[i].lastLargest = f.lastLargest.load(std::memory_order_relaxed);
    out.masks[i].minFreeSinceBoot = f.minFreeBoot.load(std::memory_order_relaxed);
    out.masks[i].minLargestSinceBoot = f.minLargestBoot.load(std::memory_order_relaxed);
    out.masks[i].minFreeWindow = f.minFreeWindow.load(std::memory_order_relaxed);
    out.masks[i].minLargestWindow = f.minLargestWindow.load(std::memory_order_relaxed);
    out.masks[i].ctxAtMinFree = (uint8_t)f.ctxAtMinFree.load(std::memory_order_relaxed);
    out.masks[i].ctxAtMinLargest = (uint8_t)f.ctxAtMinLargest.load(std::memory_order_relaxed);
    out.masks[i].samples = f.samples.load(std::memory_order_relaxed);
  }
  out.sampleCount = sFloorSampleCount.load(std::memory_order_relaxed);
  // Médiane approchée : copie locale de l'histogramme (7 mots de pile) puis
  // repliement par la fonction PURE. Aucun tri, aucune allocation, aucun
  // flottant — et aucun parcours de tas (condition 2 du gate).
  uint32_t buckets[kHeapWalkBucketCount];
  for (uint8_t i = 0; i < kHeapWalkBucketCount; i++) {
    buckets[i] = sWalkBuckets[i].load(std::memory_order_relaxed);
  }
  out.walkMaxUs = sWalkMaxUs.load(std::memory_order_relaxed);
  out.walkMedianUs = heapWalkMedianUpperUs(buckets, kHeapWalkBucketCount);
  out.pred = heapFloorsPredMask();
  out.ctxSeen = (uint8_t)sFloorCtxSeen.load(std::memory_order_relaxed);
  // ⚠️ DEUX accumulateurs, et non un — relevé du 08/09/2026. `ctxSeen` est remis
  // à zéro par heapFloorsRearmWindow() (donc toutes les 30 min), ce que son nom
  // ne disait pas : lu seul, il annonçait « un seul bit jamais vu en 8 h 49 »
  // alors qu'il ne portait que la fenêtre courante. `ctxSeenBoot` ne se réarme
  // JAMAIS, et c'est lui qui répond à « quels régimes cet appareil a-t-il
  // connus depuis son démarrage ».
  out.ctxSeenBoot = (uint8_t)sFloorCtxSeenBoot.load(std::memory_order_relaxed);
  out.witness = (uint8_t)sWitnessBits.load(std::memory_order_relaxed);
  return out;
}

void heapFloorsRearmWindow() {
  for (uint8_t i = 0; i < kHeapFloorMaskCount; i++) {
    sFloors[i].minFreeWindow.store(kHeapFloorEmpty, std::memory_order_relaxed);
    sFloors[i].minLargestWindow.store(kHeapFloorEmpty, std::memory_order_relaxed);
  }
  sFloorCtxSeen.store(0, std::memory_order_relaxed);
}

void heapNoteHttpGetSample(uint32_t maxAlloc8) {
  // Site `async_tcp` : la valeur est DÉJÀ mesurée par le filtre de la garde 503,
  // on ne parcourt rien. Un fetch_min (jamais un store) + un compteur.
  // Le bit de rafale HTTP est posé SUR PLACE : ici ce n'est pas une hypothèse
  // échantillonnée mais une certitude — ce site EST le filtre de la garde 503.
  heapNoteMaxAlloc8Sample(maxAlloc8, kHeapCtxHttpBurst);
  sHttpGetCount.fetch_add(1, std::memory_order_relaxed);
}

uint32_t heapHttpGetCount() {
  return sHttpGetCount.load(std::memory_order_relaxed);
}

void heapWitnessRecord(uint8_t bits) {
  sWitnessBits.store(bits, std::memory_order_relaxed);
}

uint8_t heapWitnessBits() {
  return (uint8_t)sWitnessBits.load(std::memory_order_relaxed);
}
