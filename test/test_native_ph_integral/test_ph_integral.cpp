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
//
// feature-084 (v2.27.0) — CE QUE CE FICHIER COUVRE, ET CE QU'IL NE COUVRE PAS
// ---------------------------------------------------------------------------
// Trois sujets sont livrés ; leur testabilité native est TRÈS inégale, et il faut
// le savoir avant de lire le vert de cette suite comme une garantie.
//
//   SUJET 2 — RESET SEC TEMPORISÉ : ENTIÈREMENT COUVERT. Le mécanisme vit dans
//   le module pur (`st.resetHoldMs`, étape 2 de phIntegralTick). Tenue, remise à
//   zéro sur remontée, base de temps de branche, et surtout la PROTECTION
//   GRADUÉE pendant l'attente (décharge signée + oubli exponentiel) — c'est
//   l'argument de sûreté du gate, il est vérifié ici (bloc « B »).
//
//   SUJET 1 — CADENCE : COUVERT SEULEMENT PAR SES EFFETS BORNÉS. Le prédicat
//   `PumpControllerClass::phIntegralAccumulationAllowed()` est une méthode PRIVÉE
//   de la coquille, qui interroge `sensors`, `mqttCfg`, `esp_task_wdt_status()` et
//   `_phGuardBlockingNow` : RIEN de tout cela ne compile en natif, et il n'existe
//   aucune extraction pure. Autrement dit :
//     • « n'accumule pas en `manual` / sous garde bloquante / filtre non prêt /
//       calibration < 2 points / stabilisation » N'EST PAS TESTÉ ICI, et ne peut
//       pas l'être sans extraire le prédicat dans un header pur (patron ADR-0017,
//       exactement ce qui a été fait pour ph_integral_logic.h). Une mutation qui
//       rendrait le prédicat permissif en `manual` NE TUERAIT AUCUN test.
//     • ce qui EST testable, et qui est testé (bloc « D ») : les BORNES chimiques
//       que le gate invoque pour justifier d'accumuler dans les états rythmiques —
//       sur-crédit d'une pause mélange, et plafonnement par le gel d'anti-windup
//       quand la limite horaire est épuisée.
//
//   SUJET 3 — PERSISTANCE NVS : LE MOINS COUVERT, ALORS QUE C'EST LE PLUS RISQUÉ.
//   Le blob et son empreinte (`PhIntegralSnapshot`, `phIntegralSnapshotCrc`) sont
//   dans src/config.{h,cpp}, qui tirent <Arduino.h>, freertos et <Preferences.h> :
//   config.cpp n'est PAS dans le `build_src_filter` de l'env native et ne peut pas
//   y entrer. La séquence de décision R1-R7 (clamp, CRC, schéma, contexte,
//   péremption, confirmation différée, anti-boucle) est écrite en ligne dans
//   PumpControllerClass::loadPhIntegralSnapshotDeferred() /
//   tickPhIntegralPersistence(), sans extraction pure. Conséquence nette :
//     • NE SONT PAS TESTABLES ICI : le calcul du CRC et son rejet, le rejet sur
//       schéma / contexte (vitesse, produit, consigne, plafond), le rejet à 7 h,
//       la confirmation de 5 min et son abandon si l'erreur retombe, le
//       désarmement après 3 restaurations, la cadence d'écriture. Supprimer la
//       vérification de CRC ou le clamp R1 NE TUE AUCUN test de cette suite.
//     • CE QUI EST TESTÉ (bloc « R ») : le CONTRAT NUMÉRIQUE des sept conditions
//       (valeurs et encadrements mutuels), et surtout la PROPRIÉTÉ CHIMIQUE que
//       R1 est censé garantir — une restauration au plafond de 30 % ne peut pas
//       faire passer le bassin sous la consigne, alors qu'une restauration au
//       plafond PLEIN le fait. Cela verrouille la FRACTION, pas le CLAMP.
//   Le correctif recommandé (hors périmètre du test-engineer, qui ne touche pas
//   src/) est l'extraction d'un `ph_persist_logic.h` pur rendant un verdict
//   « accepter / rejeter + motif » à partir d'un instantané et d'un contexte.
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
  // feature-084 : minimum sur TOUTE la simulation, transitoire initial compris.
  // Les statistiques « dernier jour » décrivent le régime établi ; une excursion
  // ACIDE provoquée par un état initial (une restauration NVS, précisément) est un
  // TRANSITOIRE — la moyenne du dernier jour ne la voit jamais.
  float minErrEver;
  // feature-084 : demande MAXIMALE observée pendant la PREMIÈRE HEURE, c'est-à-dire
  // pendant le transitoire de restauration NVS. C'est la grandeur que R1 borne :
  // l'« autorisation de dosage pré-chargée » que l'ADR-0052 refusait.
  float firstHourMaxFlow;
  float finalIntegral;
  float finalFillPct;
  float totalMl;
  bool  everFrozen;
  bool  everSaturated;
  long  warnCount;
  long  criticalCount;
};

// `initialIntegral` / `initialError` : état de DÉPART de la boucle. Défauts nuls
// = comportement historique (sortie de boot, bassin à la consigne). Non nuls, ils
// modélisent le scénario de restauration NVS (R4 n'applique la valeur relue
// qu'après 5 min d'erreur au-dessus du seuil de démarrage : l'erreur de départ
// est donc, par construction, franchement positive).
static ProdStats simulateProduction(float kp, float ki, float integralMax,
                                    int budgetMinPerHour, int days,
                                    DeadbandPolicy policy,
                                    float initialIntegral = 0.0f,
                                    float initialError = 0.0f) {
  const long steps = (long)days * kTicksPerDay;
  const long lastDayFrom = steps - kTicksPerDay;

  PhIntegralState st;
  PhIntegralTuning tun;
  tun.kp = kp;
  tun.ki = ki;
  tun.integralMax = integralMax;
  tun.startThreshold = kStartThreshold;
  tun.stopThreshold = kStopThreshold;

  st.integral = initialIntegral;
  float error = initialError;
  float lastError = 0.0f;
  bool active = false;
  uint32_t now = kT0;

  ProdStats s;
  s.maxErrLastDay = -1e9f;
  s.minErrLastDay = 1e9f;
  s.minErrEver = error;
  s.firstHourMaxFlow = 0.0f;
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
    if (i < (long)(3600.0f / kPidDtSec) && flow > s.firstHourMaxFlow) {
      s.firstHourMaxFlow = flow;
    }

    error += kPlantPhPerFlowPerSec * (kLoadMlPerMin - flow) * kPidDtSec;
    now += (uint32_t)(kPidDtSec * 1000.0f);

    if (error < s.minErrEver) s.minErrEver = error;
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
                               bool quota = false, bool cycles = false) {
  PhIntegralInputs in;
  in.nowMs = nowMs;
  in.error = error;
  in.ampFlow = ampFlow;
  in.budgetMinPerHour = budget;
  in.quotaNearLimit = quota;
  in.cycleQuotaExhausted = cycles;
  return in;
}

