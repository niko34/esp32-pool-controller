#include "web_helpers.h"
#include "constants.h"           // kStaticFileMinAllocBytes — seuil EXISTANT, aucun neuf
#include "heap_metrics.h"        // heapLargestFreeBlock8Bit (métrique 8 bits, bug-080)
#include "http_lowheap_logic.h"  // httpLowHeapShouldRefuse — prédicat pur EXISTANT
#include <memory>
#include <string.h>

// Envoi via AsyncCallbackResponse avec Content-Length connu.
// Évite le bug de AsyncBasicResponse qui mute _content via String::substring()
// à chaque ACK TCP — sur grosse réponse et heap fragmenté, substring() peut
// renvoyer un String à buffer null et planter en LoadProhibited.
// Le shared_ptr<String> garde le contenu vivant jusqu'à la fin du transfert,
// et Content-Length permet à lwIP de fermer le socket dès le dernier octet.
static void sendJsonBuffered(AsyncWebServerRequest* request, int code, std::shared_ptr<String> json) {
  size_t len = json->length();
  AsyncWebServerResponse* response = request->beginResponse(
    "application/json", len,
    [json](uint8_t* buffer, size_t maxLen, size_t index) -> size_t {
      size_t remaining = json->length() - index;
      size_t toCopy = remaining < maxLen ? remaining : maxLen;
      memcpy(buffer, json->c_str() + index, toCopy);
      return toCopy;
    });
  if (code != 200) response->setCode(code);
  request->send(response);
}

void sendRawJsonResponse(AsyncWebServerRequest* request, String& json) {
  // std::move évite la copie du buffer — json devient invalide après l'appel
  auto ptr = std::make_shared<String>(std::move(json));
  sendJsonBuffered(request, 200, ptr);
}

void sendJsonResponse(AsyncWebServerRequest* request, JsonDocument& doc) {
  auto json = std::make_shared<String>();
  serializeJson(doc, *json);
  sendJsonBuffered(request, 200, json);
}

void sendErrorResponse(AsyncWebServerRequest* request, int code, const String& message) {
  auto json = std::make_shared<String>();
  JsonDocument doc;
  doc["error"] = message;
  serializeJson(doc, *json);
  sendJsonBuffered(request, code, json);
}

String getCurrentTimeISO() {
  struct tm timeinfo;
  if (getLocalTime(&timeinfo, 0)) {
    char buffer[25];
    strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%S", &timeinfo);
    return String(buffer);
  }
  return "unavailable";
}

// ===== Helpers adresse ROM 1-Wire (feature-020) =====

String formatRomHex(const uint8_t addr[8]) {
  char buf[17];
  for (size_t i = 0; i < 8; ++i) {
    snprintf(&buf[i * 2], 3, "%02X", addr[i]);
  }
  buf[16] = '\0';
  return String(buf);
}

bool parseRomHex(const String& hex, uint8_t addr[8]) {
  if (hex.length() != 16) return false;
  for (size_t i = 0; i < 8; ++i) {
    char hi = hex.charAt(i * 2);
    char lo = hex.charAt(i * 2 + 1);
    auto hexVal = [](char c, int& out) -> bool {
      if (c >= '0' && c <= '9') { out = c - '0'; return true; }
      if (c >= 'a' && c <= 'f') { out = c - 'a' + 10; return true; }
      if (c >= 'A' && c <= 'F') { out = c - 'A' + 10; return true; }
      return false;
    };
    int hv, lv;
    if (!hexVal(hi, hv) || !hexVal(lo, lv)) return false;
    addr[i] = (uint8_t)((hv << 4) | lv);
  }
  return true;
}


// ===== Garde prioritaire de corps JSON (bug-101 / bug-113, v2.48.0) =====

