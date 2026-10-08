// =============================================================================
// Tests natifs — Détecteur PUR de CHUTE de contiguïté (feature-144, v2.60.0)
// src/heap_drop_logic.h :: HeapDropState / heapDropEvaluate()
// =============================================================================
// Tournent sur PC (env:native, Unity), HORS ESP32 / Arduino / FreeRTOS. Module
// PUR (ADR-0017) : aucune dépendance matérielle, l'appelant fournit tout
// (mesure déjà faite, horloge, état porté par valeur/référence).
//
// CE QUE CES TESTS PROUVENT : le comportement de la fonction heapDropEvaluate()
// elle-même — seuil, réarmement, anti-rafale, amplitude — pour toute séquence
// d'appels qu'on lui soumet.
//
// CE QU'ILS NE PROUVENT PAS : le câblage dans checkHeapHealth() (src/main.cpp,
// code impur non compilable en natif — il inclut Arduino.h/esp_heap_caps.h),
// la fraîcheur réelle de heapRingLastCtx()/heapFloorsRead() au moment de
// l'émission, ni le volume réel observé en production. Voir
// test_native_heap_telemetry_guard pour les sentinelles de structure sur
// main.cpp (D4), et le rapport de test-engineer pour le plan de test manuel
// (E1).
// =============================================================================

#include <unity.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "constants.h"
#include "heap_drop_logic.h"

void setUp(void) {}
void tearDown(void) {}

// -----------------------------------------------------------------------------
// 1 — Franchissement : chute de 18 432 depuis 45 044 (incident réel 08/10)
// -----------------------------------------------------------------------------
void test_franchissement_18432_depuis_45044(void) {
  HeapDropState st;
  // Premier tick : ancrage silencieux (invariant A5).
  HeapDropResult r0 = heapDropEvaluate(st, 45044u, 0u, kHeapDropFloorBytes,
                                        kHeapDropRefShift, kHeapDropMinIntervalMs);
  TEST_ASSERT_FALSE_MESSAGE(r0.emit, "Le premier tick ne doit JAMAIS émettre (invariant A5).");
  TEST_ASSERT_EQUAL_UINT32(45044u, st.ref);

  // Second tick, assez tard pour l'anti-rafale : chute réelle de l'incident.
  HeapDropResult r1 = heapDropEvaluate(st, 26612u, kHeapDropMinIntervalMs, kHeapDropFloorBytes,
                                        kHeapDropRefShift, kHeapDropMinIntervalMs);
  TEST_ASSERT_TRUE_MESSAGE(r1.emit, "Chute de 18432 (45044->26612, -41%) doit être signalée.");
  TEST_ASSERT_EQUAL_UINT32(45044u, r1.before);
  TEST_ASSERT_EQUAL_UINT32(26612u, r1.after);
  TEST_ASSERT_EQUAL_UINT32(18432u, r1.dropAmount);
  TEST_ASSERT_EQUAL_UINT32(26612u, st.ref);  // ré-ancrage à l'émission
}

// -----------------------------------------------------------------------------
// 2 — Non-franchissement : baisse sous le seuil ⇒ 0 ligne
// -----------------------------------------------------------------------------
void test_non_franchissement_sous_seuil(void) {
  HeapDropState st;
  heapDropEvaluate(st, 50000u, 0u, kHeapDropFloorBytes, kHeapDropRefShift,
                    kHeapDropMinIntervalMs);
  // seuil = max(4096, 50000>>2=12500) = 12500. Baisse de 10000 < seuil.
  HeapDropResult r = heapDropEvaluate(st, 40000u, kHeapDropMinIntervalMs, kHeapDropFloorBytes,
                                       kHeapDropRefShift, kHeapDropMinIntervalMs);
  TEST_ASSERT_FALSE_MESSAGE(r.emit, "Baisse de 10000 (< seuil 12500) ne doit PAS émettre.");
  // Silencieux : ref ne bouge pas tant que courant <= ref (pas de rearm vers le
  // bas, et courant(40000) < ref(50000) donc pas de rearm vers le haut non plus).
  TEST_ASSERT_EQUAL_UINT32(50000u, st.ref);
}

