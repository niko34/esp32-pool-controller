#include "schedule_logic.h"

#include <string.h>

// =============================================================================
// schedule_logic — implémentation PURE (feature-038, characterization refactor)
// =============================================================================
// IMPORTANT : NE PAS modifier les seuils, l'ordre des comparaisons ni les
// arrondis. Tout doit rester strictement identique à filtration.cpp d'origine.

// Parse les `digits` premiers chiffres décimaux d'une sous-chaîne, à la manière
// de String::substring(...).toInt() pour une entrée normalisée "HH:MM" : on lit
// les chiffres consécutifs et on s'arrête au premier non-chiffre. Sur "HH:MM"
// les positions [0,1] et [3,4] sont garanties contenir 2 caractères (longueur
// >= 5 vérifiée par l'appelant) → équivalent à substring(a,b).toInt().
static int parseTwoDigits(const char* s) {
  int value = 0;
  for (int i = 0; i < 2; i++) {
    char c = s[i];
    if (c < '0' || c > '9') break;
    value = value * 10 + (c - '0');
  }
  return value;
}

int timeStringToMinutes(const char* hhmm) {
  if (hhmm == nullptr) return -1;
  if (strlen(hhmm) < 5 || hhmm[2] != ':') return -1;
  int hh = parseTwoDigits(&hhmm[0]);
  int mm = parseTwoDigits(&hhmm[3]);
  if (hh < 0 || hh > 23 || mm < 0 || mm > 59) return -1;
  return hh * 60 + mm;
}

bool isMinutesInRange(int now, int start, int end, bool equalMeansAlways) {
  // La garde -1 reste AVANT le test start==end : sinon -1==-1 renverrait
  // equalMeansAlways à tort.
  if (start == -1 || end == -1) return false;
  if (start == end) return equalMeansAlways;  // filtration: false ; éclairage: true
  if (start < end) {
    return now >= start && now < end;
  }
  return now >= start || now < end;
}

int remainingRangeMinutes(int nowMin, int startMin, int endMin) {
  // Réutilise isMinutesInRange (equalMeansAlways=false) : couvre start/end==-1,
  // start==end (plage invalide) et le cas hors plage → 0 (feature-011).
  if (!isMinutesInRange(nowMin, startMin, endMin)) return 0;
  if (startMin < endMin) {
    return endMin - nowMin;
  }
  // Plage à cheval sur minuit (start > end, garanti par isMinutesInRange).
  if (nowMin >= startMin) {
    // Partie du soir : horizon BORNÉ À MINUIT (les compteurs journaliers se
    // réinitialisent à minuit — la répartition ne franchit jamais la frontière).
    return 1440 - nowMin;
  }
  // Partie du matin (nowMin < endMin).
  return endMin - nowMin;
}

FiltrationMode parseFiltrationMode(const char* token) {
  if (token == nullptr) return FiltrationMode::Inconnu;
  // UNIQUE site de correspondance jeton -> sens (voir schedule_logic.h).
  if (strcmp(token, "auto") == 0)   return FiltrationMode::Auto;
  if (strcmp(token, "manual") == 0) return FiltrationMode::Programme;
  if (strcmp(token, "force") == 0)  return FiltrationMode::Marche;
  if (strcmp(token, "off") == 0)    return FiltrationMode::Arret;
  return FiltrationMode::Inconnu;
}

RunWindow effectiveRunWindow(ScheduleWindow base, bool boostActive, int nowMin) {
  if (!boostActive) return RunWindow{base};
  // Boost actif : créneau élargi jusqu'au prochain minuit (expiration exacte
  // du Boost, cf. tickBoostExpiry). 1440, pas 0 : évite start==end à 00:00.
  return RunWindow{ScheduleWindow{nowMin, 1440}};
}

ScheduleWindow computeAutoWindow(float tempC, float pivotHour) {
  if (tempC < 0) tempC = 0;
  float durationHours = tempC / 2.0f;
  if (durationHours < 1.0f) durationHours = 1.0f;
  if (durationHours > 24.0f) durationHours = 24.0f;

  float startHour = pivotHour - (durationHours / 2.0f);
  float endHour = startHour + durationHours;

  // wrap dans [0, 24)
  while (startHour < 0) startHour += 24.0f;
  while (startHour >= 24.0f) startHour -= 24.0f;
  while (endHour < 0) endHour += 24.0f;
  while (endHour >= 24.0f) endHour -= 24.0f;

  // Conversion heure flottante → minutes avec arrondi/carry identique à l'ancien
  // float→"HH:MM"→re-parse.
  auto toMinutes = [](float hour) -> int {
    int h = static_cast<int>(hour);
    int m = static_cast<int>(round((hour - h) * 60.0f));
    if (m >= 60) { m -= 60; h = (h + 1) % 24; }
    return h * 60 + m;
  };

  return { toMinutes(startHour), toMinutes(endHour) };
}

