#ifndef HEAP_CAPS_LOGIC_H
#define HEAP_CAPS_LOGIC_H

#include <stddef.h>
#include <stdint.h>

// =============================================================================
// heap_caps_logic — module PUR de la métrique multi-masques (feature-122, C-1a)
// =============================================================================
// ADR-0017 : aucune dépendance Arduino / FreeRTOS / FS / esp_heap_caps ici. Ce
// fichier ne contient QUE de l'arithmétique testable en natif ; la coquille
// impure vit dans heap_metrics.cpp (planchers, atomiques) et heap_ring.cpp
// (cadence, composition du contexte).
//
// CE LOT MESURE, IL NE CORRIGE RIEN. Aucune garde du projet ne lit ce fichier :
// le refus 503 des fichiers statiques (web_server.cpp), la garde d'ouverture de
// fichiers (fs_guard.cpp) et le parachute OOM restent sur 0x0804 et sur
// kStaticFileMinAllocBytes. Le choix éventuel d'une autre métrique de garde
// appartient au lot C-1b (feature-123), avec son ADR.
//
// POURQUOI QUATRE MASQUES (bug-080, lot C) : les allocations qui échouent en
// production portent caps = 0x080C = 8BIT|DMA|INTERNAL, tandis que TOUTES les
// gardes du projet mesurent 0x0804. Le moteur DMA de l'ESP32 ne sait pas
// adresser la PSRAM : ces octets ne peuvent structurellement jamais y aller. Il
// faut donc voir les quatre pools en parallèle, et le plus petit d'entre eux,
// avant de décider quoi que ce soit.
// =============================================================================

// ---------------------------------------------------------------------------
// Les quatre masques suivis + le masque PSRAM (A6)
// ---------------------------------------------------------------------------
// Valeurs LITTÉRALES et non MALLOC_CAP_* : ce fichier doit compiler en natif,
// sans esp_heap_caps.h. L'égalité avec les constantes de l'IDF est épinglée par
// static_assert dans heap_metrics.cpp — si l'IDF changeait un bit, c'est la
// COMPILATION du firmware qui casse, pas un test natif devenu silencieusement
// faux (même idiome que kOomProbeInternalCapBit).
constexpr uint32_t kHeapMaskInternal8 = 0x0804u;         // INTERNAL|8BIT — métrique ACTUELLE des gardes
constexpr uint32_t kHeapMaskInternalDma8 = 0x080Cu;      // INTERNAL|DMA|8BIT — ce que le WiFi demande
constexpr uint32_t kHeapMaskDefault = 0x1000u;           // DEFAULT seul — voit AUSSI la PSRAM
constexpr uint32_t kHeapMaskInternal8Default = 0x1804u;  // INTERNAL|8BIT|DEFAULT — candidate C-1b
constexpr uint32_t kHeapMaskSpiram = 0x0400u;            // SPIRAM — plancher PSRAM (A6)

// ---------------------------------------------------------------------------
// all_caps des trois tas, tels que l'IDF les enregistre
// ---------------------------------------------------------------------------
// DRAM interne : DEFAULT|INTERNAL|DMA|8BIT|32BIT.
constexpr uint32_t kHeapAllCapsInternalDram = 0x180Eu;
// PSRAM : DEFAULT|SPIRAM|8BIT|32BIT (pas d'INTERNAL — MALLOC_CAP_INTERNAL est
// DÉFINI comme « ne disparaît pas quand le cache flash/spiram est coupé », ce
// que la PSRAM ne peut structurellement pas satisfaire).
constexpr uint32_t kHeapAllCapsPsram = 0x1406u;
// Réserve DMA telle que la ré-enregistrerait esp_psram_extram_reserve_dma_pool :
// caps[] = {0, DMA|INTERNAL, 8BIT|32BIT} → INTERNAL|DMA|8BIT|32BIT, SANS DEFAULT.
// ⚠️ HYPOTHÈSE (source IDF, corroborée par le désassemblage) et NON une mesure :
// aucune réserve n'est posée par ce lot. C'est la PRÉDICTION que feature-123
// devra confronter aux planchers relevés.
constexpr uint32_t kHeapAllCapsDmaReserve = 0x080Eu;