// -----------------------------------------------------------------------------
// 3 — Frontière exacte : courant + seuil == ref ⇒ déterministe (inclusif)
// -----------------------------------------------------------------------------
void test_frontiere_exacte_inclusive(void) {
  // ref=40000, seuil=max(4096,10000)=10000.
  {
    HeapDropState st;
    heapDropEvaluate(st, 40000u, 0u, kHeapDropFloorBytes, kHeapDropRefShift,
                      kHeapDropMinIntervalMs);
    // courant + seuil == ref exactement : 30000 + 10000 == 40000.
    HeapDropResult r = heapDropEvaluate(st, 30000u, kHeapDropMinIntervalMs, kHeapDropFloorBytes,
                                         kHeapDropRefShift, kHeapDropMinIntervalMs);
    TEST_ASSERT_TRUE_MESSAGE(r.emit,
        "courant + seuil == ref est un cas de FRANCHISSEMENT (comparaison <=, inclusive).");
    TEST_ASSERT_EQUAL_UINT32(10000u, r.dropAmount);
  }
  // Un cran au-dessus de la frontière (courant+1) : ne doit PAS franchir.
  {
    HeapDropState st;
    heapDropEvaluate(st, 40000u, 0u, kHeapDropFloorBytes, kHeapDropRefShift,
                      kHeapDropMinIntervalMs);
    HeapDropResult r = heapDropEvaluate(st, 30001u, kHeapDropMinIntervalMs, kHeapDropFloorBytes,
                                         kHeapDropRefShift, kHeapDropMinIntervalMs);
    TEST_ASSERT_FALSE_MESSAGE(r.emit,
        "courant + seuil == ref+1 (juste au-dessus de la frontière) ne doit PAS franchir.");
  }
}

// -----------------------------------------------------------------------------
// 4 — Plancher actif : sous ref=16384 (et en-dessous), le seuil vaut
// kHeapDropFloorBytes (4096) et non ref>>2 (qui serait plus bas, donc plus
// sensible — le plancher doit DOMINER, pas s'ajouter).
// -----------------------------------------------------------------------------
void test_plancher_actif_domine_le_seuil_relatif(void) {
  // ref=8192 : ref>>2 = 2048 (< plancher 4096) ⇒ seuil EFFECTIF = 4096.
  HeapDropState st;
  heapDropEvaluate(st, 8192u, 0u, kHeapDropFloorBytes, kHeapDropRefShift,
                    kHeapDropMinIntervalMs);
  // Baisse de 2200 (> 2048 = ref>>2 brut, mais < 4096 = plancher) : si le
  // plancher n'était pas appliqué, le seuil brut 2048 aurait laissé passer
  // cette chute (2200 > 2048). Avec le plancher, elle est EN-DESSOUS du
  // seuil réel (4096) et ne doit PAS émettre.
  HeapDropResult r = heapDropEvaluate(st, 5992u, kHeapDropMinIntervalMs, kHeapDropFloorBytes,
                                       kHeapDropRefShift, kHeapDropMinIntervalMs);
  TEST_ASSERT_FALSE_MESSAGE(r.emit,
      "MUTANT seuil-sans-plancher : une baisse de 2200 (> ref>>2=2048 mais < plancher 4096) "
      "ne doit PAS émettre quand le plancher est actif.");

  // Une baisse de exactement 4096 (= plancher) doit, elle, émettre.
  HeapDropState st2;
  heapDropEvaluate(st2, 8192u, 0u, kHeapDropFloorBytes, kHeapDropRefShift,
                    kHeapDropMinIntervalMs);
  HeapDropResult r2 = heapDropEvaluate(st2, 4096u, kHeapDropMinIntervalMs, kHeapDropFloorBytes,
                                        kHeapDropRefShift, kHeapDropMinIntervalMs);
  TEST_ASSERT_TRUE_MESSAGE(r2.emit, "Baisse de 4096 (= plancher) doit émettre sous ref=8192.");
}

// -----------------------------------------------------------------------------
// 5 — Asymétrie corrigée : 23 540 -> 16 372 (-30%), ratée par un seuil absolu
// de 8192 (diff réelle 7168 < 8192), attrapée par le seuil relatif retenu.
// Incident réel du 13/09 12:21 (captures pool-captures).
// -----------------------------------------------------------------------------
void test_asymetrie_corrigee_23540_vers_16372(void) {
  HeapDropState st;
  heapDropEvaluate(st, 23540u, 0u, kHeapDropFloorBytes, kHeapDropRefShift,
                    kHeapDropMinIntervalMs);
  HeapDropResult r = heapDropEvaluate(st, 16372u, kHeapDropMinIntervalMs, kHeapDropFloorBytes,
                                       kHeapDropRefShift, kHeapDropMinIntervalMs);
  const uint32_t diff = 23540u - 16372u;  // 7168
  TEST_ASSERT_TRUE_MESSAGE(diff < 8192u,
      "Pré-condition du test : la diff réelle (7168) doit être SOUS l'ancien seuil absolu "
      "8192, sinon ce test ne démontre pas l'asymétrie corrigée.");
  TEST_ASSERT_TRUE_MESSAGE(r.emit,
      "Chute de 30% (23540->16372) ratée par un seuil absolu de 8192 (diff 7168 < 8192) doit "
      "être ATTRAPÉE par le seuil relatif (ref>>2 = 5885).");
  TEST_ASSERT_EQUAL_UINT32(7168u, r.dropAmount);
}

