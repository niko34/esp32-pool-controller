#ifndef WEB_HELPERS_H
#define WEB_HELPERS_H

#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <stdint.h>
#include <stddef.h>  // size_t (capBytes) — self-contained

// Fonctions helper partagées entre tous les modules de routes
void sendJsonResponse(AsyncWebServerRequest* request, JsonDocument& doc);
void sendErrorResponse(AsyncWebServerRequest* request, int code, const String& message);
void sendRawJsonResponse(AsyncWebServerRequest* request, String& json);
String getCurrentTimeISO();

// Helpers adresse ROM 1-Wire 64 bits (feature-020)
// Format : 16 caractères hex majuscules sans séparateur, ex. "28FF1A2B3C4D5E6F".
String formatRomHex(const uint8_t addr[8]);
// Renvoie false si la chaîne n'est pas exactement 16 chars hex valides.
bool parseRomHex(const String& hex, uint8_t addr[8]);

/**
 * @brief Enregistre un handler PRIORITAIRE qui refuse proprement un corps JSON
 *        hors gabarit, ou tout corps en famine mémoire (bug-101 / bug-113).
 *
 * Patron ADR-0067 (le même que la garde 503 « fichiers statiques » de
 * web_server.cpp) : AsyncWebServer::_attachHandler (WebServer.cpp:147) retient
 * le PREMIER handler tel que `filter(request) && canHandle(request)`. Un
 * handler enregistré AVANT celui de la bibliothèque le PRÉEMPTE donc.
 *
 * ⚠️ ORDRE D'APPEL SIGNIFICATIF : appeler cette fonction AVANT
 * `server->addHandler(monJsonHandler)`. Enregistrée après, elle ne verrait
 * jamais passer une requête.
 *
 * Deux verdicts, tous deux en JSON maison — c'est ce qui remplace le `413` NU
 * de la bibliothèque, émis avant REQUIRE_AUTH et sans passer par
 * sendErrorResponse(), donc muet côté client (défaut n°2 de bug-101) :
 *  - 413 si `contentLength() > capBytes` : connu dès l'analyse des EN-TÊTES,
 *    donc AVANT le moindre octet de corps et avant le calloc de
 *    AsyncCallbackJsonWebHandler. Le message est actionnable (octets reçus,
 *    plafond, et quoi faire).
 *  - 503 + Retry-After en famine, via
 *    httpLowHeapShouldRefuse(heapLargestFreeBlock8Bit(), kStaticFileMinAllocBytes)
 *    — constante et prédicat EXISTANTS, déjà couverts par
 *    test_native_http_lowheap_guard : aucun seuil n'est inventé ici.
 *
 * @param server    Serveur sur lequel enregistrer (ignoré si nullptr).
 * @param uri       URI exacte de la route protégée. ⚠️ DOIT être un littéral ou
 *                  une chaîne de durée de vie statique : le pointeur est capturé
 *                  tel quel par le filtre (aucune String n'est construite —
 *                  cf. contrainte async_tcp dans web_helpers.cpp).
 * @param capBytes  Plafond d'octets de corps (kHistoryImportMaxBodyBytes,
 *                  kWifiConnectMaxBodyBytes, kSensorIdentifyMaxBodyBytes).
 */
void registerJsonBodyGuard(AsyncWebServer* server, const char* uri, size_t capBytes);

#endif // WEB_HELPERS_H
