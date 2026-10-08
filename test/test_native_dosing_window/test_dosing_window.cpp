// =============================================================================
// Tests unitaires natifs — SURVIE DU BUDGET HORAIRE DE DOSAGE AU REDÉMARRAGE
// (feature-136, gate pool-chemistry du 2026-10-08, GO sous 12 conditions)
// =============================================================================
// Cible : src/dosing_window_logic.h, module PUR (ADR-0017). Tourne sur PC
// (env:native, Unity), hors ESP32 / NVS / RTC_NOINIT / Arduino.
//
// CE QUE CETTE SUITE PROUVE
// -------------------------
// Les décisions de dosingWindowResolveChannel() / dosingWindowRebaseAnchor() /
// dosingWindowRestoreRingEntry() : fusion T1/T2, péremption (deux horloges
// distinctes, 1 h vs 24 h), C4 (écoulé inconnu ≠ T3), C5 (armement de la pause
// de mélange), C6 (ring), C9 (sentinelle 0 / collision d'ancre), intégrité des
// deux instantanés (taille exacte, CRC, schéma).
//
// CE QU'ELLE NE PROUVE PAS (dit sans détour, cf. rapport de test) :
//   - le câblage réel au boot (dosing_window.cpp::begin(), impure — inclut
//     esp_system.h / esp_rtc_time.h / Preferences.h / Arduino.h, non
//     compilable en natif) ;
//   - la condition C7 : qu'esp_rtc_get_time_us() traverse RÉELLEMENT
//     esp_restart() sur le matériel (le module pur se contente de CONSOMMER
//     un bool unknownElapsed déjà tranché par la coquille — voir procédure
//     on-target dans le rapport) ;
//   - le filtre « reset logiciel uniquement » (resetIsSoftware(), condition
//     C7/C9) : il vit dans dosing_window.cpp, impur, et n'est PAS compilé par
//     env:native. Le test 8 ci-dessous prouve le CONTRAT que le module pur
//     respecte quand on lui dit t2.available=false — pas le filtre lui-même ;
//   - le forfait conservateur T1 (+kDosingWindowPersistIntervalMs si pompe-ON
//     à l'écriture, écrêté à l'heure) : il est calculé dans dosing_window.cpp
//     AVANT d'appeler ce module (le commentaire du module le dit explicitement :
//     « ce module n'a pas à le recalculer »). Seule la partie C5 (armement de
//     la pause de mélange) est testée ici.
//
// ORGANISATION
//   1-15 : les onze scénarios D1 de la spec + les quatre extensions du gate
//          (plus défavorable retenu, anti-blocage perpétuel C8, ring, C4 côté
//          T2).
// =============================================================================

#include <unity.h>
#include <stdint.h>

#include "constants.h"
#include "dosing_window_logic.h"
#include "dosing_logic.h"  // countCyclesInWindow (pur, déjà compilé en natif) : preuve bout-en-bout du ring (test 14)

void setUp(void) {}
void tearDown(void) {}

// -----------------------------------------------------------------------------
// Décor commun
// -----------------------------------------------------------------------------
static const uint32_t kHour = kDosingWindowHourMs;   // 3 600 000 ms
static const uint32_t kDay  = kDosingWindowDayMs;     // 86 400 000 ms
static const uint32_t kFullMixPh = (uint32_t)kPhMixingDelayMs;   // 900 000 ms
static const uint32_t kFullMixOrp = (uint32_t)kOrpMixingDelayMs; // 1 200 000 ms

static DosingWindowSourceInputs unavailable() {
  DosingWindowSourceInputs in;
  in.available = false;
  return in;
}

// =============================================================================
// Test 1 — reprise à chaud (RTC intact, reset logiciel, 12 s) ⇒ usedMs
// conservé, source RTC
// =============================================================================
void test_01_warm_resume_rtc_conserves_used_ms(void) {
  DosingWindowSourceInputs t2;
  t2.available = true;
  t2.usedMs = 480000u;                    // 8 min déjà consommées
  t2.windowElapsedAtWriteMs = 480000u;     // 8 min écoulées dans la fenêtre à l'écriture
  t2.cyclesDayElapsedAtWriteMs = 3600000u; // 1 h dans le jour
  t2.cyclesToday = 3;
  t2.outageElapsedMs = 12000u;             // 12 s de coupure (reset logiciel)
  t2.unknownElapsed = false;
  t2.mixingRemainingMs = 0;
  t2.pumpOnAtWrite = false;

  const DosingWindowSourceInputs t1 = unavailable();
  const uint32_t nowMs = 1000000u;  // domaine millis() courant, arbitraire mais > elapsed+outage

  const DosingWindowChannelVerdict v = dosingWindowResolveChannel(t2, t1, kFullMixPh, nowMs);

  TEST_ASSERT_EQUAL(DosingWindowSource::kRtc, v.source);
  TEST_ASSERT_EQUAL_UINT32(480000u, v.usedMs);  // conservé, pas remis à zéro
  // L'ancre doit redonner EXACTEMENT l'écoulé total (8 min + 12 s) une fois
  // reporté dans le domaine courant.
  TEST_ASSERT_EQUAL_UINT32(480000u + 12000u, nowMs - v.windowStart);
  TEST_ASSERT_EQUAL_UINT16(3, v.cyclesToday);
}