// -----------------------------------------------------------------------------
// 6 — Plateau bas maintenu 20 ticks ⇒ exactement 1 ligne
// -----------------------------------------------------------------------------
void test_plateau_bas_20_ticks_une_seule_ligne(void) {
  HeapDropState st;
  heapDropEvaluate(st, 50000u, 0u, kHeapDropFloorBytes, kHeapDropRefShift,
                    kHeapDropMinIntervalMs);
  uint32_t nowMs = kHeapDropMinIntervalMs;
  HeapDropResult first = heapDropEvaluate(st, 10000u, nowMs, kHeapDropFloorBytes,
                                           kHeapDropRefShift, kHeapDropMinIntervalMs);
  TEST_ASSERT_TRUE(first.emit);
  int emits = 1;
  // 10 ticks rapprochés (10 s, << anti-rafale) : le plateau lui-même ne doit
  // rien redéclencher, pas seulement l'anti-rafale qui masquerait le défaut.
  for (int i = 0; i < 10; i++) {
    nowMs += 10000u;
    HeapDropResult r = heapDropEvaluate(st, 10000u, nowMs, kHeapDropFloorBytes,
                                         kHeapDropRefShift, kHeapDropMinIntervalMs);
    if (r.emit) emits++;
  }
  // MUTANT D3a (voir aussi test dédié) : 10 ticks SUPPLÉMENTAIRES espacés de
  // kHeapDropMinIntervalMs + 1 (juste au-delà de l'anti-rafale), plateau
  // TOUJOURS à la même valeur. Sans le ré-ancrage à l'émission
  // (`state.ref = courant`), `ref` serait restée bloquée à 50000 et CHAQUE
  // tick ici retrouverait la condition de franchissement (10000+seuil<=50000)
  // vraie, avec l'anti-rafale cette fois ÉCOULÉ à chaque tick : 10 émissions
  // de plus. Avec le code correct, `ref` vaut 10000 depuis la 1ère émission :
  // 10000 + seuil(4096, plancher) <= 10000 est FAUX, donc silence total.
  for (int i = 0; i < 10; i++) {
    nowMs += kHeapDropMinIntervalMs + 1u;
    HeapDropResult r = heapDropEvaluate(st, 10000u, nowMs, kHeapDropFloorBytes,
                                         kHeapDropRefShift, kHeapDropMinIntervalMs);
    if (r.emit) emits++;
  }
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, emits,
      "Un plateau bas maintenu ne doit produire qu'UNE SEULE ligne (à la chute initiale) — "
      "même sur une durée dépassant largement l'anti-rafale (MUTANT D3a : sans le ré-ancrage "
      "de `ref` à l'émission, ce plateau réémettrait toutes les 5 min indéfiniment).");
}

// -----------------------------------------------------------------------------
// 7 — Chute / remontée à 45044 / nouvelle chute ⇒ 2 lignes (scénario
// d'échec du cliquet nommé par la spec — LE test qui tue le mutant D3b).
// -----------------------------------------------------------------------------
void test_chute_remontee_nouvelle_chute_deux_lignes(void) {
  HeapDropState st;
  heapDropEvaluate(st, 45044u, 0u, kHeapDropFloorBytes, kHeapDropRefShift,
                    kHeapDropMinIntervalMs);
  uint32_t nowMs = kHeapDropMinIntervalMs;
  HeapDropResult r1 = heapDropEvaluate(st, 20000u, nowMs, kHeapDropFloorBytes, kHeapDropRefShift,
                                        kHeapDropMinIntervalMs);
  TEST_ASSERT_TRUE_MESSAGE(r1.emit, "Première chute doit émettre.");

  // Remontée complète au palier d'origine : réarmement SILENCIEUX (ref suit).
  nowMs += 10000u;
  HeapDropResult rUp = heapDropEvaluate(st, 45044u, nowMs, kHeapDropFloorBytes, kHeapDropRefShift,
                                         kHeapDropMinIntervalMs);
  TEST_ASSERT_FALSE_MESSAGE(rUp.emit, "La remontée elle-même ne doit JAMAIS émettre (arbitrage).");
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(45044u, st.ref,
      "MUTANT D3b : le réarmement silencieux (ref = max(ref, courant)) doit avoir recollé la "
      "référence au nouveau palier haut — sans lui, la seconde chute identique ne "
      "redéclencherait jamais (cliquet).");

  // Nouvelle chute, assez tard pour l'anti-rafale depuis la dernière ÉMISSION
  // (pas depuis la remontée, qui n'en est pas une).
  nowMs += kHeapDropMinIntervalMs;
  HeapDropResult r2 = heapDropEvaluate(st, 20000u, nowMs, kHeapDropFloorBytes, kHeapDropRefShift,
                                        kHeapDropMinIntervalMs);
  TEST_ASSERT_TRUE_MESSAGE(r2.emit,
      "MUTANT D3b (scénario nommé par la spec) : une seconde chute identique depuis le palier "
      "haut réarmé doit émettre une DEUXIÈME ligne — sans le réarmement silencieux, ref serait "
      "resté bloqué à 20000 et cette chute n'aurait produit AUCUNE ligne (cliquet).");
}

