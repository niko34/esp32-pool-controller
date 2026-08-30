// =============================================================================
// Tests unitaires natifs — TERME INTÉGRAL pH (v2.26.0, module PUR extrait)
// =============================================================================
// Tournent sur PC (env:native, Unity), HORS ESP32 / Arduino / Atlas EZO.
//
// CE QUE CE FICHIER PROTÈGE
// -------------------------
// La levée de `Ki = 0` (« NON NÉGOCIABLE » de feature-025) sur le SEUL pH repose
// sur UNE affirmation structurelle : un correcteur proportionnel pur, sous charge
// basique permanente, ne peut PAS rejoindre sa consigne — il se stabilise sur son
// droop `e* = besoin / kp`. Si aucun test n'établit ce fait, rien ne protège la
// décision : un futur « nettoyage » remettrait Ki à zéro en toute bonne foi.
//
//   D1 — DROOP : P pur ne converge pas / PI converge ;
//   D2 — le piège `integralMax = 50` : autorité intégrale insuffisante ⇒ droop
//        INTACTE malgré un Ki non nul (échec silencieux de feature-025 réactivée) ;
//   I1 — charge de l'intégrale jusqu'à `integralMax`, POUR CHAQUE VITESSE ;
//   I2 — verrou d'autorité à l'exécution : ki × integralMax == 6 mL/min ;
//   A1 — anti-windup de computePidPure : bornage symétrique, gel, invariant
//        « flow ≤ maxFlow quelle que soit l'intégrale » ;
//   P* — le MODULE PUR src/ph_integral_logic.h, appelé DIRECTEMENT (quantum,
//        rejet des dt aberrants, gel asymétrique, soutenabilité, oubli, reset sec,
//        péremption, hystérésis 95/85, paliers 2 h/6 h et rappel 12 h) ;
//   E*  — L'ÉQUILIBRE en BOUCLE FERMÉE de production : module pur + computePidPure
//        + hystérésis de dosage. L'erreur doit converger vers le seuil d'ARRÊT,
//        l'intégrale se stabiliser SANS saturer, et le volume total rester celui
//        du P pur (bilan de matière : on corrige sans surdoser).
//
// ÉVOLUTION DEPUIS LE TOUR DE TEST PRÉCÉDENT
// ------------------------------------------
// Les mécanismes C2/C3/C4/C5 vivaient dans `PumpControllerClass::update()` : les
// tests `test_M*` en étaient une TRANSCRIPTION (un modèle recopié dans le test),
// et les mutations « restaurer le reset à −0,01 » ou « supprimer la décroissance »
// laissaient la suite verte. L'extraction en `src/ph_integral_logic.h` (header
// only, patron ADR-0017) a supprimé ce modèle : le bloc `P*` appelle désormais le
// CODE DE PRODUCTION.
//
// ⚠️ CE QUI RESTE HORS D'ATTEINTE DU NATIF (à connaître avant de faire confiance)
//   - le CHOIX de la bande morte (`phWasActiveTick ? phStopThreshold :
//     phStartThreshold`) est une ligne de pump_controller.cpp, pas du module pur.
//     Le simulateur `simulateProduction()` la REPRODUIT (paramètre `policy`) : les
//     tests E discriminent la bonne politique de la mauvaise, mais une mutation de
//     la ligne réelle ne les tuerait pas. Idem pour la fraction de quota 80 %
//     (prédicat de la coquille) — seul son EFFET, via `quotaNearLimit`, est testé.
//   - l'ordonnanceur d'impulsions (pouls discret, pause mélange, temps minimum
//     d'injection) n'est pas modélisé : `flow` est la demande MOYENNE, ce qui est
//     exactement la grandeur que le module et computePidPure manipulent.
// =============================================================================

#include <unity.h>
#include <math.h>    // C header uniquement (libc++ <cmath> indisponible sur l'hôte)
#include <stdio.h>   // snprintf : diagnostic des simulations longues
#include "dosing_logic.h"
#include "ph_integral_logic.h"
#include "constants.h"

// -----------------------------------------------------------------------------
// Contexte de production reproduit (aucun miroir de valeur : on IMPORTE)
// -----------------------------------------------------------------------------
static const float kEps = 1e-3f;

// Seuils de dosage pH par défaut (config.h) — recopiés en CONSTANTES DE SCÉNARIO
// parce que ce sont des réglages utilisateur, pas des constantes firmware.
static const float kStartThreshold = 0.05f;  // pumpProtection.phStartThreshold
static const float kStopThreshold  = 0.01f;  // pumpProtection.phStopThreshold

// Débit d'amplitude calibré (pesée 2026-08-23, duty 127) — borne haute passée à
// computePidPure par la coquille (`phAmpFlow`).
static const float kAmpFlow = 24.7f;

// Charge basique permanente MESURÉE (août 2026, ~325 g CO₂/j) exprimée en débit
// d'acide équivalent : c'est le « besoin » du droop e* = besoin / kp.
static const float kLoadMlPerMin = 2.86f;

// Gain du bassin : pH gagné par seconde et par (mL/min) de déséquilibre.
// Valeur LITTÉRALE volontaire (et non dérivée de kPhKpNormal) : le modèle de
// bassin ne doit pas suivre une retouche de gain, sinon la simulation masquerait
// l'effet qu'elle est censée mesurer. 1/(3600 × 8) ⇒ constante de temps de la
// boucle P d'une heure pour kp = 8.
static const float kPlantPhPerFlowPerSec = 3.47222e-5f;

static const float kPidDtSec = 10.0f;   // cadence de régulation (tick de 10 s)
static const long  kTicksPerDay = 8640; // 86 400 s / 10 s

// Base de temps NON NULLE des scénarios. `lastTickMs == 0` est le sentinelle
// « jamais évalué » : un scénario naïf partant de 0 n'armerait jamais rien.
static const uint32_t kT0 = 1000000u;

// Budget horaire des scénarios d'équilibre. Le DÉFAUT de config.h (5 min/h) rend
// la charge mesurée INTENABLE — c'est l'objet du test E dédié ; les scénarios
// nominaux utilisent 10 min/h, qui couvre 2,86 mL/min avec marge.
static const int kBudgetNominalMinPerHour = 10;

// =============================================================================
// Simulation TEXTBOOK — bassin intégrateur + computePidPure qui intègre lui-même
// =============================================================================
// Utilisée par les tests D1/D2 : ils portent sur la STRUCTURE du correcteur
// (P vs PI) et n'ont pas à connaître la coquille. En production computePidPure
// est appelé avec `freezeIntegral = true` (l'intégrale appartient au module pur) :
// c'est la simulation `simulateProduction()` plus bas qui reproduit ce chemin.
struct LoopStats {
  float finalError;      // erreur au dernier tick
  float maxErrLastDay;   // erreur max sur les dernières 24 h
  float minErrLastDay;   // erreur min sur les dernières 24 h
  float meanErrLastDay;  // erreur moyenne sur les dernières 24 h
  float finalIntegral;   // intégrale au dernier tick (pH·s)
  float finalIntegralTermMlPerMin;  // ki × intégrale
  float totalMl;         // volume total injecté sur toute la simulation
};

static LoopStats simulateClosedLoop(float kp, float ki, float integralMax,
                                    float deadband, int days) {
  const long steps = (long)days * kTicksPerDay;
  const long lastDayFrom = steps - kTicksPerDay;

  float error = 0.0f;      // on part À la consigne : toute erreur observée est
                           // produite par la charge, jamais par l'état initial.
  float integral = 0.0f;
  float lastError = 0.0f;

  LoopStats st;
  st.maxErrLastDay = -1e9f;
  st.minErrLastDay = 1e9f;
  st.totalMl = 0.0f;
  double sumLastDay = 0.0;

  for (long i = 0; i < steps; ++i) {
    PidResult r = computePidPure(kp, ki, /*kd*/0.0f, error, lastError, integral,
                                 kPidDtSec, integralMax, deadband,
                                 /*minFlow*/0.0f, kAmpFlow, /*freeze*/false);
    integral = r.integral;
    lastError = r.lastError;
    st.totalMl += r.flow * kPidDtSec / 60.0f;

    // Bassin : la charge basique fait monter le pH, l'acide le fait descendre.
    error += kPlantPhPerFlowPerSec * (kLoadMlPerMin - r.flow) * kPidDtSec;

    if (i >= lastDayFrom) {
      if (error > st.maxErrLastDay) st.maxErrLastDay = error;
      if (error < st.minErrLastDay) st.minErrLastDay = error;
      sumLastDay += (double)error;
    }
  }

  st.finalError = error;
  st.meanErrLastDay = (float)(sumLastDay / (double)kTicksPerDay);
  st.finalIntegral = integral;
  st.finalIntegralTermMlPerMin = ki * integral;
  return st;
}

// =============================================================================
// Simulation de PRODUCTION — module pur + computePidPure + hystérésis de dosage
// =============================================================================
// Reproduit l'enchaînement EXACT de PumpControllerClass::update(), branche
// automatique (cf. src/pump_controller.cpp) :
//   1. décision d'hystérésis (démarrage à startThreshold, poursuite à
//      stopThreshold) sur l'erreur du tour ;
//   2. phIntegralTick() — À CHAQUE TOUR, que l'on dose ou non ;
//   3. si l'on dose : computePidPure(freezeIntegral = true), bande morte alignée
//      sur l'hystérésis, sortie bornée [0, ampFlow].
// Le seul degré de liberté ajouté est `policy` : il permet d'exhiber le DÉFAUT
// corrigé (bande morte figée à startThreshold) à côté du comportement correct.
enum class DeadbandPolicy {
  kAlignedOnHysteresis,  // production v2.26.0 : actif ? stop : start
  kFrozenOnStart,        // DÉFAUT corrigé : toujours startThreshold
};

struct ProdStats {
  float finalError;
  float minErrLastDay;
  float maxErrLastDay;
  float meanErrLastDay;
  float finalIntegral;
  float finalFillPct;
  float totalMl;
  bool  everFrozen;
  bool  everSaturated;
  long  warnCount;
  long  criticalCount;
};

static ProdStats simulateProduction(float kp, float ki, float integralMax,
                                    int budgetMinPerHour, int days,
                                    DeadbandPolicy policy) {
  const long steps = (long)days * kTicksPerDay;
  const long lastDayFrom = steps - kTicksPerDay;

  PhIntegralState st;
  PhIntegralTuning tun;
  tun.kp = kp;
  tun.ki = ki;
  tun.integralMax = integralMax;
  tun.startThreshold = kStartThreshold;
  tun.stopThreshold = kStopThreshold;

  float error = 0.0f;
  float lastError = 0.0f;
  bool active = false;
  uint32_t now = kT0;

  ProdStats s;
  s.maxErrLastDay = -1e9f;
  s.minErrLastDay = 1e9f;
  s.totalMl = 0.0f;
  s.everFrozen = false;
  s.everSaturated = false;
  s.warnCount = 0;
  s.criticalCount = 0;
  double sumLastDay = 0.0;

  for (long i = 0; i < steps; ++i) {
    // 1. hystérésis de dosage (shouldStartDosingPure / shouldContinueDosingPure,
    //    hors quotas : le sujet est le point de fonctionnement, pas les gardes).
    const bool shouldDose = active ? (error > kStopThreshold)
                                   : (error > kStartThreshold);

    // 2. tick du module PUR — à chaque tour, dosage ou non.
    PhIntegralInputs in;
    in.nowMs = now;
    in.error = error;
    in.ampFlow = kAmpFlow;
    in.budgetMinPerHour = budgetMinPerHour;
    in.quotaNearLimit = false;
    const PhIntegralVerdict v = phIntegralTick(st, in, tun);
    if (v.frozen) s.everFrozen = true;
    if (v.saturated) s.everSaturated = true;
    if (v.emitWarn) s.warnCount++;
    if (v.emitCritical) s.criticalCount++;

    // 3. sortie — l'intégrale n'est PLUS entretenue par computePidPure.
    float flow = 0.0f;
    if (shouldDose) {
      const float deadband = (policy == DeadbandPolicy::kAlignedOnHysteresis)
                                 ? (active ? kStopThreshold : kStartThreshold)
                                 : kStartThreshold;
      PidResult r = computePidPure(kp, ki, 0.0f, error, lastError, st.integral,
                                   kPidDtSec, integralMax, deadband,
                                   /*minFlow*/0.0f, kAmpFlow, /*freeze*/true);
      flow = r.flow;
      lastError = r.lastError;
    }
    active = shouldDose;
    s.totalMl += flow * kPidDtSec / 60.0f;

    error += kPlantPhPerFlowPerSec * (kLoadMlPerMin - flow) * kPidDtSec;
    now += (uint32_t)(kPidDtSec * 1000.0f);

    if (i >= lastDayFrom) {
      if (error > s.maxErrLastDay) s.maxErrLastDay = error;
      if (error < s.minErrLastDay) s.minErrLastDay = error;
      sumLastDay += (double)error;
    }
  }

  s.finalError = error;
  s.meanErrLastDay = (float)(sumLastDay / (double)kTicksPerDay);
  s.finalIntegral = st.integral;
  s.finalFillPct = phIntegralFillPct(st.integral, integralMax);
  return s;
}

