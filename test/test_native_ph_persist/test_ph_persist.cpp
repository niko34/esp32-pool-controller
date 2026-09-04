// =============================================================================
// Tests unitaires natifs — PERSISTANCE NVS de l'intégrale pH (feature-084)
// =============================================================================
// Cible : src/ph_persist_logic.h, module PUR (ADR-0017). Tournent sur PC
// (env:native, Unity), hors ESP32 / NVS / Arduino.
//
// POURQUOI CETTE SUITE EXISTE
// ---------------------------
// Au passage de test précédent, les sept garanties R1-R7 vivaient dans
// config.cpp et pump_controller.cpp, aucun des deux n'étant dans le
// `build_src_filter` de env:native. TROIS MUTATIONS SURVIVAIENT à la suite
// complète : supprimer l'écrêtage R1, supprimer la vérification de CRC, rendre
// le prédicat anti-boucle permissif. La couverture se limitait alors au
// « contrat numérique » des constantes (test_native_ph_integral, bloc R), qui
// ne teste AUCUN comportement. L'extraction du module rend ces décisions
// atteignables ; cette suite les couvre condition par condition.
//
// L'ENJEU, en une phrase : une restauration est une DOSE POTENTIELLEMENT
// PRÉ-ARMÉE AU DÉMARRAGE. Chaque garantie qui tombe silencieusement se paie en
// acide versé dans un bassin dont on ne connaît pas encore l'état.
//
// ORGANISATION
//   A — Intégrité du blob : CRC, taille, schéma, déterminisme du scellement
//   B — Empreinte de contexte (R2)
//   C — R1, le DOUBLE plafond : butée absolue ∧ borne dérivée de la charge
//   D — R3, péremption
//   E — R4, confirmation différée par la mesure
//   F — R7, anti-boucle de démarrage
//   G — R5, cadence d'écriture
//   H — Contrat numérique et libellés de rejet
//   I — L'anneau de charge : datation, fenêtre d'âge, vraisemblance, médiane
//
// CE QUI RESTE HORS D'ATTEINTE (dit sans détour, cf. rapport de test) :
//   - l'accès NVS lui-même (config.cpp : nvs_get_blob / nvs_set_blob) ;
//   - le câblage de pump_controller.cpp : fenêtre d'armement
//     kPhIntegralRestoreArmWindowMs, points d'appel de Interrupt/Abandon,
//     `phIntegralAccumulationAllowed()` qui décide quand un tick R4 est légitime,
//     et la journalisation R6 ;
//   - la DATE DU JOUR fournie au bloc I : elle vient de localtime_r dans
//     config.cpp. Le module, lui, ne voit qu'un entier AAAAMMJJ — et le traite
//     comme tel, y compris quand il vaut 0 (horloge absente ⇒ anneau écarté).
// =============================================================================

#include <unity.h>
#include <math.h>    // C header uniquement (libc++ <cmath> indisponible sur l'hôte)
#include <stdio.h>   // snprintf : messages des balayages
#include <string.h>  // memcpy

#include "constants.h"
#include "ph_integral_logic.h"  // phSustainableAvgFlow : le seuil du filet en dépend
#include "ph_persist_logic.h"

void setUp(void) {}
void tearDown(void) {}

// -----------------------------------------------------------------------------
// Décor commun — réglages de PRODUCTION à vitesse `normal`, bassin de référence.
// -----------------------------------------------------------------------------
static const float kKp    = kPhKpNormal;           // 8 mL/min par pH
static const float kKi    = kPhKiNormal;           // 3,704e-4 mL/min par pH·s
static const float kImax  = kPhIntegralMaxNormal;  // 16 200 pH·s
static const float kStop  = 0.01f;                 // pumpProtection.phStopThreshold
static const float kStart = 0.05f;                 // pumpProtection.phStartThreshold

// Heure murale de référence : 15 janv. 2027, très au-dessus de kMinValidEpoch.
static const uint32_t kNow = 1800000000UL;
// feature-093 : compteur MONOTONE de filtration « courant » (minutes). Les
// instantanés scellés par défaut portent ce compteur → âge en filtration nul.
static const uint32_t kFiltNow = 100000UL;


static PhPersistLoadObservation obs(float loadMlPerMin, uint8_t days) {
  PhPersistLoadObservation o;
  o.loadMlPerMin = loadMlPerMin;
  o.days = days;
  return o;
}

/** Plafond R1 aux réglages `normal`, historique plein, pour une charge donnée. */
static PhPersistCapVerdict capFor(float loadMlPerMin, uint8_t days = 7) {
  return phPersistRestoreCap(kImax, kKp, kKi, kStop, obs(loadMlPerMin, days));
}

/** Remplissage d'ÉQUILIBRE I* = (charge − kp·stop) / ki, recalculé ICI (et non
 *  repris du verdict) : sans quoi le test validerait le module par lui-même. */
static float equilibriumPhS(float loadMlPerMin, float kp, float ki, float stop) {
  return (loadMlPerMin - kp * stop) / ki;
}

/** Instantané SCELLÉ, prêt à relire. */
static PhIntegralSnapshot sealed(float value, uint32_t epoch, uint32_t context,
                                 uint8_t streak = 0,
                                 uint8_t schema = kPhIntegralPersistSchema,
                                 uint32_t filtTotalMin = kFiltNow) {
  PhIntegralSnapshot s;
  s.schema = schema;
  s.restoreStreak = streak;
  s.context = context;
  s.epoch = epoch;
  s.value = value;
  s.filtTotalMin = filtTotalMin;
  phPersistSealSnapshot(s);
  return s;
}

/** R3 (feature-093) : fraîcheur avec les deux bases de temps explicites. */
static bool fresh(uint32_t epochWritten, uint32_t epochNow,
                  uint32_t filtWritten = kFiltNow, uint32_t filtNow = kFiltNow) {
  PhPersistAgeInputs in;
  in.epochWritten = epochWritten;
  in.epochNow = epochNow;
  in.filtTotalWritten = filtWritten;
  in.filtTotalNow = filtNow;
  return phPersistFresh(in);
}

static PhIntegralContext ctxRef() {
  PhIntegralContext c;
  c.speedCode = 1;      // `normal`
  c.phPlus = false;     // ph_minus
  c.target = 7.20f;
  c.integralMax = kImax;
  return c;
}

// =============================================================================
// A — INTÉGRITÉ DU BLOB (R2, volet intégrité)
// =============================================================================

// La disposition EST le contrat : 24 octets (schéma 2, feature-093), et un CRC
// qui couvre les 20 octets amont sans se couvrir lui-même. Si la structure grossit sans que le schéma
// change, un ancien enregistrement serait relu avec la mauvaise taille — c'est
// précisément ce que le garde-fou de taille attrape, à condition que la taille
// soit celle qu'on croit.
void test_A_disposition_du_blob(void) {
  TEST_ASSERT_EQUAL_UINT32(24u, (uint32_t)sizeof(PhIntegralSnapshot));
  TEST_ASSERT_EQUAL_UINT32(20u, (uint32_t)offsetof(PhIntegralSnapshot, crc));
  TEST_ASSERT_EQUAL_UINT32(16u, (uint32_t)offsetof(PhIntegralSnapshot, filtTotalMin));
  TEST_ASSERT_EQUAL_UINT32(0u,  (uint32_t)offsetof(PhIntegralSnapshot, schema));
  TEST_ASSERT_EQUAL_UINT32(1u,  (uint32_t)offsetof(PhIntegralSnapshot, restoreStreak));
}

// Vecteurs FNV-1a 32 bits de la littérature. Verrouille l'ALGORITHME lui-même :
// une « optimisation » du hachage (offset basis ou nombre premier retouché)
// invaliderait silencieusement tous les enregistrements du parc au prochain boot
// — panne muette, sans autre symptôme qu'une régulation qui repart de zéro.
void test_A_fnv1a_vecteurs_connus(void) {
  TEST_ASSERT_EQUAL_HEX32(0x811c9dc5u, phPersistFnv1a("", 0));
  TEST_ASSERT_EQUAL_HEX32(0xe40c292cu, phPersistFnv1a("a", 1));
  TEST_ASSERT_EQUAL_HEX32(0xbf9cf968u, phPersistFnv1a("foobar", 6));
}

void test_A_blob_scelle_est_accepte(void) {
  PhIntegralSnapshot s = sealed(1234.5f, kNow, 0xDEADBEEFu, 1);
  TEST_ASSERT_TRUE(phPersistSnapshotIntact(s, sizeof(s)));
}

// Un blob jamais scellé (crc = 0) : c'est le cas d'un secteur NVS effacé, ou
// d'une structure écrite par un code qui aurait oublié le scellement.
void test_A_blob_non_scelle_est_rejete(void) {
  PhIntegralSnapshot s;
  s.schema = kPhIntegralPersistSchema;
  s.epoch = kNow;
  s.value = 1000.0f;
  s.crc = 0;  // pas de phPersistSealSnapshot()
  TEST_ASSERT_FALSE(phPersistSnapshotIntact(s, sizeof(s)));

  PhIntegralSnapshot vierge;  // tout à zéro, y compris le CRC
  TEST_ASSERT_FALSE(phPersistSnapshotIntact(vierge, sizeof(vierge)));
}

// EXHAUSTIF : les 192 bits du blob, un par un. Aucun ne doit passer — ni dans la
// charge utile, ni dans le CRC lui-même. C'est la propriété qui protège d'un
// secteur NVS à demi écrit : un `value` dont l'exposant a basculé, c'est une
// intégrale multipliée par 2^n, donc une dose pré-armée arbitraire.
void test_A_tout_bit_retourne_est_rejete(void) {
  const PhIntegralSnapshot ref = sealed(3210.75f, kNow, 0x12345678u, 2);
  unsigned char bytes[sizeof(PhIntegralSnapshot)];

  int testes = 0;
  for (size_t octet = 0; octet < sizeof(PhIntegralSnapshot); ++octet) {
    for (int bit = 0; bit < 8; ++bit) {
      memcpy(bytes, &ref, sizeof(bytes));
      bytes[octet] ^= (unsigned char)(1u << bit);
      PhIntegralSnapshot mute;
      memcpy(&mute, bytes, sizeof(bytes));

      char msg[96];
      snprintf(msg, sizeof(msg), "octet %u bit %d accepte malgre le flip",
               (unsigned)octet, bit);
      TEST_ASSERT_FALSE_MESSAGE(phPersistSnapshotIntact(mute, sizeof(mute)), msg);
      ++testes;
    }
  }
  TEST_ASSERT_EQUAL_INT(192, testes);
}

// Taille inattendue = enregistrement d'un autre schéma ou tronqué. Rejet NET,
// jamais de réinterprétation partielle : une structure relue à moitié aurait un
// `value` composé pour partie d'octets de pile.
void test_A_troncature_ou_surplus_est_rejete(void) {
  const PhIntegralSnapshot s = sealed(500.0f, kNow, 7u);
  TEST_ASSERT_TRUE(phPersistSnapshotIntact(s, sizeof(s)));
  TEST_ASSERT_FALSE(phPersistSnapshotIntact(s, 0));
  TEST_ASSERT_FALSE(phPersistSnapshotIntact(s, sizeof(s) - 1));
  TEST_ASSERT_FALSE(phPersistSnapshotIntact(s, sizeof(s) - 4));  // blob v0 à 16 o
  TEST_ASSERT_FALSE(phPersistSnapshotIntact(s, sizeof(s) + 1));
  TEST_ASSERT_FALSE(phPersistSnapshotIntact(s, 4096));
}

// Le trou d'alignement DOIT être normalisé : sans cela, deux écritures de la même
// valeur produiraient deux empreintes différentes selon les octets de pile
// résiduels — et une relecture par un autre chemin de code pourrait échouer sans
// qu'aucune donnée utile n'ait changé.
void test_A_scellement_normalise_le_trou_d_alignement(void) {
  PhIntegralSnapshot sale;
  sale.schema = kPhIntegralPersistSchema;
  sale.restoreStreak = 1;
  sale.reserved = 0xBEEFu;  // résidu de pile
  sale.context = 42u;
  sale.epoch = kNow;
  sale.value = 999.0f;
  sale.filtTotalMin = kFiltNow;
  phPersistSealSnapshot(sale);

  TEST_ASSERT_EQUAL_UINT16(0u, sale.reserved);
  const PhIntegralSnapshot propre = sealed(999.0f, kNow, 42u, 1);
  TEST_ASSERT_EQUAL_HEX32(propre.crc, sale.crc);
  TEST_ASSERT_TRUE(phPersistSnapshotIntact(sale, sizeof(sale)));
}

// Le CRC ne dit RIEN du schéma : un enregistrement d'une version future peut être
// parfaitement intact. C'est la décision de restauration qui doit le refuser —
// et l'ordre des contrôles fait que le schéma passe AVANT tout le reste.
void test_A_un_autre_schema_reste_intact_mais_est_refuse(void) {
  const uint8_t autre = (uint8_t)(kPhIntegralPersistSchema + 1);
  PhIntegralSnapshot s = sealed(2000.0f, kNow - 60u, 0xABCDu, 0, autre);
  TEST_ASSERT_TRUE(phPersistSnapshotIntact(s, sizeof(s)));  // intègre…

  const PhPersistRestoreVerdict v =
      phPersistEvaluateRestore(s, 0xABCDu, kNow, kFiltNow, capFor(2.86f).capPhS);
  TEST_ASSERT_FALSE(v.accepted);  // …mais refusé
  TEST_ASSERT_TRUE(v.reject == PhPersistReject::kSchema);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, v.candidate);

  // Schéma 0 (secteur effacé dont le CRC tomberait juste) : même traitement.
  PhIntegralSnapshot zero = sealed(2000.0f, kNow - 60u, 0xABCDu, 0, 0);
  const PhPersistRestoreVerdict vz =
      phPersistEvaluateRestore(zero, 0xABCDu, kNow, kFiltNow, capFor(2.86f).capPhS);
  TEST_ASSERT_TRUE(vz.reject == PhPersistReject::kSchema);
}

// L'ordre des contrôles est un contrat : un enregistrement d'un autre schéma ne
// doit pas être jugé sur son contexte, qui n'a pas le même sens dans ce schéma.
void test_A_l_ordre_des_controles_est_stable(void) {
  // Schéma faux ET streak épuisé ET contexte faux ET périmé → kSchema.
  PhIntegralSnapshot s = sealed(2000.0f, kNow - 100000u, 0xBADu,
                                kPhIntegralRestoreMaxStreak,
                                (uint8_t)(kPhIntegralPersistSchema + 9));
  PhPersistRestoreVerdict v =
      phPersistEvaluateRestore(s, 0u, kNow, kFiltNow, capFor(2.86f).capPhS);
  TEST_ASSERT_TRUE(v.reject == PhPersistReject::kSchema);

  // Schéma bon, streak épuisé ET contexte faux ET périmé → kStreak.
  s = sealed(2000.0f, kNow - 100000u, 0xBADu, kPhIntegralRestoreMaxStreak);
  v = phPersistEvaluateRestore(s, 0u, kNow, kFiltNow, capFor(2.86f).capPhS);
  TEST_ASSERT_TRUE(v.reject == PhPersistReject::kStreak);

  // Streak sain, contexte faux ET périmé → kContext.
  s = sealed(2000.0f, kNow - 100000u, 0xBADu, 0);
  v = phPersistEvaluateRestore(s, 0u, kNow, kFiltNow, capFor(2.86f).capPhS);
  TEST_ASSERT_TRUE(v.reject == PhPersistReject::kContext);

  // Contexte bon, périmé (7 h de filtration) ET valeur nulle → kStale (la
  // péremption prime sur la valeur).
  s = sealed(0.0f, kNow - 100000u, 0xBADu, 0, kPhIntegralPersistSchema,
             kFiltNow - 420u);
  v = phPersistEvaluateRestore(s, 0xBADu, kNow, kFiltNow, capFor(2.86f).capPhS);
  TEST_ASSERT_TRUE(v.reject == PhPersistReject::kStale);
  // Compteur reculé ET valeur nulle → kFiltCounter (l'âge prime aussi).
  s = sealed(0.0f, kNow - 100000u, 0xBADu, 0, kPhIntegralPersistSchema, kFiltNow + 1u);
  v = phPersistEvaluateRestore(s, 0xBADu, kNow, kFiltNow, capFor(2.86f).capPhS);
  TEST_ASSERT_TRUE(v.reject == PhPersistReject::kFiltCounter);
}

// =============================================================================
// B — EMPREINTE DE CONTEXTE (R2)
// =============================================================================
// Une intégrale n'a de sens que pour le quadruplet qui l'a chargée. Le cas le
// plus dangereux est la bascule ph_minus → ph_plus : le signe de l'erreur
// s'inverse, et restaurer une autorité apprise en acide reviendrait à pré-armer
// une dose de BASE. C'est un test de sécurité chimique, pas de hachage.

void test_B_meme_contexte_meme_empreinte(void) {
  TEST_ASSERT_EQUAL_HEX32(phPersistContextHash(ctxRef()),
                          phPersistContextHash(ctxRef()));
}

void test_B_chaque_composante_change_l_empreinte(void) {
  const uint32_t ref = phPersistContextHash(ctxRef());

  PhIntegralContext c = ctxRef();
  c.speedCode = 2;  // normal → fast
  TEST_ASSERT_TRUE(phPersistContextHash(c) != ref);

  c = ctxRef();
  c.phPlus = true;  // ph_minus → ph_plus : INVERSION DU SIGNE DE L'ERREUR
  TEST_ASSERT_TRUE(phPersistContextHash(c) != ref);

  c = ctxRef();
  c.target = 7.30f;  // consigne déplacée
  TEST_ASSERT_TRUE(phPersistContextHash(c) != ref);

  c = ctxRef();
  c.integralMax = kPhIntegralMaxFast;  // échelle de la valeur changée
  TEST_ASSERT_TRUE(phPersistContextHash(c) != ref);

  // Même une retouche fine de consigne (le pas de l'IHM est 0,05) est vue.
  c = ctxRef();
  c.target = 7.25f;
  TEST_ASSERT_TRUE(phPersistContextHash(c) != ref);
}

