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

namespace {
// Consomme `need` octets d'abord sur le budget `a` puis sur `b`. Uniquement
// des soustractions gardées (jamais d'addition) → aucun débordement possible,
// même avec des paramètres à SIZE_MAX. Renvoie false si a+b (conceptuel) < need.
bool consumeBudget(size_t& a, size_t& b, size_t need) {
  size_t fromA = (need < a) ? need : a;
  a -= fromA;
  need -= fromA;
  if (need > b) return false;
  b -= need;
  return true;
}
}  // namespace

// Admission d'un save : équivalent de freeBytes + oldFileBytes >= jsonBytes + marginBytes
// sans jamais additionner (anti-overflow). Frontière LARGE (égalité exacte → admis).
bool historySaveAdmit(size_t freeBytes, size_t oldFileBytes, size_t jsonBytes, size_t marginBytes) {
  size_t freePool = freeBytes;
  size_t oldPool = oldFileBytes;
  return consumeBudget(freePool, oldPool, jsonBytes) &&
         consumeBudget(freePool, oldPool, marginBytes);
}