// -----------------------------------------------------------------------------
// 8 — Chute progressive sur 6 ticks ⇒ 1 ligne d'amplitude CUMULÉE depuis le
// palier d'origine (et non depuis le dernier pas intermédiaire).
// -----------------------------------------------------------------------------
void test_chute_progressive_amplitude_cumulee(void) {
  HeapDropState st;
  heapDropEvaluate(st, 50000u, 0u, kHeapDropFloorBytes, kHeapDropRefShift,
                    kHeapDropMinIntervalMs);
  // seuil tant que ref=50000 : max(4096, 12500) = 12500. Franchissement
  // requiert courant <= 37500. Les 5 premiers paliers restent AU-DESSUS (pas
  // de franchissement, pas de rearm car courant < ref) ; seul le dernier
  // franchit, avec l'amplitude CUMULÉE depuis 50000 (pas depuis 40000).
  const uint32_t paliers[] = {48000u, 46000u, 44000u, 42000u, 40000u, 37400u};
  uint32_t nowMs = 0;
  int emits = 0;
  HeapDropResult last;
  for (size_t i = 0; i < sizeof(paliers) / sizeof(paliers[0]); i++) {
    nowMs += kHeapDropMinIntervalMs;  // large marge anti-rafale à chaque pas
    HeapDropResult r = heapDropEvaluate(st, paliers[i], nowMs, kHeapDropFloorBytes,
                                         kHeapDropRefShift, kHeapDropMinIntervalMs);
    if (r.emit) {
      emits++;
      last = r;
    }
  }
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, emits, "Une seule émission attendue, au dernier palier.");
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(50000u, last.before,
      "L'amplitude doit être mesurée depuis le PALIER D'ORIGINE (50000), pas depuis le dernier "
      "pas intermédiaire (40000) : la référence ne doit pas glisser silencieusement vers le bas "
      "tant qu'aucun franchissement n'a eu lieu.");
  TEST_ASSERT_EQUAL_UINT32(37400u, last.after);
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(12600u, last.dropAmount,
      "Amplitude cumulée attendue : 50000 - 37400 = 12600.");
}

// -----------------------------------------------------------------------------
// 8bis — Pendant la SUPPRESSION anti-rafale, `ref` n'est PAS ré-ancrée : une
// chute en attente garde son amplitude VRAIE depuis le palier d'origine, même
// si de nouveaux ticks surviennent (suppressed) avant l'émission effective.
// -----------------------------------------------------------------------------
void test_anti_rafale_ne_reancre_pas_ref_pendant_la_suppression(void) {
  HeapDropState st;
  heapDropEvaluate(st, 50000u, 0u, kHeapDropFloorBytes, kHeapDropRefShift,
                    kHeapDropMinIntervalMs);
  // seuil = 12500. Franchissement dès courant <= 37500.
  HeapDropResult pending1 =
      heapDropEvaluate(st, 30000u, 1000u, kHeapDropFloorBytes, kHeapDropRefShift,
                        kHeapDropMinIntervalMs);
  TEST_ASSERT_FALSE_MESSAGE(pending1.emit, "Anti-rafale : trop tôt depuis l'ancrage (1000ms).");
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(50000u, st.ref,
      "MUTANT ré-ancrage-pendant-suppression : `ref` ne doit PAS bouger tant que la chute est "
      "SEULEMENT en attente (anti-rafale non écoulé), sinon l'amplitude serait amputée à "
      "l'émission.");

  HeapDropResult pending2 =
      heapDropEvaluate(st, 25000u, 2000u, kHeapDropFloorBytes, kHeapDropRefShift,
                        kHeapDropMinIntervalMs);
  TEST_ASSERT_FALSE(pending2.emit);
  TEST_ASSERT_EQUAL_UINT32(50000u, st.ref);

  // L'anti-rafale est enfin écoulé (depuis l'ANCRAGE initial, qui vaut aussi
  // comme dernière "émission" au sens de l'anti-rafale) : la chute en attente
  // s'émet, avec son amplitude VRAIE depuis 50000 (pas depuis 30000).
  HeapDropResult r = heapDropEvaluate(st, 25000u, kHeapDropMinIntervalMs, kHeapDropFloorBytes,
                                       kHeapDropRefShift, kHeapDropMinIntervalMs);
  TEST_ASSERT_TRUE(r.emit);
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(50000u, r.before,
      "MUTANT ré-ancrage-pendant-suppression : before doit rester 50000 (palier d'origine), "
      "pas 30000 (valeur vue pendant la suppression).");
  TEST_ASSERT_EQUAL_UINT32(25000u, r.after);
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(25000u, r.dropAmount,
      "Amplitude vraie attendue : 50000 - 25000 = 25000 (et NON 30000 - 25000 = 5000, qui "
      "serait le symptôme d'un ré-ancrage pendant la suppression).");
}

