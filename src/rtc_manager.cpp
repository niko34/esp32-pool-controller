#include "rtc_manager.h"
#include "config.h"
#include "constants.h"
#include "logger.h"
#include <Wire.h>
#include <time.h>
#include <sys/time.h>

// Instance globale
RtcManager rtcManager;

// Helper pour formater DateTime en ISO (YYYY-MM-DD HH:MM:SS)
static String formatDateTime(const DateTime& dt) {
  char buf[20];
  snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
           dt.year(), dt.month(), dt.day(),
           dt.hour(), dt.minute(), dt.second());
  return String(buf);
}

bool RtcManager::begin() {
  // Prendre le mutex I2C
  if (xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(kI2cMutexTimeoutMs)) != pdTRUE) {
    systemLogger.warning("RTC: Impossible d'acquérir le mutex I2C");
    return false;
  }

  rtcAvailable = rtc.begin();

  xSemaphoreGive(i2cMutex);

  if (!rtcAvailable) {
    systemLogger.warning("RTC DS3231 non détecté sur le bus I2C (adresse 0x68)");
    return false;
  }

  // Vérifier si le RTC a perdu l'alimentation (batterie vide ou première utilisation)
  if (rtc.lostPower()) {
    rtcLostPower = true;
    systemLogger.warning("RTC: L'horloge était arrêtée (batterie vide ou première utilisation)");
    // Démarrer l'horloge avec une date par défaut
    rtc.adjust(DateTime(2021, 1, 1, 0, 0, 0));
  } else {
    rtcLostPower = false;
  }

  DateTime now = rtc.now();
  systemLogger.info("RTC DS3231 initialisé - Heure: " + formatDateTime(now));

  return true;
}

DateTime RtcManager::now() {
  if (!rtcAvailable) {
    return DateTime();  // DateTime invalide
  }

  // Prendre le mutex I2C
  if (xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(kI2cMutexTimeoutMs)) != pdTRUE) {
    return DateTime();
  }

  DateTime dt = rtc.now();

  xSemaphoreGive(i2cMutex);

  return dt;
}

bool RtcManager::isTimeValid() {
  if (!rtcAvailable) return false;

  DateTime dt = now();
  return dt.year() >= MIN_VALID_YEAR;
}

bool RtcManager::syncFromSystem() {
  if (!rtcAvailable) {
    systemLogger.warning("RTC: Sync impossible - RTC non disponible");
    return false;
  }

  // Obtenir l'heure système
  time_t now_t;
  time(&now_t);

  if (now_t < 1609459200) {  // 2021-01-01
    systemLogger.warning("RTC: Sync impossible - Heure système invalide");
    return false;
  }

  return setTimeFromEpoch(now_t);
}

bool RtcManager::setTime(const DateTime& dt) {
  if (!rtcAvailable) return false;

  // Prendre le mutex I2C
  if (xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(kI2cMutexTimeoutMs)) != pdTRUE) {
    systemLogger.warning("RTC: Impossible d'acquérir le mutex I2C pour setTime");
    return false;
  }

  rtc.adjust(dt);
  rtcLostPower = false;  // L'heure est maintenant valide

  xSemaphoreGive(i2cMutex);

  systemLogger.info("RTC mis à jour: " + formatDateTime(dt));

  return true;
}

bool RtcManager::setTimeFromEpoch(time_t epoch) {
  if (epoch < 1609459200) {  // 2021-01-01
    return false;
  }

  DateTime dt(epoch);
  return setTime(dt);
}

bool RtcManager::applyToSystem() {
  if (!rtcAvailable || !isTimeValid()) {
    return false;
  }

  DateTime dt = now();
  time_t epoch = dt.unixtime();

  // Appliquer au système ESP32
  struct timeval tv;
  tv.tv_sec = epoch;
  tv.tv_usec = 0;
  settimeofday(&tv, nullptr);

  systemLogger.info("Heure système initialisée depuis RTC: " + formatDateTime(dt));

  return true;
}

time_t RtcManager::getEpoch() {
  if (!rtcAvailable || !isTimeValid()) {
    return 0;
  }

  DateTime dt = now();
  return dt.unixtime();
}
