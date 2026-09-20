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
//     en calcule la MOYENNE INTERQUARTILE (voir plus bas — feature-097) ;
//   - les statistiques de bloc alimentent un anneau glissant de
//     params.windowMin minutes ;
//   - la voie est STABLE quand l'anneau est PLEIN et que son étendue
//     (max − min des statistiques de bloc) est ≤ params.*StableRange ;
//   - la voie est INSTABLE (critère de re-fermeture, C1 pool-chemistry) quand
//     l'anneau est plein et que son étendue est > params.*RecloseRange (2× le
//     seuil d'ouverture : hystérésis).
//
// POURQUOI LA MÉDIANE A ÉTÉ ABANDONNÉE (feature-097, mesure du 2026-09-20) :
//   le pH brut de ce bassin n'est pas du bruit autour d'une moyenne — c'est un
//   signal ANTI-CORRÉLÉ, presque à DEUX NIVEAUX (autocorrélation au lag 1 :
//   −0,440 ; saut moyen entre deux lectures : 1,47 σ). Bloc réel mesuré
//   (minute 22, trace du 20/09) :
//     7.051 · 6.747 · 7.083 · 6.752 · 7.083 · 6.743 · 7.085 · 6.775 · 7.079 ·
//     6.749 · 7.079 · 7.068
//   — deux niveaux séparés de ~0,21, en alternance quasi stricte. Sur un tel
//   signal, la MÉDIANE N'EST PAS UNE MOYENNE ROBUSTE : c'est un INTERRUPTEUR.
//   Sept lectures hautes contre cinq et elle saute sur le niveau haut ; cinq
//   contre sept et elle retombe sur le niveau bas. Ce sont ces bascules — pas
//   une excursion réelle de l'eau — qui produisaient la majorité des fenêtres
//   en échec sur données terrain (3 sur 16). La moyenne interquartile, elle,
//   moyenne les deux niveaux au lieu d'en choisir un : elle absorbe
//   l'alternance au lieu de trancher dessus.
//   ⚠️ NE PAS RESTAURER LA MÉDIANE « pour la robustesse » : c'est précisément
//   l'argument qui l'avait fait choisir à l'origine (ADR-0059 §1.2, amendé par
//   feature-097) — il est valide contre une lecture aberrante ISOLÉE, pas
//   contre un signal bimodal. Le point de rupture recule de 50 % à 25 % des
//   lectures d'un bloc (arbitrage explicite pool-chemistry, feature-097) :
//   c'est le prix payé, en échange de la disparition des bascules.
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
  // Anneau des statistiques de bloc (moyenne interquartile depuis feature-097 ;
  // le nom du champ garde « medians » pour ne pas remuer tout le fichier —
  // c'est la valeur de purgeStabilityBlockStat() qui y est stockée).
  float medians[kPurgeStabilityMaxWindowMin];
  uint8_t medianCount;                                // statistiques valides (≤ windowMin)
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
// Étendue (max − min) des statistiques de bloc d'une voie ; NaN si aucun bloc.
float purgeChannelRange(const PurgeChannelState& ch);
// Nombre de statistiques de bloc disponibles (0..windowMin).
uint8_t purgeChannelMinutes(const PurgeChannelState& ch);
// min/max des BRUTS depuis le reset (NaN si jamais lue).
float purgeChannelRawMin(const PurgeChannelState& ch);
float purgeChannelRawMax(const PurgeChannelState& ch);

// Médiane d'un tableau de n flottants (n ≥ 1), sans modifier l'entrée. N'est
// plus l'estimateur de bloc utilisé par channelAdd() depuis feature-097 (voir
// plus haut), mais reste exportée : elle sert de référence de comparaison aux
// tests, et pour n ≤ 2 elle EST la définition de purgeStabilityBlockStat().
float purgeStabilityMedian(const float* v, uint8_t n);

// Statistique de bloc retenue (feature-097) : moyenne interquartile — on écarte
// trim = max(1, n/4) valeurs les plus basses et trim les plus hautes, on
// moyenne les n − 2·trim valeurs centrales. Pour n ≤ 2, retombe sur la médiane
// (rien à écarter). Sans modifier l'entrée (tri sur une copie bornée, tri par
// insertion partagé avec purgeStabilityMedian()). Exposée pour les tests.
float purgeStabilityBlockStat(const float* v, uint8_t n);

#endif  // PURGE_STABILITY_LOGIC_H
