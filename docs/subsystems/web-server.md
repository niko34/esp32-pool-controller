# Subsystem — `web_server`

- **Fichiers** : [`src/web_server.h`](../../src/web_server.h), [`src/web_server.cpp`](../../src/web_server.cpp)
- **Singleton** : `extern WebServerManager webServer;`
- **Framework** : [ESPAsyncWebServer v3.6.0](https://github.com/me-no-dev/ESPAsyncWebServer)
- **Port** : 80 (`kHttpServerPort` [`constants.h:97`](../../src/constants.h:97))

## Rôle

Orchestre le serveur HTTP asynchrone. Déclare toutes les routes REST (en déléguant leur implémentation aux modules `web_routes_*`), gère les CORS headers, les requêtes de reboot différé, et l'agrégation des gros payloads POST chunkés.

## API publique

```cpp
void begin(AsyncWebServer* webServer, DNSServer* dnsServer);
void update();                          // applique reboot différé
bool isRestartApRequested() const;
void clearRestartRequest();
```

## Organisation des routes

Les routes sont réparties par domaine fonctionnel dans des fichiers séparés :

| Fichier | Domaine | Exemples d'endpoints |
|---------|---------|----------------------|
| [`web_routes_data.cpp`](../../src/web_routes_data.cpp) | Lecture de données | `/data`, `/get-history`, `/history/clear`, `/history/import` |
| [`web_routes_config.cpp`](../../src/web_routes_config.cpp) | Config | `/save-config`, `/get-config`, `/reboot`, `/factory-reset` |
| [`web_routes_control.cpp`](../../src/web_routes_control.cpp) | Actions | `/filtration/on/off`, `/lighting/on/off`, `/ph/inject/*`, `/orp/inject/*`, `/pump[12]/on/off` |
| [`web_routes_calibration.cpp`](../../src/web_routes_calibration.cpp) | Calibration pH | `/calibrate_ph_neutral`, `/calibrate_ph_acid`, `/clear_ph_calibration` |
| [`web_routes_ota.cpp`](../../src/web_routes_ota.cpp) | OTA | `/update`, `/check-update`, `/download-update` |
| [`web_routes_auth.cpp`](../../src/) | Authentification | `/auth/login`, `/auth/token`, `/auth/change-password`, etc. |

Toutes les routes sont enregistrées dans `setupRoutes()` via des fonctions `register_*_routes(AsyncWebServer*)` déclarées dans les headers `web_routes_*.h`.

## Niveaux d'auth

Chaque handler commence par :
```cpp
REQUIRE_AUTH(req, RouteProtection::WRITE);
```
Voir [auth.md](auth.md) pour les 3 niveaux (`NONE`, `WRITE`, `CRITICAL`) et [docs/API.md](../API.md) pour la matrice complète.

## POST chunké (gros JSON config)

Le serveur supporte les POST multi-chunks pour les payloads dépassant la MTU. `configBuffers` (std::map indexé par `AsyncWebServerRequest*`) accumule les fragments ; `configErrors` signale si un chunk a échoué. Une fois toute la requête reçue, le JSON final est parsé avec `kMaxConfigSizeBytes = 16384` ([`constants.h:45`](../../src/constants.h:45)).

## CORS

`setCorsHeaders(req)` ajoute `Access-Control-Allow-Origin`, `Access-Control-Allow-Methods`, `Access-Control-Allow-Headers` selon `authCfg.corsOrigins` :
- Vide → CORS désactivé (pas d'en-têtes).
- `*` → wildcard (moins sécurisé).
- Liste (séparée par `,`) → match strict.

Un reboot est nécessaire pour appliquer une modification de CORS (voir [page-settings.md](../features/page-settings.md)).

## Reboot différé

`restartRequested` / `restartApRequested` sont armés par les handlers `/reboot`, `/reboot-ap`, `/factory-reset`. `update()` vérifie `millis() - restartRequestedTime > kRestartAfterOtaDelayMs` (3 s) avant d'appeler `ESP.restart()` — le délai laisse le temps à la réponse HTTP d'être envoyée.

## DNS captive portal

En mode AP (premier démarrage ou factory reset), un `DNSServer` est associé pour rediriger toutes les requêtes DNS vers l'ESP32, déclenchant l'assistant de configuration WiFi sur le smartphone.

## Fichiers liés

- [`src/web_server.h`](../../src/web_server.h), [`src/web_server.cpp`](../../src/web_server.cpp)
- [`src/web_routes_*.cpp`](../../src/) — implémentations par domaine
- [`src/ws_manager.cpp`](../../src/ws_manager.cpp) — le WebSocket est également attaché à ce serveur
- [docs/API.md](../API.md) — matrice des endpoints et niveaux d'auth
- [auth.md](auth.md)