// =============================================================================
// D1 — LE TEST DE STRUCTURE : la droop
// =============================================================================

// P PUR : l'erreur résiduelle est NON NULLE et vaut exactement le droop
// analytique e* = charge / kp. C'est la démonstration que la consigne est
// INATTEIGNABLE par construction, quelle que soit la durée d'attente.
void test_D1_p_pur_ne_rejoint_jamais_la_consigne(void) {
  // 10 jours = ~240 constantes de temps de la boucle P : tout transitoire est mort.
  LoopStats p = simulateClosedLoop(kPhKpNormal, /*ki*/0.0f, kPhIntegralMaxNormal,
                                   /*deadband*/0.0f, /*days*/10);

  const float droopAnalytique = kLoadMlPerMin / kPhKpNormal;  // 2,86/8 = 0,3575

  // L'erreur ne tend PAS vers zéro : elle tend vers le droop.
  TEST_ASSERT_FLOAT_WITHIN(0.01f, droopAnalytique, p.finalError);
  TEST_ASSERT_TRUE(p.finalError > 0.30f);
  // Et elle y RESTE : sur les dernières 24 h, jamais elle ne s'approche de la
  // consigne (le minimum du dernier jour est encore au-dessus de 0,30).
  TEST_ASSERT_TRUE(p.minErrLastDay > 0.30f);
  // Le régulateur dose en permanence sans progresser : ~2,86 mL/min × 24 h × 10 j.
  TEST_ASSERT_TRUE(p.totalMl > 30000.0f);
}

// PI (constantes de PRODUCTION) : l'erreur tend vers ZÉRO, et l'intégrale a pris
// à sa charge la TOTALITÉ du besoin d'équilibre (le terme P s'annule avec l'erreur).
void test_D1_pi_rejoint_la_consigne(void) {
  LoopStats pi = simulateClosedLoop(kPhKpNormal, kPhKiNormal, kPhIntegralMaxNormal,
                                    /*deadband*/0.0f, /*days*/10);

  // Convergence : erreur finale négligeable devant le bruit du filtre (~0,05 pH).
  TEST_ASSERT_TRUE(fabsf(pi.finalError) < 0.005f);
  TEST_ASSERT_TRUE(fabsf(pi.meanErrLastDay) < 0.005f);
  // À l'équilibre le terme P est nul : l'intégrale fournit TOUTE la charge.
  TEST_ASSERT_FLOAT_WITHIN(0.15f, kLoadMlPerMin, pi.finalIntegralTermMlPerMin);
  // …et elle le fait SANS saturer (marge de dérive saisonnière préservée).
  TEST_ASSERT_TRUE(pi.finalIntegral < kPhIntegralMaxNormal);
}

// Comparaison directe, sur la MÊME charge : le PI fait mieux d'au moins un ordre
// de grandeur. C'est l'énoncé qui justifie le cycle v2.26.0.
void test_D1_pi_bat_le_p_pur_d_un_ordre_de_grandeur(void) {
  LoopStats p  = simulateClosedLoop(kPhKpNormal, 0.0f, kPhIntegralMaxNormal, 0.0f, 10);
  LoopStats pi = simulateClosedLoop(kPhKpNormal, kPhKiNormal, kPhIntegralMaxNormal,
                                    0.0f, 10);
  TEST_ASSERT_TRUE(fabsf(pi.finalError) < 0.1f * fabsf(p.finalError));
}

// RÉÉCRIT (v2.26.0 correction). L'ancienne version attendait un résidu égal à
// `phStartThreshold` (0,05) : c'était la conséquence du DÉFAUT — la bande morte
// valait toujours le seuil de DÉMARRAGE, y compris session ouverte, si bien que la
// sortie retombait à zéro dès 0,05 alors que l'hystérésis déclare poursuivre
// jusqu'à 0,01. Avec la bande alignée, le résidu vaut le seuil d'ARRÊT.
void test_D1_droop_avec_bande_morte_de_production(void) {
  ProdStats p  = simulateProduction(kPhKpNormal, 0.0f, kPhIntegralMaxNormal,
                                    kBudgetNominalMinPerHour, 12,
                                    DeadbandPolicy::kAlignedOnHysteresis);
  ProdStats pi = simulateProduction(kPhKpNormal, kPhKiNormal, kPhIntegralMaxNormal,
                                    kBudgetNominalMinPerHour, 12,
                                    DeadbandPolicy::kAlignedOnHysteresis);
  // P pur : l'erreur reste très au-dessus du seuil de démarrage (droop 0,358).
  TEST_ASSERT_TRUE(p.minErrLastDay > 5.0f * kStartThreshold);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, kLoadMlPerMin / kPhKpNormal, p.meanErrLastDay);
  // PI : le résidu vaut le seuil d'ARRÊT (0,01), pas le seuil de démarrage —
  // correcteur et ordonnanceur visent le MÊME point de fonctionnement.
  TEST_ASSERT_FLOAT_WITHIN(0.005f, kStopThreshold, pi.meanErrLastDay);
  TEST_ASSERT_TRUE(pi.maxErrLastDay < kStartThreshold);
  TEST_ASSERT_TRUE(pi.meanErrLastDay < 0.1f * p.meanErrLastDay);
}

// RÉÉCRIT — ce test affirmait exactement le CONTRAIRE (« aucune voie de
// décharge »), et sa prémisse est morte avec l'intégration signée : l'intégrande
// vaut `erreur − stopThreshold`, donc toute erreur SOUS le seuil d'arrêt décharge
// l'intégrale, sans avoir besoin d'une excursion sous la consigne.
void test_P_la_decharge_existe_sans_excursion_sous_la_consigne(void) {
  PhIntegralState st;
  PhIntegralTuning tun;
  tun.kp = kPhKpNormal;
  tun.ki = kPhKiNormal;
  tun.integralMax = kPhIntegralMaxNormal;
  tun.startThreshold = kStartThreshold;
  tun.stopThreshold = kStopThreshold;

  st.integral = 10000.0f;
  st.lastTickMs = kT0;

  // Erreur POSITIVE mais sous le seuil d'arrêt : +0,005 pH. L'ancienne règle
  // (`error < 0`) n'y voyait aucune raison de décharger.
  TEST_ASSERT_TRUE(0.005f > 0.0f);
  PhIntegralInputs in;
  in.error = 0.005f;
  in.ampFlow = kAmpFlow;
  in.budgetMinPerHour = 0;
  in.quotaNearLimit = false;

  uint32_t t = kT0;
  for (int i = 0; i < 360; ++i) {  // 1 h de ticks de 10 s
    t += 10000u;
    in.nowMs = t;
    phIntegralTick(st, in, tun);
  }
  // (0,005 − 0,01) × 3600 s = −18 pH·s : la décharge est structurelle.
  // Tolérance 0,5 pH·s : à 10 000 pH·s l'ulp du float vaut 1e-3, et 360 additions
  // de −0,05 accumulent jusqu'à ~0,2 pH·s d'erreur d'arrondi (c'est exactement le
  // phénomène que le quantum d'une seconde borne — cf. test dédié).
  TEST_ASSERT_FLOAT_WITHIN(0.5f, 10000.0f - 18.0f, st.integral);
  TEST_ASSERT_TRUE(st.integral < 10000.0f);
  // Et l'oubli exponentiel n'a PAS été armé (l'erreur n'est pas négative) :
  // la décharge vient bien de l'intégration signée.
  TEST_ASSERT_EQUAL_UINT32(0u, st.decayMs);
}

// La levée de Ki ne coûte pas de produit : à charge égale, le volume injecté par le
// PI reste du même ordre que celui du P pur (il ne « pousse » pas plus, il place
// la même quantité au bon endroit). Garde-fou chimique.
void test_D1_pi_ne_consomme_pas_plus_de_produit_que_le_p_pur(void) {
  LoopStats p  = simulateClosedLoop(kPhKpNormal, 0.0f, kPhIntegralMaxNormal, 0.0f, 10);
  LoopStats pi = simulateClosedLoop(kPhKpNormal, kPhKiNormal, kPhIntegralMaxNormal,
                                    0.0f, 10);
  TEST_ASSERT_TRUE(pi.totalMl < 1.25f * p.totalMl);
}

// =============================================================================
// D2 — LE PIÈGE : Ki levé mais integralMax laissé à 50 pH·s
// =============================================================================
// L'ancien plafond (inerte tant que Ki valait 0) plafonne la contribution
// intégrale à ki × 50 = 0,0185 mL/min, soit 0,6 % du besoin. Le correcteur est
// « PI » sur le papier et reste un P pur dans le bassin. CE test meurt si
// kPhIntegralMaxNormal revient à 50.
static const float kLegacyIntegralMax = 50.0f;  // valeur historique, pour COMPARER

void test_D2_plafond_legacy_50_laisse_la_droop_intacte(void) {
  LoopStats legacy = simulateClosedLoop(kPhKpNormal, kPhKiNormal,
                                        kLegacyIntegralMax, 0.0f, 10);
  const float droopAnalytique = kLoadMlPerMin / kPhKpNormal;
  // Droop quasi intacte : le « PI » n'a rien changé.
  TEST_ASSERT_FLOAT_WITHIN(0.01f, droopAnalytique, legacy.finalError);
  // Contribution intégrale ridicule devant le besoin.
  TEST_ASSERT_TRUE(legacy.finalIntegralTermMlPerMin < 0.02f * kLoadMlPerMin);

  // …alors que le plafond de PRODUCTION converge. C'est cette seconde assertion
  // qui tue la mutation `kPhIntegralMaxNormal = 50`.
  LoopStats prod = simulateClosedLoop(kPhKpNormal, kPhKiNormal,
                                      kPhIntegralMaxNormal, 0.0f, 10);
  TEST_ASSERT_TRUE(fabsf(prod.finalError) < 0.005f);
  TEST_ASSERT_TRUE(prod.finalIntegralTermMlPerMin > 0.9f * kLoadMlPerMin);
}

// Le plafond de production doit COUVRIR la charge d'équilibre mesurée, avec marge.
// (Formulation « autorité » : l'invariant de dimensionnement, indépendant de toute
// simulation.) Meurt pour tout plafond sous-dimensionné, sur les trois vitesses.
void test_D2_plafond_couvre_la_charge_d_equilibre_aux_trois_vitesses(void) {
  TEST_ASSERT_TRUE(kPhKiSlow   * kPhIntegralMaxSlow   > 1.5f * kLoadMlPerMin);
  TEST_ASSERT_TRUE(kPhKiNormal * kPhIntegralMaxNormal > 1.5f * kLoadMlPerMin);
  TEST_ASSERT_TRUE(kPhKiFast   * kPhIntegralMaxFast   > 1.5f * kLoadMlPerMin);
}

// =============================================================================
// I1 — Charge de l'intégrale jusqu'à integralMax, PAR VITESSE
// =============================================================================
// Charge l'intégrale à erreur constante et renvoie le nombre de ticks nécessaires
// pour atteindre le plafond (0 = jamais atteint dans la limite fixée).
static long chargeToCeiling(float ki, float integralMax, float error,
                            long maxTicks, float* outIntegral) {
  float integral = 0.0f;
  float lastError = 0.0f;
  long ticks = 0;
  for (long i = 0; i < maxTicks; ++i) {
    PidResult r = computePidPure(kPhKpNormal, ki, 0.0f, error, lastError, integral,
                                 kPidDtSec, integralMax, kStartThreshold,
                                 0.0f, kAmpFlow, /*freeze*/false);
    integral = r.integral;
    lastError = r.lastError;
    ++ticks;
    if (integral >= integralMax) break;
  }
  *outIntegral = integral;
  return (integral >= integralMax) ? ticks : 0;
}

