#include "atlas_ezo.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"   // i2cMutex
#include "logger.h"   // systemLogger

// =============================================================================
// Constantes locales
// =============================================================================

// Codes de statut renvoyés par les modules EZO (premier octet de la réponse).
// Documentés dans le datasheet Atlas Scientific (EZO pH / EZO ORP).
static constexpr uint8_t kEzoStatusSuccess  = 1;    // Commande exécutée avec succès
static constexpr uint8_t kEzoStatusFailed   = 2;    // Échec syntaxe / commande invalide
static constexpr uint8_t kEzoStatusPending  = 254;  // Pas encore prêt (relire plus tard)
static constexpr uint8_t kEzoStatusNoData   = 255;  // Pas de donnée à retourner

// Taille de buffer de lecture par défaut (les EZO ne renvoient jamais plus de
// ~30 octets en mode commande).
static constexpr size_t kEzoReadBufLen = 32;

// =============================================================================
// Construction
// =============================================================================

AtlasEzoSensor::AtlasEzoSensor(uint8_t i2cAddress, const char* name)
    : _address(i2cAddress), _name(name ? name : "EZO") {}

// =============================================================================
// Helpers bas niveau (mutex I²C doit être pris par l'appelant)
// =============================================================================

bool AtlasEzoSensor::_sendCmdLocked(const char* cmd) {
  if (cmd == nullptr) return false;

  Wire.beginTransmission(_address);
  Wire.write(reinterpret_cast<const uint8_t*>(cmd), strlen(cmd));
  uint8_t err = Wire.endTransmission();
  if (err != 0) {
    // err != 0 : NACK adresse, NACK data, timeout, autre erreur bus.
    systemLogger.warning(String(_name) + " : Wire.endTransmission err=" + String(err) +
                         " (cmd=" + String(cmd) + ")");
    return false;
  }
  return true;
}

int AtlasEzoSensor::_readResponseLocked(char* buf, size_t bufLen, uint32_t delayMs) {
  if (buf == nullptr || bufLen < kEzoReadBufLen) return -1;

  // Le firmware EZO requiert un délai minimal entre la commande et la lecture
  // (600 ms pour RT, 900 ms pour R/Cal/I). Pendant ce délai, FreeRTOS reste
  // libre — vTaskDelay équivalent à delay() ici, le watchdog (30 s) n'est
  // jamais menacé.
  delay(delayMs);

  // Lecture I²C : on demande bufLen-1 octets pour pouvoir terminer par '\0'.
  size_t requested = bufLen - 1;
  size_t received = Wire.requestFrom(static_cast<int>(_address), static_cast<int>(requested));
  if (received == 0) {
    systemLogger.warning(String(_name) + " : aucune réponse I²C");
    return -1;
  }

  // Premier octet = code de statut (1, 2, 254 ou 255).
  uint8_t status = Wire.available() ? static_cast<uint8_t>(Wire.read()) : 0;

  // Lire le reste du payload ASCII jusqu'au '\0' ou fin de trame.
  size_t idx = 0;
  while (Wire.available() && idx < (bufLen - 1)) {
    char c = static_cast<char>(Wire.read());
    if (c == '\0') break;        // Atlas termine ses chaînes par '\0'
    buf[idx++] = c;
  }
  // Vidanger d'éventuels octets restants (ex. firmware bavard).
  while (Wire.available()) {
    (void)Wire.read();
  }
  buf[idx] = '\0';

  switch (status) {
    case kEzoStatusSuccess:
      return static_cast<int>(idx);
    case kEzoStatusPending:
      // Pas encore prêt : peut arriver si délai trop court. Non bloquant.
      systemLogger.warning(String(_name) + " : statut 254 (pas prêt)");
      return -1;
    case kEzoStatusNoData:
      // Pas de données — typique pour certaines commandes sans réponse utile.
      return 0;
    case kEzoStatusFailed:
      systemLogger.warning(String(_name) + " : statut 2 (commande échouée)");
      return -1;
    default:
      // Boot du module ou réponse parasite : on ignore silencieusement,
      // l'appel suivant aura généralement un statut valide.
      return -1;
  }
}