// =============================================================================
// Test 2 — coupure courte (RTC perdu, NVS valide, 90 s, horloge valide) ⇒
// fenêtre reconstruite, source NVS
// =============================================================================
void test_02_short_outage_nvs_rebuilds_window(void) {
  const DosingWindowSourceInputs t2 = unavailable();

  DosingWindowSourceInputs t1;
  t1.available = true;
  t1.usedMs = 600000u;                     // 10 min consommées (déjà forfaitées par la coquille)
  t1.windowElapsedAtWriteMs = 1200000u;     // 20 min dans la fenêtre à l'écriture
  t1.cyclesDayElapsedAtWriteMs = 7200000u;  // 2 h dans le jour
  t1.cyclesToday = 7;
  t1.outageElapsedMs = 90000u;              // 90 s de coupure secteur
  t1.unknownElapsed = false;
  t1.mixingRemainingMs = 0;
  t1.pumpOnAtWrite = false;

  const uint32_t nowMs = 2000000u;
  const DosingWindowChannelVerdict v = dosingWindowResolveChannel(t2, t1, kFullMixPh, nowMs);

  TEST_ASSERT_EQUAL(DosingWindowSource::kNvs, v.source);
  TEST_ASSERT_EQUAL_UINT32(600000u, v.usedMs);
  TEST_ASSERT_EQUAL_UINT32(1200000u + 90000u, nowMs - v.windowStart);
  TEST_ASSERT_EQUAL_UINT16(7, v.cyclesToday);
}

// =============================================================================
// Test 3 — coupure longue (> 1 h) ⇒ budget PLEIN (péremption horaire), MAIS
// cycles CONSERVÉS (< 24 h). Les deux péremptions sont DISTINCTES.
// =============================================================================
void test_03_long_outage_resets_window_but_conserves_cycles_under_24h(void) {
  DosingWindowSourceInputs t2;
  t2.available = true;
  t2.usedMs = 540000u;                     // 9 min consommées avant la coupure
  t2.windowElapsedAtWriteMs = 100000u;      // seulement 100 s dans la fenêtre horaire à l'écriture
  t2.cyclesDayElapsedAtWriteMs = 100000u;   // et 100 s dans le jour
  t2.cyclesToday = 42;
  t2.outageElapsedMs = 7200000u;            // 2 h de coupure : > le reliquat horaire (3 500 000 ms)
                                             //                  < le reliquat journalier (~86,3 Ms)
  t2.unknownElapsed = false;
  t2.mixingRemainingMs = 0;
  t2.pumpOnAtWrite = false;

  const DosingWindowSourceInputs t1 = unavailable();
  const uint32_t nowMs = 10000000u;

  const DosingWindowChannelVerdict v = dosingWindowResolveChannel(t2, t1, kFullMixPh, nowMs);

  // Fenêtre horaire : périmée, budget plein (0 consommé), ancre = maintenant.
  TEST_ASSERT_EQUAL_UINT32(0u, v.usedMs);
  TEST_ASSERT_EQUAL_UINT32(nowMs, v.windowStart);
  // Jour des cycles : PAS périmé (2 h << 24 h) : cyclesToday conservé, ancre
  // reconstruite (PAS "maintenant").
  TEST_ASSERT_EQUAL_UINT16(42, v.cyclesToday);
  TEST_ASSERT_NOT_EQUAL(nowMs, v.cyclesDayStart);
  TEST_ASSERT_EQUAL_UINT32(100000u + 7200000u, nowMs - v.cyclesDayStart);
}

// =============================================================================
// Test 4 — horloge invalide + RTC absent ⇒ étage T3 : usedMs = limite pour
// UNE SEULE fenêtre, cyclesToday à 0, PUIS retour au nominal (le module est
// pur/sans état : l'appel suivant, à entrées nominales, n'est pas affecté).
// =============================================================================
void test_04_no_source_triggers_t3_single_window_then_nominal(void) {
  const DosingWindowSourceInputs t2 = unavailable();
  const DosingWindowSourceInputs t1 = unavailable();
  const uint32_t nowMs = 55555u;

  const DosingWindowChannelVerdict v = dosingWindowResolveChannel(t2, t1, kFullMixPh, nowMs);

  TEST_ASSERT_EQUAL(DosingWindowSource::kFailClosed, v.source);
  TEST_ASSERT_EQUAL_UINT32(kHour, v.usedMs);     // UNE SEULE fenêtre pleine, pas plus
  TEST_ASSERT_EQUAL_UINT32(nowMs, v.windowStart);
  TEST_ASSERT_EQUAL_UINT16(0, v.cyclesToday);
  TEST_ASSERT_EQUAL_UINT32(0u, v.mixingEndMs);

  // "Retour au nominal" : le même appel pur, avec des entrées nominales,
  // n'est pas pollué par l'appel précédent (aucun état caché/statique).
  DosingWindowSourceInputs t2b;
  t2b.available = true;
  t2b.usedMs = 60000u;
  t2b.windowElapsedAtWriteMs = 60000u;
  t2b.cyclesDayElapsedAtWriteMs = 60000u;
  t2b.cyclesToday = 1;
  t2b.outageElapsedMs = 1000u;
  const DosingWindowChannelVerdict v2 =
      dosingWindowResolveChannel(t2b, unavailable(), kFullMixPh, nowMs + 1000000u);
  TEST_ASSERT_EQUAL(DosingWindowSource::kRtc, v2.source);
  TEST_ASSERT_EQUAL_UINT32(60000u, v2.usedMs);
}

