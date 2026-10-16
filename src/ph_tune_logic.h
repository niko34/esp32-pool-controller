#ifndef PH_TUNE_LOGIC_H
#define PH_TUNE_LOGIC_H

// =============================================================================
// ph_tune_logic — Accord de la mesure pH sur un test manuel, logique PURE
// (feature-155)
// =============================================================================
// L'utilisateur AMÈNE un nombre affiché à la valeur qu'il a relevée sur son
// test (bandelette, PoolLab, pH-mètre). Ce module décide SI l'appareil peut en
// déduire un offset, et lequel — c'est la fonction appelée par
// web_routes_config.cpp::handleTunePhOffset, extraite ici pour rester
// testable en natif (ADR-0017) sans AsyncWebServer ni matériel.
//
// CONDITION 1 DU GATE pool-chemistry (change l'arbitrage initial de
// l'architect) : LE FIRMWARE calcule l'écart, avec SA mesure au moment de la
// réception (`phValueRaw`), jamais une valeur amenée par le navigateur sur une
// référence qui aurait vieilli pendant le geste — c'est pour cela que cette
// fonction ne reçoit PAS d'offset déjà calculé, mais les deux valeurs brutes.
//
// ORDRE DES CONTRÔLES IMPOSÉ par le gate, NE PAS RÉORDONNER (chaque test
// matérialise un refus distinct, imposé mot pour mot côté appelant) :
//   1. mesure de référence indisponible (NaN)
//   2. régime hydraulique pas établi depuis assez longtemps (filtration)
//   3. sonde non étalonnée (calPoints insuffisants)
//   4. écart hors SALUBRITÉ de domaine (feature-168 : ce n'est plus une borne
//      de réglage, isPhOffsetValid() ne refuse plus que NaN/inf/|écart| > 14)
//
// ⚠️ LOAD-BEARING depuis feature-158 (condition 8 du gate pool-chemistry) :
// le refus n°1 (isnan) DOIT rester STRICTEMENT AVANT le refus n°2
// (filtration). Depuis que la durée de filtration peut survivre à un
// redémarrage (feature-158), le refus n°2 peut être franchi dès les toutes
// premières secondes après un boot — c'est désormais le refus n°1, et LUI
// SEUL, qui protège la fenêtre de médiane pH (~45 s) tant qu'elle n'est pas
// remplie. Réordonner ces deux tests ouvrirait une fenêtre où une mesure pH
// non représentative pourrait servir de référence à un accord.
//
// PIÈGE FERMÉ ICI (identifié par l'orchestrator avant codage) : l'appelant
// DOIT passer `phValueRaw` — la valeur OFFICIELLE échelle BRUTE
// (sensors.getPhValueRaw()) — jamais `ph`/`phFiltered`/`phMedian`, qui sont
// déjà CORRIGÉS de l'offset courant. Partir d'une valeur déjà corrigée
// ajouterait l'écart à un écart déjà appliqué : défaut INVISIBLE à offset
// 0,00 (celui de l'utilisateur aujourd'hui), faux dès que l'offset ne l'est
// plus.
//
// CONTRAINTE (style ADR-0017, comme ph_offset / ph_guard_logic) : pas
// d'Arduino.h, pas de FreeRTOS, pas de String — en-têtes C uniquement, plus
// constants.h qui est self-contained et porte les seuils (source unique).
// =============================================================================

#include <math.h>
#include <stdint.h>

#include "constants.h"    // kPhTuneMinFiltrationMs, kPhTuneCalPointsMin
#include "ph_offset.h"    // isPhOffsetValid
#include "ph_guard_logic.h"  // roundPhOffsetToStep

// Code du refus (0 = accordé). Codes STABLES : un futur appelant (MQTT ?) peut
// s'y accrocher sans dépendre des textes, qui vivent côté HTTP.
enum class PhTuneRefusal : uint8_t {
  None = 0,
  MeasureUnavailable       = 1,  // isnan(phValueRaw)
  FiltrationNotEstablished = 2,  // arrêtée OU trop récente
  ProbeNotCalibrated       = 3,  // calPoints < kPhTuneCalPointsMin
  OutOfRange               = 4,  // écart hors salubrité (NaN/inf/|écart| > 14, feature-168)
};