static void assertCeilingForSpeed(float ki, float integralMax, const char* label) {
  float integral = 0.0f;
  // Erreur de 0,5 pH soutenue : 5 pH·s par tick de 10 s.
  long ticks = chargeToCeiling(ki, integralMax, 0.5f, /*maxTicks*/200000, &integral);
  char msg[96];
  snprintf(msg, sizeof(msg), "plafond jamais atteint (%s)", label);
  TEST_ASSERT_TRUE_MESSAGE(ticks > 0, msg);
  // Le plafond est ATTEINT EXACTEMENT (clamp, pas de dépassement d'un pas).
  TEST_ASSERT_FLOAT_WITHIN(kEps, integralMax, integral);
  // Durée théorique : integralMax / (erreur × dt) ticks, à un tick près.
  const long expected = (long)(integralMax / (0.5f * kPidDtSec)) + 1;
  TEST_ASSERT_INT_WITHIN(2, (int)expected, (int)ticks);
  // Une fois au plafond, l'accumulation NE DÉPASSE JAMAIS (clamp répété).
  float lastError = 0.5f;
  for (int i = 0; i < 100; ++i) {
    PidResult r = computePidPure(kPhKpNormal, ki, 0.0f, 0.5f, lastError, integral,
                                 kPidDtSec, integralMax, kStartThreshold,
                                 0.0f, kAmpFlow, false);
    integral = r.integral;
    lastError = r.lastError;
  }
  TEST_ASSERT_FLOAT_WITHIN(kEps, integralMax, integral);
  // Et au plafond, la contribution vaut l'AUTORITÉ posée (6 mL/min), pas plus.
  TEST_ASSERT_FLOAT_WITHIN(0.06f, kPhIntegralAuthorityMlPerMin, ki * integral);
}

void test_I1_charge_jusqu_au_plafond_vitesse_slow(void) {
  assertCeilingForSpeed(kPhKiSlow, kPhIntegralMaxSlow, "slow");
}

void test_I1_charge_jusqu_au_plafond_vitesse_normal(void) {
  assertCeilingForSpeed(kPhKiNormal, kPhIntegralMaxNormal, "normal");
}

void test_I1_charge_jusqu_au_plafond_vitesse_fast(void) {
  assertCeilingForSpeed(kPhKiFast, kPhIntegralMaxFast, "fast");
}

// Le plafond VARIE avec la vitesse (c'est le point de conception le plus
// contre-intuitif de v2.26.0 : integralMax n'est pas une constante unique).
// Une régulation « rapide » sature donc PLUS TÔT, à autorité identique.
void test_I1_le_plafond_varie_avec_la_vitesse_et_reste_ordonne(void) {
  TEST_ASSERT_TRUE(kPhIntegralMaxFast < kPhIntegralMaxNormal);
  TEST_ASSERT_TRUE(kPhIntegralMaxNormal < kPhIntegralMaxSlow);

  float iSlow = 0.0f, iNormal = 0.0f, iFast = 0.0f;
  long tSlow   = chargeToCeiling(kPhKiSlow,   kPhIntegralMaxSlow,   0.5f, 200000, &iSlow);
  long tNormal = chargeToCeiling(kPhKiNormal, kPhIntegralMaxNormal, 0.5f, 200000, &iNormal);
  long tFast   = chargeToCeiling(kPhKiFast,   kPhIntegralMaxFast,   0.5f, 200000, &iFast);
  TEST_ASSERT_TRUE(tFast > 0 && tNormal > 0 && tSlow > 0);
  TEST_ASSERT_TRUE(tFast < tNormal);
  TEST_ASSERT_TRUE(tNormal < tSlow);
}

// =============================================================================
// I2 — Verrou d'autorité À L'EXÉCUTION (doublon volontaire du static_assert)
// =============================================================================
// Les static_assert de constants.h tombent à la COMPILATION ; ce test tombe au
// RUN. Doublon assumé : un `#if 0`, un déplacement de bloc ou une désactivation
// d'assertion neutraliserait le premier sans que rien ne le signale.
void test_I2_autorite_integrale_est_de_6_ml_par_min_aux_trois_vitesses(void) {
  TEST_ASSERT_FLOAT_WITHIN(0.06f, kPhIntegralAuthorityMlPerMin,
                           kPhKiSlow * kPhIntegralMaxSlow);
  TEST_ASSERT_FLOAT_WITHIN(0.06f, kPhIntegralAuthorityMlPerMin,
                           kPhKiNormal * kPhIntegralMaxNormal);
  TEST_ASSERT_FLOAT_WITHIN(0.06f, kPhIntegralAuthorityMlPerMin,
                           kPhKiFast * kPhIntegralMaxFast);
  // ki = kp / Ti, sur les trois vitesses (dérivation, pas de valeur en dur).
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, kPhKpSlow   / kPhIntegralTiSlowS,   kPhKiSlow);
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, kPhKpNormal / kPhIntegralTiNormalS, kPhKiNormal);
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, kPhKpFast   / kPhIntegralTiFastS,   kPhKiFast);
  // Ki_ORP HORS PÉRIMÈTRE : aucune constante intégrale ORP n'existe. Kd = 0
  // partout — vérifié par l'absence de tout kPhKd*/kOrpKd* (compilation).
}

// =============================================================================
// A1 — Anti-windup de computePidPure
// =============================================================================

// Saturation PROLONGÉE : la sortie est écrêtée à maxFlow pendant des heures.
// L'intégrale ne doit jamais dépasser son plafond, et le bornage est SYMÉTRIQUE.
void test_A1_saturation_prolongee_ne_fait_pas_exploser_l_integrale(void) {
  float integral = 0.0f, lastError = 0.0f;
  // 6 h d'erreur énorme (3 pH) : la sortie sature (kp×3 = 24 ≈ maxFlow).
  for (long i = 0; i < 6 * 360; ++i) {
    PidResult r = computePidPure(kPhKpNormal, kPhKiNormal, 0.0f, 3.0f, lastError,
                                 integral, kPidDtSec, kPhIntegralMaxNormal,
                                 kStartThreshold, 0.0f, kAmpFlow, /*freeze*/false);
    integral = r.integral;
    lastError = r.lastError;
    // INVARIANT DE SÉCURITÉ : la sortie ne dépasse JAMAIS le débit d'amplitude,
    // quelle que soit la charge de l'intégrale.
    TEST_ASSERT_TRUE(r.flow <= kAmpFlow + kEps);
  }
  TEST_ASSERT_FLOAT_WITHIN(kEps, kPhIntegralMaxNormal, integral);  // borné, pas explosé
}

void test_A1_bornage_symetrique_avec_les_constantes_de_production(void) {
  // Bornage haut.
  PidResult r = computePidPure(kPhKpNormal, kPhKiNormal, 0.0f, 5.0f, 0.0f,
                               kPhIntegralMaxNormal, 3600.0f, kPhIntegralMaxNormal,
                               kStartThreshold, 0.0f, kAmpFlow, false);
  TEST_ASSERT_FLOAT_WITHIN(kEps, kPhIntegralMaxNormal, r.integral);
  // Bornage bas, à la MÊME valeur absolue (une saturation négative est tout aussi
  // anormale : elle représenterait une autorité d'alcalinisation non prévue).
  r = computePidPure(kPhKpNormal, kPhKiNormal, 0.0f, -5.0f, 0.0f,
                     -kPhIntegralMaxNormal, 3600.0f, kPhIntegralMaxNormal,
                     kStartThreshold, 0.0f, kAmpFlow, false);
  TEST_ASSERT_FLOAT_WITHIN(kEps, -kPhIntegralMaxNormal, r.integral);
}

void test_A1_gel_suspend_totalement_l_accumulation(void) {
  const float in = 9000.0f;
  // Gelée : même sur 1 000 ticks à forte erreur, l'intégrale ne bouge PAS d'un pH·s.
  float integral = in, lastError = 0.0f;
  for (int i = 0; i < 1000; ++i) {
    PidResult r = computePidPure(kPhKpNormal, kPhKiNormal, 0.0f, 1.0f, lastError,
                                 integral, kPidDtSec, kPhIntegralMaxNormal,
                                 kStartThreshold, 0.0f, kAmpFlow, /*freeze*/true);
    integral = r.integral;
    lastError = r.lastError;
  }
  TEST_ASSERT_FLOAT_WITHIN(kEps, in, integral);
  // Le terme ACQUIS continue d'être SERVI pendant le gel (on cesse d'ajouter, on
  // ne retire pas) : le débit inclut bien ki × intégrale.
  PidResult r = computePidPure(kPhKpNormal, kPhKiNormal, 0.0f, 0.10f, 0.0f, in,
                               kPidDtSec, kPhIntegralMaxNormal, kStartThreshold,
                               0.0f, kAmpFlow, /*freeze*/true);
  TEST_ASSERT_FLOAT_WITHIN(kEps, kPhKpNormal * 0.10f + kPhKiNormal * in, r.flow);
}

// LE TROU FERMÉ EN v2.26.0 : la saturation se jugeait sur le terme PROPORTIONNEL
// SEUL. Il existe une fenêtre d'erreur où P < maxFlow (donc « pas saturé » selon
// l'ancienne règle) alors que P + I dépasse maxFlow : la sortie était écrêtée par
// computePidPure SANS que personne ne gèle l'accumulation — windup pur.
// Ce test EXHIBE la fenêtre puis vérifie que la règle « demande TOTALE » la ferme.
void test_A1_fenetre_de_windup_de_l_ancienne_regle_P_seul(void) {
  // Fenêtre : P < maxFlow ⇔ error < 24,7/8 = 3,09 ; P + I_max > maxFlow ⇔
  // 8·e + 6 > 24,7 ⇔ error > 2,34. On prend 2,60.
  const float error = 2.60f;
  const float p = kPhKpNormal * error;                       // 20,8
  TEST_ASSERT_TRUE(p < kAmpFlow);                            // ancienne règle : PAS saturé
  TEST_ASSERT_TRUE(p + kPhIntegralAuthorityMlPerMin > kAmpFlow);  // mais P+I déborde

  // (a) ANCIENNE règle (gel sur P seul) : l'intégrale monte jusqu'au PLAFOND et la
  //     sortie est écrêtée — le windup s'installe en silence.
  float integral = 0.0f, lastError = 0.0f;
  bool clampedSeen = false;
  for (long i = 0; i < 20000; ++i) {
    const bool freezeLegacy = (kPhKpNormal * error >= kAmpFlow);  // faux ici
    PidResult r = computePidPure(kPhKpNormal, kPhKiNormal, 0.0f, error, lastError,
                                 integral, kPidDtSec, kPhIntegralMaxNormal,
                                 kStartThreshold, 0.0f, kAmpFlow, freezeLegacy);
    integral = r.integral;
    lastError = r.lastError;
    if (kPhKpNormal * error + kPhKiNormal * integral > kAmpFlow + kEps) clampedSeen = true;
  }
  TEST_ASSERT_TRUE(clampedSeen);  // demande > débit réalisable = windup
  TEST_ASSERT_FLOAT_WITHIN(kEps, kPhIntegralMaxNormal, integral);

  // (b) NOUVELLE règle (gel sur la demande TOTALE, intégrale AVANT intégration) :
  //     l'accumulation s'arrête au point où P + I atteint le débit réalisable.
  integral = 0.0f;
  lastError = 0.0f;
  for (long i = 0; i < 20000; ++i) {
    const bool freezeTotal = (kPhKpNormal * error + kPhKiNormal * integral >= kAmpFlow);
    PidResult r = computePidPure(kPhKpNormal, kPhKiNormal, 0.0f, error, lastError,
                                 integral, kPidDtSec, kPhIntegralMaxNormal,
                                 kStartThreshold, 0.0f, kAmpFlow, freezeTotal);
    integral = r.integral;
    lastError = r.lastError;
  }
  // Arrêt bien AVANT le plafond : on s'immobilise à (maxFlow − P)/ki ≈ 10 530 pH·s
  // (un pas d'intégration près : le gel est évalué avant l'ajout).
  const float expected = (kAmpFlow - p) / kPhKiNormal;
  TEST_ASSERT_TRUE(integral < kPhIntegralMaxNormal);
  TEST_ASSERT_FLOAT_WITHIN(error * kPidDtSec + 1.0f, expected, integral);
}

// =============================================================================
// P* — LE MODULE PUR src/ph_integral_logic.h, APPELÉ DIRECTEMENT
// =============================================================================
// Plus aucun modèle recopié : ces tests exécutent le code de production.