// =============================================================================
// Test 5 — horloge qui RECULE (correction NTP) ⇒ instantané T1 daté du futur
// (epochAtWrite > epochNow) ⇒ C4 : écoulé déclaré INCONNU. Comportement SÛR :
// usedMs conservé intégralement (jamais réduit ni remis à zéro), ancre = now
// (une fenêtre pleine recommence, conservateur). Ce n'est PAS le T3 formel
// (source reste kNvs) : "fail-closed" est ici au sens de "jamais permissif".
// =============================================================================
void test_05_clock_stepped_backward_unknown_elapsed_is_conservative(void) {
  DosingWindowSourceInputs t1;
  t1.available = true;             // instantané NVS intact, intégrité OK
  t1.usedMs = 900000u;              // 15 min déjà consommées
  t1.windowElapsedAtWriteMs = 1800000u;
  t1.cyclesDayElapsedAtWriteMs = 1800000u;
  t1.cyclesToday = 9;
  t1.unknownElapsed = true;         // epochNow < epochAtWrite (NTP a corrigé l'horloge en arrière)
  t1.mixingRemainingMs = 300000u;   // 5 min de mélange restant à l'écriture

  const uint32_t nowMs = 3000000u;
  const DosingWindowChannelVerdict v =
      dosingWindowResolveChannel(unavailable(), t1, kFullMixPh, nowMs);

  TEST_ASSERT_EQUAL(DosingWindowSource::kNvs, v.source);  // pas kFailClosed : T1 reste "disponible"
  TEST_ASSERT_EQUAL_UINT32(900000u, v.usedMs);            // conservé À L'IDENTIQUE, jamais réduit
  TEST_ASSERT_EQUAL_UINT32(nowMs, v.windowStart);          // ancre = maintenant (conservateur)
  TEST_ASSERT_EQUAL_UINT16(9, v.cyclesToday);
  TEST_ASSERT_EQUAL_UINT32(nowMs + 300000u, v.mixingEndMs);  // mélange reconduit tel quel (unknownElapsed)
}

// =============================================================================
// Test 6 — nowMs faible + elapsedMs grand ⇒ sous-débordement NON SIGNÉ EXACT.
// Au boot nowMs ≈ 3 000, elapsedMs = 1 800 000 ⇒ ancre = 4 293 170 296, et
// now − windowStart doit redonner EXACTEMENT 1 800 000.
// =============================================================================
void test_06_underflow_wraparound_is_exact(void) {
  const uint32_t nowMs = 3000u;
  const uint32_t elapsedAtWriteMs = 1800000u;
  const uint32_t outageElapsedMs = 0u;

  const DosingWindowAnchorRebase r =
      dosingWindowRebaseAnchor(elapsedAtWriteMs, outageElapsedMs, false, kHour, nowMs);

  TEST_ASSERT_FALSE(r.resetToFresh);
  TEST_ASSERT_EQUAL_UINT32(4293170296u, r.anchorMs);
  // L'arithmétique non signée doit redonner l'écoulé exact à travers le repli.
  TEST_ASSERT_EQUAL_UINT32(1800000u, nowMs - r.anchorMs);
}

