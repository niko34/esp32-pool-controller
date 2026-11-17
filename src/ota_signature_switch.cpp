#include "ota_signature_switch.h"

#include <Arduino.h>
#include <Preferences.h>
#include <atomic>
#include <esp_task_wdt.h>
#include <time.h>

#include "constants.h"
#include "logger.h"

namespace {

// -----------------------------------------------------------------------------
// MIROIR RAM : UN SEUL mot atomique — aucune lecture ne peut voir un état déchiré
// (trois atomiques séparés recréeraient l'incohérence bool + date écartée en NVS).
// Codage :
//   0            = FERMÉ : pas d'entrée (valeur initiale : FERMÉ PAR DÉFAUT, C6)
//   0xFFFFFFFF   = NVS illisible
//   0xFFFFFFFE   = VERROU : désarmement en cours ou RATÉ — fermé pour toute décision,
//                  mais une échéance peut subsister en NVS (publiée ON sur MQTT)
//   autre        = échéance (epoch UTC, s) lue/écrite en NVS
// UNE SEULE fonction produit un mot à partir d'une échéance : wordFromNvs() (Read, Init ET
// Arm). Elle porte l'invariant : toute échéance >= kWordLocked, ou égale à 0, est ramenée à
// FERMÉ — un repli NVS à 0 (désarmement par putUInt(0)) se lit donc « no_entry », JAMAIS
// « invalid_expiry » (voulu, testé).
// Écrit par `async_tcp` + setup, lu par mqttTask. Relaxé : un mot de 32 bits, rien
// n'est publié à travers lui.
// -----------------------------------------------------------------------------
constexpr uint32_t kWordClosed = 0u;
constexpr uint32_t kWordUnreadable = 0xFFFFFFFFu;
constexpr uint32_t kWordLocked = 0xFFFFFFFEu;
std::atomic<uint32_t> sWord{kWordClosed};

// Échéance qui peut subsister en NVS après un désarmement raté (async_tcp seulement).
// 0 = aucune (constaté) ; kOtaSwitchResidualUnknown = INCONNUE (begin() en échec) ; sinon l'échéance.
std::atomic<uint32_t> sResidualEpoch{0};

bool clockNow(uint32_t* nowOut) {
  const time_t now = time(nullptr);
  if (now < kMinValidEpoch) {
    *nowOut = 0;
    return false;
  }
  *nowOut = static_cast<uint32_t>(now);
  return true;
}

OtaSignatureSwitchDecision decisionFromWord(uint32_t word) {
  OtaSignatureSwitchInputs in{};
  in.nvsReadable = (word != kWordUnreadable);
  // Posé à `true` UNIQUEMENT à partir d'un mot qui porte une échéance (jamais en dur) :
  // les sentinelles fermées ne produisent jamais entryPresent.
  in.entryPresent = (word != kWordClosed && word != kWordUnreadable && word != kWordLocked);
  in.expiryEpoch = in.entryPresent ? word : 0;
  in.clockValid = clockNow(&in.nowEpoch);
  OtaSignatureSwitchDecision d = decideOtaSignatureSwitch(in, kOtaSignatureSwitchWindowSec);
  if (word == kWordLocked) d.reason = OtaSwitchReason::kLocked;  // requirement reste kRequired
  return d;
}

// Lecture NVS POSITIVE (C5) : la clé doit exister ET être de type U32. begin() en
// lecture/écriture (un namespace encore inexistant ferait échouer begin() en lecture
// seule, pris pour une NVS illisible). C'est LA SEULE fonction qui peut produire
// `present = true` à partir de la NVS.
void readNvs(bool* readable, bool* present, uint32_t* expiry) {
  *readable = false;
  *present = false;
  *expiry = 0;
  esp_task_wdt_reset();
  Preferences prefs;
  if (prefs.begin(kOtaNvsNamespace, false)) {
    *readable = true;
    if (prefs.isKey(kOtaSignatureSwitchNvsKey) &&
        prefs.getType(kOtaSignatureSwitchNvsKey) == PT_U32) {
      *expiry = prefs.getUInt(kOtaSignatureSwitchNvsKey, 0);
      *present = true;
    }
    prefs.end();
  }
  esp_task_wdt_reset();
}

uint32_t wordFromNvs(bool readable, bool present, uint32_t expiry) {
  if (!readable) return kWordUnreadable;
  if (!present) return kWordClosed;
  // Une échéance qui collisionnerait avec une sentinelle est de toute façon hors fenêtre.
  if (expiry == 0 || expiry >= kWordLocked) return kWordClosed;
  return expiry;
}

// Désarmement commun à Clear et Consume (C7). Retourne true si la NVS ne porte
// plus d'échéance exploitable.
bool disarm(const char* motif) {
  sWord.store(kWordLocked, std::memory_order_relaxed);  // VERROU posé AVANT toute E/S

  bool done = false;
  bool fallbackUsed = false;
  esp_task_wdt_reset();
  Preferences prefs;
  if (prefs.begin(kOtaNvsNamespace, false)) {
    if (!prefs.isKey(kOtaSignatureSwitchNvsKey)) {
      done = true;  // rien à effacer (remove() d'une clé absente rendrait false)
    } else if (prefs.remove(kOtaSignatureSwitchNvsKey)) {
      done = true;
    } else {
      // Repli : 0 = « pas d'échéance » pour la décision (C1).
      done = (prefs.putUInt(kOtaSignatureSwitchNvsKey, 0) == sizeof(uint32_t));
      fallbackUsed = done;
    }
    prefs.end();
  }
  esp_task_wdt_reset();

  char ligne[256];
  if (done) {
    sResidualEpoch.store(0, std::memory_order_relaxed);
    sWord.store(kWordClosed, std::memory_order_relaxed);
    if (fallbackUsed) {
      snprintf(ligne, sizeof(ligne),
               "Autorisation temporaire de mise à jour non signée (%s) : effacement NVS en échec, repli à 0 appliqué",
               motif);
      systemLogger.warning(ligne);
    }
  } else {
    // Le verrou RAM reste posé : fermé pour toute décision jusqu'au prochain armement
    // explicite ; l'échéance NVS, elle, SURVIT : un redémarrage ré-armerait (<= kOtaSignatureSwitchWindowSec).
    uint32_t w = 0;
    bool r = false, p = false;
    readNvs(&r, &p, &w);
    // R2 : begin() en échec (!r) = état de la NVS INCONNU, pas « rien ne reste ».
    // Clé lisible mais portant 0/sentinelle ⇒ wordFromNvs() la dit fermée ⇒ aucun résidu.
    const uint32_t resid = !r ? kOtaSwitchResidualUnknown
                              : (p ? (wordFromNvs(r, p, w) == kWordClosed ? 0u : w) : 0u);
    sResidualEpoch.store(resid, std::memory_order_relaxed);
    snprintf(ligne, sizeof(ligne),
             "Autorisation temporaire de mise à jour non signée (%s) : RETRAIT NVS ÉCHOUÉ — verrou RAM fermé, "
             "un redémarrage la RÉACTIVERAIT (<= 60 min) si une échéance subsiste en NVS",
             motif);
    systemLogger.critical(ligne);
  }
  return done;
}

}  // namespace

