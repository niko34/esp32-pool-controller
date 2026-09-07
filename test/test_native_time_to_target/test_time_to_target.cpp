// =============================================================================
// Tests unitaires natifs — Indicateur « temps d'atteinte de cible »
// (time_to_target_logic, feature-096 v2.37.0)
// =============================================================================
// Tournent sur PC (env:native, Unity), HORS ESP32 / Arduino. Module PUR
// (ADR-0017) : l'appelant fournit l'heure, les minutes de circulation, la
// mesure et les seuils ; l'état vit dans une struct POD.
//
// LE HARNAIS TICKE À LA SECONDE, comme la coquille. Ce n'est pas un détail de
// confort : le cas « déjà à la cible » ne rend 0 minute QUE parce que le tick
// qui démarre la tenue tombe dans la MÊME minute de filtration que celui qui a
// posé t0. Un harnais qui ticquerait à la minute masquerait ce contrat.
//
// Les PARAMÈTRES viennent de constants.h / config.h (kTimeToTargetHoldMin,
// kTttMaxPlausibleE0*, kPhStartThresholdDefault) : une mutation des constantes
// de production doit être VUE par cette suite.
//
// ORGANISATION
//   A — Contrat de format figé (tailles, trivialité RTC, valeurs d'encodage)
//   B — Sceaux FNV-1a et anneau 7 jours
//   C — Rejeux de la journée du 04/09/2026 (fixture de référence de la spec)
//   D — Tenue : la sortie de bande remet la TENUE à zéro, JAMAIS le chronomètre
//   E — Départ : un seul t0 par jour civil, t0FiltMin jamais réécrit
//   F — Garde de vraisemblance sur e0
//   G — Drapeaux (consigne, boost, purge, coupure secteur, opérateur, quota)
//   H — Assertions sur les SOURCES : sens unique (C1) et base de temps
//   I — Rattrapage des journées orphelines (appareil éteint > 1 jour civil)
//
// CE QUE CETTE SUITE NE PROUVE PAS (cf. rapport) : l'effet réel de la RAM RTC
// au reboot, la persistance NVS, getPhDecisionValue() sur cible, et la
// composition dans time_to_target.cpp — hors les deux invariants de la
// section H, qui sont vérifiés sur le TEXTE des sources.
// =============================================================================

#include <unity.h>

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "time_to_target_logic.h"

// =============================================================================
// Contexte figé des rejeux — journée du 04/09/2026
// =============================================================================
static const uint32_t kD = 20260904UL;   // jour de la fixture
static const uint32_t kD1 = 20260905UL;  // lendemain

static const float kPhTarget = 7.20f;
static const float kPhBand = kPhStartThresholdDefault;  // 0,05 pH
static const float kOrpTarget = 530.0f;
static const float kOrpBand = 15.0f;  // ProtectionConfig::orpStartThreshold (défaut)

// Minutes de filtration écoulées à l'ouverture de la porte de mesure (07:47),
// la filtration ayant démarré à 07:00.
static const uint32_t kGateFiltMin = 47;
// Le signal lent de DÉCISION du pH n'existe qu'une fenêtre plus tard (07:57).
static const uint32_t kPhDecisionFiltMin = 57;

// =============================================================================
// Harnais de simulation
// =============================================================================
// `filtSec` n'avance QUE quand la circulation tourne : c'est la base de temps
// normative (minutes d'eau brassée, pas minutes murales). `wallSec` avance
// toujours — l'écart entre les deux est ce qui tue la mutation M2.
struct Sim {
  TttLive st;
  uint32_t filtSec;
  uint32_t wallSec;
  int nStarted;
  int nReached;
  int nMissed;
  int nImplausible;
  TttDay started;
  TttDay reached;
  TttDay missed;
  float rejected;
  float residual;
};

static void simInit(Sim& s, uint32_t filtStartMin) {
  memset(&s, 0, sizeof(s));
  tttLiveReset(s.st);
  s.filtSec = filtStartMin * 60UL;
  s.rejected = NAN;
  s.residual = NAN;
}

static TttInputs baseIn(void) {
  TttInputs in;
  in.date = kD;
  in.filtrationRunning = true;
  in.dayEnd = false;
  in.automatic = true;
  in.boostActive = false;
  in.unsettled = false;
  return in;
}

static TttInputs phIn(float value) {
  TttInputs in = baseIn();
  in.value = value;
  in.target = kPhTarget;
  in.configuredTarget = kPhTarget;
  in.band = kPhBand;
  in.maxPlausibleE0 = kTttMaxPlausibleE0Ph;
  return in;
}

static TttInputs orpIn(float value) {
  TttInputs in = baseIn();
  in.value = value;
  in.target = kOrpTarget;
  in.configuredTarget = kOrpTarget;
  in.band = kOrpBand;
  in.maxPlausibleE0 = kTttMaxPlausibleE0Orp;
  return in;
}

// UN tick d'une seconde. `filtTotalMin` est DÉRIVÉ du compteur de circulation :
// le test ne le fabrique jamais à la main.
static void tick1s(Sim& s, TttInputs in) {
  in.filtTotalMin = s.filtSec / 60UL;
  const TttVerdict v = tttTick(s.st, in);
  switch (v.event) {
    case TttEvent::kStarted:
      s.nStarted++;
      s.started = v.entry;
      break;
    case TttEvent::kReached:
      s.nReached++;
      s.reached = v.entry;
      s.residual = v.residual;
      break;
    case TttEvent::kMissed:
      s.nMissed++;
      s.missed = v.entry;
      s.residual = v.residual;
      break;
    case TttEvent::kImplausible:
      s.nImplausible++;
      s.rejected = v.rejected;
      break;
    default:
      break;
  }
  s.wallSec++;
  if (in.filtrationRunning) s.filtSec++;
}

static void runSeconds(Sim& s, const TttInputs& in, uint32_t seconds) {
  for (uint32_t i = 0; i < seconds; ++i) tick1s(s, in);
}

static void runMinutes(Sim& s, const TttInputs& in, uint32_t minutes) {
  runSeconds(s, in, minutes * 60UL);
}

// =============================================================================
// A — CONTRAT DE FORMAT FIGÉ
// =============================================================================
// `state` et `flags` partent en NVS et à l'UI : ce sont des valeurs de CONTRAT,
// pas des détails d'implémentation. Un renumérotage silencieux rendrait
// illisibles les journées déjà archivées.

void test_A_tailles_archivees(void) {
  TEST_ASSERT_EQUAL_UINT32(12u, (uint32_t)sizeof(TttDay));
  TEST_ASSERT_EQUAL_UINT32(28u, (uint32_t)sizeof(TttLive));
  TEST_ASSERT_EQUAL_UINT32(188u, (uint32_t)sizeof(TttHistory));
  TEST_ASSERT_EQUAL_UINT32(64u, (uint32_t)sizeof(TttRtcRecord));
}

// Le POURQUOI du « sans NSDMI » : un initialiseur de membre rendrait TttLive
// non trivial, donc DYNAMIQUEMENT initialisé au démarrage — et l'objet
// RTC_NOINIT serait effacé à chaque reset, c'est-à-dire l'exact contraire de ce
// que la RAM RTC doit apporter. Ce test est la garde de cette décision.
void test_A_types_rtc_triviaux(void) {
#if defined(__clang__) || defined(__GNUC__)
  TEST_ASSERT_TRUE_MESSAGE(__is_trivial(TttLive),
                           "TttLive doit rester TRIVIAL (RTC_NOINIT)");
  TEST_ASSERT_TRUE_MESSAGE(__is_trivial(TttRtcRecord),
                           "TttRtcRecord doit rester TRIVIAL (RTC_NOINIT)");
#else
  TEST_IGNORE_MESSAGE("__is_trivial indisponible sur ce compilateur");
#endif
}

void test_A_encodage_etats_et_drapeaux(void) {
  TEST_ASSERT_EQUAL_UINT8(0, kTttStateIdle);
  TEST_ASSERT_EQUAL_UINT8(1, kTttStateRunning);
  TEST_ASSERT_EQUAL_UINT8(2, kTttStateReached);
  TEST_ASSERT_EQUAL_UINT8(3, kTttStateMissed);
  // Champ de BITS : les cinq drapeaux doivent pouvoir cumuler.
  TEST_ASSERT_EQUAL_UINT8(0x01, kTttFlagTargetChanged);
  TEST_ASSERT_EQUAL_UINT8(0x02, kTttFlagBoost);
  TEST_ASSERT_EQUAL_UINT8(0x04, kTttFlagUnsettled);
  TEST_ASSERT_EQUAL_UINT8(0x08, kTttFlagPowerCycle);
  TEST_ASSERT_EQUAL_UINT8(0x10, kTttFlagOperatorOff);
  const uint8_t all = kTttFlagTargetChanged | kTttFlagBoost | kTttFlagUnsettled |
                      kTttFlagPowerCycle | kTttFlagOperatorOff;
  TEST_ASSERT_EQUAL_UINT8(0x1F, all);
  // Index de voie FIGÉS : ils indexent l'anneau NVS archivé.
  TEST_ASSERT_EQUAL_UINT8(0, kTttChannelPh);
  TEST_ASSERT_EQUAL_UINT8(1, kTttChannelOrp);
  TEST_ASSERT_EQUAL_UINT8(2, kTttChannelCount);
  TEST_ASSERT_EQUAL_UINT8(1, kTttHistorySchema);
  TEST_ASSERT_EQUAL_UINT32(0x4B504954UL, kTttRtcMagic);
}

void test_A_constantes_de_production(void) {
  TEST_ASSERT_EQUAL_UINT16(30, kTimeToTargetHoldMin);
  TEST_ASSERT_EQUAL_UINT8(7, kTimeToTargetHistoryDays);
  TEST_ASSERT_EQUAL_UINT16(60, kTimeToTargetOffDayEndMin);
  TEST_ASSERT_EQUAL_FLOAT(1.0f, kTttMaxPlausibleE0Ph);
  TEST_ASSERT_EQUAL_FLOAT(300.0f, kTttMaxPlausibleE0Orp);
}

// =============================================================================
// B — SCEAUX ET ANNEAU
// =============================================================================

void test_B_sceau_historique_aller_retour(void) {
  TttHistory h;
  tttHistorySeal(h, 600000UL);
  TEST_ASSERT_TRUE(tttHistoryIntact(h, sizeof(TttHistory)));
  TEST_ASSERT_EQUAL_UINT32(600000UL, h.serieStamp);
}

void test_B_sceau_historique_rejette_corruption_et_taille(void) {
  TttHistory h;
  tttHistorySeal(h, 600000UL);
  // Taille lue différente ⇒ rejet NET, jamais de réinterprétation.
  TEST_ASSERT_FALSE(tttHistoryIntact(h, sizeof(TttHistory) - 1));
  TEST_ASSERT_FALSE(tttHistoryIntact(h, 0));
  // Un seul octet retourné dans une journée archivée doit casser l'empreinte.
  h.days[kTttChannelOrp][3].minutes ^= 0x01;
  TEST_ASSERT_FALSE(tttHistoryIntact(h, sizeof(TttHistory)));
  // Schéma étranger ⇒ rejet (anneau vide, fail-closed).
  tttHistorySeal(h, 600000UL);
  h.schema = 2;
  TEST_ASSERT_FALSE(tttHistoryIntact(h, sizeof(TttHistory)));
}

void test_B_sceau_rtc_rejette_ram_aleatoire(void) {
  TttRtcRecord r;
  memset(&r, 0xA5, sizeof(r));  // RAM RTC après coupure secteur
  TEST_ASSERT_FALSE(tttRtcIntact(r));
  tttRtcSeal(r);
  TEST_ASSERT_TRUE(tttRtcIntact(r));
  r.magic ^= 0x01u;
  TEST_ASSERT_FALSE(tttRtcIntact(r));
  tttRtcSeal(r);
  r.live[kTttChannelPh].minutes ^= 0x01;
  TEST_ASSERT_FALSE(tttRtcIntact(r));
}

// Le scellement NORMALISE le remplissage : sans cela, deux écritures du même
// contenu produiraient deux empreintes selon les octets de pile résiduels.
void test_B_scellement_normalise_le_remplissage(void) {
  TttRtcRecord a;
  TttRtcRecord b;
  memset(&a, 0x00, sizeof(a));
  memset(&b, 0xFF, sizeof(b));
  for (uint8_t c = 0; c < kTttChannelCount; ++c) {
    a.live[c] = b.live[c] = TttLive{};
  }
  tttRtcSeal(a);
  tttRtcSeal(b);
  TEST_ASSERT_EQUAL_UINT32(a.crc, b.crc);
}