// -----------------------------------------------------------------------------
// feature-084 — TENIR une condition pendant N ms de temps de BRANCHE
// -----------------------------------------------------------------------------
// Le reset sec n'est plus instantané : un scénario d'excursion ne se décrit plus
// par « un tick sous le seuil » mais par « le seuil TENU pendant une durée ». Ce
// helper avance le curseur de temps `t` par pas de `pasMs` (10 s par défaut,
// c'est-à-dire la frontière haute du dt acceptable) et rend le verdict du DERNIER
// tick ; `purges` compte les purges sèches rencontrées en chemin.
//
// `dureeMs` est du temps de BRANCHE si `pasMs <= kPhIntegralMaxTickDtMs` ; au-delà
// c'est du temps MURAL non crédité, ce qui sert justement à tester la distinction.
static PhIntegralVerdict tenirPendant(PhIntegralState& st,
                                      const PhIntegralTuning& tun, float error,
                                      uint32_t dureeMs, uint32_t& t,
                                      int* purges = nullptr,
                                      uint32_t pasMs = kPhIntegralMaxTickDtMs,
                                      float ampFlow = kAmpFlow, int budget = 0) {
  PhIntegralVerdict v;
  const uint32_t fin = t + dureeMs;
  if (purges != nullptr) *purges = 0;
  while (t < fin) {
    t += pasMs;
    v = phIntegralTick(st, inputs(t, error, ampFlow, budget), tun);
    if (purges != nullptr && v.event == PhIntegralEvent::kDryReset) (*purges)++;
  }
  return v;
}

// Excursion FRANCHE de référence : au-delà du seuil de purge (−0,15 avec les
// défauts), donc éligible au reset sec — dérivée, jamais recodée en dur.
static const float kSeuilPurge = kStartThreshold + kPhIntegralResetMargin;
static const float kExcursionFranche = -(kSeuilPurge + 0.01f);  // −0,16

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

// ⭐ QUOTA DE CYCLES ÉPUISÉ (gate pool-chemistry du 30/08/2026) — l'anti-cycling
// ne se relève qu'au basculement de 24 h, exactement comme le quota volumique :
// il appartient donc au GEL, et non aux suspensions rythmiques. Épuisé à 10 h du
// matin, il laissait l'intégrale charger neuf heures sur une erreur que
// l'actionneur n'avait plus le droit de résorber.
void test_P_quota_de_cycles_epuise_gele_la_charge_pas_la_decharge(void) {
  const PhIntegralTuning tun = tuning();

  // (a) charge INTERDITE tant que les cycles sont épuisés — 9 h d'erreur franche
  //     n'ajoutent rien. Sans le gel, (0,50 − 0,01) × 32 400 s ≈ 15 876 pH·s
  //     seraient accumulés, soit le plafond d'anti-windup plein.
  PhIntegralState up;
  up.integral = 2000.0f;
  up.lastTickMs = kT0;
  for (int i = 0; i < 3240; ++i) {  // 9 h par pas de 10 s
    PhIntegralVerdict v = phIntegralTick(
        up, inputs(kT0 + (uint32_t)(i + 1) * 10000u, 0.50f, kAmpFlow, 0,
                   /*quota*/false, /*cycles*/true), tun);
    TEST_ASSERT_TRUE(v.frozen);
  }
  TEST_ASSERT_FLOAT_WITHIN(kEps, 2000.0f, up.integral);

  // (b) la DÉCHARGE reste ouverte sous le même gel : c'est la différence entre
  //     geler et fermer le prédicat d'accumulation. Le prédicat figerait
  //     l'intégrale à sa valeur haute ; le gel la laisse redescendre.
  PhIntegralState down;
  down.integral = 2000.0f;
  down.lastTickMs = kT0;
  for (int i = 0; i < 360; ++i) {  // 1 h
    PhIntegralVerdict v = phIntegralTick(
        down, inputs(kT0 + (uint32_t)(i + 1) * 10000u, 0.0f, kAmpFlow, 0,
                     /*quota*/false, /*cycles*/true), tun);
    TEST_ASSERT_TRUE(v.frozen);
  }
  TEST_ASSERT_FLOAT_WITHIN(0.05f, 2000.0f - 36.0f, down.integral);

  // (c) drapeau retombé (nouveau jour) : la même situation accumule à nouveau —
  //     c'est bien LUI qui gelait, et le gel n'a rien latché.
  PhIntegralState libre;
  libre.lastTickMs = kT0;
  PhIntegralVerdict v = phIntegralTick(
      libre, inputs(kT0 + 10000u, 0.50f, kAmpFlow, 0, false, /*cycles*/false), tun);
  TEST_ASSERT_FALSE(v.frozen);
  TEST_ASSERT_TRUE(libre.integral > 0.0f);
}

