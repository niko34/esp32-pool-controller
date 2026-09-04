#ifndef PURGE_STABILITY_LOGIC_H
#define PURGE_STABILITY_LOGIC_H

// =============================================================================
// purge_stability_logic — Détecteur PUR de stabilité des bruts pH/ORP
// (feature-091, v2.34.0)
// =============================================================================
// Répond à la question « les bruts pH/ORP ont-ils cessé de bouger ? » pour
// piloter la fin de la purge de reprise d'eau (measure_validity_logic). Module
// pur au sens de l'ADR-0017 : aucune API Arduino, aucun millis(), aucune
// allocation — l'appelant fournit les lectures et les paramètres, l'état vit
// dans une struct qu'il possède.
//
// PRINCIPE :
//   - chaque VOIE (pH brut, ORP brut) accumule les lectures d'une « minute »
//     (params.samplesPerMin lectures valides, 12 à la cadence EZO de 5 s) puis
//     en calcule la MÉDIANE (robuste à une lecture aberrante isolée) ;
//   - les médianes alimentent un anneau glissant de params.windowMin minutes ;
//   - la voie est STABLE quand l'anneau est PLEIN et que son étendue
//     (max − min des médianes) est ≤ params.*StableRange ;
//   - la voie est INSTABLE (critère de re-fermeture, C1 pool-chemistry) quand
//     l'anneau est plein et que son étendue est > params.*RecloseRange (2× le
//     seuil d'ouverture : hystérésis).
//
// AGRÉGATION des deux voies :
//   - une voie JAMAIS lue depuis le reset (NaN permanent : module absent, sonde
//     débranchée) est IGNORÉE — l'autre voie décide seule ;
//   - une voie lue au moins une fois mais dont l'anneau n'est pas encore plein
//     est NON STABLE (elle bloque l'ouverture — pas d'ouverture sur une preuve
//     partielle) et NON INSTABLE (elle ne referme pas) ;
//   - stable = au moins une voie pleine ET toutes les voies lues sont stables ;
//   - unstable = au moins une voie pleine ET instable.
//   - aucune voie jamais lue ⇒ ni stable ni instable (la porte attendra le
//     plafond, fail-safe).
//
// Les lectures NaN (stale, erreur I²C) sont simplement ignorées : elles ne
// comptent pas dans la minute. La « minute » est donc définie en NOMBRE de
// lectures valides, pas en temps mural — sans horloge, le module reste pur ;
// à 5 s de cadence les deux coïncident.
//
// Le min/max des BRUTS depuis le reset est conservé pour le journal de fin de
// purge (« [STAB] … pH 7,02→7,31 »).
//
// CONTRAINTE : pas d'Arduino.h, pas de <String>. <stdint.h> et <math.h> (isnan)
// uniquement, pour compiler en natif (pio test -e native).
// =============================================================================

#include <stdint.h>

// Bornes de dimensionnement des tampons (statiques : aucune allocation). Les
// paramètres passés à l'exécution sont bornés à ces valeurs.
constexpr uint8_t kPurgeStabilityMaxSamplesPerMin = 60;
constexpr uint8_t kPurgeStabilityMaxWindowMin     = 10;

// Paramètres du détecteur — fournis par l'appelant (constants.h côté cible,
// valeurs choisies par le test côté natif).
struct PurgeStabilityParams {
  uint8_t samplesPerMin;   // lectures valides par « minute » (kPurgeSamplesPerMin)
  uint8_t windowMin;       // minutes de la fenêtre glissante (kPurgeWindowMin)
  float phStableRange;     // étendue max des médianes pH pour « stable »
  float orpStableRange;    // idem ORP (mV)
  float phRecloseRange;    // étendue au-delà de laquelle la voie est « instable »
  float orpRecloseRange;   // idem ORP (mV)
};

// État d'UNE voie. Zéro-initialisé = « jamais lue ». Ne pas lire les champs
// directement hors des accesseurs ci-dessous (représentation interne).
struct PurgeChannelState {
  float minuteBuf[kPurgeStabilityMaxSamplesPerMin];  // lectures de la minute en cours
  uint8_t minuteCount;                                // lectures valides accumulées
  float medians[kPurgeStabilityMaxWindowMin];         // anneau de médianes-minute
  uint8_t medianCount;                                // médianes valides (≤ windowMin)
  uint8_t medianHead;                                 // prochaine position d'écriture
  bool everSampled;                                   // ≥ 1 lecture valide depuis reset
  float rawMin;                                       // min des bruts depuis reset
  float rawMax;                                       // max des bruts depuis reset
};

// État complet du détecteur (les deux voies).
struct PurgeStabilityState {
  PurgeChannelState ph;
  PurgeChannelState orp;
};

// Verdict agrégé d'un instant.
struct PurgeStabilityVerdict {
  bool stable;        // fenêtre pleine ET étendue ≤ seuil sur toutes les voies lues
  bool unstable;      // une voie pleine dépasse le seuil de re-fermeture
  bool anyFull;       // au moins une voie a sa fenêtre pleine
  float phRange;      // étendue courante des médianes pH (NaN si < 1 médiane)
  float orpRange;     // idem ORP
};

// Remet le détecteur à « jamais lu » (début de purge : reprise d'eau OU
// re-fermeture — chaque purge est jugée sur ses propres minutes).
void purgeStabilityReset(PurgeStabilityState& st);

// Ajoute une lecture (pH brut, ORP brut). NaN sur une voie = pas de lecture
// pour cette voie à cet instant. Les paramètres sont bornés aux tampons.
void purgeStabilityAddSample(PurgeStabilityState& st, const PurgeStabilityParams& p,
                             float phRaw, float orpRaw);

// Verdict agrégé (voir règles en tête de fichier).
PurgeStabilityVerdict purgeStabilityEvaluate(const PurgeStabilityState& st,
                                             const PurgeStabilityParams& p);

// --- Accesseurs de diagnostic / journal -------------------------------------
// Étendue (max − min) des médianes d'une voie ; NaN si aucune médiane.
float purgeChannelRange(const PurgeChannelState& ch);
// Nombre de médianes-minute disponibles (0..windowMin).
uint8_t purgeChannelMinutes(const PurgeChannelState& ch);
// min/max des BRUTS depuis le reset (NaN si jamais lue).
float purgeChannelRawMin(const PurgeChannelState& ch);
float purgeChannelRawMax(const PurgeChannelState& ch);

// Médiane d'un tableau de n flottants (n ≥ 1), sans modifier l'entrée. Exposée
// pour les tests (tri par insertion sur une copie bornée).
float purgeStabilityMedian(const float* v, uint8_t n);

#endif  // PURGE_STABILITY_LOGIC_H