static TttDay day(uint32_t date, uint16_t minutes, uint8_t state, uint8_t flags) {
  TttDay d;
  d.date = date;
  d.e0 = 0.0f;
  d.minutes = minutes;
  d.state = state;
  d.flags = flags;
  return d;
}

void test_B_anneau_ajoute_et_decale(void) {
  TttHistory h;
  for (uint32_t i = 0; i < 7; ++i) {
    TEST_ASSERT_TRUE(tttHistoryAppend(h, kTttChannelPh,
                                      day(20260901UL + i, (uint16_t)(10 + i),
                                          kTttStateReached, 0)));
  }
  // La plus récente est en queue.
  TEST_ASSERT_EQUAL_UINT32(20260907UL, h.days[kTttChannelPh][6].date);
  TEST_ASSERT_EQUAL_UINT32(20260901UL, h.days[kTttChannelPh][0].date);
  // La 8e chasse la plus ancienne.
  tttHistoryAppend(h, kTttChannelPh, day(20260908UL, 99, kTttStateReached, 0));
  TEST_ASSERT_EQUAL_UINT32(20260902UL, h.days[kTttChannelPh][0].date);
  TEST_ASSERT_EQUAL_UINT32(20260908UL, h.days[kTttChannelPh][6].date);
  TEST_ASSERT_NULL(tttHistoryFind(h, kTttChannelPh, 20260901UL));
  // Les voies sont indépendantes.
  TEST_ASSERT_NULL(tttHistoryFind(h, kTttChannelOrp, 20260908UL));
}

// C'est ce qui permet à la CLÔTURE de réécrire l'entrée « en cours » posée à t0
// sans doubler la journée (exactement deux écritures NVS par jour et par voie).
void test_B_anneau_remplace_une_date_deja_presente(void) {
  TttHistory h;
  tttHistoryAppend(h, kTttChannelOrp, day(kD, 0, kTttStateRunning, 0));
  tttHistoryAppend(h, kTttChannelOrp, day(kD, 303, kTttStateReached, 0));
  int count = 0;
  for (uint8_t i = 0; i < kTimeToTargetHistoryDays; ++i) {
    if (h.days[kTttChannelOrp][i].date == kD) count++;
  }
  TEST_ASSERT_EQUAL_INT(1, count);
  const TttDay* d = tttHistoryFind(h, kTttChannelOrp, kD);
  TEST_ASSERT_NOT_NULL(d);
  TEST_ASSERT_EQUAL_UINT16(303, d->minutes);
  TEST_ASSERT_EQUAL_UINT8(kTttStateReached, d->state);
}

void test_B_anneau_refuse_date_nulle_et_voie_hors_bornes(void) {
  TttHistory h;
  TEST_ASSERT_FALSE(tttHistoryAppend(h, kTttChannelPh, day(0, 10, kTttStateReached, 0)));
  TEST_ASSERT_FALSE(tttHistoryAppend(h, kTttChannelCount, day(kD, 10, kTttStateReached, 0)));
  TEST_ASSERT_NULL(tttHistoryFind(h, kTttChannelPh, 0));
  TEST_ASSERT_NULL(tttHistoryFind(h, kTttChannelCount, kD));
}

// =============================================================================
// C — REJEUX DE LA JOURNÉE DU 04/09/2026
// =============================================================================
// Fixture de la spec : filtration depuis 07:00, porte de mesure ouverte à 07:47.
// Les profils sont exprimés en ÉCART À LA CONSIGNE, en fonction des minutes de
// filtration écoulées depuis 07:47 — donc ancrés sur l'HORLOGE, pas sur t0.
// C'est ce qui permet de faire varier t0 (07:47 pour l'ORP, 07:57 pour la
// grandeur de décision du pH) sans toucher à la courbe.

// ORP : plateau bas puis remontée, un FAUX DÉPART dans la bande vers 12:09 qui
// ne tient que 11 min, rebond, puis atteinte définitive à 12:50 (w = 303).
static float orpErrorAt(uint32_t w) {
  if (w < 262) return -45.0f + (float)w * 0.10f;  // −45 → −18,9 mV : hors bande
  if (w < 273) return -8.0f;                      // faux départ (11 min de tenue)
  if (w < 303) return -22.0f;                     // rebond hors bande
  return -6.0f;                                   // atteinte définitive
}

// pH : palier initial (la dose n'a pas encore agi), décrue lente, stagnation
// juste AU-DESSUS de la bande, puis entrée définitive à 11:40 (w = 233).
static float phErrorAt(uint32_t w) {
  if (w < 10) return 0.18f;
  if (w < 210) return 0.18f - (float)(w - 10) * 0.0005f;  // → +0,0805
  if (w < 233) return 0.06f;
  return 0.02f;
}

void test_C_rejeu_0409_orp_303_minutes(void) {
  Sim s;
  simInit(s, 0);  // filtration démarrée à 07:00
  // 07:00 → 07:47 : la porte de mesure est fermée, la grandeur n'EXISTE pas.
  runMinutes(s, orpIn(NAN), kGateFiltMin);
  TEST_ASSERT_EQUAL_UINT8(kTttStateIdle, s.st.state);
  TEST_ASSERT_EQUAL_INT(0, s.nStarted);

  for (uint32_t w = 0; w <= 400 && s.nReached == 0; ++w) {
    runMinutes(s, orpIn(kOrpTarget + orpErrorAt(w)), 1);
  }
  TEST_ASSERT_EQUAL_INT(1, s.nStarted);
  TEST_ASSERT_EQUAL_INT(1, s.nReached);
  TEST_ASSERT_EQUAL_INT(0, s.nMissed);
  TEST_ASSERT_EQUAL_UINT32(kGateFiltMin, s.st.t0FiltMin);
  TEST_ASSERT_FLOAT_WITHIN(0.5f, -45.0f, s.reached.e0);
  TEST_ASSERT_UINT16_WITHIN(5, 303, s.reached.minutes);
  TEST_ASSERT_EQUAL_UINT8(kTttStateReached, s.reached.state);
  TEST_ASSERT_EQUAL_UINT8(0, s.reached.flags);
  // L'écart résiduel à la validation de la tenue est RENSEIGNÉ (jamais NAN ici).
  TEST_ASSERT_FLOAT_WITHIN(0.5f, -6.0f, s.residual);
}

void test_C_rejeu_0409_ph_223_minutes_sur_la_grandeur_de_decision(void) {
  Sim s;
  simInit(s, 0);
  // Le pH de DÉCISION est un signal lent : il n'existe qu'une fenêtre après
  // l'ouverture de la porte. Rien avant 07:57 — et surtout pas 0.
  runMinutes(s, phIn(NAN), kPhDecisionFiltMin);
  for (uint32_t w = 10; w <= 400 && s.nReached == 0; ++w) {
    runMinutes(s, phIn(kPhTarget + phErrorAt(w)), 1);
  }
  TEST_ASSERT_EQUAL_INT(1, s.nStarted);
  TEST_ASSERT_EQUAL_INT(1, s.nReached);
  TEST_ASSERT_EQUAL_UINT32(kPhDecisionFiltMin, s.st.t0FiltMin);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.18f, s.reached.e0);
  TEST_ASSERT_UINT16_WITHIN(5, 223, s.reached.minutes);
}

// L'ARBITRAGE, rendu explicite : la spec annonçait 233 min pour le pH, chiffre
// mesuré sur la grandeur BRUTE disponible dès 07:47. Le KPI juge sur la
// grandeur de DÉCISION, disponible 10 min plus tard : 223. Même courbe, même
// instant d'atteinte (11:40) — seul t0 diffère. Les 10 minutes d'écart sont
// exactement la fenêtre de décision, et rien d'autre.
void test_C_rejeu_0409_ph_donnerait_233_depuis_le_brut(void) {
  Sim s;
  simInit(s, 0);
  runMinutes(s, phIn(NAN), kGateFiltMin);  // brut disponible dès 07:47
  for (uint32_t w = 0; w <= 400 && s.nReached == 0; ++w) {
    runMinutes(s, phIn(kPhTarget + phErrorAt(w)), 1);
  }
  TEST_ASSERT_EQUAL_INT(1, s.nReached);
  TEST_ASSERT_EQUAL_UINT32(kGateFiltMin, s.st.t0FiltMin);
  TEST_ASSERT_UINT16_WITHIN(5, 233, s.reached.minutes);
  TEST_ASSERT_EQUAL_UINT16(kPhDecisionFiltMin - kGateFiltMin, 10);
}

// =============================================================================
// D — LA TENUE
// =============================================================================

// Le test knife-edge de la feature. 29 minutes de tenue accomplies, la bande
// s'ouvre : la TENUE repart de zéro, le CHRONOMÈTRE continue. Le temps final
// est celui du DÉBUT de la seconde tenue (50), ni 10 (première entrée), ni 80
// (fin de la tenue).
void test_D_sortie_de_bande_a_29_min_remet_la_tenue_pas_le_chrono(void) {
  Sim s;
  simInit(s, 0);
  runMinutes(s, phIn(kPhTarget + 0.30f), 10);  // hors bande, t0 = 0
  TEST_ASSERT_EQUAL_INT(1, s.nStarted);
  TEST_ASSERT_EQUAL_UINT32(0, s.st.t0FiltMin);

  runMinutes(s, phIn(kPhTarget + 0.01f), 30);  // dans la bande : minutes 10..39
  TEST_ASSERT_EQUAL_INT(0, s.nReached);        // 29 min de tenue seulement
  TEST_ASSERT_EQUAL_UINT32(10, s.st.holdStartFiltMin);
  TEST_ASSERT_EQUAL_UINT8(1, s.st.holding);

  runMinutes(s, phIn(kPhTarget + 0.30f), 10);  // minutes 40..49 : hors bande
  TEST_ASSERT_EQUAL_INT(0, s.nReached);
  TEST_ASSERT_EQUAL_UINT8(0, s.st.holding);
  TEST_ASSERT_EQUAL_UINT32(0, s.st.holdStartFiltMin);  // tenue remise à zéro
  TEST_ASSERT_EQUAL_UINT32(0, s.st.t0FiltMin);         // chronomètre INTACT
  TEST_ASSERT_EQUAL_UINT8(kTttStateRunning, s.st.state);
  TEST_ASSERT_EQUAL_UINT16(49, s.st.minutes);  // il a continué de courir

  runMinutes(s, phIn(kPhTarget + 0.01f), 31);  // tenue complète depuis 50
  TEST_ASSERT_EQUAL_INT(1, s.nReached);
  TEST_ASSERT_EQUAL_UINT16(50, s.reached.minutes);
}

// La frontière exacte : 29 min ne suffisent pas, 30 min suffisent.
void test_D_frontiere_29_versus_30_minutes_de_tenue(void) {
  Sim a;
  simInit(a, 0);
  runMinutes(a, phIn(kPhTarget + 0.30f), 1);
  runMinutes(a, phIn(kPhTarget + 0.01f), kTimeToTargetHoldMin - 1);
  TEST_ASSERT_EQUAL_INT(0, a.nReached);

  Sim b;
  simInit(b, 0);
  runMinutes(b, phIn(kPhTarget + 0.30f), 1);
  runMinutes(b, phIn(kPhTarget + 0.01f), kTimeToTargetHoldMin + 1);
  TEST_ASSERT_EQUAL_INT(1, b.nReached);
  TEST_ASSERT_EQUAL_UINT16(1, b.reached.minutes);  // début de tenue, pas fin
}

// Une mesure NON FINIE vaut SORTIE DE BANDE (C3) : jamais 0, jamais « dans la
// bande ». Une purge longue en pleine tenue ne doit ni valider ni redémarrer.
void test_D_nan_prolonge_remet_la_tenue_sans_arreter_le_chrono(void) {
  Sim s;
  simInit(s, 0);
  runMinutes(s, phIn(kPhTarget + 0.30f), 5);
  runMinutes(s, phIn(kPhTarget + 0.01f), 20);  // 20 min de tenue
  TEST_ASSERT_EQUAL_UINT8(1, s.st.holding);

  runMinutes(s, phIn(NAN), 40);  // purge : la mesure n'existe plus
  TEST_ASSERT_EQUAL_INT(0, s.nReached);
  TEST_ASSERT_EQUAL_UINT8(0, s.st.holding);
  TEST_ASSERT_EQUAL_UINT8(kTttStateRunning, s.st.state);
  TEST_ASSERT_EQUAL_UINT32(0, s.st.t0FiltMin);
  TEST_ASSERT_EQUAL_UINT16(64, s.st.minutes);  // le chronomètre a couru

  runMinutes(s, phIn(kPhTarget + 0.01f), 31);
  TEST_ASSERT_EQUAL_INT(1, s.nReached);
  TEST_ASSERT_EQUAL_UINT16(65, s.reached.minutes);  // tenue redémarrée à 65
}