static PhIntegralTuning tuning(float ki = kPhKiNormal,
                               float integralMax = kPhIntegralMaxNormal,
                               float kp = kPhKpNormal) {
  PhIntegralTuning t;
  t.kp = kp;
  t.ki = ki;
  t.integralMax = integralMax;
  t.startThreshold = kStartThreshold;
  t.stopThreshold = kStopThreshold;
  return t;
}

// budget = 0 → illimité : le débit soutenable vaut l'amplitude (24,7 mL/min), donc
// aucune saturation parasite ne vient geler les scénarios d'accumulation.
static PhIntegralInputs inputs(uint32_t nowMs, float error,
                               float ampFlow = kAmpFlow, int budget = 0,
                               bool quota = false) {
  PhIntegralInputs in;
  in.nowMs = nowMs;
  in.error = error;
  in.ampFlow = ampFlow;
  in.budgetMinPerHour = budget;
  in.quotaNearLimit = quota;
  return in;
}

// --- Base de temps ------------------------------------------------------------

void test_P_premier_tick_arme_la_base_de_temps_sans_integrer(void) {
  PhIntegralState st;
  st.integral = 1000.0f;
  phIntegralTick(st, inputs(kT0, 0.5f), tuning());
  TEST_ASSERT_EQUAL_UINT32(kT0, st.lastTickMs);
  TEST_ASSERT_EQUAL_UINT32(0u, st.pendingMs);
  TEST_ASSERT_FLOAT_WITHIN(kEps, 1000.0f, st.integral);  // rien n'a été SYNTHÉTISÉ
}

// LE QUANTUM D'UNE SECONDE : rien n'est crédité sous le quantum, et le reliquat
// est REPORTÉ (aucun temps perdu tant que la branche tourne).
void test_P_charge_par_quanta_d_une_seconde_avec_report_du_reliquat(void) {
  PhIntegralState st;
  st.lastTickMs = kT0;
  const PhIntegralTuning tun = tuning();

  // 500 ms : sous le quantum → rien, mais le temps est GARDÉ.
  phIntegralTick(st, inputs(kT0 + 500u, 0.5f), tun);
  TEST_ASSERT_FLOAT_WITHIN(kEps, 0.0f, st.integral);
  TEST_ASSERT_EQUAL_UINT32(500u, st.pendingMs);

  // +500 ms : le quantum est atteint → UN pas de (0,5 − 0,01) × 1 s.
  phIntegralTick(st, inputs(kT0 + 1000u, 0.5f), tun);
  TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.49f, st.integral);
  TEST_ASSERT_EQUAL_UINT32(0u, st.pendingMs);

  // +1 500 ms : un seul pas crédité, 500 ms reportées.
  phIntegralTick(st, inputs(kT0 + 2500u, 0.5f), tun);
  TEST_ASSERT_FLOAT_WITHIN(1e-4f, 2.0f * 0.49f, st.integral);
  TEST_ASSERT_EQUAL_UINT32(500u, st.pendingMs);

  TEST_ASSERT_EQUAL_UINT32(1000UL, kPhIntegralChargeQuantumMs);
}

// POURQUOI le quantum existe : à l'échelle du plafond, un incrément de 5 ms est
// PERDU à l'addition flottante. Ce test fait la démonstration arithmétique, puis
// vérifie que le module, lui, crédite bien le temps observé.
void test_P_un_increment_de_5ms_serait_perdu_dans_le_float(void) {
  const float base = 16000.0f;  // ordre de grandeur du plafond `normal` (16 200)
  // ulp(16000) = 2^13 × 2^-23 ≈ 9,8e-4 ; l'incrément 0,01 pH × 5 ms = 5e-5 est
  // très en dessous du demi-ulp → l'addition est un NO-OP, indéfiniment.
  float naive = base;
  for (int i = 0; i < 2000; ++i) naive += 0.01f * 0.005f;  // 2 000 × 5 ms = 10 s
  TEST_ASSERT_TRUE(naive == base);  // 10 s d'intégration purement PERDUES

  // Le même temps, par quanta d'1 s, produit un incrément REPRÉSENTABLE.
  float quantized = base;
  for (int i = 0; i < 10; ++i) quantized += 0.01f * 1.0f;
  TEST_ASSERT_TRUE(quantized > base);

  // Et c'est bien ce que fait le module, MÊME appelé toutes les 5 ms.
  PhIntegralState st;
  st.integral = base;
  st.lastTickMs = kT0;
  const PhIntegralTuning tun = tuning();
  uint32_t t = kT0;
  for (int i = 0; i < 2000; ++i) {
    t += 5u;
    phIntegralTick(st, inputs(t, 0.02f), tun);  // intégrande = 0,01 pH/s
  }
  TEST_ASSERT_TRUE(st.integral > base);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, base + 0.10f, st.integral);
}

// REJET DES dt ABERRANTS : un trou d'évaluation n'est JAMAIS crédité d'un bloc.
void test_P_un_dt_superieur_a_10s_est_rejete_jamais_credite(void) {
  const PhIntegralTuning tun = tuning();
  TEST_ASSERT_EQUAL_UINT32(10000UL, kPhIntegralMaxTickDtMs);

  // 10 000 ms PILE : frontière inclusive → 10 pas crédités.
  PhIntegralState ok;
  ok.lastTickMs = kT0;
  phIntegralTick(ok, inputs(kT0 + 10000u, 0.5f), tun);
  TEST_ASSERT_FLOAT_WITHIN(1e-3f, 4.9f, ok.integral);

  // 10 001 ms : trou d'évaluation → RIEN, et la base de temps repart de now.
  PhIntegralState hole;
  hole.lastTickMs = kT0;
  phIntegralTick(hole, inputs(kT0 + 10001u, 0.5f), tun);
  TEST_ASSERT_FLOAT_WITHIN(kEps, 0.0f, hole.integral);
  TEST_ASSERT_EQUAL_UINT32(kT0 + 10001u, hole.lastTickMs);
  TEST_ASSERT_EQUAL_UINT32(0u, hole.pendingMs);

  // Une heure d'absence ne crédite pas davantage (pas de rattrapage de charge).
  PhIntegralState gap;
  gap.lastTickMs = kT0;
  phIntegralTick(gap, inputs(kT0 + 3600000u, 0.5f), tun);
  TEST_ASSERT_FLOAT_WITHIN(kEps, 0.0f, gap.integral);

  // Et le RELIQUAT accumulé avant le trou est abandonné (il n'a plus de sens).
  PhIntegralState carry;
  carry.lastTickMs = kT0;
  phIntegralTick(carry, inputs(kT0 + 900u, 0.5f), tun);       // 900 ms en attente
  TEST_ASSERT_EQUAL_UINT32(900u, carry.pendingMs);
  phIntegralTick(carry, inputs(kT0 + 900u + 20000u, 0.5f), tun);
  TEST_ASSERT_EQUAL_UINT32(0u, carry.pendingMs);
  phIntegralTick(carry, inputs(kT0 + 900u + 20900u, 0.5f), tun);  // 900 ms à nouveau
  TEST_ASSERT_FLOAT_WITHIN(kEps, 0.0f, carry.integral);  // toujours aucun pas
}

// --- Intégration SIGNÉE -------------------------------------------------------

// L'intégrande est `erreur − stopThreshold` : le POINT FIXE de l'intégration est
// le seuil d'ARRÊT, pas la consigne. Trois régimes en un test.
void test_P_l_integrande_est_l_erreur_moins_le_seuil_d_arret(void) {
  const PhIntegralTuning tun = tuning();

  // (a) erreur AU-DESSUS du seuil d'arrêt → charge.
  PhIntegralState up;
  up.lastTickMs = kT0;
  phIntegralTick(up, inputs(kT0 + 10000u, 0.11f), tun);
  TEST_ASSERT_FLOAT_WITHIN(1e-3f, (0.11f - kStopThreshold) * 10.0f, up.integral);

  // (b) erreur EXACTEMENT au seuil d'arrêt → point fixe, rien ne bouge.
  PhIntegralState fix;
  fix.integral = 5000.0f;
  fix.lastTickMs = kT0;
  for (int i = 0; i < 100; ++i) {
    phIntegralTick(fix, inputs(kT0 + (uint32_t)(i + 1) * 10000u, kStopThreshold), tun);
  }
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 5000.0f, fix.integral);

  // (c) erreur POSITIVE mais sous le seuil → décharge (cf. test dédié).
  PhIntegralState down;
  down.integral = 5000.0f;
  down.lastTickMs = kT0;
  phIntegralTick(down, inputs(kT0 + 10000u, 0.0f), tun);
  TEST_ASSERT_FLOAT_WITHIN(1e-3f, 5000.0f - kStopThreshold * 10.0f, down.integral);
}

void test_P_bornage_symetrique_du_module(void) {
  const PhIntegralTuning tun = tuning();
  // Haut : 3 h d'erreur 2 pH par ticks de 10 s → plafonné.
  PhIntegralState hi;
  hi.lastTickMs = kT0;
  for (int i = 0; i < 1080; ++i) {
    phIntegralTick(hi, inputs(kT0 + (uint32_t)(i + 1) * 10000u, 2.0f), tun);
  }
  TEST_ASSERT_FLOAT_WITHIN(kEps, kPhIntegralMaxNormal, hi.integral);

  // Bas : erreur −0,10 (sous la consigne mais AU-DESSUS du seuil de reset sec
  // −0,15, sinon la purge court-circuiterait le scénario) — l'oubli exponentiel
  // ne peut pas empêcher le bornage négatif de s'appliquer.
  PhIntegralState lo;
  lo.integral = -kPhIntegralMaxNormal + 5.0f;
  lo.lastTickMs = kT0;
  lo.decayMs = kT0;
  for (int i = 0; i < 200; ++i) {
    phIntegralTick(lo, inputs(kT0 + (uint32_t)(i + 1) * 10000u, -0.10f), tun);
  }
  TEST_ASSERT_TRUE(lo.integral >= -kPhIntegralMaxNormal - kEps);
}

// --- C3 : soutenabilité et gel ASYMÉTRIQUE ------------------------------------

void test_P_debit_soutenable_tient_compte_du_budget_horaire(void) {
  // Le prédicat de production : ampFlow × minutes/60.
  TEST_ASSERT_FLOAT_WITHIN(1e-3f, 24.7f * 5.0f / 60.0f,
                           phSustainableAvgFlow(24.7f, 5));
  TEST_ASSERT_FLOAT_WITHIN(1e-3f, 24.7f * 10.0f / 60.0f,
                           phSustainableAvgFlow(24.7f, 10));
  // Budget désactivé ou ≥ 60 min/h : la pompe peut tourner en continu.
  TEST_ASSERT_FLOAT_WITHIN(1e-3f, 24.7f, phSustainableAvgFlow(24.7f, 0));
  TEST_ASSERT_FLOAT_WITHIN(1e-3f, 24.7f, phSustainableAvgFlow(24.7f, -1));
  TEST_ASSERT_FLOAT_WITHIN(1e-3f, 24.7f, phSustainableAvgFlow(24.7f, 60));
  TEST_ASSERT_FLOAT_WITHIN(1e-3f, 24.7f, phSustainableAvgFlow(24.7f, 90));
  // Pompe NON caractérisée → 0 (l'appelant traite en fail-closed).
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0f, phSustainableAvgFlow(0.0f, 5));
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0f, phSustainableAvgFlow(-3.0f, 5));
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0f, phSustainableAvgFlow(NAN, 5));
  // Le budget par défaut divise l'amplitude par 12 : c'est CE plafond (~2 mL/min)
  // que la demande dépasse réellement, pas les 24,7 mL/min de l'ancien prédicat.
  TEST_ASSERT_TRUE(phSustainableAvgFlow(kAmpFlow, 5) < 0.1f * kAmpFlow);
}

