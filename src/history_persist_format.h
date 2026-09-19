#ifndef HISTORY_PERSIST_FORMAT_H
#define HISTORY_PERSIST_FORMAT_H

// =============================================================================
// history_persist_format — Logique PURE du format PERSISTÉ de /history.json
// (feature-131, v2.53.0)
// =============================================================================
// ⚠️ BANDEAU DE RENVOI CROISÉ — NE JAMAIS CONFONDRE LES DEUX FORMATS :
//   - CE FICHIER            : ce qui part sur la FLASH (/history.json), clés
//     COURTES ("t","p","o","T","pf","of","f","d","g"), clés ABSENTES si la
//     mesure est non finie (pas de "null" écrit — on économise l'octet sur un
//     fichier relu des dizaines de milliers de fois).
//   - history_export_chunk.h : ce qui part sur le FIL (réponse HTTP chunked de
//     /get-history), clés LONGUES ("timestamp","ph","phFiltered",...),
//     TOUJOURS présentes (valeur "null" si non finie — contrat JS historique).
// Les deux modules formatent le MÊME DataPoint mais produisent des sorties
// DIFFÉRENTES. Un copier-coller de l'un vers l'autre est un bug par
// construction — c'est arrivé une fois (le pré-arrondi `roundf(x*10)/10` de
// history_export_chunk.h a failli migrer ici, cf. avertissement plus bas).
//
// Header-only, ADR-0017 : <stddef.h>/<stdint.h>/<stdio.h>/<math.h>/<string.h>
// uniquement — AUCUN Arduino.h, AUCUN FreeRTOS. Testable en natif sans libc++.
// =============================================================================

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <math.h>
#include <string.h>

// Encadrement du tableau JSON persisté — écrits UNE FOIS chacun par
// emitPersistedJson() (history.cpp), avant le premier point / après le
// dernier. Constantes plutôt que littéraux dupliqués : la mesure (passe 1,
// puits compteur) et l'écriture (passe 2, puits fichier) doivent utiliser
// EXACTEMENT la même chaîne, jamais deux copies qui pourraient diverger.
inline constexpr char kHistoryPersistedPrefix[] = "{\"data\":[";  // 9 o
inline constexpr char kHistoryPersistedSuffix[] = "]}";           // 2 o

// -----------------------------------------------------------------------
// historyFormatFixed — PORTAGE FIDÈLE de dtostrf(), PAS un snprintf("%.*f")
// -----------------------------------------------------------------------
// `String(float, n)` ET `String(double, n)` (WString.cpp,
// framework-arduinoespressif32) appellent TOUTES DEUX
// `dtostrf(value, n + 2, n, buf)` — seule la taille du `malloc()` temporaire
// diffère entre les deux surcharges (44 o pour `float`, 314 o pour `double`;
// sans incidence sur l'arithmétique). `point.ph/orp/temperature/phFiltered/
// orpFiltered` étant des `float` (history.h), l'ancien code (`String(point.ph,
// 2)`) résolvait vers la surcharge `float` — mais `dtostrf`
// (cores/esp32/stdlib_noniso.c) promeut son paramètre en `double` et se
// comporte IDENTIQUEMENT dans les deux cas : ce portage reste donc valide quel
// que soit l'overload d'origine. `dtostrf` N'APPELLE PAS printf : il ajoute
// `0.5·10⁻ⁿ` (rounding = 1/(2·10ⁿ)) puis TRONQUE chiffre à chiffre par
// divisions/soustractions successives. Ce n'est PAS équivalent à un arrondi
// "au plus proche, pair" de printf — sur le pas réel du DS18B20 (0,0625 °C),
// un balayage exhaustif (architecte, feature-131) a mesuré 174/2881 = 6,0 %
// de valeurs divergentes entre les deux méthodes. Exemple mesuré : x = -54,75
// → String(x,1) = "-54.7", "%.1f" = "-54.8".
//
// /history.json est écrit par `String(v, prec)` depuis sa création : changer
// de méthode de formatage ICI changerait le contenu du fichier pour des
// valeurs déjà en usage, ce qui est interdit par la spec (« le format ne doit
// pas changer », vérifié par comparaison octet à octet). D'où le portage
// bit-à-bit plutôt qu'un remplacement par printf.
//
// ⚠️ NE PAS "CORRIGER" CE PORT NI Y INTRODUIRE UN PRÉ-ARRONDI
// (`roundf(x * 10) / 10`, comme dans history_export_chunk.h) : mesuré ENCORE
// PLUS divergent de `String(x,n)` que `%.*f` (architecte, feature-131). Les
// deux modules ont des contraintes opposées : le format persisté doit rester
// bit-compatible avec l'historique déjà écrit, le format exporté n'a jamais eu
// cette contrainte (JSON reconstruit à chaque requête, jamais relu par le
// firmware lui-même).
//
// LIMITE ASSUMÉE (dépassement de capacité) : si le résultat ne tient pas dans
// `cap` (buffer trop petit) OU si `prec` dépasse kHistoryFormatFixedMaxPrec
// (garde-fou de taille de pile — jamais atteint en pratique : seuls prec=1 et
// prec=2 sont utilisés par ce firmware), la fonction renvoie 0 SANS RIEN
// ÉCRIRE. L'appelant (formatPersistedHistoryPoint) doit alors traiter le point
// comme "ne tient pas" et le sauter — jamais de JSON partiel.
constexpr uint8_t kHistoryFormatFixedMaxPrec = 17;  // > double::max_digits10 (17)

