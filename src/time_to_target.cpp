// time_to_target.cpp — coquille de l'indicateur « temps d'atteinte de cible ».
// Voir time_to_target.h pour les contextes d'appel et l'intention structurelle
// (sens UNIQUE : cet organe lit la régulation, la régulation ne le lit jamais).

#include "time_to_target.h"

#include <time.h>

#include "config.h"
#include "constants.h"
#include "filtration.h"
#include "logger.h"
#include "measure_validity.h"
#include "pump_controller.h"  // grandeur de décision pH + consigne ORP effective
#include "sensors.h"          // ORP filtré = grandeur de décision ORP

TimeToTargetManager timeToTarget;

namespace {

// Mémoire RTC de l'état VIVANT : survit à un reset logiciel / panic / watchdog,
// PAS à une coupure secteur (RAM RTC aléatoire ⇒ magic/CRC faux ⇒ reprise par
// la NVS, fail-closed). Magic et structure PROPRES à cet instrument.
RTC_NOINIT_ATTR TttRtcRecord sTttRtc;

// Cadence du tick (le chronomètre compte des minutes : la seconde suffit).
constexpr uint32_t kTttTickMs = 1000UL;

// Arrêt de filtration valant fin de journée, converti en millisecondes murales.
constexpr uint32_t kTttOffDayEndMs = (uint32_t)kTimeToTargetOffDayEndMin * 60000UL;

// Libellés de voie et précision d'affichage de l'écart (pH : 2 décimales ;
// ORP : entier, en mV).
const char* const kTttChanName[kTttChannelCount] = {"pH", "ORP"};

// Écart formaté, NaN-safe : une mesure absente se lit "n/a", jamais "0.00"
// (condition C3 du gate : NAN = absence, jamais « à la consigne »).
void formatError(char* out, size_t len, float v, uint8_t chan) {
  if (!isfinite(v)) {
    snprintf(out, len, "n/a");
    return;
  }
  if (chan == kTttChannelPh) {
    snprintf(out, len, "%+.2f", (double)v);
  } else {
    snprintf(out, len, "%+.0f mV", (double)v);
  }
}

}  // namespace

// -----------------------------------------------------------------------------
// Journal — UN SEUL gabarit pour tous les événements
// -----------------------------------------------------------------------------
// Trois messages distincts pesaient ~0,6 Ko de chaînes sur une flash occupée à
// 98,7 % : le gabarit est unique et paramétré par la phase.
void TimeToTargetManager::logEvent(uint8_t chan, const char* phase,
                                   uint16_t minutes, float e0, float residual,
                                   bool warn) {
  char initial[16];
  char current[16];
  formatError(initial, sizeof(initial), e0, chan);
  formatError(current, sizeof(current), residual, chan);
  char msg[144];
  snprintf(msg, sizeof(msg),
           "[KPI] %s : %s — %u min de filtration, écart initial %s, écart "
           "courant %s",
           kTttChanName[chan], phase, (unsigned)minutes, initial, current);
  if (warn) {
    systemLogger.warning(msg);
  } else {
    systemLogger.info(msg);
  }
}

// -----------------------------------------------------------------------------
// Persistance — DEUX ÉTAGES
// -----------------------------------------------------------------------------
// NVS : `t0FiltMin`, `e0`, la date et les drapeaux, écrits UNE FOIS quand t0 est
// posé (c'est l'unique donnée irrécupérable) puis UNE FOIS à la clôture ⇒
// exactement deux écritures par jour et par régulation. Tout le reste se
// RECALCULE : minutes = filtTotalMin − t0FiltMin, et `filtTotalMin` survit déjà
// en NVS (`pool-daily`, clé `filt_total`, lue ici en LECTURE SEULE).
void TimeToTargetManager::archive(uint8_t chan, const TttDay& entry,
                                  uint32_t t0FiltMin) {
  if (_memoryUnavailable) return;
  if (!tttHistoryAppend(_history, chan, entry)) return;
  _history.t0FiltMin[chan] = t0FiltMin;  // 0 à la clôture : plus rien en cours
  // C6 — estampille de série : la fenêtre du signal lent de décision indexe le
  // zéro du chronomètre pH ; toute modification future crée une discontinuité
  // de série, qui doit être lisible plutôt que silencieuse.
  const bool ok = saveTttHistory(_history, kPhDecisionWindowMs);
  publishHistory();  // le miroir suit l'anneau, écrit ou non
  if (ok) return;

  // C4 — fail-closed SILENCIEUX MAIS VISIBLE : un seul avertissement, aucune
  // boucle de retry, aucun repli d'écriture vers `pool-daily` (le namespace des
  // quotas de dosage ne s'ouvre pas en écriture pour un instrument). Pas de
  // `critical` ni d'alerte MQTT : ce serait crier au loup.
  _memoryUnavailable = true;
  for (uint8_t c = 0; c < kTttChannelCount; ++c) tttLiveReset(_live[c]);
  if (!_memoryWarned) {
    _memoryWarned = true;
    systemLogger.warning("[KPI] Mémoire NVS indisponible — indicateur suspendu");
  }
}

