#ifndef HEAP_DROP_LOGIC_H
#define HEAP_DROP_LOGIC_H

#include <stdint.h>

// =============================================================================
// heap_drop_logic — Détecteur PUR de CHUTE de contiguïté (feature-144, v2.60.0)
// =============================================================================
// ADR-0017, idiome de heap_caps_logic.h / slope_retry_logic.h : AUCUNE
// dépendance Arduino/FreeRTOS/esp_heap_caps ici. La décision est une fonction
// TOTALE de (courant, référence, instant, dernière émission, plancher,
// décalage, intervalle minimum) — testable en natif, sans ESP32. La coquille
// impure (lecture heap, snprintf, systemLogger) vit dans main.cpp.
//
// CONTEXTE (ne pas reproduire l'erreur de juillet) : l'ancien bloc de
// main.cpp émettait une trace INFO toutes les 5 min OU sur chute — les deux
// déclencheurs partageaient le même `if`. Désactiver la cadence périodique
// (2026-07-23, demande utilisateur, ~288 lignes/j sur un journal de 8 Ko) a
// éteint le détecteur de CHUTE avec elle. Ce module réactive la chute SEULE :
// aucune émission périodique, sous AUCUNE forme — y compris « au premier tick
// après boot » (c'était un troisième déclencheur caché de l'ancien bloc, via
// `lastHeapLogMs == 0`).
//
// -----------------------------------------------------------------------------
// RÉARMEMENT SILENCIEUX (ref = max(ref, courant), SANS hystérésis)
// -----------------------------------------------------------------------------
// Dans l'ancien bloc, la référence n'était rafraîchie QU'À L'ÉMISSION, et seule
// une BAISSE déclenchait. La cadence de 5 min jouait donc un second rôle NON
// documenté : réarmer implicitement la référence vers le haut à chaque tick.
// Sans elle, la référence devient un CLIQUET MONOTONE DESCENDANT : le
// détecteur émet au plus quelques lignes par démarrage puis se tait
// DÉFINITIVEMENT — une remontée suivie d'une nouvelle chute depuis un palier
// haut serait manquée, précisément le scénario qu'on cherche à attraper
// (feature-144, « LE POINT DUR »). D'où le réarmement À CHAQUE TICK, silencieux
// (aucune ligne de remontée — arbitrage utilisateur) : une hystérésis ne
// réduirait aucun bruit (ce réarmement n'en produit aucun, lui-même) et ne
// ferait que désensibiliser le détecteur aux vraies chutes.
//
// -----------------------------------------------------------------------------
// SEUIL : max(kHeapDropFloorBytes, ref >> kHeapDropRefShift)
// -----------------------------------------------------------------------------
// Un quart de la référence, plancher 4 Ko (= kRestartMaxAllocHeapBytes).
// Comparaison en deux temps : on écarte d'abord `courant >= ref` (cas normal du
// réarmement, aucune chute possible), PUIS on soustrait en sachant `courant < ref`.
//
// ⚠️ La forme purement ADDITIVE `courant + seuil <= ref`, écrite en premier jet,
// DÉBORDE quand `courant` approche UINT32_MAX : la somme se replie et la garde
// devient vraie, produisant une émission fantôme avec une amplitude incohérente
// qui corrompt `ref` DURABLEMENT. Trouvé par le test-engineer (feature-144).
// On pourrait juger le cas théorique — `courant` vient de heapLargestFreeBlock8Bit()
// et n'approche jamais 4 Go en fonctionnement normal. Mais le scénario réaliste
// est une LECTURE CORROMPUE PENDANT UNE CORRUPTION DE TAS DÉJÀ EN COURS, c'est-à-
// dire exactement ce que cet instrument sert à diagnostiquer : il mentirait au
// seul moment qui compte. D'où la garde, qui ne coûte qu'une comparaison.
//
// Simulé sur 35 boots (feature-144, contre-correction du 2026-10-08) :
// 10 lignes / 273,5 h (~0,88 ligne/j), TOUTES des chutes ≥ 30 %.
//
// -----------------------------------------------------------------------------
// ANTI-RAFALE TEMPOREL : deux émissions séparées d'au moins kHeapDropMinIntervalMs
// -----------------------------------------------------------------------------
// ⚠️ POINT CRITIQUE : pendant la suppression (fenêtre anti-rafale non
// écoulée), `ref` N'EST PAS ré-ancrée. La chute reste « en attente » et sera
// émise au premier instant autorisé AVEC SON AMPLITUDE VRAIE depuis le palier
// d'origine. L'anti-rafale DÉCALE la ligne, il ne l'AMPUTE jamais.
// `ref = courant` UNIQUEMENT à l'émission effective.
// =============================================================================