// =============================================================================
// Test 7 — taille relue ≠ taille exacte ⇒ rejet NET, jamais de
// réinterprétation partielle (condition C9, RTC et NVS).
// =============================================================================
void test_07_wrong_size_is_rejected_outright(void) {
  DosingWindowRtcRecord rtc;
  rtc.magic = kDosingWindowRtcMagic;
  rtc.version = kDosingWindowRtcSchema;
  for (int ch = 0; ch < 2; ++ch) {
    rtc.usedMs[ch] = 0;
    rtc.windowElapsedMs[ch] = 0;
    rtc.cyclesDayElapsedMs[ch] = 0;
    rtc.mixingRemainingMs[ch] = 0;
    rtc.cyclesToday[ch] = 0;
    rtc.pumpOnAtWrite[ch] = 0;
    rtc.ringIdx[ch] = 0;
    for (size_t i = 0; i < kDosingCycleHistorySize; ++i) rtc.ringAgeMs[ch][i] = kDosingWindowRingEmptySentinel;
  }
  rtc.rtcCounterAtWriteUsLo = 0;
  rtc.rtcCounterAtWriteUsHi = 0;
  dosingWindowRtcSeal(rtc);

  // Taille exacte : accepté.
  TEST_ASSERT_TRUE(dosingWindowRtcIntact(rtc, sizeof(rtc)));
  // Taille tronquée d'UN SEUL octet : rejet net, pas de lecture partielle.
  TEST_ASSERT_FALSE(dosingWindowRtcIntact(rtc, sizeof(rtc) - 1));
  TEST_ASSERT_FALSE(dosingWindowRtcIntact(rtc, 0));

  DosingWindowNvsRecord nvs;
  nvs.magic = kDosingWindowNvsMagic;
  nvs.version = kDosingWindowNvsSchema;
  dosingWindowNvsSeal(nvs);
  TEST_ASSERT_TRUE(dosingWindowNvsIntact(nvs, sizeof(nvs)));
  TEST_ASSERT_FALSE(dosingWindowNvsIntact(nvs, sizeof(nvs) - 1));
  TEST_ASSERT_FALSE(dosingWindowNvsIntact(nvs, sizeof(nvs) + 1));
}

// =============================================================================
// Test 8 — reset de type power-on / brownout ⇒ RAM RTC REFUSÉE même si son
// empreinte (CRC/taille/schéma) passe. ⚠️ Le FILTRE resetIsSoftware() lui-même
// vit dans dosing_window.cpp (impur, non natif) — voir honnêteté de périmètre
// en tête de fichier. Ce que ce test prouve : le module pur respecte à la
// lettre le verdict `available` qu'on lui donne, SANS jamais relire les
// champs T2 par un autre chemin quand `available=false` — même avec un CRC
// qui passerait au contrôle d'intégrité.
// =============================================================================
void test_08_rtc_marked_unavailable_is_never_consulted(void) {
  DosingWindowSourceInputs t2;
  t2.available = false;           // ce que la coquille pose sur un reset power-on/brownout
  t2.usedMs = 999999u;             // valeurs "CRC-valides" mais qui NE DOIVENT JAMAIS sortir
  t2.windowElapsedAtWriteMs = 1u;
  t2.cyclesToday = 250;

  DosingWindowSourceInputs t1;
  t1.available = true;
  t1.usedMs = 120000u;
  t1.windowElapsedAtWriteMs = 120000u;
  t1.cyclesDayElapsedAtWriteMs = 120000u;
  t1.cyclesToday = 2;
  t1.outageElapsedMs = 1000u;

  const uint32_t nowMs = 500000u;
  const DosingWindowChannelVerdict v = dosingWindowResolveChannel(t2, t1, kFullMixPh, nowMs);

  TEST_ASSERT_EQUAL(DosingWindowSource::kNvs, v.source);  // jamais kRtc ni kWorst : T2 ignoré
  TEST_ASSERT_EQUAL_UINT32(120000u, v.usedMs);             // valeur T1, pas la valeur T2 "CRC-valide"
  TEST_ASSERT_EQUAL_UINT16(2, v.cyclesToday);
}

// =============================================================================
// Test 9 — ancre reconstruite valant 0 ⇒ normalisée à 1 (collision avec la
// sentinelle « fenêtre jamais ouverte » de refreshDosingState/tickDosingCycleDay)
// =============================================================================
void test_09_zero_anchor_normalized_to_one(void) {
  // Fenêtre horaire : now - elapsed - outage == 0 exactement.
  const DosingWindowAnchorRebase rw =
      dosingWindowRebaseAnchor(/*elapsedAtWrite*/ 900000u, /*outage*/ 100000u, false, kHour,
                                /*nowMs*/ 1000000u);
  TEST_ASSERT_FALSE(rw.resetToFresh);
  TEST_ASSERT_EQUAL_UINT32(1u, rw.anchorMs);  // jamais 0

  // Même collision côté jour (période différente, même piège).
  const DosingWindowAnchorRebase rd =
      dosingWindowRebaseAnchor(/*elapsedAtWrite*/ 500000u, /*outage*/ 500000u, false, kDay,
                                /*nowMs*/ 1000000u);
  TEST_ASSERT_FALSE(rd.resetToFresh);
  TEST_ASSERT_EQUAL_UINT32(1u, rd.anchorMs);

  // Et par le chemin complet (resolveChannel) : jamais 0 non plus.
  DosingWindowSourceInputs t2;
  t2.available = true;
  t2.usedMs = 100u;
  t2.windowElapsedAtWriteMs = 900000u;
  t2.cyclesDayElapsedAtWriteMs = 500000u;
  t2.cyclesToday = 1;
  t2.outageElapsedMs = 100000u;
  const DosingWindowChannelVerdict v =
      dosingWindowResolveChannel(t2, unavailable(), kFullMixPh, 1000000u);
  TEST_ASSERT_NOT_EQUAL(0u, v.windowStart);
}