// -----------------------------------------------------------------------------
// 9 — Anti-rafale à la cadence 5 s (famine, kHeapCheckFastIntervalMs) ⇒
// au plus 12 lignes/heure (3 600 000 / kHeapDropMinIntervalMs).
// -----------------------------------------------------------------------------
void test_anti_rafale_cadence_5s_famine_max_12_par_heure(void) {
  HeapDropState st;
  heapDropEvaluate(st, 50000u, 0u, kHeapDropFloorBytes, kHeapDropRefShift,
                    kHeapDropMinIntervalMs);
  int emits = 0;
  uint32_t nowMs = 0;
  // 1 heure de ticks à 5 s, alternant un gros creux et un plein réarmement à
  // CHAQUE tick — la tentative la plus adverse possible de maximiser le
  // nombre d'émissions.
  const unsigned long oneHourMs = 3600000UL;
  bool low = true;
  for (nowMs = 5000u; nowMs <= oneHourMs; nowMs += 5000u) {
    uint32_t courant = low ? 10000u : 50000u;
    HeapDropResult r = heapDropEvaluate(st, courant, nowMs, kHeapDropFloorBytes,
                                         kHeapDropRefShift, kHeapDropMinIntervalMs);
    if (r.emit) emits++;
    low = !low;
  }
  const int maxExpected = (int)(oneHourMs / kHeapDropMinIntervalMs);  // 12
  char msg[160];
  snprintf(msg, sizeof(msg),
           "Famine adversariale a 5s pendant 1h : %d emissions, plafond attendu %d "
           "(3600000 / kHeapDropMinIntervalMs).",
           emits, maxExpected);
  TEST_ASSERT_TRUE_MESSAGE(emits <= maxExpected, msg);
}

// -----------------------------------------------------------------------------
// 10 — Repliement millis() (24,8 j) sur nowMs - lastEmitMs
// -----------------------------------------------------------------------------
void test_repliement_millis_24_8_jours(void) {
  HeapDropState st;
  // Ancrage juste avant le repliement.
  const uint32_t lastEmit = 0xFFFFFFF0u;  // 16 avant le wrap
  heapDropEvaluate(st, 50000u, lastEmit, kHeapDropFloorBytes, kHeapDropRefShift,
                    kHeapDropMinIntervalMs);
  TEST_ASSERT_EQUAL_UINT32(lastEmit, st.lastEmitMs);

  // nowMs APRÈS le repliement : écart RÉEL faible (16 + 1000 = 1016 ms),
  // très inférieur à l'anti-rafale (300000). Une arithmétique naïve (signée,
  // ou qui ne tolère pas le wrap) rendrait un écart ÉNORME et autoriserait à
  // tort l'émission.
  const uint32_t nowAfterWrap = 1000u;  // a "wrappé" : 0xFFFFFFF0 + 1016 mod 2^32 = 1000
  HeapDropResult r = heapDropEvaluate(st, 10000u, nowAfterWrap, kHeapDropFloorBytes,
                                       kHeapDropRefShift, kHeapDropMinIntervalMs);
  TEST_ASSERT_FALSE_MESSAGE(r.emit,
      "MUTANT arithmétique signée : l'écart réel à travers le repliement de millis() est de "
      "1016 ms (<< 300000), l'anti-rafale doit donc SUPPRIMER cette émission malgré une chute "
      "bien réelle (50000->10000).");

  // Un peu plus tard, une fois l'écart réel >= anti-rafale, la même chute (en
  // attente — ref n'a pas bougé car `courant <= ref` et la chute a été
  // différée) doit enfin s'émettre, TOUJOURS avec son amplitude vraie.
  const uint32_t nowFarEnough = (uint32_t)(lastEmit + (uint64_t)kHeapDropMinIntervalMs);  // wrap
  HeapDropResult r2 = heapDropEvaluate(st, 10000u, nowFarEnough, kHeapDropFloorBytes,
                                        kHeapDropRefShift, kHeapDropMinIntervalMs);
  TEST_ASSERT_TRUE_MESSAGE(r2.emit, "Une fois l'anti-rafale écoulé (à travers le wrap), la "
                                    "chute en attente doit enfin être émise.");
  TEST_ASSERT_EQUAL_UINT32(50000u, r2.before);
  TEST_ASSERT_EQUAL_UINT32(10000u, r2.after);
}

