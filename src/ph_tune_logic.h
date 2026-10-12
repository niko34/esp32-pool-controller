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
//   4. écart hors bornes APRÈS calcul (arrondi au pas, kPhOffsetMin/Max)
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
  OutOfRange               = 4,  // écart hors kPhOffsetMin/Max après calcul
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

  // Refus 4/4 — écart hors ±0,50 pH. Bornes INCHANGÉES (kPhOffsetMin/Max) :
  // kPhFloorRawHard/kPhCeilingRawHard en dérivent.
  //
  // 🔴 CORRIGÉ : LE TEST PORTE SUR L'ÉCART **DEMANDÉ**, PAS SUR LA SORTIE DE
  // roundPhOffsetToStep(). La version précédente écrivait
  //     offsetRounded = roundPhOffsetToStep(phReference - phValueRaw);
  //     if (!isPhOffsetValid(offsetRounded)) ...
  // or roundPhOffsetToStep() **CLAMPE AVANT D'ARRONDIR** (ph_guard_logic.h) :
  // sa sortie est donc TOUJOURS dans [kPhOffsetMin, kPhOffsetMax], et
  // isPhOffsetValid() sur cette sortie est STRUCTURELLEMENT TOUJOURS VRAI.
  // ⇒ Le refus OutOfRange était **MORT, INATTEIGNABLE**. Un écart de +0,80
  // n'aurait pas été refusé : il aurait été **SILENCIEUSEMENT ÉCRÊTÉ à +0,50
  // ET APPLIQUÉ** — l'exact inverse du critère d'acceptation de la spec, et une
  // violation directe de l'invariant du gate « refus, JAMAIS écrêtage
  // silencieux ». Sur une entrée qui pilote le dosage d'acide, l'utilisateur
  // aurait vu son aperçu annoncer un refus pendant que l'appareil appliquait
  // la moitié de sa demande. Trouvé par web-ui-developer en câblant l'aperçu
  // client, PAS par la relecture du lot firmware.
  // ⚠️ roundPhOffsetToStep() reste INCHANGÉE : son écrêtage est VOULU pour son
  // autre usage (migration NVS au boot, feature-067), où il n'y a pas
  // d'utilisateur à qui opposer un refus.
  if (!isPhOffsetValid(offsetDemande)) {
    result.refusal = PhTuneRefusal::OutOfRange;
    return result;
  }

  // L'arrondi n'intervient qu'APRÈS acceptation : à ce stade l'écart est déjà
  // dans la plage, le clamp de roundPhOffsetToStep() ne peut plus rien masquer.
  const float offsetRounded = roundPhOffsetToStep(offsetDemande);

  result.refusal = PhTuneRefusal::None;
  result.offsetApplied = offsetRounded;
  return result;
}

#endif  // PH_TUNE_LOGIC_H