void test_B_contexte_different_est_refuse(void) {
  const uint32_t ecrit = phPersistContextHash(ctxRef());
  PhIntegralContext apres = ctxRef();
  apres.phPlus = true;

  const PhIntegralSnapshot s = sealed(1000.0f, kNow - 600u, ecrit);
  const PhPersistRestoreVerdict v = phPersistEvaluateRestore(
      s, phPersistContextHash(apres), kNow, kFiltNow, capFor(2.86f).capPhS);
  TEST_ASSERT_FALSE(v.accepted);
  TEST_ASSERT_TRUE(v.reject == PhPersistReject::kContext);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, v.candidate);
}

// =============================================================================
// C — R1 : LE DOUBLE PLAFOND DE RESTAURATION
// =============================================================================
//   plafond = min( 0,30 × integralMax , 0,50 × (charge − kp·stop) / ki )
// Le premier terme est une butée absolue ; le second est la vérification À
// L'EXÉCUTION, sur la charge de CE bassin, que le static_assert de constants.h ne
// peut pas faire (il ne connaît que l'installation de référence).

// Installation de RÉFÉRENCE (2,86 mL/min mesurés) : c'est la CHARGE qui borne,
// pas la butée. 3 753 pH·s, soit 23,2 % du plafond au lieu des 30 % forfaitaires.
void test_C_reference_la_charge_observee_gouverne(void) {
  const PhPersistCapVerdict v = capFor(kPhEquilibriumLoadMlPerMin);

  TEST_ASSERT_FALSE(v.historyShort);
  TEST_ASSERT_TRUE(v.loadGoverns);
  TEST_ASSERT_FLOAT_WITHIN(1.0f, 4860.0f, v.absoluteCapPhS);   // 0,30 × 16 200
  TEST_ASSERT_FLOAT_WITHIN(5.0f, 7506.0f, v.equilibriumPhS);   // I* observé
  TEST_ASSERT_FLOAT_WITHIN(5.0f, 3753.0f, v.loadCapPhS);       // 0,50 × I*
  TEST_ASSERT_FLOAT_WITHIN(5.0f, 3753.0f, v.capPhS);           // le min

  // Le chiffre annoncé au gate : 23,2 % du plafond, 1,39 mL/min d'autorité.
  TEST_ASSERT_FLOAT_WITHIN(0.2f, 23.2f, 100.0f * v.capPhS / kImax);
  TEST_ASSERT_FLOAT_WITHIN(0.02f, 1.39f, kKi * v.capPhS);
}

// À forte charge, la butée absolue reprend la main : elle n'est JAMAIS dépassée,
// quelle que soit la charge observée. C'est ce qui empêche une charge aberrante
// (fuite, sonde folle, historique pollué) de déverrouiller le plafond plein.
void test_C_charge_forte_la_butee_absolue_gouverne(void) {
  const PhPersistCapVerdict v = capFor(10.0f);  // charge délirante
  TEST_ASSERT_FALSE(v.loadGoverns);
  TEST_ASSERT_FLOAT_WITHIN(1.0f, 4860.0f, v.capPhS);
  TEST_ASSERT_EQUAL_FLOAT(v.absoluteCapPhS, v.capPhS);
  TEST_ASSERT_TRUE(v.loadCapPhS > v.absoluteCapPhS);

  // Et une charge 100× plus grande ne déplace pas le plafond d'un pH·s.
  const PhPersistCapVerdict fou = capFor(1000.0f);
  TEST_ASSERT_EQUAL_FLOAT(v.capPhS, fou.capPhS);
}

// La BASCULE entre les deux branches. Elle se produit là où
// 0,50 × I* = 0,30 × integralMax, soit charge = kp·stop + 0,6 × ki × integralMax
// = 0,08 + 3,6 = 3,68 mL/min aux réglages `normal`.
void test_C_bascule_entre_les_deux_branches(void) {
  const float bascule = kKp * kStop + 0.6f * kKi * kImax;
  TEST_ASSERT_FLOAT_WITHIN(0.02f, 3.68f, bascule);

  const PhPersistCapVerdict sous = capFor(bascule - 0.20f);
  TEST_ASSERT_TRUE(sous.loadGoverns);
  TEST_ASSERT_TRUE(sous.capPhS < sous.absoluteCapPhS);

  const PhPersistCapVerdict sur = capFor(bascule + 0.20f);
  TEST_ASSERT_FALSE(sur.loadGoverns);
  TEST_ASSERT_EQUAL_FLOAT(sur.absoluteCapPhS, sur.capPhS);

  // Le plafond est monotone croissant en charge, puis PLAT : pas de discontinuité
  // ni de retour en arrière au franchissement.
  float precedent = 0.0f;
  for (float charge = 0.5f; charge <= 6.0f; charge += 0.10f) {
    const PhPersistCapVerdict v = capFor(charge);
    char msg[96];
    snprintf(msg, sizeof(msg), "charge %.2f : plafond %.0f < precedent %.0f",
             (double)charge, (double)v.capPhS, (double)precedent);
    TEST_ASSERT_TRUE_MESSAGE(v.capPhS >= precedent - 0.5f, msg);
    TEST_ASSERT_TRUE_MESSAGE(v.capPhS <= v.absoluteCapPhS + 0.5f, msg);
    precedent = v.capPhS;
  }
}

// LA PROPRIÉTÉ CHIMIQUE, sur un balayage de charges ET les trois vitesses :
// le plafond retenu reste STRICTEMENT sous le remplissage d'équilibre du bassin
// — au plus sa moitié. C'est la garantie « aucune restauration ne peut, à elle
// seule, produire un surdosage net », vérifiée sur la charge de CE bassin.
void test_C_le_plafond_reste_sous_l_equilibre_pour_toute_charge(void) {
  struct { const char* nom; float kp, ki, imax; } vitesses[] = {
      {"slow",   kPhKpSlow,   kPhKiSlow,   kPhIntegralMaxSlow},
      {"normal", kPhKpNormal, kPhKiNormal, kPhIntegralMaxNormal},
      {"fast",   kPhKpFast,   kPhKiFast,   kPhIntegralMaxFast},
  };
  const float charges[] = {1.0f, 1.5f, 2.0f, 2.86f, 3.5f, 5.0f};

  for (int iv = 0; iv < 3; ++iv) {
    for (int ic = 0; ic < 6; ++ic) {
      const float charge = charges[ic];
      const PhPersistCapVerdict v = phPersistRestoreCap(
          vitesses[iv].imax, vitesses[iv].kp, vitesses[iv].ki, kStop,
          obs(charge, 7));
      const float etoile = equilibriumPhS(charge, vitesses[iv].kp,
                                          vitesses[iv].ki, kStop);
      char msg[144];
      snprintf(msg, sizeof(msg),
               "%s @ %.2f mL/min : plafond %.0f pH.s, equilibre %.0f pH.s",
               vitesses[iv].nom, (double)charge, (double)v.capPhS,
               (double)etoile);

      TEST_ASSERT_TRUE_MESSAGE(v.capPhS > 0.0f, msg);
      // STRICTEMENT sous l'équilibre…
      TEST_ASSERT_TRUE_MESSAGE(v.capPhS < etoile, msg);
      // …et même sous sa MOITIÉ (la fraction de charge vaut 0,50).
      TEST_ASSERT_TRUE_MESSAGE(v.capPhS <= 0.5f * etoile + 1.0f, msg);
      // Exprimé en débit : l'autorité restaurée ne dépasse pas la moitié de ce
      // que le bassin consomme réellement.
      TEST_ASSERT_TRUE_MESSAGE(vitesses[iv].ki * v.capPhS < 0.5f * charge, msg);
    }
  }
}

// ⚠️ SENTINELLE DE FLOTTE — celle qui doit mourir si l'on remplace le min() par
// la seule butée 0,30. Sur un bassin plus petit (1,0 ou 1,5 mL/min), la butée
// forfaitaire passe AU-DESSUS de l'équilibre : elle restaurerait jusqu'à 1,27 fois
// ce que le bassin consomme, exactement le surdosage pré-armé que R1 interdit.
// Le double plafond, lui, reste sous l'équilibre aux trois vitesses.
void test_C_SENTINELLE_la_butee_seule_depasserait_l_equilibre_des_petits_bassins(void) {
  struct { const char* nom; float kp, ki, imax; } vitesses[] = {
      {"slow",   kPhKpSlow,   kPhKiSlow,   kPhIntegralMaxSlow},
      {"normal", kPhKpNormal, kPhKiNormal, kPhIntegralMaxNormal},
      {"fast",   kPhKpFast,   kPhKiFast,   kPhIntegralMaxFast},
  };
  const float petites[] = {1.0f, 1.5f};

  for (int iv = 0; iv < 3; ++iv) {
    for (int ic = 0; ic < 2; ++ic) {
      const float charge = petites[ic];
      const float etoile = equilibriumPhS(charge, vitesses[iv].kp,
                                          vitesses[iv].ki, kStop);
      const float buteeSeule = kPhIntegralRestoreMaxFraction * vitesses[iv].imax;
      const PhPersistCapVerdict v = phPersistRestoreCap(
          vitesses[iv].imax, vitesses[iv].kp, vitesses[iv].ki, kStop,
          obs(charge, 7));
      char msg[160];
      snprintf(msg, sizeof(msg),
               "%s @ %.2f : butee seule %.0f, equilibre %.0f, retenu %.0f",
               vitesses[iv].nom, (double)charge, (double)buteeSeule,
               (double)etoile, (double)v.capPhS);

      // 1. La butée FORFAITAIRE serait bel et bien au-dessus de l'équilibre.
      TEST_ASSERT_TRUE_MESSAGE(buteeSeule > etoile, msg);
      // 2. Le plafond RETENU, lui, est la borne de charge — et il est dessous.
      TEST_ASSERT_TRUE_MESSAGE(v.loadGoverns, msg);
      TEST_ASSERT_TRUE_MESSAGE(v.capPhS < etoile, msg);
      TEST_ASSERT_TRUE_MESSAGE(v.capPhS < buteeSeule, msg);
    }
  }

  // Le chiffre du gate : à 1,5 mL/min le plafond tombe à 11,8 % du plafond
  // d'anti-windup, contre 30 % forfaitaires — et 30 % vaudraient 1,27 × I*.
  const PhPersistCapVerdict petit = capFor(1.5f);
  TEST_ASSERT_FLOAT_WITHIN(0.2f, 11.8f, 100.0f * petit.capPhS / kImax);
  TEST_ASSERT_FLOAT_WITHIN(0.02f, 1.27f,
                           (kPhIntegralRestoreMaxFraction * kImax) /
                               equilibriumPhS(1.5f, kKp, kKi, kStop));
}

// FAIL-CLOSED nº 1 — l'historique. Un appareil neuf ne restaure RIEN. La butée
// absolue est tout de même renseignée (diagnostic), mais le plafond RETENU est
// nul : c'est lui seul qui autorise quoi que ce soit.
void test_C_historique_trop_court_ferme_le_plafond(void) {
  for (uint8_t days = 0; days < kPhIntegralRestoreMinHistoryDays; ++days) {
    const PhPersistCapVerdict v = capFor(kPhEquilibriumLoadMlPerMin, days);
    char msg[96];
    snprintf(msg, sizeof(msg), "%u jour(s) : plafond %.0f attendu nul",
             (unsigned)days, (double)v.capPhS);
    TEST_ASSERT_TRUE_MESSAGE(v.historyShort, msg);
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(0.0f, v.capPhS, msg);
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(0.0f, v.loadCapPhS, msg);
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(0.0f, v.equilibriumPhS, msg);
    TEST_ASSERT_TRUE_MESSAGE(v.absoluteCapPhS > 0.0f, msg);  // diagnostic seul
  }
  // 2 jours d'historique ⇒ 0, le chiffre cité au gate.
  TEST_ASSERT_EQUAL_FLOAT(0.0f, capFor(kPhEquilibriumLoadMlPerMin, 2).capPhS);

  // Au minimum requis, le plafond s'ouvre.
  const PhPersistCapVerdict ok =
      capFor(kPhEquilibriumLoadMlPerMin, kPhIntegralRestoreMinHistoryDays);
  TEST_ASSERT_FALSE(ok.historyShort);
  TEST_ASSERT_TRUE(ok.capPhS > 0.0f);
}

// FAIL-CLOSED nº 2 — réglages inertes ou absurdes, et arithmétique dégénérée.
void test_C_reglages_inertes_ferment_le_plafond(void) {
  const PhPersistLoadObservation bonne = obs(kPhEquilibriumLoadMlPerMin, 7);

  TEST_ASSERT_EQUAL_FLOAT(0.0f, phPersistRestoreCap(0.0f, kKp, kKi, kStop, bonne).capPhS);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, phPersistRestoreCap(-1.0f, kKp, kKi, kStop, bonne).capPhS);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, phPersistRestoreCap(NAN, kKp, kKi, kStop, bonne).capPhS);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, phPersistRestoreCap(INFINITY, kKp, kKi, kStop, bonne).capPhS);
  // ki inerte : le mode « P pur » — l'intégrale n'existe pas, rien à restaurer.
  TEST_ASSERT_EQUAL_FLOAT(0.0f, phPersistRestoreCap(kImax, kKp, 0.0f, kStop, bonne).capPhS);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, phPersistRestoreCap(kImax, kKp, -1e-4f, kStop, bonne).capPhS);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, phPersistRestoreCap(kImax, kKp, NAN, kStop, bonne).capPhS);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, phPersistRestoreCap(kImax, -1.0f, kKi, kStop, bonne).capPhS);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, phPersistRestoreCap(kImax, NAN, kKi, kStop, bonne).capPhS);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, phPersistRestoreCap(kImax, kKp, kKi, -0.1f, bonne).capPhS);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, phPersistRestoreCap(kImax, kKp, kKi, NAN, bonne).capPhS);
  // Charge non exploitable.
  TEST_ASSERT_EQUAL_FLOAT(0.0f, capFor(0.0f).capPhS);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, capFor(-2.0f).capPhS);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, capFor(NAN).capPhS);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, capFor(INFINITY).capPhS);
}

// FAIL-CLOSED nº 3 — la charge n'excède même pas ce que le terme proportionnel
// fournit au seuil d'arrêt : l'intégrale n'a rien à apprendre, donc rien à
// restaurer. Frontière exacte à charge = kp × stop = 0,08 mL/min.
void test_C_charge_sous_l_apport_proportionnel_ferme_le_plafond(void) {
  const float frontiere = kKp * kStop;  // 0,08 mL/min
  TEST_ASSERT_EQUAL_FLOAT(0.0f, capFor(frontiere).capPhS);          // I* = 0
  TEST_ASSERT_EQUAL_FLOAT(0.0f, capFor(frontiere - 0.01f).capPhS);  // I* < 0
  TEST_ASSERT_TRUE(capFor(frontiere + 0.05f).capPhS > 0.0f);
}

// L'ÉCRÊTAGE proprement dit — la fonction que la mutation « suppression du
// clamp » supprime. Elle n'était couverte par AUCUN test avant l'extraction.
void test_C_ecretage_borne_la_valeur_au_plafond(void) {
  const float cap = 3753.0f;
  TEST_ASSERT_EQUAL_FLOAT(cap, phPersistClampRestore(10000.0f, cap));  // écrêtée
  TEST_ASSERT_EQUAL_FLOAT(cap, phPersistClampRestore(kImax, cap));     // plafond plein
  TEST_ASSERT_EQUAL_FLOAT(cap, phPersistClampRestore(cap, cap));       // pile
  TEST_ASSERT_EQUAL_FLOAT(1000.0f, phPersistClampRestore(1000.0f, cap));  // intacte

  // Rien à restaurer — y compris pour une valeur INFINIE : elle n'est PAS
  // ramenée au plafond mais annulée (fail-closed). Un ±inf en NVS signale une
  // corruption ou un calcul dégénéré, pas « beaucoup d'intégrale ».
  TEST_ASSERT_EQUAL_FLOAT(0.0f, phPersistClampRestore(INFINITY, cap));
  TEST_ASSERT_EQUAL_FLOAT(0.0f, phPersistClampRestore(-INFINITY, cap));
  TEST_ASSERT_EQUAL_FLOAT(0.0f, phPersistClampRestore(1000.0f, 0.0f));
  TEST_ASSERT_EQUAL_FLOAT(0.0f, phPersistClampRestore(1000.0f, -5.0f));
  TEST_ASSERT_EQUAL_FLOAT(0.0f, phPersistClampRestore(1000.0f, NAN));
  TEST_ASSERT_EQUAL_FLOAT(0.0f, phPersistClampRestore(1000.0f, INFINITY));
  TEST_ASSERT_EQUAL_FLOAT(0.0f, phPersistClampRestore(0.0f, cap));
  TEST_ASSERT_EQUAL_FLOAT(0.0f, phPersistClampRestore(-100.0f, cap));
  TEST_ASSERT_EQUAL_FLOAT(0.0f, phPersistClampRestore(NAN, cap));
}

