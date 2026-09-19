#include "log_arena_logic.h"
#include "constants.h"    // kLogTruncTail (sizeof, budget du marqueur — C2)
#include "utf8_clamp.h"   // utf8ClampLen (module pur partagé, feature-129)
#include <stdio.h>        // snprintf (formatage de l'en-tête, pas de String)

LogArenaPlan logArenaPlan(uint16_t cap, uint16_t head, uint16_t tail, uint16_t used, uint16_t len) {
  LogArenaPlan plan{0, 0, 0, false};
  if (cap == 0) return plan;  // garde-fou défensif (kLogArenaBytes > 0 garanti par static_assert)

  const uint16_t contiguousFreeAtEnd = static_cast<uint16_t>(cap - head);
  // C4 : politique « pas d'enjambement » — un message n'est jamais scindé par
  // la frontière de fin d'arène. Le reliquat [head, cap) est abandonné plutôt
  // que d'y écrire un message coupé en deux.
  plan.wrapped = (len > contiguousFreeAtEnd);
  plan.writeOff = plan.wrapped ? static_cast<uint16_t>(0) : head;
  plan.tailDrop = plan.wrapped ? contiguousFreeAtEnd : static_cast<uint16_t>(0);

  // Place disponible AVANT de rattraper `tail`, mesurée depuis `head` — la
  // position d'écriture COURANTE, jamais depuis `writeOff`.
  //
  // ⚠️ C'est le défaut qui a été livré puis rattrapé le 16/09/2026, et il ne se
  // voit pas à la lecture : mesurer depuis `writeOff` paraît naturel puisque
  // c'est là qu'on va écrire. Mais après un saut de queue `writeOff` vaut 0, et
  // la formule conclut alors que presque tout l'anneau est libre alors que la
  // zone vivante commence peut-être à l'offset 1. `evictBytes` sort à 0, aucune
  // éviction n'a lieu, et le `memcpy` RECOUVRE des messages encore vivants.
  //
  // Le saut ne crée pas d'espace : il en CONSOMME — la queue [head, cap) est
  // abandonnée. L'espace libre reste donc celui qui sépare `head` de `tail`,
  // et c'est `tailDrop` (compté dans `need` ci-dessous) qui porte le coût du
  // saut. Deux agents l'ont retrouvé indépendamment, en simulation et par
  // modèle de référence : recouvrement à la 853e et à la 508e écriture, avec
  // les constantes réelles du projet. `used > kLogArenaBytes` est le symptôme
  // observable — la somme des octets vivants dépassait la capacité physique.
  //
  // `used` ne sert qu'à lever l'ambiguïté d'un anneau à deux index
  // (head == tail : vide si used == 0, plein sinon).
  uint16_t room;
  if (used == 0) {
    room = cap;
  } else {
    room = static_cast<uint16_t>((static_cast<uint32_t>(tail) + cap - head) % cap);
  }

  // Le saut de queue (tailDrop) est COMPTÉ dans le budget à évincer : sans
  // cela, l'occupation réelle de l'anneau après le saut serait sous-estimée au
  // prochain appel, et une entrée pas encore évincée finirait recouverte en
  // silence (C4).
  const uint32_t need = static_cast<uint32_t>(len) + plan.tailDrop;
  plan.evictBytes = (need > room) ? static_cast<uint16_t>(need - room) : static_cast<uint16_t>(0);
  return plan;
}

size_t logTruncatePlan(const char* src, size_t len, size_t maxBytes, bool& truncated) {
  if (len <= maxBytes) {
    truncated = false;
    return len;
  }
  truncated = true;
  // C2 : le marqueur de troncature est réservé AVANT le clamp UTF-8, jamais après.
  const size_t tailReserve = sizeof(kLogTruncTail);  // inclut le NUL (marge, cf. kAlertTail)
  const size_t keep = (maxBytes > tailReserve) ? (maxBytes - tailReserve) : 0;
  return utf8ClampLen(src, keep);
}

size_t logRingEvictCount(const LogSlot* slots, size_t n, size_t oldest, size_t bytesNeeded) {
  if (slots == nullptr || n == 0 || bytesNeeded == 0) return 0;
  size_t freed = 0;
  size_t count = 0;
  for (size_t i = 0; i < n && freed < bytesNeeded; ++i) {
    const size_t idx = (oldest + i) % n;
    freed += slots[idx].len;
    ++count;
  }
  return count;
}