void TimeToTargetManager::restoreFromNvs(uint32_t today) {
  // Reprise après COUPURE SECTEUR (RAM RTC perdue). Même jour civil : t0 est
  // conservé, donc les minutes restent EXACTES ; la tenue, elle, repart de zéro
  // (fail-closed : l'eau s'est arrêtée, la purge se rejoue, une tenue ne
  // traverse pas une coupure). Jour différent ou heure invalide : état 0, et
  // AUCUNE écriture datée faux.
  if (today == 0) return;
  for (uint8_t c = 0; c < kTttChannelCount; ++c) {
    const TttDay* d = tttHistoryFind(_history, c, today);
    if (d == nullptr || d->state != kTttStateRunning) continue;
    TttLive& st = _live[c];
    tttLiveReset(st);
    st.date = today;
    st.t0FiltMin = _history.t0FiltMin[c];
    st.e0 = d->e0;
    st.state = kTttStateRunning;
    st.flags = (uint8_t)(d->flags | kTttFlagPowerCycle);
    // La consigne surveillée est REPHOTOGRAPHIÉE : une modification survenue
    // pendant la coupure n'est pas détectable, et inventer un drapeau serait
    // pire que de l'omettre.
    st.watchTarget = (c == kTttChannelPh) ? mqttCfg.phTarget : mqttCfg.orpTarget;
    st.minutes = tttElapsedMinutes(st, safetyLimits.filtTotalMin);
    logEvent(c, "reprise après coupure", st.minutes, st.e0, NAN, false);
  }
}

void TimeToTargetManager::begin() {
  loadTttHistory(_history);  // absent ou corrompu ⇒ anneau vide (fail-closed)

  // C6 — discontinuité de série : les journées archivées sous une autre fenêtre
  // de décision ne sont pas comparables aux suivantes.
  if (_history.serieStamp != 0 && _history.serieStamp != kPhDecisionWindowMs) {
    char msg[128];
    snprintf(msg, sizeof(msg),
             "[KPI] Discontinuité de série : fenêtre de décision %lu → %lu ms — "
             "les journées archivées ne sont pas comparables",
             (unsigned long)_history.serieStamp,
             (unsigned long)kPhDecisionWindowMs);
    systemLogger.warning(msg);
  }

  const uint32_t today = currentLoadDateNumber();
  if (tttRtcIntact(sTttRtc)) {
    // Reset logiciel : l'état vivant survit tel quel, tenue en cours comprise.
    for (uint8_t c = 0; c < kTttChannelCount; ++c) {
      _live[c] = sTttRtc.live[c];
      if (today == 0 || (_live[c].date != 0 && _live[c].date != today)) {
        tttLiveReset(_live[c]);  // jour révolu ou heure perdue ⇒ état 0
      }
    }
  } else {
    restoreFromNvs(today);
  }
  // Rattrapage des journées orphelines (revue, bloquant n°1). La DÉCISION vit
  // dans le module pur — c'est une règle d'état, pas de la plomberie, et elle
  // doit être couverte par mutation (point de contrôle n°7 de la revue).
  // Placé APRÈS les deux branches de reprise : la journée du jour est exclue
  // par la fonction elle-même, donc ni l'état vivant restauré ni l'entrée « en
  // cours » légitime ne sont touchés.
  tttHistoryCloseOrphans(_history, today);

  _lastDate = today;
  publishHistory();
  publishCache();
}

// -----------------------------------------------------------------------------
// Le tick
// -----------------------------------------------------------------------------