// Une bande absente ou nulle ne vaut pas « tout est dans la bande ».
void test_D_bande_non_finie_ou_nulle_vaut_hors_bande(void) {
  Sim s;
  simInit(s, 0);
  runMinutes(s, phIn(kPhTarget + 0.30f), 1);
  TttInputs in = phIn(kPhTarget);  // pile sur la consigne
  in.band = 0.0f;
  runMinutes(s, in, 60);
  TEST_ASSERT_EQUAL_INT(0, s.nReached);
  in.band = NAN;
  runMinutes(s, in, 60);
  TEST_ASSERT_EQUAL_INT(0, s.nReached);
}

void test_D_arret_de_circulation_sans_tenue_est_une_non_atteinte(void) {
  Sim s;
  simInit(s, 0);
  runMinutes(s, orpIn(kOrpTarget - 45.0f), 120);
  TEST_ASSERT_EQUAL_UINT8(kTttStateRunning, s.st.state);

  TttInputs off = orpIn(NAN);
  off.filtrationRunning = false;
  off.dayEnd = true;  // la coquille a laissé passer kTimeToTargetOffDayEndMin
  runSeconds(s, off, 2);

  TEST_ASSERT_EQUAL_INT(1, s.nMissed);
  TEST_ASSERT_EQUAL_INT(0, s.nReached);
  TEST_ASSERT_EQUAL_UINT8(kTttStateMissed, s.missed.state);
  TEST_ASSERT_EQUAL_UINT16(120, s.missed.minutes);
  TEST_ASSERT_EQUAL_UINT8(0, s.missed.flags);  // non-atteinte NUE
  TEST_ASSERT_TRUE(isnan(s.residual));         // mesure absente ⇒ NAN, pas 0
}

// Après une atteinte, un rebond ne rouvre pas la journée : l'état est FIGÉ
// jusqu'à minuit.
void test_D_apres_atteinte_le_rebond_ne_rouvre_pas_la_journee(void) {
  Sim s;
  simInit(s, 0);
  runMinutes(s, phIn(kPhTarget + 0.30f), 1);
  runMinutes(s, phIn(kPhTarget + 0.01f), 32);
  TEST_ASSERT_EQUAL_INT(1, s.nReached);
  const uint16_t reached = s.reached.minutes;

  runMinutes(s, phIn(kPhTarget + 0.50f), 200);  // grosse dérive
  TEST_ASSERT_EQUAL_INT(1, s.nReached);
  TEST_ASSERT_EQUAL_INT(0, s.nMissed);
  TEST_ASSERT_EQUAL_UINT8(kTttStateReached, s.st.state);
  TEST_ASSERT_EQUAL_UINT16(reached, s.st.minutes);
}

// =============================================================================
// E — LE DÉPART
// =============================================================================

// Un seul départ par jour civil : après clôture, aucun nouveau t0 le même jour.
void test_E_un_seul_depart_par_jour_civil(void) {
  Sim s;
  simInit(s, 0);
  runMinutes(s, orpIn(kOrpTarget - 45.0f), 60);
  TttInputs off = orpIn(NAN);
  off.filtrationRunning = false;
  off.dayEnd = true;
  runSeconds(s, off, 2);
  TEST_ASSERT_EQUAL_INT(1, s.nMissed);

  // La filtration repart le même jour, la mesure revient : AUCUN nouveau t0.
  runMinutes(s, orpIn(kOrpTarget - 45.0f), 240);
  TEST_ASSERT_EQUAL_INT(1, s.nStarted);
  TEST_ASSERT_EQUAL_INT(1, s.nMissed);
  TEST_ASSERT_EQUAL_INT(0, s.nReached);
  TEST_ASSERT_EQUAL_UINT8(kTttStateMissed, s.st.state);
}

// t0FiltMin est un CHAMP DISTINCT, jamais réécrit : ni par une re-purge, ni par
// une sortie de bande, ni par un retour de mesure.
void test_E_t0_jamais_reecrit_par_les_repurges(void) {
  Sim s;
  simInit(s, 47);
  runMinutes(s, phIn(kPhTarget + 0.30f), 5);
  TEST_ASSERT_EQUAL_UINT32(47, s.st.t0FiltMin);
  for (int cycle = 0; cycle < 4; ++cycle) {
    runMinutes(s, phIn(NAN), 12);                // purge
    runMinutes(s, phIn(kPhTarget + 0.30f), 18);  // mesure revenue, hors bande
    TEST_ASSERT_EQUAL_UINT32(47, s.st.t0FiltMin);
    TEST_ASSERT_EQUAL_INT(1, s.nStarted);
  }
  TEST_ASSERT_EQUAL_UINT16(124, s.st.minutes);  // compté sans la moindre coupure
}

// Déjà à la cible à t0 : la tenue démarre au tick SUIVANT, dans la MÊME minute
// de filtration ⇒ temps final 0.
void test_E_deja_a_la_cible_donne_zero_minute(void) {
  Sim s;
  simInit(s, 100);
  runMinutes(s, phIn(kPhTarget), 31);
  TEST_ASSERT_EQUAL_INT(1, s.nStarted);
  TEST_ASSERT_EQUAL_INT(1, s.nReached);
  TEST_ASSERT_EQUAL_UINT16(0, s.reached.minutes);
  TEST_ASSERT_EQUAL_UINT32(100, s.st.t0FiltMin);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, s.reached.e0);
}

// Minuit en filtration MANUELLE ON : la journée qui s'achève se clôt en « non
// atteinte » et un NOUVEAU t0 est posé au premier instant valide après minuit.
// Le chronomètre ne traverse JAMAIS minuit.
void test_E_minuit_en_filtration_manuelle_repart_a_zero(void) {
  Sim s;
  simInit(s, 500);
  runMinutes(s, orpIn(kOrpTarget - 45.0f), 100);
  TEST_ASSERT_EQUAL_INT(1, s.nStarted);
  TEST_ASSERT_EQUAL_UINT32(500, s.st.t0FiltMin);

  TttInputs next = orpIn(kOrpTarget - 45.0f);
  next.date = kD1;
  runSeconds(s, next, 2);  // minuit
  TEST_ASSERT_EQUAL_INT(1, s.nMissed);
  TEST_ASSERT_EQUAL_UINT32(kD, s.missed.date);
  TEST_ASSERT_EQUAL_UINT16(100, s.missed.minutes);

  runMinutes(s, next, 10);
  TEST_ASSERT_EQUAL_INT(2, s.nStarted);
  TEST_ASSERT_EQUAL_UINT32(kD1, s.started.date);
  TEST_ASSERT_EQUAL_UINT32(600, s.st.t0FiltMin);  // t0 au premier instant valide
  TEST_ASSERT_EQUAL_UINT16(10, s.st.minutes);
}

// Heure inexploitable : l'indicateur est SUSPENDU. Rien n'est daté faux.
void test_E_heure_perdue_suspend_sans_rien_dater(void) {
  Sim s;
  simInit(s, 0);
  runMinutes(s, orpIn(kOrpTarget - 45.0f), 30);
  TEST_ASSERT_EQUAL_INT(1, s.nStarted);

  TttInputs blind = orpIn(kOrpTarget - 45.0f);
  blind.date = 0;
  runMinutes(s, blind, 60);
  TEST_ASSERT_EQUAL_UINT8(kTttStateIdle, s.st.state);
  TEST_ASSERT_EQUAL_UINT32(0, s.st.date);
  TEST_ASSERT_EQUAL_INT(1, s.nStarted);  // aucune écriture datée faux
  TEST_ASSERT_EQUAL_INT(0, s.nMissed);
  TEST_ASSERT_EQUAL_INT(0, s.nReached);
}

// Pas d'eau, pas de départ : t0 exige la circulation.
void test_E_pas_de_depart_hors_circulation(void) {
  Sim s;
  simInit(s, 0);
  TttInputs off = orpIn(kOrpTarget - 45.0f);
  off.filtrationRunning = false;
  runSeconds(s, off, 3600);
  TEST_ASSERT_EQUAL_INT(0, s.nStarted);
  TEST_ASSERT_EQUAL_UINT8(kTttStateIdle, s.st.state);
}

// La BASE DE TEMPS est le compteur de circulation, en LECTURE SEULE : deux
// journées de durées MURALES très différentes mais de mêmes minutes d'eau
// donnent le MÊME résultat. Le module n'a aucune horloge cachée.
void test_E_base_de_temps_insensible_au_temps_mural(void) {
  Sim direct;
  simInit(direct, 0);
  runMinutes(direct, orpIn(kOrpTarget - 45.0f), 60);
  runMinutes(direct, orpIn(kOrpTarget - 2.0f), 31);

  Sim hache;
  simInit(hache, 0);
  TttInputs pause = orpIn(NAN);
  pause.filtrationRunning = false;  // 3 h murales SANS eau, entrecoupées
  for (int k = 0; k < 6; ++k) {
    runMinutes(hache, orpIn(kOrpTarget - 45.0f), 10);
    runMinutes(hache, pause, 30);
  }
  runMinutes(hache, orpIn(kOrpTarget - 2.0f), 31);

  TEST_ASSERT_EQUAL_INT(1, direct.nReached);
  TEST_ASSERT_EQUAL_INT(1, hache.nReached);
  TEST_ASSERT_EQUAL_UINT16(direct.reached.minutes, hache.reached.minutes);
  TEST_ASSERT_EQUAL_UINT16(60, hache.reached.minutes);
  // Le temps MURAL, lui, a plus que doublé — preuve que le test discrimine.
  TEST_ASSERT_TRUE(hache.wallSec > 2 * direct.wallSec);
}

// Compteur de filtration en RECUL (NVS `pool-daily` effacée) : une durée
// négative n'existe pas — 0 plutôt qu'un repliement à 4 milliards.
void test_E_compteur_en_recul_ne_se_replie_pas(void) {
  TttLive st;
  tttLiveReset(st);
  st.t0FiltMin = 500;
  TEST_ASSERT_EQUAL_UINT16(0, tttUpdateElapsedMinutes(st, 0));
  TEST_ASSERT_EQUAL_UINT16(0, tttUpdateElapsedMinutes(st, 499));
  TEST_ASSERT_EQUAL_UINT16(1, tttUpdateElapsedMinutes(st, 501));
}

// `minutes` est un uint16 qui porte une JOURNÉE : borné à 1440 à l'insertion.
void test_E_minutes_bornees_a_1440(void) {
  TttLive st;
  tttLiveReset(st);
  st.t0FiltMin = 10;
  TEST_ASSERT_EQUAL_UINT16(1440, tttUpdateElapsedMinutes(st, 10 + 1440));
  TEST_ASSERT_EQUAL_UINT16(1440, tttUpdateElapsedMinutes(st, 10 + 1441));
  TEST_ASSERT_EQUAL_UINT16(1440, tttUpdateElapsedMinutes(st, 4000000000UL));
  TEST_ASSERT_EQUAL_UINT16(1439, tttUpdateElapsedMinutes(st, 10 + 1439));

  // Et sur le chemin de CLÔTURE, celui qui part réellement en NVS.
  Sim s;
  simInit(s, 0);
  s.st.date = kD;
  s.st.state = kTttStateRunning;
  s.st.t0FiltMin = 0;
  s.st.watchTarget = kOrpTarget;
  s.filtSec = 5000UL * 60UL;  // compteur multi-jours (filtration manuelle ON)
  TttInputs off = orpIn(NAN);
  off.filtrationRunning = false;
  off.dayEnd = true;
  runSeconds(s, off, 2);
  TEST_ASSERT_EQUAL_INT(1, s.nMissed);
  TEST_ASSERT_EQUAL_UINT16(1440, s.missed.minutes);
}

