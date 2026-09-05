#ifndef HTTP_LOWHEAP_LOGIC_H
#define HTTP_LOWHEAP_LOGIC_H

// =============================================================================
// http_lowheap_logic — Garde 503 « fichiers statiques » : décision PURE (v2.41.0)
// =============================================================================
// bug-080, lot B-3. Deux décisions, aucune collecte, aucun effet :
//   1. httpLowHeapShouldRefuse() — faut-il refuser ce GET de fichier statique ?
//      Évalué depuis le FILTRE du handler prioritaire, donc sur la tâche
//      `async_tcp`. Doit rester une comparaison, rien d'autre.
//   2. httpLowHeapDrain() — machine à états du BORNAGE du journal : deux lignes
//      par épisode (front montant, front descendant), jamais une par requête.
//      Évalué depuis la loop.
//
// INVARIANT (ADR-0017) : ce module ne fait QUE décider à partir de valeurs déjà
// collectées. La mesure (heapLargestFreeBlock8Bit, millis), le compteur atomique
// et l'émission des lignes restent dans la coquille impure web_server.cpp.
//
// ⚠️ CONTRAINTE DE SÛRETÉ QUI JUSTIFIE CE DÉCOUPAGE
// logger.cpp:71-73 flushe EN SYNCHRONE, dans la tâche appelante, sur ERROR et
// CRITICAL → _persistFs->open("/system.log", "a") → fopen() → exactement
// l'abort() que cette garde prévient. D'où, dans tout ce chemin :
//   - WARN / INFO uniquement, JAMAIS ERROR ni CRITICAL ;
//   - AUCUN log depuis le callback `async_tcp` : fetch_add atomique + send, zéro
//     String, zéro allocation hors la réponse elle-même.
// Ce header, purement arithmétique, ne peut structurellement rien émettre.
//
// CONTRAINTE DE PORTABILITÉ : pas d'Arduino.h, pas de FreeRTOS, pas d'ESP.*, pas
// de String. En-tête C <stdint.h> (et non <cstdint>) pour compiler en natif.
// =============================================================================

#include <stdint.h>

// ---------------------------------------------------------------------------
// 1. Prédicat de refus (exécuté sur async_tcp)
// ---------------------------------------------------------------------------
// `largestFreeBlock8` = heapLargestFreeBlock8Bit(), c'est-à-dire le plus gros
// bloc libre du pool MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT — exactement le pool
// où pvPortMalloc sert le mutex newlib dont l'échec aborte. La métrique est donc
// juste AU BIT PRÈS pour la seule allocation qui tue.
// Le libre TOTAL serait aveugle à ce crash : à t=5474 s le terrain montrait
// libre8 = 58 424 o (« plein ») alors que maxBloc8 valait 15 860 o et que
// 11 allocations échouaient.
// Frontière STRICTE : `< seuil` refuse, `>= seuil` sert (un tas exactement au
// seuil couvre encore ×13,6 le besoin dur ; refuser y serait gratuit).
inline bool httpLowHeapShouldRefuse(uint32_t largestFreeBlock8, uint32_t minAllocBytes) {
  return largestFreeBlock8 < minAllocBytes;
}

// ---------------------------------------------------------------------------
// 2. Bornage du journal : machine à états d'épisode (exécutée depuis la loop)
// ---------------------------------------------------------------------------
enum class HttpLowHeapLogAction : uint8_t {
  None = 0,      // rien à journaliser
  EpisodeStart,  // front montant  → WARN « service fichiers suspendu »
  EpisodeEnd     // front descendant → INFO « service fichiers rétabli »
};

// État persistant de l'épisode courant. Détenu par la coquille impure.
struct HttpLowHeapEpisode {
  bool     active = false;           // un épisode est en cours
  uint32_t drainedRefusals = 0;      // dernière valeur du compteur vue par la loop
  uint32_t refusalsAtStart = 0;      // valeur du compteur juste AVANT l'épisode
  uint32_t episodeStartMs = 0;       // millis du 1er refus drainé de l'épisode
  uint32_t lastRefusalMs = 0;        // millis du dernier refus drainé
};

// Verdict d'un drain.
struct HttpLowHeapDrainResult {
  HttpLowHeapLogAction action = HttpLowHeapLogAction::None;
  uint32_t refusals = 0;    // refus cumulés de l'épisode (renseigné sur EpisodeEnd)
  uint32_t durationMs = 0;  // durée de l'épisode (renseignée sur EpisodeEnd)
};

// La loop n'a besoin de MESURER le tas que si un épisode est ouvert ET que le
// silence exigé est déjà écoulé. Ailleurs, `httpLowHeapDrain` n'exploite pas
// `largestFreeBlock8` : l'appelant peut passer 0 sans fausser la décision.
// Sans ce garde-fou, `update()` walkerait le tas à la fréquence de la loop
// pendant toute la famine — au pire moment possible.
inline bool httpLowHeapNeedsHeapSample(const HttpLowHeapEpisode& st,
                                       uint32_t nowMs, uint32_t quietMs) {
  // Soustraction non signée : correcte au repliement de millis() (24,8 j).
  return st.active && (nowMs - st.lastRefusalMs) >= quietMs;
}

// Drain du compteur de refus + transitions d'épisode.
//   refusalCount ...... valeur courante du compteur atomique (écrit par async_tcp)
//   nowMs ............. millis()
//   largestFreeBlock8 . mesure du tas, PERTINENTE seulement si
//                       httpLowHeapNeedsHeapSample() a renvoyé true
//   minAllocBytes ..... kStaticFileMinAllocBytes
//   quietMs ........... kHttpLowHeapEpisodeQuietMs
//
// Règles, dans cet ordre :
//   a) compteur qui a bougé → l'épisode vit ; s'il n'était pas ouvert, on
//      l'ouvre (EpisodeStart). Un seul WARN, même pour 200 refus.
//   b) épisode ouvert, compteur figé depuis >= quietMs ET tas rétabli
//      (>= seuil) → fermeture (EpisodeEnd). Les DEUX conditions sont exigées :
//      le silence seul pourrait n'être que l'absence de visiteur pendant une
//      famine toujours active.
//   c) sinon → rien. Le journal persistant (8 Ko, rétention 25-35 min) reste la
//      fenêtre forensique de l'épisode, pas son déchet.
inline HttpLowHeapDrainResult httpLowHeapDrain(HttpLowHeapEpisode& st,
                                               uint32_t refusalCount,
                                               uint32_t nowMs,
                                               uint32_t largestFreeBlock8,
                                               uint32_t minAllocBytes,
                                               uint32_t quietMs) {
  HttpLowHeapDrainResult out;

  if (refusalCount != st.drainedRefusals) {
    if (!st.active) {
      st.active = true;
      st.refusalsAtStart = st.drainedRefusals;  // baseline = dernier état connu
      st.episodeStartMs = nowMs;
      out.action = HttpLowHeapLogAction::EpisodeStart;
    }
    st.drainedRefusals = refusalCount;
    st.lastRefusalMs = nowMs;
    return out;
  }

  if (st.active && (nowMs - st.lastRefusalMs) >= quietMs &&
      !httpLowHeapShouldRefuse(largestFreeBlock8, minAllocBytes)) {
    st.active = false;
    out.action = HttpLowHeapLogAction::EpisodeEnd;
    // Soustractions non signées : justes même si le compteur ou millis() se
    // replient (le compteur est un uint32 monotone alimenté par fetch_add).
    out.refusals = refusalCount - st.refusalsAtStart;
    out.durationMs = nowMs - st.episodeStartMs;
  }
  return out;
}

#endif  // HTTP_LOWHEAP_LOGIC_H