struct PhTuneResult {
  PhTuneRefusal refusal = PhTuneRefusal::None;
  // Valide seulement si refusal == None : l'offset RÉELLEMENT à appliquer
  // (déjà arrondi au pas, cf. roundPhOffsetToStep) — PAS la valeur brute
  // amenée par l'utilisateur, que l'arrondi peut légèrement déplacer
  // (Piège 1 : amener 7,40 pile quand l'appareil lit 7,13 donne 0,27, arrondi
  // à 0,30 — l'appareil atteindra 7,43, pas 7,40).
  float offsetApplied = 0.0f;
  // Valide seulement si refusal == FiltrationNotEstablished : temps restant
  // avant que kPhTuneMinFiltrationMs soit atteint. Filtration à l'arrêt ⇒
  // l'appelant doit avoir passé filtrationElapsedMs = 0 (régime hydraulique
  // INEXISTANT, pas juste récent) : le reste annoncé est alors le délai PLEIN,
  // ce qui reste vrai — il faudra bien attendre ce temps APRÈS le démarrage.
  unsigned long remainingFiltrationMs = 0;
};

inline PhTuneResult evaluatePhTune(float phReference, float phValueRaw,
                                    bool filtrationRunning,
                                    unsigned long filtrationElapsedMs,
                                    int calPoints) {
  PhTuneResult result;

  // Refus 1/4 — mesure de référence indisponible. Couvre remplissage de
  // fenêtre, sonde figée, porte fermée, péremption : un seul test isnan()
  // suffit (5 étages du filtre, feature-152).
  if (isnan(phValueRaw)) {
    result.refusal = PhTuneRefusal::MeasureUnavailable;
    return result;
  }

  // Refus 2/4 — régime hydraulique pas établi. kPhTuneMinFiltrationMs
  // (constants.h) : plancher retenu par l'utilisateur après que l'option
  // « 3 min » lui a été proposée et explicitement écartée (2026-10-11).
  const unsigned long elapsed = filtrationRunning ? filtrationElapsedMs : 0UL;
  if (!filtrationRunning || elapsed < kPhTuneMinFiltrationMs) {
    result.refusal = PhTuneRefusal::FiltrationNotEstablished;
    result.remainingFiltrationMs = kPhTuneMinFiltrationMs - elapsed;
    return result;
  }

  // Refus 3/4 — sonde non étalonnée (réponse d du gate : un offset est une
  // CONSTANTE, une erreur d'étalonnage est PROPORTIONNELLE — sans 2ᵉ point,
  // une pente dégradée serait figée dans l'offset au seul point d'accord).
  if (calPoints < (int)kPhTuneCalPointsMin) {
    result.refusal = PhTuneRefusal::ProbeNotCalibrated;
    return result;
  }

  // Calcul — condition 1 du gate : l'écart est calculé ICI, avec phValueRaw
  // passé par l'appelant (sa mesure à l'instant de la réception), jamais une
  // valeur pré-calculée par le navigateur.
  const float offsetDemande = phReference - phValueRaw;

  // Refus 4/4 — salubrité de domaine (feature-168). ⚠️ CE N'EST PLUS UNE BORNE
  // DE RÉGLAGE : depuis feature-168, isPhOffsetValid() n'écarte plus que
  // NaN/infini/|écart| > kPhOffsetScaleSpan (14 pH, l'échelle entière). Un
  // écart « inhabituel » (> kPhOffsetUnusualAbs = 0,50) n'est PLUS refusé ici —
  // il est accepté et signalé ailleurs (avertissement UI C8, log+alerte C7).
  //
  // 🔴 Historique (feature-155) — pourquoi ce test porte sur l'écart
  // **DEMANDÉ**, jamais sur la sortie de roundPhOffsetToStep() : une version
  // antérieure écrivait `isPhOffsetValid(roundPhOffsetToStep(ecart))`, or
  // roundPhOffsetToStep() CLAMPAIT AVANT D'ARRONDIR — sa sortie était donc
  // TOUJOURS dans la plage autorisée, rendant ce refus MORT, INATTEIGNABLE :
  // un écart hors plage était silencieusement ÉCRÊTÉ ET APPLIQUÉ au lieu
  // d'être refusé. ⚠️ Ce piège a CHANGÉ DE NATURE avec feature-168 :
  // roundPhOffsetToStep() n'écrête plus DU TOUT (son clamp a été retiré, voir
  // ph_guard_logic.h) — la leçon reste la même, vérifier EN AMONT de
  // l'arrondi, jamais sur sa sortie.
  if (!isPhOffsetValid(offsetDemande)) {
    result.refusal = PhTuneRefusal::OutOfRange;
    return result;
  }

  // L'arrondi n'intervient qu'APRÈS acceptation : à ce stade l'écart est déjà
  // valide (salubrité), et roundPhOffsetToStep() n'ARRONDIT que — elle n'a
  // plus aucun clamp susceptible de masquer quoi que ce soit (feature-168).
  const float offsetRounded = roundPhOffsetToStep(offsetDemande);

  result.refusal = PhTuneRefusal::None;
  result.offsetApplied = offsetRounded;
  return result;
}

#endif  // PH_TUNE_LOGIC_H