// LA FENÊTRE QUE L'ANCIEN PRÉDICAT NE VOYAIT PAS : une demande de 2,5 mL/min est
// très en dessous de l'amplitude (jamais gelée avant) mais AU-DESSUS du débit
// moyen soutenable (gelée désormais).
void test_P_gel_sur_le_debit_soutenable_pas_sur_l_amplitude(void) {
  const PhIntegralTuning tun = tuning();
  const float sustainable = phSustainableAvgFlow(kAmpFlow, 5);  // ≈ 2,058
  // Demande construite à 2,5 mL/min : integral tel que ki·I + kp·e = 2,5.
  const float error = 0.02f;
  const float integral = (2.5f - kPhKpNormal * error) / kPhKiNormal;
  TEST_ASSERT_TRUE(2.5f < kAmpFlow);        // ancien prédicat : PAS saturé
  TEST_ASSERT_TRUE(2.5f > sustainable);     // nouveau prédicat : saturé

  PhIntegralState st;
  st.integral = integral;
  st.lastTickMs = kT0;
  PhIntegralVerdict v =
      phIntegralTick(st, inputs(kT0 + 10000u, error, kAmpFlow, /*budget*/5), tun);
  TEST_ASSERT_TRUE(v.frozen);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, sustainable, v.sustainableMlPerMin);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 2.5f, v.demandMlPerMin);
  TEST_ASSERT_FLOAT_WITHIN(kEps, integral, st.integral);  // accumulation SUSPENDUE

  // Avec le budget illimité, la même demande n'est PAS gelée.
  PhIntegralState unlimited;
  unlimited.integral = integral;
  unlimited.lastTickMs = kT0;
  v = phIntegralTick(unlimited, inputs(kT0 + 10000u, error, kAmpFlow, /*budget*/0), tun);
  TEST_ASSERT_FALSE(v.frozen);
  TEST_ASSERT_TRUE(unlimited.integral > integral);
}

void test_P_amplitude_invalide_gele_en_fail_closed(void) {
  const PhIntegralTuning tun = tuning();
  PhIntegralState st;
  st.integral = 1000.0f;
  st.lastTickMs = kT0;
  PhIntegralVerdict v =
      phIntegralTick(st, inputs(kT0 + 10000u, 0.5f, /*ampFlow*/0.0f, 5), tun);
  TEST_ASSERT_TRUE(v.frozen);              // pompe non caractérisée → gel
  TEST_ASSERT_FALSE(v.saturated);          // et pas d'épisode de saturation
  TEST_ASSERT_FLOAT_WITHIN(kEps, 1000.0f, st.integral);
}

// LE GEL EST ASYMÉTRIQUE : il bloque ce qui AGGRAVE la saturation, jamais la
// décharge. Un gel total (l'ancien) laissait l'intégrale prisonnière de son
// plafond même quand l'erreur était déjà rentrée.
void test_P_le_gel_bloque_la_charge_jamais_la_decharge(void) {
  const PhIntegralTuning tun = tuning();

  // (a) gelé (quota journalier) + erreur au-dessus du seuil → RIEN ne s'ajoute.
  PhIntegralState up;
  up.integral = 8000.0f;
  up.lastTickMs = kT0;
  for (int i = 0; i < 360; ++i) {  // 1 h
    PhIntegralVerdict v = phIntegralTick(
        up, inputs(kT0 + (uint32_t)(i + 1) * 10000u, 0.50f, kAmpFlow, 0,
                   /*quota*/true), tun);
    TEST_ASSERT_TRUE(v.frozen);
  }
  TEST_ASSERT_FLOAT_WITHIN(kEps, 8000.0f, up.integral);

  // (b) MÊME gel, erreur SOUS le seuil d'arrêt → la décharge s'applique quand même.
  PhIntegralState down;
  down.integral = 8000.0f;
  down.lastTickMs = kT0;
  for (int i = 0; i < 360; ++i) {
    PhIntegralVerdict v = phIntegralTick(
        down, inputs(kT0 + (uint32_t)(i + 1) * 10000u, 0.0f, kAmpFlow, 0,
                     /*quota*/true), tun);
    TEST_ASSERT_TRUE(v.frozen);
  }
  // (0 − 0,01) × 3 600 s = −36 pH·s.
  TEST_ASSERT_FLOAT_WITHIN(0.05f, 8000.0f - 36.0f, down.integral);
}

// Quota journalier : le prédicat de fraction (80 %) appartient à la coquille ;
// on verrouille ici la CONSTANTE et l'effet du drapeau sur le module.
void test_P_fraction_de_gel_sur_quota_journalier(void) {
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.80f, kPhIntegralFreezeDailyFraction);
  TEST_ASSERT_TRUE(kPhIntegralFreezeDailyFraction > 0.5f);
  TEST_ASSERT_TRUE(kPhIntegralFreezeDailyFraction < 1.0f);

  // Sans le drapeau, la même situation accumule : c'est bien LUI qui gèle.
  const PhIntegralTuning tun = tuning();
  PhIntegralState st;
  st.lastTickMs = kT0;
  PhIntegralVerdict v =
      phIntegralTick(st, inputs(kT0 + 10000u, 0.5f, kAmpFlow, 0, /*quota*/false), tun);
  TEST_ASSERT_FALSE(v.frozen);
  TEST_ASSERT_TRUE(st.integral > 0.0f);
}

// --- C2 : reset sec et oubli exponentiel --------------------------------------

// LE TUEUR SILENCIEUX : −0,011 pH n'efface plus six heures d'accumulation.
void test_P_erreur_moins_0011_n_efface_pas_l_integrale(void) {
  const float charged = 12000.0f;  // ~6 h d'accumulation à 0,55 pH
  // L'ANCIEN seuil était `error < -phStopThreshold` = −0,01 : −0,011 le franchissait.
  TEST_ASSERT_TRUE(-0.011f < -kStopThreshold);
  // Le NOUVEAU seuil est −(0,05 + 0,10) = −0,15 : −0,011 en est très loin.
  TEST_ASSERT_TRUE(-0.011f > -(kStartThreshold + kPhIntegralResetMargin));

  PhIntegralState st;
  st.integral = charged;
  st.lastTickMs = kT0;
  const PhIntegralTuning tun = tuning();
  bool anyDryReset = false;
  for (int i = 0; i < 60; ++i) {  // 10 min de dérive, ticks de 10 s
    PhIntegralVerdict v =
        phIntegralTick(st, inputs(kT0 + (uint32_t)(i + 1) * 10000u, -0.011f), tun);
    if (v.event == PhIntegralEvent::kDryReset) anyDryReset = true;
  }
  TEST_ASSERT_FALSE(anyDryReset);                  // AUCUNE purge sèche
  TEST_ASSERT_TRUE(st.integral > 0.85f * charged); // l'essentiel est conservé
  TEST_ASSERT_TRUE(st.integral < charged);         // mais l'oubli travaille
}

void test_P_reset_sec_uniquement_au_dela_de_moins_015(void) {
  const float seuil = kStartThreshold + kPhIntegralResetMargin;  // 0,15
  const PhIntegralTuning tun = tuning();

  // Juste EN DEÇÀ (−0,14) : pas de purge.
  PhIntegralState a;
  a.integral = 12000.0f;
  a.lastTickMs = kT0;
  PhIntegralVerdict v = phIntegralTick(a, inputs(kT0 + 10000u, -(seuil - 0.01f)), tun);
  TEST_ASSERT_TRUE(v.event != PhIntegralEvent::kDryReset);
  TEST_ASSERT_TRUE(a.integral > 11000.0f);

  // Frontière stricte : −0,15 pile n'est PAS < −0,15 → pas de purge.
  PhIntegralState b;
  b.integral = 12000.0f;
  b.lastTickMs = kT0;
  v = phIntegralTick(b, inputs(kT0 + 10000u, -seuil), tun);
  TEST_ASSERT_TRUE(v.event != PhIntegralEvent::kDryReset);

  // AU-DELÀ (−0,16) : purge immédiate et complète.
  PhIntegralState c;
  c.integral = 12000.0f;
  c.lastTickMs = kT0;
  v = phIntegralTick(c, inputs(kT0 + 10000u, -(seuil + 0.01f)), tun);
  TEST_ASSERT_TRUE(v.event == PhIntegralEvent::kDryReset);
  TEST_ASSERT_FLOAT_WITHIN(kEps, 12000.0f, v.integralBefore);
  TEST_ASSERT_FLOAT_WITHIN(kEps, 0.0f, c.integral);
  TEST_ASSERT_EQUAL_UINT32(0u, c.decayMs);
  // La base de temps est RÉARMÉE au tick : pas de péremption fantôme derrière.
  TEST_ASSERT_EQUAL_UINT32(kT0 + 10000u, c.lastTickMs);
}

// Oubli exponentiel : demi-vie d'UNE HEURE. Les ticks sont espacés de 60 s (donc
// au-delà de kPhIntegralMaxTickDtMs) : la CHARGE est rejetée, seul l'oubli agit —
// on mesure ainsi la demi-vie sans la superposition de la décharge signée.
void test_P_oubli_demi_vie_1h(void) {
  const float charged = kPhIntegralMaxNormal;
  const PhIntegralTuning tun = tuning();
  PhIntegralState st;
  st.integral = charged;
  st.lastTickMs = kT0;

  uint32_t t = kT0;
  phIntegralTick(st, inputs(t, -0.02f), tun);  // arme l'épisode
  for (int i = 1; i <= 60; ++i) {              // 1 h par pas d'une minute
    t = kT0 + (uint32_t)i * 60000u;
    phIntegralTick(st, inputs(t, -0.02f), tun);
  }
  TEST_ASSERT_FLOAT_WITHIN(0.005f * charged, 0.5f * charged, st.integral);

  for (int i = 61; i <= 120; ++i) {            // deuxième heure → le quart
    t = kT0 + (uint32_t)i * 60000u;
    phIntegralTick(st, inputs(t, -0.02f), tun);
  }
  TEST_ASSERT_FLOAT_WITHIN(0.005f * charged, 0.25f * charged, st.integral);
}

void test_P_oubli_rattrape_par_pas_entiers_apres_une_absence(void) {
  const float charged = kPhIntegralMaxNormal;
  const PhIntegralTuning tun = tuning();
  PhIntegralState st;
  st.integral = charged;
  st.lastTickMs = kT0;
  phIntegralTick(st, inputs(kT0, -0.02f), tun);              // arme
  // La branche automatique n'a pas tourné pendant 90 min : rattrapage en un tour
  // (le dt de charge, lui, est REJETÉ — les deux mécanismes sont indépendants).
  phIntegralTick(st, inputs(kT0 + 5400000u, -0.02f), tun);
  TEST_ASSERT_FLOAT_WITHIN(0.01f * charged, 0.35355f * charged, st.integral);  // 0,5^1,5
}

void test_P_oubli_reliquat_reporte_aucun_temps_perdu(void) {
  const PhIntegralTuning tun = tuning();
  PhIntegralState st;
  st.integral = 10000.0f;
  st.lastTickMs = kT0;
  phIntegralTick(st, inputs(kT0, -0.02f), tun);
  phIntegralTick(st, inputs(kT0 + 59000u, -0.02f), tun);
  TEST_ASSERT_FLOAT_WITHIN(kEps, 10000.0f, st.integral);   // pas encore un pas
  phIntegralTick(st, inputs(kT0 + 118000u, -0.02f), tun);  // 118 s ⇒ UN pas
  TEST_ASSERT_FLOAT_WITHIN(0.5f, 10000.0f * kPhIntegralDecayStepFactor, st.integral);
}

void test_P_oubli_plancher_1_phs_ferme_l_episode(void) {
  const PhIntegralTuning tun = tuning();
  PhIntegralState st;
  st.integral = 2.0f;
  st.lastTickMs = kT0;
  phIntegralTick(st, inputs(kT0, -0.02f), tun);
  // 6 h d'excursion : 2 pH·s × 0,5^6 = 0,031 < 1 → épisode fermé.
  PhIntegralVerdict v = phIntegralTick(st, inputs(kT0 + 21600000u, -0.02f), tun);
  TEST_ASSERT_TRUE(v.event == PhIntegralEvent::kDecayClosed);
  TEST_ASSERT_FLOAT_WITHIN(kEps, 0.0f, st.integral);
  TEST_ASSERT_EQUAL_UINT32(0u, st.decayMs);
}

// RÉÉCRIT : appelle le module au lieu du modèle recopié. L'épisode d'oubli est
// DÉSARMÉ dès que l'erreur redevient positive — et la charge signée reprend.
void test_P_oubli_desarme_des_que_l_erreur_redevient_positive(void) {
  const PhIntegralTuning tun = tuning();
  PhIntegralState st;
  st.integral = 10000.0f;
  st.lastTickMs = kT0;

  phIntegralTick(st, inputs(kT0 + 5000u, -0.02f), tun);  // arme l'épisode
  TEST_ASSERT_TRUE(st.decayMs != 0u);
  const float afterNegative = st.integral;

  // Erreur positive, sous le seuil de démarrage mais AU-DESSUS du seuil d'arrêt.
  phIntegralTick(st, inputs(kT0 + 10000u, 0.03f), tun);
  TEST_ASSERT_EQUAL_UINT32(0u, st.decayMs);              // épisode désarmé
  TEST_ASSERT_TRUE(st.integral > afterNegative);         // et la charge a repris
  TEST_ASSERT_FLOAT_WITHIN(1e-3f, afterNegative + (0.03f - kStopThreshold) * 5.0f,
                           st.integral);
}

