#include "web_routes_ota_switch.h"

#include <stdio.h>

#include "auth.h"
#include "constants.h"
#include "logger.h"
#include "mqtt_manager.h"
#include "ota_signature_switch.h"

// =============================================================================
// feature-151 D3-b2/D3-c — administration de l'autorisation temporaire de mise à jour non signée
// =============================================================================
// ⛔ Ce fichier ne contient ni upload, ni accès au filesystem, ni lecture de
// paramètre de requête : il ne reçoit aucune image. Il ne fait qu'activer ou retirer
// l'autorisation que web_routes_ota.cpp lit pour lever le refus d'authenticité.
// ⛔ `REQUIRE_AUTH(req, RouteProtection::CRITICAL)` est la PREMIÈRE instruction de
// chaque lambda. ATTENTION : checkAuth() rend `true` quand l'authentification est
// coupée et ne distingue pas CRITICAL de WRITE — d'où le 403 explicite de l'armement.
// Contexte : `async_tcp`. Réponses construites sans JsonDocument (petits tampons).
// =============================================================================

namespace {

// 200 {"armed":..,"reason":"..","remaining_s":n} — état de la décision FAISANT AUTORITÉ.
void sendState(AsyncWebServerRequest* req, int code, const OtaSignatureSwitchDecision& d,
               const char* extra) {
  char buf[260];
  snprintf(buf, sizeof(buf), "{%s\"armed\":%s,\"reason\":\"%s\",\"remaining_s\":%lu}", extra,
           d.requirement == OtaSignatureRequirement::kWaived ? "true" : "false",
           otaSwitchReasonCode(d.reason), static_cast<unsigned long>(d.remainingSec));
  req->send(code, "application/json", buf);
}

// R2 : en état « locked », le verrou est en RAM et la NVS survit — le client doit pouvoir lire
// si un redémarrage RÉ-ARMERAIT. "unknown" (NVS inaccessible) ne se lit jamais « non ».
void appendRearmFields(char* extra, size_t cap) {
  const OtaSwitchRearm rr = otaSignatureSwitchRearmsOnReboot();
  const uint32_t res = otaSignatureSwitchResidualEpoch();
  const char* rearm = rr == OtaSwitchRearm::kYes ? "true" : (rr == OtaSwitchRearm::kNo ? "false" : "\"unknown\"");
  if (res == kOtaSwitchResidualUnknown) {
    snprintf(extra, cap, "\"rearms_on_reboot\":%s,\"nvs_residual_until\":null,", rearm);
  } else {
    snprintf(extra, cap, "\"rearms_on_reboot\":%s,\"nvs_residual_until\":%lu,", rearm,
             static_cast<unsigned long>(res));
  }
}

}  // namespace

void setupOtaSignatureSwitchRoutes(AsyncWebServer* server) {
  // C2 : AsyncURIMatcher::exact — le mode par défaut `server->on(const char*)` équivaut à
  // `^{uri}(/.*)?$` et ferait capter `/ota-signature-switch/arm` par `/ota-signature-switch`.
  server->on(AsyncURIMatcher::exact(kOtaSignatureSwitchArmRoutePath), HTTP_POST,
             [](AsyncWebServerRequest* req) {
    REQUIRE_AUTH(req, RouteProtection::CRITICAL);
    // C3 : auth coupée ou premier démarrage (mot de passe par défaut) ⇒ l'armement serait
    // ouvert à tout le LAN : refus explicite (le désarmement, lui, reste toujours permis).
    if (!authManager.isEnabled() || authManager.isFirstBootDetected()) {
      systemLogger.warning("Autorisation temporaire de mise à jour non signée : activation refusée (authentification inactive ou premier démarrage)");
      req->send(403, "application/json", "{\"error\":\"auth_required\"}");
      return;
    }
    // C10 : MQTT n'est PAS un témoin garanti — la réponse et le journal disent s'il était connecté.
    const bool mqttUp = mqttManager.isConnected();
    uint32_t until = 0;
    const OtaSwitchArmResult r = otaSignatureSwitchArm(&until);
    if (r == OtaSwitchArmResult::kNoClock) {
      req->send(409, "application/json", "{\"error\":\"no_clock\"}");
      return;
    }
    if (r == OtaSwitchArmResult::kWriteFailed) {
      req->send(500, "application/json", "{\"error\":\"nvs_write_failed\"}");  // NON armé
      return;
    }
    char ligne[256];
    snprintf(ligne, sizeof(ligne),
             "Autorisation temporaire de mise à jour non signée ACTIVÉE pour %lu min (échéance %lu) — MQTT %s",
             static_cast<unsigned long>(kOtaSignatureSwitchWindowSec / 60),
             static_cast<unsigned long>(until), mqttUp ? "connecté" : "NON connecté (témoin absent)");
    systemLogger.warning(ligne);
    char buf[130];
    snprintf(buf, sizeof(buf),
             "{\"armed\":true,\"until\":%lu,\"window_s\":%lu,\"mqtt_connected\":%s}",
             static_cast<unsigned long>(until), static_cast<unsigned long>(kOtaSignatureSwitchWindowSec),
             mqttUp ? "true" : "false");
    req->send(200, "application/json", buf);
  });

  server->on(AsyncURIMatcher::exact(kOtaSignatureSwitchRoutePath), HTTP_GET,
             [](AsyncWebServerRequest* req) {
    REQUIRE_AUTH(req, RouteProtection::CRITICAL);
    const OtaSignatureSwitchDecision d = otaSignatureSwitchRead();
    char extra[100];
    extra[0] = '\0';
    if (d.reason == OtaSwitchReason::kLocked) appendRearmFields(extra, sizeof(extra));
    sendState(req, 200, d, extra);
  });

  server->on(AsyncURIMatcher::exact(kOtaSignatureSwitchRoutePath), HTTP_POST,
             [](AsyncWebServerRequest* req) {
    REQUIRE_AUTH(req, RouteProtection::CRITICAL);
    const bool cleared = otaSignatureSwitchClear();
    const OtaSignatureSwitchDecision d = otaSignatureSwitchRead();  // état RÉEL après l'opération
    if (!cleared) {
      // C7 : le 500 rend l'état réel (verrou fermé) et signale le résidu NVS possible.
      char extra[140];
      int n = snprintf(extra, sizeof(extra), "\"error\":\"nvs_clear_failed\",");
      appendRearmFields(extra + n, sizeof(extra) - n);
      sendState(req, 500, d, extra);
      return;
    }
    systemLogger.info("Autorisation temporaire de mise à jour non signée retirée");
    sendState(req, 200, d, "");
  });
}