void TimeToTargetManager::tickChannel(uint8_t chan, const TttInputs& in) {
  const TttVerdict v = tttTick(_live[chan], in);
  switch (v.event) {
    case TttEvent::kStarted:
      logEvent(chan, "départ", 0, v.entry.e0, NAN, false);
      archive(chan, v.entry, _live[chan].t0FiltMin);
      break;
    case TttEvent::kReached:
      logEvent(chan, "cible atteinte", v.entry.minutes, v.entry.e0, v.residual,
               false);
      archive(chan, v.entry, 0);
      break;
    case TttEvent::kMissed:
      // « Interrompue » et « non atteinte » ne se disent pas de la même façon :
      // la première n'est pas un verdict sur la régulation.
      logEvent(chan,
               (v.entry.flags & kTttFlagOperatorOff)
                   ? "interrompue (régulation non automatique)"
                   : "cible NON atteinte",
               v.entry.minutes, v.entry.e0, v.residual, false);
      archive(chan, v.entry, 0);
      break;
    case TttEvent::kImplausible:
      // La garde refuse SEULEMENT de démarrer : t0 sera posé au premier
      // échantillon plausible. Un seul avertissement par voie et par journée
      // (réarmé au changement de jour civil, cf. tick()).
      if (!_implausibleWarned[chan]) {
        _implausibleWarned[chan] = true;
        logEvent(chan, "départ refusé (écart invraisemblable)", 0, v.rejected,
                 NAN, true);
      }
      break;
    default:
      break;
  }
}

void TimeToTargetManager::tick() {
  const uint32_t nowMs = millis();
  if (_lastTickMs != 0 && (uint32_t)(nowMs - _lastTickMs) < kTttTickMs) return;
  _lastTickMs = nowMs;

  // C4 : NVS indisponible ⇒ instrument suspendu, sans nouvelle tentative.
  if (_memoryUnavailable) {
    publishCache();
    return;
  }

  const uint32_t today = currentLoadDateNumber();
  if (today == 0 && !_clockWarned) {
    _clockWarned = true;
    systemLogger.warning("[KPI] Heure non synchronisée — indicateur suspendu "
                         "(aucune journée n'est datable)");
  }
  if (today != 0 && today != _lastDate) {
    _lastDate = today;
    for (uint8_t c = 0; c < kTttChannelCount; ++c) _implausibleWarned[c] = false;
  }

  // Fin de journée de filtration : un seul critère couvre la fin de plage
  // horaire et le passage en OFF — kTimeToTargetOffDayEndMin minutes MURALES
  // sans circulation. Soustraction non signée ⇒ wrap-safe.
  const bool running = filtration.isRunning();
  if (running) {
    _filtOff = false;
  } else if (!_filtOff) {
    _filtOff = true;
    _offSinceMs = nowMs;
  }
  const bool dayEnd =
      _filtOff && ((uint32_t)(nowMs - _offSinceMs) >= kTttOffDayEndMs);

  const MeasureValiditySnapshot mv = measureValidity.getSnapshot();
  // Porte de mesure (feature-091) : hors circulation ou porte fermée, la
  // grandeur de décision n'EXISTE pas — on transmet NAN, jamais une valeur non
  // représentative (C3 : absence, pas « dans la bande »).
  const bool usable = mv.measuresValid && running;
  const bool unsettled = (mv.lastPurgeEnd == (uint8_t)PurgeEnd::kCap);

  // Bit 4 — DÉBRANCHEMENT PAR L'OPÉRATEUR, commun aux deux voies : mode
  // maintenance (qui pose lui-même l'inhibition de dosage) et assistant de
  // mesure de débit. Le mode de régulation, lui, se lit par voie ci-dessous.
  // Ce que la régulation s'impose à elle-même (quota journalier, limite
  // horaire, actionneur sous-dimensionné) n'entre PAS ici : ces journées-là
  // doivent rester des « non atteintes » nues, c'est ce que la métrique mesure.
  const bool operatorOff = PumpController.isMaintenanceInhibited() ||
                           PumpController.isFlowAssistSuspended();

  TttInputs in;
  in.date = today;
  in.filtTotalMin = safetyLimits.filtTotalMin;  // LECTURE SEULE de `pool-daily`
  in.filtrationRunning = running;
  in.dayEnd = dayEnd;
  in.unsettled = unsettled;

  // --- pH : la grandeur de DÉCISION est le signal lent de la porte de session.
  // C5 — RETARD ASSUMÉ D'UNE SECONDE : ce tick est placé avant
  // PumpController.update() dans la loop (l'indicateur ne doit pas s'intercaler
  // entre filtration.update() et PumpController.update(), qui doivent voir le
  // même tour). Il lit donc la décision du TOUR PRÉCÉDENT. Sur une métrique qui
  // compte des minutes, une seconde de retard est sans effet.
  in.value = usable ? PumpController.getPhDecisionValue() : NAN;
  in.target = mqttCfg.phTarget;            // le pH n'a AUCUNE surcouche de consigne
  in.configuredTarget = mqttCfg.phTarget;  // juger et surveiller coïncident ici
  in.band = pumpProtection.phStartThreshold;  // seuil VIVANT, relu à chaque tick
  in.maxPlausibleE0 = kTttMaxPlausibleE0Ph;
  in.boostActive = false;  // le boost est une surcouche ORP : aucun boost pH
  in.automatic = !operatorOff && (mqttCfg.phRegulationMode != "manual");
  tickChannel(kTttChannelPh, in);

  // --- ORP : la filtrée EST la grandeur de décision de la régulation ORP.
  // On JUGE contre la consigne EFFECTIVE (elle monte au boost) mais on
  // SURVEILLE la consigne CONFIGURÉE : un boost n'est pas une modification
  // utilisateur, il a son propre drapeau (bit 1).
  in.value = usable ? sensors.getOrpFiltered() : NAN;
  in.target = PumpController.getEffectiveOrpTarget();
  in.configuredTarget = mqttCfg.orpTarget;
  in.band = pumpProtection.orpStartThreshold;
  in.maxPlausibleE0 = kTttMaxPlausibleE0Orp;
  in.boostActive = isBoostActive(time(nullptr));
  in.automatic = !operatorOff && (mqttCfg.orpRegulationMode != "manual");
  tickChannel(kTttChannelOrp, in);

  // Mémoire RTC : elle ne porte QUE le volatil (progression de tenue et état
  // courant) — tout le reste se recalcule depuis la NVS.
  for (uint8_t c = 0; c < kTttChannelCount; ++c) sTttRtc.live[c] = _live[c];
  tttRtcSeal(sTttRtc);

  publishCache();
}