bool decideFiltrationRun(FiltrationMode mode, bool haveTime, int nowMin, int startMin, int endMin,
                         bool currentlyRunning) {
  // feature-146 : quatre modes, chacun fait EXACTEMENT ce que son nom annonce.
  // `startMin`/`endMin` sont déjà le créneau EFFECTIF (base, ou élargi par le
  // Boost via effectiveRunWindow — résolu par l'appelant, schedule_logic.h).
  // Switch EXHAUSTIF, SANS `default` : un mode non traité ne compile pas
  // (-Wswitch), au lieu de tomber silencieusement dans un `return` par
  // défaut — exactement le défaut qui a rendu "force" invisible avant ce lot.
  switch (mode) {
    case FiltrationMode::Auto:
    case FiltrationMode::Programme:
      // Heure indisponible : on conserve l'état courant pour éviter un faux
      // start/stop pendant un OTA ou une perte RTC (condition pool-chemistry).
      return haveTime ? isMinutesInRange(nowMin, startMin, endMin) : currentlyRunning;
    case FiltrationMode::Marche:
      return true;
    case FiltrationMode::Arret:
      return false;
    case FiltrationMode::Inconnu:
      // Fail-safe : état courant conservé plutôt qu'un start/stop sur un
      // jeton non reconnu. ensureTimesValid() ramène normalement tout jeton
      // inconnu à Auto en tête d'update() — ce cas protège une course entre
      // lecture et coercition.
      return currentlyRunning;
  }
  return currentlyRunning;  // inatteignable (switch exhaustif ci-dessus)
}

LightingMode parseLightingMode(const char* token) {
  if (token == nullptr) return LightingMode::Inconnu;
  // UNIQUE site de correspondance jeton -> sens, forward (voir schedule_logic.h).
  if (strcmp(token, "on") == 0)       return LightingMode::On;
  if (strcmp(token, "off") == 0)      return LightingMode::Off;
  if (strcmp(token, "schedule") == 0) return LightingMode::Programme;
  return LightingMode::Inconnu;
}

LightingMode migrateLightingMode(bool lightEnabled, bool lightScheduleEnabled) {
  // light_sched_en PRIME : une installation où les deux anciens booléens sont
  // vrais migre vers Programme, jamais vers Allumé (voir schedule_logic.h).
  if (lightScheduleEnabled) return LightingMode::Programme;
  return lightEnabled ? LightingMode::On : LightingMode::Off;
}

const char* lightingModeToken(LightingMode mode) {
  // UNIQUE site de correspondance sens -> jeton, réciproque de parseLightingMode.
  switch (mode) {
    case LightingMode::On:        return "on";
    case LightingMode::Off:       return "off";
    case LightingMode::Programme: return "schedule";
    case LightingMode::Inconnu:   return "off";  // fail-closed, non choisissable
  }
  return "off";  // inatteignable (switch exhaustif ci-dessus)
}

bool decideLightingOn(LightingMode mode, bool haveTime, int nowMin, int startMin, int endMin,
                      bool currentlyOn) {
  // feature-162 : trois modes, chacun fait EXACTEMENT ce que son nom annonce.
  // Switch EXHAUSTIF, SANS `default` : un mode non traité ne compile pas
  // (-Wswitch), même motif que decideFiltrationRun ci-dessus.
  switch (mode) {
    case LightingMode::On:
      return true;
    case LightingMode::Off:
      return false;
    case LightingMode::Programme:
      // Heure indisponible : on conserve l'état courant pour éviter un faux
      // toggle pendant un OTA / perte RTC.
      return haveTime ? isMinutesInRange(nowMin, startMin, endMin, true) : currentlyOn;
    case LightingMode::Inconnu:
      // Fail-safe : état courant conservé plutôt qu'un toggle sur un jeton
      // non reconnu. ensureTimesValid() ramène normalement tout jeton inconnu
      // à Off en tête d'update() — ce cas protège une course entre lecture et
      // coercition.
      return currentlyOn;
  }
  return currentlyOn;  // inatteignable (switch exhaustif ci-dessus)
}

// =============================================================================
// bug-126 — planning auto calculé à l'arrêt, promu à minuit
// =============================================================================

