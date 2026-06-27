#include "history_logic.h"

// Tronque ts au début de son bucket (division entière puis multiplication).
// Garde bucketSeconds==0 : on renvoie ts inchangé pour éviter /0.
uint32_t bucketTimestamp(uint32_t ts, uint32_t bucketSeconds) {
  return (bucketSeconds == 0) ? ts : (ts / bucketSeconds) * bucketSeconds;
}

// Ancienneté stricte : age == maxAge → false. L'opération (now - ts) est en
// uint32 : si ts > now (horloge revenue en arrière), le résultat wrap et
// produit un grand âge — comportement existant volontairement conservé.
bool isOlderThan(uint32_t now, uint32_t ts, uint32_t maxAgeSeconds) {
  return (uint32_t)(now - ts) > maxAgeSeconds;
}

// Moyenne : count==0 → NAN (point ignoré par la coquille), sinon sum/count.
float finalizeMean(float sum, int count) {
  return (count > 0) ? (sum / count) : NAN;
}

// Majorité stricte avec division ENTIÈRE de total : reproduit (count > total/2).
bool isMajority(int trueCount, int total) {
  return trueCount > total / 2;
}

// Vrai dès qu'au moins un élément est compté.
bool anyTrue(int count) {
  return count > 0;
}