// feature-105 (A4) — COMPTEUR DE FILTRATION EN RECUL PENDANT UNE TENUE
// (time_to_target_logic.cpp : `if (in.filtTotalMin < st.holdStartFiltMin)`)
//
// filtTotalMin est un compteur MONOTONE de minutes de filtration, persisté en
// NVS (`pool-daily`). Il ne se replie pas (uint32 en minutes = 8 000 ans), mais
// il peut RECULER : effacement de la NVS, restauration d'usine ou de
// configuration en cours de tenue. Sans la garde, la soustraction
// `filtTotalMin - holdStartFiltMin` se fait en NON SIGNÉ : elle sous-déborde en
// une valeur énorme, franchit instantanément kTimeToTargetHoldMin, et la tenue
// se croit ACCOMPLIE alors qu'elle vient à peine de commencer.
//
// Le comportement correct est de RELANCER la tenue : cela ALLONGE le temps
// rendu, jamais ne le raccourcit. L'indicateur est purement informatif — aucune
// sortie de ce module n'est lue par pump_controller (cf. section H).
void test_D_compteur_en_recul_relance_la_tenue(void) {
  Sim s;
  simInit(s, 100);  // la filtration a déjà 100 minutes au compteur
  TttInputs in = orpIn(kOrpTarget);  // dans la bande dès le départ

  runSeconds(s, in, 2);  // t0 posé, tenue ouverte
  TEST_ASSERT_EQUAL_INT(1, s.nStarted);
  TEST_ASSERT_EQUAL_UINT8(kTttStateRunning, s.st.state);
  TEST_ASSERT_EQUAL_UINT8(1, s.st.holding);
  TEST_ASSERT_EQUAL_UINT32(100u, s.st.holdStartFiltMin);

  runMinutes(s, in, 10);  // 10 minutes de tenue acquises, loin des 30 requises
  TEST_ASSERT_EQUAL_UINT32(110u, s.st.holdStartFiltMin + 10u);
  TEST_ASSERT_EQUAL_INT(0, s.nReached);

  // Effacement de la NVS `pool-daily` : le compteur repart de zéro.
  s.filtSec = 0;
  runSeconds(s, in, 1);
  // La tenue est RÉALIGNÉE sur le nouveau compteur…
  TEST_ASSERT_EQUAL_UINT32(0u, s.st.holdStartFiltMin);
  TEST_ASSERT_EQUAL_UINT8(1, s.st.holding);
  // …et surtout : elle n'est PAS déclarée accomplie. C'est ce que tue la
  // suppression de la branche (sous-débordement non signé ⇒ atteinte immédiate).
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, s.nReached,
                                "un compteur en recul ne doit PAS valider la tenue");
  TEST_ASSERT_EQUAL_UINT8(kTttStateRunning, s.st.state);

  // La tenue repart bien de ZÉRO : 29 minutes après le recul, toujours rien.
  runMinutes(s, in, 29);
  TEST_ASSERT_EQUAL_INT(0, s.nReached);
  TEST_ASSERT_EQUAL_UINT8(kTttStateRunning, s.st.state);

  // Et elle aboutit une fois les 30 minutes pleines écoulées depuis le recul.
  runMinutes(s, in, 2);
  TEST_ASSERT_EQUAL_INT(1, s.nReached);
  TEST_ASSERT_EQUAL_UINT8(kTttStateReached, s.st.state);
}

// Un recul ne fait jamais GAGNER du temps : la tenue redemande 30 minutes
// PLEINES de circulation après le recul. La référence sans recul aboutit
// strictement plus tôt en temps écoulé.
void test_D_recul_ne_fait_jamais_gagner_de_temps(void) {
  TttInputs in = orpIn(kOrpTarget);

  // Référence : aucun recul.
  Sim ref;
  simInit(ref, 100);
  runSeconds(ref, in, 2);
  runMinutes(ref, in, 31);
  TEST_ASSERT_EQUAL_INT(1, ref.nReached);
  const uint32_t refWall = ref.wallSec;

  // Même journée, mais le compteur recule de 20 minutes après 10 de tenue
  // (restauration de configuration : `pool-daily` revient à un état antérieur).
  Sim rec;
  simInit(rec, 100);
  runSeconds(rec, in, 2);
  runMinutes(rec, in, 10);
  const uint32_t wallAvantRecul = rec.wallSec;
  rec.filtSec = 80UL * 60UL;  // 80 < holdStartFiltMin = 100
  runSeconds(rec, in, 1);
  TEST_ASSERT_EQUAL_UINT32(80u, rec.st.holdStartFiltMin);
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, rec.nReached,
                                "recul de 20 min : la tenue ne doit PAS aboutir");

  runMinutes(rec, in, 31);
  TEST_ASSERT_EQUAL_INT(1, rec.nReached);
  // Au moins 30 minutes de circulation se sont écoulées DEPUIS le recul.
  TEST_ASSERT_TRUE_MESSAGE(rec.wallSec - wallAvantRecul >= 30UL * 60UL,
                           "la tenue a abouti en moins de 30 min apres le recul");
  // NOTE (revue feature-105) : une comparaison `rec.wallSec > refWall` figurait ici.
  // Elle était TAUTOLOGIQUE — `rec` exécute 601 ticks de plus que `ref` par
  // construction du harnais, elle ne pouvait donc jamais échouer. Retirée : une
  // assertion qui ne peut pas mourir n'est pas une assertion. Le travail est fait
  // par celle qui précède (>= 30 min pleines depuis le recul), laquelle tue bien
  // le mutant du sous-débordement.
}

// =============================================================================
// D/E bis — bug-106 : LE 0 D'UN COMPTEUR EN RECUL SE DÉCLARE
// =============================================================================
// LE MODE DE NUISANCE. `minutes` est une DURÉE : son domaine ne peut pas porter
// l'absence de durée. Quand `filtTotalMin` recule (NVS `pool-daily` effacée,
// restauration d'usine ou de configuration), la soustraction n'a plus de sens
// et l'on rend 0 — mais 0 minute de filtration est aussi la MEILLEURE journée
// possible (« la cible était atteinte dès le départ »). La journée s'archivait
// donc comme un RECORD, indiscernable d'une vraie atteinte immédiate. Ce
// n'était pas une perte d'information : c'était une donnée fausse et FAVORABLE,
// qui tire vers le bas la moyenne servant à comparer deux versions du firmware.
//
// Le correctif pose kTttFlagCounterReset (0x20) aux TROIS branches de recul.
// Chaque test ci-dessous en isole UNE : les trois mutants (drapeau retiré site
// par site) doivent mourir séparément, sinon un site est en réalité non couvert.
//
//   site 1 — tttUpdateElapsedMinutes (filtTotalMin < t0FiltMin) : le chemin VIF,
//            celui qui alimente l'instantané servi à l'UI ;
//   site 2 — tttTick, tenue en cours (filtTotalMin < holdStartFiltMin) : recul
//            survenu APRÈS t0, donc invisible du site 1 ;
//   site 3 — tttTick, archivage (holdStartFiltMin < t0FiltMin) : le site nommé
//            par la spec, celui qui écrit réellement en NVS.

void test_bug106_encodage_du_bit_de_recul(void) {
  // Valeur de CONTRAT (archivée en NVS, servie à l'UI) et bit DISJOINT des cinq
  // autres : une journée peut cumuler « recul » et « boost », par exemple.
  TEST_ASSERT_EQUAL_UINT8(0x20, kTttFlagCounterReset);
  TEST_ASSERT_EQUAL_UINT8(0, kTttFlagCounterReset & kTttFlagTargetChanged);
  TEST_ASSERT_EQUAL_UINT8(0, kTttFlagCounterReset & kTttFlagBoost);
  TEST_ASSERT_EQUAL_UINT8(0, kTttFlagCounterReset & kTttFlagUnsettled);
  TEST_ASSERT_EQUAL_UINT8(0, kTttFlagCounterReset & kTttFlagPowerCycle);
  TEST_ASSERT_EQUAL_UINT8(0, kTttFlagCounterReset & kTttFlagOperatorOff);
}

// --- SITE 1 : le chemin vif -------------------------------------------------
void test_bug106_site1_vif_le_zero_rendu_porte_le_drapeau(void) {
  TttLive st;
  tttLiveReset(st);
  st.t0FiltMin = 500;

  // Recul franc : le compteur repart de zéro (effacement NVS `pool-daily`).
  TEST_ASSERT_EQUAL_UINT16(0, tttUpdateElapsedMinutes(st, 0));
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(kTttFlagCounterReset,
      st.flags & kTttFlagCounterReset,
      "le 0 rendu par un compteur en recul ne se declare pas : la journee "
      "s'archive comme la meilleure possible");

  // Recul d'UNE minute : même événement, même drapeau — la frontière compte.
  TttLive st2;
  tttLiveReset(st2);
  st2.t0FiltMin = 500;
  TEST_ASSERT_EQUAL_UINT16(0, tttUpdateElapsedMinutes(st2, 499));
  TEST_ASSERT_EQUAL_UINT8(kTttFlagCounterReset, st2.flags & kTttFlagCounterReset);
}

void test_bug106_site1_aucun_recul_ne_pose_rien(void) {
  // LA distinction qui fait tout l'objet du bug : un compteur qui n'a PAS
  // reculé ne porte jamais le bit, y compris à la frontière exacte
  // (filtTotalMin == t0FiltMin, soit 0 minute LÉGITIME) et au plafond 1440.
  TttLive st;
  tttLiveReset(st);
  st.t0FiltMin = 500;
  TEST_ASSERT_EQUAL_UINT16(0, tttUpdateElapsedMinutes(st, 500));  // 0 minute LÉGITIME
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, st.flags & kTttFlagCounterReset,
      "faux positif : une journee atteinte a t=0 est signalee comme invalidee");
  TEST_ASSERT_EQUAL_UINT16(1, tttUpdateElapsedMinutes(st, 501));
  TEST_ASSERT_EQUAL_UINT16(1440, tttUpdateElapsedMinutes(st, 500 + 1440));
  TEST_ASSERT_EQUAL_UINT16(1440, tttUpdateElapsedMinutes(st, 4000000000UL));
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, st.flags,
      "un compteur qui AVANCE ne doit poser aucun drapeau");
}

void test_bug106_site1_le_drapeau_ne_pietine_pas_les_autres(void) {
  // Le bit s'AJOUTE (|=). Un `=` effacerait les circonstances déjà relevées
  // dans la journée (consigne modifiée, boost, purge, coupure secteur).
  TttLive st;
  tttLiveReset(st);
  st.t0FiltMin = 500;
  st.flags = (uint8_t)(kTttFlagBoost | kTttFlagPowerCycle);
  TEST_ASSERT_EQUAL_UINT16(0, tttUpdateElapsedMinutes(st, 100));
  TEST_ASSERT_EQUAL_UINT8(kTttFlagBoost, st.flags & kTttFlagBoost);
  TEST_ASSERT_EQUAL_UINT8(kTttFlagPowerCycle, st.flags & kTttFlagPowerCycle);
  TEST_ASSERT_EQUAL_UINT8(kTttFlagCounterReset, st.flags & kTttFlagCounterReset);
  // Idempotent : deux reculs de suite ne cumulent pas deux fois le même bit.
  TEST_ASSERT_EQUAL_UINT16(0, tttUpdateElapsedMinutes(st, 50));
  TEST_ASSERT_EQUAL_UINT8(
      (uint8_t)(kTttFlagBoost | kTttFlagPowerCycle | kTttFlagCounterReset),
      st.flags);
}

// --- SITE 2 : recul pendant la tenue, invisible du site 1 -------------------
void test_bug106_site2_recul_pendant_la_tenue_pose_le_drapeau(void) {
  // Montage ISOLANT : le compteur reste AU-DESSUS de t0FiltMin (site 1 muet)
  // mais passe SOUS holdStartFiltMin. C'est le recul d'une restauration de
  // configuration survenue après le départ du chronomètre.
  Sim s;
  simInit(s, 0);
  s.st.date = kD;
  s.st.state = kTttStateRunning;
  s.st.t0FiltMin = 0;           // 50 >= 0 → tttUpdateElapsedMinutes ne voit RIEN
  s.st.holdStartFiltMin = 100;  // 50 < 100 → la branche de tenue, elle, voit
  s.st.holding = 1;
  s.st.flags = 0;
  s.st.watchTarget = kOrpTarget;
  s.filtSec = 50UL * 60UL;

  runSeconds(s, orpIn(kOrpTarget), 1);

  TEST_ASSERT_EQUAL_UINT32_MESSAGE(50u, s.st.holdStartFiltMin,
      "la tenue n'a pas ete realignee : ce n'est pas la branche visee");
  TEST_ASSERT_EQUAL_INT(0, s.nReached);
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(kTttFlagCounterReset,
      s.st.flags & kTttFlagCounterReset,
      "recul entre t0 et le debut de tenue non signale : la journee restera "
      "presentable alors que sa base de temps a saute");
}

void test_bug106_site2_tenue_normale_ne_pose_rien(void) {
  // Le contre-cas du site 2 : une tenue qui progresse normalement pendant
  // 29 minutes ne pose aucun drapeau (mutant « bit posé inconditionnellement »).
  Sim s;
  simInit(s, 100);
  TttInputs in = orpIn(kOrpTarget);
  runSeconds(s, in, 2);
  runMinutes(s, in, 29);
  TEST_ASSERT_EQUAL_INT(0, s.nReached);
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, s.st.flags & kTttFlagCounterReset,
      "faux positif : une tenue normale est signalee comme compteur en recul");
}