// L'écrêtage DANS la décision : une valeur écrite avant un changement de réglages
// (donc énorme au regard du plafond courant) est acceptée, mais RAMENÉE au
// plafond. C'est le chemin nominal de la protection.
void test_C_une_valeur_excessive_est_acceptee_mais_ramenee_au_plafond(void) {
  const float cap = capFor(kPhEquilibriumLoadMlPerMin).capPhS;
  const PhIntegralSnapshot s = sealed(kImax, kNow - 300u, 0x5A5Au);  // plafond plein

  const PhPersistRestoreVerdict v =
      phPersistEvaluateRestore(s, 0x5A5Au, kNow, kFiltNow, cap);
  TEST_ASSERT_TRUE(v.accepted);
  TEST_ASSERT_FLOAT_WITHIN(1.0f, cap, v.candidate);
  TEST_ASSERT_TRUE(v.candidate < 0.30f * kImax);  // strictement sous la butée
  TEST_ASSERT_EQUAL_UINT32(300u, v.ageS);
}

// Plafond nul (cas de PRODUCTION aujourd'hui : days = 0) : l'instantané est
// parfaitement valide et il n'est PAS restauré. Rejet volontairement silencieux.
void test_C_plafond_nul_refuse_une_restauration_par_ailleurs_valide(void) {
  const PhIntegralSnapshot s = sealed(1000.0f, kNow - 300u, 0x5A5Au);
  const PhPersistRestoreVerdict v = phPersistEvaluateRestore(s, 0x5A5Au, kNow, kFiltNow, 0.0f);
  TEST_ASSERT_FALSE(v.accepted);
  TEST_ASSERT_TRUE(v.reject == PhPersistReject::kNoCap);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, v.candidate);
  TEST_ASSERT_NULL(phPersistRejectLabel(v.reject));  // silence assumé
}

// Valeur non exploitable dans un blob par ailleurs intègre (NaN ou ±inf produits
// par un calcul dégénéré avant l'écriture, valeur négative d'un schéma futur…).
// Aucune n'est « rattrapée » par l'écrêtage : elles sont REFUSÉES. Un ±inf relu
// signale une corruption, pas une intégrale très grande.
void test_C_valeur_nulle_ou_non_finie_est_refusee(void) {
  const float cap = capFor(kPhEquilibriumLoadMlPerMin).capPhS;
  const float mauvaises[] = {0.0f, -1.0f, -kImax, NAN, INFINITY, -INFINITY};
  for (int i = 0; i < 6; ++i) {
    const PhIntegralSnapshot s = sealed(mauvaises[i], kNow - 60u, 0x1u);
    const PhPersistRestoreVerdict v = phPersistEvaluateRestore(s, 0x1u, kNow, kFiltNow, cap);
    char msg[96];
    snprintf(msg, sizeof(msg), "valeur %.1f acceptee", (double)mauvaises[i]);
    TEST_ASSERT_FALSE_MESSAGE(v.accepted, msg);
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(0.0f, v.candidate, msg);
    TEST_ASSERT_TRUE_MESSAGE(v.reject == PhPersistReject::kValue, msg);
  }
}

// =============================================================================
// D — R3 : PÉREMPTION
// =============================================================================

// feature-093 : l'âge se mesure en TEMPS DE FILTRATION — 6 h de filtration
// accumulée depuis l'écriture (359 min → frais, 361 min → périmé), l'heure murale
// n'étant qu'un plafond de sécurité (72 h).
void test_D_frais_a_5h59_perime_a_6h01(void) {
  const uint32_t ecrit = kFiltNow - kPhIntegralPersistMaxFiltMin;  // 6 h pile
  TEST_ASSERT_TRUE(fresh(kNow - 60u, kNow, ecrit + 1u, kFiltNow));   // 5 h 59 → frais
  TEST_ASSERT_FALSE(fresh(kNow - 60u, kNow, ecrit - 1u, kFiltNow));  // 6 h 01 → périmé
  // Le cas du 03/09/2026 : écrit la veille 15:00 (17 h murales), 4 h de
  // filtration depuis → FRAIS. L'ancien critère mural (6 h) le rejetait.
  TEST_ASSERT_TRUE(fresh(kNow - 17u * 3600u, kNow, kFiltNow - 240u, kFiltNow));
  // Une nuit ENTIÈRE sans filtration ne vieillit pas la valeur : 0 min de
  // filtration accumulée, 14 h murales → frais.
  TEST_ASSERT_TRUE(fresh(kNow - 14u * 3600u, kNow, kFiltNow, kFiltNow));
}

// Frontières EXACTES : les deux âges maximaux sont inclusifs (<=), une unité de
// plus ne l'est pas — en minutes de filtration comme en secondes murales.
void test_D_frontiere_exacte_de_la_peremption(void) {
  TEST_ASSERT_TRUE(fresh(kNow, kNow, kFiltNow - kPhIntegralPersistMaxFiltMin, kFiltNow));
  TEST_ASSERT_FALSE(fresh(kNow, kNow, kFiltNow - kPhIntegralPersistMaxFiltMin - 1u, kFiltNow));
  TEST_ASSERT_TRUE(fresh(kNow - kPhIntegralPersistMaxWallAgeS, kNow));
  TEST_ASSERT_FALSE(fresh(kNow - kPhIntegralPersistMaxWallAgeS - 1u, kNow));
  TEST_ASSERT_TRUE(fresh(kNow, kNow));  // âges nuls
  // Compteur de filtration courant INFÉRIEUR à celui écrit (NVS effacée) : âge
  // incalculable ⇒ périmé, avec son propre motif.
  TEST_ASSERT_FALSE(fresh(kNow, kNow, kFiltNow + 1u, kFiltNow));
  PhPersistAgeInputs in;
  in.epochWritten = kNow; in.epochNow = kNow;
  in.filtTotalWritten = kFiltNow + 1u; in.filtTotalNow = kFiltNow;
  TEST_ASSERT_TRUE(phPersistAge(in).reject == PhPersistReject::kFiltCounter);
}

// Sans heure murale exploitable, l'âge est INCALCULABLE — et un âge incalculable
// vaut un âge périmé. C'est le cas d'un boot sans NTP ni RTC.
void test_D_horloge_inexploitable_vaut_perime(void) {
  TEST_ASSERT_FALSE(phPersistClockUsable(0));
  TEST_ASSERT_FALSE(phPersistClockUsable(1u));
  TEST_ASSERT_FALSE(phPersistClockUsable((uint32_t)kMinValidEpoch - 1u));
  TEST_ASSERT_TRUE(phPersistClockUsable((uint32_t)kMinValidEpoch));
  TEST_ASSERT_TRUE(phPersistClockUsable(kNow));

  // Même un instantané écrit à l'instant est refusé si l'horloge est inutilisable.
  TEST_ASSERT_FALSE(fresh(1000u, 1000u));
  TEST_ASSERT_FALSE(fresh(kNow, 12345u));
}

// Horodatage nul (champ jamais posé) ou situé dans le FUTUR (horloge reculée
// après une resynchronisation) : traités comme incohérents, pas comme « très
// frais » — sans quoi une horloge en avance rendrait un instantané éternel.
void test_D_horodatage_nul_ou_futur_est_incoherent(void) {
  TEST_ASSERT_FALSE(fresh(0u, kNow));
  TEST_ASSERT_FALSE(fresh(kNow + 1u, kNow));
  TEST_ASSERT_FALSE(fresh(kNow + 86400u, kNow));
}

void test_D_peremption_dans_la_decision(void) {
  const float cap = capFor(kPhEquilibriumLoadMlPerMin).capPhS;
  const uint32_t ctx = 0xC0FFEEu;

  // 5 h 59 de filtration (et 20 h murales) → accepté ; les deux âges sont rendus.
  PhIntegralSnapshot s = sealed(1000.0f, kNow - 20u * 3600u, ctx, 0,
                                kPhIntegralPersistSchema,
                                kFiltNow - (kPhIntegralPersistMaxFiltMin - 1u));
  PhPersistRestoreVerdict v = phPersistEvaluateRestore(s, ctx, kNow, kFiltNow, cap);
  TEST_ASSERT_TRUE(v.accepted);
  TEST_ASSERT_EQUAL_UINT32(kPhIntegralPersistMaxFiltMin - 1u, v.filtAgeMin);
  TEST_ASSERT_EQUAL_UINT32(20u * 3600u, v.ageS);

  // 6 h 01 de filtration → kStale, et l'âge est tout de même renseigné.
  s = sealed(1000.0f, kNow - 60u, ctx, 0, kPhIntegralPersistSchema,
             kFiltNow - (kPhIntegralPersistMaxFiltMin + 1u));
  v = phPersistEvaluateRestore(s, ctx, kNow, kFiltNow, cap);
  TEST_ASSERT_FALSE(v.accepted);
  TEST_ASSERT_TRUE(v.reject == PhPersistReject::kStale);
  TEST_ASSERT_EQUAL_UINT32(kPhIntegralPersistMaxFiltMin + 1u, v.filtAgeMin);
  TEST_ASSERT_NOT_NULL(phPersistRejectLabel(v.reject));

  // Plafond mural de sécurité : 0 min de filtration mais 72 h + 1 s → kStale.
  s = sealed(1000.0f, kNow - (kPhIntegralPersistMaxWallAgeS + 1u), ctx);
  v = phPersistEvaluateRestore(s, ctx, kNow, kFiltNow, cap);
  TEST_ASSERT_TRUE(v.reject == PhPersistReject::kStale);

  // Compteur reculé → kFiltCounter (journalisable).
  s = sealed(1000.0f, kNow - 60u, ctx, 0, kPhIntegralPersistSchema, kFiltNow + 5u);
  v = phPersistEvaluateRestore(s, ctx, kNow, kFiltNow, cap);
  TEST_ASSERT_TRUE(v.reject == PhPersistReject::kFiltCounter);
  TEST_ASSERT_NOT_NULL(phPersistRejectLabel(v.reject));

  // Horloge inexploitable / epoch nul / epoch futur → kEpoch.
  s = sealed(1000.0f, kNow - 60u, ctx);
  v = phPersistEvaluateRestore(s, ctx, 12345u, kFiltNow, cap);
  TEST_ASSERT_TRUE(v.reject == PhPersistReject::kEpoch);

  s = sealed(1000.0f, 0u, ctx);
  v = phPersistEvaluateRestore(s, ctx, kNow, kFiltNow, cap);
  TEST_ASSERT_TRUE(v.reject == PhPersistReject::kEpoch);

  s = sealed(1000.0f, kNow + 3600u, ctx);
  v = phPersistEvaluateRestore(s, ctx, kNow, kFiltNow, cap);
  TEST_ASSERT_TRUE(v.reject == PhPersistReject::kEpoch);
}

// =============================================================================
// E — R4 : CONFIRMATION DIFFÉRÉE PAR LA MESURE
// =============================================================================
// La valeur relue n'est JAMAIS appliquée au boot. Elle attend que le bassin
// confirme lui-même qu'une charge acide est présente : erreur au-dessus du seuil
// de DÉMARRAGE pendant 5 min CONSÉCUTIVES de temps de branche.

static PhPersistConfirmInputs confirmIn(float error, uint32_t dtMs = 10000u,
                                        float cap = 4000.0f,
                                        uint32_t epochNow = kNow,
                                        uint32_t filtNow = kFiltNow) {
  PhPersistConfirmInputs in;
  in.epochNow = epochNow;
  in.filtTotalNow = filtNow;
  in.dtMs = dtMs;
  in.error = error;
  in.startThreshold = kStart;
  in.capPhS = cap;
  return in;
}

static PhPersistConfirmState armed(float candidate = 3000.0f,
                                   uint32_t epoch = kNow - 600u,
                                   uint32_t filtTotalMin = kFiltNow) {
  PhPersistConfirmState st;
  st.candidate = candidate;
  st.epoch = epoch;
  st.filtTotalMin = filtTotalMin;
  st.heldMs = 0;
  return st;
}

void test_E_sans_candidate_la_machine_est_inerte(void) {
  PhPersistConfirmState st;  // aucune candidate
  const PhPersistConfirmVerdict v = phPersistConfirmTick(st, confirmIn(1.0f));
  TEST_ASSERT_TRUE(v.outcome == PhPersistConfirmOutcome::kIdle);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, v.restored);
  TEST_ASSERT_EQUAL_UINT32(0u, st.heldMs);
}

// Le chemin nominal : 5 minutes continues, pas une de moins.
void test_E_confirmation_apres_cinq_minutes_continues(void) {
  PhPersistConfirmState st = armed(3000.0f);
  const uint32_t pas = 10000u;
  const int ticksRequis = (int)(kPhIntegralRestoreConfirmMs / pas);  // 30

  for (int i = 1; i < ticksRequis; ++i) {
    const PhPersistConfirmVerdict v =
        phPersistConfirmTick(st, confirmIn(kStart + 0.10f, pas));
    char msg[96];
    snprintf(msg, sizeof(msg), "tick %d/%d : applique trop tot", i, ticksRequis);
    TEST_ASSERT_TRUE_MESSAGE(v.outcome == PhPersistConfirmOutcome::kWaiting, msg);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE((uint32_t)i * pas, st.heldMs, msg);
  }

  const PhPersistConfirmVerdict v =
      phPersistConfirmTick(st, confirmIn(kStart + 0.10f, pas));
  TEST_ASSERT_TRUE(v.outcome == PhPersistConfirmOutcome::kApplied);
  TEST_ASSERT_EQUAL_FLOAT(3000.0f, v.restored);
  // La candidate est CONSOMMÉE : pas de seconde application.
  TEST_ASSERT_EQUAL_FLOAT(0.0f, st.candidate);
  const PhPersistConfirmVerdict apres =
      phPersistConfirmTick(st, confirmIn(kStart + 0.10f, pas));
  TEST_ASSERT_TRUE(apres.outcome == PhPersistConfirmOutcome::kIdle);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, apres.restored);
}

// L'écart qui retombe ROMPT la continuité : le compteur repart de ZÉRO, pas de
// cumul par morceaux. Un bassin qui oscille autour du seuil ne confirme rien.
void test_E_ecart_retombe_remet_la_tenue_a_zero(void) {
  PhPersistConfirmState st = armed();
  const uint32_t pas = 10000u;

  for (int i = 0; i < 20; ++i) phPersistConfirmTick(st, confirmIn(kStart + 0.10f, pas));
  TEST_ASSERT_EQUAL_UINT32(20u * pas, st.heldMs);

  // Un seul tick sous le seuil de démarrage suffit.
  const PhPersistConfirmVerdict v = phPersistConfirmTick(st, confirmIn(0.0f, pas));
  TEST_ASSERT_TRUE(v.outcome == PhPersistConfirmOutcome::kWaiting);
  TEST_ASSERT_EQUAL_UINT32(0u, st.heldMs);
  TEST_ASSERT_EQUAL_FLOAT(3000.0f, st.candidate);  // la candidate SURVIT

  // …et il faut de nouveau la tenue COMPLÈTE.
  for (int i = 1; i < (int)(kPhIntegralRestoreConfirmMs / pas); ++i) {
    const PhPersistConfirmVerdict w =
        phPersistConfirmTick(st, confirmIn(kStart + 0.10f, pas));
    TEST_ASSERT_TRUE(w.outcome == PhPersistConfirmOutcome::kWaiting);
  }
  const PhPersistConfirmVerdict fin =
      phPersistConfirmTick(st, confirmIn(kStart + 0.10f, pas));
  TEST_ASSERT_TRUE(fin.outcome == PhPersistConfirmOutcome::kApplied);
}

// L'exigence porte sur le seuil de DÉMARRAGE, strictement : à l'égalité, on ne
// compte pas. Un pH déjà à sa consigne au retour du courant ne doit jamais
// déclencher l'application — c'est l'objection « dose pré-armée » de l'ADR-0052.
void test_E_ecart_egal_au_seuil_ou_negatif_ne_compte_pas(void) {
  const float ecarts[] = {kStart, kStart - 0.001f, 0.0f, -0.20f, NAN};
  for (int i = 0; i < 5; ++i) {
    PhPersistConfirmState st = armed();
    for (int t = 0; t < 100; ++t) {  // 1 000 s ≫ 300 s
      const PhPersistConfirmVerdict v =
          phPersistConfirmTick(st, confirmIn(ecarts[i], 10000u));
      TEST_ASSERT_TRUE(v.outcome == PhPersistConfirmOutcome::kWaiting);
    }
    char msg[96];
    snprintf(msg, sizeof(msg), "ecart %.3f a fait murir la tenue", (double)ecarts[i]);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0u, st.heldMs, msg);
    TEST_ASSERT_TRUE_MESSAGE(st.candidate > 0.0f, msg);
  }
}

// Les dt aberrants ne sont JAMAIS crédités (ni synthétisés) : c'est la même base
// de temps que la charge de l'intégrale. Un trou d'évaluation ne doit pas faire
// mûrir une confirmation « en dormant ».
void test_E_dt_aberrant_n_est_jamais_credite(void) {
  PhPersistConfirmState st = armed();
  for (int i = 0; i < 200; ++i) {  // 200 × 30 s = 100 min de temps MURAL
    const PhPersistConfirmVerdict v =
        phPersistConfirmTick(st, confirmIn(kStart + 0.50f, 30000u));
    TEST_ASSERT_TRUE(v.outcome == PhPersistConfirmOutcome::kWaiting);
  }
  TEST_ASSERT_EQUAL_UINT32(0u, st.heldMs);

  // dt nul : idem.
  PhPersistConfirmState st0 = armed();
  for (int i = 0; i < 200; ++i) phPersistConfirmTick(st0, confirmIn(kStart + 0.50f, 0u));
  TEST_ASSERT_EQUAL_UINT32(0u, st0.heldMs);

  // dt EXACTEMENT au maximum : crédité (frontière inclusive).
  PhPersistConfirmState stMax = armed();
  phPersistConfirmTick(stMax, confirmIn(kStart + 0.50f, kPhIntegralMaxTickDtMs));
  TEST_ASSERT_EQUAL_UINT32(kPhIntegralMaxTickDtMs, stMax.heldMs);
  phPersistConfirmTick(stMax, confirmIn(kStart + 0.50f, kPhIntegralMaxTickDtMs + 1u));
  TEST_ASSERT_EQUAL_UINT32(kPhIntegralMaxTickDtMs, stMax.heldMs);  // inchangé
}