inline size_t historyFormatFixed(char* out, size_t cap, double v, uint8_t prec) {
  if (out == nullptr || cap == 0) return 0;
  if (prec > kHistoryFormatFixedMaxPrec) return 0;

  // NaN/Inf : dtostrf() écrit "nan"/"inf" sans consulter prec. Ne devrait
  // jamais arriver ici — formatPersistedHistoryPoint() filtre isfinite()
  // avant d'appeler ce port — mais on reste fidèle au comportement source.
  if (isnan(v)) {
    if (cap < 4) return 0;
    memcpy(out, "nan", 3);
    out[3] = '\0';
    return 3;
  }
  if (isinf(v)) {
    if (cap < 4) return 0;
    memcpy(out, "inf", 3);
    out[3] = '\0';
    return 3;
  }

  // Buffer local de travail : sign + digits entiers + '.' + prec décimales +
  // NUL. 40 o couvre très largement tout double dont la partie entière tient
  // sur 15 chiffres significatifs (au-delà, dtostrf() lui-même n'a plus de
  // sens — un double n'a que ~17 chiffres significatifs au total) avec
  // prec <= kHistoryFormatFixedMaxPrec.
  char buf[40];
  char* w = buf;
  bool negative = false;

  // Idiome EXACT de String(double,prec)/String(float,prec) : width = prec+2.
  const int width = static_cast<int>(prec) + 2;
  int fillme = width;
  if (prec > 0) fillme -= static_cast<int>(prec + 1);

  double number = v;
  if (number < 0.0) {
    negative = true;
    fillme--;
    number = -number;
  }

  // « Round correctly so that print(1.999, 2) prints as "2.00" » (commentaire
  // d'origine, cores/esp32/stdlib_noniso.c) — ce N'EST PAS un arrondi au sens
  // usuel, c'est un décalage additif suivi d'une troncature.
  double rounding = 2.0;
  for (uint8_t i = 0; i < prec; ++i) rounding *= 10.0;
  rounding = 1.0 / rounding;
  number += rounding;

  double tenpow = 1.0;
  unsigned int digitcount = 1;
  while (number >= 10.0 * tenpow) {
    tenpow *= 10.0;
    digitcount++;
    // Garde-fou anti-débordement du buffer local : ne devrait jamais se
    // déclencher pour des mesures pH/ORP/température, mais un `double`
    // pathologique (NaN/Inf déjà écartés, donc une magnitude énorme) ne doit
    // pas dépasser silencieusement `buf`.
    if (digitcount > 30) return 0;
  }

  number /= tenpow;
  fillme -= static_cast<int>(digitcount);

  // Espaces de remplissage — n'intervient jamais en pratique pour prec∈{1,2}
  // et des valeurs pH/ORP/température (au moins 1 chiffre entier), conservé
  // pour la fidélité du portage.
  while (fillme-- > 0) *w++ = ' ';
  if (negative) *w++ = '-';

  unsigned int totalDigits = digitcount + prec;
  int8_t digit = 0;
  while (totalDigits-- > 0) {
    digit = static_cast<int8_t>(number);
    if (digit > 9) digit = 9;  // garde-fou d'origine (« insurance »)
    if (static_cast<size_t>(w - buf) >= sizeof(buf) - 1) return 0;  // garde-fou buffer
    *w++ = static_cast<char>('0' | digit);
    if (totalDigits == prec && prec > 0) {
      if (static_cast<size_t>(w - buf) >= sizeof(buf) - 1) return 0;
      *w++ = '.';
    }
    number -= digit;
    number *= 10.0;
  }
  *w = '\0';

  const size_t len = static_cast<size_t>(w - buf);
  if (len >= cap) return 0;  // ne tient pas dans la sortie de l'appelant
  memcpy(out, buf, len + 1);
  return len;
}

