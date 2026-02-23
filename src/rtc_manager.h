#ifndef RTC_MANAGER_H
#define RTC_MANAGER_H

#include <Arduino.h>
#include <RTClib.h>

/**
 * Gestionnaire du module RTC DS1307
 *
 * Hiérarchie des sources de temps :
 * 1. NTP (WiFi) - priorité haute, précis
 * 2. RTC DS1307 - priorité moyenne, conserve l'heure hors tension
 * 3. NVS + uptime - priorité basse, estimation
 *
 * Le RTC est mis à jour automatiquement quand NTP se synchronise
 * ou quand l'utilisateur règle l'heure manuellement.
 */
class RtcManager {
private:
  RTC_DS1307 rtc;
  bool rtcAvailable = false;
  bool rtcLostPower = false;  // true si batterie vide détectée

  static constexpr uint32_t MIN_VALID_YEAR = 2021;

public:
  /**
   * Initialise le RTC sur le bus I2C
   * Doit être appelé après Wire.begin()
   * @return true si le RTC est détecté et fonctionnel
   */
  bool begin();

  /**
   * Vérifie si le RTC est disponible et fonctionnel
   */
  bool isAvailable() const { return rtcAvailable; }

  /**
   * Vérifie si le RTC a perdu l'alimentation (batterie vide)
   * Dans ce cas, l'heure n'est pas fiable
   */
  bool hasLostPower() const { return rtcLostPower; }

  /**
   * Lit l'heure depuis le RTC
   * @return DateTime du RTC, ou DateTime invalide si non disponible
   */
  DateTime now();

  /**
   * Vérifie si l'heure du RTC est valide (année >= 2021)
   */
  bool isTimeValid();

  /**
   * Met à jour le RTC avec l'heure système actuelle (après sync NTP)
   * Utilise le mutex I2C pour éviter les conflits
   * @return true si mise à jour réussie
   */
  bool syncFromSystem();

  /**
   * Met à jour le RTC avec une heure spécifique
   * @param dt DateTime à enregistrer
   * @return true si mise à jour réussie
   */
  bool setTime(const DateTime& dt);

  /**
   * Met à jour le RTC depuis un timestamp Unix
   * @param epoch Timestamp Unix (secondes depuis 1970)
   * @return true si mise à jour réussie
   */
  bool setTimeFromEpoch(time_t epoch);

  /**
   * Applique l'heure du RTC au système ESP32
   * Utilisé au démarrage avant que NTP ne soit disponible
   * @return true si l'heure a été appliquée
   */
  bool applyToSystem();

  /**
   * Retourne l'heure RTC sous forme de timestamp Unix
   * @return epoch ou 0 si RTC non disponible/invalide
   */
  time_t getEpoch();
};

extern RtcManager rtcManager;

#endif // RTC_MANAGER_H