// Interruption ≠ abandon. L'une rompt la continuité (mode manuel, garde
// bloquante, mesure invalide) ; l'autre détruit la candidate (changement de
// vitesse, bascule de produit, passage en mode Programmée).
void test_E_interruption_conserve_la_candidate_abandon_la_detruit(void) {
  PhPersistConfirmState st = armed();
  for (int i = 0; i < 25; ++i) phPersistConfirmTick(st, confirmIn(kStart + 0.10f));
  TEST_ASSERT_EQUAL_UINT32(250000u, st.heldMs);

  phPersistConfirmInterrupt(st);
  TEST_ASSERT_EQUAL_UINT32(0u, st.heldMs);
  TEST_ASSERT_EQUAL_FLOAT(3000.0f, st.candidate);
  TEST_ASSERT_EQUAL_UINT32(kNow - 600u, st.epoch);

  for (int i = 0; i < 25; ++i) phPersistConfirmTick(st, confirmIn(kStart + 0.10f));
  phPersistConfirmAbandon(st);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, st.candidate);
  TEST_ASSERT_EQUAL_UINT32(0u, st.heldMs);
  TEST_ASSERT_EQUAL_UINT32(0u, st.epoch);
  const PhPersistConfirmVerdict v = phPersistConfirmTick(st, confirmIn(kStart + 0.10f));
  TEST_ASSERT_TRUE(v.outcome == PhPersistConfirmOutcome::kIdle);
}

// R3 est RÉÉVALUÉE à chaque tick : une candidate qui attend sa confirmation peut
// mourir de vieillesse. Elle n'est pas gelée à son âge de lecture.
void test_E_candidate_perimee_avant_confirmation_est_abandonnee(void) {
  // Écrite il y a 5 h 59 de filtration.
  PhPersistConfirmState st =
      armed(3000.0f, kNow - 600u, kFiltNow - (kPhIntegralPersistMaxFiltMin - 1u));
  // Encore fraîche : la tenue court.
  PhPersistConfirmVerdict v = phPersistConfirmTick(st, confirmIn(kStart + 0.10f));
  TEST_ASSERT_TRUE(v.outcome == PhPersistConfirmOutcome::kWaiting);

  // 2 minutes de FILTRATION plus tard, elle a dépassé les 6 h.
  v = phPersistConfirmTick(st, confirmIn(kStart + 0.10f, 10000u, 4000.0f, kNow + 120u,
                                         kFiltNow + 2u));
  TEST_ASSERT_TRUE(v.outcome == PhPersistConfirmOutcome::kExpired);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, v.restored);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, st.candidate);  // détruite

  // Une horloge devenue inexploitable produit le même abandon.
  PhPersistConfirmState st2 = armed();
  const PhPersistConfirmVerdict w =
      phPersistConfirmTick(st2, confirmIn(kStart + 0.10f, 10000u, 4000.0f, 42u));
  TEST_ASSERT_TRUE(w.outcome == PhPersistConfirmOutcome::kExpired);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, st2.candidate);
}

// R1 est RÉAPPLIQUÉE à l'instant de l'application, sur le plafond COURANT : entre
// la lecture NVS et la confirmation, la vitesse de régulation ou la charge
// observée ont pu changer. Le plafond photographié au boot ne fait pas foi.
void test_E_le_plafond_courant_est_reapplique_a_l_application(void) {
  PhPersistConfirmState st = armed(4000.0f);
  const uint32_t pas = 10000u;
  const int ticks = (int)(kPhIntegralRestoreConfirmMs / pas);
  PhPersistConfirmVerdict v;
  for (int i = 0; i < ticks; ++i) {
    v = phPersistConfirmTick(st, confirmIn(kStart + 0.10f, pas, /*cap*/1000.0f));
  }
  TEST_ASSERT_TRUE(v.outcome == PhPersistConfirmOutcome::kApplied);
  TEST_ASSERT_EQUAL_FLOAT(1000.0f, v.restored);  // écrêtée au plafond COURANT
}

// Plafond courant devenu NUL au moment de confirmer (la charge observée a
// disparu, l'historique s'est vidé, ki est passé à zéro) : rien n'est appliqué et
// la candidate est détruite. Le verdict porte son propre code — kNoCap et non
// kExpired : les deux valent abandon, mais journaliser « périmé » un refus de
// PLAFOND enverrait le diagnostic sur l'horloge au lieu de la chimie.
void test_E_plafond_courant_nul_annule_la_candidate_avec_son_propre_code(void) {
  PhPersistConfirmState st = armed(4000.0f);
  const uint32_t pas = 10000u;
  PhPersistConfirmVerdict v;
  for (int i = 0; i < (int)(kPhIntegralRestoreConfirmMs / pas); ++i) {
    v = phPersistConfirmTick(st, confirmIn(kStart + 0.10f, pas, /*cap*/0.0f));
  }
  TEST_ASSERT_TRUE(v.outcome == PhPersistConfirmOutcome::kNoCap);
  TEST_ASSERT_FALSE(v.outcome == PhPersistConfirmOutcome::kExpired);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, v.restored);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, st.candidate);
}

// Scénario complet, bout en bout : instantané écrit 10 min avant la coupure, boot,
// évaluation, puis confirmation par une mesure soutenue. C'est le seul chemin par
// lequel de l'autorité de dosage peut être pré-armée — il est ici entièrement
// déroulé, plafond de la CHARGE OBSERVÉE compris.
void test_E_scenario_complet_de_la_lecture_a_l_application(void) {
  const uint32_t ctx = phPersistContextHash(ctxRef());
  const PhPersistCapVerdict cap = capFor(kPhEquilibriumLoadMlPerMin);
  const PhIntegralSnapshot s = sealed(/*value*/6000.0f, kNow - 600u, ctx);

  TEST_ASSERT_TRUE(phPersistSnapshotIntact(s, sizeof(s)));
  const PhPersistRestoreVerdict v =
      phPersistEvaluateRestore(s, ctx, kNow, kFiltNow, cap.capPhS);
  TEST_ASSERT_TRUE(v.accepted);
  TEST_ASSERT_FLOAT_WITHIN(5.0f, 3753.0f, v.candidate);  // écrêtée à 23,2 %

  PhPersistConfirmState st;
  st.candidate = v.candidate;
  st.epoch = s.epoch;
  st.filtTotalMin = s.filtTotalMin;
  PhPersistConfirmVerdict cv;
  uint32_t epochNow = kNow;
  for (int i = 0; i < (int)(kPhIntegralRestoreConfirmMs / 10000u); ++i) {
    epochNow += 10u;
    cv = phPersistConfirmTick(st, confirmIn(kStart + 0.10f, 10000u, cap.capPhS,
                                            epochNow));
  }
  TEST_ASSERT_TRUE(cv.outcome == PhPersistConfirmOutcome::kApplied);
  TEST_ASSERT_FLOAT_WITHIN(5.0f, 3753.0f, cv.restored);
  // Et l'autorité ainsi rendue reste sous la moitié de la charge du bassin.
  TEST_ASSERT_TRUE(kKi * cv.restored < 0.5f * kPhEquilibriumLoadMlPerMin);
}

// =============================================================================
// F — R7 : ANTI-BOUCLE DE DÉMARRAGE
// =============================================================================
// Motif : le PANIC du 29/08 (bug-080) n'est pas élucidé. Une boucle de
// redémarrage ne doit pas recharger indéfiniment la même dose.

void test_F_desarmement_au_troisieme(void) {
  TEST_ASSERT_EQUAL_UINT8(3, kPhIntegralRestoreMaxStreak);
  TEST_ASSERT_FALSE(phPersistStreakExhausted(0));
  TEST_ASSERT_FALSE(phPersistStreakExhausted(1));
  TEST_ASSERT_FALSE(phPersistStreakExhausted(2));
  TEST_ASSERT_TRUE(phPersistStreakExhausted(3));
  TEST_ASSERT_TRUE(phPersistStreakExhausted(4));
  TEST_ASSERT_TRUE(phPersistStreakExhausted(255));
}

// L'incrément est SATURÉ : un repliement à 255 réarmerait silencieusement la
// persistance, c'est-à-dire annulerait R7 au bout de 253 reboots de plus.
void test_F_increment_sature_a_255(void) {
  TEST_ASSERT_EQUAL_UINT8(1, phPersistNextStreak(0));
  TEST_ASSERT_EQUAL_UINT8(3, phPersistNextStreak(2));
  TEST_ASSERT_EQUAL_UINT8(255, phPersistNextStreak(254));
  TEST_ASSERT_EQUAL_UINT8(255, phPersistNextStreak(255));  // pas de repliement

  uint8_t s = 250;
  for (int i = 0; i < 50; ++i) s = phPersistNextStreak(s);
  TEST_ASSERT_EQUAL_UINT8(255, s);
  TEST_ASSERT_TRUE(phPersistStreakExhausted(s));
}

// La BOUCLE DE BOOT, telle que la coquille l'exécute : chaque démarrage qui
// TENTE une restauration incrémente le compteur AVANT toute confirmation, et
// réécrit l'instantané. Au quatrième, plus rien n'est restauré.
void test_F_boucle_de_boot_trois_essais_puis_refus(void) {
  const uint32_t ctx = 0xB007u;
  const float cap = capFor(kPhEquilibriumLoadMlPerMin).capPhS;
  PhIntegralSnapshot nvs = sealed(3000.0f, kNow - 60u, ctx, 0);

  int acceptes = 0;
  for (int boot = 1; boot <= 6; ++boot) {
    TEST_ASSERT_TRUE(phPersistSnapshotIntact(nvs, sizeof(nvs)));
    const PhPersistRestoreVerdict v = phPersistEvaluateRestore(nvs, ctx, kNow, kFiltNow, cap);
    char msg[96];
    snprintf(msg, sizeof(msg), "boot %d : streak %u, accepte %d", boot,
             (unsigned)nvs.restoreStreak, (int)v.accepted);
    if (boot <= kPhIntegralRestoreMaxStreak) {
      TEST_ASSERT_TRUE_MESSAGE(v.accepted, msg);
      ++acceptes;
      // Ce que fait la coquille : incrément SATURÉ puis réécriture.
      nvs.restoreStreak = phPersistNextStreak(nvs.restoreStreak);
      phPersistSealSnapshot(nvs);
    } else {
      TEST_ASSERT_FALSE_MESSAGE(v.accepted, msg);
      TEST_ASSERT_TRUE_MESSAGE(v.reject == PhPersistReject::kStreak, msg);
      TEST_ASSERT_EQUAL_FLOAT_MESSAGE(0.0f, v.candidate, msg);
    }
  }
  TEST_ASSERT_EQUAL_INT(kPhIntegralRestoreMaxStreak, acceptes);
}

// Une ÉCRITURE NORMALE réarme la persistance : c'est la preuve que l'appareil a
// tourné assez longtemps pour produire une intégrale, donc qu'il ne boucle pas.
// (La coquille pose restoreStreak = 0 dans tickPhIntegralPersistence.)
void test_F_une_ecriture_normale_reamorce_la_persistance(void) {
  const uint32_t ctx = 0xB007u;
  const float cap = capFor(kPhEquilibriumLoadMlPerMin).capPhS;

  PhIntegralSnapshot bloque = sealed(3000.0f, kNow - 60u, ctx,
                                     kPhIntegralRestoreMaxStreak);
  TEST_ASSERT_TRUE(
      phPersistEvaluateRestore(bloque, ctx, kNow, kFiltNow, cap).reject == PhPersistReject::kStreak);

  // Écriture normale : streak remis à zéro (R7).
  const PhIntegralSnapshot apresEcriture = sealed(3200.0f, kNow - 10u, ctx, 0);
  const PhPersistRestoreVerdict v =
      phPersistEvaluateRestore(apresEcriture, ctx, kNow, kFiltNow, cap);
  TEST_ASSERT_TRUE(v.accepted);
}

// =============================================================================
// G — R5 : CADENCE D'ÉCRITURE
// =============================================================================
// Deux conditions CONJONCTIVES : au plus une écriture par 30 min ET amplitude
// supérieure à 5 % du plafond. La NVS ne garantit que ~100 000 cycles par
// secteur ; une écriture par tour de loop la détruirait en quelques jours.

static PhPersistWriteInputs writeIn(uint32_t nowMs, uint32_t lastSaveMs,
                                    float value, float savedValue,
                                    uint32_t epochNow = kNow,
                                    float integralMax = kPhIntegralMaxNormal) {
  PhPersistWriteInputs in;
  in.nowMs = nowMs;
  in.lastSaveMs = lastSaveMs;
  in.epochNow = epochNow;
  in.value = value;
  in.savedValue = savedValue;
  in.integralMax = integralMax;
  in.filtMinNow = kFiltNow;    // feature-093 : pas d'échéance P3 par défaut
  in.filtMinSaved = kFiltNow;
  return in;
}

// Les deux conditions sont bien CONJONCTIVES : ni l'une ni l'autre ne suffit.
void test_G_les_deux_conditions_sont_conjonctives(void) {
  const float span = kPhIntegralPersistMinDeltaFrac * kImax;  // 810 pH·s
  const uint32_t t0 = 100000u;
  const uint32_t assez = t0 + kPhIntegralPersistMinIntervalMs;
  const uint32_t tropTot = t0 + kPhIntegralPersistMinIntervalMs - 1u;

  // Délai OK + amplitude insuffisante ⇒ NON.
  TEST_ASSERT_FALSE(phPersistShouldWrite(
      writeIn(assez, t0, 1000.0f + 0.5f * span, 1000.0f)));
  // Amplitude OK + délai insuffisant ⇒ NON.
  TEST_ASSERT_FALSE(phPersistShouldWrite(
      writeIn(tropTot, t0, 1000.0f + 3.0f * span, 1000.0f)));
  // Les deux ⇒ OUI.
  TEST_ASSERT_TRUE(phPersistShouldWrite(
      writeIn(assez, t0, 1000.0f + 3.0f * span, 1000.0f)));
  // Aucune des deux ⇒ NON.
  TEST_ASSERT_FALSE(phPersistShouldWrite(
      writeIn(tropTot, t0, 1000.0f + 0.5f * span, 1000.0f)));
  // L'amplitude est prise en VALEUR ABSOLUE : une décharge s'écrit aussi.
  TEST_ASSERT_TRUE(phPersistShouldWrite(
      writeIn(assez, t0, 1000.0f - 3.0f * span, 1000.0f)));
}

// Frontières exactes : le délai est atteint (>=), l'amplitude est strictement
// supérieure (>). Une valeur qui bouge d'exactement 5 % n'écrit pas.
void test_G_frontieres_exactes(void) {
  const float span = kPhIntegralPersistMinDeltaFrac * kImax;
  const uint32_t t0 = 100000u;

  TEST_ASSERT_TRUE(phPersistShouldWrite(writeIn(
      t0 + kPhIntegralPersistMinIntervalMs, t0, 1000.0f + 2.0f * span, 1000.0f)));
  TEST_ASSERT_FALSE(phPersistShouldWrite(writeIn(
      t0 + kPhIntegralPersistMinIntervalMs - 1u, t0, 1000.0f + 2.0f * span, 1000.0f)));

  const uint32_t assez = t0 + kPhIntegralPersistMinIntervalMs;
  TEST_ASSERT_FALSE(phPersistShouldWrite(writeIn(assez, t0, 1000.0f + span, 1000.0f)));
  TEST_ASSERT_TRUE(phPersistShouldWrite(
      writeIn(assez, t0, 1000.0f + span * 1.01f, 1000.0f)));
  // 810 pH·s : la valeur du seuil aux réglages `normal`.
  TEST_ASSERT_FLOAT_WITHIN(0.5f, 810.0f, span);
}

// Première écriture de la vie de l'appareil (lastSaveMs = 0) : la cadence ne
// s'applique pas, seule l'amplitude compte.
void test_G_premiere_ecriture_ignore_la_cadence(void) {
  const float span = kPhIntegralPersistMinDeltaFrac * kImax;
  TEST_ASSERT_TRUE(phPersistShouldWrite(writeIn(5000u, 0u, 3.0f * span, 0.0f)));
  TEST_ASSERT_FALSE(phPersistShouldWrite(writeIn(5000u, 0u, 0.5f * span, 0.0f)));
}

// Sans heure valide, on n'écrit PAS : l'âge serait incalculable au prochain boot
// et l'enregistrement y serait de toute façon rejeté (R3) — autant épargner le
// cycle d'écriture.
void test_G_heure_invalide_interdit_l_ecriture(void) {
  const float span = kPhIntegralPersistMinDeltaFrac * kImax;
  const uint32_t t0 = 100000u;
  const uint32_t assez = t0 + kPhIntegralPersistMinIntervalMs;
  TEST_ASSERT_FALSE(phPersistShouldWrite(
      writeIn(assez, t0, 1000.0f + 3.0f * span, 1000.0f, /*epoch*/0u)));
  TEST_ASSERT_FALSE(phPersistShouldWrite(
      writeIn(assez, t0, 1000.0f + 3.0f * span, 1000.0f,
              (uint32_t)kMinValidEpoch - 1u)));
  TEST_ASSERT_TRUE(phPersistShouldWrite(
      writeIn(assez, t0, 1000.0f + 3.0f * span, 1000.0f,
              (uint32_t)kMinValidEpoch)));
}

