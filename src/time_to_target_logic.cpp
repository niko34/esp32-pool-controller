// time_to_target_logic.cpp — voir l'en-tête pour le POURQUOI complet.
// Module PUR (ADR-0017) : aucune dépendance Arduino, aucune allocation.

#include "time_to_target_logic.h"

// -----------------------------------------------------------------------------
// Sceaux
// -----------------------------------------------------------------------------

uint32_t tttFnv1a(const void* data, uint32_t len) {
  const uint8_t* p = static_cast<const uint8_t*>(data);
  uint32_t h = 2166136261UL;  // offset basis FNV-1a
  for (uint32_t i = 0; i < len; ++i) {
    h ^= p[i];
    h *= 16777619UL;  // prime FNV-1a
  }
  return h;
}

uint32_t tttHistoryCrc(const TttHistory& h) {
  return tttFnv1a(&h, (uint32_t)offsetof(TttHistory, crc));
}

void tttHistorySeal(TttHistory& h, uint32_t serieStamp) {
  h.schema = kTttHistorySchema;
  // Les octets d'alignement DOIVENT être déterministes : sans cela, deux
  // écritures du même contenu produiraient des empreintes différentes selon les
  // octets de pile laissés dans le trou.
  h.pad0 = 0;
  h.pad1 = 0;
  h.pad2 = 0;
  h.serieStamp = serieStamp;
  h.crc = tttHistoryCrc(h);
}

bool tttHistoryIntact(const TttHistory& h, size_t bytesRead) {
  if (bytesRead != sizeof(TttHistory)) return false;
  if (h.schema != kTttHistorySchema) return false;
  return tttHistoryCrc(h) == h.crc;
}

uint32_t tttRtcCrc(const TttRtcRecord& r) {
  return tttFnv1a(&r, (uint32_t)offsetof(TttRtcRecord, crc));
}

void tttRtcSeal(TttRtcRecord& r) {
  r.magic = kTttRtcMagic;
  for (uint8_t c = 0; c < kTttChannelCount; ++c) {
    r.live[c].pad[0] = 0;
    r.live[c].pad[1] = 0;
    r.live[c].pad[2] = 0;
  }
  r.crc = tttRtcCrc(r);
}

bool tttRtcIntact(const TttRtcRecord& r) {
  if (r.magic != kTttRtcMagic) return false;
  return tttRtcCrc(r) == r.crc;
}

// -----------------------------------------------------------------------------
// Anneau
// -----------------------------------------------------------------------------

bool tttHistoryAppend(TttHistory& h, uint8_t chan, const TttDay& d) {
  if (chan >= kTttChannelCount) return false;
  if (d.date == 0) return false;  // sans horloge, une journée n'est pas datable

  const uint8_t n = kTimeToTargetHistoryDays;
  TttDay* row = h.days[chan];
  // Une date DÉJÀ présente est remplacée, où qu'elle soit dans l'anneau : la
  // clôture réécrit ainsi l'entrée « en cours » posée à t0, et un recul
  // d'horloge à cheval sur minuit ne peut pas compter la même journée deux fois.
  for (uint8_t i = 0; i < n; ++i) {
    if (row[i].date == d.date) {
      row[i] = d;
      return true;
    }
  }
  for (uint8_t i = 0; i + 1 < n; ++i) row[i] = row[i + 1];
  row[n - 1] = d;
  return true;
}

const TttDay* tttHistoryFind(const TttHistory& h, uint8_t chan, uint32_t date) {
  if (chan >= kTttChannelCount || date == 0) return nullptr;
  for (uint8_t i = 0; i < kTimeToTargetHistoryDays; ++i) {
    if (h.days[chan][i].date == date) return &h.days[chan][i];
  }
  return nullptr;
}

uint8_t tttHistoryCloseOrphans(TttHistory& h, uint32_t today) {
  // GARDE 1 — heure invalide : on ne sait pas quel jour on est, donc AUCUNE
  // journée n'est « révolue ». Sans elle, un démarrage sans NTP clôturerait
  // tout l'anneau et détruirait sept journées d'observation.
  if (today == 0) return 0;

  uint8_t closed = 0;
  for (uint8_t c = 0; c < kTttChannelCount; ++c) {
    for (uint8_t i = 0; i < kTimeToTargetHistoryDays; ++i) {
      TttDay& d = h.days[c][i];
      if (d.date == 0) continue;
      // GARDE 2 — la journée EN COURS est épargnée.
      if (d.date == today) continue;
      // GARDE 3 — seule une journée restée « en cours » est close. Une journée
      // ATTEINTE n'est jamais rouverte : ce serait fabriquer une régression
      // apparente sur la métrique qui arbitre les comparaisons de versions.
      if (d.state != kTttStateRunning) continue;
      d.state = kTttStateMissed;
      d.flags = (uint8_t)(d.flags | kTttFlagPowerCycle);
      ++closed;
    }
  }
  return closed;
}