// -----------------------------------------------------------------------
// formatPersistedHistoryPoint — UN point du tableau JSON de /history.json
// -----------------------------------------------------------------------
// Format PERSISTÉ EXACT (reproduit à l'identique l'ancienne construction par
// JsonDocument de HistoryManager::saveToFile()) :
//   ORDRE DES CLÉS : t, p, o, T, pf, of, f, d, g — un consommateur qui
//   dépendrait de l'ordre serait déjà en tort (JSON), mais reproduire l'ordre
//   d'origine simplifie la preuve « octet à octet identique ».
//   - "t"  : timestamp, entier, TOUJOURS présent.
//   - "p"  : pH, 2 décimales, OMIS si non fini.
//   - "o"  : ORP, 1 décimale, OMIS si non fini.
//   - "T"  : température, 1 décimale, OMIS si non fini.
//   - "pf" : pH filtré, 2 décimales, OMIS si non fini OU si granularity != RAW
//            (feature-095 : jamais persisté sur un agrégat HOURLY/DAILY).
//   - "of" : ORP filtré, 1 décimale, mêmes conditions que "pf".
//   - "f"  : filtration active, booléen, TOUJOURS présent ("true"/"false").
//   - "d"  : masque de dosage (kHistDose*), entier, TOUJOURS présent.
//   - "g"  : granularité (Granularity), entier, TOUJOURS présent.
// `granularity` est reçu en uint8_t brut (idiome de history_import_logic.h) :
// ce module ne dépend pas de l'enum Granularity (history.h inclut Arduino.h).
// La valeur RAW == 0 (cf. history.h) est reproduite ici par
// kHistoryPersistRawGranularity, à tenir manuellement synchronisée.
//
// `first` : true pour le premier point du tableau (aucune virgule de tête),
// false sinon (virgule de séparation, comme formatHistoryPointJson).
//
// Retour : longueur écrite dans `out` (hors NUL), ou 0 si le point ne tient
// pas dans `cap` (kHistoryPersistPointMaxBytes, constants.h) — JAMAIS de JSON
// partiel émis. L'appelant (emitPersistedJson, history.cpp) saute alors ce
// point, IDENTIQUEMENT dans la passe de mesure et la passe d'écriture — les
// deux ne peuvent donc jamais diverger (même code, même buffer, même seuil).
constexpr uint8_t kHistoryPersistRawGranularity = 0;  // == Granularity::RAW (history.h)

inline size_t formatPersistedHistoryPoint(char* out, size_t cap, bool first,
                                          unsigned long ts, float ph, float orp,
                                          float temp, float phFiltered,
                                          float orpFiltered, bool filtration,
                                          uint8_t doseMask, uint8_t granularity) {
  if (out == nullptr || cap == 0) return 0;

  size_t pos = 0;
  // `+1` : réserve systématiquement la place du NUL final, jamais écrit par
  // erreur hors de `cap`.
  auto put = [&](const char* s, size_t n) -> bool {
    if (pos + n + 1 > cap) return false;
    memcpy(out + pos, s, n);
    pos += n;
    return true;
  };
  auto putChar = [&](char c) -> bool { return put(&c, 1); };
  auto putNumberField = [&](const char* key, size_t keyLen, double v,
                            uint8_t prec) -> bool {
    char num[24];
    const size_t n = historyFormatFixed(num, sizeof(num), v, prec);
    if (n == 0) return false;
    return put(key, keyLen) && put(num, n);
  };

  if (!first && !putChar(',')) return 0;

  {
    char head[24];
    const int n = snprintf(head, sizeof(head), "{\"t\":%lu", ts);
    if (n < 0 || static_cast<size_t>(n) >= sizeof(head)) return 0;
    if (!put(head, static_cast<size_t>(n))) return 0;
  }

  if (isfinite(ph) && !putNumberField(",\"p\":", 5, static_cast<double>(ph), 2)) {
    return 0;  // échec réel de formatage (buffer trop petit)
  }
  if (isfinite(orp) && !putNumberField(",\"o\":", 5, static_cast<double>(orp), 1)) {
    return 0;
  }
  if (isfinite(temp) && !putNumberField(",\"T\":", 5, static_cast<double>(temp), 1)) {
    return 0;
  }

  // feature-095 : la filtrée n'est persistée QUE sur les points RAW.
  if (granularity == kHistoryPersistRawGranularity) {
    if (isfinite(phFiltered) &&
        !putNumberField(",\"pf\":", 6, static_cast<double>(phFiltered), 2)) {
      return 0;
    }
    if (isfinite(orpFiltered) &&
        !putNumberField(",\"of\":", 6, static_cast<double>(orpFiltered), 1)) {
      return 0;
    }
  }

  if (filtration) {
    if (!put(",\"f\":true", 9)) return 0;
  } else {
    if (!put(",\"f\":false", 10)) return 0;
  }

  {
    char dbuf[24];
    const int n = snprintf(dbuf, sizeof(dbuf), ",\"d\":%u", (unsigned)doseMask);
    if (n < 0 || static_cast<size_t>(n) >= sizeof(dbuf)) return 0;
    if (!put(dbuf, static_cast<size_t>(n))) return 0;
  }
  {
    char gbuf[24];
    const int n = snprintf(gbuf, sizeof(gbuf), ",\"g\":%u}", (unsigned)granularity);
    if (n < 0 || static_cast<size_t>(n) >= sizeof(gbuf)) return 0;
    if (!put(gbuf, static_cast<size_t>(n))) return 0;
  }

  out[pos] = '\0';
  return pos;
}

#endif  // HISTORY_PERSIST_FORMAT_H