void test_G_echelle_absente_ou_valeur_non_finie_interdit_l_ecriture(void) {
  const uint32_t t0 = 100000u;
  const uint32_t assez = t0 + kPhIntegralPersistMinIntervalMs;
  // Plafond nul ou négatif : le seuil d'amplitude n'a pas d'échelle.
  TEST_ASSERT_FALSE(phPersistShouldWrite(
      writeIn(assez, t0, 5000.0f, 0.0f, kNow, /*integralMax*/0.0f)));
  TEST_ASSERT_FALSE(phPersistShouldWrite(
      writeIn(assez, t0, 5000.0f, 0.0f, kNow, -1.0f)));
  TEST_ASSERT_FALSE(phPersistShouldWrite(
      writeIn(assez, t0, 5000.0f, 0.0f, kNow, NAN)));
  // Valeur courante non finie : jamais persistée.
  TEST_ASSERT_FALSE(phPersistShouldWrite(writeIn(assez, t0, NAN, 0.0f)));
}

// millis() boucle toutes les ~49,7 jours. La soustraction non signée doit rester
// juste au franchissement, sinon l'appareil se met à écrire à chaque tour de loop
// pendant 30 minutes — ou cesse d'écrire pendant 49 jours.
void test_G_wrap_de_millis(void) {
  const float span = kPhIntegralPersistMinDeltaFrac * kImax;
  const uint32_t avantWrap = 0xFFFFFF00u;  // 256 ms avant le repliement

  // 256 ms après le wrap : le délai n'est PAS atteint.
  TEST_ASSERT_FALSE(phPersistShouldWrite(
      writeIn(0x000000FFu, avantWrap, 1000.0f + 3.0f * span, 1000.0f)));
  // 30 min après le dernier flush, wrap traversé : le délai EST atteint.
  const uint32_t apres =
      (uint32_t)(avantWrap + kPhIntegralPersistMinIntervalMs);  // repliement
  TEST_ASSERT_TRUE(phPersistShouldWrite(
      writeIn(apres, avantWrap, 1000.0f + 3.0f * span, 1000.0f)));
}

// =============================================================================
// H — CONTRAT NUMÉRIQUE ET JOURNALISATION
// =============================================================================

void test_H_contrat_numerique_du_module(void) {
  // R1
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.30f, kPhIntegralRestoreMaxFraction);
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.50f, kPhIntegralRestoreLoadFraction);
  TEST_ASSERT_EQUAL_UINT8(7, kPhIntegralRestoreHistoryWindowDays);
  TEST_ASSERT_EQUAL_UINT8(3, kPhIntegralRestoreMinHistoryDays);
  TEST_ASSERT_TRUE(kPhIntegralRestoreMinHistoryDays <=
                   kPhIntegralRestoreHistoryWindowDays);
  // R2
  TEST_ASSERT_EQUAL_UINT8(2, kPhIntegralPersistSchema);  // feature-093 : 24 octets
  // R3 (feature-093) — 6 h DE FILTRATION = une constante d'intégration à
  // vitesse `normal` ; plafond mural de sécurité 72 h ; échéance P3 à mi-fenêtre.
  TEST_ASSERT_EQUAL_UINT32(360UL, kPhIntegralPersistMaxFiltMin);
  TEST_ASSERT_EQUAL_UINT32((uint32_t)kPhIntegralTiNormalS, kPhIntegralPersistMaxFiltMin * 60UL);
  TEST_ASSERT_EQUAL_UINT32(259200UL, kPhIntegralPersistMaxWallAgeS);
  TEST_ASSERT_EQUAL_UINT32(180UL, kPhIntegralPersistDueFiltMin);
  TEST_ASSERT_TRUE(kPhIntegralPersistDueFiltMin < kPhIntegralPersistMaxFiltMin);
  // R4
  TEST_ASSERT_EQUAL_UINT32(300000UL, kPhIntegralRestoreConfirmMs);
  TEST_ASSERT_EQUAL_UINT32(600000UL, kPhIntegralRestoreArmWindowMs);
  TEST_ASSERT_TRUE(kPhIntegralRestoreConfirmMs < kPhIntegralRestoreArmWindowMs);
  // R5 — le seuil d'amplitude reste très en dessous du plafond de restauration.
  TEST_ASSERT_EQUAL_UINT32(1800000UL, kPhIntegralPersistMinIntervalMs);
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.05f, kPhIntegralPersistMinDeltaFrac);
  TEST_ASSERT_TRUE(kPhIntegralPersistMinDeltaFrac < kPhIntegralRestoreMaxFraction);
  // R7
  TEST_ASSERT_EQUAL_UINT8(3, kPhIntegralRestoreMaxStreak);
}

// R6 — tout rejet doit être JOURNALISABLE, sauf les deux silences volontaires.
// Un rejet muet rendrait la persistance indébogable : on ne saurait pas
// distinguer « rien à restaurer » de « restauration refusée ».
void test_H_libelles_de_rejet(void) {
  const PhPersistReject parlants[] = {
      PhPersistReject::kSchema, PhPersistReject::kStreak,
      PhPersistReject::kContext, PhPersistReject::kEpoch,
      PhPersistReject::kStale,  PhPersistReject::kValue,
      PhPersistReject::kFiltCounter,
  };
  for (int i = 0; i < 7; ++i) {
    const char* label = phPersistRejectLabel(parlants[i]);
    TEST_ASSERT_NOT_NULL(label);
    TEST_ASSERT_TRUE(label[0] != '\0');
  }
  // Silences volontaires.
  TEST_ASSERT_NULL(phPersistRejectLabel(PhPersistReject::kNone));
  TEST_ASSERT_NULL(phPersistRejectLabel(PhPersistReject::kNoCap));
}

// =============================================================================
// I — L'ANNEAU DE CHARGE : datation, fenêtre d'âge, déduplication, médiane
// =============================================================================
// C'est la SOURCE du plafond R1. Le bloc C mesure ce que le plafond fait d'une
// charge donnée ; celui-ci mesure d'où vient cette charge — et surtout ce qui en
// est ÉCARTÉ. Chaque échantillon indûment retenu gonfle la médiane, donc le
// plafond, donc l'autorité de dosage rendue au démarrage.

/** Date du jour de référence pour tout ce bloc. */
static const uint32_t kAujourdhui = 20260830UL;

// DÉBIT MOYEN SOUTENABLE — le seuil du filet de vraisemblance en dépend, et c'est
// une FONCTION des réglages, jamais une constante. Deux décors :
//   - `kSoutenable5` : réglages par défaut, 5 min/h × 26 mL/min = 2,17 mL/min ;
//   - `kSoutenable15` : limite horaire relevée à 15 min/h (la recommandation du
//     gate pour combler le déficit demande/délivré) = 6,50 mL/min.
// Les journées de ce bloc portent des volumes DÉLIVRÉS (ce qui sort de la pompe),
// pas des demandes : à 5 min/h, un délivré réaliste vaut 1,5 à 2,1 mL/min.
static const float kAmpFlow26     = 26.0f;
static const float kSoutenable5   = phSustainableAvgFlow(kAmpFlow26, 5);
static const float kSoutenable15  = phSustainableAvgFlow(kAmpFlow26, 15);

static PhLoadDaySample day(uint32_t date, uint16_t ml, uint16_t minutes) {
  PhLoadDaySample d;
  d.date = date;
  d.ml = ml;
  d.minutes = minutes;
  return d;
}

/** Anneau SCELLÉ garni des `n` premières journées fournies (le reste vide). */
static PhLoadHistory ring(const PhLoadDaySample* src, uint8_t n) {
  PhLoadHistory h;
  for (uint8_t i = 0; i < n && i < kPhIntegralRestoreHistoryWindowDays; ++i) {
    h.days[i] = src[i];
  }
  phLoadHistorySeal(h);
  return h;
}

// --- Datation ----------------------------------------------------------------

// La conversion AAAAMMJJ → jour calendaire est le SEUL moyen d'obtenir un écart
// en jours : une soustraction décimale donne 73 pour « 28 février → 1er mars » et
// 8 870 pour « 31 décembre → 1er janvier ». On vérifie les trois pièges : fin de
// mois, changement d'année, et la règle bissextile des 400 ans dans les deux sens.
void test_I_ecarts_de_jours_aux_frontieres_du_calendrier(void) {
  // Année COMMUNE : 28 févr. → 1er mars = 1 jour.
  TEST_ASSERT_EQUAL_UINT32(1, phLoadDayNumber(20260301UL) - phLoadDayNumber(20260228UL));
  // Année BISSEXTILE (divisible par 4) : le 29 s'intercale ⇒ 2 jours.
  TEST_ASSERT_EQUAL_UINT32(2, phLoadDayNumber(20240301UL) - phLoadDayNumber(20240228UL));
  // Séculaire NON bissextile (divisible par 100, pas par 400) ⇒ 1 jour.
  TEST_ASSERT_EQUAL_UINT32(1, phLoadDayNumber(21000301UL) - phLoadDayNumber(21000228UL));
  // Séculaire BISSEXTILE (divisible par 400) ⇒ 2 jours.
  TEST_ASSERT_EQUAL_UINT32(2, phLoadDayNumber(20000301UL) - phLoadDayNumber(20000228UL));
  // Passage d'année.
  TEST_ASSERT_EQUAL_UINT32(1, phLoadDayNumber(20270101UL) - phLoadDayNumber(20261231UL));
  // Mois de 30 et de 31 jours.
  TEST_ASSERT_EQUAL_UINT32(1, phLoadDayNumber(20260501UL) - phLoadDayNumber(20260430UL));
  TEST_ASSERT_EQUAL_UINT32(1, phLoadDayNumber(20260801UL) - phLoadDayNumber(20260731UL));
  // Une année complète non bissextile.
  TEST_ASSERT_EQUAL_UINT32(365, phLoadDayNumber(20270101UL) - phLoadDayNumber(20260101UL));
  // Une année complète bissextile (2024).
  TEST_ASSERT_EQUAL_UINT32(366, phLoadDayNumber(20250101UL) - phLoadDayNumber(20240101UL));
  // Monotonie stricte sur un mois entier, jour par jour.
  for (uint32_t j = 1; j < 31; ++j) {
    TEST_ASSERT_EQUAL_UINT32(1, phLoadDayNumber(20260300UL + j + 1) -
                                    phLoadDayNumber(20260300UL + j));
  }
}

// Une date illisible ne doit JAMAIS être réinterprétée : elle rend 0, sentinelle
// qui fait échouer toute comparaison de fenêtre (fail-closed).
void test_I_date_malformee_rend_zero(void) {
  TEST_ASSERT_EQUAL_UINT32(0, phLoadDayNumber(0));
  TEST_ASSERT_EQUAL_UINT32(0, phLoadDayNumber(20261301UL));  // mois 13
  TEST_ASSERT_EQUAL_UINT32(0, phLoadDayNumber(20260001UL));  // mois 0
  TEST_ASSERT_EQUAL_UINT32(0, phLoadDayNumber(20260100UL));  // jour 0
  TEST_ASSERT_EQUAL_UINT32(0, phLoadDayNumber(20260132UL));  // jour 32
  TEST_ASSERT_EQUAL_UINT32(0, phLoadDayNumber(19691231UL));  // avant 1970
  TEST_ASSERT_EQUAL_UINT32(0, phLoadDayNumber(123UL));       // format absurde
}

// --- Fenêtre d'âge -----------------------------------------------------------

// La fenêtre est FERMÉE à kPhIntegralRestoreHistoryWindowDays jours : la journée
// d'hier compte, celle d'il y a 7 jours aussi, celle d'il y a 8 non.
void test_I_fenetre_fermee_a_sept_jours(void) {
  TEST_ASSERT_TRUE(phLoadDayWithinWindow(20260830UL, kAujourdhui));   // 0 j
  TEST_ASSERT_TRUE(phLoadDayWithinWindow(20260829UL, kAujourdhui));   // 1 j
  TEST_ASSERT_TRUE(phLoadDayWithinWindow(20260823UL, kAujourdhui));   // 7 j
  TEST_ASSERT_FALSE(phLoadDayWithinWindow(20260822UL, kAujourdhui));  // 8 j
  TEST_ASSERT_FALSE(phLoadDayWithinWindow(20260630UL, kAujourdhui));  // 2 mois
}

// Date FUTURE (horloge reculée par une correction NTP) : âge non calculable, donc
// échantillon écarté. Sans horloge du tout (today = 0), c'est tout l'anneau qui
// disparaît — l'appareil ne restaure alors rien.
void test_I_date_future_ou_horloge_absente_ecarte_l_echantillon(void) {
  TEST_ASSERT_FALSE(phLoadDayWithinWindow(20260831UL, kAujourdhui));  // demain
  TEST_ASSERT_FALSE(phLoadDayWithinWindow(20270101UL, kAujourdhui));
  TEST_ASSERT_FALSE(phLoadDayWithinWindow(20260829UL, 0));            // pas d'heure
  TEST_ASSERT_FALSE(phLoadDayWithinWindow(0, kAujourdhui));           // case vide
}

// --- Critère de jour exploitable ---------------------------------------------

void test_I_criteres_cumulatifs_du_jour_exploitable(void) {
  // Circulation suffisante ET dans la fenêtre — 200 mL délivrés en 2 h = 1,67.
  TEST_ASSERT_TRUE(phLoadDayUsable(day(20260829UL, 200, kPhLoadDayMinFiltrationMin),
                                   kAujourdhui, kSoutenable5));
  // Une minute de trop peu : écartée (le rapport décrirait des transitoires).
  TEST_ASSERT_FALSE(phLoadDayUsable(
      day(20260829UL, 200, (uint16_t)(kPhLoadDayMinFiltrationMin - 1)),
      kAujourdhui, kSoutenable5));
  // Circulation suffisante mais journée trop VIEILLE : écartée.
  TEST_ASSERT_FALSE(
      phLoadDayUsable(day(20260601UL, 1500, 1000), kAujourdhui, kSoutenable5));
  // Case vide.
  TEST_ASSERT_FALSE(phLoadDayUsable(day(0, 1500, 1000), kAujourdhui, kSoutenable5));
  // Volume NUL mais circulation suffisante : CONSERVÉE. L'écarter tirerait la
  // médiane vers le haut, c'est-à-dire du mauvais côté.
  TEST_ASSERT_TRUE(phLoadDayUsable(day(20260829UL, 0, 600), kAujourdhui, kSoutenable5));
}

// --- Filet de vraisemblance physique -----------------------------------------

// ⭐ L'anneau enregistre du DÉLIVRÉ (ce qui sort de la pompe), pas de la demande.
// Le délivré est structurellement borné par « limite horaire × débit d'amplitude »
// : un rapport au-dessus n'est pas l'observation d'une charge élevée, c'est la
// PREUVE que des minutes manquent au dénominateur (flush NVS inatteignable,
// compteur amputé, blob à demi écrit).
void test_I_journee_physiquement_impossible_est_ecartee(void) {
  const float plafond = kPhLoadDayMaxRateFactor * kSoutenable5;  // 2,38 mL/min

  // Le délivré NOMINAL de la référence à 5 min/h (1,5 à 2,1 mL/min) passe.
  TEST_ASSERT_TRUE(phLoadDayRatePlausible(day(20260829UL, 1500, 1000), kSoutenable5));
  TEST_ASSERT_TRUE(phLoadDayRatePlausible(day(20260829UL, 2100, 1000), kSoutenable5));

  // 2,5 mL/min : au-dessus de ce que la pompe peut délivrer à ce réglage. Écartée.
  TEST_ASSERT_TRUE(2.5f > plafond);
  TEST_ASSERT_FALSE(phLoadDayRatePlausible(day(20260829UL, 2500, 1000), kSoutenable5));
  TEST_ASSERT_FALSE(
      phLoadDayUsable(day(20260829UL, 2500, 1000), kAujourdhui, kSoutenable5));

  // Le cas du gate : 1 000 mL annoncés en 130 min = 7,7 mL/min. Écartée.
  TEST_ASSERT_FALSE(phLoadDayRatePlausible(day(20260829UL, 1000, 130), kSoutenable5));

  // Le scénario de maintenance : 5 mL sur 120 min comptées au lieu de 480 est en
  // revanche SOUS le plafond — le filet est un garde-fou grossier, pas un
  // substitut au flush remonté en amont des sorties anticipées d'update().
  TEST_ASSERT_TRUE(phLoadDayRatePlausible(day(20260829UL, 5, 120), kSoutenable5));

  // Juste sous / juste au-dessus du plafond, à 1 000 min de circulation.
  const uint16_t justeSous = (uint16_t)(plafond * 1000.0f) - 1u;
  TEST_ASSERT_TRUE(phLoadDayRatePlausible(day(20260829UL, justeSous, 1000), kSoutenable5));
  TEST_ASSERT_FALSE(phLoadDayRatePlausible(
      day(20260829UL, (uint16_t)(justeSous + 3u), 1000), kSoutenable5));

  // Le filet n'écarte QUE par le haut : une journée à VOLUME NUL est conservée,
  // elle tire la médiane vers le bas — donc le plafond de restauration aussi.
  TEST_ASSERT_TRUE(phLoadDayRatePlausible(day(20260829UL, 0, 400), kSoutenable5));

  // Minutes nulles : le rapport n'existe pas.
  TEST_ASSERT_FALSE(phLoadDayRatePlausible(day(20260829UL, 100, 0), kSoutenable5));
}