// Les trois motifs de gel sont INDÉPENDANTS et disjonctifs : chacun suffit seul.
void test_P_les_trois_motifs_de_gel_sont_disjonctifs(void) {
  const PhIntegralTuning tun = tuning();
  const uint32_t t = kT0 + 10000u;

  PhIntegralState a; a.lastTickMs = kT0;
  TEST_ASSERT_TRUE(phIntegralTick(a, inputs(t, 0.5f, kAmpFlow, /*budget*/5), tun)
                       .frozen);  // saturation de sortie seule
  PhIntegralState b; b.lastTickMs = kT0;
  TEST_ASSERT_TRUE(phIntegralTick(b, inputs(t, 0.5f, kAmpFlow, 0, true, false), tun)
                       .frozen);  // quota volumique seul
  PhIntegralState c; c.lastTickMs = kT0;
  TEST_ASSERT_TRUE(phIntegralTick(c, inputs(t, 0.5f, kAmpFlow, 0, false, true), tun)
                       .frozen);  // quota de cycles seul
  PhIntegralState d; d.lastTickMs = kT0;
  TEST_ASSERT_FALSE(phIntegralTick(d, inputs(t, 0.5f, kAmpFlow, 0, false, false), tun)
                        .frozen);  // aucun des trois
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

// RÉÉCRIT (feature-084) — le SEUIL est inchangé, sa satisfaction ne suffit plus.
// L'ancienne version concluait sur UN tick ; elle figeait donc la purge
// INSTANTANÉE, qui n'est plus le comportement. On tient désormais chaque
// excursion sur toute la fenêtre de tenue : seule celle qui franchit VRAIMENT le
// seuil, et le tient, purge.
void test_P_reset_sec_uniquement_au_dela_de_moins_015(void) {
  const PhIntegralTuning tun = tuning();
  int purges = 0;

  // Juste EN DEÇÀ (−0,14), tenu DEUX fois la fenêtre : jamais de purge, et la
  // tenue ne s'arme même pas.
  PhIntegralState a;
  a.integral = 12000.0f;
  a.lastTickMs = kT0;
  uint32_t ta = kT0;
  tenirPendant(a, tun, -(kSeuilPurge - 0.01f), 2u * kPhIntegralResetHoldMs, ta, &purges);
  TEST_ASSERT_EQUAL_INT(0, purges);
  TEST_ASSERT_EQUAL_UINT32(0u, a.resetHoldMs);
  TEST_ASSERT_TRUE(a.integral > 0.0f);  // l'oubli travaille, il n'efface pas tout

  // Frontière stricte : −0,15 PILE n'est pas < −0,15 → pas de purge, même tenu.
  PhIntegralState b;
  b.integral = 12000.0f;
  b.lastTickMs = kT0;
  uint32_t tb = kT0;
  tenirPendant(b, tun, -kSeuilPurge, 2u * kPhIntegralResetHoldMs, tb, &purges);
  TEST_ASSERT_EQUAL_INT(0, purges);
  TEST_ASSERT_EQUAL_UINT32(0u, b.resetHoldMs);

  // AU-DELÀ (−0,16) : la tenue s'arme, mais RIEN n'est purgé avant son terme.
  PhIntegralState c;
  c.integral = 12000.0f;
  c.lastTickMs = kT0;
  uint32_t tc = kT0;
  tenirPendant(c, tun, kExcursionFranche,
               kPhIntegralResetHoldMs - kPhIntegralMaxTickDtMs, tc, &purges);
  TEST_ASSERT_EQUAL_INT(0, purges);
  TEST_ASSERT_EQUAL_UINT32(kPhIntegralResetHoldMs - kPhIntegralMaxTickDtMs, c.resetHoldMs);
  const float avantPurge = c.integral;
  TEST_ASSERT_TRUE(avantPurge > 0.0f);

  // …et au terme exact de la tenue, la purge tombe, complète.
  tc += kPhIntegralMaxTickDtMs;
  PhIntegralVerdict v = phIntegralTick(c, inputs(tc, kExcursionFranche), tun);
  TEST_ASSERT_TRUE(v.event == PhIntegralEvent::kDryReset);
  TEST_ASSERT_FLOAT_WITHIN(kEps, avantPurge, v.integralBefore);
  TEST_ASSERT_FLOAT_WITHIN(kEps, 0.0f, c.integral);
  TEST_ASSERT_EQUAL_UINT32(0u, c.decayMs);
  TEST_ASSERT_EQUAL_UINT32(0u, c.resetHoldMs);  // la tenue repart de zéro aussi
  // La base de temps est RÉARMÉE au tick : pas de péremption fantôme derrière.
  TEST_ASSERT_EQUAL_UINT32(tc, c.lastTickMs);
}

// =============================================================================
// B — feature-084, sujet 2 : LA TEMPORISATION DU RESET SEC
// =============================================================================
// Ce que le gate a acheté avec ce différé : un transitoire de −0,15 (une pluie
// suffit) n'efface plus deux jours et demi d'apprentissage. Ce qu'il a promis en
// échange : « protection graduée, pas suspendue ». Les deux moitiés du marché
// sont vérifiées ici — la seconde par le test `..._la_protection_tourne_toujours`,
// sans lequel l'argument de sûreté du gate ne repose sur rien.

// LA FRONTIÈRE. Non déclenché à 14 min, déclenché à 15 — dérivé de la constante,
// jamais recodé (seul le test de contrat ci-dessous fige la valeur elle-même).
void test_P_reset_sec_non_declenche_a_14_min_declenche_a_15(void) {
  const PhIntegralTuning tun = tuning();
  const uint32_t uneMinute = 60000UL;
  const uint32_t presqueLaTenue = kPhIntegralResetHoldMs - uneMinute;  // 14 min

  PhIntegralState st;
  st.integral = 12000.0f;
  st.lastTickMs = kT0;
  uint32_t t = kT0;
  int purges = 0;

  tenirPendant(st, tun, kExcursionFranche, presqueLaTenue, t, &purges);
  TEST_ASSERT_EQUAL_INT(0, purges);                       // 14 min : rien
  TEST_ASSERT_EQUAL_UINT32(presqueLaTenue, st.resetHoldMs);
  TEST_ASSERT_TRUE(st.integral > 0.0f);

  tenirPendant(st, tun, kExcursionFranche, uneMinute, t, &purges);
  TEST_ASSERT_EQUAL_INT(1, purges);                       // 15 min : purge, UNE fois
  TEST_ASSERT_FLOAT_WITHIN(kEps, 0.0f, st.integral);
}

// LA REMISE À ZÉRO. Une seule remontée au-dessus du seuil et la tenue repart de
// zéro : deux excursions de 14 min séparées par un souffle ne purgent PAS. C'est
// exactement la propriété qui distingue « excursion réelle » de « à-coup ».
void test_P_la_tenue_du_reset_repart_a_zero_a_la_moindre_remontee(void) {
  const PhIntegralTuning tun = tuning();
  const uint32_t uneMinute = 60000UL;
  const uint32_t presqueLaTenue = kPhIntegralResetHoldMs - uneMinute;

  PhIntegralState st;
  st.integral = 12000.0f;
  st.lastTickMs = kT0;
  uint32_t t = kT0;
  int purges = 0;

  tenirPendant(st, tun, kExcursionFranche, presqueLaTenue, t, &purges);
  TEST_ASSERT_EQUAL_UINT32(presqueLaTenue, st.resetHoldMs);

  // UN SEUL tick au-dessus du seuil (−0,14 : toujours sous la consigne, mais pas
  // une excursion franche) suffit à annuler la tenue.
  t += kPhIntegralMaxTickDtMs;
  phIntegralTick(st, inputs(t, -(kSeuilPurge - 0.01f)), tun);
  TEST_ASSERT_EQUAL_UINT32(0u, st.resetHoldMs);

  // Deuxième excursion de 14 min : toujours pas de purge (la tenue a bien
  // recommencé à zéro et non repris là où elle s'était arrêtée).
  tenirPendant(st, tun, kExcursionFranche, presqueLaTenue, t, &purges);
  TEST_ASSERT_EQUAL_INT(0, purges);
  TEST_ASSERT_EQUAL_UINT32(presqueLaTenue, st.resetHoldMs);
  TEST_ASSERT_TRUE(st.integral > 0.0f);

  // La minute manquante, elle, déclenche.
  tenirPendant(st, tun, kExcursionFranche, uneMinute, t, &purges);
  TEST_ASSERT_EQUAL_INT(1, purges);
}

// L'ARGUMENT DE SÛRETÉ DU GATE, vérifié : pendant l'attente, la protection n'est
// pas suspendue mais GRADUÉE. Les DEUX voies de décharge tournent — l'oubli
// exponentiel ET la décharge signée. La démonstration est faite par ENCADREMENT :
// l'intégrale observée doit tomber sous CHACUNE des deux références « un seul
// mécanisme actif ». Si l'une des deux voies était coupée, l'encadrement casse.
void test_P_pendant_l_attente_du_reset_la_protection_tourne_toujours(void) {
  const PhIntegralTuning tun = tuning();
  const float charge = 12000.0f;
  const uint32_t attente = kPhIntegralResetHoldMs - 60000UL;  // 14 min, avant la purge

  // Référence 1 — BORNE HAUTE de l'« oubli seul » : le nombre de pas d'oubli
  // applicables sur la fenêtre, au facteur de pas du module (aucune valeur
  // recopiée). Borne HAUTE car l'épisode d'oubli s'arme au premier tick, ce qui
  // lui coûte un pas ; l'inégalité reste donc valide dans le sens qu'on teste.
  float oubliSeulBorne = charge;
  const uint32_t pasDOubli =
      (attente - kPhIntegralMaxTickDtMs) / kPhIntegralDecayStepMs;
  for (uint32_t i = 0; i < pasDOubli; ++i) oubliSeulBorne *= kPhIntegralDecayStepFactor;
  // Référence 2 — DÉCHARGE SIGNÉE SEULE : (erreur − seuil d'arrêt) × durée.
  const float perteParDecharge =
      (kStopThreshold - kExcursionFranche) * (float)(attente / 1000u);
  const float dechargeSeule = charge - perteParDecharge;

  PhIntegralState st;
  st.integral = charge;
  st.lastTickMs = kT0;
  uint32_t t = kT0;
  int purges = 0;
  tenirPendant(st, tun, kExcursionFranche, attente, t, &purges);

  TEST_ASSERT_EQUAL_INT(0, purges);          // on est bien AVANT la purge…
  TEST_ASSERT_TRUE(st.resetHoldMs > 0u);     // …et la tenue court
  TEST_ASSERT_TRUE(st.decayMs != 0u);        // épisode d'oubli ARMÉ

  // Sous la référence « oubli seul » ⇒ la décharge signée a bien tourné EN PLUS
  // (marge dérivée : au moins la moitié de la décharge théorique de la fenêtre).
  TEST_ASSERT_TRUE(st.integral < oubliSeulBorne - 0.5f * perteParDecharge);
  // Sous la référence « décharge seule » ⇒ l'oubli exponentiel a bien tourné.
  TEST_ASSERT_TRUE(st.integral < dechargeSeule - 500.0f);
  // Ordre de grandeur annoncé par le gate (« −16 % sur les 15 minutes ») : la
  // protection graduée est réelle, sans être une purge déguisée.
  const float perte = (charge - st.integral) / charge;
  TEST_ASSERT_TRUE(perte > 0.10f);
  TEST_ASSERT_TRUE(perte < 0.30f);

  // Et quand la purge finit par tomber, elle abandonne l'intégrale DÉJÀ ENTAMÉE,
  // pas les 12 000 pH·s du départ.
  PhIntegralVerdict v = tenirPendant(st, tun, kExcursionFranche, 60000UL, t, &purges);
  TEST_ASSERT_EQUAL_INT(1, purges);
  TEST_ASSERT_TRUE(v.integralBefore < charge);
}

// La tenue se compte en temps de BRANCHE, comme la charge et comme la tenue
// d'anomalie : des ticks espacés au-delà du dt maximal (branche dégradée) ne la
// font PAS mûrir toute seule. Sans cela, un appareil qui tourne mal purgerait son
// apprentissage sur une base de temps qu'il ne maîtrise pas.
void test_P_la_tenue_du_reset_se_compte_en_temps_de_branche(void) {
  const PhIntegralTuning tun = tuning();
  PhIntegralState st;
  st.integral = 12000.0f;
  st.lastTickMs = kT0;
  uint32_t t = kT0;
  int purges = 0;

  // 2 h de temps MURAL par pas de 30 s (> kPhIntegralMaxTickDtMs) : aucun dt
  // n'est crédité, donc la tenue reste à zéro malgré une excursion permanente.
  tenirPendant(st, tun, kExcursionFranche, 2u * 3600000UL, t, &purges,
               /*pasMs*/30000u);
  TEST_ASSERT_EQUAL_INT(0, purges);
  TEST_ASSERT_EQUAL_UINT32(0u, st.resetHoldMs);
  // L'oubli, lui, a bien tourné (asymétrie assumée, cf. étape 5 du module) : la
  // protection ne dépend donc pas de la qualité de la base de temps.
  TEST_ASSERT_TRUE(st.integral < 0.30f * 12000.0f);
}

// CONTRAT NUMÉRIQUE de la temporisation — ici la VALEUR est le sujet : 15 min,
// c'est la pause mélange, en deçà de laquelle l'excursion mesurée peut n'être que
// la dose précédente pas encore homogénéisée.
void test_P_temporisation_du_reset_encadree_par_la_chimie(void) {
  TEST_ASSERT_EQUAL_UINT32(900000UL, kPhIntegralResetHoldMs);
  TEST_ASSERT_EQUAL_UINT32((uint32_t)kPhMixingDelayMs, kPhIntegralResetHoldMs);
  // Franchement sous la demi-vie de l'oubli : au-delà, l'oubli aurait déjà fait le
  // travail et le reset sec — seul mécanisme RAPIDE sur inversion de sens — ne
  // servirait plus à rien.
  TEST_ASSERT_TRUE(kPhIntegralResetHoldMs < kPhIntegralDecayHalfLifeMs);
  // …et très en deçà de la constante d'intégration : la purge reste « rapide »
  // devant la dynamique qu'elle est censée corriger.
  TEST_ASSERT_TRUE((float)kPhIntegralResetHoldMs < 0.25f * kPhIntegralTiNormalS * 1000.0f);
  // La tenue doit rester représentable en pas de dt acceptables (sinon elle ne
  // pourrait jamais s'accumuler complètement).
  TEST_ASSERT_TRUE(kPhIntegralResetHoldMs > kPhIntegralMaxTickDtMs);
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
  st.resetHoldMs = 4242;  // feature-084 : la tenue fait partie de l'état à purger
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
  TEST_ASSERT_EQUAL_UINT32(0u, st.resetHoldMs);
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

// RÉÉCRIT (feature-084) — l'ancienne version codait 7 200 000 / 21 600 000 /
// 43 200 000 en dur : elle figeait la CALIBRATION, alors que le sujet du test est
// la MÉCANIQUE des paliers (un warning, puis un critical, puis des rappels
// périodiques qui ne retombent jamais dans le silence). Tout est dérivé ; les
// valeurs elles-mêmes sont verrouillées par le test de recalibration dédié.
void test_P_paliers_warn_puis_critical_puis_rappels(void) {
  // Ordonnancement STRUCTUREL des trois seuils (invariant (vii ter) du gate).
  TEST_ASSERT_TRUE(kPhIntegralSaturationWarnMs < kPhIntegralSaturationCriticalMs);
  TEST_ASSERT_TRUE(kPhIntegralSaturationReminderMs >= kPhIntegralSaturationCriticalMs);

  const PhIntegralTuning tun = tuning(/*ki*/0.0f);
  PhIntegralState st;
  st.lastTickMs = kT0;
  const float error = 0.50f;  // demande 4 mL/min ≫ 2,06 soutenables, erreur ≫ 0,01

  long warns = 0, crits = 0;
  uint32_t warnAt = 0, firstCritAt = 0, secondCritAt = 0, thirdCritAt = 0;
  uint32_t t = kT0;
  // Fenêtre DÉRIVÉE : de quoi voir le critical et DEUX rappels, plus une marge
  // d'une heure de branche pour prouver qu'il n'en tombe pas un troisième.
  const uint32_t pas = kPhIntegralMaxTickDtMs;
  const uint64_t fenetreMs = (uint64_t)kPhIntegralSaturationCriticalMs +
                             2ULL * kPhIntegralSaturationReminderMs + 3600000ULL;
  const long ticks = (long)(fenetreMs / pas);
  for (long i = 0; i < ticks; ++i) {
    t += pas;
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
  // Warning UNE seule fois, à son palier.
  TEST_ASSERT_EQUAL_INT(1, warns);
  TEST_ASSERT_UINT32_WITHIN(pas, kPhIntegralSaturationWarnMs, warnAt);
  // Critical à son palier, puis rappels — un état anormal qui dure ne doit PAS
  // retomber dans le silence.
  TEST_ASSERT_EQUAL_INT(3, crits);
  TEST_ASSERT_UINT32_WITHIN(pas, kPhIntegralSaturationCriticalMs, firstCritAt);
  TEST_ASSERT_UINT32_WITHIN(pas,
                            kPhIntegralSaturationCriticalMs + kPhIntegralSaturationReminderMs,
                            secondCritAt);
  TEST_ASSERT_UINT32_WITHIN(pas,
                            kPhIntegralSaturationCriticalMs + 2u * kPhIntegralSaturationReminderMs,
                            thirdCritAt);
}

// ⚠️ SENTINELLE — feature-084 : LA RECALIBRATION DES PALIERS C5.
// -----------------------------------------------------------------------------
// HISTORIQUE, à conserver : la première version de ce test figeait le facteur ×4
// annoncé — « le tick tournait sous canDose(0) (~25 % du temps), il tourne
// désormais sur TOUTE la fenêtre », donc ×4, à fenêtre réelle prétendument
// inchangée. Or le rapport cyclique chiffré par le gate lui-même ne passait pas de
// 25 % à 100 % mais de 25 % à 29 %, soit ×1,16. Le test a EXHIBÉ l'écart (il
// exigeait 28 800 000, la constante valait 8 400 000) et la recalibration a été
// corrigée. On le GARDE, réécrit sur les valeurs retenues.
//
// CE QU'IL SURVEILLE, et que rien d'autre ne surveille à l'exécution :
// l'invariant « palier ÷ rapport cyclique = constante », c'est-à-dire la fenêtre
// de preuve en TEMPS RÉEL — la seule grandeur qui ait un sens pour l'exploitant.
// constants.h le verrouille par trois static_assert à 5 % près ; ceux-ci peuvent
// être relâchés dans le même geste que les paliers. Ici, la référence terrain
// (v2.26.0 : 2 h / 6 h / 12 h de branche à 25 %) est RÉÉCRITE EN DUR : une
// recalibration future ne peut pas la déplacer avec elle.
void test_P_SENTINELLE_recalibration_laisse_la_fenetre_reelle_inchangee(void) {
  // 1. La référence terrain v2.26.0, en dur — indépendante de constants.h.
  const uint32_t kWarnV2260 = 7200000UL;   // 2 h de branche — validé terrain
  const uint32_t kCritV2260 = 21600000UL;  // 6 h de branche — validé terrain
  const uint32_t kRappV2260 = 43200000UL;  // 12 h de branche — validé terrain
  // …et son report dans constants.h, qui sert de base aux static_assert.
  TEST_ASSERT_EQUAL_UINT32(kWarnV2260, kPhIntegralSaturationWarnMsV2260);
  TEST_ASSERT_EQUAL_UINT32(kCritV2260, kPhIntegralSaturationCriticalMsV2260);
  TEST_ASSERT_EQUAL_UINT32(kRappV2260, kPhIntegralSaturationReminderMsV2260);

  // 2. Les paliers RETENUS (facteur 1,16, pas 4).
  TEST_ASSERT_EQUAL_UINT32(8400000UL,  kPhIntegralSaturationWarnMs);      // 2 h 20
  TEST_ASSERT_EQUAL_UINT32(25200000UL, kPhIntegralSaturationCriticalMs);  // 7 h
  TEST_ASSERT_EQUAL_UINT32(50400000UL, kPhIntegralSaturationReminderMs);  // 14 h

  // 3. Les rapports cycliques déclarés au gate — ce sont EUX qui justifient le
  //    facteur ; les figer empêche de « corriger » un palier en bougeant sa base.
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.25f, kPhIntegralDutyV2260);
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.29f, kPhIntegralDutyF084);

  // 4. LA PROPRIÉTÉ : la fenêtre de preuve en temps RÉEL est inchangée, palier par
  //    palier (8 h / 24 h / 48 h), à 1 % près — soit l'arrondi de +0,6 % entre le
  //    facteur appliqué (1,1667) et le rapport des rapports cycliques (1,16).
  struct { uint32_t avant, apres; float heures; const char* nom; } paliers[] = {
      {kWarnV2260, kPhIntegralSaturationWarnMs,      8.0f, "warning"},
      {kCritV2260, kPhIntegralSaturationCriticalMs, 24.0f, "critical"},
      {kRappV2260, kPhIntegralSaturationReminderMs, 48.0f, "rappel"},
  };
  for (int i = 0; i < 3; ++i) {
    const float heuresAvant = paliers[i].avant / kPhIntegralDutyV2260 / 3600000.0f;
    const float heuresApres = paliers[i].apres / kPhIntegralDutyF084 / 3600000.0f;
    char msg[128];
    snprintf(msg, sizeof(msg), "palier %s : %.2f h avant, %.2f h apres",
             paliers[i].nom, (double)heuresAvant, (double)heuresApres);
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.01f * paliers[i].heures, paliers[i].heures,
                                     heuresAvant, msg);
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.01f * heuresAvant, heuresAvant, heuresApres,
                                     msg);
    // Et le facteur appliqué au palier suit bien celui du rapport cyclique.
    const float facteurPalier = (float)paliers[i].apres / (float)paliers[i].avant;
    const float facteurDuty = kPhIntegralDutyF084 / kPhIntegralDutyV2260;
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.01f * facteurDuty, facteurDuty,
                                     facteurPalier, msg);
    TEST_ASSERT_TRUE_MESSAGE(facteurPalier < 2.0f, msg);  // ×4 ⇒ ce test tombe
  }

  // 5. Le rappel n'est plus aligné sur le palier critical (il l'était en v2.26.0
  //    par un ×4 tronqué) : il vaut le double, comme le voulait la v2.26.0.
  TEST_ASSERT_EQUAL_UINT32(2u * kPhIntegralSaturationCriticalMs,
                           kPhIntegralSaturationReminderMs);
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
// tenue repart de zéro — une deuxième alerte devra remûrir tout le palier.
// RÉÉCRIT (feature-084) : l'ancienne version ne pilotait que 3 h en dur, ce qui ne
// franchit plus le palier de warning recalibré. La durée est désormais DÉRIVÉE.
void test_P_fin_d_anomalie_leve_l_alerte_et_remet_la_tenue_a_zero(void) {
  const PhIntegralTuning tun = tuning(/*ki*/0.0f);
  PhIntegralState st;
  st.lastTickMs = kT0;
  uint32_t t = kT0;
  // Le palier de warning, plus 10 min de branche de marge → warning émis.
  const uint32_t pas = kPhIntegralMaxTickDtMs;
  const long ticks = (long)((kPhIntegralSaturationWarnMs + 600000UL) / pas);
  for (long i = 0; i < ticks; ++i) {
    t += pas;
    phIntegralTick(st, inputs(t, 0.50f, kAmpFlow, kSatBudget), tun);
  }
  TEST_ASSERT_TRUE(st.warnSent);
  TEST_ASSERT_TRUE(st.anomalyHeldMs >= kPhIntegralSaturationWarnMs);

  // Retour à la normale : demande ramenée sous 85 % du soutenable.
  t += pas;
  PhIntegralVerdict v =
      phIntegralTick(st, inputs(t, errorForRatio(50.0f), kAmpFlow, kSatBudget), tun);
  TEST_ASSERT_FALSE(v.saturated);
  TEST_ASSERT_TRUE(v.emitClear);
  TEST_ASSERT_EQUAL_UINT32(0u, st.anomalyHeldMs);
  TEST_ASSERT_FALSE(st.warnSent);
  TEST_ASSERT_FALSE(st.critSent);

  // Et le clear n'est émis QU'UNE fois (pas de spam au tour suivant).
  t += pas;
  v = phIntegralTick(st, inputs(t, errorForRatio(50.0f), kAmpFlow, kSatBudget), tun);
  TEST_ASSERT_FALSE(v.emitClear);
}