void test_P_integrale_pleine_ne_survit_pas_a_24h_sous_consigne(void) {
  const PhIntegralTuning tun = tuning();
  PhIntegralState st;
  st.integral = kPhIntegralMaxNormal;
  st.lastTickMs = kT0;
  phIntegralTick(st, inputs(kT0, -0.02f), tun);
  for (int i = 1; i <= 1440; ++i) {  // 24 h par pas d'une minute
    phIntegralTick(st, inputs(kT0 + (uint32_t)i * 60000u, -0.02f), tun);
  }
  TEST_ASSERT_FLOAT_WITHIN(kEps, 0.0f, st.integral);
}

// Le rattrapage est BORNÉ à 600 pas (10 demi-vies) : aucune boucle longue dans
// update(), quelle que soit la durée de l'absence.
// ⚠️ CARACTÉRISATION (déjà signalée) : le commentaire de constants.h affirme
// qu'« au-delà de 10 demi-vies […] le plancher de 1 pH·s a déjà fermé l'épisode ».
// C'est FAUX pour une intégrale pleine : 0,1 % de 16 200 pH·s = 15,8 pH·s, soit
// 16× le plancher. Résidu chimiquement négligeable (0,006 mL/min), mais
// l'affirmation de la doc ne tient pas.
void test_P_rattrapage_borne_a_600_pas(void) {
  TEST_ASSERT_EQUAL_UINT32(600u, kPhIntegralDecayMaxSteps);
  const PhIntegralTuning tun = tuning();
  PhIntegralState st;
  st.integral = kPhIntegralMaxNormal;
  st.lastTickMs = kT0;
  phIntegralTick(st, inputs(kT0, -0.02f), tun);
  // 7 jours d'absence = 10 080 pas demandés → 600 appliqués, PAS 10 080.
  phIntegralTick(st, inputs(kT0 + 7u * 86400000u, -0.02f), tun);
  TEST_ASSERT_FLOAT_WITHIN(0.5f, kPhIntegralMaxNormal * 0.0009766f, st.integral);
  TEST_ASSERT_TRUE(st.integral > 1.0f);                // le plancher n'a pas fermé
  TEST_ASSERT_TRUE(kPhKiNormal * st.integral < 0.01f); // résidu sans effet chimique
  // Le tour suivant (10 h plus tard) le ramène sous le plancher → épisode fermé.
  phIntegralTick(st, inputs(kT0 + 7u * 86400000u + 36000000u, -0.02f), tun);
  TEST_ASSERT_FLOAT_WITHIN(kEps, 0.0f, st.integral);
  TEST_ASSERT_EQUAL_UINT32(0u, st.decayMs);
}

// --- C4 : péremption 24 h -----------------------------------------------------

void test_P_peremption_24h_purge_l_integrale(void) {
  PhIntegralState st;
  st.integral = 12000.0f;
  st.lastTickMs = kT0;
  // 23 h 59 : rien.
  TEST_ASSERT_FALSE(phIntegralExpire(st, kT0 + kPhIntegralStaleResetMs - 60000u));
  TEST_ASSERT_FLOAT_WITHIN(kEps, 12000.0f, st.integral);
  // 24 h pile (frontière >= inclusive) : purge, et la fonction le SIGNALE.
  TEST_ASSERT_TRUE(phIntegralExpire(st, kT0 + kPhIntegralStaleResetMs));
  TEST_ASSERT_FLOAT_WITHIN(kEps, 0.0f, st.integral);
  TEST_ASSERT_EQUAL_UINT32(0u, st.lastTickMs);  // dt reconstruit, jamais synthétisé
}

void test_P_peremption_ne_touche_pas_un_tick_jamais_evalue(void) {
  PhIntegralState st;
  st.integral = 500.0f;  // situation impossible en prod : on vérifie la garde
  st.lastTickMs = 0;
  TEST_ASSERT_FALSE(phIntegralExpire(st, 10u * 86400000u));
  TEST_ASSERT_FLOAT_WITHIN(kEps, 500.0f, st.integral);
}

void test_P_peremption_d_une_integrale_deja_nulle_ne_signale_rien(void) {
  PhIntegralState st;
  st.integral = 0.0f;
  st.lastTickMs = kT0;
  st.satActive = true;
  TEST_ASSERT_FALSE(phIntegralExpire(st, kT0 + kPhIntegralStaleResetMs));
  TEST_ASSERT_FALSE(st.satActive);  // …mais l'état est bien remis à neuf
}

void test_P_peremption_wrap_safe_du_compteur_millis(void) {
  // millis() boucle à ~49,7 j : la soustraction non signée doit rester juste.
  PhIntegralState st;
  st.integral = 9000.0f;
  st.lastTickMs = 0xFFFFF000u;              // juste avant le wrap
  TEST_ASSERT_FALSE(phIntegralExpire(st, 0x00000100u));  // ~4,3 s plus tard
  TEST_ASSERT_FLOAT_WITHIN(kEps, 9000.0f, st.integral);
  TEST_ASSERT_TRUE(phIntegralExpire(st, 0xFFFFF000u + kPhIntegralStaleResetMs));
}

void test_P_reset_remet_tout_l_etat_a_neuf(void) {
  PhIntegralState st;
  st.integral = 5000.0f;
  st.lastTickMs = kT0;
  st.pendingMs = 700;
  st.decayMs = kT0;
  st.satActive = true;
  st.anomalyHeldMs = 12345;
  st.nextCriticalMs = 999;
  st.warnSent = true;
  st.critSent = true;
  phIntegralReset(st);
  TEST_ASSERT_FLOAT_WITHIN(kEps, 0.0f, st.integral);
  TEST_ASSERT_EQUAL_UINT32(0u, st.lastTickMs);
  TEST_ASSERT_EQUAL_UINT32(0u, st.pendingMs);
  TEST_ASSERT_EQUAL_UINT32(0u, st.decayMs);
  TEST_ASSERT_FALSE(st.satActive);
  TEST_ASSERT_EQUAL_UINT32(0u, st.anomalyHeldMs);
  TEST_ASSERT_EQUAL_UINT32(0u, st.nextCriticalMs);
  TEST_ASSERT_FALSE(st.warnSent);
  TEST_ASSERT_FALSE(st.critSent);
}

// --- Mesure non finie ---------------------------------------------------------

void test_P_mesure_non_finie_gele_et_ne_credite_aucun_temps(void) {
  const PhIntegralTuning tun = tuning();
  PhIntegralState st;
  st.integral = 6000.0f;
  st.lastTickMs = kT0;
  st.pendingMs = 800;
  PhIntegralVerdict v = phIntegralTick(st, inputs(kT0 + 5000u, NAN), tun);
  TEST_ASSERT_TRUE(v.frozen);                     // fail-closed
  TEST_ASSERT_FALSE(v.saturated);
  TEST_ASSERT_EQUAL_UINT32(0u, st.pendingMs);     // aucun temps crédité
  TEST_ASSERT_FLOAT_WITHIN(kEps, 6000.0f, st.integral);
}

// --- C5 : hystérésis de saturation, paliers et rappel -------------------------

// Tuning à ki = 0 pour les scénarios de saturation : la demande vaut alors
// exactement kp × erreur, ce qui rend le RATIO demande/soutenable pilotable au
// pourcent près (l'accumulation, elle, reste active — elle ne dépend pas de ki).
static const int   kSatBudget = 5;   // min/h
static float satSustainable() { return phSustainableAvgFlow(kAmpFlow, kSatBudget); }
static float errorForRatio(float pct) {
  return pct * satSustainable() / (100.0f * kPhKpNormal);
}

void test_P_hysterese_de_saturation_95_puis_85(void) {
  TEST_ASSERT_FLOAT_WITHIN(1e-3f, 95.0f, kPhIntegralSaturationPct);
  TEST_ASSERT_FLOAT_WITHIN(1e-3f, 85.0f, kPhIntegralSaturationClearPct);

  const PhIntegralTuning tun = tuning(/*ki*/0.0f);
  PhIntegralState st;
  st.lastTickMs = kT0;
  uint32_t t = kT0;

  // 94 % : sous le seuil d'entrée → pas d'épisode.
  t += 10000u;
  PhIntegralVerdict v =
      phIntegralTick(st, inputs(t, errorForRatio(94.0f), kAmpFlow, kSatBudget), tun);
  TEST_ASSERT_FALSE(v.saturated);

  // 95 % : entrée (frontière inclusive).
  t += 10000u;
  v = phIntegralTick(st, inputs(t, errorForRatio(95.0f), kAmpFlow, kSatBudget), tun);
  TEST_ASSERT_TRUE(v.saturated);

  // 90 % : DANS la bande d'hystérésis → l'épisode reste ouvert.
  t += 10000u;
  v = phIntegralTick(st, inputs(t, errorForRatio(90.0f), kAmpFlow, kSatBudget), tun);
  TEST_ASSERT_TRUE(v.saturated);

  // 85 % pile : la sortie est STRICTE (< 85) → toujours ouvert.
  t += 10000u;
  v = phIntegralTick(st, inputs(t, errorForRatio(85.0f), kAmpFlow, kSatBudget), tun);
  TEST_ASSERT_TRUE(v.saturated);

  // 84 % : sortie de saturation.
  t += 10000u;
  v = phIntegralTick(st, inputs(t, errorForRatio(84.0f), kAmpFlow, kSatBudget), tun);
  TEST_ASSERT_FALSE(v.saturated);
}

// Le seuil porte sur la DEMANDE rapportée au soutenable, PAS sur le remplissage
// de l'anti-windup : une intégrale à 99 % du plafond avec un débit soutenable
// confortable n'est pas une anomalie.
void test_P_le_seuil_porte_sur_la_demande_pas_sur_le_remplissage(void) {
  const PhIntegralTuning tun = tuning();
  PhIntegralState st;
  st.integral = 0.99f * kPhIntegralMaxNormal;  // remplissage 99 %
  st.lastTickMs = kT0;
  // Budget illimité : soutenable = 24,7 mL/min, demande ≈ 6 mL/min → 24 %.
  PhIntegralVerdict v = phIntegralTick(st, inputs(kT0 + 10000u, 0.02f), tun);
  TEST_ASSERT_TRUE(phIntegralFillPct(st.integral, kPhIntegralMaxNormal) > 95.0f);
  TEST_ASSERT_FALSE(v.saturated);
  TEST_ASSERT_FALSE(v.emitWarn);
  TEST_ASSERT_FALSE(v.emitCritical);
}

// L'anomalie exige DEUX termes : saturée ET erreur encore au-dessus du seuil
// d'arrêt. Une demande saturée avec l'erreur déjà rentrée est un transitoire de
// fin de correction, pas un défaut.
void test_P_saturation_sans_erreur_residuelle_n_est_pas_une_anomalie(void) {
  const PhIntegralTuning tun = tuning();  // ki de production : l'intégrale porte la demande
  PhIntegralState st;
  st.integral = 8000.0f;                  // ki·I ≈ 2,96 mL/min > soutenable (2,06)
  st.lastTickMs = kT0;
  uint32_t t = kT0;
  bool anyAlert = false;
  for (int i = 0; i < 3600; ++i) {  // 10 h de ticks de 10 s
    t += 10000u;
    // Erreur SOUS le seuil d'arrêt : la correction a abouti.
    PhIntegralVerdict v =
        phIntegralTick(st, inputs(t, 0.005f, kAmpFlow, kSatBudget), tun);
    TEST_ASSERT_TRUE(v.saturated);        // la demande, elle, EST saturée
    TEST_ASSERT_EQUAL_UINT32(0u, v.anomalyHeldMs);  // …mais rien ne mûrit
    if (v.emitWarn || v.emitCritical) anyAlert = true;
  }
  TEST_ASSERT_FALSE(anyAlert);
  // Et pendant ces 10 h l'intégrale s'est DÉCHARGÉE malgré le gel (asymétrie).
  TEST_ASSERT_TRUE(st.integral < 8000.0f);
}

