// =============================================================================
// dosing_window — voir dosing_window.h
// =============================================================================

#include "dosing_window.h"

#include <esp_system.h>    // esp_reset_reason
#include <esp_rtc_time.h>  // esp_rtc_get_time_us (C7 : compteur monotone nommé)
#include <Preferences.h>
#include <time.h>

#include "config.h"     // configMutex, kConfigMutexTimeoutMs
#include "constants.h"
#include "logger.h"

DosingWindowManager dosingWindow;

namespace {

// T2 — RAM RTC (ADR-0017, patron measure_validity.cpp / heap_ring.cpp) : survit
// à un reset logiciel/panic/WDT, PAS à une coupure secteur (contenu aléatoire
// après power-on/brownout ⇒ magic/CRC faux ⇒ fail-closed). AUCUN initialiseur
// de membre (cf. dosing_window_logic.h — piège documenté sur TttLive).
RTC_NOINIT_ATTR DosingWindowRtcRecord sDosingWindowRtc;

// Heure système valide (posée par rtcManager.applyToSystem() ou NTP) — même
// contrat que measure_validity.cpp::wallClockValid().
bool wallClockValid(uint32_t& epochOut) {
  const time_t t = time(nullptr);
  if (t < kMinValidEpoch) return false;
  epochOut = (uint32_t)t;
  return true;
}

// Condition C7 : T2 n'est retenu que pour un motif de reset LOGICIEL — un
// power-on/brownout peut laisser la RAM RTC dans un état qui, par pure chance,
// satisferait quand même le CRC (RTC_NOINIT_ATTR n'est jamais réinitialisée,
// elle hérite du bruit électrique de la mise sous tension).
bool resetIsSoftware(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_SW:
    case ESP_RST_PANIC:
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
      return true;
    default:
      return false;
  }
}

// Sature une soustraction 64 bits (µs) en ms 32 bits (défensif : à cette
// échelle de temps — jours, pas années — la saturation n'est jamais atteinte
// en pratique, mais l'écrire évite tout comportement indéfini).
uint32_t satMs(uint64_t deltaUs) {
  const uint64_t ms = deltaUs / 1000ULL;
  return (ms > 0xFFFFFFFFULL) ? 0xFFFFFFFFUL : (uint32_t)ms;
}

}  // namespace