// -----------------------------------------------------------------------------
// 11 — Premier tick après boot ⇒ 0 ligne, référence ancrée
// -----------------------------------------------------------------------------
void test_premier_tick_apres_boot_ancre_sans_emettre(void) {
  HeapDropState st;
  TEST_ASSERT_FALSE(st.initialized);
  HeapDropResult r = heapDropEvaluate(st, 12345u, 999u, kHeapDropFloorBytes, kHeapDropRefShift,
                                       kHeapDropMinIntervalMs);
  TEST_ASSERT_FALSE_MESSAGE(r.emit, "Le tout premier tick ne doit jamais émettre, quelle que "
                                    "soit la valeur de courant.");
  TEST_ASSERT_TRUE(st.initialized);
  TEST_ASSERT_EQUAL_UINT32(12345u, st.ref);
  TEST_ASSERT_EQUAL_UINT32(999u, st.lastEmitMs);
}

// -----------------------------------------------------------------------------
// 12 — Sentinelle UINT32_MAX ⇒ BUG RÉEL TROUVÉ, documenté ci-dessous.
// -----------------------------------------------------------------------------
// ⚠️ DÉCOUVERTE (test-engineer, 2026-10-08) : la comparaison ADDITIVE
// `courant + threshold <= ref` n'est PAS protégée contre le DÉBORDEMENT de
// l'addition quand `courant` est proche de UINT32_MAX. En pratique, `courant`
// vient toujours de heapLargestFreeBlock8Bit() (quelques dizaines de Ko,
// jamais proche de 4 Go) — mais le commentaire de tête de heap_drop_logic.h
// revendique une fonction « TOTALE » et « SANS branche indéfinie quelles que
// soient les valeurs fournies », ce qui est FAUX pour ce cas précis : une
// lecture corrompue (ex. tas corrompu — précisément le scénario que ce genre
// d'instrumentation sert à diagnostiquer) ferait déborder l'addition,
// déclencherait une émission FAUSSE (ref "monterait" à une valeur proche de
// UINT32_MAX), et resterait ensuite indéfiniment "coincée" sur cette
// référence corrompue (toute lecture réelle ultérieure, bien plus petite,
// paraîtrait être une chute géante et réémettrait au rythme de l'anti-rafale).
//
// Reproduit isolément lors du passage test-engineer (feature-144) :
//   ref=20000 (ancré), courant=UINT32_MAX à t=300000 :
//     seuil=5000, `courant + seuil` DÉBORDAIT (4294967295+5000 mod 2^32 = 4999)
//     4999 <= 20000 ⇒ VRAI ⇒ FAUX POSITIF, after=4294967295,
//     dropAmount = 20000 - 4294967295 (mod 2^32) = 20001 — valeur ABSURDE,
//     et `ref` corrompue DURABLEMENT.
//
// ✅ CORRIGÉ le 2026-10-08 : `heapDropEvaluate()` écarte désormais `courant >= ref`
// AVANT toute arithmétique, puis soustrait en sachant `courant < ref`.
//
// Pourquoi le défaut méritait d'être corrigé alors qu'il paraissait théorique :
// `courant` vient de heapLargestFreeBlock8Bit() et n'approche jamais 4 Go en
// fonctionnement normal. Mais le scénario réaliste est une LECTURE CORROMPUE
// PENDANT UNE CORRUPTION DE TAS DÉJÀ EN COURS — exactement ce que cet instrument
// sert à diagnostiquer. Il aurait menti au seul moment qui compte.
//
// Ce test VERROUILLE désormais la garde : il meurt si quelqu'un revient à la
// forme additive.
void test_sentinelle_uint32_max_ne_deborde_plus(void) {
  HeapDropState st;
  heapDropEvaluate(st, 20000u, 0u, kHeapDropFloorBytes, kHeapDropRefShift,
                    kHeapDropMinIntervalMs);
  HeapDropResult r = heapDropEvaluate(st, UINT32_MAX, kHeapDropMinIntervalMs, kHeapDropFloorBytes,
                                       kHeapDropRefShift, kHeapDropMinIntervalMs);
  TEST_ASSERT_FALSE_MESSAGE(r.emit,
      "courant=UINT32_MAX est une REMONTEE, pas une chute : aucune emission. "
      "Si ce test meurt, la forme additive `courant + seuil <= ref` est revenue.");
  // Et la remontee doit avoir rearme la reference, pas la corrompre.
  HeapDropResult r2 = heapDropEvaluate(st, 20000u, 2u * kHeapDropMinIntervalMs,
                                        kHeapDropFloorBytes, kHeapDropRefShift,
                                        kHeapDropMinIntervalMs);
  TEST_ASSERT_TRUE_MESSAGE(r2.emit,
      "apres rearmement a UINT32_MAX, un retour a 20000 EST une chute enorme");
}

