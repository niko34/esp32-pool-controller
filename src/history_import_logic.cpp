#include "history_import_logic.h"

bool historyImportSameSlot(uint32_t tsA, uint8_t granA, uint32_t tsB, uint8_t granB) {
  // Égalité STRICTE sur les deux composantes. Pas de tolérance temporelle : un
  // créneau RAW voisin (5 min plus loin) est un AUTRE point, pas un doublon —
  // les rapprocher perdrait une mesure réelle.
  return tsA == tsB && granA == granB;
}

HistoryImportTrimPlan historyImportTrimPlan(size_t raw, size_t hourly, size_t daily) {
  HistoryImportTrimPlan plan;
  // Soustractions GARDÉES : `raw - kMaxRawDataPoints` sur un size_t donnerait
  // ~1,8e19 pour un effectif sous le plafond, donc une purge totale.
  if (raw > kMaxRawDataPoints) plan.rawToRemove = raw - kMaxRawDataPoints;
  if (hourly > kMaxHourlyDataPoints) plan.hourlyToRemove = hourly - kMaxHourlyDataPoints;
  if (daily > kMaxDailyDataPoints) plan.dailyToRemove = daily - kMaxDailyDataPoints;
  return plan;
}

bool historyImportModeIsReplace(const char* mode) {
  // Champ absent → comportement historique de importData() : remplacement.
  if (mode == nullptr) return true;

  // Comparaison manuelle avec "append", insensible à la casse, SANS <ctype.h>
  // (tolower() dépend de la locale ; ici on ne veut que l'ASCII).
  static const char kAppend[] = "append";
  size_t i = 0;
  for (; kAppend[i] != '\0'; ++i) {
    char c = mode[i];
    if (c == '\0') return true;                       // plus court que "append"
    if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    if (c != kAppend[i]) return true;                 // divergence → inconnu → replace
  }
  // Longueur exacte exigée : "appendx" n'est PAS "append".
  if (mode[i] != '\0') return true;

  return false;  // seul cas d'ajout
}