// =============================================================================
// begin() — lecture T2 + T1, résolution par voie, journalisation (C11)
// =============================================================================
void DosingWindowManager::begin() {
  // --- T2 : RAM RTC -----------------------------------------------------------
  const DosingWindowRtcRecord rtcSnap = sDosingWindowRtc;  // copie AVANT toute écriture
  const bool rtcUsable = resetIsSoftware(esp_reset_reason()) &&
                         dosingWindowRtcIntact(rtcSnap, sizeof(rtcSnap));
  // C7 : esp_rtc_get_time_us() (esp_hw_support/esp_rtc_time.h) lit le compteur
  // RTC_CNTL_TIME, horloge TOUJOURS alimentée tant que le domaine RTC l'est —
  // donc à travers un reset logiciel/panic/WDT, comme RTC_NOINIT_ATTR
  // elle-même ; PAS à travers une coupure secteur (power-on/brownout), ce qui
  // est cohérent avec `rtcUsable` ci-dessus. Un compteur revenu EN ARRIÈRE
  // (now < écrit) prouve qu'il n'a PAS traversé le reboot : écoulé déclaré
  // INCONNU (retombée sur C4), jamais une durée de boot supposée.
  const uint64_t rtcNowUs = esp_rtc_get_time_us();

  // --- T1 : NVS -----------------------------------------------------------------
  DosingWindowNvsRecord nvsSnap;
  bool nvsUsable = false;
  {
    Preferences prefsRead;
    if (prefsRead.begin("pool-dose", true)) {  // lecture seule
      const size_t read = prefsRead.getBytes("snap", &nvsSnap, sizeof(nvsSnap));
      prefsRead.end();
      nvsUsable = dosingWindowNvsIntact(nvsSnap, read);
    }
  }

  uint32_t epochNow = 0;
  const bool clockValid = wallClockValid(epochNow);
  const bool t1Available = nvsUsable && clockValid;

  const uint32_t nowMs = (uint32_t)millis();
  const char* names[2] = {"pH", "ORP"};
  const uint32_t fullMixing[2] = {(uint32_t)kPhMixingDelayMs, (uint32_t)kOrpMixingDelayMs};

  for (int ch = 0; ch < 2; ++ch) {
    // ----- Entrée T2 -----------------------------------------------------------
    DosingWindowSourceInputs t2;
    t2.available = rtcUsable;
    if (rtcUsable) {
      t2.usedMs = rtcSnap.usedMs[ch];
      t2.windowElapsedAtWriteMs = rtcSnap.windowElapsedMs[ch];
      t2.cyclesDayElapsedAtWriteMs = rtcSnap.cyclesDayElapsedMs[ch];
      t2.cyclesToday = rtcSnap.cyclesToday[ch];
      t2.mixingRemainingMs = rtcSnap.mixingRemainingMs[ch];
      t2.pumpOnAtWrite = (rtcSnap.pumpOnAtWrite[ch] != 0);
      const uint64_t writtenUs = ((uint64_t)rtcSnap.rtcCounterAtWriteUsHi << 32) |
                                 (uint64_t)rtcSnap.rtcCounterAtWriteUsLo;
      if (rtcNowUs < writtenUs) {
        t2.unknownElapsed = true;
      } else {
        t2.outageElapsedMs = satMs(rtcNowUs - writtenUs);
      }
    }

    // ----- Entrée T1 -------------------------------------------------------------
    DosingWindowSourceInputs t1;
    t1.available = t1Available;
    if (t1Available) {
      uint32_t usedMs1 = nvsSnap.usedMs[ch];
      // Forfait conservateur (annexe chiffrée du plan) : cadence NVS 30 s —
      // si la pompe tournait À L'ÉCRITURE, jusqu'à kDosingWindowPersistIntervalMs
      // d'injection supplémentaire a pu avoir lieu avant la coupure effective.
      // Sur-comptage assumé, jamais de sous-comptage.
      if (nvsSnap.pumpOnAtWrite[ch] != 0) {
        const uint32_t bumped = usedMs1 + kDosingWindowPersistIntervalMs;
        usedMs1 = (bumped < usedMs1 || bumped > kDosingWindowHourMs) ? kDosingWindowHourMs : bumped;
      }
      t1.usedMs = usedMs1;
      t1.windowElapsedAtWriteMs = nvsSnap.windowElapsedAtWriteMs[ch];
      t1.cyclesDayElapsedAtWriteMs = nvsSnap.cyclesDayElapsedAtWriteMs[ch];
      t1.cyclesToday = nvsSnap.cyclesToday[ch];
      t1.mixingRemainingMs = nvsSnap.mixingRemainingMs[ch];
      t1.pumpOnAtWrite = (nvsSnap.pumpOnAtWrite[ch] != 0);
      if (epochNow < nvsSnap.epochAtWrite) {
        t1.unknownElapsed = true;  // horloge reculée entre l'écriture et maintenant : incalculable (C4)
      } else {
        t1.outageElapsedMs = satMs((uint64_t)(epochNow - nvsSnap.epochAtWrite) * 1000ULL);
      }
    }

    const DosingWindowChannelVerdict v =
        dosingWindowResolveChannel(t2, t1, fullMixing[ch], nowMs);

    DosingWindowRestoredChannel& out = _restored[ch];
    out.usedMs = v.usedMs;
    out.windowStart = v.windowStart;
    out.cyclesToday = v.cyclesToday;
    out.cyclesDayStart = v.cyclesDayStart;
    out.mixingEndMs = v.mixingEndMs;
    out.source = v.source;

    // Ring anti-rafale (C6) : T2 UNIQUEMENT (recommandation C9). Rebasé entrée
    // par entrée dans le domaine millis() courant.
    out.ringIdx = rtcUsable ? (rtcSnap.ringIdx[ch] % kDosingCycleHistorySize) : 0;
    for (size_t i = 0; i < kDosingCycleHistorySize; ++i) {
      out.ring[i] = rtcUsable
          ? dosingWindowRestoreRingEntry(rtcSnap.ringAgeMs[ch][i], t2.outageElapsedMs,
                                         t2.unknownElapsed, nowMs)
          : 0u;
    }

    // C11 — journal : CHAQUE grandeur restaurée et SA SOURCE.
    //
    // S2 (gate pool-chemistry P2, 2026-10-08) : l'ÉCOULÉ RETENU et son statut
    // figurent ici. Sans eux, un repli conservateur C4 (« écoulé inconnu ») est
    // INDISCERNABLE d'une reprise nominale dans le journal — et la dette C7 (le
    // compteur RTC traverse-t-il réellement esp_restart() ?) ne pourrait alors
    // JAMAIS être levée par l'observation terrain. Deux mots qui transforment
    // une surveillance en preuve.
    // L'écoulé rapporté est celui de la SOURCE RETENUE. En mode « plus
    // défavorable » les deux ont contribué : on rapporte le plus grand, qui est
    // celui qui a pu périmer une fenêtre.
    char elapsedTxt[48];
    bool elapsedKnown = false;
    uint32_t elapsedMs = 0;
    switch (v.source) {
      case DosingWindowSource::kRtc:
        elapsedKnown = t2.available && !t2.unknownElapsed;
        elapsedMs = t2.outageElapsedMs;
        break;
      case DosingWindowSource::kNvs:
        elapsedKnown = t1.available && !t1.unknownElapsed;
        elapsedMs = t1.outageElapsedMs;
        break;
      case DosingWindowSource::kWorst: {
        const bool k2 = t2.available && !t2.unknownElapsed;
        const bool k1 = t1.available && !t1.unknownElapsed;
        elapsedKnown = k2 || k1;
        const uint32_t e2 = k2 ? t2.outageElapsedMs : 0u;
        const uint32_t e1 = k1 ? t1.outageElapsedMs : 0u;
        elapsedMs = (e2 > e1) ? e2 : e1;
        break;
      }
      default:  // kFailClosed : aucune source exploitable, donc aucun écoulé
        elapsedKnown = false;
        break;
    }
    if (elapsedKnown) {
      snprintf(elapsedTxt, sizeof(elapsedTxt), "coupure=%lus",
               (unsigned long)(elapsedMs / 1000UL));
    } else {
      snprintf(elapsedTxt, sizeof(elapsedTxt), "écoulé=inconnu");
    }
    char line[288];
    snprintf(line, sizeof(line),
             "[Dosage] Budget horaire %s restauré (source : %s, %s) : usedMs=%lu/%lu "
             "cyclesToday=%u mélange=%lus",
             names[ch], dosingWindowSourceLabel(v.source), elapsedTxt,
             (unsigned long)out.usedMs, (unsigned long)kDosingWindowHourMs,
             (unsigned)out.cyclesToday,
             (unsigned long)((out.mixingEndMs > nowMs) ? (out.mixingEndMs - nowMs) / 1000UL : 0UL));
    if (v.source == DosingWindowSource::kFailClosed) {
      systemLogger.warning(line);
    } else {
      systemLogger.info(line);
    }
  }
}

