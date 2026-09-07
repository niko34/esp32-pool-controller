#ifndef HISTORY_LOGIC_H
#define HISTORY_LOGIC_H

// Module pur de math scalaire pour l'agrégation de l'historique.
// Headers C uniquement (<stdint.h>/<stddef.h>/<math.h>) : testable en natif sans libc++.
// PAS de <vector>/<map>/<cstdint>/Arduino/FreeRTOS ici.
//
// INVARIANT : characterization refactor. Ces fonctions reproduisent EXACTEMENT
// la math inline historique de consolidateData() (frontières strictes,
// divisions entières, wrap uint32). NE PAS "corriger" ces comportements.
// (Exception : historySaveAdmit, logique NOUVELLE — bug-nospc-partition-history.)

#include <stdint.h>
#include <stddef.h>
#include <math.h>

// Tronque un timestamp au début de son bucket temporel.
// Reproduit (ts / bucketSeconds) * bucketSeconds. Garde bucketSeconds==0
// (renvoie ts tel quel) pour éviter une division par zéro.
uint32_t bucketTimestamp(uint32_t ts, uint32_t bucketSeconds);

// Prédicat d'ancienneté : (now - ts) > maxAgeSeconds.
// Frontière STRICTE (>) : age == maxAge renvoie false.
// Arithmétique uint32 : un wrap (ts > now) est possible et conservé tel quel.
bool isOlderThan(uint32_t now, uint32_t ts, uint32_t maxAgeSeconds);

// Finalise une moyenne : sum/count si count>0, sinon NAN.
// count==0 → NAN (le point est ignoré par la coquille).
float finalizeMean(float sum, int count);

// Règle de majorité stricte : trueCount > total/2.
// Division ENTIÈRE : 2/4 → false (2 > 2 faux), 3/4 → true, 3/5 → true.
bool isMajority(int trueCount, int total);

// bug-nospc-partition-history : admission d'une écriture de /history.json.
// Vrai si l'écriture peut être admise : freeBytes + oldFileBytes >= jsonBytes + marginBytes
// (oldFileBytes = octets bruts de l'ancien fichier, supprimé avant réécriture ;
// sous-estime les blocs réellement récupérés (arrondi bloc) → conservateur).
// Anti-overflow : aucune addition — consommation par soustractions gardées,
// correct jusqu'à SIZE_MAX sur chaque paramètre.
bool historySaveAdmit(size_t freeBytes, size_t oldFileBytes, size_t jsonBytes, size_t marginBytes);

// bug-history-doublons-troncature (A1) : un bucket n'est consolidable que
// lorsqu'il est COMPLET et ancien — sa DERNIÈRE seconde (bucketStart+len-1)
// dépasse maxAge. Évite l'agrégat partiel par passage de consolidation (les
// points d'une même heure franchissent le seuil progressivement → jusqu'à
// 12 doublons/heure avant ce correctif). bucketLen==0 → prédicat sur
// bucketStart seul (défensif).
bool historyBucketConsolidable(uint32_t now, uint32_t bucketStart,
                               uint32_t bucketLen, uint32_t maxAgeSeconds);

// bug-history-doublons-troncature (A2) : fusion équipondérée de deux valeurs
// de même champ lors de la déduplication d'agrégats du même bucket :
// fini+fini → moyenne, un seul fini → lui, aucun → NAN. Équipondéré car les
// effectifs d'origine ne sont pas stockés (approximation assumée, cf. spec).
float historyMergeMeanFinite(float a, float b);

// bug-points-futurs-et-ux-courbes : ancienneté FUTURE-SAFE. Un point horodaté
// dans le futur (ts > now — excursion d'horloge) ne doit JAMAIS être considéré
// comme ancien : le wrap uint32 d'isOlderThan (invariant documenté, conservé)
// le ferait paraître vieux de ~136 ans → purge 90 j ou consolidation immédiate
// en agrégat daté du futur (constaté en production le 2026-08-10 : journalier
// fantôme g2 daté du jour courant).
bool historyIsExpired(uint32_t now, uint32_t ts, uint32_t maxAgeSeconds);

#endif // HISTORY_LOGIC_H