namespace {

// Décomposition calendaire grégorienne PURE (algorithme civil_from_days,
// domaine public, Howard Hinnant — https://howardhinnant.github.io/date_algorithms.html).
// Volontairement SANS localtime_r/TZ : l'ajustement fuseau horaire dépend d'un
// état global mutable (setenv("TZ", ...), config.cpp:95) que ce module pur ne
// peut pas consulter — `wallSec` doit déjà porter les secondes dans le fuseau
// voulu (ajout de l'offset local en amont, côté coquille impure, phase C).
void civilFromEpoch(uint32_t epochSec, int& year, int& month, int& day,
                     int& hour, int& minute, int& second) {
  const int64_t days = static_cast<int64_t>(epochSec) / 86400;
  const int64_t rem = static_cast<int64_t>(epochSec) % 86400;
  hour = static_cast<int>(rem / 3600);
  minute = static_cast<int>((rem % 3600) / 60);
  second = static_cast<int>(rem % 60);

  const int64_t z = days + 719468;
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const uint64_t doe = static_cast<uint64_t>(z - era * 146097);           // [0, 146096]
  const uint64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;  // [0, 399]
  const int64_t y = static_cast<int64_t>(yoe) + era * 400;
  const uint64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);           // [0, 365]
  const uint64_t mp = (5 * doy + 2) / 153;                                // [0, 11]
  const uint64_t d = doy - (153 * mp + 2) / 5 + 1;                        // [1, 31]
  const uint64_t m = mp + (mp < 10 ? 3 : static_cast<uint64_t>(-9));      // [1, 12]

  year = static_cast<int>(y + (m <= 2 ? 1 : 0));
  month = static_cast<int>(m);
  day = static_cast<int>(d);
}

// Emplacement centralisé du format : évite de dupliquer les deux variantes
// (repli non daté / date calculée) dans logFormatHeader.
int formatHeaderLine(char* out, size_t n, const char* dateText, LogLevel level) {
  return snprintf(out, n, "%s %s ", dateText, logLevelName(level));
}

}  // namespace

// feature-130 : anciennement `levelShortName`, interne à ce fichier — promu
// en fonction publique pour que logger.cpp (et les appelants JSON migrés,
// ws_manager.cpp/web_routes_data.cpp) cessent de passer par
// `Logger::getLevelString()` (String allouée, ADR-0082).
const char* logLevelName(LogLevel level) {
  switch (level) {
    case LogLevel::DEBUG:    return "DEBUG";
    case LogLevel::INFO:     return "INFO";
    case LogLevel::WARNING:  return "WARN";
    case LogLevel::ERROR:    return "ERROR";
    case LogLevel::CRITICAL: return "CRIT";
    default:                 return "?????";
  }
}

int logFormatHeader(char* out, size_t n, uint32_t wallSec, LogLevel level) {
  if (wallSec == 0) {
    // Horloge non fixée : jamais une ligne datée « 1970 ».
    return formatHeaderLine(out, n, "???\?-?\?-??T??:??:??", level);
  }
  int year, month, day, hour, minute, second;
  civilFromEpoch(wallSec, year, month, day, hour, minute, second);
  char dateText[20];
  snprintf(dateText, sizeof(dateText), "%04d-%02d-%02dT%02d:%02d:%02d",
           year, month, day, hour, minute, second);
  return formatHeaderLine(out, n, dateText, level);
}

bool logShouldPersist(uint32_t seq, uint32_t persistEnableSeq, LogLevel level) {
  if (seq < persistEnableSeq) {
    // C7 : rattrapage de démarrage, filtré — sinon un boot verse plus de
    // kMaxLogFileBytes d'INFO et déclenche la rotation immédiate.
    return static_cast<int>(level) >= static_cast<int>(LogLevel::WARNING);
  }
  return level != LogLevel::DEBUG;  // règle courante, inchangée
}

size_t logNextFlushSlot(const LogSlot* slots, size_t n, size_t oldest,
                         size_t liveCount, uint32_t flushCursor) {
  if (slots == nullptr || n == 0) return n;
  const size_t bound = (liveCount < n) ? liveCount : n;  // défensif, best-effort
  for (size_t i = 0; i < bound; ++i) {
    const size_t idx = (oldest + i) % n;
    if (slots[idx].seq >= flushCursor) return idx;
  }
  return n;
}

bool logArenaWriteOverlapsLive(const LogSlot* slots, size_t n, size_t oldest,
                                size_t liveCount, uint16_t writeOff, uint16_t writeLen) {
  if (slots == nullptr || n == 0 || writeLen == 0) return false;
  const size_t bound = (liveCount < n) ? liveCount : n;  // défensif, best-effort
  const uint32_t writeEnd = static_cast<uint32_t>(writeOff) + writeLen;
  for (size_t i = 0; i < bound; ++i) {
    const size_t idx = (oldest + i) % n;
    const LogSlot& s = slots[idx];
    if (s.len == 0) continue;  // créneau vide (défensif) : ne peut rien recouper
    const uint32_t sEnd = static_cast<uint32_t>(s.off) + s.len;
    // Chevauchement d'intervalles [writeOff, writeEnd) x [s.off, sEnd) :
    // deux intervalles NE se chevauchent PAS si l'un finit avant que l'autre
    // commence — la négation de "pas de chevauchement" est ce test.
    if (writeOff < sEnd && s.off < writeEnd) return true;
  }
  return false;
}