// =============================================================================
// Test 10 — pompe-ON à l'écriture ⇒ pause de mélange ARMÉE PLEINEMENT (C5),
// prioritaire sur tout reliquat calculé. C'est le chemin de surdosage que le
// plan initial avait manqué : le produit a été injecté, notifyPhDose() n'a
// jamais tourné. ⚠️ Le forfait de cadence sur usedMs (+30 s, écrêté à l'heure)
// est calculé dans dosing_window.cpp, impur — non exercé ici (voir en-tête).
// =============================================================================
void test_10_pump_on_at_write_arms_full_mixing_pause(void) {
  DosingWindowSourceInputs t1;
  t1.available = true;
  t1.usedMs = kHour;                 // déjà au plafond (forfait déjà appliqué par la coquille, simulé ici)
  t1.windowElapsedAtWriteMs = 1800000u;
  t1.cyclesDayElapsedAtWriteMs = 1800000u;
  t1.cyclesToday = 4;
  t1.outageElapsedMs = 5000u;
  t1.mixingRemainingMs = 0u;          // AUCUNE pause en cours à l'écriture (pompe tournait encore)
  t1.pumpOnAtWrite = true;            // ← C5

  const uint32_t nowMs = 2000000u;
  const DosingWindowChannelVerdict v =
      dosingWindowResolveChannel(unavailable(), t1, kFullMixOrp, nowMs);

  // Armement PLEIN (20 min ORP ici), pas 0, malgré mixingRemainingMs=0 en entrée.
  TEST_ASSERT_EQUAL_UINT32(nowMs + kFullMixOrp, v.mixingEndMs);
}

// =============================================================================
// Test 11 — cyclesDayStart restauré ⇒ la garde anti-cycling (shouldRolloverByMillis,
// pur et déjà compilé en natif dans dosing_logic.cpp) NE remet PAS cyclesToday
// à zéro au tick suivant. C'EST LE MUTANT CLÉ (M4) : omettre cyclesDayStart
// reproduit un no-op SILENCIEUX — cyclesToday semble restauré, mais la
// sentinelle « jamais ouvert » (0) le ferait repartir à zéro dès le 1er tick.
// =============================================================================
void test_11_restored_cycles_day_start_prevents_silent_rollover(void) {
  DosingWindowSourceInputs t2;
  t2.available = true;
  t2.usedMs = 60000u;
  t2.windowElapsedAtWriteMs = 60000u;
  t2.cyclesDayElapsedAtWriteMs = 3600000u;  // 1 h dans le jour à l'écriture
  t2.cyclesToday = 55;                      // proche du plafond anti-cycling
  t2.outageElapsedMs = 12000u;               // reboot de 12 s
  t2.unknownElapsed = false;

  const uint32_t nowMs = 5000000u;
  const DosingWindowChannelVerdict v =
      dosingWindowResolveChannel(t2, unavailable(), kFullMixPh, nowMs);

  TEST_ASSERT_EQUAL_UINT16(55, v.cyclesToday);
  TEST_ASSERT_NOT_EQUAL(0u, v.cyclesDayStart);  // jamais la sentinelle "jamais ouvert"

  // La garde réelle (dosing_logic.cpp, déjà compilée nativement) ne doit PAS
  // déclencher de rollover juste après restauration : l'ancre restaurée a
  // tout juste 1 h + 12 s, très sous les 24 h.
  TEST_ASSERT_FALSE(shouldRolloverByMillis(v.cyclesDayStart, nowMs));

  // Contraste pédagogique : si cyclesDayStart avait été omis (mutant M4) et
  // valait 0 (sentinelle "jamais ouvert"), la même garde déclencherait un
  // rollover dès que l'horloge dépasse 86 400 000 ms d'uptime — ce qui arrive
  // sur un appareil qui tourne depuis plus d'un jour. C'est le no-op silencieux
  // du gate : cyclesToday=55 semblerait restauré puis serait effacé au
  // 1er tick suivant largement avant la vraie fin de journée.
  const uint32_t uptimeOverOneDay = nowMs + kDay + 1u;
  TEST_ASSERT_TRUE(shouldRolloverByMillis(0u, uptimeOverOneDay));
}