// --- SITE 3 : l'archivage, le site nommé par la spec ------------------------
void test_bug106_site3_archivage_journee_invalidee_est_distinguable(void) {
  // Montage ISOLANT : holdStartFiltMin < t0FiltMin (séquelle d'un recul
  // antérieur) MAIS filtTotalMin >= t0FiltMin, donc le site 1 reste muet et le
  // site 2 aussi (filtTotalMin >= holdStartFiltMin). Seul le repli
  // `reached = 0` de l'archivage peut poser le bit.
  Sim s;
  simInit(s, 0);
  s.st.date = kD;
  s.st.state = kTttStateRunning;
  s.st.t0FiltMin = 100;
  s.st.holdStartFiltMin = 80;   // < t0FiltMin : le repli à 0 va s'appliquer
  s.st.holding = 1;
  s.st.flags = 0;
  s.st.watchTarget = kOrpTarget;
  s.filtSec = 110UL * 60UL;     // 110 >= 100 (site 1 muet), 110-80 = 30 → atteinte

  runSeconds(s, orpIn(kOrpTarget), 1);

  TEST_ASSERT_EQUAL_INT_MESSAGE(1, s.nReached, "l'atteinte n'a pas eu lieu : "
      "le montage ne passe pas par le repli d'archivage");
  TEST_ASSERT_EQUAL_UINT8(kTttStateReached, s.st.state);
  // La valeur archivée reste 0 (une durée négative n'existe pas)…
  TEST_ASSERT_EQUAL_UINT16(0, s.reached.minutes);
  // …mais elle est DÉSORMAIS distinguable d'une atteinte immédiate légitime.
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(kTttFlagCounterReset,
      s.reached.flags & kTttFlagCounterReset,
      "journee archivee a 0 minute SANS drapeau : elle passera pour la "
      "meilleure journee jamais observee");
  // Et le drapeau part bien dans l'ENTRÉE (donc en NVS), pas seulement dans
  // l'état vivant.
  TEST_ASSERT_EQUAL_UINT8(s.st.flags & kTttFlagCounterReset,
                          s.reached.flags & kTttFlagCounterReset);
}

void test_bug106_site3_atteinte_legitime_a_t0_ne_porte_pas_le_drapeau(void) {
  // LA distinction qui fait tout l'objet du bug, sur le chemin d'archivage :
  // un bassin déjà à la cible à t0 vaut LÉGITIMEMENT 0 minute. Cette journée
  // doit rester nue — sinon le correctif fabrique le faux positif symétrique
  // et exclut des moyennes les meilleures journées réelles.
  Sim s;
  simInit(s, 100);
  runMinutes(s, phIn(kPhTarget), 31);
  TEST_ASSERT_EQUAL_INT(1, s.nStarted);
  TEST_ASSERT_EQUAL_INT(1, s.nReached);
  TEST_ASSERT_EQUAL_UINT16(0, s.reached.minutes);
  TEST_ASSERT_EQUAL_UINT32(100, s.st.t0FiltMin);
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, s.reached.flags & kTttFlagCounterReset,
      "faux positif : une journee atteinte a t=0 est marquee « compteur en "
      "recul » — les meilleures journees reelles sortiraient des moyennes");
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, s.reached.flags,
      "une journee nominale ne doit porter AUCUN drapeau");
}

void test_bug106_deux_journees_a_zero_minute_ne_se_ressemblent_plus(void) {
  // La comparaison FRONTALE, celle que la spec demande : deux journées
  // archivées à 0 minute, l'une légitime, l'autre invalidée par un recul.
  // Avant le correctif, leurs enregistrements étaient IDENTIQUES.
  Sim legitime;
  simInit(legitime, 100);
  runMinutes(legitime, phIn(kPhTarget), 31);

  Sim invalidee;
  simInit(invalidee, 0);
  invalidee.st.date = kD;
  invalidee.st.state = kTttStateRunning;
  invalidee.st.t0FiltMin = 100;
  invalidee.st.holdStartFiltMin = 80;
  invalidee.st.holding = 1;
  invalidee.st.flags = 0;
  invalidee.st.watchTarget = kPhTarget;
  invalidee.filtSec = 110UL * 60UL;
  runSeconds(invalidee, phIn(kPhTarget), 1);

  TEST_ASSERT_EQUAL_INT(1, legitime.nReached);
  TEST_ASSERT_EQUAL_INT(1, invalidee.nReached);
  // Mêmes minutes, même état : c'est bien le cas ambigu d'avant bug-106…
  TEST_ASSERT_EQUAL_UINT16(0, legitime.reached.minutes);
  TEST_ASSERT_EQUAL_UINT16(0, invalidee.reached.minutes);
  TEST_ASSERT_EQUAL_UINT8(legitime.reached.state, invalidee.reached.state);
  // …et les deux enregistrements ne sont plus interchangeables.
  TEST_ASSERT_TRUE_MESSAGE(legitime.reached.flags != invalidee.reached.flags,
      "les deux journees a 0 minute restent INDISCERNABLES : le bug est intact");
}

void test_bug106_le_drapeau_survit_a_la_cloture_en_non_atteinte(void) {
  // Une journée dont le compteur a reculé et qui se clôt SANS tenue doit elle
  // aussi partir marquée : sa base de temps a sauté, ses minutes ne sont pas
  // une mesure. La clôture passe par tttUpdateElapsedMinutes (site 1).
  Sim s;
  simInit(s, 500);
  TttInputs in = orpIn(kOrpTarget - 45.0f);  // hors bande : aucune tenue
  runSeconds(s, in, 2);
  TEST_ASSERT_EQUAL_INT(1, s.nStarted);
  TEST_ASSERT_EQUAL_UINT32(500u, s.st.t0FiltMin);

  s.filtSec = 30UL * 60UL;  // restauration d'usine : le compteur recule
  runSeconds(s, in, 1);
  TEST_ASSERT_EQUAL_UINT8(kTttFlagCounterReset, s.st.flags & kTttFlagCounterReset);

  TttInputs off = in;
  off.filtrationRunning = false;
  off.dayEnd = true;
  runSeconds(s, off, 2);
  TEST_ASSERT_EQUAL_INT(1, s.nMissed);
  TEST_ASSERT_EQUAL_UINT16(0, s.missed.minutes);
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(kTttFlagCounterReset,
      s.missed.flags & kTttFlagCounterReset,
      "journee close a 0 minute apres un recul, sans drapeau : indiscernable "
      "d'une journee ou la cible n'a jamais bouge");
}

void test_bug106_recul_pendant_la_tenue_de_bout_en_bout(void) {
  // Scénario complet et réaliste (celui de test_D_compteur_en_recul_relance_la_
  // tenue, augmenté de l'assertion qui manquait) : la tenue est relancée, la
  // journée finit par être atteinte — et son enregistrement porte la marque.
  Sim s;
  simInit(s, 100);
  TttInputs in = orpIn(kOrpTarget);
  runSeconds(s, in, 2);
  runMinutes(s, in, 10);
  TEST_ASSERT_EQUAL_UINT8(0, s.st.flags);  // rien avant le recul

  s.filtSec = 0;  // effacement NVS `pool-daily`
  runMinutes(s, in, 31);

  TEST_ASSERT_EQUAL_INT(1, s.nReached);
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(kTttFlagCounterReset,
      s.reached.flags & kTttFlagCounterReset,
      "la journee est atteinte apres un recul de compteur mais ne le dit pas : "
      "ses minutes sont comptees sur deux bases de temps differentes");
}

// =============================================================================
// F — GARDE DE VRAISEMBLANCE SUR e0
// =============================================================================

// Le cas RÉEL du 05/09 : brut 5,45 pour une consigne 7,20 (air dans le circuit,
// niveau d'eau bas) ⇒ e0 = −1,75. La garde REFUSE de démarrer. Elle n'invente
// jamais de valeur : t0 est posé au PREMIER échantillon plausible, et le
// chronomètre part de LÀ.
void test_F_e0_aberrant_du_0509_refuse_le_depart(void) {
  Sim s;
  simInit(s, 40);
  runMinutes(s, phIn(5.45f), 20);  // 20 min d'air dans le circuit
  TEST_ASSERT_EQUAL_INT(0, s.nStarted);
  TEST_ASSERT_EQUAL_UINT8(kTttStateIdle, s.st.state);
  TEST_ASSERT_TRUE(s.nImplausible > 0);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, -1.75f, s.rejected);
  TEST_ASSERT_EQUAL_UINT32(0, s.st.t0FiltMin);

  // Le circuit se réamorce : mesure plausible, t0 posé ICI (filt = 60).
  runMinutes(s, phIn(kPhTarget + 0.40f), 5);
  TEST_ASSERT_EQUAL_INT(1, s.nStarted);
  TEST_ASSERT_EQUAL_UINT32(60, s.st.t0FiltMin);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.40f, s.started.e0);
  TEST_ASSERT_EQUAL_UINT16(4, s.st.minutes);
}

// Le verdict kImplausible est émis à CHAQUE tick tant que la mesure est
// aberrante : c'est la coquille qui LATCHE l'avertissement (un seul par voie et
// par jour). Le contrat pur est ici ; le latch est hors périmètre natif.
void test_F_verdict_implausible_a_chaque_tick_le_latch_est_coquille(void) {
  Sim s;
  simInit(s, 0);
  runSeconds(s, phIn(5.45f), 10);
  TEST_ASSERT_EQUAL_INT(10, s.nImplausible);
  TEST_ASSERT_EQUAL_INT(0, s.nStarted);
}

void test_F_frontieres_de_la_garde_ph_et_orp(void) {
  // pH : |e0| au-delà de 1,0 refusé, juste en deçà accepté. On s'écarte de
  // 1 millipH de la frontière exacte : à ce niveau, 7,20f + 1,0f − 7,20f ne
  // vaut pas 1,0f au bit près, et le test mesurerait l'arrondi, pas la règle.
  Sim a;
  simInit(a, 0);
  runSeconds(a, phIn(kPhTarget + kTttMaxPlausibleE0Ph + 0.01f), 2);
  TEST_ASSERT_EQUAL_INT(0, a.nStarted);
  TEST_ASSERT_TRUE(a.nImplausible > 0);

  Sim b;
  simInit(b, 0);
  runSeconds(b, phIn(kPhTarget + kTttMaxPlausibleE0Ph - 0.001f), 2);
  TEST_ASSERT_EQUAL_INT(1, b.nStarted);

  // ORP : |e0| > 300 mV refusé ; 300 accepté. Et le signe est indifférent.
  Sim c;
  simInit(c, 0);
  runSeconds(c, orpIn(kOrpTarget - kTttMaxPlausibleE0Orp - 1.0f), 2);
  TEST_ASSERT_EQUAL_INT(0, c.nStarted);

  Sim d;
  simInit(d, 0);
  runSeconds(d, orpIn(kOrpTarget - kTttMaxPlausibleE0Orp), 2);
  TEST_ASSERT_EQUAL_INT(1, d.nStarted);
  TEST_ASSERT_FLOAT_WITHIN(0.5f, -300.0f, d.started.e0);
}

// Une garde non configurée (0 ou négative) ne vaut pas « tout est plausible ».
void test_F_garde_non_configuree_refuse_de_demarrer(void) {
  Sim s;
  simInit(s, 0);
  TttInputs in = phIn(kPhTarget + 0.10f);
  in.maxPlausibleE0 = 0.0f;
  runSeconds(s, in, 5);
  TEST_ASSERT_EQUAL_INT(0, s.nStarted);
  TEST_ASSERT_TRUE(s.nImplausible > 0);
}

// La garde ne s'applique QU'AU DÉPART : une dérive ultérieure au-delà du seuil
// ne referme pas la journée, elle rompt seulement la tenue.
void test_F_la_garde_ne_referme_pas_une_journee_en_cours(void) {
  Sim s;
  simInit(s, 0);
  runMinutes(s, phIn(kPhTarget + 0.20f), 10);
  TEST_ASSERT_EQUAL_INT(1, s.nStarted);
  runMinutes(s, phIn(kPhTarget + 3.0f), 30);  // aberration en cours de route
  TEST_ASSERT_EQUAL_UINT8(kTttStateRunning, s.st.state);
  TEST_ASSERT_EQUAL_INT(1, s.nStarted);
  TEST_ASSERT_EQUAL_INT(0, s.nMissed);
  TEST_ASSERT_EQUAL_UINT16(39, s.st.minutes);
}

// =============================================================================
// G — LES DRAPEAUX
// =============================================================================