// -----------------------------------------------------------------------------
// Instantanés servis au web (patron measure_validity : spinlock portMUX, jamais
// de mutex bloquant en contexte async)
// -----------------------------------------------------------------------------

void TimeToTargetManager::publishCache() {
  TttSnapshot snap;
  TttChannelSnapshot* dst[kTttChannelCount] = {&snap.ph, &snap.orp};
  for (uint8_t c = 0; c < kTttChannelCount; ++c) {
    const TttLive& st = _live[c];
    dst[c]->state = st.state;
    dst[c]->minutes = st.minutes;
    // Pas de départ ⇒ pas d'écart initial : NAN, jamais 0 (le WS le traduit en
    // null).
    dst[c]->e0 = (st.state == kTttStateIdle) ? NAN : st.e0;
    dst[c]->flags = st.flags;
  }
  snap.memoryUnavailable = _memoryUnavailable;
  portENTER_CRITICAL(&_mux);
  _cache = snap;
  portEXIT_CRITICAL(&_mux);
}

TttSnapshot TimeToTargetManager::getSnapshot() const {
  portENTER_CRITICAL(&_mux);
  const TttSnapshot snap = _cache;
  portEXIT_CRITICAL(&_mux);
  return snap;
}

void TimeToTargetManager::publishHistory() {
  portENTER_CRITICAL(&_mux);
  _historyPub = _history;
  portEXIT_CRITICAL(&_mux);
}

void TimeToTargetManager::copyHistory(TttHistory& out) const {
  portENTER_CRITICAL(&_mux);
  out = _historyPub;
  portEXIT_CRITICAL(&_mux);
}

bool TimeToTargetManager::isMemoryUnavailable() const {
  // REVUE (mineur) : cet accesseur est appelé depuis le contexte async_tcp
  // (handler /get-kpi). On sert donc la copie CACHÉE, publiée sous portMUX par
  // loopTask, et non le champ nu `_memoryUnavailable` — même contrat de lecture
  // que getSnapshot(), zéro course, zéro coût.
  portENTER_CRITICAL(&_mux);
  const bool unavailable = _cache.memoryUnavailable;
  portEXIT_CRITICAL(&_mux);
  return unavailable;
}