// ⭐⭐ LE PIÈGE SILENCIEUX QUE LE SEUIL FIGÉ AURAIT POSÉ. Le gate recommande de
// relever `phInjectionLimitMinutes` pour combler le déficit demande/délivré. À
// 15 min/h le délivré légitime monte à 6,5 mL/min : contre un seuil figé sur les
// 5 min/h par défaut, TOUTES les journées seraient devenues invraisemblables,
// `days` serait retombé à 0 et la restauration ne se serait plus jamais armée —
// sans une ligne de journal, au moment exact où l'utilisateur suit la
// recommandation. Le seuil doit donc SUIVRE la configuration.
void test_I_SENTINELLE_le_seuil_suit_la_limite_horaire_configuree(void) {
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 2.17f, kSoutenable5);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 6.50f, kSoutenable15);

  const PhLoadDaySample chargee = day(20260829UL, 6000, 1000);  // 6,00 mL/min

  // Aux réglages par défaut, 6 mL/min est impossible : écartée, à juste titre.
  TEST_ASSERT_FALSE(phLoadDayRatePlausible(chargee, kSoutenable5));
  // Limite horaire relevée à 15 min/h : le même délivré devient LÉGITIME.
  TEST_ASSERT_TRUE(phLoadDayRatePlausible(chargee, kSoutenable15));
  TEST_ASSERT_TRUE(phLoadDayUsable(chargee, kAujourdhui, kSoutenable15));

  // Et l'anneau entier survit au relèvement : trois journées chargées restent
  // exploitables, donc le plafond R1 reste armé.
  const PhLoadDaySample trois[3] = {
      day(20260829UL, 6000, 1000), day(20260828UL, 5800, 1000),
      day(20260827UL, 6200, 1000),
  };
  const PhLoadHistory h = ring(trois, 3);
  TEST_ASSERT_EQUAL_UINT8(0, phLoadObservation(h, kAujourdhui, kSoutenable5).days);
  const PhPersistLoadObservation o15 = phLoadObservation(h, kAujourdhui, kSoutenable15);
  TEST_ASSERT_EQUAL_UINT8(3, o15.days);
  TEST_ASSERT_FLOAT_WITHIN(1e-3f, 6.00f, o15.loadMlPerMin);
}

// Débit soutenable INCALCULABLE (pompe non caractérisée, config pas encore
// chargée, valeur non finie) : repli documenté, jamais de seuil nul — un seuil
// nul écarterait tout, un seuil infini n'écarterait rien.
void test_I_repli_quand_le_debit_soutenable_est_indisponible(void) {
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, kPhLoadSustainableFallbackMlPerMin,
                           phLoadSustainableOrFallback(0.0f));
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, kPhLoadSustainableFallbackMlPerMin,
                           phLoadSustainableOrFallback(-1.0f));
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, kPhLoadSustainableFallbackMlPerMin,
                           phLoadSustainableOrFallback(NAN));
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, 6.5f, phLoadSustainableOrFallback(6.5f));

  // Pompe non caractérisée (ampFlow = 0) : phSustainableAvgFlow rend 0, le filet
  // retombe donc sur le repli — 2,5 mL/min reste écartée, 1,5 reste retenue.
  const float indispo = phSustainableAvgFlow(/*ampFlow*/0.0f, 5);
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0f, indispo);
  TEST_ASSERT_FALSE(phLoadDayRatePlausible(day(20260829UL, 2500, 1000), indispo));
  TEST_ASSERT_TRUE(phLoadDayRatePlausible(day(20260829UL, 1500, 1000), indispo));
}

// Une journée invraisemblable ne doit pas seulement être ignorée du critère :
// elle doit disparaître de la MÉDIANE, sans quoi elle la tirerait vers le haut.
void test_I_journee_impossible_ne_pese_pas_dans_la_mediane(void) {
  const PhLoadDaySample quatre[4] = {
      day(20260829UL, 1500, 1000),  // 1,50 mL/min
      day(20260828UL, 1500, 1000),  // 1,50
      day(20260827UL, 1500, 1000),  // 1,50
      day(20260826UL, 1000, 130),   // 7,69 — dénominateur amputé
  };
  const PhPersistLoadObservation o =
      phLoadObservation(ring(quatre, 4), kAujourdhui, kSoutenable5);
  TEST_ASSERT_EQUAL_UINT8(3, o.days);
  TEST_ASSERT_FLOAT_WITHIN(1e-3f, 1.50f, o.loadMlPerMin);
}

// --- Médiane ------------------------------------------------------------------

void test_I_mediane_impaire_et_paire(void) {
  // Cinq journées de DÉLIVRÉ réaliste, mL/min = 2,1 / 1,5 / 1,9 / 1,6 / 1,7
  // (désordonnées) ⇒ médiane 1,7.
  const PhLoadDaySample cinq[5] = {
      day(20260829UL, 2100, 1000), day(20260828UL, 1500, 1000),
      day(20260827UL, 1900, 1000), day(20260826UL, 1600, 1000),
      day(20260825UL, 1700, 1000),
  };
  PhPersistLoadObservation o =
      phLoadObservation(ring(cinq, 5), kAujourdhui, kSoutenable5);
  TEST_ASSERT_EQUAL_UINT8(5, o.days);
  TEST_ASSERT_FLOAT_WITHIN(1e-4f, 1.7f, o.loadMlPerMin);

  // Quatre journées, mL/min = 2,1 / 1,5 / 1,9 / 1,7 ⇒ moyenne des centrales = 1,8.
  const PhLoadDaySample quatre[4] = {
      day(20260829UL, 2100, 1000), day(20260828UL, 1500, 1000),
      day(20260827UL, 1900, 1000), day(20260826UL, 1700, 1000),
  };
  o = phLoadObservation(ring(quatre, 4), kAujourdhui, kSoutenable5);
  TEST_ASSERT_EQUAL_UINT8(4, o.days);
  TEST_ASSERT_FLOAT_WITHIN(1e-4f, 1.8f, o.loadMlPerMin);
}

// La médiane, et non la moyenne : trois journées folles sur sept ne la déplacent
// pas. C'est ce qui protège le plafond d'une purge, d'une mise en service ou d'un
// passage de l'assistant de mesure de débit.
void test_I_trois_journees_aberrantes_sur_sept_ne_deplacent_pas_la_mediane(void) {
  const PhLoadDaySample sains[7] = {
      day(20260829UL, 1500, 1000), day(20260828UL, 1500, 1000),
      day(20260827UL, 1500, 1000), day(20260826UL, 1500, 1000),
      day(20260825UL, 1500, 1000), day(20260824UL, 1500, 1000),
      day(20260823UL, 1500, 1000),
  };
  PhLoadHistory h = ring(sains, 7);
  TEST_ASSERT_FLOAT_WITHIN(1e-4f, 1.5f,
                           phLoadObservation(h, kAujourdhui, kSoutenable5).loadMlPerMin);
  // Trois journées à 2,3 mL/min (poussée de consommation, encore sous le plafond
  // de vraisemblance donc encore comptées) : la médiane ne bouge pas d'un pouce.
  h.days[0].ml = 2300; h.days[1].ml = 2300; h.days[2].ml = 2300;
  phLoadHistorySeal(h);
  const PhPersistLoadObservation o = phLoadObservation(h, kAujourdhui, kSoutenable5);
  TEST_ASSERT_EQUAL_UINT8(7, o.days);
  TEST_ASSERT_FLOAT_WITHIN(1e-4f, 1.5f, o.loadMlPerMin);
}

// ⭐ LE SCÉNARIO DE LA REVUE — quatre journées d'ÉTÉ à 2,10 mL/min de délivré
// dorment dans l'anneau, l'appareil est arrêté deux mois, trois journées
// d'AUTOMNE à 1,00 mL/min sont archivées. Sans fenêtre d'âge, la médiane des sept
// vaut 2,10 (valeur estivale, majoritaire à 4 contre 3) et le plafond qui en
// découle passe AU-DESSUS de l'équilibre réel de l'automne : une restauration qui
// rend plus d'autorité que la boucle n'en accumulerait d'elle-même, exactement ce
// que le gate déclare inacceptable.
void test_I_SENTINELLE_journees_hors_saison_ne_pesent_plus_dans_la_mediane(void) {
  const uint32_t automne = 20261015UL;
  const PhLoadDaySample sept[7] = {
      // Été : 2 100 mL en 1 000 min = 2,10 mL/min. Hors fenêtre depuis longtemps.
      day(20260810UL, 2100, 1000), day(20260811UL, 2100, 1000),
      day(20260812UL, 2100, 1000), day(20260813UL, 2100, 1000),
      // Automne : 1 000 mL en 1 000 min = 1,00 mL/min. Dans la fenêtre.
      day(20261012UL, 1000, 1000), day(20261013UL, 1000, 1000),
      day(20261014UL, 1000, 1000),
  };
  const PhLoadHistory h = ring(sept, 7);

  const PhPersistLoadObservation o = phLoadObservation(h, automne, kSoutenable5);
  TEST_ASSERT_EQUAL_UINT8(3, o.days);  // les 4 journées d'été sont écartées
  TEST_ASSERT_FLOAT_WITHIN(1e-3f, 1.00f, o.loadMlPerMin);

  // Le plafond qui en découle reste sous l'équilibre RÉEL de l'automne.
  const PhPersistCapVerdict cap = phPersistRestoreCap(kImax, kKp, kKi, kStop, o);
  const float equilibreAutomne = equilibriumPhS(1.00f, kKp, kKi, kStop);
  TEST_ASSERT_TRUE(cap.capPhS > 0.0f);
  TEST_ASSERT_TRUE(cap.capPhS < equilibreAutomne);

  // CE QUE LA FENÊTRE ÉVITE, chiffré : la médiane estivale aurait produit un
  // plafond SUPÉRIEUR à l'équilibre d'automne — de l'autorité pré-armée que la
  // boucle n'aurait jamais accumulée d'elle-même dans les conditions du jour.
  const PhPersistCapVerdict capEte = capFor(2.10f);
  TEST_ASSERT_TRUE(capEte.capPhS > equilibreAutomne);
  TEST_ASSERT_TRUE(cap.capPhS < capEte.capPhS);
}

// Variante exigée par le gate : le même anneau avec DEUX journées récentes
// seulement. Sous kPhIntegralRestoreMinHistoryDays, le plafond est NUL — aucune
// restauration, quelle que soit la richesse apparente de l'anneau.
void test_I_deux_journees_recentes_seulement_ferment_le_plafond(void) {
  const uint32_t automne = 20261015UL;
  const PhLoadDaySample sept[7] = {
      day(20260810UL, 2100, 1000), day(20260811UL, 2100, 1000),
      day(20260812UL, 2100, 1000), day(20260813UL, 2100, 1000),
      day(20260814UL, 2100, 1000),
      day(20261013UL, 1000, 1000), day(20261014UL, 1000, 1000),
  };
  const PhPersistLoadObservation o =
      phLoadObservation(ring(sept, 7), automne, kSoutenable5);
  TEST_ASSERT_EQUAL_UINT8(2, o.days);
  TEST_ASSERT_TRUE(o.days < kPhIntegralRestoreMinHistoryDays);
  const PhPersistCapVerdict cap = phPersistRestoreCap(kImax, kKp, kKi, kStop, o);
  TEST_ASSERT_TRUE(cap.historyShort);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, cap.capPhS);
}

// Fail-closed intégral : sept journées présentes, toutes hors fenêtre ⇒ days = 0
// ⇒ plafond NUL ⇒ aucune restauration. Un appareil qui redémarre après un hiver
// d'arrêt réapprend, il ne restaure pas.
void test_I_anneau_entierement_perime_ferme_le_plafond(void) {
  const PhLoadDaySample vieux[7] = {
      day(20260101UL, 1500, 1000), day(20260102UL, 1500, 1000),
      day(20260103UL, 1500, 1000), day(20260104UL, 1500, 1000),
      day(20260105UL, 1500, 1000), day(20260106UL, 1500, 1000),
      day(20260107UL, 1500, 1000),
  };
  const PhPersistLoadObservation o =
      phLoadObservation(ring(vieux, 7), kAujourdhui, kSoutenable5);
  TEST_ASSERT_EQUAL_UINT8(0, o.days);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, o.loadMlPerMin);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, phPersistRestoreCap(kImax, kKp, kKi, kStop, o).capPhS);
}

// Sans date du jour (horloge non synchronisée), l'âge n'existe pas : l'anneau
// entier est écarté, même garni de journées parfaites.
void test_I_sans_date_du_jour_l_anneau_entier_est_ecarte(void) {
  const PhLoadDaySample recents[3] = {
      day(20260829UL, 1500, 1000), day(20260828UL, 1500, 1000),
      day(20260827UL, 1500, 1000),
  };
  TEST_ASSERT_EQUAL_UINT8(
      3, phLoadObservation(ring(recents, 3), kAujourdhui, kSoutenable5).days);
  TEST_ASSERT_EQUAL_UINT8(
      0, phLoadObservation(ring(recents, 3), 0, kSoutenable5).days);
}

// --- Insertion et déduplication ----------------------------------------------

void test_I_insertion_decale_et_evince_la_plus_ancienne(void) {
  PhLoadHistory h;
  for (uint32_t j = 1; j <= 7; ++j) {
    TEST_ASSERT_TRUE(phLoadHistoryAppend(h, day(20260820UL + j, 600, 600)));
  }
  TEST_ASSERT_EQUAL_UINT32(20260821UL, h.days[0].date);
  TEST_ASSERT_EQUAL_UINT32(20260827UL, h.days[6].date);
  // La huitième évince la première.
  TEST_ASSERT_TRUE(phLoadHistoryAppend(h, day(20260828UL, 600, 600)));
  TEST_ASSERT_EQUAL_UINT32(20260822UL, h.days[0].date);
  TEST_ASSERT_EQUAL_UINT32(20260828UL, h.days[6].date);
  // Une date nulle n'est pas datable : refusée, l'anneau ne bouge pas.
  TEST_ASSERT_FALSE(phLoadHistoryAppend(h, day(0, 600, 600)));
  TEST_ASSERT_EQUAL_UINT32(20260822UL, h.days[0].date);
}

// ⭐ Un RECUL D'HORLOGE (correction NTP à cheval sur minuit) reclôt une date déjà
// archivée UNE CASE PLUS LOIN que la dernière. Ne comparer qu'à la case la plus
// récente laisserait la même journée occuper deux cases, donc peser DOUBLE dans
// la médiane sur 7 — un poids de 2/7 accordé à une seule journée.
void test_I_deduplication_balaie_toutes_les_cases(void) {
  PhLoadHistory h;
  phLoadHistoryAppend(h, day(20260828UL, 1500, 1000));
  phLoadHistoryAppend(h, day(20260829UL, 1500, 1000));
  // L'horloge recule : la journée du 28 est reclôturée, avec un volume corrigé.
  TEST_ASSERT_TRUE(phLoadHistoryAppend(h, day(20260828UL, 1800, 1000)));

  uint8_t occurrences = 0;
  for (uint8_t i = 0; i < kPhIntegralRestoreHistoryWindowDays; ++i) {
    if (h.days[i].date == 20260828UL) ++occurrences;
  }
  TEST_ASSERT_EQUAL_UINT8(1, occurrences);   // écrasée, pas empilée
  TEST_ASSERT_EQUAL_UINT8(2, phLoadObservation(h, kAujourdhui, kSoutenable5).days);
  // C'est bien la valeur CORRIGÉE qui a remplacé l'ancienne.
  for (uint8_t i = 0; i < kPhIntegralRestoreHistoryWindowDays; ++i) {
    if (h.days[i].date == 20260828UL) TEST_ASSERT_EQUAL_UINT16(1800, h.days[i].ml);
  }
}

// --- Intégrité du blob de l'anneau -------------------------------------------

void test_I_anneau_scelle_est_accepte_toute_alteration_est_rejetee(void) {
  const PhLoadDaySample trois[3] = {
      day(20260829UL, 600, 600), day(20260828UL, 600, 600),
      day(20260827UL, 600, 600),
  };
  PhLoadHistory h = ring(trois, 3);
  TEST_ASSERT_TRUE(phLoadHistoryIntact(h, sizeof(h)));
  // Taille inattendue : autre schéma ou lecture tronquée ⇒ rejet net.
  TEST_ASSERT_FALSE(phLoadHistoryIntact(h, sizeof(h) - 1));
  TEST_ASSERT_FALSE(phLoadHistoryIntact(h, 0));
  // Schéma différent : jamais réinterprété.
  PhLoadHistory autre = h;
  autre.schema = (uint8_t)(kPhLoadHistorySchema + 1);
  TEST_ASSERT_FALSE(phLoadHistoryIntact(autre, sizeof(autre)));
  // Un seul octet retourné n'importe où en amont du CRC est détecté.
  for (size_t octet = 0; octet < offsetof(PhLoadHistory, crc); ++octet) {
    for (int bit = 0; bit < 8; ++bit) {
      PhLoadHistory abime = h;
      ((uint8_t*)&abime)[octet] ^= (uint8_t)(1u << bit);
      if (abime.schema != kPhLoadHistorySchema) continue;  // couvert plus haut
      char msg[64];
      snprintf(msg, sizeof(msg), "octet %u bit %d non détecté", (unsigned)octet, bit);
      TEST_ASSERT_FALSE_MESSAGE(phLoadHistoryIntact(abime, sizeof(abime)), msg);
    }
  }
}