// -----------------------------------------------------------------------------
// Machine d'états
// -----------------------------------------------------------------------------

uint16_t tttElapsedMinutes(const TttLive& st, uint32_t filtTotalMin) {
  // Compteur en recul (NVS `pool-daily` effacée, restauration d'usine) : une
  // durée négative n'existe pas — on rend 0 plutôt qu'un repliement à 4 milliards.
  if (filtTotalMin < st.t0FiltMin) return 0;
  const uint32_t elapsed = filtTotalMin - st.t0FiltMin;
  return (elapsed > 1440UL) ? (uint16_t)1440 : (uint16_t)elapsed;
}

void tttLiveReset(TttLive& st) {
  st = TttLive{};  // initialisation par valeur : tous les champs à zéro
}

TttDay tttEntryFromLive(const TttLive& st) {
  TttDay d;
  d.date = st.date;
  d.e0 = st.e0;
  d.minutes = st.minutes;
  d.state = st.state;
  d.flags = st.flags;
  return d;
}

namespace {

// Écart résiduel à la clôture : NAN quand la mesure est absente — « indisponible »
// se dit NAN, jamais 0 (condition C3 du gate).
float residualOrNan(const TttInputs& in) {
  if (!isfinite(in.value) || !isfinite(in.target)) return NAN;
  return in.value - in.target;
}

// Clôture d'une journée en « non atteinte » : l'état devient 3, les minutes
// portent la durée TOTALE de filtration écoulée depuis t0.
void closeMissed(TttLive& st, const TttInputs& in, TttVerdict& v) {
  st.minutes = tttElapsedMinutes(st, in.filtTotalMin);
  st.state = kTttStateMissed;
  st.holding = 0;
  st.holdStartFiltMin = 0;
  v.event = TttEvent::kMissed;
  v.entry = tttEntryFromLive(st);
  v.residual = residualOrNan(in);
}

// Circonstances du jour, posées en DEUX endroits : à chaque tick d'une journée
// en cours (étape 3) ET au départ (étape 5), parce que l'étape 3 est sautée
// tant que l'état vaut Idle et que l'entrée archivée doit néanmoins porter les
// circonstances du moment. Factorisé ici après la revue : la duplication avait
// exigé une mutation par site (M5 et M5b) pour être couverte, et deux copies
// d'une même règle finissent toujours par diverger.
// Le boost n'est JAMAIS un « changement de consigne » (bit 0) : on juge contre
// la consigne effective, on ne surveille que la consigne configurée.
void applyCircumstances(TttLive& st, const TttInputs& in) {
  if (in.boostActive) st.flags |= kTttFlagBoost;
  if (in.unsettled) st.flags |= kTttFlagUnsettled;
}

}  // namespace

