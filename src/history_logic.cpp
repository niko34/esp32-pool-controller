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

namespace {
// Consomme `need` octets sur le budget `a`. Uniquement des soustractions
// gardées (jamais d'addition) → aucun débordement possible, même avec des
// paramètres à SIZE_MAX. Renvoie false si a < need.
bool consumeBudget(size_t& a, size_t need) {
  if (need > a) return false;
  a -= need;
  return true;
}
}  // namespace

// Admission d'un save (feature-131, écriture atomique .tmp+rename) :
// équivalent de freeBytes >= jsonBytes + marginBytes sans jamais additionner
// (anti-overflow). Frontière LARGE (égalité exacte → admis). Plus de crédit de
// l'ancien fichier : il coexiste avec le .tmp pendant toute l'écriture, son
// espace n'est récupéré qu'au rename final.
bool historySaveAdmit(size_t freeBytes, size_t jsonBytes, size_t marginBytes) {
  size_t freePool = freeBytes;
  return consumeBudget(freePool, jsonBytes) &&
         consumeBudget(freePool, marginBytes);
}

// Bucket complet et ancien : sa dernière seconde dépasse maxAge. Même
// convention stricte qu'isOlderThan (== maxAge → false).
bool historyBucketConsolidable(uint32_t now, uint32_t bucketStart,
                               uint32_t bucketLen, uint32_t maxAgeSeconds) {
  uint32_t lastSecond = (bucketLen == 0) ? bucketStart : bucketStart + bucketLen - 1;
  // Future-safe (bug-points-futurs) : un bucket dont la fin est dans le futur
  // n'est jamais consolidable — sans cette garde, le wrap uint32 le rendrait
  // instantanément « très ancien » (agrégat fantôme daté du futur).
  if (lastSecond > now) return false;
  return isOlderThan(now, lastSecond, maxAgeSeconds);
}

// Fusion équipondérée : fini+fini → moyenne, un seul fini → lui, aucun → NAN.
float historyMergeMeanFinite(float a, float b) {
  bool fa = isfinite(a), fb = isfinite(b);
  if (fa && fb) return (a + b) / 2.0f;
  if (fa) return a;
  if (fb) return b;
  return NAN;
}

// Ancienneté future-safe : un point futur n'est jamais « expiré ».
bool historyIsExpired(uint32_t now, uint32_t ts, uint32_t maxAgeSeconds) {
  if (ts > now) return false;
  return isOlderThan(now, ts, maxAgeSeconds);
}