// RÉÉCRIT (feature-084) — L'ALERTE TOMBE TOUT DE SUITE, LA PURGE ATTEND.
// L'ancienne version figeait l'enchaînement « un tick sous −0,15 ⇒ purge ET levée
// d'alerte dans le même verdict ». Avec la temporisation, les deux se dissocient,
// et c'est une PROPRIÉTÉ DE SÛRETÉ à verrouiller : l'étape 7 lève l'alerte dès le
// PREMIER tick de l'excursion (l'anomalie exige une erreur positive), donc le
// différé ne laisse JAMAIS une alerte pendante pendant les 15 minutes d'attente.
// Quand la purge finit par tomber, il n'y a plus rien à lever.
void test_P_excursion_leve_l_alerte_aussitot_et_purge_apres_la_tenue(void) {
  const PhIntegralTuning tun = tuning(/*ki*/0.0f);
  PhIntegralState st;
  st.lastTickMs = kT0;
  st.satActive = true;
  st.anomalyHeldMs = kPhIntegralSaturationCriticalMs;
  st.warnSent = true;
  st.critSent = true;
  st.integral = 9000.0f;

  // PREMIER tick de l'excursion : pas de purge (la tenue démarre), mais l'alerte
  // est levée immédiatement et la tenue d'anomalie remise à zéro.
  uint32_t t = kT0 + kPhIntegralMaxTickDtMs;
  PhIntegralVerdict v = phIntegralTick(st, inputs(t, -0.30f), tun);
  TEST_ASSERT_TRUE(v.event != PhIntegralEvent::kDryReset);
  TEST_ASSERT_TRUE(v.emitClear);
  TEST_ASSERT_FALSE(st.warnSent);
  TEST_ASSERT_FALSE(st.critSent);
  TEST_ASSERT_EQUAL_UINT32(0u, st.anomalyHeldMs);
  TEST_ASSERT_TRUE(st.resetHoldMs > 0u);

  // La purge tombe au terme de la tenue, sans réémettre de levée (plus d'alerte
  // ouverte) et sur une intégrale déjà entamée par la protection graduée.
  int purges = 0;
  v = tenirPendant(st, tun, -0.30f,
                   kPhIntegralResetHoldMs - kPhIntegralMaxTickDtMs, t, &purges);
  TEST_ASSERT_EQUAL_INT(1, purges);
  TEST_ASSERT_TRUE(v.event == PhIntegralEvent::kDryReset);
  TEST_ASSERT_FALSE(v.emitClear);
  TEST_ASSERT_TRUE(v.integralBefore > 0.0f);
  TEST_ASSERT_TRUE(v.integralBefore < 9000.0f);
  TEST_ASSERT_FLOAT_WITHIN(kEps, 0.0f, st.integral);
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
// D — feature-084, sujet 1 : LA CADENCE (ce qui en est atteignable en natif)
// =============================================================================
// Le prédicat `phIntegralAccumulationAllowed()` est une méthode PRIVÉE de la
// coquille : « n'accumule pas en manual / sous garde bloquante / filtre non prêt »
// NE PEUT PAS être vérifié ici (cf. l'en-tête du fichier — une mutation permissive
// du prédicat ne tuerait aucun test). Ce qui EST vérifiable, et qui l'est ci-dessous,
// ce sont les BORNES CHIMIQUES sur lesquelles le gate s'appuie pour autoriser
// l'accumulation dans les états où le dosage est seulement SUSPENDU.

// PAUSE MÉLANGE. Le tick tourne désormais pendant les 15 min de mélange, où l'on
// mesure une eau que la dose précédente n'a pas fini d'homogénéiser. Le gate borne
// ce sur-crédit à « 15 min × 0,15 pH » ; on le vérifie sur le module RÉEL, avec la
// pire erreur plausible dans cet état (l'excursion de purge, en valeur absolue).
void test_D_sur_credit_d_une_pause_melange_reste_negligeable(void) {
  const PhIntegralTuning tun = tuning();
  PhIntegralState st;
  st.lastTickMs = kT0;
  uint32_t t = kT0;
  tenirPendant(st, tun, kSeuilPurge, (uint32_t)kPhMixingDelayMs, t);

  // Moins de 1 % du plafond d'anti-windup…
  TEST_ASSERT_TRUE(st.integral > 0.0f);
  TEST_ASSERT_TRUE(st.integral < 0.01f * kPhIntegralMaxNormal);
  // …et moins de 5 % de la charge d'équilibre en demande : chimiquement inerte,
  // même si la pause mélange se répétait à chaque cycle.
  TEST_ASSERT_TRUE(kPhKiNormal * st.integral < 0.05f * kPhEquilibriumLoadMlPerMin);
}

// LIMITE HORAIRE ÉPUISÉE. C'est l'état qui inquiète le plus : le dosage est
// suspendu, mais l'intégrale continue d'accumuler. Le gate répond « les deux gels
// d'anti-windup restent actifs et bornent l'accumulation exactement comme avant ».
// Vérification : sur 24 h de branche à limite épuisée, l'intégrale ne monte PAS au
// plafond — elle se pose exactement au point où la demande atteint le débit
// SOUTENABLE, et n'en bouge plus.
void test_D_limite_horaire_epuisee_l_accumulation_est_bornee_par_le_gel(void) {
  const PhIntegralTuning tun = tuning();
  const float erreur = 0.10f;                        // au-dessus du seuil de démarrage
  const float soutenable = phSustainableAvgFlow(kAmpFlow, kSatBudget);
  // Point de gel analytique : kp·e + ki·I == soutenable.
  const float integraleAuGel = (soutenable - kPhKpNormal * erreur) / kPhKiNormal;

  PhIntegralState st;
  st.lastTickMs = kT0;
  uint32_t t = kT0;
  PhIntegralVerdict v = tenirPendant(st, tun, erreur, 24u * 3600000UL, t,
                                     /*purges*/nullptr, kPhIntegralMaxTickDtMs,
                                     kAmpFlow, kSatBudget);

  // L'accumulation a bien eu lieu (c'est tout l'objet du changement de cadence)…
  TEST_ASSERT_TRUE(st.integral > 0.0f);
  // …mais elle s'est arrêtée AU point de gel, très loin du plafond.
  TEST_ASSERT_FLOAT_WITHIN(0.02f * integraleAuGel, integraleAuGel, st.integral);
  TEST_ASSERT_TRUE(st.integral < 0.25f * kPhIntegralMaxNormal);
  TEST_ASSERT_TRUE(v.frozen);
  // Et la demande accumulée ne dépasse jamais ce que l'actionneur peut tenir.
  TEST_ASSERT_TRUE(v.demandMlPerMin <= soutenable * 1.02f);
}

// =============================================================================
// R — feature-084, sujet 3 : LA PERSISTANCE NVS (couverture PARTIELLE, assumée)
// =============================================================================
// ⚠️ LIRE L'EN-TÊTE DU FICHIER AVANT DE FAIRE CONFIANCE À CE BLOC.
// Ni le blob, ni son CRC, ni la séquence de décision R1-R7 ne sont atteignables en
// natif (src/config.cpp et src/pump_controller.cpp ne compilent pas hors ESP32, et
// aucune extraction pure n'existe). Ce bloc couvre donc DEUX choses seulement :
//   (1) le CONTRAT NUMÉRIQUE des sept conditions, avec leurs encadrements mutuels ;
//   (2) la PROPRIÉTÉ CHIMIQUE que R1 est censé garantir, vérifiée en boucle fermée
//       de production — c'est-à-dire ce que la fraction de 30 % achète réellement.
// Ce qui reste NON COUVERT est listé sans détour dans le rapport de test.

// R1 — LE VERROU CHIMIQUE, aux TROIS vitesses de régulation. Le static_assert de
// constants.h ne porte que sur l'autorité nominale ; ici on recalcule la
// contribution restaurable réelle pour chaque vitesse (ki × 30 % × plafond).
void test_R1_le_plafond_de_restauration_reste_sous_la_charge_d_equilibre(void) {
  // La valeur EST le sujet : c'est elle que le gate a négociée.
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.30f, kPhIntegralRestoreMaxFraction);
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, 2.86f, kPhEquilibriumLoadMlPerMin);

  const float slow   = kPhKiSlow   * kPhIntegralRestoreMaxFraction * kPhIntegralMaxSlow;
  const float normal = kPhKiNormal * kPhIntegralRestoreMaxFraction * kPhIntegralMaxNormal;
  const float fast   = kPhKiFast   * kPhIntegralRestoreMaxFraction * kPhIntegralMaxFast;

  // Strictement sous la charge d'équilibre MESURÉE : aucune restauration ne peut,
  // à elle seule, produire un surdosage net.
  TEST_ASSERT_TRUE(slow   < kPhEquilibriumLoadMlPerMin);
  TEST_ASSERT_TRUE(normal < kPhEquilibriumLoadMlPerMin);
  TEST_ASSERT_TRUE(fast   < kPhEquilibriumLoadMlPerMin);
  // …avec une marge réelle, pas au ras du seuil : 1,80 mL/min pour 2,86.
  TEST_ASSERT_FLOAT_WITHIN(0.05f, 1.80f, normal);
  TEST_ASSERT_TRUE(normal < 0.70f * kPhEquilibriumLoadMlPerMin);
  // L'autorité restaurable est la même aux trois vitesses (verrou I2 hérité).
  TEST_ASSERT_FLOAT_WITHIN(0.05f, normal, slow);
  TEST_ASSERT_FLOAT_WITHIN(0.05f, normal, fast);
}