// Paliers 2 h → warning, 6 h → critical, puis RAPPEL toutes les 12 h.
void test_P_paliers_2h_6h_puis_rappel_toutes_les_12h(void) {
  TEST_ASSERT_EQUAL_UINT32(7200000UL, kPhIntegralSaturationWarnMs);
  TEST_ASSERT_EQUAL_UINT32(21600000UL, kPhIntegralSaturationCriticalMs);
  TEST_ASSERT_EQUAL_UINT32(43200000UL, kPhIntegralSaturationReminderMs);

  const PhIntegralTuning tun = tuning(/*ki*/0.0f);
  PhIntegralState st;
  st.lastTickMs = kT0;
  const float error = 0.50f;  // demande 4 mL/min ≫ 2,06 soutenables, erreur ≫ 0,01

  long warns = 0, crits = 0;
  uint32_t warnAt = 0, firstCritAt = 0, secondCritAt = 0, thirdCritAt = 0;
  uint32_t t = kT0;
  const long kTicks31h = 31L * 3600L / 10L;  // 11 160 ticks : couvre 2, 6, 18 et 30 h
  for (long i = 0; i < kTicks31h; ++i) {
    t += 10000u;
    PhIntegralVerdict v =
        phIntegralTick(st, inputs(t, error, kAmpFlow, kSatBudget), tun);
    if (v.emitWarn) {
      warns++;
      warnAt = v.anomalyHeldMs;
    }
    if (v.emitCritical) {
      crits++;
      if (crits == 1) firstCritAt = v.anomalyHeldMs;
      if (crits == 2) secondCritAt = v.anomalyHeldMs;
      if (crits == 3) thirdCritAt = v.anomalyHeldMs;
    }
  }
  // Warning UNE seule fois, au palier de 2 h.
  TEST_ASSERT_EQUAL_INT(1, warns);
  TEST_ASSERT_UINT32_WITHIN(10000u, kPhIntegralSaturationWarnMs, warnAt);
  // Critical à 6 h, puis rappels à 18 h et 30 h — un état anormal qui dure ne
  // doit PAS retomber dans le silence.
  TEST_ASSERT_EQUAL_INT(3, crits);
  TEST_ASSERT_UINT32_WITHIN(10000u, kPhIntegralSaturationCriticalMs, firstCritAt);
  TEST_ASSERT_UINT32_WITHIN(10000u,
                            kPhIntegralSaturationCriticalMs + kPhIntegralSaturationReminderMs,
                            secondCritAt);
  TEST_ASSERT_UINT32_WITHIN(10000u,
                            kPhIntegralSaturationCriticalMs + 2u * kPhIntegralSaturationReminderMs,
                            thirdCritAt);
}

// La tenue se compte en temps de BRANCHE AUTOMATIQUE : des ticks espacés au-delà
// du dt maximal (mode manuel, garde bloquante, filtration coupée entre deux
// passages) ne font PAS mûrir l'anomalie toute seule.
void test_P_la_tenue_de_l_anomalie_ne_court_pas_en_temps_mural(void) {
  const PhIntegralTuning tun = tuning(/*ki*/0.0f);
  PhIntegralState st;
  st.lastTickMs = kT0;
  uint32_t t = kT0;
  bool anyAlert = false;
  for (int i = 0; i < 1200; ++i) {  // 1 200 × 30 s = 10 h de temps MURAL
    t += 30000u;                    // dt > 10 s → non comptabilisé
    PhIntegralVerdict v =
        phIntegralTick(st, inputs(t, 0.50f, kAmpFlow, kSatBudget), tun);
    TEST_ASSERT_TRUE(v.saturated);
    if (v.emitWarn || v.emitCritical) anyAlert = true;
  }
  TEST_ASSERT_FALSE(anyAlert);
  TEST_ASSERT_EQUAL_UINT32(0u, st.anomalyHeldMs);
}

// Fin d'anomalie : l'épisode alerté est CLOS (la coquille lève l'alerte) et la
// tenue repart de zéro — une deuxième alerte devra remûrir 2 h.
void test_P_fin_d_anomalie_leve_l_alerte_et_remet_la_tenue_a_zero(void) {
  const PhIntegralTuning tun = tuning(/*ki*/0.0f);
  PhIntegralState st;
  st.lastTickMs = kT0;
  uint32_t t = kT0;
  for (int i = 0; i < 1080; ++i) {  // 3 h saturées → warning émis
    t += 10000u;
    phIntegralTick(st, inputs(t, 0.50f, kAmpFlow, kSatBudget), tun);
  }
  TEST_ASSERT_TRUE(st.warnSent);
  TEST_ASSERT_TRUE(st.anomalyHeldMs >= kPhIntegralSaturationWarnMs);

  // Retour à la normale : demande ramenée sous 85 % du soutenable.
  t += 10000u;
  PhIntegralVerdict v =
      phIntegralTick(st, inputs(t, errorForRatio(50.0f), kAmpFlow, kSatBudget), tun);
  TEST_ASSERT_FALSE(v.saturated);
  TEST_ASSERT_TRUE(v.emitClear);
  TEST_ASSERT_EQUAL_UINT32(0u, st.anomalyHeldMs);
  TEST_ASSERT_FALSE(st.warnSent);
  TEST_ASSERT_FALSE(st.critSent);

  // Et le clear n'est émis QU'UNE fois (pas de spam au tour suivant).
  t += 10000u;
  v = phIntegralTick(st, inputs(t, errorForRatio(50.0f), kAmpFlow, kSatBudget), tun);
  TEST_ASSERT_FALSE(v.emitClear);
}

// Un reset sec pendant un épisode alerté lève aussi l'alerte (l'accumulation
// qui la motivait n'existe plus).
void test_P_reset_sec_pendant_une_alerte_leve_l_alerte(void) {
  const PhIntegralTuning tun = tuning(/*ki*/0.0f);
  PhIntegralState st;
  st.lastTickMs = kT0;
  st.warnSent = true;
  st.critSent = true;
  st.integral = 9000.0f;
  PhIntegralVerdict v = phIntegralTick(st, inputs(kT0 + 10000u, -0.30f), tun);
  TEST_ASSERT_TRUE(v.event == PhIntegralEvent::kDryReset);
  TEST_ASSERT_TRUE(v.emitClear);
  TEST_ASSERT_FALSE(st.warnSent);
  TEST_ASSERT_FALSE(st.critSent);
}

// --- Remplissage exposé à /data et au WebSocket -------------------------------

void test_P_remplissage_en_pourcent_du_plafond(void) {
  TEST_ASSERT_FLOAT_WITHIN(0.1f, 50.0f,
                           phIntegralFillPct(0.5f * kPhIntegralMaxNormal,
                                             kPhIntegralMaxNormal));
  // Valeur ABSOLUE : une saturation négative est tout aussi anormale.
  TEST_ASSERT_FLOAT_WITHIN(0.1f, 50.0f,
                           phIntegralFillPct(-0.5f * kPhIntegralMaxNormal,
                                             kPhIntegralMaxNormal));
  TEST_ASSERT_FLOAT_WITHIN(0.1f, 100.0f,
                           phIntegralFillPct(kPhIntegralMaxNormal, kPhIntegralMaxNormal));
  // Plafond nul ou intégrale non finie → NaN (l'UI affiche « — »).
  TEST_ASSERT_TRUE(isnan(phIntegralFillPct(100.0f, 0.0f)));
  TEST_ASSERT_TRUE(isnan(phIntegralFillPct(NAN, kPhIntegralMaxNormal)));
}

// --- Contrats numériques (constantes) -----------------------------------------

void test_P_facteur_de_pas_divise_bien_par_deux_en_une_demi_vie(void) {
  const uint32_t stepsPerHalfLife = kPhIntegralDecayHalfLifeMs / kPhIntegralDecayStepMs;
  TEST_ASSERT_EQUAL_UINT32(60u, stepsPerHalfLife);
  float v = 1.0f;
  for (uint32_t i = 0; i < stepsPerHalfLife; ++i) v *= kPhIntegralDecayStepFactor;
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.5f, v);
  TEST_ASSERT_TRUE(kPhIntegralDecayStepFactor > 0.0f);
  TEST_ASSERT_TRUE(kPhIntegralDecayStepFactor < 1.0f);
}

void test_P_marge_de_reset_reste_au_dessus_du_bruit_du_filtre(void) {
  TEST_ASSERT_TRUE(kPhIntegralResetMargin >= 0.05f);
  TEST_ASSERT_TRUE(kPhIntegralResetMargin < kPhSafetyMargin);
  const float effectif = kStartThreshold + kPhIntegralResetMargin;
  TEST_ASSERT_TRUE(effectif >= 10.0f * kStopThreshold);
}

void test_P_ti_domine_la_pause_melange_et_reste_ordonne(void) {
  TEST_ASSERT_TRUE(kPhIntegralTiFastS >= 4.0f * (kPhMixingDelayMs / 1000.0f));
  TEST_ASSERT_TRUE(kPhIntegralTiFastS < kPhIntegralTiNormalS);
  TEST_ASSERT_TRUE(kPhIntegralTiNormalS < kPhIntegralTiSlowS);
  TEST_ASSERT_TRUE(kPhKpSlow < kPhKpNormal && kPhKpNormal < kPhKpFast);
  TEST_ASSERT_TRUE(kPhIntegralDecayHalfLifeMs * 4.0f <= kPhIntegralTiFastS * 1000.0f);
  TEST_ASSERT_TRUE(kPhIntegralDecayHalfLifeMs >= kPhMixingDelayMs);
}

void test_P_valeurs_de_production_des_constantes_de_temps(void) {
  TEST_ASSERT_EQUAL_UINT32(86400000UL, kPhIntegralStaleResetMs);
  TEST_ASSERT_EQUAL_UINT32(3600000UL, kPhIntegralDecayHalfLifeMs);
  TEST_ASSERT_EQUAL_UINT32(60000UL, kPhIntegralDecayStepMs);
  TEST_ASSERT_EQUAL_UINT32(1000UL, kPhIntegralChargeQuantumMs);
  TEST_ASSERT_EQUAL_UINT32(10000UL, kPhIntegralMaxTickDtMs);
  // Le quantum doit rester représentable DANS un dt acceptable.
  TEST_ASSERT_TRUE(kPhIntegralChargeQuantumMs <= kPhIntegralMaxTickDtMs);
}

// =============================================================================
// E* — L'ÉQUILIBRE : la boucle fermée de PRODUCTION sous charge permanente
// =============================================================================
// LE test du cycle. Il ne suffit pas que l'intégrale se décharge « en principe » :
// il faut que le point de fonctionnement du système COMPLET (module pur +
// computePidPure + hystérésis de dosage) soit celui qu'on a visé, et qu'il ne
// coûte pas un millilitre de plus que le régime P pur.

void test_E_l_erreur_converge_vers_le_seuil_d_arret(void) {
  ProdStats s = simulateProduction(kPhKpNormal, kPhKiNormal, kPhIntegralMaxNormal,
                                   kBudgetNominalMinPerHour, /*days*/12,
                                   DeadbandPolicy::kAlignedOnHysteresis);
  // Le régime établi se parque au seuil d'ARRÊT : correcteur et ordonnanceur
  // visent le MÊME point (c'est la condition de validité de l'intégration signée).
  TEST_ASSERT_FLOAT_WITHIN(0.004f, kStopThreshold, s.meanErrLastDay);
  TEST_ASSERT_FLOAT_WITHIN(0.004f, kStopThreshold, s.finalError);
  // Bande de fonctionnement étroite sur les dernières 24 h : pas de cycle limite
  // entre le seuil d'arrêt et le seuil de démarrage.
  TEST_ASSERT_TRUE(s.maxErrLastDay - s.minErrLastDay < 0.01f);
  TEST_ASSERT_TRUE(s.maxErrLastDay < kStartThreshold);
  // …et l'erreur reste POSITIVE : on ne descend jamais sous la consigne (aucun
  // risque d'excursion acide provoquée par le terme intégral).
  TEST_ASSERT_TRUE(s.minErrLastDay > 0.0f);
}