// Consigne CONFIGURÉE modifiée : bit 0 posé, calcul POURSUIVI, entrée NON
// effacée (ni t0, ni e0 ne bougent — sinon la journée mentirait sur son point
// de départ).
void test_G_changement_de_consigne_pose_bit0_et_poursuit(void) {
  Sim s;
  simInit(s, 20);
  runMinutes(s, phIn(kPhTarget + 0.30f), 60);
  TEST_ASSERT_EQUAL_UINT8(0, s.st.flags);
  const float e0 = s.started.e0;

  TttInputs moved = phIn(7.10f + 0.30f);  // l'utilisateur descend la consigne
  moved.target = 7.10f;
  moved.configuredTarget = 7.10f;
  runMinutes(s, moved, 10);
  TEST_ASSERT_EQUAL_UINT8(kTttFlagTargetChanged, s.st.flags);
  TEST_ASSERT_EQUAL_UINT8(kTttStateRunning, s.st.state);
  TEST_ASSERT_EQUAL_UINT32(20, s.st.t0FiltMin);   // entrée intacte
  TEST_ASSERT_EQUAL_FLOAT(e0, s.st.e0);           // e0 intact
  TEST_ASSERT_EQUAL_UINT16(69, s.st.minutes);     // calcul poursuivi
  TEST_ASSERT_EQUAL_INT(1, s.nStarted);

  // Le drapeau est PORTÉ par la journée archivée.
  TttInputs settle = moved;
  settle.value = 7.10f + 0.01f;
  runMinutes(s, settle, 32);
  TEST_ASSERT_EQUAL_INT(1, s.nReached);
  TEST_ASSERT_TRUE(s.reached.flags & kTttFlagTargetChanged);

  // Et il ne se pose qu'UNE FOIS malgré des ticks répétés (champ de bits).
  TEST_ASSERT_EQUAL_UINT8(kTttFlagTargetChanged, s.reached.flags);
}

// Boost chlore : bit 1 posé, bit 0 NON levé. On JUGE contre la consigne
// EFFECTIVE (qui monte au boost) mais on SURVEILLE la consigne CONFIGURÉE :
// un boost n'est pas une modification utilisateur.
void test_G_boost_pose_bit1_et_jamais_bit0(void) {
  Sim s;
  simInit(s, 0);
  runMinutes(s, orpIn(kOrpTarget - 45.0f), 30);
  TEST_ASSERT_EQUAL_UINT8(0, s.st.flags);

  TttInputs boost = orpIn(kOrpTarget - 45.0f);
  boost.boostActive = true;
  boost.target = 600.0f;              // consigne EFFECTIVE relevée par le boost
  boost.configuredTarget = kOrpTarget;  // consigne CONFIGURÉE inchangée
  boost.value = 600.0f - 45.0f;
  runMinutes(s, boost, 60);

  TEST_ASSERT_TRUE(s.st.flags & kTttFlagBoost);
  TEST_ASSERT_FALSE_MESSAGE(s.st.flags & kTttFlagTargetChanged,
                            "un boost ne doit PAS lever « consigne modifiée »");
  TEST_ASSERT_EQUAL_UINT8(kTttFlagBoost, s.st.flags);
}

// Boost déjà actif AU DÉPART : le bit est posé dès t0, et pas le bit 0.
// Les deux sites de pose passent désormais par applyCircumstances() : UNE seule
// mutation de cette fonction tue ce test ET le précédent. Les deux tests sont
// conservés parce qu'ils décrivent deux moments distincts de la journée (départ
// et cours de route) — c'est la mutation qui a fusionné, pas l'exigence.
void test_G_boost_actif_des_le_depart(void) {
  Sim s;
  simInit(s, 0);
  TttInputs boost = orpIn(555.0f);
  boost.boostActive = true;
  boost.target = 600.0f;
  boost.configuredTarget = kOrpTarget;
  boost.unsettled = true;
  runSeconds(s, boost, 3);
  TEST_ASSERT_EQUAL_INT(1, s.nStarted);
  TEST_ASSERT_TRUE(s.started.flags & kTttFlagBoost);
  TEST_ASSERT_TRUE(s.started.flags & kTttFlagUnsettled);
  TEST_ASSERT_FALSE(s.started.flags & kTttFlagTargetChanged);
}

void test_G_purge_au_plafond_pose_bit2(void) {
  Sim s;
  simInit(s, 0);
  runMinutes(s, phIn(kPhTarget + 0.30f), 5);
  TEST_ASSERT_EQUAL_UINT8(0, s.st.flags);
  TttInputs cap = phIn(kPhTarget + 0.30f);
  cap.unsettled = true;
  runMinutes(s, cap, 5);
  TEST_ASSERT_EQUAL_UINT8(kTttFlagUnsettled, s.st.flags);
}

// -----------------------------------------------------------------------------
// BIT 4 — RÈGLE DE PARTAGE. Le cœur de la sémantique de la métrique.
// -----------------------------------------------------------------------------

// Garde LONGUE que la régulation s'impose à ELLE-MÊME (quota journalier atteint,
// limite horaire, actionneur sous-dimensionné) : AUCUN bit. C'est une vraie
// non-atteinte — précisément ce que la métrique existe pour mesurer.
void test_G_quota_journalier_est_une_non_atteinte_NUE(void) {
  Sim s;
  simInit(s, 0);
  // La régulation reste AUX COMMANDES (automatic = true) : elle a simplement
  // épuisé son quota et ne dose plus. Le pH stagne toute la journée.
  runMinutes(s, phIn(kPhTarget + 0.35f), 600);
  TttInputs off = phIn(kPhTarget + 0.35f);
  off.filtrationRunning = false;
  off.dayEnd = true;
  runSeconds(s, off, 2);

  TEST_ASSERT_EQUAL_INT(1, s.nMissed);
  TEST_ASSERT_EQUAL_UINT8(kTttStateMissed, s.missed.state);
  TEST_ASSERT_EQUAL_UINT16(600, s.missed.minutes);
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(
      0, s.missed.flags,
      "quota / limite horaire / actionneur saturé : AUCUN bit, la journée est "
      "une vraie non-atteinte");
  TEST_ASSERT_FALSE(s.missed.flags & kTttFlagOperatorOff);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.35f, s.residual);
}

// Bascule en manuel EN COURS de journée : clôture à l'instant même, minutes
// FIGÉES, bit 4.
void test_G_bascule_en_manuel_cloture_avec_bit4_et_fige_les_minutes(void) {
  Sim s;
  simInit(s, 0);
  runMinutes(s, phIn(kPhTarget + 0.30f), 100);
  TEST_ASSERT_EQUAL_UINT8(kTttStateRunning, s.st.state);

  TttInputs manual = phIn(kPhTarget + 0.30f);
  manual.automatic = false;
  runSeconds(s, manual, 2);
  TEST_ASSERT_EQUAL_INT(1, s.nMissed);
  TEST_ASSERT_TRUE(s.missed.flags & kTttFlagOperatorOff);
  TEST_ASSERT_EQUAL_UINT16(100, s.missed.minutes);

  // Les minutes ne bougent plus, même après des heures de manuel.
  runMinutes(s, manual, 180);
  TEST_ASSERT_EQUAL_UINT16(100, s.st.minutes);
  TEST_ASSERT_EQUAL_INT(1, s.nMissed);
}

// Retour en automatique le MÊME jour : PAS de nouveau t0 (l'invariant « un seul
// départ par jour civil » prime).
void test_G_retour_en_automatique_ne_repose_pas_de_t0(void) {
  Sim s;
  simInit(s, 0);
  runMinutes(s, phIn(kPhTarget + 0.30f), 100);
  TttInputs manual = phIn(kPhTarget + 0.30f);
  manual.automatic = false;
  runMinutes(s, manual, 60);
  TEST_ASSERT_EQUAL_INT(1, s.nMissed);

  runMinutes(s, phIn(kPhTarget + 0.30f), 300);  // retour en automatique
  TEST_ASSERT_EQUAL_INT(1, s.nStarted);
  TEST_ASSERT_EQUAL_INT(1, s.nMissed);
  TEST_ASSERT_EQUAL_INT(0, s.nReached);
  TEST_ASSERT_EQUAL_UINT16(100, s.st.minutes);
  TEST_ASSERT_TRUE(s.st.flags & kTttFlagOperatorOff);

  // Le lendemain, en revanche, une nouvelle journée démarre normalement.
  TttInputs next = phIn(kPhTarget + 0.30f);
  next.date = kD1;
  runMinutes(s, next, 10);
  TEST_ASSERT_EQUAL_INT(2, s.nStarted);
  TEST_ASSERT_EQUAL_UINT8(0, s.st.flags);
}

// Journée DÉMARRÉE en manuel : t0 est quand même posé (l'historique reste
// DENSE — un trou muet se lirait comme un bug de l'indicateur), puis clôture
// immédiate avec le bit 4 et 0 minute.
void test_G_journee_demarree_en_manuel_pose_t0_puis_cloture_a_zero(void) {
  Sim s;
  simInit(s, 300);
  TttInputs manual = phIn(kPhTarget + 0.30f);
  manual.automatic = false;
  runSeconds(s, manual, 5);

  TEST_ASSERT_EQUAL_INT(1, s.nStarted);
  TEST_ASSERT_EQUAL_INT(1, s.nMissed);
  TEST_ASSERT_EQUAL_UINT32(kD, s.missed.date);
  TEST_ASSERT_EQUAL_UINT16(0, s.missed.minutes);
  TEST_ASSERT_TRUE(s.missed.flags & kTttFlagOperatorOff);
  TEST_ASSERT_EQUAL_UINT8(kTttStateMissed, s.missed.state);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.30f, s.missed.e0);  // e0 tout de même porté
}

// Maintenance et assistant de débit empruntent LE MÊME chemin (in.automatic =
// false) : ce sont des débranchements par l'OPÉRATEUR. Le partage entre ces
// causes et les gardes longues est vérifié sur les SOURCES en section H.
void test_G_maintenance_et_assistant_debit_empruntent_le_bit4(void) {
  Sim s;
  simInit(s, 0);
  runMinutes(s, orpIn(kOrpTarget - 45.0f), 42);
  TttInputs suspended = orpIn(kOrpTarget - 45.0f);
  suspended.automatic = false;  // isMaintenanceInhibited() || isFlowAssistSuspended()
  runSeconds(s, suspended, 2);
  TEST_ASSERT_EQUAL_INT(1, s.nMissed);
  TEST_ASSERT_EQUAL_UINT8(kTttFlagOperatorOff, s.missed.flags);
  TEST_ASSERT_EQUAL_UINT16(42, s.missed.minutes);
}

// Fin de journée ET manuel simultanés : le bit 4 gagne (l'opérateur a débranché
// avant que la journée ne se termine d'elle-même).
void test_G_fin_de_journee_et_manuel_simultanes(void) {
  Sim s;
  simInit(s, 0);
  runMinutes(s, phIn(kPhTarget + 0.30f), 30);
  TttInputs both = phIn(kPhTarget + 0.30f);
  both.automatic = false;
  both.dayEnd = true;
  both.filtrationRunning = false;
  runSeconds(s, both, 2);
  TEST_ASSERT_EQUAL_INT(1, s.nMissed);
  TEST_ASSERT_TRUE(s.missed.flags & kTttFlagOperatorOff);
}

// -----------------------------------------------------------------------------
// BIT 3 — COUPURE SECTEUR
// -----------------------------------------------------------------------------
// La reprise elle-même vit dans la coquille (restoreFromNvs) ; ce qu'on prouve
// ici, ce sont les PRIMITIVES pures dont elle est faite et le comportement de
// la machine APRÈS reprise. Cf. rapport pour ce qui reste non prouvé.
void test_G_coupure_secteur_meme_jour_conserve_t0_et_pose_bit3(void) {
  // Avant la coupure : t0 posé à 47 min de filtration, entrée « en cours »
  // archivée, boost en cours.
  TttHistory h;
  TttDay running = day(kD, 0, kTttStateRunning, kTttFlagBoost);
  running.e0 = -45.0f;
  TEST_ASSERT_TRUE(tttHistoryAppend(h, kTttChannelOrp, running));
  h.t0FiltMin[kTttChannelOrp] = kGateFiltMin;

  // La RAM RTC est perdue : magic/CRC faux ⇒ reprise par la NVS (fail-closed).
  TttRtcRecord rtc;
  memset(&rtc, 0x5C, sizeof(rtc));
  TEST_ASSERT_FALSE(tttRtcIntact(rtc));

  const TttDay* found = tttHistoryFind(h, kTttChannelOrp, kD);
  TEST_ASSERT_NOT_NULL(found);
  TEST_ASSERT_EQUAL_UINT8(kTttStateRunning, found->state);

  Sim s;
  simInit(s, 200);  // 200 min de filtration au moment du redémarrage
  s.st.date = kD;
  s.st.t0FiltMin = h.t0FiltMin[kTttChannelOrp];
  s.st.e0 = found->e0;
  s.st.state = kTttStateRunning;
  s.st.flags = (uint8_t)(found->flags | kTttFlagPowerCycle);
  // La consigne surveillée est REPHOTOGRAPHIÉE (une modification survenue
  // pendant la coupure n'est pas détectable : inventer un drapeau serait pire).
  s.st.watchTarget = kOrpTarget;
  s.st.minutes = tttUpdateElapsedMinutes(s.st, 200);

  TEST_ASSERT_EQUAL_UINT16(153, s.st.minutes);  // 200 − 47 : minutes EXACTES
  TEST_ASSERT_EQUAL_UINT8(0, s.st.holding);     // tenue à zéro, fail-closed
  TEST_ASSERT_EQUAL_UINT32(0, s.st.holdStartFiltMin);
  TEST_ASSERT_TRUE(s.st.flags & kTttFlagPowerCycle);
  TEST_ASSERT_TRUE(s.st.flags & kTttFlagBoost);  // les bits d'avant survivent

  // Après reprise, la tenue doit se rejouer ENTIÈREMENT et le temps final rester
  // compté depuis le t0 D'ORIGINE.
  runMinutes(s, orpIn(kOrpTarget - 2.0f), 31);
  TEST_ASSERT_EQUAL_INT(1, s.nReached);
  TEST_ASSERT_EQUAL_UINT16(153, s.reached.minutes);
  TEST_ASSERT_TRUE(s.reached.flags & kTttFlagPowerCycle);
}