/**
 * @brief Un masque de capacités sélectionne-t-il ce tas ?
 *
 * Recopie la règle d'heap_caps_match() de l'IDF : `(all_caps & caps) == caps`.
 * C'est la DÉRIVATION centrale de feature-122, mise sous test (12 cas = 4
 * masques × 3 tas).
 *
 * ⚠️ `== caps` et JAMAIS `!= 0` : avec `!= 0`, un masque portant INTERNAL
 * sélectionnerait le tas PSRAM (bit 8BIT commun) et la table entière
 * s'effondrerait — c'est exactement le défaut qui a rendu quatre gardes
 * aveugles (ADR-0058).
 *
 * @param heapAllCaps Capacités agrégées du tas (kHeapAllCaps*).
 * @param mask        Masque demandé (kHeapMask*).
 * @return true si une allocation portant `mask` peut puiser dans ce tas.
 */
// `constexpr` (et non simplement `inline`) pour que la table de sélection entière
// puisse être épinglée par static_assert côté firmware : une réécriture de la
// règle en `!= 0` casserait alors la COMPILATION, pas seulement un test.
constexpr bool heapMaskSelectsHeap(uint32_t heapAllCaps, uint32_t mask) {
  return (heapAllCaps & mask) == mask;
}

// Plancher « vide » (aucun échantillon) — la valeur neutre d'un minimum.
constexpr uint32_t kHeapFloorEmpty = 0xFFFFFFFFu;

/**
 * @brief Plancher glissant : un minimum + le contexte qui l'a produit.
 *
 * Le contexte est OBSERVATIONNEL (A5) : il dit QUEL événement tenait la machine
 * quand le creux a été atteint. Il n'est JAMAIS relu par une décision.
 */
struct HeapFloorSlot {
  uint32_t minBytes;  // kHeapFloorEmpty si aucun échantillon
  uint8_t ctxAtMin;   // bits kHeapCtx* observés au moment du minimum
};

/**
 * @brief Applique un échantillon à un plancher glissant.
 *
 * ⚠️ UN MINIMUM N'EST JAMAIS OUBLIÉ : c'est un `min`, pas un instantané. Le cas
 * limite de la spec l'exige (« Le minimum doit être retenu par un plancher
 * glissant, jamais par un instantané ») — un échantillonnage à cadence fixe
 * rate par construction une pointe de quelques millisecondes, seul le
 * repliement par minimum la conserve.
 *
 * @param slot        Plancher à mettre à jour (in/out).
 * @param sampleBytes Valeur mesurée.
 * @param ctx         Contexte observé pour cet échantillon.
 * @return true si le plancher a changé (nouveau creux).
 */
inline bool heapFloorUpdate(HeapFloorSlot& slot, uint32_t sampleBytes, uint8_t ctx) {
  if (sampleBytes >= slot.minBytes) return false;
  slot.minBytes = sampleBytes;
  slot.ctxAtMin = ctx;
  return true;
}

/**
 * @brief `fetch_min` atomique — il n'existe pas dans <atomic>, on le compose.
 *
 * ⚠️ POURQUOI CE N'EST PAS UN `load`-`compare`-`store` (condition 1 du gate
 * pool-chemistry, feature-122) : le plancher 0x0804 avait UN SEUL écrivain
 * (loopTask) jusqu'à ce lot. Il en a maintenant TROIS — le filtre de la garde
 * 503 sur `async_tcp` (web_server.cpp), la garde d'ouverture de fichiers depuis
 * n'importe quelle tâche (fs_guard.cpp) et la loop. Avec deux écrivains, un
 * `load`-`compare`-`store` PERD un minimum au lieu de le reporter : A charge
 * 30 Ko, B charge 30 Ko, A stocke 12 Ko, B stocke 20 Ko → le creux de 12 Ko a
 * disparu. Le cas limite de la spec l'interdit explicitement (« Le minimum doit
 * être retenu par un plancher glissant, jamais par un instantané »).
 *
 * L'ORDRE MÉMOIRE EST UN PARAMÈTRE DE PATRON, et le type atomique aussi, pour
 * que ce fichier n'inclue PAS <atomic> (ADR-0017 : en-têtes C uniquement) et
 * pour que les tests natifs puissent lui passer un atomique SCRIPTÉ qui joue le
 * second écrivain à un point d'entrelacement choisi. C'est ce qui rend la course
 * testable sans fil d'exécution et sans réimplémenter la règle dans un modèle
 * parallèle (précédent à ne pas répéter : oomProbeTakeTokenModel).
 *
 * @param target Atomique à abaisser (in/out).
 * @param value  Échantillon proposé.
 * @param order  Ordre mémoire (relaxed côté firmware : ces planchers ne
 *               publient aucune donnée à travers eux).
 */