TttVerdict tttTick(TttLive& st, const TttInputs& in) {
  TttVerdict v;

  // 1. Heure inexploitable : l'indicateur est SUSPENDU. Aucune entrée ne doit
  //    être datée faux — mieux vaut une journée manquante qu'une journée fausse.
  if (in.date == 0) {
    tttLiveReset(st);
    return v;
  }

  // 2. Bascule de jour civil. La filtration peut tourner à minuit (mode manuel
  //    ON toute la nuit) : la journée qui s'achève se clôt en « non atteinte »,
  //    et le tick suivant posera un nouveau t0. Le chronomètre ne traverse
  //    JAMAIS minuit — c'est la définition normative de la métrique.
  if (st.date != 0 && st.date != in.date) {
    if (st.state == kTttStateRunning) closeMissed(st, in, v);
    tttLiveReset(st);
    return v;
  }

  // 3. Drapeaux vivants — cumulatifs, jamais effacés avant la fin de journée.
  //    Seulement tant que le chronomètre TOURNE : une fois la journée close et
  //    archivée, un drapeau posé n'atteindrait plus la NVS et l'instantané
  //    servi à l'UI divergerait de la pastille mémorisée.
  if (st.state == kTttStateRunning) {
    // On SURVEILLE la consigne CONFIGURÉE : un boost ORP déplace la consigne
    // EFFECTIVE sans être une modification utilisateur, il ne doit donc pas
    // lever le drapeau « consigne modifiée » (il a son propre bit).
    if (isfinite(in.configuredTarget) && in.configuredTarget != st.watchTarget) {
      st.flags |= kTttFlagTargetChanged;
      st.watchTarget = in.configuredTarget;
    }
    applyCircumstances(st, in);
  }

  // 4. Clôture de la journée — DEUX causes, DEUX lectures.
  //    - Fin de journée de filtration : « non atteinte » NUE. C'est ce que la
  //      métrique doit voir, y compris quand la cause est un quota atteint, une
  //      limite horaire ou un actionneur sous-dimensionné : ce sont des
  //      décisions de la régulation ELLE-MÊME.
  //    - Régulation débranchée par l'OPÉRATEUR : « interrompue » (bit 4). Mêmes
  //      minutes, même e0, mais la journée s'exclut des moyennes de comparaison.
  //      Les minutes sont FIGÉES à l'instant de la bascule ; un retour en
  //      automatique le même jour ne repose PAS de t0 (l'état n'est plus
  //      « en cours », l'invariant « un seul départ par jour » prime).
  if (st.state == kTttStateRunning && (in.dayEnd || !in.automatic)) {
    if (!in.automatic) st.flags |= kTttFlagOperatorOff;
    closeMissed(st, in, v);
    return v;
  }

  // 5. Départ. t0 est le premier instant où la grandeur de décision EXISTE et
  //    est PLAUSIBLE. La garde refuse seulement de DÉMARRER — elle n'invente
  //    jamais de valeur, et t0 sera posé au premier échantillon plausible.
  //    Une journée qui COMMENCE en manuel pose quand même son t0 : le tick
  //    suivant la clôt aussitôt avec le bit 4 et 0 minute. L'historique reste
  //    ainsi DENSE — un trou muet se lirait comme un bug de l'indicateur.
  if (st.state == kTttStateIdle) {
    if (!in.filtrationRunning) return v;
    if (!isfinite(in.value) || !isfinite(in.target)) return v;
    const float e0 = in.value - in.target;
    if (!isfinite(e0)) return v;
    if (!(in.maxPlausibleE0 > 0.0f) || fabsf(e0) > in.maxPlausibleE0) {
      v.event = TttEvent::kImplausible;
      v.rejected = e0;
      return v;
    }
    st.date = in.date;
    st.t0FiltMin = in.filtTotalMin;
    st.holdStartFiltMin = 0;
    st.holding = 0;
    st.e0 = e0;
    st.watchTarget = in.configuredTarget;
    st.minutes = 0;
    st.state = kTttStateRunning;
    st.flags = 0;
    applyCircumstances(st, in);
    v.event = TttEvent::kStarted;
    v.entry = tttEntryFromLive(st);
    // La tenue est évaluée au tick SUIVANT (1 s plus tard, même minute de
    // filtration) : un tick ne rend qu'un événement, et le départ doit être
    // journalisé et persisté. Le cas « déjà à la cible » y démarre donc sa
    // tenue avec holdStartFiltMin == t0FiltMin, soit un temps final de 0 min.
    return v;
  }

  // 6. En cours : tenue dans la bande. Les états 2 et 3 sont FIGÉS jusqu'à
  //    minuit (un rebond après l'atteinte ne rouvre pas la journée).
  if (st.state != kTttStateRunning) return v;

  st.minutes = tttElapsedMinutes(st, in.filtTotalMin);

  // Une mesure absente vaut SORTIE DE BANDE (C3) : la tenue repart de zéro, le
  // chronomètre continue.
  const bool inBand = isfinite(in.value) && isfinite(in.target) &&
                      isfinite(in.band) && in.band > 0.0f &&
                      fabsf(in.value - in.target) < in.band;
  if (!inBand) {
    st.holding = 0;
    st.holdStartFiltMin = 0;
    return v;
  }

  if (!st.holding) {
    st.holding = 1;
    st.holdStartFiltMin = in.filtTotalMin;
    return v;
  }
  if (in.filtTotalMin < st.holdStartFiltMin) {  // compteur en recul : tenue relancée
    st.holdStartFiltMin = in.filtTotalMin;
    return v;
  }
  if ((in.filtTotalMin - st.holdStartFiltMin) >= kTimeToTargetHoldMin) {
    // Le temps rendu est celui du DÉBUT de la tenue, pas de sa fin : la cible a
    // été atteinte à holdStartFiltMin, les 30 minutes suivantes n'ont fait que
    // le prouver.
    st.state = kTttStateReached;
    const uint32_t reached = (st.holdStartFiltMin >= st.t0FiltMin)
                                 ? (st.holdStartFiltMin - st.t0FiltMin)
                                 : 0UL;
    st.minutes = (reached > 1440UL) ? (uint16_t)1440 : (uint16_t)reached;
    st.holding = 0;
    v.event = TttEvent::kReached;
    v.entry = tttEntryFromLive(st);
    v.residual = residualOrNan(in);  // écart au moment de la validation de la tenue
  }
  return v;
}