// R1 — CE QUE LA FRACTION ACHÈTE, en boucle fermée de PRODUCTION : la demande du
// TRANSITOIRE de restauration. C'est l'objection exacte de l'ADR-0052 (« une
// autorisation de dosage PRÉ-CHARGÉE au boot ») ramenée à une grandeur mesurable —
// la demande maximale de la première heure après application.
//   • écrêtée à 30 %, elle reste de l'ordre de la charge d'équilibre du bassin :
//     le bidon ne peut pas partir plus vite que le bassin ne consomme ;
//   • servie au plafond PLEIN — ce qu'un instantané non écrêté produirait — elle
//     vaut plus du DOUBLE de cette charge.
//
// ⚠️ DEUX LIMITES DE PORTÉE, à lire avant d'en tirer une garantie :
//   1. ce test verrouille la FRACTION, pas le CLAMP. Le `if (candidate > cap)` de
//      pump_controller.cpp n'est jamais exécuté ici ; le supprimer ne tue rien.
//   2. le modèle de bassin n'a AUCUN retard de transport (la pause mélange n'y
//      est pas) : l'hystérésis y coupe le dosage à l'instant exact où l'erreur
//      passe sous le seuil d'arrêt. Le modèle SOUS-ESTIME donc l'excursion d'une
//      intégrale sur-restaurée — c'est bien pourquoi R1 borne l'AUTORITÉ plutôt
//      que de s'en remettre au comportement de la boucle.
void test_R1_une_restauration_plafonnee_ne_precharge_pas_de_dosage(void) {
  const float erreurDeDepart = kStartThreshold + 0.01f;  // R4 : erreur soutenue
  const float plafondR1 = kPhIntegralRestoreMaxFraction * kPhIntegralMaxNormal;

  ProdStats ecrete = simulateProduction(kPhKpNormal, kPhKiNormal, kPhIntegralMaxNormal,
                                        kBudgetNominalMinPerHour, /*days*/6,
                                        DeadbandPolicy::kAlignedOnHysteresis,
                                        plafondR1, erreurDeDepart);
  // Le transitoire ne dépasse pas de plus de 25 % la charge d'équilibre mesurée :
  // « au pire elle dispense la boucle de réapprendre », pas de surdosage net.
  TEST_ASSERT_TRUE(ecrete.firstHourMaxFlow < 1.25f * kPhEquilibriumLoadMlPerMin);
  // JAMAIS sous la consigne, transitoire initial compris.
  TEST_ASSERT_TRUE(ecrete.minErrEver >= 0.0f);
  // …et le régime établi reste celui de la boucle nominale : la restauration
  // dispense d'apprendre, elle ne déplace pas le point de fonctionnement.
  TEST_ASSERT_FLOAT_WITHIN(0.005f, kStopThreshold, ecrete.meanErrLastDay);
  TEST_ASSERT_EQUAL_INT(0, ecrete.criticalCount);

  // Contre-épreuve : la MÊME boucle servie au plafond PLEIN réclame d'emblée plus
  // du double de la charge du bassin — l'autorisation pré-chargée que R1 refuse.
  ProdStats plein = simulateProduction(kPhKpNormal, kPhKiNormal, kPhIntegralMaxNormal,
                                       kBudgetNominalMinPerHour, 6,
                                       DeadbandPolicy::kAlignedOnHysteresis,
                                       kPhIntegralMaxNormal, erreurDeDepart);
  TEST_ASSERT_TRUE(plein.firstHourMaxFlow > 2.0f * kPhEquilibriumLoadMlPerMin);
  TEST_ASSERT_TRUE(plein.firstHourMaxFlow > 2.0f * ecrete.firstHourMaxFlow);

  // …et le prix ne se paie pas qu'au transitoire : SIX JOURS plus tard, la boucle
  // sur-restaurée n'est TOUJOURS pas revenue au seuil d'arrêt. Elle s'installe sur
  // un cycle limite trois fois plus haut, avec une intégrale parquée bien au-dessus
  // de sa valeur d'équilibre. Une restauration excessive ne coûte donc pas « un
  // peu de transitoire » : elle déplace durablement le point de fonctionnement.
  TEST_ASSERT_TRUE(plein.meanErrLastDay > 2.0f * ecrete.meanErrLastDay);
  TEST_ASSERT_TRUE(plein.finalIntegral > 1.3f * ecrete.finalIntegral);
}

