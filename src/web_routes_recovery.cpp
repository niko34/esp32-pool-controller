#include "web_routes_recovery.h"

#include <LittleFS.h>

#include "ota_recovery_marker.h"
#include "ota_recovery_page.h"

// Envoie la page de secours. Contexte : tâche `async_tcp` — rien d'allouant hors
// la réponse elle-même, aucun log.
//
// 🔴 SURCHARGE `(const uint8_t*, size_t)` OBLIGATOIRE ⇒ AsyncProgmemResponse,
// ZÉRO copie en tas : la page (~15 Ko) est lue directement en flash. ⛔ JAMAIS la
// surcharge `const char*` : elle construit une AsyncBasicResponse qui COPIE tout
// le contenu dans une `String` — 15 Ko contigus à demander à un tas dont le plus
// gros bloc 8 bits nominal est d'environ 21 Ko (feature-151 D3-b1, ADR-0124).
static void sendRecoveryPage(AsyncWebServerRequest* req) {
  AsyncWebServerResponse* res =
      req->beginResponse(200, "text/html", reinterpret_cast<const uint8_t*>(kOtaRecoveryPageHtml),
                         kOtaRecoveryPageHtmlLen);
  // La page est un état TRANSITOIRE servi à l'adresse de l'interface : qu'aucun
  // cache ne la garde une fois l'interface restaurée.
  res->addHeader("Cache-Control", "no-store");
  req->send(res);
}

void setupRecoveryRoutes(AsyncWebServer* server) {
  server->on("/recovery", HTTP_GET, [](AsyncWebServerRequest* req) { sendRecoveryPage(req); });
}

void setupRecoveryUiRoute(AsyncWebServer* server) {
  // Sans filesystem, send(FS&) rend 501 (404 avec If-None-Match) — condition S1 :
  // rien ne doit dépendre de cette valeur.
  server->on("/ui", HTTP_GET, [](AsyncWebServerRequest* req) {
    req->send(LittleFS, "/index.html", "text/html");
  });
}

bool serveRecoveryPageIfActive(AsyncWebServerRequest* req) {
  if (!otaRecoveryActive()) return false;
  sendRecoveryPage(req);
  return true;
}