// -----------------------------------------------------------------------------
// 13 — Valeurs des constantes épinglées (ADR-0073 : mesurer, pas recopier —
// ce test verrouille les CONSTANTES, pas un comptage de lignes).
// -----------------------------------------------------------------------------
void test_constantes_epinglees(void) {
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(4096u, kHeapDropFloorBytes,
      "kHeapDropFloorBytes doit rester 4096 (= kRestartMaxAllocHeapBytes).");
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(2u, kHeapDropRefShift,
      "kHeapDropRefShift doit rester 2 (seuil = ref >> 2, un quart de la référence).");
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(300000u, kHeapDropMinIntervalMs,
      "kHeapDropMinIntervalMs doit rester 300000 (5 min, plancher anti-rafale).");
}

// =============================================================================
// REJEU hors cible — captures RÉELLES de /root/pool-captures (lecture seule),
// embarquées ICI en données statiques pour que le test reste PORTABLE (aucune
// dépendance runtime à un répertoire hors dépôt, absent de la CI).
//
// ⚠️ HONNÊTETÉ DE PÉRIMÈTRE (mesuré le 2026-10-08, méthode DISTINCTE de celle
// citée dans la spec — ne pas comparer les comptages bruts, voir le rapport
// de test-engineer) : déduplication sur la ligne complète « Heap 8bits: »
// (toutes les valeurs, pas seulement maxBloc8), segmentation par marqueur de
// démarrage réel « === Démarrage ESP32 Pool Controller vX.Y.Z === ». Le
// segment embarqué ci-dessous est un boot réel du 2026-09-12 15:19:35
// (110 points uniques après dédoublonnage), qui contient l'incident NOMMÉ par
// la spec : la chute du 13/09 12:21, 23540 -> 16372 (-30%), ratée par un seuil
// absolu de 8192 (diff réelle 7168).
// =============================================================================
static const uint32_t kReplaySegment5[][2] = {
  {0u, 59380u}, {1802u, 23540u}, {3604u, 23540u}, {5405u, 23540u}, {7208u, 23540u}, {9012u, 23540u},
  {10817u, 23540u}, {12619u, 23540u}, {14422u, 23540u}, {16225u, 23540u}, {18028u, 23540u}, {19831u, 23540u},
  {21634u, 23540u}, {23438u, 23540u}, {25241u, 23540u}, {27044u, 23540u}, {28847u, 23540u}, {30650u, 23540u},
  {32453u, 23540u}, {34257u, 23540u}, {36060u, 23540u}, {37863u, 23540u}, {39666u, 23540u}, {41469u, 23540u},
  {43272u, 23540u}, {45076u, 23540u}, {46879u, 23540u}, {48682u, 23540u}, {50485u, 23540u}, {52288u, 23540u},
  {54091u, 23540u}, {55895u, 23540u}, {57698u, 23540u}, {59500u, 23540u}, {61302u, 23540u}, {63104u, 23540u},
  {64907u, 23540u}, {66709u, 23540u}, {68512u, 23540u}, {70319u, 23540u}, {72121u, 23540u}, {73924u, 23540u},
  {75727u, 16372u}, {77533u, 16372u}, {79335u, 14836u}, {81138u, 14836u}, {82941u, 10228u}, {84743u, 10228u},
  {84744u, 10228u}, {86546u, 10228u}, {86547u, 10228u}, {88348u, 10228u}, {88349u, 10228u}, {90151u, 10228u},
  {90152u, 10228u}, {91953u, 10228u}, {91954u, 10228u}, {93755u, 10228u}, {93756u, 10228u}, {95559u, 10228u},
  {95560u, 10228u}, {97362u, 10228u}, {97363u, 10228u}, {99165u, 10228u}, {99166u, 10228u}, {100966u, 7924u},
  {100967u, 7924u}, {102770u, 7924u}, {102771u, 7924u}, {104572u, 7924u}, {104573u, 7924u}, {106373u, 7924u},
  {106374u, 7924u}, {108175u, 7924u}, {108176u, 7924u}, {109978u, 7924u}, {109979u, 7924u}, {111780u, 7924u},
  {111781u, 7924u}, {113582u, 7924u}, {115384u, 7924u}, {117185u, 7924u}, {118987u, 7924u}, {120790u, 7924u},
  {122592u, 7924u}, {124394u, 7924u}, {126196u, 7924u}, {127998u, 7924u}, {129800u, 7924u}, {131602u, 7924u},
  {133404u, 7924u}, {135206u, 7924u}, {137008u, 7924u}, {138810u, 7924u}, {140612u, 7924u}, {142413u, 7924u},
  {144215u, 7924u}, {146017u, 7924u}, {147819u, 7924u}, {149621u, 7924u}, {151424u, 7924u}, {153226u, 7924u},
  {155028u, 7924u}, {156830u, 7924u}, {158632u, 7924u}, {160434u, 7924u}, {162236u, 7924u}, {164038u, 7924u},
  {165840u, 7924u}, {167641u, 7924u},
};
static const size_t kReplaySegment5Count = sizeof(kReplaySegment5) / sizeof(kReplaySegment5[0]);