// R2/R3/R4/R5/R7 — CONTRAT NUMÉRIQUE des conditions restantes. Ce test ne vérifie
// AUCUN comportement (le code de décision n'est pas atteignable en natif) : il
// verrouille les valeurs négociées au gate et leurs encadrements mutuels, de sorte
// qu'un ajustement silencieux d'une des sept conditions casse la suite.
void test_R_contrat_numerique_des_conditions_de_persistance(void) {
  // R2 — schéma : toute retouche de structure DOIT l'incrémenter.
  TEST_ASSERT_EQUAL_UINT8(1, kPhIntegralPersistSchema);

  // R3 — péremption 6 h = UNE constante d'intégration à vitesse `normal`.
  TEST_ASSERT_EQUAL_UINT32(21600UL, kPhIntegralPersistMaxAgeS);
  TEST_ASSERT_FLOAT_WITHIN(1.0f, kPhIntegralTiNormalS, (float)kPhIntegralPersistMaxAgeS);
  // …et franchement plus courte que la péremption d'inactivité de 24 h.
  TEST_ASSERT_TRUE((uint64_t)kPhIntegralPersistMaxAgeS * 1000ULL <
                   (uint64_t)kPhIntegralStaleResetMs);

  // R4 — confirmation par la mesure : 5 min, soit bien plus qu'un tick, et très
  // en deçà de la péremption (sinon la candidate mourrait avant d'être confirmée).
  TEST_ASSERT_EQUAL_UINT32(300000UL, kPhIntegralRestoreConfirmMs);
  TEST_ASSERT_TRUE(kPhIntegralRestoreConfirmMs >= 10UL * kPhIntegralChargeQuantumMs);
  TEST_ASSERT_TRUE((uint64_t)kPhIntegralRestoreConfirmMs <
                   (uint64_t)kPhIntegralPersistMaxAgeS * 1000ULL / 10ULL);
  // La fenêtre d'armement doit laisser le temps d'au moins une confirmation.
  TEST_ASSERT_EQUAL_UINT32(600000UL, kPhIntegralRestoreArmWindowMs);
  TEST_ASSERT_TRUE(kPhIntegralRestoreArmWindowMs >= kPhIntegralRestoreConfirmMs);

  // R5 — cadence d'écriture : 30 min ET 5 % du plafond.
  TEST_ASSERT_EQUAL_UINT32(1800000UL, kPhIntegralPersistMinIntervalMs);
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.05f, kPhIntegralPersistMinDeltaFrac);
  // Le seuil d'écriture doit rester SOUS le plafond de restauration, sans quoi une
  // intégrale sous ce plafond ne serait jamais persistée (mécanisme mort-né).
  TEST_ASSERT_TRUE(kPhIntegralPersistMinDeltaFrac < kPhIntegralRestoreMaxFraction);
  // Usure NVS au PIRE cas (une écriture toutes les 30 min, en continu) : le
  // secteur doit tenir plusieurs années devant les 100 000 cycles garantis.
  const float ecrituresParJour = 24.0f * 3600000.0f / (float)kPhIntegralPersistMinIntervalMs;
  TEST_ASSERT_TRUE(100000.0f / (ecrituresParJour * 365.0f) > 5.0f);

  // R7 — anti-boucle de démarrage : 3 restaurations sans écriture intercalée.
  TEST_ASSERT_EQUAL_UINT8(3, kPhIntegralRestoreMaxStreak);
  TEST_ASSERT_TRUE(kPhIntegralRestoreMaxStreak >= 1 && kPhIntegralRestoreMaxStreak <= 5);
  // Le garde-fou n'a de sens que si une boucle de reboot ne peut PAS produire
  // d'écriture normale (qui remettrait le compteur à zéro) : l'intervalle minimal
  // d'écriture doit dominer la fenêtre d'armement, donc a fortiori le temps que
  // met un appareil en panique à se relancer.
  TEST_ASSERT_TRUE(kPhIntegralPersistMinIntervalMs > kPhIntegralRestoreArmWindowMs);
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
  RUN_TEST(test_P_quota_de_cycles_epuise_gele_la_charge_pas_la_decharge);
  RUN_TEST(test_P_les_trois_motifs_de_gel_sont_disjonctifs);
  RUN_TEST(test_P_erreur_moins_0011_n_efface_pas_l_integrale);
  RUN_TEST(test_P_reset_sec_uniquement_au_dela_de_moins_015);
  // B — feature-084 sujet 2 : la temporisation du reset sec
  RUN_TEST(test_P_reset_sec_non_declenche_a_14_min_declenche_a_15);
  RUN_TEST(test_P_la_tenue_du_reset_repart_a_zero_a_la_moindre_remontee);
  RUN_TEST(test_P_pendant_l_attente_du_reset_la_protection_tourne_toujours);
  RUN_TEST(test_P_la_tenue_du_reset_se_compte_en_temps_de_branche);
  RUN_TEST(test_P_temporisation_du_reset_encadree_par_la_chimie);
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
  RUN_TEST(test_P_paliers_warn_puis_critical_puis_rappels);
  RUN_TEST(test_P_SENTINELLE_recalibration_laisse_la_fenetre_reelle_inchangee);
  RUN_TEST(test_P_la_tenue_de_l_anomalie_ne_court_pas_en_temps_mural);
  RUN_TEST(test_P_fin_d_anomalie_leve_l_alerte_et_remet_la_tenue_a_zero);
  RUN_TEST(test_P_excursion_leve_l_alerte_aussitot_et_purge_apres_la_tenue);
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
  // D — feature-084 sujet 1 : bornes chimiques de la nouvelle cadence
  RUN_TEST(test_D_sur_credit_d_une_pause_melange_reste_negligeable);
  RUN_TEST(test_D_limite_horaire_epuisee_l_accumulation_est_bornee_par_le_gel);
  // R — feature-084 sujet 3 : persistance NVS (couverture partielle assumée)
  RUN_TEST(test_R1_le_plafond_de_restauration_reste_sous_la_charge_d_equilibre);
  RUN_TEST(test_R1_une_restauration_plafonnee_ne_precharge_pas_de_dosage);
  RUN_TEST(test_R_contrat_numerique_des_conditions_de_persistance);

  return UNITY_END();
}
