#ifndef WEB_ROUTES_RECOVERY_H
#define WEB_ROUTES_RECOVERY_H

#include <ESPAsyncWebServer.h>

// =============================================================================
// web_routes_recovery — page de secours OTA (feature-151 D3-b1)
// =============================================================================
// - GET /recovery : page de secours, PUBLIQUE, TOUJOURS servie (depuis la flash,
//   aucun accès au filesystem). Enregistrée AVANT la garde 503 « fichiers
//   statiques » : elle doit répondre même en famine mémoire (condition C2).
// - GET /ui       : l'interface normale (index.html), échappatoire d'un faux
//   positif collant. Enregistrée APRÈS la garde 503 (condition S5) : elle OUVRE
//   un fichier, donc elle doit être couverte par la garde.
// - `/` et `/index.html` rendent la page de secours quand le déclencheur est
//   vrai, via serveRecoveryPageIfActive().
//
// ⛔ Aucune route d'envoi ici : la page poste sur le POST /update EXISTANT, avec
// sa garde d'authentification et sa vérification de signature, inchangées.
// =============================================================================

/** @brief Enregistre GET /recovery. À appeler AVANT la garde 503. */
void setupRecoveryRoutes(AsyncWebServer* server);

/** @brief Enregistre GET /ui. À appeler APRÈS la garde 503 (S5). */
void setupRecoveryUiRoute(AsyncWebServer* server);

/**
 * @brief Si le secours est dû, répond avec la page et rend true ; sinon rend false
 *        sans rien faire (l'appelant sert alors le fichier normalement).
 * C'est LE SEUL lecteur de otaRecoveryActive() de tout src/ (contrôle CI).
 */
bool serveRecoveryPageIfActive(AsyncWebServerRequest* req);

#endif  // WEB_ROUTES_RECOVERY_H
