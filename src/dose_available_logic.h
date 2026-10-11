#ifndef DOSE_AVAILABLE_LOGIC_H
#define DOSE_AVAILABLE_LOGIC_H

#include <stdint.h>

// =============================================================================
// dose_available_logic — Délai PUR avant dosage automatique (feature-139,
// v2.62.0, B1 gate pool-chemistry 2e passage ; defaut residuel trouve et
// corrige par test-engineer au moment de l'extraction, meme passage)
// =============================================================================
// ADR-0017, idiome de heap_drop_logic.h / acct_window_logic.h : AUCUNE
// dépendance Arduino/FreeRTOS ici — header-only, inline, testable en natif.
// La coquille impure (PumpControllerClass::getDoseAvailableInS(), qui lit
// measureValidity.getSnapshot() et millis()) vit dans pump_controller.cpp.
//
// ⚠️ feature-152 : le plancher de GEL DU FILTRE capteur (ex-kSensorFilterGateFloorMs,
// sensor_filter.{h,cpp}) a été RETIRÉ — la porte d'alimentation du filtre est
// désormais simplifiée (vider/remplir, sans plancher ni ré-amorçage). La
// coquille transmet donc `filterRemainingMs = 0` en permanence ; la formule
// à deux planchers est conservée TELLE QUELLE (générique) pour un éventuel
// futur plancher indépendant, plutôt que simplifiée à un seul terme.
//
// UN SEUL PLANCHER reste pertinent aujourd'hui :
//   - le reste du plancher de PURGE de measure_validity (kPurgeFloorMs,
//     10 min) — pertinent UNIQUEMENT tant que measure_validity n'a pas encore
//     ouvert sa porte (measuresValid == false, purge en cours OU attente
//     fail-closed).
//
// DÉFAUT TROUVÉ PAR LE TEST (à l'extraction, 2026-10-08) : la première version
// de la coquille (pump_controller.cpp) ne passait PAS `measuresValid` à la
// formule — elle calculait `floorS - stabElapsedS` SANS jamais regarder si
// measure_validity avait déjà ouvert sa porte. Or `measureValidityElapsedS()`
// renvoie TOUJOURS 0 dès que la porte est ouverte (dispensée OU par plancher),
// donc le terme de purge valait `floorS` COMPLET (10 min) en PERMANENCE tant
// que measure_validity restait valide — y compris sur un cycle DISPENSÉ,
// recréant EXACTEMENT le défaut d'affichage d'un facteur 10 que ce lot était
// censé corriger, ET affichant potentiellement « 10 min » en continu via
// /data + WebSocket pendant un fonctionnement par ailleurs normal. Corrigé en
// clampant le terme de purge à 0 dès que `measuresValid` est vrai : il n'y a
// alors plus aucun reste de purge à attendre, seul le plancher du filtre
// capteur (indépendant) peut encore dominer.
//
// Le délai annoncé à l'utilisateur est le MAX des deux restes (celui qui
// domine), jamais négatif, jamais replié (arithmétique non signée avec
// clamp explicite).
//
// TROISIÈME DÉFAUT TROUVÉ (code-reviewer, 2e passage, 2026-10-08) : ni l'un ni
// l'autre des deux planchers ne COURT quand l'eau est absente (filtration à
// l'arrêt) — measureValidityElapsedS() fige à 0 (pas de purge en cours) et le
// plancher de gel du filtre n'a aucun front montant à attendre. Le calcul
// précédent retombait alors sur `floorS − 0 = 600` (10 min), FIGÉ toute la
// nuit : même défaut d'affichage, troisième occurrence, cette fois sur un
// compteur qui ne court dans AUCUN état lié à l'eau absente. Corrigé en
// rendant la fonction TOTALE : `waterPresent` est désormais un paramètre
// explicite, et son absence renvoie UINT32_MAX — sentinelle « délai
// INDÉTERMINÉ », DÉJÀ idiomatique au projet (cf. getPhSlopeAgeMs()),
// JAMAIS sérialisée telle quelle : la coquille (pump_controller.cpp) la
// retourne telle quelle, et les deux points d'exposition JSON
// (web_routes_data.cpp / ws_manager.cpp) OMETTENT le champ quand ils la
// voient plutôt que d'inventer un nombre.
inline uint32_t computeDoseAvailableInS(bool waterPresent, bool measuresValid,
                                        uint32_t stabElapsedS, uint32_t floorMs,
                                        uint32_t filterRemainingMs) {
  if (!waterPresent) return UINT32_MAX;  // délai indéterminé, PAS 600
  uint32_t purgeRemainS = 0;
  if (!measuresValid) {
    const uint32_t floorS = floorMs / 1000UL;
    purgeRemainS = (stabElapsedS < floorS) ? (floorS - stabElapsedS) : 0UL;
  }
  // Arrondi ms → s PAR EXCÈS : ne jamais annoncer « déjà disponible » (0 s)
  // tant qu'il reste la moindre milliseconde de plancher filtre à attendre.
  const uint32_t filterRemainS = (filterRemainingMs + 999UL) / 1000UL;
  return (filterRemainS > purgeRemainS) ? filterRemainS : purgeRemainS;
}

#endif  // DOSE_AVAILABLE_LOGIC_H