// État porté par l'appelant (statique locale dans checkHeapHealth(), comme
// MeasureValidityState / HeapDropState sœurs du même style ADR-0017).
struct HeapDropState {
  uint32_t ref = 0;          // Référence (palier haut courant)
  uint32_t lastEmitMs = 0;   // millis() de la dernière émission (ou de l'ancrage initial)
  bool initialized = false;  // false au 1er tick : ancre `ref` SANS émettre (invariant A5)
};

// Résultat d'un tick. `emit == false` ⇒ les autres champs ne sont pas
// significatifs (rien à journaliser).
struct HeapDropResult {
  bool emit = false;
  uint32_t before = 0;       // `ref` avant la chute (valeur haute du palier)
  uint32_t after = 0;        // valeur courante (`courant`)
  uint32_t dropAmount = 0;   // ampleur de la chute = before - after
};

// Évalue un tick du détecteur et met à jour `state` en place (ref, lastEmitMs,
// initialized). Fonction TOTALE : aucune branche indéfinie quelles que soient
// les valeurs fournies (arithmétique non signée, pas d'underflow possible —
// `nowMs - state.lastEmitMs` tolère le repliement millis() à 24,8 j, comme
// partout ailleurs dans ce projet).
//
// @param state        état du détecteur, mis à jour par effet de bord
// @param courant       maxBloc8 courant (heapLargestFreeBlock8Bit(), DÉJÀ mesuré
//                      par l'appelant — ce module ne mesure rien)
// @param nowMs         millis() courant
// @param floorBytes    kHeapDropFloorBytes (plancher absolu du seuil)
// @param refShift      kHeapDropRefShift (seuil relatif = ref >> refShift)
// @param minIntervalMs kHeapDropMinIntervalMs (plancher anti-rafale)
inline HeapDropResult heapDropEvaluate(HeapDropState& state, uint32_t courant,
                                        uint32_t nowMs, uint32_t floorBytes,
                                        uint8_t refShift, uint32_t minIntervalMs) {
  HeapDropResult result;

  if (!state.initialized) {
    // Premier tick après boot : ancre la référence SANS émettre (invariant A5
    // — aucune émission périodique, même « au premier passage »). Arme aussi
    // l'anti-rafale dès cet instant : pas de rafale gratuite juste après boot.
    state.ref = courant;
    state.lastEmitMs = nowMs;
    state.initialized = true;
    return result;
  }

  uint32_t threshold = state.ref >> refShift;
  if (threshold < floorBytes) threshold = floorBytes;

  // Garde anti-débordement AVANT toute arithmétique (voir l'en-tête de fichier) :
  // `courant >= ref` est le cas normal du réarmement, il ne peut pas être une
  // chute. L'écarter ici rend la soustraction ci-dessous sûre par construction,
  // et ferme le débordement de la forme additive.
  const bool chute = (courant < state.ref) && ((state.ref - courant) >= threshold);
  if (chute) {
    // Chute franchie. Anti-rafale TEMPOREL : peut DIFFÉRER la ligne, ne
    // l'AMPUTE jamais — `ref` reste intacte tant qu'elle n'est pas émise.
    uint32_t elapsed = nowMs - state.lastEmitMs;  // non signé, wrap-safe
    if (elapsed >= minIntervalMs) {
      result.emit = true;
      result.before = state.ref;
      result.after = courant;
      result.dropAmount = state.ref - courant;  // before > after ici, jamais d'underflow
      state.ref = courant;        // Ré-ancrage SEULEMENT à l'émission effective
      state.lastEmitMs = nowMs;
    }
    // Sinon : rien n'est fait, la chute reste EN ATTENTE (ref inchangée) — elle
    // sera émise au tick suivant où l'anti-rafale laisse passer, avec son
    // amplitude vraie depuis `state.ref`.
  } else {
    // RÉARMEMENT SILENCIEUX : voir le bloc de commentaires en tête de fichier.
    if (courant > state.ref) state.ref = courant;
  }

  return result;
}

#endif  // HEAP_DROP_LOGIC_H