// =============================================================================
// Extension gate — T1 et T2 CONTRADICTOIRES ⇒ le PLUS DÉFAVORABLE retenu sur
// les TROIS grandeurs indépendamment (jamais la moyenne, jamais le plus récent).
// =============================================================================
void test_12_worst_of_both_sources_retained_per_quantity(void) {
  DosingWindowSourceInputs t2;  // T2 : usedMs haut, cyclesToday bas, mélange court
  t2.available = true;
  t2.usedMs = 3000000u;             // 50 min — le pire pour usedMs
  t2.windowElapsedAtWriteMs = 100000u;
  t2.cyclesDayElapsedAtWriteMs = 100000u;
  t2.cyclesToday = 10;               // bas
  t2.outageElapsedMs = 5000u;
  t2.mixingRemainingMs = 60000u;     // 1 min — court
  t2.pumpOnAtWrite = false;

  DosingWindowSourceInputs t1;  // T1 : usedMs bas, cyclesToday haut, mélange long
  t1.available = true;
  t1.usedMs = 100000u;               // bas
  t1.windowElapsedAtWriteMs = 100000u;
  t1.cyclesDayElapsedAtWriteMs = 100000u;
  t1.cyclesToday = 50;                // le pire pour cyclesToday
  t1.outageElapsedMs = 5000u;
  t1.mixingRemainingMs = 600000u;     // 10 min — le pire (le plus long)
  t1.pumpOnAtWrite = false;

  const uint32_t nowMs = 4000000u;
  const DosingWindowChannelVerdict v = dosingWindowResolveChannel(t2, t1, kFullMixPh, nowMs);

  TEST_ASSERT_EQUAL(DosingWindowSource::kWorst, v.source);
  TEST_ASSERT_EQUAL_UINT32(3000000u, v.usedMs);        // T2 gagne ici
  TEST_ASSERT_EQUAL_UINT16(50, v.cyclesToday);          // T1 gagne ici
  // T1 gagne ici (le plus long) : son reliquat décompte l'outage (600000-5000).
  TEST_ASSERT_EQUAL_UINT32(nowMs + 595000u, v.mixingEndMs);
  // Ni la moyenne (1 550 000) ni l'un des deux `usedMs` pris au hasard :
  // vérifié explicitement, pas par coïncidence.
  TEST_ASSERT_NOT_EQUAL((3000000u + 100000u) / 2u, v.usedMs);
}

// =============================================================================
// Extension gate (C8) — 5 reboots en 20 min : le module PUR n'a aucun état
// caché ; la "pénalité" T3 ne s'accumule jamais, et l'ancre journalière
// restaurée avance avec le temps réel (pas de rollover repoussé sans fin).
// =============================================================================
void test_13_five_reboots_in_twenty_minutes_no_perpetual_penalty(void) {
  // Cinq "redémarrages" successifs, tous en échec de source (T3), espacés de
  // 4 min (0, 4, 8, 12, 16 min) : chacun doit produire EXACTEMENT la même
  // pénalité d'UNE SEULE fenêtre — jamais cumulative, jamais croissante.
  const uint32_t startTimes[5] = {0u, 240000u, 480000u, 720000u, 960000u};
  for (int i = 0; i < 5; ++i) {
    const uint32_t nowMs = 1000000u + startTimes[i];
    const DosingWindowChannelVerdict v =
        dosingWindowResolveChannel(unavailable(), unavailable(), kFullMixPh, nowMs);
    TEST_ASSERT_EQUAL(DosingWindowSource::kFailClosed, v.source);
    TEST_ASSERT_EQUAL_UINT32(kHour, v.usedMs);   // TOUJOURS une seule fenêtre, jamais davantage
    TEST_ASSERT_EQUAL_UINT32(nowMs, v.windowStart);
  }

  // Le jour des cycles, lui, n'est PAS gelé par ces échecs répétés : une
  // source valide survenant ENTRE deux reboots avance bien l'ancre avec le
  // temps réel (pas de rollover repoussé indéfiniment par la pénalité T3).
  DosingWindowSourceInputs t2;
  t2.available = true;
  t2.usedMs = 0u;
  t2.windowElapsedAtWriteMs = 0u;
  t2.cyclesDayElapsedAtWriteMs = 1200000u;  // 20 min dans le jour
  t2.cyclesToday = 6;
  t2.outageElapsedMs = 10000u;
  const DosingWindowChannelVerdict vOk =
      dosingWindowResolveChannel(t2, unavailable(), kFullMixPh, 3000000u);
  TEST_ASSERT_EQUAL_UINT16(6, vOk.cyclesToday);
  TEST_ASSERT_EQUAL_UINT32(1200000u + 10000u, 3000000u - vOk.cyclesDayStart);
}