template <typename AtomicU32, typename MemoryOrder>
inline void heapAtomicFetchMin(AtomicU32& target, uint32_t value, MemoryOrder order) {
  uint32_t current = target.load(order);
  while (value < current) {
    // compare_exchange_weak rafraîchit `current` en cas d'échec : la condition de
    // boucle est réévaluée, donc un concurrent AYANT DÉJÀ posé un minimum plus
    // bas nous fait sortir sans l'écraser.
    if (target.compare_exchange_weak(current, value, order, order)) return;
  }
}

/**
 * @brief Un pointeur tombe-t-il dans la fenêtre d'adressage de la RAM externe ?
 *
 * Part PURE du témoin 4096/4095 (A7) : c'est la seule preuve DIRECTE du camp où
 * atterrit une allocation ordinaire, `psram_free` n'étant qu'un indice (152
 * relevés le montrent immobile — ce qui peut aussi bien signifier « rien ne part
 * en PSRAM » que « l'indicateur ne le voit pas »).
 *
 * Bornes passées par l'appelant et épinglées sur SOC_EXTRAM_DATA_LOW/HIGH par
 * static_assert côté firmware : ce fichier ne connaît aucun registre.
 *
 * @param p    Adresse à classer (0 → false, une allocation refusée n'est pas
 *             « interne »).
 * @param low  Première adresse de la fenêtre externe (incluse).
 * @param high Première adresse au-delà de la fenêtre (exclue).
 */
inline bool heapPtrIsExternal(uintptr_t p, uintptr_t low, uintptr_t high) {
  if (p == 0u) return false;
  return (p >= low) && (p < high);
}

/**
 * @brief Curseur suivant du tour de rôle des masques à parcours O(blocs).
 *
 * PLAFOND DE COÛT (condition 3 du gate pool-chemistry) : un seul créneau
 * O(blocs) est libre par échantillon, donc un seul masque supplémentaire est
 * mesuré par tour — pas deux. Le tour de rôle est ce qui rend le coût NET NUL
 * par rapport à l'état antérieur (deux parcours 0x0804 remplacés par un seul).
 * Ne jamais l'accélérer : `heap_caps_*` s'exécute interruptions coupées.
 *
 * Robuste à un curseur hors bornes (RAM corrompue) : retombe sur 0.
 */
inline uint8_t heapMaskRoundRobinNext(uint8_t cursor, uint8_t ringSize) {
  if (ringSize == 0u) return 0u;
  const uint8_t next = (uint8_t)(cursor + 1u);
  return (next >= ringSize) ? 0u : next;
}

// ---------------------------------------------------------------------------
// DURÉE D'UN PARCOURS (condition 5 du gate pool-chemistry)
// ---------------------------------------------------------------------------
// Le plafond « 1 parcours O(blocs) par seconde » était adossé à un SEUIL
// d'autolimitation (kOomProbeWarnDurationUs = 300 µs), pas à une borne
// OBSERVÉE. On chronomètre donc le parcours de référence 0x0804 de
// l'échantillonnage de planchers — grandeur DISTINCTE d'`oom_probe_max_us`, qui
// mesure la sonde du chemin d'échec, dans un état de tas et une fréquence tout
// autres. Repère attendu : ~84 µs (docs/subsystems/oom-guard.md).
//
// MÉDIANE SANS TRI, SANS ALLOCATION, SANS FLOTTANT : histogramme à 7 seaux
// dyadiques. La valeur rapportée est la BORNE SUPÉRIEURE du seau qui contient la
// médiane — donc une SURESTIMATION bornée par la largeur du seau (biais
// conservateur, du bon côté pour un plafond de coût). Autour de 84 µs, le seau
// est [64, 128) et la médiane est rapportée à 128 µs : à comparer aux 300 µs du
// gate en gardant ce biais en tête.
constexpr uint32_t kHeapWalkOverflowUs = 65535u;  // « ≥ 1024 µs, hors échelle »
constexpr uint8_t kHeapWalkBucketCount = 7;
constexpr uint32_t kHeapWalkBucketUpperUs[kHeapWalkBucketCount] = {
    32u, 64u, 128u, 256u, 512u, 1024u, kHeapWalkOverflowUs};

