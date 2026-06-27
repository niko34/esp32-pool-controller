#ifndef HISTORY_LOGIC_H
#define HISTORY_LOGIC_H

// Module pur de math scalaire pour l'agrégation de l'historique.
// Headers C uniquement (<stdint.h>/<math.h>) : testable en natif sans libc++.
// PAS de <vector>/<map>/<cstdint>/Arduino/FreeRTOS ici.
//
// INVARIANT : characterization refactor. Ces fonctions reproduisent EXACTEMENT
// la math inline historique de consolidateData() (frontières strictes,
// divisions entières, wrap uint32). NE PAS "corriger" ces comportements.

#include <stdint.h>
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

// « Au moins un » : count > 0.
bool anyTrue(int count);

#endif // HISTORY_LOGIC_H