// =============================================================================
// Extension gate — RING : 20 démarrages puis reboot de 12 s ⇒ la 21e demande
// est REFUSÉE par le ring restauré (preuve bout-en-bout avec
// countCyclesInWindow, pur et déjà compilé en natif dans dosing_logic.cpp).
// =============================================================================
void test_14_restored_ring_blocks_21st_cycle_start(void) {
  TEST_ASSERT_EQUAL_UINT32(20u, (uint32_t)kDosingCycleHistorySize);
  TEST_ASSERT_EQUAL_UINT8(20, kMaxDosingCyclesPer15Min);

  const uint32_t outageElapsedMs = 12000u;  // reboot de 12 s
  const uint32_t nowMs = 5000000u;
  uint32_t ring[kDosingCycleHistorySize];
  for (size_t i = 0; i < kDosingCycleHistorySize; ++i) {
    // 20 démarrages espacés de 10 s avant la coupure (190 s d'étalement total,
    // tous largement sous les 15 min de la fenêtre du ring).
    const uint32_t ageAtWriteMs = (uint32_t)i * 10000u;
    ring[i] = dosingWindowRestoreRingEntry(ageAtWriteMs, outageElapsedMs, false, nowMs);
    TEST_ASSERT_NOT_EQUAL(0u, ring[i]);  // aucune entrée écartée
  }

  const int count = countCyclesInWindow(ring, kDosingCycleHistorySize, nowMs, kDosingWindowRingDiscardMs);
  TEST_ASSERT_EQUAL_INT(20, count);
  // Le ring restauré est donc déjà SATURÉ : la 21e demande serait refusée
  // (count >= kMaxDosingCyclesPer15Min), exactement comme avant le reboot.
  TEST_ASSERT_TRUE(count >= kMaxDosingCyclesPer15Min);

  // Slot vide à l'écriture (sentinelle) : reste vide dans tous les cas,
  // écoulé inconnu ou pas (C6) — un écoulé inconnu ne fabrique jamais un cycle
  // qui n'a pas eu lieu.
  TEST_ASSERT_EQUAL_UINT32(
      0u, dosingWindowRestoreRingEntry(kDosingWindowRingEmptySentinel, outageElapsedMs, false, nowMs));
  TEST_ASSERT_EQUAL_UINT32(
      0u, dosingWindowRestoreRingEntry(kDosingWindowRingEmptySentinel, outageElapsedMs, true, nowMs));
}

// =============================================================================
// Extension gate (C4, côté T2) — écoulé inconnu (compteur monotone revenu en
// arrière, cf. dosing_window.cpp) MAIS instantané intact ⇒ PAS de T3 : usedMs
// restauré intégralement, windowStart = now. Miroir du test 5 (T1), côté T2.
// =============================================================================
void test_15_unknown_elapsed_t2_restores_fully_without_t3(void) {
  DosingWindowSourceInputs t2;
  t2.available = true;              // CRC/schéma intacts : l'instantané EST exploitable
  t2.usedMs = 420000u;               // 7 min déjà consommées
  t2.windowElapsedAtWriteMs = 1500000u;
  t2.cyclesDayElapsedAtWriteMs = 1500000u;
  t2.cyclesToday = 5;
  t2.unknownElapsed = true;          // esp_rtc_get_time_us() revenu en arrière (n'a pas traversé le reboot)
  t2.mixingRemainingMs = 120000u;

  const uint32_t nowMs = 8000000u;
  const DosingWindowChannelVerdict v =
      dosingWindowResolveChannel(t2, unavailable(), kFullMixPh, nowMs);

  TEST_ASSERT_EQUAL(DosingWindowSource::kRtc, v.source);  // disponible, pas T3
  TEST_ASSERT_EQUAL_UINT32(420000u, v.usedMs);             // intégral, jamais réduit
  TEST_ASSERT_EQUAL_UINT32(nowMs, v.windowStart);
  TEST_ASSERT_EQUAL_UINT16(5, v.cyclesToday);
  TEST_ASSERT_EQUAL_UINT32(nowMs + 120000u, v.mixingEndMs);
}

// =============================================================================
// Extension — frontière EXACTE de péremption : outageElapsedMs == reliquat
// exact (ni plus, ni moins) ⇒ péremption (frontière `>=` inclusive, pas `>`).
// Couvre le mutant M8 (`>=` affaibli en `>`) que les autres scénarios ne
// touchent pas (ils visent tous une marge franche au-dessus ou en dessous).
// =============================================================================
void test_16_expiry_threshold_is_inclusive_at_exact_boundary(void) {
  const uint32_t nowMs = 5000000u;
  const uint32_t elapsedAtWriteMs = 1000000u;
  const uint32_t remainingExact = kHour - elapsedAtWriteMs;  // 2 600 000 ms

  const DosingWindowAnchorRebase r =
      dosingWindowRebaseAnchor(elapsedAtWriteMs, /*outage*/ remainingExact, false, kHour, nowMs);

  TEST_ASSERT_TRUE(r.resetToFresh);  // pile au reliquat : la fenêtre A FINI de tourner
}