// Le scellement doit être DÉTERMINISTE : deux anneaux de même contenu, construits
// depuis des piles différentes, produisent la même empreinte. Sans normalisation
// des octets d'alignement, la relecture échouerait au hasard.
void test_I_scellement_deterministe(void) {
  PhLoadHistory a;
  memset(&a, 0xA5, sizeof(a));
  PhLoadHistory b;
  memset(&b, 0x5C, sizeof(b));
  for (uint8_t i = 0; i < kPhIntegralRestoreHistoryWindowDays; ++i) {
    a.days[i] = day(20260820UL + i, 600, 600);
    b.days[i] = day(20260820UL + i, 600, 600);
  }
  phLoadHistorySeal(a);
  phLoadHistorySeal(b);
  TEST_ASSERT_EQUAL_UINT32(a.crc, b.crc);
  TEST_ASSERT_TRUE(phLoadHistoryIntact(a, sizeof(a)));
  TEST_ASSERT_TRUE(phLoadHistoryIntact(b, sizeof(b)));
}

// =============================================================================
// J — feature-093 : INTÉGRALE TRANSPARENTE AU REBOOT (v2.34.0)
// =============================================================================
// Ce qui a cassé le 03/09/2026 : valeur écrite la veille 15:00, stable ensuite
// (aucune écriture kDelta), rejetée « périmée » au matin par un âge MURAL de
// 6 h. Deux correctifs : (a) l'âge nominal se compte en MINUTES DE FILTRATION
// (schéma 2, champ filtTotalMin), l'heure murale n'est qu'un filet à 72 h ;
// (b) une écriture d'ÉCHÉANCE (kDue) toutes les 180 min de filtration garantit
// qu'un instantané frais existe à tout instant. Plus kRewrite (C5) : un
// enregistrement rejeté au boot est remplacé dès la 1re intégrale positive.

static const float kCapJ = 3000.0f;  // plafond R1 confortable pour ces tests
static const uint32_t kCtxJ = 0x0930C0DEu;

// Aller-retour : sceller, relire les 24 octets, vérifier CRC + champs, dont le
// compteur de filtration (offset 16) ; un bit du compteur inversé → non intact.
void test_J_aller_retour_24_octets_schema_2(void) {
  PhIntegralSnapshot s = sealed(1234.5f, kNow - 3600u, kCtxJ, 2, kPhIntegralPersistSchema, 4242u);
  uint8_t blob[24];
  memcpy(blob, &s, sizeof(blob));
  PhIntegralSnapshot r;
  memcpy(&r, blob, sizeof(r));
  TEST_ASSERT_TRUE(phPersistSnapshotIntact(r, 24));
  TEST_ASSERT_EQUAL_UINT8(2, r.schema);
  TEST_ASSERT_EQUAL_UINT8(2, r.restoreStreak);
  TEST_ASSERT_EQUAL_UINT32(kCtxJ, r.context);
  TEST_ASSERT_EQUAL_UINT32(kNow - 3600u, r.epoch);
  TEST_ASSERT_EQUAL_FLOAT(1234.5f, r.value);
  TEST_ASSERT_EQUAL_UINT32(4242u, r.filtTotalMin);
  TEST_ASSERT_EQUAL_UINT32(16, offsetof(PhIntegralSnapshot, filtTotalMin));
  TEST_ASSERT_EQUAL_UINT32(20, offsetof(PhIntegralSnapshot, crc));
  // Le CRC couvre bien le compteur : l'inverser est détecté.
  blob[16] ^= 0x01;
  memcpy(&r, blob, sizeof(r));
  TEST_ASSERT_FALSE(phPersistSnapshotIntact(r, 24));
  // Et il est relu par la décision : accepté tel quel, sans rejet.
  PhPersistRestoreVerdict v = phPersistEvaluateRestore(s, kCtxJ, kNow, 4242u + 100u, kCapJ);
  TEST_ASSERT_TRUE(v.accepted);
  TEST_ASSERT_EQUAL_UINT32(100u, v.filtAgeMin);
  TEST_ASSERT_EQUAL_UINT32(3600u, v.ageS);
}

// Un enregistrement du schéma 1 (v2.27-v2.33, sans compteur) est rejeté kSchema
// AVANT tout autre contrôle — même intact, même frais, même contexte identique.
void test_J_schema_1_est_rejete_avant_tout_autre_controle(void) {
  PhIntegralSnapshot s = sealed(1000.0f, kNow - 60u, kCtxJ, 0, /*schema*/1);
  TEST_ASSERT_TRUE(phPersistSnapshotIntact(s, sizeof(s)));  // intact…
  PhPersistRestoreVerdict v = phPersistEvaluateRestore(s, kCtxJ, kNow, kFiltNow, kCapJ);
  TEST_ASSERT_FALSE(v.accepted);
  TEST_ASSERT_TRUE(v.reject == PhPersistReject::kSchema);  // …mais refusé
  TEST_ASSERT_EQUAL_UINT8(2, kPhIntegralPersistSchema);
}

// LE CAS DU 03/09 : écrit la veille (14 h murales), 0 min de filtration depuis
// (nuit) → accepté, âge filtration 0.
void test_J_nuit_sans_filtration_puis_reboot_accepte(void) {
  PhIntegralSnapshot s = sealed(1200.0f, kNow - 14u * 3600u, kCtxJ, 0, kPhIntegralPersistSchema, kFiltNow);
  PhPersistRestoreVerdict v = phPersistEvaluateRestore(s, kCtxJ, kNow, kFiltNow, kCapJ);
  TEST_ASSERT_TRUE(v.accepted);
  TEST_ASSERT_TRUE(v.reject == PhPersistReject::kNone);
  TEST_ASSERT_EQUAL_UINT32(0, v.filtAgeMin);
  TEST_ASSERT_EQUAL_UINT32(14u * 3600u, v.ageS);
  TEST_ASSERT_EQUAL_FLOAT(1200.0f, v.candidate);
}

// Reboot (OTA, watchdog) en PLEINE filtration : écrit il y a 45 min de filtration
// et 45 min murales → accepté.
void test_J_reboot_en_cours_de_filtration_accepte(void) {
  PhIntegralSnapshot s = sealed(1200.0f, kNow - 45u * 60u, kCtxJ, 0, kPhIntegralPersistSchema, kFiltNow - 45u);
  PhPersistRestoreVerdict v = phPersistEvaluateRestore(s, kCtxJ, kNow, kFiltNow, kCapJ);
  TEST_ASSERT_TRUE(v.accepted);
  TEST_ASSERT_EQUAL_UINT32(45u, v.filtAgeMin);
}

// 361 min de filtration accumulées → kStale, même si l'heure murale ne dit que
// 1 h (impossible physiquement, mais la règle est en filtration : elle prime).
void test_J_361_min_de_filtration_rejete_meme_a_1h_murale(void) {
  PhIntegralSnapshot s = sealed(1200.0f, kNow - 3600u, kCtxJ, 0, kPhIntegralPersistSchema,
                                kFiltNow - (kPhIntegralPersistMaxFiltMin + 1u));
  PhPersistRestoreVerdict v = phPersistEvaluateRestore(s, kCtxJ, kNow, kFiltNow, kCapJ);
  TEST_ASSERT_FALSE(v.accepted);
  TEST_ASSERT_TRUE(v.reject == PhPersistReject::kStale);
  TEST_ASSERT_EQUAL_UINT32(kPhIntegralPersistMaxFiltMin + 1u, v.filtAgeMin);
  // 360 pile → accepté (inclusif).
  s = sealed(1200.0f, kNow - 3600u, kCtxJ, 0, kPhIntegralPersistSchema,
             kFiltNow - kPhIntegralPersistMaxFiltMin);
  TEST_ASSERT_TRUE(phPersistEvaluateRestore(s, kCtxJ, kNow, kFiltNow, kCapJ).accepted);
}

// 72 h + 1 s murales → kStale même à 0 min de filtration (hivernage, longue
// coupure : la chimie a pu changer sans circulation) ; 72 h pile → accepté.
void test_J_72h_plus_1s_rejete_meme_a_0_min_de_filtration(void) {
  PhIntegralSnapshot s = sealed(1200.0f, kNow - (kPhIntegralPersistMaxWallAgeS + 1u), kCtxJ);
  PhPersistRestoreVerdict v = phPersistEvaluateRestore(s, kCtxJ, kNow, kFiltNow, kCapJ);
  TEST_ASSERT_FALSE(v.accepted);
  TEST_ASSERT_TRUE(v.reject == PhPersistReject::kStale);
  TEST_ASSERT_EQUAL_UINT32(0, v.filtAgeMin);
  s = sealed(1200.0f, kNow - kPhIntegralPersistMaxWallAgeS, kCtxJ);
  TEST_ASSERT_TRUE(phPersistEvaluateRestore(s, kCtxJ, kNow, kFiltNow, kCapJ).accepted);
}

// Compteur non monotone (NVS `pool-daily` effacée : compteur courant < écrit) ET
// âge mural > 72 h : c'est kFiltCounter qui doit sortir, pas kStale — l'ordre
// compteur → péremption est verrouillé (le journal doit nommer la cause racine,
// une NVS effacée, pas un symptôme). Un ordre inversé, ou une régression de
// compteur non détectée (soustraction non signée → âge géant → kStale),
// donnerait kStale.
void test_J_compteur_non_monotone_avec_72h_c_est_le_compteur_qui_parle(void) {
  PhIntegralSnapshot s = sealed(1200.0f, kNow - (kPhIntegralPersistMaxWallAgeS + 1u), kCtxJ, 0,
                                kPhIntegralPersistSchema, kFiltNow + 1u);
  PhPersistRestoreVerdict v = phPersistEvaluateRestore(s, kCtxJ, kNow, kFiltNow, kCapJ);
  TEST_ASSERT_FALSE(v.accepted);
  TEST_ASSERT_TRUE(v.reject == PhPersistReject::kFiltCounter);
  // Régression de 1 seule minute, tout le reste frais : refusée aussi.
  s = sealed(1200.0f, kNow - 60u, kCtxJ, 0, kPhIntegralPersistSchema, kFiltNow + 1u);
  v = phPersistEvaluateRestore(s, kCtxJ, kNow, kFiltNow, kCapJ);
  TEST_ASSERT_FALSE(v.accepted);
  TEST_ASSERT_TRUE(v.reject == PhPersistReject::kFiltCounter);
  // Régression énorme (compteur écrit ≫ courant) : pas d'âge « négatif » relu
  // comme frais ni comme périmé.
  s = sealed(1200.0f, kNow - 60u, kCtxJ, 0, kPhIntegralPersistSchema, 0xFFFFFFF0u);
  v = phPersistEvaluateRestore(s, kCtxJ, kNow, kFiltNow, kCapJ);
  TEST_ASSERT_TRUE(v.reject == PhPersistReject::kFiltCounter);
  // Compteur égal : âge nul, accepté.
  s = sealed(1200.0f, kNow - 60u, kCtxJ, 0, kPhIntegralPersistSchema, kFiltNow);
  TEST_ASSERT_TRUE(phPersistEvaluateRestore(s, kCtxJ, kNow, kFiltNow, kCapJ).accepted);
}

// Ordre kEpoch → kFiltCounter → kStale, chaque étape avec toutes les suivantes
// fautives : epoch futur + compteur reculé + 72 h → kEpoch ; compteur reculé +
// 72 h → kFiltCounter ; 72 h seul → kStale.
void test_J_ordre_epoch_compteur_peremption(void) {
  PhPersistAgeInputs in;
  in.epochWritten = kNow + 1u;          // futur
  in.epochNow = kNow;
  in.filtTotalWritten = kFiltNow + 1u;  // reculé
  in.filtTotalNow = kFiltNow;
  TEST_ASSERT_TRUE(phPersistAge(in).reject == PhPersistReject::kEpoch);
  in.epochWritten = kNow - (kPhIntegralPersistMaxWallAgeS + 1u);  // 72 h + 1
  TEST_ASSERT_TRUE(phPersistAge(in).reject == PhPersistReject::kFiltCounter);
  in.filtTotalWritten = kFiltNow;
  TEST_ASSERT_TRUE(phPersistAge(in).reject == PhPersistReject::kStale);
  // Horloge non sync avec tout le reste faux → kEpoch.
  in.epochNow = 5u;
  in.filtTotalWritten = kFiltNow + 1u;
  TEST_ASSERT_TRUE(phPersistAge(in).reject == PhPersistReject::kEpoch);
}

// Heure non synchronisée : la relecture refuse (kEpoch) ET l'écriture s'abstient
// (kNone), quelle que soit l'échéance de filtration ou la demande de réécriture.
void test_J_heure_non_sync_fail_closed_a_la_relecture_et_a_l_ecriture(void) {
  PhIntegralSnapshot s = sealed(1200.0f, kNow - 60u, kCtxJ);
  TEST_ASSERT_TRUE(phPersistEvaluateRestore(s, kCtxJ, 0u, kFiltNow, kCapJ).reject == PhPersistReject::kEpoch);
  TEST_ASSERT_TRUE(phPersistEvaluateRestore(s, kCtxJ, (uint32_t)kMinValidEpoch - 1u, kFiltNow, kCapJ).reject == PhPersistReject::kEpoch);

  PhPersistWriteInputs w = writeIn(10 * kPhIntegralPersistMinIntervalMs, 1000u, 3000.0f, 0.0f, /*epoch*/0u);
  w.filtMinNow = kFiltNow + 10u * kPhIntegralPersistDueFiltMin;
  w.rewriteWanted = true;
  TEST_ASSERT_TRUE(phPersistWriteReason(w) == PhPersistWriteReason::kNone);
  w.epochNow = kNow;
  TEST_ASSERT_TRUE(phPersistWriteReason(w) == PhPersistWriteReason::kRewrite);
}

// kDelta : variation > 5 % du plafond, cadence respectée → motif kDelta (et pas
// kDue même si l'échéance est aussi atteinte : kDelta est testé d'abord).
void test_J_motif_kDelta(void) {
  const float span = kPhIntegralPersistMinDeltaFrac * kImax;
  const uint32_t t0 = 100000u;
  PhPersistWriteInputs w = writeIn(t0 + kPhIntegralPersistMinIntervalMs, t0, 1000.0f + 1.5f * span, 1000.0f);
  TEST_ASSERT_TRUE(phPersistWriteReason(w) == PhPersistWriteReason::kDelta);
  w.filtMinNow = kFiltNow + kPhIntegralPersistDueFiltMin;
  TEST_ASSERT_TRUE(phPersistWriteReason(w) == PhPersistWriteReason::kDelta);
  // Sous 5 % et sans échéance : kNone.
  w = writeIn(t0 + kPhIntegralPersistMinIntervalMs, t0, 1000.0f + 0.5f * span, 1000.0f);
  TEST_ASSERT_TRUE(phPersistWriteReason(w) == PhPersistWriteReason::kNone);
}

// Revue v2.34.0 (Majeur 1) : pendant la confirmation R4, la RAM vaut 0 alors que
// la NVS porte l'instantané accepté (3 000 pH·s). 30 min plus tard, sans le
// drapeau `candidatePending`, kDelta écraserait la NVS par un zéro — l'intégrale
// serait perdue au reboot suivant. Le drapeau interdit TOUTE écriture (kDelta,
// kDue et même kRewrite) tant que la candidate vit ; une fois la candidate morte
// (drapeau baissé), la cadence normale reprend et le zéro peut s'écrire.
void test_J_candidate_en_attente_interdit_toute_ecriture(void) {
  const uint32_t t0 = 100000u;
  // Sans garde : le scénario du bug — kDelta vers 0.
  PhPersistWriteInputs w = writeIn(t0 + kPhIntegralPersistMinIntervalMs, t0, 0.0f, 3000.0f);
  TEST_ASSERT_TRUE(phPersistWriteReason(w) == PhPersistWriteReason::kDelta);
  // Avec la candidate en attente : rien, même échéance atteinte.
  w.candidatePending = true;
  TEST_ASSERT_TRUE(phPersistWriteReason(w) == PhPersistWriteReason::kNone);
  w.filtMinNow = kFiltNow + kPhIntegralPersistDueFiltMin;
  TEST_ASSERT_TRUE(phPersistWriteReason(w) == PhPersistWriteReason::kNone);
  // Même kRewrite s'incline (cas théorique : pas de candidate sans acceptation,
  // donc pas de rejet — mais la priorité doit être sans ambiguïté).
  w.rewriteWanted = true;
  w.value = 500.0f;
  TEST_ASSERT_TRUE(phPersistWriteReason(w) == PhPersistWriteReason::kNone);
  // Candidate morte : le zéro s'écrit (l'ancien instantané n'était plus restaurable).
  w = writeIn(t0 + kPhIntegralPersistMinIntervalMs, t0, 0.0f, 3000.0f);
  w.candidatePending = false;
  TEST_ASSERT_TRUE(phPersistWriteReason(w) == PhPersistWriteReason::kDelta);
}

// kDue : 180 min de filtration depuis la dernière écriture, valeur INCHANGÉE,
// cadence respectée → écriture d'échéance. 179 min → rien.
void test_J_motif_kDue_a_180_min_de_filtration_sans_delta(void) {
  const uint32_t t0 = 100000u;
  PhPersistWriteInputs w = writeIn(t0 + 4u * kPhIntegralPersistMinIntervalMs, t0, 1500.0f, 1500.0f);
  w.filtMinSaved = kFiltNow;
  w.filtMinNow = kFiltNow + kPhIntegralPersistDueFiltMin;
  TEST_ASSERT_TRUE(phPersistWriteReason(w) == PhPersistWriteReason::kDue);
  TEST_ASSERT_TRUE(phPersistShouldWrite(w));
  w.filtMinNow = kFiltNow + kPhIntegralPersistDueFiltMin - 1u;
  TEST_ASSERT_TRUE(phPersistWriteReason(w) == PhPersistWriteReason::kNone);
  // Valeur légèrement différente (sous 5 %) + échéance → kDue quand même.
  w.filtMinNow = kFiltNow + kPhIntegralPersistDueFiltMin;
  w.value = 1500.0f + 100.0f;
  TEST_ASSERT_TRUE(phPersistWriteReason(w) == PhPersistWriteReason::kDue);
  // Compteur de filtration reculé (NVS effacée) : échéance immédiate (fail-open :
  // une écriture de trop, jamais une de moins).
  w.filtMinNow = kFiltNow - 1u;
  TEST_ASSERT_TRUE(phPersistWriteReason(w) == PhPersistWriteReason::kDue);
}

