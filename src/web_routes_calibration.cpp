#include "web_routes_calibration.h"
#include "web_helpers.h"
#include "config.h"
#include "constants.h"
#include "auth.h"
#include "sensors.h"
#include <ArduinoJson.h>

void setupCalibrationRoutes(AsyncWebServer* server) {
  // Routes de calibration pH (DFRobot SEN0161-V2) - PROTÉGÉES
  server->on("/calibrate_ph_neutral", HTTP_POST, [](AsyncWebServerRequest *req) {
    REQUIRE_AUTH(req, RouteProtection::WRITE);

    // Protéger l'accès I2C (évite collision avec sensors.update())
    if (xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(kI2cMutexTimeoutMs)) != pdTRUE) {
      sendErrorResponse(req, 503, "I2C busy");
      return;
    }

    sensors.calibratePhNeutral();
    xSemaphoreGive(i2cMutex);

    mqttCfg.phCalibrationDate = getCurrentTimeISO();
    mqttCfg.phCalibrationTemp = sensors.getTemperature();
    saveMqttConfig();

    StaticJsonDocument<128> doc;
    doc["success"] = true;
    doc["temperature"] = mqttCfg.phCalibrationTemp;
    sendJsonResponse(req, doc);
  });

  server->on("/calibrate_ph_acid", HTTP_POST, [](AsyncWebServerRequest *req) {
    REQUIRE_AUTH(req, RouteProtection::WRITE);

    // Protéger l'accès I2C (évite collision avec sensors.update())
    if (xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(kI2cMutexTimeoutMs)) != pdTRUE) {
      sendErrorResponse(req, 503, "I2C busy");
      return;
    }

    sensors.calibratePhAcid();
    xSemaphoreGive(i2cMutex);

    mqttCfg.phCalibrationDate = getCurrentTimeISO();
    mqttCfg.phCalibrationTemp = sensors.getTemperature();
    saveMqttConfig();

    StaticJsonDocument<128> doc;
    doc["success"] = true;
    doc["temperature"] = mqttCfg.phCalibrationTemp;
    sendJsonResponse(req, doc);
  });

  server->on("/clear_ph_calibration", HTTP_POST, [](AsyncWebServerRequest *req) {
    REQUIRE_AUTH(req, RouteProtection::WRITE);

    sensors.clearPhCalibration();
    mqttCfg.phCalibrationDate = "";
    mqttCfg.phCalibrationTemp = NAN;
    saveMqttConfig();

    JsonDocument doc;
    doc["success"] = true;
    sendJsonResponse(req, doc);
  });
}