/** @brief Seau d'une durée mesurée (µs). Le dernier seau capte tout le reste. */
inline uint8_t heapWalkBucketIndex(uint32_t durationUs) {
  for (uint8_t i = 0; i + 1 < kHeapWalkBucketCount; i++) {
    if (durationUs < kHeapWalkBucketUpperUs[i]) return i;
  }
  return (uint8_t)(kHeapWalkBucketCount - 1);
}

/**
 * @brief Médiane approchée d'un histogramme, rendue comme borne supérieure de
 *        seau (kHeapWalkOverflowUs si la médiane tombe hors échelle).
 * @return 0 si aucun échantillon.
 */
inline uint32_t heapWalkMedianUpperUs(const uint32_t* counts, uint8_t bucketCount) {
  if (counts == nullptr || bucketCount == 0u) return 0u;
  uint32_t total = 0;
  for (uint8_t i = 0; i < bucketCount; i++) total += counts[i];
  if (total == 0u) return 0u;
  const uint32_t target = (total + 1u) / 2u;  // rang médian, arrondi au-dessus
  uint32_t cumulated = 0;
  for (uint8_t i = 0; i < bucketCount; i++) {
    cumulated += counts[i];
    if (cumulated >= target) return kHeapWalkBucketUpperUs[i];
  }
  return kHeapWalkOverflowUs;  // inatteignable (cumulated == total ≥ target)
}

// ---------------------------------------------------------------------------
// Bits de CONTEXTE (A5) — purement observationnels
// ---------------------------------------------------------------------------
// Sans eux, les planchers ne diraient pas QUEL événement les a produits, et le
// dimensionnement d'une réserve (C-1b) reposerait sur un minimum anonyme.
// ⚠️ AUCUNE décision ne lit ces bits — ni dosage, ni reboot, ni refus 503.
// ---------------------------------------------------------------------------
// Diviseur de cadence du tour de rôle (condition 5 du gate pool-chemistry)
// ---------------------------------------------------------------------------
// Le masque supplémentaire n'est mesuré qu'un échantillon sur `every`. Extrait en
// logique pure pour que la RÈGLE soit testable sans dépendre du texte du site
// d'appel : le 08/09/2026, une sentinelle purement textuelle a survécu au
// remplacement de `if (walkThisSample)` par `if (true)` — elle comptait des
// jetons encore présents.
inline bool heapFloorWalkDue(uint8_t divider) { return divider == 0; }

inline uint8_t heapFloorWalkNextDivider(uint8_t divider, uint8_t every) {
  if (every == 0) return 0;               // diviseur nul : on mesure toujours
  if (divider >= every) return 0;         // RAM corrompue → repli sur « mesurer »
  return (uint8_t)((divider + 1u) % every);
}

constexpr uint8_t kHeapCtxWsClients = 1u << 0;      // au moins un client WebSocket connecté
constexpr uint8_t kHeapCtxHttpBurst = 1u << 1;      // requêtes GET servies depuis l'échantillon précédent
constexpr uint8_t kHeapCtxDosing = 1u << 2;         // au moins une pompe doseuse sous tension
constexpr uint8_t kHeapCtxMqttDiscovery = 1u << 3;  // rafale de découverte HA en cours ou récente

/**
 * @brief La rafale de découverte MQTT est-elle « en cours ou récente » ?
 *
 * Une rafale republie ~60 entités et dure bien plus qu'un échantillon : la
 * fenêtre étend le marquage après le dernier repère posé, sinon un creux causé
 * par la rafale serait attribué au régime calme qui la suit.
 *
 * Arithmétique non signée : correcte au repliement de millis() (49,7 j).
 * SENTINELLE `lastMarkMs == 0` = « jamais marqué » : millis() ne vaut 0 que
 * pendant la première milliseconde du boot, où aucune rafale MQTT ne peut avoir
 * eu lieu (le WiFi n'est pas monté).
 */
inline bool heapCtxDiscoveryWindowActive(uint32_t nowMs, uint32_t lastMarkMs,
                                         uint32_t windowMs) {
  if (lastMarkMs == 0u) return false;
  return (uint32_t)(nowMs - lastMarkMs) < windowMs;
}

#endif  // HEAP_CAPS_LOGIC_H