// =============================================================================
// refreshRtc() — hook 1/3 : T2, bon marché, cadencé ~1 s
// =============================================================================
void DosingWindowManager::refreshRtc(uint32_t nowMs, const DosingWindowSnapshot& snap) {
  if ((nowMs - _lastRtcRefreshMs) < kDosingWindowRtcRefreshMs) return;
  _lastRtcRefreshMs = nowMs;

  DosingWindowRtcRecord r;
  r.magic = kDosingWindowRtcMagic;
  r.version = kDosingWindowRtcSchema;
  const uint64_t nowUs = esp_rtc_get_time_us();
  r.rtcCounterAtWriteUsLo = (uint32_t)(nowUs & 0xFFFFFFFFULL);
  r.rtcCounterAtWriteUsHi = (uint32_t)(nowUs >> 32);

  for (int ch = 0; ch < 2; ++ch) {
    const DosingWindowChannelSnapshot& s = snap.ch[ch];
    r.usedMs[ch] = s.usedMs;
    r.windowElapsedMs[ch] = (s.windowStart != 0) ? (nowMs - s.windowStart) : 0u;
    r.cyclesDayElapsedMs[ch] = (s.cyclesDayStart != 0) ? (nowMs - s.cyclesDayStart) : 0u;
    r.cyclesToday[ch] = (uint8_t)((s.cyclesToday > 255) ? 255 : s.cyclesToday);
    const bool mixingActive = (s.mixingEndMs != 0) && ((int32_t)(s.mixingEndMs - nowMs) > 0);
    r.mixingRemainingMs[ch] = mixingActive ? (s.mixingEndMs - nowMs) : 0u;
    r.pumpOnAtWrite[ch] = s.pumpOnNow ? 1 : 0;
    r.ringIdx[ch] = (uint8_t)(s.ringIdx % kDosingCycleHistorySize);
    for (size_t i = 0; i < kDosingCycleHistorySize; ++i) {
      const uint32_t ts = s.ring ? s.ring[i] : 0u;
      r.ringAgeMs[ch][i] = (ts != 0u) ? (nowMs - ts) : kDosingWindowRingEmptySentinel;
    }
  }
  dosingWindowRtcSeal(r);
  sDosingWindowRtc = r;
}