// =============================================================================
// Méthodes publiques bas niveau (l'appelant tient le mutex)
// =============================================================================

bool AtlasEzoSensor::sendCmd(const char* cmd) {
  return _sendCmdLocked(cmd);
}

int AtlasEzoSensor::readResponse(char* buf, size_t bufLen, uint32_t delayMs) {
  return _readResponseLocked(buf, bufLen, delayMs);
}

// =============================================================================
// Méthodes publiques de haut niveau (mutex pris en interne, séquence atomique)
// =============================================================================

bool AtlasEzoSensor::readSingle(float& out, float tempC) {
  if (xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(kI2cMutexTimeoutMs)) != pdTRUE) {
    systemLogger.warning(String(_name) + " : timeout mutex I²C (readSingle)");
    return false;
  }

  bool ok = false;
  char buf[kEzoReadBufLen];

  // Commande "RT,<tempC>" : sur l'EZO pH, applique la compensation T° (Nernst)
  // ET retourne la valeur pH compensée en une seule commande (statut 1 + payload).
  // L'EZO ORP ignore la T° mais accepte la commande et retourne la valeur ORP
  // courante en mV.
  //
  // Hotfix oscillation 2026-05-07 (PCB v2) : la séquence précédente envoyait
  // RT,<t> puis R, ce qui causait une oscillation cyclique pH "1 sur 2"
  // (~0.1 pH crête-à-crête, période 10 s). Cause : la lecture R après RT
  // retournait parfois une valeur transitoire (sonde pas stabilisée après
  // changement de T° interne) et le délai kEzoRtDelayMs = 600 ms était sous le
  // seuil 900 ms recommandé par le datasheet Atlas → statut 254 intermittent.
  // RT seul = une lecture stable, pas de double appel I²C.
  char rtCmd[16];
  snprintf(rtCmd, sizeof(rtCmd), "RT,%.1f", tempC);
  if (_sendCmdLocked(rtCmd)) {
    // 900 ms : délai standard pour R/RT sur EZO (datasheet Atlas).
    int n = _readResponseLocked(buf, sizeof(buf), kEzoReadDelayMs);
    if (n > 0) {
      // La réponse est une chaîne ASCII type "7.234" (pH) ou "650.0" (ORP).
      char* endptr = nullptr;
      float v = strtof(buf, &endptr);
      if (endptr != buf) {
        out = v;
        ok = true;
      } else {
        systemLogger.warning(String(_name) + " : parse float échoué (\"" + String(buf) + "\")");
      }
    }
  }

  xSemaphoreGive(i2cMutex);
  return ok;
}

bool AtlasEzoSensor::calibrate(const char* arg) {
  if (arg == nullptr) return false;

  if (xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(kI2cMutexTimeoutMs)) != pdTRUE) {
    systemLogger.warning(String(_name) + " : timeout mutex I²C (calibrate)");
    return false;
  }

  bool ok = false;
  char cmd[32];
  snprintf(cmd, sizeof(cmd), "Cal,%s", arg);

  if (_sendCmdLocked(cmd)) {
    char buf[kEzoReadBufLen];
    int n = _readResponseLocked(buf, sizeof(buf), kEzoCalDelayMs);
    // n >= 0 signifie que le module a répondu avec un statut valide
    // (succès ou no-data sans erreur). Atlas renvoie statut=1 sans payload
    // pour Cal,* — donc n peut valoir 0 légitimement.
    ok = (n >= 0);
    if (ok) {
      systemLogger.info(String(_name) + " : calibration OK (" + String(arg) + ")");
    } else {
      systemLogger.error(String(_name) + " : calibration échouée (" + String(arg) + ")");
    }
  }

  xSemaphoreGive(i2cMutex);
  return ok;
}