// ---------------------------------------------------------------------------
// 17. C10 — le budget RESTAURÉ couvre AUSSI l'injection MANUELLE (ADR-0020)
// ---------------------------------------------------------------------------
// Exigé par le gate pool-chemistry P2 (condition S1). Jusqu'ici, C10 ne reposait
// que sur une LECTURE DE CODE : `applyRestoredDosingWindow()` écrit
// `phDosingState.usedMs`, seul champ lu par le chemin manuel. Vrai — mais une
// lecture ne résiste pas au prochain remaniement. Ce test cloue le contrat :
// un budget restauré depuis la persistance DOIT refuser une commande manuelle,
// exactement comme un budget consommé en vol.
//
// Le scénario est celui qui a motivé tout le lot : l'appareil redémarre avec
// 9 min déjà consommées sur 10 ; sans restauration il en autoriserait 10 de
// plus — ici il doit refuser.
static ManualInjectInputs manualInputsAllOk(void) {
  ManualInjectInputs in{};
  in.watchdogActive = true;          // toutes les gardes AMONT passantes,
  in.waterPresent = true;            // pour isoler la seule limite horaire
  in.stabilizationActive = false;
  in.alreadyInjecting = false;
  in.requestedMl = 50.0f;
  in.dailyInjectedMl = 0.0f;
  in.maxDailyMl = 2000.0f;
  in.hourlyLimitMs = 600000UL;       // 10 min/h, valeur relevée sur l'appareil
  in.requestedDurationMs = 120000UL; // 2 min demandées
  in.cyclesToday = 1; in.maxCyclesPerDay = 24;
  in.cyclesLastMin = 0; in.maxCyclesPerMin = 6;
  in.cyclesLast15Min = 0; in.maxCyclesPer15Min = 20;
  return in;
}

void test_c10_restored_budget_blocks_manual_injection(void) {
  // Un instantané portant 9 min consommées, restauré à chaud.
  DosingWindowSourceInputs t2{};
  t2.available = true;
  t2.usedMs = 540000UL;              // 9 min sur 10
  t2.windowElapsedAtWriteMs = 600000UL;
  t2.outageElapsedMs = 12000UL;      // reboot de 12 s
  t2.cyclesToday = 3;
  DosingWindowSourceInputs t1{};     // indisponible
  const DosingWindowChannelVerdict v =
      dosingWindowResolveChannel(t2, t1, /*nowMs=*/3000UL, /*fullMixingDelayMs=*/900000UL);

  TEST_ASSERT_EQUAL_UINT32_MESSAGE(540000UL, v.usedMs,
      "prealable : le budget doit bien etre restaure a 9 min");

  // Le verdict restauré alimente le chemin MANUEL.
  ManualInjectInputs in = manualInputsAllOk();
  in.usedMs = v.usedMs;              // <-- le lien que C10 exige
  const ManualInjectDecision d = evaluateManualInject(in);

  TEST_ASSERT_FALSE_MESSAGE(d.allowed,
      "une commande manuelle NE DOIT PAS contourner un budget horaire restaure");
  TEST_ASSERT_EQUAL_MESSAGE(ManualInjectRefusal::HourlyLimit, d.cause,
      "le refus doit etre impute a la limite HORAIRE, pas a une autre garde");
}

// Contraste : sans restauration (le defaut que ce lot ferme), la MEME commande
// passe. C'est la preuve que le test ci-dessus mesure bien la restauration.
void test_c10_without_restore_manual_would_pass(void) {
  ManualInjectInputs in = manualInputsAllOk();
  in.usedMs = 0;                     // budget neuf = comportement AVANT feature-136
  const ManualInjectDecision d = evaluateManualInject(in);
  TEST_ASSERT_TRUE_MESSAGE(d.allowed,
      "sans restauration la commande passait : c'est l'expo que feature-136 ferme");
}

int main(int argc, char** argv) {
  UNITY_BEGIN();
  RUN_TEST(test_01_warm_resume_rtc_conserves_used_ms);
  RUN_TEST(test_02_short_outage_nvs_rebuilds_window);
  RUN_TEST(test_03_long_outage_resets_window_but_conserves_cycles_under_24h);
  RUN_TEST(test_04_no_source_triggers_t3_single_window_then_nominal);
  RUN_TEST(test_05_clock_stepped_backward_unknown_elapsed_is_conservative);
  RUN_TEST(test_06_underflow_wraparound_is_exact);
  RUN_TEST(test_07_wrong_size_is_rejected_outright);
  RUN_TEST(test_08_rtc_marked_unavailable_is_never_consulted);
  RUN_TEST(test_09_zero_anchor_normalized_to_one);
  RUN_TEST(test_10_pump_on_at_write_arms_full_mixing_pause);
  RUN_TEST(test_11_restored_cycles_day_start_prevents_silent_rollover);
  RUN_TEST(test_12_worst_of_both_sources_retained_per_quantity);
  RUN_TEST(test_13_five_reboots_in_twenty_minutes_no_perpetual_penalty);
  RUN_TEST(test_14_restored_ring_blocks_21st_cycle_start);
  RUN_TEST(test_15_unknown_elapsed_t2_restores_fully_without_t3);
  RUN_TEST(test_16_expiry_threshold_is_inclusive_at_exact_boundary);
  RUN_TEST(test_c10_restored_budget_blocks_manual_injection);
  RUN_TEST(test_c10_without_restore_manual_would_pass);
  return UNITY_END();
}