void otaSignatureSwitchInit() {
  bool readable, present;
  uint32_t expiry;
  readNvs(&readable, &present, &expiry);
  sWord.store(wordFromNvs(readable, present, expiry), std::memory_order_relaxed);
  if (!readable) {
    systemLogger.warning("Autorisation temporaire de mise à jour non signée illisible (NVS) — signature exigée");
  }
}

OtaSignatureSwitchDecision otaSignatureSwitchRead() {
  if (sWord.load(std::memory_order_relaxed) == kWordLocked) {
    return decisionFromWord(kWordLocked);  // C7 : le verrou est honoré, la NVS n'est PAS relue
  }
  bool readable, present;
  uint32_t expiry;
  readNvs(&readable, &present, &expiry);
  const uint32_t word = wordFromNvs(readable, present, expiry);
  // Rafraîchit le miroir (async_tcp, sérialisée avec Arm/Clear/Consume) pour que le
  // témoin MQTT reste fidèle à ce que la décision a réellement vu.
  sWord.store(word, std::memory_order_relaxed);
  return decisionFromWord(word);
}

OtaSignatureSwitchDecision otaSignatureSwitchPeek() {
  return decisionFromWord(sWord.load(std::memory_order_relaxed));
}

OtaSwitchArmResult otaSignatureSwitchArm(uint32_t* untilOut) {
  uint32_t now = 0;
  if (!clockNow(&now)) return OtaSwitchArmResult::kNoClock;
  const uint32_t until = now + kOtaSignatureSwitchWindowSec;

  bool ok = false;
  esp_task_wdt_reset();
  Preferences prefs;
  if (prefs.begin(kOtaNvsNamespace, false)) {
    ok = (prefs.putUInt(kOtaSignatureSwitchNvsKey, until) == sizeof(uint32_t));
    // Relecture : « armé » n'est dit que si la valeur est RÉELLEMENT en NVS.
    if (ok) {
      ok = prefs.isKey(kOtaSignatureSwitchNvsKey) &&
           prefs.getType(kOtaSignatureSwitchNvsKey) == PT_U32 &&
           prefs.getUInt(kOtaSignatureSwitchNvsKey, 0) == until;
    }
    prefs.end();
  }
  esp_task_wdt_reset();

  if (!ok) {
    systemLogger.error("Autorisation temporaire de mise à jour non signée : écriture NVS échouée — NON activée");
    return OtaSwitchArmResult::kWriteFailed;
  }
  sResidualEpoch.store(0, std::memory_order_relaxed);
  sWord.store(wordFromNvs(true, true, until), std::memory_order_relaxed);  // miroir : APRÈS la persistance, via le codage unique (R4)
  *untilOut = until;
  return OtaSwitchArmResult::kArmed;
}

bool otaSignatureSwitchClear() {
  return disarm("retrait");
}

void otaSignatureSwitchConsume(bool sessionWasWaived) {
  const uint32_t word = sWord.load(std::memory_order_relaxed);
  // C8 : la décision de DÉBUT de session est conservée ; on journalise seulement le fait
  // qu'elle ne tenait plus à la fin (expiration ou désarmement pendant le transfert).
  if (sessionWasWaived &&
      decisionFromWord(word).requirement != OtaSignatureRequirement::kWaived) {
    systemLogger.warning("Autorisation temporaire de mise à jour non signée : plus activée à la fin du transfert "
                         "(expirée ou retirée en cours) — décision de début de session conservée");
  }
  // Rien à consommer si aucune échéance n'existe (cas nominal : aucune E/S flash).
  if (word == kWordClosed || word == kWordUnreadable) return;
  if (disarm("consommé par un flash réussi")) {
    systemLogger.info("Autorisation temporaire de mise à jour non signée consommée (premier flash réussi)");
  }
}

uint32_t otaSignatureSwitchResidualEpoch() {
  return sResidualEpoch.load(std::memory_order_relaxed);
}

OtaSwitchRearm otaSignatureSwitchRearmsOnReboot() {
  const uint32_t r = sResidualEpoch.load(std::memory_order_relaxed);
  if (r == kOtaSwitchResidualUnknown) return OtaSwitchRearm::kUnknown;
  return r != 0 ? OtaSwitchRearm::kYes : OtaSwitchRearm::kNo;
}