namespace {

// Reproduit EXACTEMENT AsyncURIMatcher::matches() pour le type
// BackwardCompatible — `(_value == path) || path.startsWith(_value + "/")`,
// WebServer.cpp:339 — mais SANS ALLOUER.
//
// ⚠️ POURQUOI NE PAS APPELER matches() : il fait `String path = request->url();`
// (une COPIE) puis, en BackwardCompatible, `_value + "/"` (une seconde). Deux
// allocations, sur `async_tcp`, dans un filtre appelé pour toute requête qui
// atteint cette position de la chaîne. C'est exactement ce que la contrainte
// interdit ici.
// Et pourquoi reproduire le préfixe plutôt qu'une égalité stricte : le handler
// JSON de la bibliothèque, lui, est en BackwardCompatible. Une égalité stricte
// laisserait « /history/import/ » filer jusqu'à lui sans passer par la garde.
bool uriMatchesBackwardCompatible(const String& path, const char* uri) {
  const size_t n = strlen(uri);
  const size_t len = path.length();
  if (len < n) return false;
  if (strncmp(path.c_str(), uri, n) != 0) return false;
  return len == n || path.c_str()[n] == '/';
}

}  // namespace

void registerJsonBodyGuard(AsyncWebServer* server, const char* uri, size_t capBytes) {
  if (server == nullptr || uri == nullptr) return;

  server->on(uri, HTTP_POST, [uri, capBytes](AsyncWebServerRequest* request) {
    // Contexte : tâche `async_tcp`. AUCUN log (un ERROR flusherait le journal en
    // synchrone → fopen → l'abort() même que la garde prévient), AUCUN fichier
    // ouvert, AUCUNE String concaténée : snprintf dans un tampon de PILE, et la
    // seule allocation est la réponse elle-même (inévitable, et déjà le cas de la
    // garde 503 de web_server.cpp).
    //
    // VERDICT DÉTERMINISTE, SANS SECONDE LECTURE DU TAS : le filtre n'accepte que
    // si le corps dépasse le plafond OU si le tas est en famine. Donc ici,
    // « corps sous le plafond » implique « c'est la famine » — inutile de
    // re-mesurer, et surtout aucun risque de tomber dans un troisième cas si le
    // tas s'est rétabli entre le filtre et cet appel.
    if (request->contentLength() > capBytes) {
      char body[224];
      snprintf(body, sizeof(body),
               // Message GÉNÉRIQUE : ce garde sert /history/import, /wifi/connect
               // et /sensors/onewire/identify. Une mention de découpage d'import
               // serait absurde sur les deux dernières (relevé en revue, bug-101).
               // Le conseil actionnable propre à l'import vit côté interface, qui
               // affiche la limite AVANT l'envoi et découpe seule.
               "{\"error\":\"Corps JSON trop volumineux : %u octets reçus, "
               "plafond %u octets pour cette route.\"}",
               (unsigned)request->contentLength(), (unsigned)capBytes);
      request->send(413, "application/json", body);
      return;
    }

    AsyncWebServerResponse* res = request->beginResponse(
      503, "application/json",
      "{\"error\":\"Mémoire insuffisante, réessayez dans quelques secondes\"}");
    // 5 s : même valeur que la garde 503 des fichiers statiques — sous le
    // débounce d'auto-guérison (~15 s), au-dessus d'une rafale de chargement.
    res->addHeader("Retry-After", "5");
    request->send(res);
  }).setFilter([uri, capBytes](AsyncWebServerRequest* request) {
    // Aussi sur `async_tcp`, et appelé AVANT canHandle() (donc avant le contrôle
    // de méthode et d'URI) pour toute requête qui atteint cette position de la
    // chaîne de handlers. D'où l'ordre des tests : les deux court-circuits
    // gratuits d'abord, le PARCOURS DU TAS en dernier — sans quoi chaque GET de
    // page walkerait le tas trois fois de plus (une par route gardée).
    if (request->method() != HTTP_POST) return false;
    if (!uriMatchesBackwardCompatible(request->url(), uri)) return false;
    // 413 : le Content-Length est connu dès la fin des en-têtes
    // (WebRequest.cpp:632 puis _attachHandler à la ligne 997), donc AVANT le
    // premier octet de corps et avant tout calloc.
    if (request->contentLength() > capBytes) return true;
    // 503 : heapLargestFreeBlock8Bit() seulement, JAMAIS heapMetricsSnapshot().
    return httpLowHeapShouldRefuse(heapLargestFreeBlock8Bit(),
                                   kStaticFileMinAllocBytes);
  });
}