void test_E_l_integrale_se_stabilise_sans_saturer(void) {
  ProdStats s = simulateProduction(kPhKpNormal, kPhKiNormal, kPhIntegralMaxNormal,
                                   kBudgetNominalMinPerHour, 12,
                                   DeadbandPolicy::kAlignedOnHysteresis);
  // Valeur d'équilibre analytique : ki·I = charge − kp·stopThreshold.
  const float attendu = (kLoadMlPerMin - kPhKpNormal * kStopThreshold) / kPhKiNormal;
  TEST_ASSERT_FLOAT_WITHIN(0.05f * attendu, attendu, s.finalIntegral);
  // ≈ 46 % du plafond : la moitié de l'autorité reste disponible pour la dérive
  // saisonnière — c'est ce que « ne pas saturer » veut dire concrètement.
  TEST_ASSERT_FLOAT_WITHIN(5.0f, 46.0f, s.finalFillPct);
  TEST_ASSERT_TRUE(s.finalIntegral < 0.60f * kPhIntegralMaxNormal);
  // Et la surveillance C5 ne crie PAS sur un système qui va bien.
  TEST_ASSERT_EQUAL_INT(0, s.criticalCount);
  TEST_ASSERT_EQUAL_INT(0, s.warnCount);
}

// GARDE-FOU CHIMIQUE : à l'équilibre, le volume injecté vaut la charge du bassin,
// quelle que soit la structure du correcteur (bilan de matière). Corriger la
// droop ne doit RIEN coûter en produit.
void test_E_volume_total_equivalent_au_regime_p_pur(void) {
  ProdStats p  = simulateProduction(kPhKpNormal, 0.0f, kPhIntegralMaxNormal,
                                    kBudgetNominalMinPerHour, 12,
                                    DeadbandPolicy::kAlignedOnHysteresis);
  ProdStats pi = simulateProduction(kPhKpNormal, kPhKiNormal, kPhIntegralMaxNormal,
                                    kBudgetNominalMinPerHour, 12,
                                    DeadbandPolicy::kAlignedOnHysteresis);
  // Volume théorique : charge × durée = 2,86 mL/min × 12 j × 1 440 min.
  const float theorique = kLoadMlPerMin * 12.0f * 1440.0f;
  TEST_ASSERT_FLOAT_WITHIN(0.05f * theorique, theorique, pi.totalMl);
  // Écart PI / P pur inférieur à 2 % : on place la même quantité, au bon endroit.
  TEST_ASSERT_TRUE(fabsf(pi.totalMl - p.totalMl) < 0.02f * p.totalMl);
}

// LE DÉFAUT CORRIGÉ, exhibé côte à côte : avec la bande morte figée au seuil de
// DÉMARRAGE, la sortie retombe à zéro dès 0,05 alors que l'hystérésis poursuit
// jusqu'à 0,01 — le régime se parque à 0,05 (5× le point visé), l'intégrande
// (erreur − 0,01) reste POSITIF en permanence, et l'intégrale monte jusqu'à ce que
// la demande atteigne le débit SOUTENABLE : gel permanent, épisode de saturation
// permanent, et `critical` répété toutes les 12 h sur un bassin qui va bien.
// (C'est le gel corrigé de C3 qui l'empêche d'atteindre le plafond : sans lui elle
// s'y collerait.) Le point fixe visé n'est tout simplement pas atteignable.
// ⚠️ Portée : la politique de bande morte est une ligne de pump_controller.cpp
// (coquille) ; ce test discrimine les deux politiques, il ne détecte pas une
// mutation de la ligne réelle.
void test_E_bande_morte_figee_au_seuil_de_demarrage_sature_la_regulation(void) {
  ProdStats bad = simulateProduction(kPhKpNormal, kPhKiNormal, kPhIntegralMaxNormal,
                                     kBudgetNominalMinPerHour, 12,
                                     DeadbandPolicy::kFrozenOnStart);
  ProdStats good = simulateProduction(kPhKpNormal, kPhKiNormal, kPhIntegralMaxNormal,
                                      kBudgetNominalMinPerHour, 12,
                                      DeadbandPolicy::kAlignedOnHysteresis);
  // Le régime se parque au seuil de DÉMARRAGE, 5× au-dessus du point visé.
  TEST_ASSERT_FLOAT_WITHIN(0.005f, kStartThreshold, bad.meanErrLastDay);
  TEST_ASSERT_TRUE(bad.meanErrLastDay > 3.0f * good.meanErrLastDay);
  // La demande vient buter sur le débit soutenable : gel et saturation PERMANENTS…
  TEST_ASSERT_TRUE(bad.everFrozen);
  TEST_ASSERT_TRUE(bad.everSaturated);
  // …d'où une FAUSSE alerte critique, répétée, sur un bassin nominal.
  TEST_ASSERT_TRUE(bad.criticalCount >= 1);
  TEST_ASSERT_TRUE(bad.finalFillPct > 55.0f);

  // La politique alignée, elle, ne gèle pas, ne sature pas et n'alerte pas.
  TEST_ASSERT_FALSE(good.everFrozen);
  TEST_ASSERT_FALSE(good.everSaturated);
  TEST_ASSERT_EQUAL_INT(0, good.criticalCount);
  TEST_ASSERT_TRUE(good.finalFillPct < 50.0f);
}

// CARACTÉRISATION (défaut de CONFIGURATION, pas de code — cf. rapport de test) :
// le budget horaire par défaut (5 min/h) plafonne le débit moyen à 2,06 mL/min,
// SOUS la charge basique mesurée (2,86 mL/min). Le gel d'anti-windup est alors
// permanent, l'intégrale ne peut plus aider, et le système retombe sur la droop
// du P pur — ce que C5 signale, à juste titre, en `critical`.
void test_E_budget_horaire_sous_la_charge_gele_et_alerte(void) {
  TEST_ASSERT_TRUE(phSustainableAvgFlow(kAmpFlow, 5) < kLoadMlPerMin);

  ProdStats s = simulateProduction(kPhKpNormal, kPhKiNormal, kPhIntegralMaxNormal,
                                   /*budget*/5, 12,
                                   DeadbandPolicy::kAlignedOnHysteresis);
  TEST_ASSERT_TRUE(s.everFrozen);
  TEST_ASSERT_TRUE(s.everSaturated);
  // L'alerte est LEVÉE (et répétée) : l'installation ne peut pas tenir la charge.
  TEST_ASSERT_TRUE(s.criticalCount >= 1);
  // Et l'erreur reste sur la droop du P pur : l'intégrale ne peut rien y faire.
  TEST_ASSERT_TRUE(s.meanErrLastDay > 0.20f);
}

// =============================================================================
// Runner
// =============================================================================
void setUp(void) {}
void tearDown(void) {}

int main(int argc, char** argv) {
  (void)argc;
  (void)argv;
  UNITY_BEGIN();

  // D1 — la droop (justification du cycle)
  RUN_TEST(test_D1_p_pur_ne_rejoint_jamais_la_consigne);
  RUN_TEST(test_D1_pi_rejoint_la_consigne);
  RUN_TEST(test_D1_pi_bat_le_p_pur_d_un_ordre_de_grandeur);
  RUN_TEST(test_D1_droop_avec_bande_morte_de_production);
  RUN_TEST(test_D1_pi_ne_consomme_pas_plus_de_produit_que_le_p_pur);
  // D2 — le piège integralMax = 50
  RUN_TEST(test_D2_plafond_legacy_50_laisse_la_droop_intacte);
  RUN_TEST(test_D2_plafond_couvre_la_charge_d_equilibre_aux_trois_vitesses);
  // I1 — charge au plafond par vitesse
  RUN_TEST(test_I1_charge_jusqu_au_plafond_vitesse_slow);
  RUN_TEST(test_I1_charge_jusqu_au_plafond_vitesse_normal);
  RUN_TEST(test_I1_charge_jusqu_au_plafond_vitesse_fast);
  RUN_TEST(test_I1_le_plafond_varie_avec_la_vitesse_et_reste_ordonne);
  // I2 — verrou d'autorité au run
  RUN_TEST(test_I2_autorite_integrale_est_de_6_ml_par_min_aux_trois_vitesses);
  // A1 — anti-windup de computePidPure
  RUN_TEST(test_A1_saturation_prolongee_ne_fait_pas_exploser_l_integrale);
  RUN_TEST(test_A1_bornage_symetrique_avec_les_constantes_de_production);
  RUN_TEST(test_A1_gel_suspend_totalement_l_accumulation);
  RUN_TEST(test_A1_fenetre_de_windup_de_l_ancienne_regle_P_seul);
  // P* — le MODULE PUR, appelé directement
  RUN_TEST(test_P_la_decharge_existe_sans_excursion_sous_la_consigne);
  RUN_TEST(test_P_premier_tick_arme_la_base_de_temps_sans_integrer);
  RUN_TEST(test_P_charge_par_quanta_d_une_seconde_avec_report_du_reliquat);
  RUN_TEST(test_P_un_increment_de_5ms_serait_perdu_dans_le_float);
  RUN_TEST(test_P_un_dt_superieur_a_10s_est_rejete_jamais_credite);
  RUN_TEST(test_P_l_integrande_est_l_erreur_moins_le_seuil_d_arret);
  RUN_TEST(test_P_bornage_symetrique_du_module);
  RUN_TEST(test_P_debit_soutenable_tient_compte_du_budget_horaire);
  RUN_TEST(test_P_gel_sur_le_debit_soutenable_pas_sur_l_amplitude);
  RUN_TEST(test_P_amplitude_invalide_gele_en_fail_closed);
  RUN_TEST(test_P_le_gel_bloque_la_charge_jamais_la_decharge);
  RUN_TEST(test_P_fraction_de_gel_sur_quota_journalier);
  RUN_TEST(test_P_erreur_moins_0011_n_efface_pas_l_integrale);
  RUN_TEST(test_P_reset_sec_uniquement_au_dela_de_moins_015);
  RUN_TEST(test_P_oubli_demi_vie_1h);
  RUN_TEST(test_P_oubli_rattrape_par_pas_entiers_apres_une_absence);
  RUN_TEST(test_P_oubli_reliquat_reporte_aucun_temps_perdu);
  RUN_TEST(test_P_oubli_plancher_1_phs_ferme_l_episode);
  RUN_TEST(test_P_oubli_desarme_des_que_l_erreur_redevient_positive);
  RUN_TEST(test_P_integrale_pleine_ne_survit_pas_a_24h_sous_consigne);
  RUN_TEST(test_P_rattrapage_borne_a_600_pas);
  RUN_TEST(test_P_peremption_24h_purge_l_integrale);
  RUN_TEST(test_P_peremption_ne_touche_pas_un_tick_jamais_evalue);
  RUN_TEST(test_P_peremption_d_une_integrale_deja_nulle_ne_signale_rien);
  RUN_TEST(test_P_peremption_wrap_safe_du_compteur_millis);
  RUN_TEST(test_P_reset_remet_tout_l_etat_a_neuf);
  RUN_TEST(test_P_mesure_non_finie_gele_et_ne_credite_aucun_temps);
  RUN_TEST(test_P_hysterese_de_saturation_95_puis_85);
  RUN_TEST(test_P_le_seuil_porte_sur_la_demande_pas_sur_le_remplissage);
  RUN_TEST(test_P_saturation_sans_erreur_residuelle_n_est_pas_une_anomalie);
  RUN_TEST(test_P_paliers_2h_6h_puis_rappel_toutes_les_12h);
  RUN_TEST(test_P_la_tenue_de_l_anomalie_ne_court_pas_en_temps_mural);
  RUN_TEST(test_P_fin_d_anomalie_leve_l_alerte_et_remet_la_tenue_a_zero);
  RUN_TEST(test_P_reset_sec_pendant_une_alerte_leve_l_alerte);
  RUN_TEST(test_P_remplissage_en_pourcent_du_plafond);
  RUN_TEST(test_P_facteur_de_pas_divise_bien_par_deux_en_une_demi_vie);
  RUN_TEST(test_P_marge_de_reset_reste_au_dessus_du_bruit_du_filtre);
  RUN_TEST(test_P_ti_domine_la_pause_melange_et_reste_ordonne);
  RUN_TEST(test_P_valeurs_de_production_des_constantes_de_temps);
  // E* — l'équilibre en boucle fermée de production
  RUN_TEST(test_E_l_erreur_converge_vers_le_seuil_d_arret);
  RUN_TEST(test_E_l_integrale_se_stabilise_sans_saturer);
  RUN_TEST(test_E_volume_total_equivalent_au_regime_p_pur);
  RUN_TEST(test_E_bande_morte_figee_au_seuil_de_demarrage_sature_la_regulation);
  RUN_TEST(test_E_budget_horaire_sous_la_charge_gele_et_alerte);

  return UNITY_END();
}