// Jour DIFFÉRENT ou heure invalide : aucune reprise, aucune écriture datée faux.
void test_G_coupure_secteur_jour_different_ne_reprend_rien(void) {
  TttHistory h;
  TttDay running = day(kD, 0, kTttStateRunning, 0);
  tttHistoryAppend(h, kTttChannelOrp, running);
  h.t0FiltMin[kTttChannelOrp] = kGateFiltMin;

  TEST_ASSERT_NULL(tttHistoryFind(h, kTttChannelOrp, kD1));
  TEST_ASSERT_NULL(tttHistoryFind(h, kTttChannelOrp, 0));
  // Une journée déjà CLOSE ne se reprend pas non plus.
  tttHistoryAppend(h, kTttChannelOrp, day(kD, 303, kTttStateReached, 0));
  const TttDay* d = tttHistoryFind(h, kTttChannelOrp, kD);
  TEST_ASSERT_NOT_NULL(d);
  TEST_ASSERT_NOT_EQUAL(kTttStateRunning, d->state);
}

// =============================================================================
// H — ASSERTIONS SUR LES SOURCES
// =============================================================================
// Ces deux tests ne portent pas sur le comportement du module mais sur le TEXTE
// des fichiers de production. C'est délibéré : les invariants qu'ils gardent
// sont STRUCTURELS (qui dépend de qui, quelle horloge alimente la métrique) et
// aucun test de comportement natif ne peut les voir — le premier parce qu'il
// s'agit d'une NON-dépendance, le second parce que la base de temps est un
// paramètre du module pur, choisi dans la coquille.

static char* readProjectFile(const char* rel, long* outLen) {
  char path[512];
  const char* candidates[5];
  char rooted[512];

  // Racine déduite de __FILE__ (chemin tel que vu par le compilateur), plus des
  // replis relatifs selon le répertoire courant du lanceur de tests.
  rooted[0] = '\0';
  {
    const char* f = __FILE__;
    const char* m = strstr(f, "test/test_native_time_to_target");
    if (m != NULL && (size_t)(m - f) < sizeof(rooted) - 256) {
      const size_t n = (size_t)(m - f);
      memcpy(rooted, f, n);
      rooted[n] = '\0';
      strncat(rooted, rel, sizeof(rooted) - strlen(rooted) - 1);
    }
  }
  candidates[0] = rooted[0] ? rooted : NULL;
  candidates[1] = rel;
  candidates[2] = NULL;
  candidates[3] = NULL;
  candidates[4] = NULL;

  const char* prefixes[3] = {"../", "../../", "../../../"};
  for (int i = 0; i < 5; ++i) {
    if (i >= 2) {
      snprintf(path, sizeof(path), "%s%s", prefixes[i - 2], rel);
    } else {
      if (candidates[i] == NULL) continue;
      snprintf(path, sizeof(path), "%s", candidates[i]);
    }
    FILE* fp = fopen(path, "rb");
    if (fp == NULL) continue;
    fseek(fp, 0, SEEK_END);
    const long len = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (len <= 0 || len > 4000000L) {
      fclose(fp);
      continue;
    }
    char* buf = (char*)malloc((size_t)len + 1);
    if (buf == NULL) {
      fclose(fp);
      continue;
    }
    const size_t got = fread(buf, 1, (size_t)len, fp);
    fclose(fp);
    buf[got] = '\0';
    *outLen = (long)got;
    return buf;
  }
  return NULL;
}

// Recherche insensible à la casse, sans dépendre de strcasestr (BSD/GNU).
static int containsCI(const char* hay, const char* needle) {
  const size_t n = strlen(needle);
  if (n == 0) return 1;
  for (const char* p = hay; *p; ++p) {
    size_t i = 0;
    while (i < n) {
      char a = p[i];
      char b = needle[i];
      if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
      if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
      if (a != b) break;
      ++i;
    }
    if (i == n) return 1;
  }
  return 0;
}

static void assertSourceFree(const char* rel, const char* needle) {
  long len = 0;
  char* src = readProjectFile(rel, &len);
  char msg[256];
  if (src == NULL) {
    snprintf(msg, sizeof(msg), "Source INTROUVABLE : %s (le test ne peut pas "
                               "conclure — c'est un échec, pas un succès)", rel);
    TEST_FAIL_MESSAGE(msg);
    return;
  }
  const int found = containsCI(src, needle);
  free(src);
  snprintf(msg, sizeof(msg),
           "SENS UNIQUE ROMPU : « %s » apparaît dans %s. pump_controller ne "
           "doit RIEN savoir de l'indicateur (condition C1 du gate).",
           needle, rel);
  TEST_ASSERT_FALSE_MESSAGE(found, msg);
}

// C1 — LE SENS EST UNIQUE : l'indicateur lit la régulation, la régulation ne
// lit JAMAIS l'indicateur. Fermer la boucle (adapter un gain sur un KPI)
// exigerait une spec et un passage par pool-chemistry ; ce test est le garde-fou
// qui rend une telle dérive impossible à commettre par inadvertance.
void test_H_pump_controller_ignore_totalement_l_indicateur(void) {
  assertSourceFree("src/pump_controller.h", "time_to_target");
  assertSourceFree("src/pump_controller.h", "ttt");
  assertSourceFree("src/pump_controller.cpp", "time_to_target");
  assertSourceFree("src/pump_controller.cpp", "ttt");
  // Contrôle NÉGATIF : le lecteur de fichiers voit bien quelque chose, sinon le
  // test ci-dessus passerait sur un fichier vide.
  long len = 0;
  char* src = readProjectFile("src/pump_controller.cpp", &len);
  TEST_ASSERT_NOT_NULL(src);
  TEST_ASSERT_TRUE(len > 10000);
  TEST_ASSERT_TRUE(containsCI(src, "PumpController"));
  free(src);
}

// La BASE DE TEMPS de la métrique est le compteur de minutes de CIRCULATION, lu
// en LECTURE SEULE. Le module pur ne peut pas garder cet invariant : il reçoit
// `filtTotalMin` en paramètre. Le choix se fait dans la coquille, et c'est là
// qu'il faut le clouer.
void test_H_base_de_temps_est_le_compteur_de_filtration(void) {
  long len = 0;
  char* src = readProjectFile("src/time_to_target.cpp", &len);
  TEST_ASSERT_NOT_NULL_MESSAGE(src, "src/time_to_target.cpp introuvable");

  TEST_ASSERT_TRUE_MESSAGE(
      containsCI(src, "in.filtTotalMin = safetyLimits.filtTotalMin"),
      "La base de temps doit être safetyLimits.filtTotalMin (minutes de "
      "circulation), jamais une horloge murale");
  // Aucune écriture sur ce compteur : l'indicateur est un observateur.
  TEST_ASSERT_FALSE_MESSAGE(containsCI(src, "safetyLimits.filtTotalMin ="),
                            "l'indicateur ne doit JAMAIS écrire filtTotalMin");
  TEST_ASSERT_FALSE_MESSAGE(containsCI(src, "safetyLimits.filtTotalMin +"),
                            "l'indicateur ne doit JAMAIS incrémenter filtTotalMin");

  // Règle de partage du bit 4 : ce que l'OPÉRATEUR débranche, et rien d'autre.
  // Les brides que la régulation s'impose (quota, limite horaire, actionneur
  // sous-dimensionné) ne doivent PAS alimenter `automatic`.
  TEST_ASSERT_TRUE(containsCI(src, "isMaintenanceInhibited"));
  TEST_ASSERT_TRUE(containsCI(src, "isFlowAssistSuspended"));
  TEST_ASSERT_FALSE_MESSAGE(
      containsCI(src, "isPhLimitedByHourlyBudget"),
      "une limite horaire est une VRAIE non-atteinte : aucun bit 4");
  TEST_ASSERT_FALSE_MESSAGE(
      containsCI(src, "isPhUndersizedSuspected"),
      "un actionneur sous-dimensionné est une VRAIE non-atteinte : aucun bit 4");
  free(src);
}

// =============================================================================
// I — RATTRAPAGE DES JOURNÉES ORPHELINES (tttHistoryCloseOrphans)
// =============================================================================
// Une entrée est archivée DÈS la pose de t0, « en cours » et 0 minute : c'est
// ce qui rend t0 récupérable après une coupure secteur. Mais si l'appareil
// reste éteint PLUS d'une journée civile, la reprise ne cherche que la date du
// jour et l'entrée de la veille resterait figée « en cours, 0 min » jusqu'à
// sortir de l'anneau — l'UI annoncerait « journée en cours » pour une journée
// révolue. Cette fonction les clôt.
//
// Elle est faite de TROIS GARDES, et chacune protège contre un dégât distinct :
// détruire l'anneau faute d'heure, clore la journée du jour, ou rouvrir une
// journée ATTEINTE en « non atteinte » — ce dernier fabriquerait une régression
// apparente sur la métrique même qui arbitre les comparaisons de versions.

static int ringEqual(const TttHistory& a, const TttHistory& b) {
  return memcmp(a.days, b.days, sizeof(a.days)) == 0;
}

void test_I_anneau_vide_ne_change_rien(void) {
  TttHistory h;
  TttHistory before = h;
  TEST_ASSERT_EQUAL_UINT8(0, tttHistoryCloseOrphans(h, kD1));
  TEST_ASSERT_TRUE(ringEqual(before, h));
}

// GARDE 1 — la plus grave. Un démarrage sans NTP ne doit RIEN clôturer : on ne
// sait pas quel jour on est, donc aucune journée n'est « révolue ». Le test
// place une orpheline BIEN RÉELLE, pour que la garde ait quelque chose à
// protéger — sinon il passerait par vacuité.
void test_I_heure_invalide_ne_detruit_pas_l_anneau(void) {
  TttHistory h;
  tttHistoryAppend(h, kTttChannelPh, day(20260830UL, 0, kTttStateRunning, 0));
  tttHistoryAppend(h, kTttChannelOrp, day(20260831UL, 0, kTttStateRunning, 0));
  tttHistoryAppend(h, kTttChannelOrp, day(20260901UL, 210, kTttStateReached, 0));
  const TttHistory before = h;

  TEST_ASSERT_EQUAL_UINT8_MESSAGE(
      0, tttHistoryCloseOrphans(h, 0),
      "sans heure, AUCUNE journée n'est révolue : l'anneau doit être intact");
  TEST_ASSERT_TRUE_MESSAGE(
      ringEqual(before, h),
      "un démarrage sans NTP détruirait sept journées d'observation");
  TEST_ASSERT_EQUAL_UINT8(kTttStateRunning,
                          tttHistoryFind(h, kTttChannelPh, 20260830UL)->state);
}

// GARDE 2 — la journée EN COURS est épargnée. Sans elle, le comportement
// s'inverse : la journée du jour serait close à chaque démarrage.
void test_I_journee_du_jour_en_cours_est_epargnee(void) {
  TttHistory h;
  TttDay today = day(kD, 0, kTttStateRunning, kTttFlagBoost);
  today.e0 = -45.0f;
  tttHistoryAppend(h, kTttChannelOrp, today);
  const TttHistory before = h;

  TEST_ASSERT_EQUAL_UINT8(0, tttHistoryCloseOrphans(h, kD));
  TEST_ASSERT_TRUE(ringEqual(before, h));
  const TttDay* d = tttHistoryFind(h, kTttChannelOrp, kD);
  TEST_ASSERT_EQUAL_UINT8(kTttStateRunning, d->state);
  TEST_ASSERT_EQUAL_UINT8(kTttFlagBoost, d->flags);  // pas de bit 3 posé
}

