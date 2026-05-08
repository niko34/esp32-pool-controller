#ifndef ATLAS_EZO_H
#define ATLAS_EZO_H

#include <Arduino.h>
#include <Wire.h>
#include "constants.h"

// =============================================================================
// AtlasEzoSensor — Mini-classe pilote pour modules Atlas Scientific EZO Embedded
// =============================================================================
//
// Encapsule la communication I²C avec un module EZO (pH ou ORP) :
//  - commandes ASCII (R, RT,<t>, Cal,*, Cal,?, I)
//  - timing requis par le firmware EZO (600/900 ms entre cmd et lecture)
//  - parsing du code de statut Atlas (1=OK, 2=err, 254=pas prêt, 255=no data)
//
// Concurrence : le bus I²C est partagé avec le DS3231 et autres périphériques.
// Le mutex global `i2cMutex` (déclaré dans config.h) doit être pris pour TOUTE
// séquence cohérente. Les méthodes publiques de haut niveau (readSingle,
// calibrate, clearCalibration, queryCalPoints, readInfo) prennent le mutex en
// interne et le tiennent pendant TOUTE la séquence cmd+delay+read pour garantir
// l'atomicité (cf. pool-chemistry condition #6).
//
// Les méthodes "bas niveau" sendCmd / readResponse sont publiques pour permettre
// des séquences personnalisées, mais l'appelant doit alors tenir le mutex
// lui-même.
//
// Voir spec : specs/features/doing/feature-021-migration-atlas-ezo.md
// =============================================================================

class AtlasEzoSensor {
public:
  // Construit le pilote. `name` est utilisé uniquement pour les logs (ex. "EZO pH").
  AtlasEzoSensor(uint8_t i2cAddress, const char* name);

  // Envoie une commande ASCII au module (ex. "R", "RT,25.5", "Cal,mid,7.00").
  // ATTENTION : le mutex I²C doit déjà être pris par l'appelant.
  // Retourne true si la transaction Wire a abouti (Wire.endTransmission == 0).
  bool sendCmd(const char* cmd);

  // Lit la réponse du module après attente du délai approprié.
  // ATTENTION : le mutex I²C doit déjà être pris par l'appelant.
  // `bufLen` doit être >= 32 octets. La fonction termine `buf` par un '\0'.
  // Retourne le nombre d'octets utiles copiés dans `buf` (sans le code statut),
  // 0 si pas de données utiles, ou -1 si erreur (statut != 1).
  int readResponse(char* buf, size_t bufLen, uint32_t delayMs);

  // Lecture d'une valeur scalaire (pH ou ORP) avec compensation T° préalable.
  // Séquence interne : RT,<tempC> + delay + R + delay + parse float.
  // Prend le mutex I²C en interne pour toute la séquence.
  // Retourne true si lecture valide (out contient la valeur). Sinon out est inchangé.
  bool readSingle(float& out, float tempC);

  // Lance une commande de calibration ("Cal,<arg>").
  // Exemples d'arguments : "mid,7.00", "low,4.00", "high,10.00", "470".
  // Prend le mutex I²C en interne. Retourne true si statut EZO = 1.
  bool calibrate(const char* arg);

  // Efface toute la calibration mémorisée dans l'EZO ("Cal,clear").
  // Prend le mutex I²C en interne. Retourne true si succès.
  bool clearCalibration();

  // Interroge le nombre de points de calibration mémorisés ("Cal,?").
  // Réponse Atlas : "?CAL,N" avec N entre 0 et 3.
  // Prend le mutex I²C en interne.
  // Retourne -1 si EZO injoignable / parsing échoué, 0..3 sinon.
  int queryCalPoints();

  // Lit la version firmware du module ("I" command).
  // Réponse type : "?I,pH,2.10" ou "?I,ORP,2.10".
  // Prend le mutex I²C en interne. Place la chaîne brute dans `fw` et retourne true si succès.
  bool readInfo(String& fw);

  // Accesseurs simples
  uint8_t address() const { return _address; }
  const char* name() const { return _name; }

private:
  uint8_t _address;
  const char* _name;

  // Helpers internes (mutex I²C doit être pris par l'appelant).
  bool _sendCmdLocked(const char* cmd);
  int  _readResponseLocked(char* buf, size_t bufLen, uint32_t delayMs);
};

#endif  // ATLAS_EZO_H
