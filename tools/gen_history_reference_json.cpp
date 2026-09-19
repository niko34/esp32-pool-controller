// =============================================================================
// gen_history_reference_json — GÉNÉRATEUR PONCTUEL (feature-131, E4)
// =============================================================================
// Outil de développement, PAS un artefact de build : reproduit fidèlement la
// boucle de sérialisation de HistoryManager::saveToFile() TELLE QU'ELLE
// ÉTAIT en 2.52.2 (git show HEAD~:src/history.cpp au moment de feature-131,
// AVANT ce lot — cf. commentaire "bug-nospc-partition-history (A2+A3)"), pour
// produire test/fixtures/history_reference.json : la fixture de référence du
// critère d'acceptation n°2 de la spec ("le fichier produit est identique à
// celui de la version précédente pour un même jeu de points").
//
// Pourquoi un programme séparé plutôt qu'un test natif : l'ancien code
// utilisait ArduinoJson::JsonDocument + Arduino::String(x,n) (= dtostrf).
// ArduinoJson n'est PAS un dépendance de l'env `native` (ADR-0017, modules
// purs uniquement) et ne doit pas génériquement en devenir une pour un seul
// test de non-régression. Ce générateur, lui, tourne HORS de `pio test`,
// compilé une fois à la main avec le ArduinoJson déjà vendu dans
// .pio/libdeps (aucune install supplémentaire) :
//
//   g++ -std=c++17 -I .pio/libdeps/esp32dev_core3/ArduinoJson/src \
//       tools/gen_history_reference_json.cpp \
//       -o /tmp/gen_history_reference_json
//   /tmp/gen_history_reference_json > test/fixtures/history_reference.json
//
// `Arduino::String(x,n)` n'existe pas ici (pas d'Arduino.h) : on la remplace
// par `dtostrfOldStyle()`, TRANSCRIPTION FIDÈLE de dtostrf() telle qu'appelée
// par String(double,n) dans WString.cpp (width = n+2) — c'est le MÊME
// algorithme que le port testé dans
// test/test_native_history_persist_format/ (historyFormatFixed). Les deux
// portages proviennent de la même source
// (framework-arduinoespressif32/cores/esp32/stdlib_noniso.c) : s'ils
// divergeaient, ce générateur produirait une fixture qui ne prouve plus rien
// — c'est pourquoi le sweep (a) de test_history_persist_format.cpp existe.
//
// Jeu de points : voir kPoints ci-dessous, PARTAGÉ (recopié à l'identique,
// commentaire renvoyant ici) par
// test/test_native_history_persist_format/test_history_persist_format.cpp
// dans test_e4_comparaison_octet_a_octet_avec_l_ancien_format().
// =============================================================================

#define ARDUINOJSON_ENABLE_ARDUINO_STRING 0
#define ARDUINOJSON_ENABLE_STD_STRING 1

#include <ArduinoJson.h>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

// Portage EXACT de dtostrf (framework-arduinoespressif32/cores/esp32/stdlib_noniso.c),
// appelé avec width = prec + 2 comme le fait String(double, prec) (WString.cpp).
static std::string dtostrfOldStyle(double number, unsigned int prec) {
  char buf[64];
  char* out = buf;
  bool negative = false;
  int fillme = static_cast<int>(prec) + 2;
  if (prec > 0) fillme -= static_cast<int>(prec + 1);
  if (number < 0.0) {
    negative = true;
    fillme--;
    number = -number;
  }
  double rounding = 2.0;
  for (unsigned int i = 0; i < prec; ++i) rounding *= 10.0;
  rounding = 1.0 / rounding;
  number += rounding;
  double tenpow = 1.0;
  unsigned int digitcount = 1;
  while (number >= 10.0 * tenpow) {
    tenpow *= 10.0;
    digitcount++;
  }
  number /= tenpow;
  fillme -= static_cast<int>(digitcount);
  while (fillme-- > 0) *out++ = ' ';
  if (negative) *out++ = '-';
  unsigned int totalDigits = digitcount + prec;
  int8_t digit = 0;
  while (totalDigits-- > 0) {
    digit = static_cast<int8_t>(number);
    if (digit > 9) digit = 9;
    *out++ = static_cast<char>('0' | digit);
    if (totalDigits == prec && prec > 0) *out++ = '.';
    number -= digit;
    number *= 10.0;
  }
  *out = '\0';
  return std::string(buf);
}

struct OldPoint {
  unsigned long ts;
  float ph, orp, temp, phFiltered, orpFiltered;
  bool filtration;
  unsigned int doseMask;
  uint8_t granularity;  // 0=RAW, 1=HOURLY, 2=DAILY
};

// Jeu de points de référence — couvre les trois granularités et les cas
// limites (NaN par champ, doseMask 0..7, filtration true/false).
static const OldPoint kPoints[] = {
    // RAW, tout fini, premier point.
    {1757000000UL, 7.23f, 650.4f, 24.6f, 7.18f, 648.6f, true, 0, 0},
    // RAW, pH NaN (mesure exclue).
    {1757000300UL, NAN, 651.0f, 24.7f, NAN, 649.0f, true, 1, 0},
    // RAW, ORP NaN.
    {1757000600UL, 7.20f, NAN, 24.5f, 7.19f, NAN, true, 2, 0},
    // RAW, température NaN.
    {1757000900UL, 7.21f, 652.0f, NAN, 7.20f, 650.0f, false, 4, 0},
    // RAW, filtrées NaN (filtre non amorcé) mais brut présent.
    {1757001200UL, 7.22f, 649.5f, 24.4f, NAN, NAN, true, 3, 0},
    // RAW, doseMask complet (pH+ORP+voie indéterminée = 7).
    {1757001500UL, 7.19f, 650.9f, 24.3f, 7.17f, 649.1f, true, 7, 0},
    // HOURLY : jamais de filtrée, même si le champ était fini en RAM.
    {1757000000UL, 7.21f, 651.3f, 24.4f, 7.99f, 999.0f, true, 1, 1},
    // DAILY : idem, filtration false, doseMask 0.
    {1756900000UL, 7.25f, 649.8f, 23.9f, 7.11f, 111.0f, false, 0, 2},
    // Dernier point : valeurs négatives (température hivernale, ORP négatif
    // hypothétique), pour vérifier le signe sur le port dtostrf.
    {1756800000UL, 6.98f, -12.3f, -1.25f, 6.95f, -10.0f, false, 5, 0},
};

int main() {
  JsonDocument doc;
  JsonArray data = doc["data"].to<JsonArray>();

  for (const auto& point : kPoints) {
    JsonObject obj = data.add<JsonObject>();
    obj["t"] = point.ts;
    if (std::isfinite(point.ph)) obj["p"] = serialized(dtostrfOldStyle(point.ph, 2));
    if (std::isfinite(point.orp)) obj["o"] = serialized(dtostrfOldStyle(point.orp, 1));
    if (std::isfinite(point.temp)) obj["T"] = serialized(dtostrfOldStyle(point.temp, 1));
    if (point.granularity == 0) {
      if (std::isfinite(point.phFiltered))
        obj["pf"] = serialized(dtostrfOldStyle(point.phFiltered, 2));
      if (std::isfinite(point.orpFiltered))
        obj["of"] = serialized(dtostrfOldStyle(point.orpFiltered, 1));
    }
    obj["f"] = point.filtration;
    obj["d"] = point.doseMask;
    obj["g"] = point.granularity;
  }

  std::string out;
  serializeJson(doc, out);
  std::fputs(out.c_str(), stdout);
  return 0;
}