// Pas de kDue : sous l'échéance ; ou à valeur nulle / négative (rien à préserver,
// la retombée à zéro est couverte par kDelta).
void test_J_pas_de_kDue_sous_l_echeance_ni_a_valeur_nulle(void) {
  const uint32_t t0 = 100000u;
  PhPersistWriteInputs w = writeIn(t0 + 4u * kPhIntegralPersistMinIntervalMs, t0, 1500.0f, 1500.0f);
  for (uint32_t m = 0; m < kPhIntegralPersistDueFiltMin; m += 15u) {
    w.filtMinNow = kFiltNow + m;
    TEST_ASSERT_TRUE(phPersistWriteReason(w) == PhPersistWriteReason::kNone);
  }
  w.filtMinNow = kFiltNow + 10u * kPhIntegralPersistDueFiltMin;
  w.value = 0.0f; w.savedValue = 0.0f;
  TEST_ASSERT_TRUE(phPersistWriteReason(w) == PhPersistWriteReason::kNone);
  w.value = -50.0f; w.savedValue = -50.0f;
  TEST_ASSERT_TRUE(phPersistWriteReason(w) == PhPersistWriteReason::kNone);
}

// L'échéance n'outrepasse pas la cadence R5 : 180 min de filtration mais dernier
// flush il y a 29 min → kNone ; à 30 min → kDue.
void test_J_kDue_respecte_la_cadence_de_30_min(void) {
  const uint32_t t0 = 100000u;
  PhPersistWriteInputs w = writeIn(t0 + kPhIntegralPersistMinIntervalMs - 1u, t0, 1500.0f, 1500.0f);
  w.filtMinNow = kFiltNow + kPhIntegralPersistDueFiltMin;
  TEST_ASSERT_TRUE(phPersistWriteReason(w) == PhPersistWriteReason::kNone);
  w.nowMs = t0 + kPhIntegralPersistMinIntervalMs;
  TEST_ASSERT_TRUE(phPersistWriteReason(w) == PhPersistWriteReason::kDue);
}

// L'échéance se compte en MINUTES DE FILTRATION, pas en minutes murales : 14 h
// murales (nuit) sans filtration → pas d'écriture ; inversement, 180 min de
// filtration en 180 min murales → écriture. (Mutant « fraîcheur/échéance en
// minutes murales ».)
void test_J_kDue_en_minutes_de_filtration_pas_murales(void) {
  const uint32_t t0 = 100000u;
  PhPersistWriteInputs w = writeIn(t0 + 14u * 3600000u, t0, 1500.0f, 1500.0f, kNow + 14u * 3600u);
  w.filtMinNow = kFiltNow;  // 0 min de filtration
  TEST_ASSERT_TRUE(phPersistWriteReason(w) == PhPersistWriteReason::kNone);
  // Côté relecture, la nuit de 14 h reste FRAÎCHE (0 min de filtration).
  TEST_ASSERT_TRUE(fresh(kNow - 14u * 3600u, kNow, kFiltNow, kFiltNow));
  // Et 361 min de filtration en 361 min murales (6 h 01) → périmé : c'est bien la
  // filtration qui vieillit, pas l'horloge seule (6 h murales ne suffiraient pas
  // à périmer avec 0 min de filtration, cf. ci-dessus).
  TEST_ASSERT_FALSE(fresh(kNow - 361u * 60u, kNow, kFiltNow - 361u, kFiltNow));
  TEST_ASSERT_TRUE(fresh(kNow - 361u * 60u, kNow, kFiltNow, kFiltNow));
}

// kRewrite (C5) : après un rejet au boot, la première intégrale POSITIVE est
// écrite sans attendre ni la cadence ni l'amplitude ; à valeur nulle, rien
// (il n'y a encore rien à préserver) ; le drapeau levé sans intégrale ne fait
// pas écrire une valeur négative.
void test_J_motif_kRewrite_apres_rejet_des_la_premiere_integrale_positive(void) {
  PhPersistWriteInputs w = writeIn(5000u, 4000u, 0.0f, 0.0f);  // 1 s après un flush
  w.rewriteWanted = true;
  TEST_ASSERT_TRUE(phPersistWriteReason(w) == PhPersistWriteReason::kNone);
  w.value = 1.0f;  // première intégrale positive, minuscule (≪ 5 %)
  TEST_ASSERT_TRUE(phPersistWriteReason(w) == PhPersistWriteReason::kRewrite);
  w.value = -1.0f;
  TEST_ASSERT_TRUE(phPersistWriteReason(w) == PhPersistWriteReason::kNone);
  // Drapeau baissé : retour au régime nominal (cadence non respectée → kNone).
  w.value = 1.0f; w.rewriteWanted = false;
  TEST_ASSERT_TRUE(phPersistWriteReason(w) == PhPersistWriteReason::kNone);
  // kRewrite prime sur kDelta et kDue.
  w.rewriteWanted = true; w.value = 5000.0f; w.filtMinNow = kFiltNow + 1000u;
  TEST_ASSERT_TRUE(phPersistWriteReason(w) == PhPersistWriteReason::kRewrite);
}

// Simulation d'une journée : 12 h de filtration continue, tick par minute,
// intégrale STABLE (3000 pH·s, jamais de kDelta après la 1re écriture). La
// coquille écrit quand le module le dit et met à jour lastSaveMs/filtMinSaved/
// savedValue. Attendu : ≤ 6 écritures (1 initiale + 4 échéances = 5), et à TOUT
// instant un instantané d'âge de filtration ≤ 180 min < 360 (frais).
static uint32_t simulateFiltrationHours(uint32_t hours, float value, uint32_t* maxAgeMin) {
  uint32_t writes = 0;
  uint32_t lastSaveMs = 0;
  uint32_t filtSaved = kFiltNow;
  float saved = 0.0f;
  uint32_t worst = 0;
  for (uint32_t m = 0; m <= hours * 60u; ++m) {
    PhPersistWriteInputs w;
    w.nowMs = 1000u + m * 60000u;
    w.lastSaveMs = lastSaveMs;
    w.epochNow = kNow + m * 60u;
    w.value = value;
    w.savedValue = saved;
    w.integralMax = kImax;
    w.filtMinNow = kFiltNow + m;
    w.filtMinSaved = filtSaved;
    if (phPersistShouldWrite(w)) {
      ++writes;
      lastSaveMs = w.nowMs;
      filtSaved = w.filtMinNow;
      saved = value;
    }
    const uint32_t age = w.filtMinNow - filtSaved;
    if (age > worst) worst = age;
    // Invariant : l'instantané persistant est TOUJOURS frais.
    TEST_ASSERT_TRUE(fresh(kNow, w.epochNow, filtSaved, w.filtMinNow));
  }
  if (maxAgeMin) *maxAgeMin = worst;
  return writes;
}

void test_J_douze_heures_de_filtration_au_plus_six_ecritures(void) {
  uint32_t worst = 0;
  const uint32_t writes = simulateFiltrationHours(12, 3000.0f, &worst);
  char msg[96];
  snprintf(msg, sizeof(msg), "%u ecritures sur 12 h (attendu <= 6), age max %u min", writes, worst);
  TEST_ASSERT_TRUE_MESSAGE(writes <= 6, msg);
  TEST_ASSERT_TRUE_MESSAGE(writes >= 4, msg);  // 1 + échéances 180/360/540 au moins
  TEST_ASSERT_TRUE_MESSAGE(worst <= kPhIntegralPersistDueFiltMin, msg);
  TEST_ASSERT_TRUE(worst < kPhIntegralPersistMaxFiltMin);
}

// 8 h de filtration (journée d'été typique) : au moins une écriture d'échéance
// après l'initiale, l'instantané reste frais tout le long.
void test_J_huit_heures_de_filtration_au_moins_une_ecriture(void) {
  uint32_t worst = 0;
  const uint32_t writes = simulateFiltrationHours(8, 3000.0f, &worst);
  char msg[96];
  snprintf(msg, sizeof(msg), "%u ecritures sur 8 h (attendu >= 2), age max %u min", writes, worst);
  TEST_ASSERT_TRUE_MESSAGE(writes >= 2, msg);
  TEST_ASSERT_TRUE_MESSAGE(writes <= 5, msg);
  TEST_ASSERT_TRUE(worst < kPhIntegralPersistMaxFiltMin);
}

void test_J_contrat_numerique_feature_093(void) {
  TEST_ASSERT_EQUAL_UINT8(2, kPhIntegralPersistSchema);
  TEST_ASSERT_EQUAL_UINT32(360, kPhIntegralPersistMaxFiltMin);
  TEST_ASSERT_EQUAL_UINT32(72u * 3600u, kPhIntegralPersistMaxWallAgeS);
  TEST_ASSERT_EQUAL_UINT32(180, kPhIntegralPersistDueFiltMin);
  // L'échéance est strictement inférieure à la péremption : deux échéances
  // ratées d'affilée sont nécessaires pour périmer.
  TEST_ASSERT_TRUE(2u * kPhIntegralPersistDueFiltMin <= kPhIntegralPersistMaxFiltMin);
  TEST_ASSERT_TRUE(kPhIntegralPersistMinIntervalMs <= kPhIntegralPersistDueFiltMin * 60000u);
}

// =============================================================================

int main(int argc, char** argv) {
  (void)argc; (void)argv;
  UNITY_BEGIN();
  // A — intégrité du blob
  RUN_TEST(test_A_disposition_du_blob);
  RUN_TEST(test_A_fnv1a_vecteurs_connus);
  RUN_TEST(test_A_blob_scelle_est_accepte);
  RUN_TEST(test_A_blob_non_scelle_est_rejete);
  RUN_TEST(test_A_tout_bit_retourne_est_rejete);
  RUN_TEST(test_A_troncature_ou_surplus_est_rejete);
  RUN_TEST(test_A_scellement_normalise_le_trou_d_alignement);
  RUN_TEST(test_A_un_autre_schema_reste_intact_mais_est_refuse);
  RUN_TEST(test_A_l_ordre_des_controles_est_stable);
  // B — empreinte de contexte
  RUN_TEST(test_B_meme_contexte_meme_empreinte);
  RUN_TEST(test_B_chaque_composante_change_l_empreinte);
  RUN_TEST(test_B_contexte_different_est_refuse);
  // C — R1, double plafond
  RUN_TEST(test_C_reference_la_charge_observee_gouverne);
  RUN_TEST(test_C_charge_forte_la_butee_absolue_gouverne);
  RUN_TEST(test_C_bascule_entre_les_deux_branches);
  RUN_TEST(test_C_le_plafond_reste_sous_l_equilibre_pour_toute_charge);
  RUN_TEST(test_C_SENTINELLE_la_butee_seule_depasserait_l_equilibre_des_petits_bassins);
  RUN_TEST(test_C_historique_trop_court_ferme_le_plafond);
  RUN_TEST(test_C_reglages_inertes_ferment_le_plafond);
  RUN_TEST(test_C_charge_sous_l_apport_proportionnel_ferme_le_plafond);
  RUN_TEST(test_C_ecretage_borne_la_valeur_au_plafond);
  RUN_TEST(test_C_une_valeur_excessive_est_acceptee_mais_ramenee_au_plafond);
  RUN_TEST(test_C_plafond_nul_refuse_une_restauration_par_ailleurs_valide);
  RUN_TEST(test_C_valeur_nulle_ou_non_finie_est_refusee);
  // D — R3, péremption
  RUN_TEST(test_D_frais_a_5h59_perime_a_6h01);
  RUN_TEST(test_D_frontiere_exacte_de_la_peremption);
  RUN_TEST(test_D_horloge_inexploitable_vaut_perime);
  RUN_TEST(test_D_horodatage_nul_ou_futur_est_incoherent);
  RUN_TEST(test_D_peremption_dans_la_decision);
  // E — R4, confirmation
  RUN_TEST(test_E_sans_candidate_la_machine_est_inerte);
  RUN_TEST(test_E_confirmation_apres_cinq_minutes_continues);
  RUN_TEST(test_E_ecart_retombe_remet_la_tenue_a_zero);
  RUN_TEST(test_E_ecart_egal_au_seuil_ou_negatif_ne_compte_pas);
  RUN_TEST(test_E_dt_aberrant_n_est_jamais_credite);
  RUN_TEST(test_E_interruption_conserve_la_candidate_abandon_la_detruit);
  RUN_TEST(test_E_candidate_perimee_avant_confirmation_est_abandonnee);
  RUN_TEST(test_E_le_plafond_courant_est_reapplique_a_l_application);
  RUN_TEST(test_E_plafond_courant_nul_annule_la_candidate_avec_son_propre_code);
  RUN_TEST(test_E_scenario_complet_de_la_lecture_a_l_application);
  // F — R7, anti-boucle
  RUN_TEST(test_F_desarmement_au_troisieme);
  RUN_TEST(test_F_increment_sature_a_255);
  RUN_TEST(test_F_boucle_de_boot_trois_essais_puis_refus);
  RUN_TEST(test_F_une_ecriture_normale_reamorce_la_persistance);
  // G — R5, cadence d'écriture
  RUN_TEST(test_G_les_deux_conditions_sont_conjonctives);
  RUN_TEST(test_G_frontieres_exactes);
  RUN_TEST(test_G_premiere_ecriture_ignore_la_cadence);
  RUN_TEST(test_G_heure_invalide_interdit_l_ecriture);
  RUN_TEST(test_G_echelle_absente_ou_valeur_non_finie_interdit_l_ecriture);
  RUN_TEST(test_G_wrap_de_millis);
  // H — contrat numérique et journalisation
  RUN_TEST(test_H_contrat_numerique_du_module);
  RUN_TEST(test_H_libelles_de_rejet);
  // I — l'anneau de charge : datation, fenêtre, vraisemblance, médiane
  RUN_TEST(test_I_ecarts_de_jours_aux_frontieres_du_calendrier);
  RUN_TEST(test_I_date_malformee_rend_zero);
  RUN_TEST(test_I_fenetre_fermee_a_sept_jours);
  RUN_TEST(test_I_date_future_ou_horloge_absente_ecarte_l_echantillon);
  RUN_TEST(test_I_criteres_cumulatifs_du_jour_exploitable);
  RUN_TEST(test_I_journee_physiquement_impossible_est_ecartee);
  RUN_TEST(test_I_SENTINELLE_le_seuil_suit_la_limite_horaire_configuree);
  RUN_TEST(test_I_repli_quand_le_debit_soutenable_est_indisponible);
  RUN_TEST(test_I_journee_impossible_ne_pese_pas_dans_la_mediane);
  RUN_TEST(test_I_mediane_impaire_et_paire);
  RUN_TEST(test_I_trois_journees_aberrantes_sur_sept_ne_deplacent_pas_la_mediane);
  RUN_TEST(test_I_SENTINELLE_journees_hors_saison_ne_pesent_plus_dans_la_mediane);
  RUN_TEST(test_I_deux_journees_recentes_seulement_ferment_le_plafond);
  RUN_TEST(test_I_anneau_entierement_perime_ferme_le_plafond);
  RUN_TEST(test_I_sans_date_du_jour_l_anneau_entier_est_ecarte);
  RUN_TEST(test_I_insertion_decale_et_evince_la_plus_ancienne);
  RUN_TEST(test_I_deduplication_balaie_toutes_les_cases);
  RUN_TEST(test_I_anneau_scelle_est_accepte_toute_alteration_est_rejetee);
  RUN_TEST(test_I_scellement_deterministe);
  // J — feature-093
  RUN_TEST(test_J_aller_retour_24_octets_schema_2);
  RUN_TEST(test_J_schema_1_est_rejete_avant_tout_autre_controle);
  RUN_TEST(test_J_nuit_sans_filtration_puis_reboot_accepte);
  RUN_TEST(test_J_reboot_en_cours_de_filtration_accepte);
  RUN_TEST(test_J_361_min_de_filtration_rejete_meme_a_1h_murale);
  RUN_TEST(test_J_72h_plus_1s_rejete_meme_a_0_min_de_filtration);
  RUN_TEST(test_J_compteur_non_monotone_avec_72h_c_est_le_compteur_qui_parle);
  RUN_TEST(test_J_ordre_epoch_compteur_peremption);
  RUN_TEST(test_J_heure_non_sync_fail_closed_a_la_relecture_et_a_l_ecriture);
  RUN_TEST(test_J_motif_kDelta);
  RUN_TEST(test_J_candidate_en_attente_interdit_toute_ecriture);
  RUN_TEST(test_J_motif_kDue_a_180_min_de_filtration_sans_delta);
  RUN_TEST(test_J_pas_de_kDue_sous_l_echeance_ni_a_valeur_nulle);
  RUN_TEST(test_J_kDue_respecte_la_cadence_de_30_min);
  RUN_TEST(test_J_kDue_en_minutes_de_filtration_pas_murales);
  RUN_TEST(test_J_motif_kRewrite_apres_rejet_des_la_premiere_integrale_positive);
  RUN_TEST(test_J_douze_heures_de_filtration_au_plus_six_ecritures);
  RUN_TEST(test_J_huit_heures_de_filtration_au_moins_une_ecriture);
  RUN_TEST(test_J_contrat_numerique_feature_093);
  return UNITY_END();
}
