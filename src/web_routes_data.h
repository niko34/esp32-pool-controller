#ifndef WEB_ROUTES_DATA_H
#define WEB_ROUTES_DATA_H

#include <ESPAsyncWebServer.h>

// Déclarations des handlers pour les routes de données
void setupDataRoutes(AsyncWebServer* server);

/**
 * @brief Nombre d'exports /download-logs dont le journal persistant n'a pas pu
 *        être ouvert pour FAMINE MÉMOIRE depuis le boot (bug-100, ADR-0070).
 *        L'export reste servi, mais tronqué à la session courante — et le dit.
 */
uint32_t logsExportFopenDeferralCount();

#endif // WEB_ROUTES_DATA_H