void test_rejeu_capture_reelle_boot_20260912_attrape_lincident_30_pourcent(void) {
  HeapDropState st;
  int emits = 0;
  bool caughtTheNamedIncident = false;
  for (size_t i = 0; i < kReplaySegment5Count; i++) {
    const uint32_t relSec = kReplaySegment5[i][0];
    const uint32_t maxBloc8 = kReplaySegment5[i][1];
    const uint32_t nowMs = relSec * 1000u;
    HeapDropResult r = heapDropEvaluate(st, maxBloc8, nowMs, kHeapDropFloorBytes,
                                         kHeapDropRefShift, kHeapDropMinIntervalMs);
    if (r.emit) {
      emits++;
      if (r.before == 23540u && r.after == 16372u) caughtTheNamedIncident = true;
    }
  }
  TEST_ASSERT_EQUAL_INT_MESSAGE(3, emits,
      "Rejeu du boot réel du 2026-09-12 15:19:35 (captures pool-captures, dédupliqué, "
      "mesuré le 2026-10-08) : 3 lignes attendues. Si ce nombre change, c'est une régression "
      "de calibrage (ou une mise à jour légitime des constantes à documenter) — ne pas "
      "ajuster l'assertion sans comprendre pourquoi.");
  TEST_ASSERT_TRUE_MESSAGE(caughtTheNamedIncident,
      "Le rejeu doit attraper l'incident NOMMÉ par la spec : 23540 -> 16372 (-30%, 13/09 "
      "12:21), raté par un seuil absolu de 8192 (diff réelle 7168 < 8192).");
}

// ===========================================================================
// main
// ===========================================================================

int main(int argc, char** argv) {
  UNITY_BEGIN();

  RUN_TEST(test_franchissement_18432_depuis_45044);
  RUN_TEST(test_non_franchissement_sous_seuil);
  RUN_TEST(test_frontiere_exacte_inclusive);
  RUN_TEST(test_plancher_actif_domine_le_seuil_relatif);
  RUN_TEST(test_asymetrie_corrigee_23540_vers_16372);
  RUN_TEST(test_plateau_bas_20_ticks_une_seule_ligne);
  RUN_TEST(test_chute_remontee_nouvelle_chute_deux_lignes);
  RUN_TEST(test_chute_progressive_amplitude_cumulee);
  RUN_TEST(test_anti_rafale_ne_reancre_pas_ref_pendant_la_suppression);
  RUN_TEST(test_anti_rafale_cadence_5s_famine_max_12_par_heure);
  RUN_TEST(test_repliement_millis_24_8_jours);
  RUN_TEST(test_premier_tick_apres_boot_ancre_sans_emettre);
  RUN_TEST(test_sentinelle_uint32_max_ne_deborde_plus);
  RUN_TEST(test_constantes_epinglees);
  RUN_TEST(test_rejeu_capture_reelle_boot_20260912_attrape_lincident_30_pourcent);

  return UNITY_END();
}
