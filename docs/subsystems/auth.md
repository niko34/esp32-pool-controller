# Subsystem — `auth`

- **Fichiers** : [`src/auth.h`](../../src/auth.h), [`src/auth.cpp`](../../src/auth.cpp) (voir aussi [`src/web_routes_auth.cpp`](../../src/) pour les endpoints)
- **Singleton** : `extern AuthManager authManager;`

## Rôle

Gère l'authentification admin + API token + rate limiting pour toutes les routes HTTP (et indirectement WebSocket). Trois niveaux de protection :

```cpp
enum class RouteProtection {
  NONE,      // Route publique (lecture seule, ex: /get-config, /data)
  WRITE,     // Route d'écriture (auth requise — ex: /filtration/on)
  CRITICAL   // Route critique admin (ex: /save-config, /factory-reset, /reboot)
};
```

## Mécanismes supportés

1. **HTTP Basic Auth** (`checkBasicAuth`) — header `Authorization: Basic base64(user:password)`.
2. **API Token** (`checkTokenAuth`) — header `Authorization: Bearer <token>` ou query `?token=...`.

Les routes CRITICAL exigent Basic Auth (pas le token), les routes WRITE acceptent l'un ou l'autre.

## Rate limiting

```cpp
MAX_REQUESTS_PER_MINUTE = kMaxRequestsPerMinute = 30  // constants.h:37
RATE_LIMIT_WINDOW_MS   = kRateLimitWindowMs   = 60000 // constants.h:38
```

`rateLimitMap` : `std::map<String, RateLimitEntry>` indexé par IP cliente. Entrées expirées nettoyées périodiquement par `cleanupRateLimitMap()`.

Au-delà de 30 requêtes / minute par IP → `sendRateLimitExceeded()` (HTTP 429).

## Premier démarrage

Flag `isFirstBoot = true` quand le mot de passe admin est encore celui par défaut (défini par le firmware). L'UI affiche alors un onboarding forçant le changement de mot de passe.

- `clearFirstBootFlag()` — appelé après le premier changement de mot de passe réussi.
- `resetPasswordToDefault()` — déclenché par maintien du bouton physique factory reset pendant `kFactoryResetButtonHoldMs = 10000` ms ([`constants.h:34`](../../src/constants.h:34)).

## AP fallback password

`getApPassword()` retourne le mot de passe SSID `PoolControllerAP` affiché au factory reset. Dérivé du chipID pour rester unique par appareil.

## Endpoints d'auth

| Endpoint | Rôle |
|----------|------|
| `POST /auth/login` | Vérifie les creds, retourne un token API |
| `GET /auth/status` | État courant (enabled, firstBoot) |
| `GET /auth/token` | Obtient le token API (Basic Auth requis) |
| `POST /auth/change-password` | Change le mot de passe admin |
| `POST /auth/regenerate-token` | Génère un nouveau token (invalide l'ancien) |
| `POST /auth/complete-wizard` | Marque la fin de l'onboarding premier démarrage |
| `POST /auth/ap-password` | Change le mot de passe AP (mode hors-ligne) |

## Macro helper

```cpp
#define REQUIRE_AUTH(req, level) \
  if (!authManager.checkAuth(req, level)) { return; }
```

Utilisé dans tous les handlers de `web_routes_*.cpp` pour garder le code concis :
```cpp
server->on("/save-config", HTTP_POST, [](AsyncWebServerRequest* req) {
  REQUIRE_AUTH(req, RouteProtection::CRITICAL);
  // ...
});
```

## Stockage du mot de passe

Le mot de passe est hashé (voir `setPassword()` dans [`auth.cpp`](../../src/auth.cpp)) et persisté en NVS. Il n'est **jamais** renvoyé par l'API.

## Cas limites

- **Auth désactivée** (`authEnabled = false`) : toutes les routes sont accessibles sans creds — déconseillé, mais possible pour debug local.
- **Token perdu** : l'utilisateur doit `POST /auth/regenerate-token` (Basic Auth).
- **Rate limit atteint** : réponse 429, pas de décompte de la requête elle-même.
- **CORS** : géré séparément dans [`web_server.cpp`](../../src/web_server.cpp) via `AuthConfig.corsOrigins`.

## Fichiers liés

- [`src/auth.h`](../../src/auth.h), [`src/auth.cpp`](../../src/auth.cpp)
- [`src/web_routes_auth.cpp`](../../src/)
- [`src/constants.h:37`](../../src/constants.h:37) — rate limits
- [docs/API.md](../API.md) — matrice complète des niveaux d'auth par route
