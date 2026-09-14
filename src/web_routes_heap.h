#ifndef WEB_ROUTES_HEAP_H
#define WEB_ROUTES_HEAP_H

#include <ESPAsyncWebServer.h>

// =============================================================================
// web_routes_heap — routes HTTP du recensement du tas (R2) et de la lecture de
// contenu (R3), feature-128.
// =============================================================================
// CONVENTION DE CE FICHIER, DÉLIBÉRÉMENT INVERSE de web_routes_debug.cpp (où
// 4 routes sur 5 sont sans authentification) : AUCUNE route d'ici ne répond
// sans garde. La PREMIÈRE instruction de chaque handler EST la garde — jamais
// un traitement métier avant elle. Sentinelle : tools/check_heap_routes_guard.sh.
//
// - POST /debug/heap_census?used_from=&used_to=            : demande un recensement (R2)
// - GET  /debug/heap_census?kind=free|used&offset=&count=  : instantané figé, paginé (R2)
// - POST /debug/heap_peek?addr=&len=                        : demande une lecture de contenu
//                                                              (R3, NON LIVRÉ EN PRODUCTION, ADR-0081)
// - GET  /debug/heap_peek                                   : hex + ASCII du dernier contenu lu (R3)
//
// R3 n'existe dans le binaire QUE si POOL_HEAP_PEEK est défini au build (jamais
// dans un fichier versionné — tools/check_no_peek_flag.sh). En son absence,
// /debug/heap_peek répond 404 (route jamais enregistrée).
void setupHeapRoutes(AsyncWebServer* server);

#endif  // WEB_ROUTES_HEAP_H
