#ifndef MBEDTLS_PSRAM_ALLOC_H
#define MBEDTLS_PSRAM_ALLOC_H

#include <stdint.h>

// =============================================================================
// mbedtls_psram_alloc — fait obéir mbedtls à la politique PSRAM du projet
// (bug-165/bug-108, v2.81.0)
// =============================================================================
// CAUSE RACINE (établie par la mesure, bug-165) : mbedtls 3.6.5 FORCE ses
// allocations en RAM INTERNE (CONFIG_MBEDTLS_INTERNAL_MEM_ALLOC=1, sdkconfig
// des libs pioarduino précompilées — non modifiable sans recompiler mbedtls).
// Pendant la vérification d'une signature ECDSA P-384, maxAlloc8 s'effondre de
// 57 332 à 3 316 o, puis remonte à 47 092 o 13 ms après l'échec — confirmé
// indépendamment par `fs_guard` (même 3 316 o, même instant). Pendant ce temps,
// 4,17 Mo de PSRAM dorment (module WROVER-E, BOM PCB v2).
//
// REMÈDE : mbedtls expose `mbedtls_platform_set_calloc_free()` pour substituer
// SES fonctions calloc/free à l'exécution — sans recompiler la bibliothèque.
// On y pose calloc()/free() DE LA LIBC, qui obéissent DÉJÀ à la politique PSRAM
// du projet (heap_caps_malloc_extmem_enable(kPsramExtmemThresholdBytes), posée
// par main.cpp dans le bloc psramFound() — bug-080 B-1, feature-124) :
//   - allocation <= kPsramExtmemThresholdBytes (1 024 o) → RAM interne ;
//   - allocation  > kPsramExtmemThresholdBytes            → PSRAM D'ABORD, repli
//     RAM interne automatique si la PSRAM refuse.
// ⚠️ ZÉRO ligne d'allocateur à auditer dans ce fichier : calloc()/free() sont
// ceux de la libc, déjà passés en revue ailleurs. Ce module ne fait QUE poser
// le crochet et VÉRIFIER, par un témoin réel, qu'il n'est pas décoratif — le
// projet s'est déjà fait avoir une fois : le parachute anti-OOM était alloué en
// PSRAM, donc inopérant, sans que rien ne le signale avant une mesure directe
// (cf. kHeapWitness* de heap_metrics.h, même idiome repris ici).
//
// ⚠️ POSE UNIQUE, AU PLUS TÔT : à appeler UNE SEULE FOIS, dans le bloc
// `psramFound()` de `main.cpp` (juste après `heap_caps_malloc_extmem_enable`),
// AVANT toute utilisation de mbedtls — Wi-Fi compris. Aucune utilisation de
// mbedtls n'a lieu entre `main.cpp:173` (initOomGuard) et `setupWiFi()` : cet
// invariant, déjà vrai avant ce lot, est PRÉSERVÉ (ne pas le défaire).

// États observables de l'allocateur — exposés par `/get-system-info`.
constexpr uint8_t kMbedtlsPsramAllocAbsent = 0;      // pose jamais tentée OU refusée par mbedtls
constexpr uint8_t kMbedtlsPsramAllocActive = 1;      // crochet posé ET témoin confirmé en PSRAM
constexpr uint8_t kMbedtlsPsramAllocDecorative = 2;  // crochet posé MAIS témoin tombé en RAM interne (ANOMALIE)

/**
 * @brief Verdict PUR (ADR-0017, Humble Object) : classe l'état de
 *        l'allocateur à partir de DEUX booléens déjà observés, sans toucher au
 *        tas ni à mbedtls — testable nativement (test_native_oom_guard, T4).
 *
 * `mbedtlsPsramAllocInstall()` (impure, ci-dessous) se réduit à PRODUIRE ces
 * deux booléens — succès de la pose du crochet, et résultat du témoin réel —
 * puis délègue la classification ici. Aucune autre logique de décision
 * n'existe ailleurs : un test qui couvre cette fonction couvre tout le
 * classement à 3 états.
 *
 * @param hookInstalled true si `mbedtls_platform_set_calloc_free()` a réussi
 *                       (valeur de retour == 0).
 * @param probeInPsram  true si le témoin alloué VIA le pointeur mbedtls
 *                       (`mbedtls_calloc()`) a été vu en PSRAM
 *                       (`esp_ptr_external_ram()`). N'a de sens que si
 *                       `hookInstalled` est vrai ; sinon ignoré (Absent prime).
 * @return kMbedtlsPsramAllocAbsent / Active / Decorative.
 */
inline uint8_t mbedtlsPsramAllocVerdict(bool hookInstalled, bool probeInPsram) {
  if (!hookInstalled) return kMbedtlsPsramAllocAbsent;
  return probeInPsram ? kMbedtlsPsramAllocActive : kMbedtlsPsramAllocDecorative;
}

/**
 * @brief Pose le crochet `mbedtls_platform_set_calloc_free(calloc, free)` puis
 *        VÉRIFIE par un témoin réel qu'il n'est pas décoratif.
 *
 * Le témoin alloue 2 048 o (> kPsramExtmemThresholdBytes, donc attendu en
 * PSRAM) **via le pointeur mbedtls** (`mbedtls_calloc()`), jamais via
 * `calloc()` directement — sinon le témoin ne prouverait que la politique
 * d'allocation du projet, pas que mbedtls l'utilise RÉELLEMENT en pratique.
 * Libéré immédiatement (aucune rétention, aucun risque).
 *
 * Idempotent (garde interne non-réentrante) : un second appel est un no-op et
 * renvoie l'état déjà établi — aucune seconde pose, aucun second témoin.
 *
 * @return true si le crochet est posé ET actif (état kMbedtlsPsramAllocActive).
 *         false dans tous les autres cas (absence de PSRAM côté appelant à
 *         vérifier AVANT d'appeler cette fonction — psramFound() — pose
 *         refusée par mbedtls, ou témoin décoratif).
 */
bool mbedtlsPsramAllocInstall();

/**
 * @brief État courant de l'allocateur (kMbedtlsPsramAllocAbsent/Active/Decorative).
 * Lecture d'un atomique : ni allocation, ni log, ni parcours de tas.
 */
uint8_t mbedtlsPsramAllocatorState();

#endif  // MBEDTLS_PSRAM_ALLOC_H