bool AtlasEzoSensor::clearCalibration() {
  return calibrate("clear");
}

int AtlasEzoSensor::queryCalPoints() {
  if (xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(kI2cMutexTimeoutMs)) != pdTRUE) {
    systemLogger.warning(String(_name) + " : timeout mutex I²C (queryCalPoints)");
    return -1;
  }

  int points = -1;
  if (_sendCmdLocked("Cal,?")) {
    char buf[kEzoReadBufLen];
    int n = _readResponseLocked(buf, sizeof(buf), kEzoCalDelayMs);
    if (n > 0) {
      // Réponse Atlas : "?CAL,N" — on cherche la dernière virgule et on lit le nombre.
      const char* comma = strrchr(buf, ',');
      if (comma != nullptr && *(comma + 1) != '\0') {
        char digit = *(comma + 1);
        if (digit >= '0' && digit <= '3') {
          points = digit - '0';
        }
      }
      if (points < 0) {
        systemLogger.warning(String(_name) + " : Cal,? réponse inattendue (\"" + String(buf) + "\")");
      }
    }
  }

  xSemaphoreGive(i2cMutex);
  return points;
}

bool AtlasEzoSensor::querySlope(PhSlopeInfo& out) {
  if (xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(kI2cMutexTimeoutMs)) != pdTRUE) {
    systemLogger.warning(String(_name) + " : timeout mutex I²C (querySlope)");
    return false;
  }

  bool ok = false;
  if (_sendCmdLocked("Slope,?")) {
    char buf[kEzoReadBufLen];
    int n = _readResponseLocked(buf, sizeof(buf), kEzoCalDelayMs);
    if (n > 0) {
      // Réponse Atlas EZO pH attendue : "?Slope,<acid>,<base>[,<zero>]".
      // Trace brute en debug uniquement (toggle DEBUG via feature-017).
      // Niveau warning évité : query auto toutes les 24h → spam HA si warning.
      systemLogger.debug(String(_name) + " : Slope,? réponse brute = \"" +
                         String(buf) + "\"");

      // Recherche de la 1ʳᵉ virgule (après "?Slope") puis parsing séquentiel.
      const char* p = strchr(buf, ',');
      if (p != nullptr) {
        ++p;  // après la virgule
        char* endptr = nullptr;
        float acid = strtof(p, &endptr);
        if (endptr != p && *endptr == ',') {
          const char* p2 = endptr + 1;
          float base = strtof(p2, &endptr);
          if (endptr != p2) {
            // 2 floats valides au minimum (acide + base).
            out.acidPct = acid;
            out.basePct = base;
            // 3ᵉ float optionnel : décalage zéro en mV si présent.
            if (*endptr == ',') {
              const char* p3 = endptr + 1;
              float zero = strtof(p3, &endptr);
              if (endptr != p3) {
                out.zeroOffsetMv = zero;
              } else {
                out.zeroOffsetMv = NAN;
              }
            } else {
              // Firmware EZO ancien : pas de 3ᵉ valeur.
              out.zeroOffsetMv = NAN;
            }
            ok = true;
          }
        }
      }
      if (!ok) {
        systemLogger.warning(String(_name) + " : Slope,? parsing échoué (\"" +
                             String(buf) + "\")");
      }
    }
  }

  xSemaphoreGive(i2cMutex);
  return ok;
}

bool AtlasEzoSensor::readInfo(String& fw) {
  if (xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(kI2cMutexTimeoutMs)) != pdTRUE) {
    systemLogger.warning(String(_name) + " : timeout mutex I²C (readInfo)");
    return false;
  }

  bool ok = false;
  if (_sendCmdLocked("I")) {
    char buf[kEzoReadBufLen];
    int n = _readResponseLocked(buf, sizeof(buf), kEzoCalDelayMs);
    if (n > 0) {
      fw = String(buf);
      ok = true;
    }
  }

  xSemaphoreGive(i2cMutex);
  return ok;
}