// Le cas nominal : éteint plus d'une journée civile.
void test_I_orpheline_est_close_avec_bit3(void) {
  TttHistory h;
  TttDay veille = day(kD, 0, kTttStateRunning, 0);
  veille.e0 = 0.18f;
  tttHistoryAppend(h, kTttChannelPh, veille);
  tttHistoryAppend(h, kTttChannelPh, day(kD1, 0, kTttStateRunning, 0));

  TEST_ASSERT_EQUAL_UINT8(1, tttHistoryCloseOrphans(h, kD1));
  const TttDay* old = tttHistoryFind(h, kTttChannelPh, kD);
  TEST_ASSERT_EQUAL_UINT8(kTttStateMissed, old->state);
  TEST_ASSERT_TRUE(old->flags & kTttFlagPowerCycle);
  TEST_ASSERT_EQUAL_FLOAT(0.18f, old->e0);   // e0 archivé conservé
  TEST_ASSERT_EQUAL_UINT16(0, old->minutes);  // aucune minute INVENTÉE
  // La journée du jour, elle, reste en cours.
  TEST_ASSERT_EQUAL_UINT8(kTttStateRunning,
                          tttHistoryFind(h, kTttChannelPh, kD1)->state);
}

// GARDE 3 — une journée ATTEINTE n'est JAMAIS rouverte. C'est le faux positif
// le plus coûteux : il transformerait des atteintes archivées en non-atteintes
// et ferait passer une version saine pour une régression.
void test_I_journee_atteinte_revolue_n_est_jamais_rouverte(void) {
  TttHistory h;
  tttHistoryAppend(h, kTttChannelPh, day(20260901UL, 223, kTttStateReached, 0));
  tttHistoryAppend(h, kTttChannelPh, day(20260902UL, 310, kTttStateReached,
                                         kTttFlagTargetChanged));
  tttHistoryAppend(h, kTttChannelOrp, day(20260901UL, 303, kTttStateReached, 0));
  tttHistoryAppend(h, kTttChannelOrp, day(20260902UL, 480, kTttStateMissed, 0));
  const TttHistory before = h;

  TEST_ASSERT_EQUAL_UINT8_MESSAGE(
      0, tttHistoryCloseOrphans(h, kD1),
      "aucune journée n'est « en cours » : rien à clôturer");
  TEST_ASSERT_TRUE_MESSAGE(
      ringEqual(before, h),
      "rouvrir une journée ATTEINTE fabriquerait une régression apparente");
  TEST_ASSERT_EQUAL_UINT8(kTttStateReached,
                          tttHistoryFind(h, kTttChannelPh, 20260901UL)->state);
  TEST_ASSERT_EQUAL_UINT16(
      223, tttHistoryFind(h, kTttChannelPh, 20260901UL)->minutes);
}

// Les deux voies sont traitées, et indépendamment l'une de l'autre.
void test_I_les_deux_voies_sont_rattrapees(void) {
  TttHistory h;
  tttHistoryAppend(h, kTttChannelPh, day(20260902UL, 0, kTttStateRunning, 0));
  tttHistoryAppend(h, kTttChannelPh, day(20260903UL, 223, kTttStateReached, 0));
  tttHistoryAppend(h, kTttChannelOrp, day(20260901UL, 0, kTttStateRunning, 0));
  tttHistoryAppend(h, kTttChannelOrp, day(kD, 0, kTttStateRunning, 0));

  TEST_ASSERT_EQUAL_UINT8(3, tttHistoryCloseOrphans(h, kD1));
  TEST_ASSERT_EQUAL_UINT8(kTttStateMissed,
                          tttHistoryFind(h, kTttChannelPh, 20260902UL)->state);
  TEST_ASSERT_EQUAL_UINT8(kTttStateReached,
                          tttHistoryFind(h, kTttChannelPh, 20260903UL)->state);
  TEST_ASSERT_EQUAL_UINT8(kTttStateMissed,
                          tttHistoryFind(h, kTttChannelOrp, 20260901UL)->state);
  TEST_ASSERT_EQUAL_UINT8(kTttStateMissed,
                          tttHistoryFind(h, kTttChannelOrp, kD)->state);
}

// IDEMPOTENCE : un second appel ne recompte rien. Le compte retourné pilote un
// journal côté coquille — un rattrapage qui se réannoncerait à chaque
// démarrage serait un faux signal.
void test_I_idempotence_le_second_appel_rend_zero(void) {
  TttHistory h;
  tttHistoryAppend(h, kTttChannelOrp, day(kD, 0, kTttStateRunning, 0));
  TEST_ASSERT_EQUAL_UINT8(1, tttHistoryCloseOrphans(h, kD1));
  const TttHistory after = h;
  TEST_ASSERT_EQUAL_UINT8(0, tttHistoryCloseOrphans(h, kD1));
  TEST_ASSERT_TRUE(ringEqual(after, h));
  // Et une orpheline DÉJÀ close n'est pas recomptée non plus.
  TttHistory g;
  tttHistoryAppend(g, kTttChannelPh, day(kD, 120, kTttStateMissed, 0));
  TEST_ASSERT_EQUAL_UINT8(0, tttHistoryCloseOrphans(g, kD1));
}

// Le bit 3 s'AJOUTE aux circonstances déjà archivées, il ne les remplace pas :
// une journée orpheline qui portait « consigne modifiée » doit garder les deux.
void test_I_bit3_s_ajoute_sans_effacer_les_drapeaux(void) {
  TttHistory h;
  const uint8_t deja = kTttFlagTargetChanged | kTttFlagBoost | kTttFlagUnsettled;
  tttHistoryAppend(h, kTttChannelOrp, day(kD, 0, kTttStateRunning, deja));
  TEST_ASSERT_EQUAL_UINT8(1, tttHistoryCloseOrphans(h, kD1));
  const TttDay* d = tttHistoryFind(h, kTttChannelOrp, kD);
  TEST_ASSERT_EQUAL_UINT8((uint8_t)(deja | kTttFlagPowerCycle), d->flags);
  TEST_ASSERT_TRUE(d->flags & kTttFlagTargetChanged);
  TEST_ASSERT_TRUE(d->flags & kTttFlagPowerCycle);
}

// =============================================================================

void setUp(void) {}
void tearDown(void) {}

int main(int argc, char** argv) {
  (void)argc;
  (void)argv;
  UNITY_BEGIN();
  // A — contrat de format
  RUN_TEST(test_A_tailles_archivees);
  RUN_TEST(test_A_types_rtc_triviaux);
  RUN_TEST(test_A_encodage_etats_et_drapeaux);
  RUN_TEST(test_A_constantes_de_production);
  // B — sceaux et anneau
  RUN_TEST(test_B_sceau_historique_aller_retour);
  RUN_TEST(test_B_sceau_historique_rejette_corruption_et_taille);
  RUN_TEST(test_B_sceau_rtc_rejette_ram_aleatoire);
  RUN_TEST(test_B_scellement_normalise_le_remplissage);
  RUN_TEST(test_B_anneau_ajoute_et_decale);
  RUN_TEST(test_B_anneau_remplace_une_date_deja_presente);
  RUN_TEST(test_B_anneau_refuse_date_nulle_et_voie_hors_bornes);
  // C — rejeux du 04/09
  RUN_TEST(test_C_rejeu_0409_orp_303_minutes);
  RUN_TEST(test_C_rejeu_0409_ph_223_minutes_sur_la_grandeur_de_decision);
  RUN_TEST(test_C_rejeu_0409_ph_donnerait_233_depuis_le_brut);
  // D — la tenue
  RUN_TEST(test_D_sortie_de_bande_a_29_min_remet_la_tenue_pas_le_chrono);
  RUN_TEST(test_D_frontiere_29_versus_30_minutes_de_tenue);
  RUN_TEST(test_D_nan_prolonge_remet_la_tenue_sans_arreter_le_chrono);
  RUN_TEST(test_D_bande_non_finie_ou_nulle_vaut_hors_bande);
  RUN_TEST(test_D_arret_de_circulation_sans_tenue_est_une_non_atteinte);
  RUN_TEST(test_D_apres_atteinte_le_rebond_ne_rouvre_pas_la_journee);
  RUN_TEST(test_D_compteur_en_recul_relance_la_tenue);
  RUN_TEST(test_D_recul_ne_fait_jamais_gagner_de_temps);
  // E — le départ
  RUN_TEST(test_E_un_seul_depart_par_jour_civil);
  RUN_TEST(test_E_t0_jamais_reecrit_par_les_repurges);
  RUN_TEST(test_E_deja_a_la_cible_donne_zero_minute);
  RUN_TEST(test_E_minuit_en_filtration_manuelle_repart_a_zero);
  RUN_TEST(test_E_heure_perdue_suspend_sans_rien_dater);
  RUN_TEST(test_E_pas_de_depart_hors_circulation);
  RUN_TEST(test_E_base_de_temps_insensible_au_temps_mural);
  RUN_TEST(test_E_compteur_en_recul_ne_se_replie_pas);
  RUN_TEST(test_E_minutes_bornees_a_1440);
  // bug-106 — le 0 d'un compteur en recul se déclare (les TROIS sites)
  RUN_TEST(test_bug106_encodage_du_bit_de_recul);
  RUN_TEST(test_bug106_site1_vif_le_zero_rendu_porte_le_drapeau);
  RUN_TEST(test_bug106_site1_aucun_recul_ne_pose_rien);
  RUN_TEST(test_bug106_site1_le_drapeau_ne_pietine_pas_les_autres);
  RUN_TEST(test_bug106_site2_recul_pendant_la_tenue_pose_le_drapeau);
  RUN_TEST(test_bug106_site2_tenue_normale_ne_pose_rien);
  RUN_TEST(test_bug106_site3_archivage_journee_invalidee_est_distinguable);
  RUN_TEST(test_bug106_site3_atteinte_legitime_a_t0_ne_porte_pas_le_drapeau);
  RUN_TEST(test_bug106_deux_journees_a_zero_minute_ne_se_ressemblent_plus);
  RUN_TEST(test_bug106_le_drapeau_survit_a_la_cloture_en_non_atteinte);
  RUN_TEST(test_bug106_recul_pendant_la_tenue_de_bout_en_bout);
  // F — garde de vraisemblance
  RUN_TEST(test_F_e0_aberrant_du_0509_refuse_le_depart);
  RUN_TEST(test_F_verdict_implausible_a_chaque_tick_le_latch_est_coquille);
  RUN_TEST(test_F_frontieres_de_la_garde_ph_et_orp);
  RUN_TEST(test_F_garde_non_configuree_refuse_de_demarrer);
  RUN_TEST(test_F_la_garde_ne_referme_pas_une_journee_en_cours);
  // G — drapeaux
  RUN_TEST(test_G_changement_de_consigne_pose_bit0_et_poursuit);
  RUN_TEST(test_G_boost_pose_bit1_et_jamais_bit0);
  RUN_TEST(test_G_boost_actif_des_le_depart);
  RUN_TEST(test_G_purge_au_plafond_pose_bit2);
  RUN_TEST(test_G_quota_journalier_est_une_non_atteinte_NUE);
  RUN_TEST(test_G_bascule_en_manuel_cloture_avec_bit4_et_fige_les_minutes);
  RUN_TEST(test_G_retour_en_automatique_ne_repose_pas_de_t0);
  RUN_TEST(test_G_journee_demarree_en_manuel_pose_t0_puis_cloture_a_zero);
  RUN_TEST(test_G_maintenance_et_assistant_debit_empruntent_le_bit4);
  RUN_TEST(test_G_fin_de_journee_et_manuel_simultanes);
  RUN_TEST(test_G_coupure_secteur_meme_jour_conserve_t0_et_pose_bit3);
  RUN_TEST(test_G_coupure_secteur_jour_different_ne_reprend_rien);
  // H — assertions sur les sources
  RUN_TEST(test_H_pump_controller_ignore_totalement_l_indicateur);
  RUN_TEST(test_H_base_de_temps_est_le_compteur_de_filtration);
  // I — rattrapage des journées orphelines
  RUN_TEST(test_I_anneau_vide_ne_change_rien);
  RUN_TEST(test_I_heure_invalide_ne_detruit_pas_l_anneau);
  RUN_TEST(test_I_journee_du_jour_en_cours_est_epargnee);
  RUN_TEST(test_I_orpheline_est_close_avec_bit3);
  RUN_TEST(test_I_journee_atteinte_revolue_n_est_jamais_rouverte);
  RUN_TEST(test_I_les_deux_voies_sont_rattrapees);
  RUN_TEST(test_I_idempotence_le_second_appel_rend_zero);
  RUN_TEST(test_I_bit3_s_ajoute_sans_effacer_les_drapeaux);
  return UNITY_END();
}