void resetCycleTempAccumulator(CycleTempAccumulator& acc) {
  acc.mean = NAN;
  acc.minC = NAN;
  acc.maxC = NAN;
  acc.count = 0;
}

bool isSampleEligible(bool circulationSettled, bool inSchedule, bool temperatureFrozen) {
  return circulationSettled && inSchedule && !temperatureFrozen;
}

bool accumulateCycleTemp(CycleTempAccumulator& acc, float sampleC, bool eligible) {
  if (!eligible || isnan(sampleC) || isinf(sampleC)) return false;
  if (acc.count == 0) {
    acc.mean = sampleC;
    acc.minC = sampleC;
    acc.maxC = sampleC;
    acc.count = 1;
    return true;
  }
  // Saturation à 65535 (~45 jours à 1 éch./min, jamais atteint sur un cycle) :
  // on garde le dernier état plutôt que de sur-pondérer artificiellement.
  if (acc.count >= 65535) return true;
  acc.count++;
  // Moyenne INCRÉMENTALE, jamais une somme (voir schedule_logic.h).
  acc.mean += (sampleC - acc.mean) / (float)acc.count;
  if (sampleC < acc.minC) acc.minC = sampleC;
  if (sampleC > acc.maxC) acc.maxC = sampleC;
  return true;
}

bool isPlausibleReferenceTemp(float tempC, float minC, float maxC) {
  if (isnan(tempC) || isinf(tempC)) return false;
  return tempC >= minC && tempC <= maxC;
}

bool selectFiltrationRefTemp(const uint8_t* roles, const float* tempsC,
                             uint8_t count, float& outTempC) {
  // Encodage attendu : 0=Unknown, 1=Water, 2=Circuit (SondeRole, vérifié par
  // static_assert côté coquille — ce module n'a pas accès à l'enum C++).
  bool anyCircuit = false;
  for (uint8_t i = 0; i < count; i++) {
    if (roles[i] == 2) anyCircuit = true;
  }
  for (uint8_t i = 0; i < count; i++) {
    if (roles[i] == 1 && !isnan(tempsC[i]) && !isinf(tempsC[i])) {
      outTempC = tempsC[i];
      return true;
    }
  }
  // Repli UNIQUEMENT si aucune sonde Circuit n'est déclarée : dès qu'une l'est,
  // une sonde Unknown pourrait tout aussi bien être la seconde sonde eau non
  // identifiée — le repli deviendrait un pari, pas une tolérance de mise en
  // service.
  if (anyCircuit) return false;
  for (uint8_t i = 0; i < count; i++) {
    if (roles[i] == 0 && !isnan(tempsC[i]) && !isinf(tempsC[i])) {
      outTempC = tempsC[i];
      return true;
    }
  }
  return false;
}

RefTempVerdict resolveReferenceTemp(const CycleTempAccumulator& acc, uint16_t minSamples,
                                    bool bootstrapAllowed, float minC, float maxC,
                                    float& outTempC) {
  if (acc.count == 0) return RefTempVerdict::NoSample;
  if (!bootstrapAllowed && acc.count < minSamples) return RefTempVerdict::TooShort;
  // P2-A : bornes traitées ASYMÉTRIQUEMENT. `mean < minC` = hivernage normal
  // (TooCold, pas d'alerte) ; seul `mean > maxC` (ou non fini) reste Implausible
  // (panne de sonde). `acc.mean` ne peut être NaN/inf quand count > 0
  // (accumulateCycleTemp ne retient qu'un échantillon fini) — isPlausibleReferenceTemp
  // le revérifie quand même, défensivement.
  if (acc.mean < minC) return RefTempVerdict::TooCold;
  if (!isPlausibleReferenceTemp(acc.mean, minC, maxC)) return RefTempVerdict::Implausible;
  outTempC = acc.mean;
  return RefTempVerdict::Ok;
}

PromotionVerdict decideSchedulePromotion(bool pendingValid, bool newDay,
                                         bool alreadyPromotedToday, bool basisUnchanged,
                                         bool modeIsAuto, bool pumpRunning) {
  if (!pendingValid) return PromotionVerdict::NoPending;
  if (!newDay) return PromotionVerdict::NotNewDay;
  if (alreadyPromotedToday) return PromotionVerdict::AlreadyPromoted;
  if (!basisUnchanged) return PromotionVerdict::BasisChanged;
  if (!modeIsAuto) return PromotionVerdict::NotAuto;
  if (pumpRunning) return PromotionVerdict::PumpRunning;
  return PromotionVerdict::Promote;
}