// =============================================================================
// persistNvsIfDue() — hooks 2/3 + 3/3 : T1, cadence 30 s pendant pompe-ON +
// événement
// =============================================================================
bool DosingWindowManager::persistNvsIfDue(uint32_t nowMs, const DosingWindowSnapshot& snap,
                                          bool anyPumpOnNow, bool event) {
  const bool due = event || (anyPumpOnNow && (nowMs - _lastNvsSaveMs) >= kDosingWindowPersistIntervalMs);
  if (!due) return true;  // rien à faire ce tour-ci : pas un échec, l'appelant peut effacer l'événement

  uint32_t epochNow = 0;
  if (!wallClockValid(epochNow)) return false;  // sans heure, rien d'exploitable à écrire (T2 couvre) — à retenter

  DosingWindowNvsRecord r;
  r.magic = kDosingWindowNvsMagic;
  r.version = kDosingWindowNvsSchema;
  r.epochAtWrite = epochNow;
  for (int ch = 0; ch < 2; ++ch) {
    const DosingWindowChannelSnapshot& s = snap.ch[ch];
    r.usedMs[ch] = s.usedMs;
    r.windowElapsedAtWriteMs[ch] = (s.windowStart != 0) ? (nowMs - s.windowStart) : 0u;
    r.cyclesDayElapsedAtWriteMs[ch] = (s.cyclesDayStart != 0) ? (nowMs - s.cyclesDayStart) : 0u;
    r.cyclesToday[ch] = (uint8_t)((s.cyclesToday > 255) ? 255 : s.cyclesToday);
    const bool mixingActive = (s.mixingEndMs != 0) && ((int32_t)(s.mixingEndMs - nowMs) > 0);
    r.mixingRemainingMs[ch] = mixingActive ? (s.mixingEndMs - nowMs) : 0u;
    r.pumpOnAtWrite[ch] = s.pumpOnNow ? 1 : 0;
  }
  dosingWindowNvsSeal(r);

  // configMutex pris ICI, jamais à l'extérieur (brief pool-chemistry) :
  // appelable UNIQUEMENT depuis loopTask (update()), jamais un callback
  // AsyncWebServer ni MQTT, jamais en tenant un autre verrou.
  if (configMutex && xSemaphoreTakeRecursive(configMutex, pdMS_TO_TICKS(kConfigMutexTimeoutMs)) != pdTRUE) {
    systemLogger.warning("[Dosage] persistNvsIfDue: timeout mutex config");
    return false;  // à retenter : l'appelant ne doit pas effacer l'événement
  }
  bool written = false;
  Preferences prefs;
  if (prefs.begin("pool-dose", false)) {
    prefs.putBytes("snap", &r, sizeof(r));
    prefs.end();
    written = true;
  } else {
    systemLogger.warning("[Dosage] Budget horaire : ouverture NVS refusée");
  }
  if (configMutex) xSemaphoreGiveRecursive(configMutex);

  if (written) _lastNvsSaveMs = nowMs;
  return written;
}
