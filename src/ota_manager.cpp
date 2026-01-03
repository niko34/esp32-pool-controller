#include "ota_manager.h"
#include "logger.h"
#include "version.h"
#include "config.h"
#include <WiFi.h>

OTAManager otaManager;

OTAManager::OTAManager() {}

void OTAManager::begin() {
  if (!WiFi.isConnected()) {
    systemLogger.warning("OTA non disponible: WiFi non connecté");
    return;
  }

  ArduinoOTA.setHostname("poolcontroller");

  // Utiliser le mot de passe administrateur pour sécuriser l'OTA
  if (authCfg.adminPassword.length() > 0 && authCfg.adminPassword != "admin") {
    ArduinoOTA.setPassword(authCfg.adminPassword.c_str());
    systemLogger.info("OTA: Mot de passe administrateur configuré");
  } else {
    systemLogger.warning("OTA: Mot de passe par défaut - OTA non sécurisé");
  }

  ArduinoOTA.onStart([]() {
    String type;
    if (ArduinoOTA.getCommand() == U_FLASH) {
      type = "firmware";
    } else { // U_SPIFFS ou U_FS
      type = "filesystem";
    }
    systemLogger.info("Début mise à jour OTA: " + type);
  });

  ArduinoOTA.onEnd([]() {
    systemLogger.info("Mise à jour OTA terminée");
  });

  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    static unsigned long lastLog = 0;
    unsigned long now = millis();
    if (now - lastLog >= 2000) { // Log toutes les 2 secondes
      unsigned int percent = (progress / (total / 100));
      systemLogger.info("Progression OTA: " + String(percent) + "%");
      lastLog = now;
    }
  });

  ArduinoOTA.onError([](ota_error_t error) {
    String errorMsg = "Erreur OTA [" + String(error) + "]: ";
    if (error == OTA_AUTH_ERROR) errorMsg += "Échec authentification";
    else if (error == OTA_BEGIN_ERROR) errorMsg += "Échec démarrage";
    else if (error == OTA_CONNECT_ERROR) errorMsg += "Échec connexion";
    else if (error == OTA_RECEIVE_ERROR) errorMsg += "Échec réception";
    else if (error == OTA_END_ERROR) errorMsg += "Échec finalisation";
    systemLogger.error(errorMsg);
  });

  ArduinoOTA.begin();
  otaEnabled = true;
  systemLogger.info("OTA activé (version: " + String(FIRMWARE_VERSION) + ")");
}

void OTAManager::handle() {
  if (otaEnabled) {
    ArduinoOTA.handle();
  }
}

void OTAManager::setPassword(const String& password) {
  if (otaEnabled) {
    if (password.length() > 0) {
      ArduinoOTA.setPassword(password.c_str());
      systemLogger.info("OTA: Mot de passe mis à jour");
    } else {
      ArduinoOTA.setPassword(nullptr);
      systemLogger.warning("OTA: Mot de passe supprimé (non sécurisé)");
    }
  }
}
