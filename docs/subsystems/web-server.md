# Subsystem — `web_server`

- **Fichiers** : [`src/web_server.h`](../../src/web_server.h), [`src/web_server.cpp`](../../src/web_server.cpp)
- **Singleton** : `extern WebServerManager webServer;`
- **Framework** : [ESPAsyncWebServer v3.6.0](https://github.com/me-no-dev/ESPAsyncWebServer)
- **Port** : 80 (`kHttpServerPort` [`constants.h:97`](../../src/constants.h:97))

## Rôle

Orchestre le serveur HTTP asynchrone. Déclare toutes les routes REST (en déléguant leur implémentation aux modules `web_routes_*`), pose les en-têtes de sécurité globaux, gère les requêtes de reboot différé et l'agrégation des gros payloads POST chunkés.

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

## Assets statiques pré-compressés (gzip)

Depuis v2.12.1 (feature-048), le filesystem n'embarque **que la variante `.gz`** des fichiers texte (`app.js.gz`, `index.html.gz`, `app.css.gz`, `uPlot.iife.min.js.gz`, `wizard.html.gz`, `wifi.html.gz`, `login.html.gz`, `uPlot.min.css.gz`) — produite par `minify.js` (zlib niveau 9, voir [BUILD.md](../BUILD.md#pré-compression-gzip-des-assets-feature-048-v2121)). Les images/icônes restent non compressées.

**Aucun code firmware n'a été modifié** : ESPAsyncWebServer v3.6.0 résout nativement `<chemin>.gz` sur les **deux** mécanismes de serving utilisés :

| Chemin de serving | Utilisé par | Résolution `.gz` (source lib) |
|---|---|---|
| `serveStatic("/", LittleFS, "/")` ([`web_server.cpp:100`](../../src/web_server.cpp)) | CSS, JS, images (filtre par extension) | `AsyncStaticWebHandler` teste `path + ".gz"` si le fichier nu est absent — `WebHandlers.cpp:150-178` |
| `request->send(LittleFS, path, "text/html")` | Routes HTML explicites `/`, `/index.html`, `/wizard.html`, `/login.html`, `/wifi.html` (portail captif inclus) | `AsyncFileResponse` bascule sur `path + ".gz"` et pose `Content-Encoding: gzip` — `WebResponses.cpp:642-644` |

Dans les deux cas la réponse porte `Content-Encoding: gzip` et le `Content-Type` reste déduit du chemin **demandé** (sans `.gz`).

Notes :
- Les routes API/JSON ne sont pas concernées (réponses générées à la volée, jamais compressées).
- Un `curl` brut sur un asset statique reçoit l'octet-stream gzip → `curl --compressed`. Tous les navigateurs décompressent de façon transparente.
- Gain : payload FS 443 → 155 KB, chargement des pages ~4× plus rapide.

## Niveaux d'auth

Chaque handler commence par :
```cpp
REQUIRE_AUTH(req, RouteProtection::WRITE);
```
Voir [auth.md](auth.md) pour les 3 niveaux (`NONE`, `WRITE`, `CRITICAL`) et [docs/API.md](../API.md) pour la matrice complète.

## POST chunké (gros JSON config)

Le serveur supporte les POST multi-chunks pour les payloads dépassant la MTU. `configBuffers` (std::map indexé par `AsyncWebServerRequest*`) accumule les fragments ; `configErrors` signale si un chunk a échoué. Une fois toute la requête reçue, le JSON final est parsé avec `kMaxConfigSizeBytes = 16384` ([`constants.h:45`](../../src/constants.h:45)).

## Politique d'origine — pas de CORS (v2.11.2, [ADR-0023](../adr/0023-politique-cors-retrait.md))

Le mécanisme CORS a été **entièrement retiré** : politique **même-origine stricte**. L'UI est servie par l'ESP32 lui-même (offline first), aucun accès cross-origin navigateur n'est supporté. Aucun en-tête `Access-Control-Allow-*` n'est émis, aucun preflight `OPTIONS` n'est traité, plus aucun champ de configuration associé (`auth_cors_origins` supprimé de `/get-config`, `/save-config` et de l'UI).

En-têtes de sécurité **globaux** posés dans `setupRoutes()` via `DefaultHeaders` :
- `Content-Security-Policy: default-src 'self'; script-src 'self' 'unsafe-inline'; style-src 'self' 'unsafe-inline'; img-src 'self' data:`
- `X-Content-Type-Options: nosniff`
- `X-Frame-Options: SAMEORIGIN`

Toute intégration cross-origin future passe par un reverse proxy ou rouvre l'[ADR-0023](../adr/0023-politique-cors-retrait.md).

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
