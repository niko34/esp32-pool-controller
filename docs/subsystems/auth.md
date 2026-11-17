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

## Mécanisme supporté : une seule preuve d'identité

**API Token** (`checkTokenAuth`) — header `X-Auth-Token: <token>` uniquement (le query param `?token=...` est refusé, voir Cas limites). Un mot de passe s'échange contre ce jeton par `POST /auth/login`.

⛔ **HTTP Basic a été retiré (bug-184, v2.92.0)** : `checkBasicAuth()` n'existe plus. La bibliothèque serveur comparait le Base64 reçu **sans tenir compte de la casse** (`WebAuthentication.cpp:24`) ; mesuré sur l'appareil, une casse inversée répondait `200`. Voir [ADR-0130](../adr/0130-l-appareil-n-accepte-que-le-jeton-pas-http-basic.md).

⚠️ **Correction d'une phrase fausse de longue date** : ce fichier affirmait « les routes CRITICAL exigent Basic Auth (pas le token), les routes WRITE acceptent l'un ou l'autre ». C'était faux **avant** ce lot : `checkAuth()` n'a jamais distingué `WRITE` de `CRITICAL` dans le choix de la méthode. Le niveau ne change que la politique de quota et de journalisation.

### Décision d'admission : module pur

`checkAuth()` ne décide plus elle-même : elle collecte les faits (auth activée, premier démarrage, URL exacte `/save-config`, jeton valide, quota) et délègue à [`decideAdmission()`](../../src/auth_admission_logic.h) → `AuthVerdict{kAllow, kDenyUnauth, kDenyRateLimited}`. Testé en natif : `test/test_native_auth_admission/` (ADR-0017).

**Exemption du premier démarrage** : elle porte sur l'URL **exacte** `/save-config` (elle portait sur le préfixe `startsWith("/save-config")`). Même resserrement dans `saveConfigBodyAuthorized()`, partagé avec `/tune-ph-offset` : avant bug-184, le premier démarrage ouvrait **aussi** l'écriture de l'offset pH à n'importe qui sur le réseau.

⚠️ **Le premier démarrage n'est pas sûr pour autant** (ouvert, traité par [bug-185](../../specs/features/todo/bug-185-fenetre-de-premier-demarrage-ouverte-indefiniment.md)) : la fenêtre ne se referme que sur `/auth/complete-wizard`, et le jeton n'est **pas régénéré** au changement de mot de passe. De plus, le **corps** de `/save-config` est admis par `saveConfigBodyAuthorized()` avant tout décompte de quota : ne pas lire « `/save-config` est sous quota au premier démarrage ».

## Comparaison de token à temps constant (v2.11.2, feature-028)

⚠️ **Règle absolue : jamais de `==` / `!=` direct sur le token API.** Une comparaison `String` classique court-circuite au premier octet différent, rendant le temps de réponse dépendant du préfixe correct — canal temporel sur le contrôle d'accès central.

Toute vérification passe par la comparaison à temps constant :

```cpp
// auth.cpp — interne
static bool constantTimeEquals(const String& a, const String& b);

// auth.h — API publique pour les autres modules (WebSocket notamment)
bool AuthManager::secureTokenEquals(const String& candidate) const;
```

- **Longueur vérifiée d'abord** (retour anticipé acceptable : la longueur du token n'est pas secrète — format fixe, 32 caractères hexadécimaux).
- Puis comparaison de **tous** les octets sans court-circuit : accumulation des différences par `diff |= a[i] ^ b[i]`, verdict sur `diff == 0`.

Deux consommateurs :
1. **Auth HTTP** — `checkTokenAuth()` ([`auth.cpp:126`](../../src/auth.cpp)) sur le header `X-Auth-Token`.
2. **Auth WebSocket** — `WsManager::_onData()` ([`ws_manager.cpp`](../../src/ws_manager.cpp)) via `authManager.secureTokenEquals(token)` sur le message `{"type":"auth","token":"..."}`. Historiquement le WS comparait le token en `!=` (court-circuit) — corrigé en v2.11.2 via l'API publique.

> ⚠️ Référence `ws_manager.cpp:114` **retirée** (v2.57.0, bug-081) : le fichier a été
> largement remanié (registre `_authRegistry` + envoi ciblé) et ce numéro de ligne ne pointe
> plus sur `_onData()`. Au-delà de la comparaison à temps constant ci-dessus, bug-081
> corrige un défaut **adjacent** : un client WS authentifié n'était **ciblé nulle part** —
> `textAll()` diffusait à tous les clients connectés dès qu'au moins un était authentifié.
> Deux conséquences côté `auth` :
> - **Ciblage** — les trois broadcasts WS passent désormais par
>   `AsyncWebSocket::text(uint32_t id, AsyncWebSocketSharedBuffer)` sur les ids de
>   `_authRegistry`, et plus par `textAll()`. Détail : [ws-manager.md](ws-manager.md#bug-081--diffusion-ciblée-sur-registre-au-lieu-de-textall).
> - **Révocation** — `WsManager::revokeAllAuthenticated()` est appelée depuis **ce** module
>   (`web_routes_auth.cpp`, `POST /auth/regenerate-token`) et depuis `web_routes_config.cpp`
>   (bascule `auth_enabled` faux→vrai) : une rotation de token ou une activation de
>   l'authentification révoque immédiatement tout client WS déjà authentifié avec
>   l'ancien état, qui doit ré-authentifier. Sans elle, le keepalive WS (30 s) aurait
>   maintenu indéfiniment une session authentifiée avec un token révoqué.

Le code mort de debug (`maskedReceived` / `maskedExpected`) a été supprimé au passage ; seul le masquage volontaire des 8 premiers caractères pour les logs de génération subsiste (le token n'est **jamais** loggué en clair).

## Rate limiting

```cpp
MAX_REQUESTS_PER_MINUTE = kMaxRequestsPerMinute = 30  // constants.h:37
RATE_LIMIT_WINDOW_MS   = kRateLimitWindowMs   = 60000 // constants.h:38
```

`rateLimitMap` : `std::map<String, RateLimitEntry>` indexé par IP cliente. Entrées expirées nettoyées périodiquement par `cleanupRateLimitMap()`.

Au-delà de 30 requêtes / minute par IP → `sendRateLimitExceeded()` (HTTP 429).

## Bornage du journal des refus (`bug-112`, v2.89.1)

Les refus d'authentification journalisent en `WARNING`, mais **de façon bornée** : sans cela un client anonyme purgeait le journal persistant en ~27 requêtes (~13 s). Décision et alternatives : [ADR-0127](../adr/0127-le-bornage-du-journal-d-authentification-est-global-par-emetteur.md).

- **Neuf sites bornés** : 401 (`sendAuthRequired`), 429 (`checkRateLimit`), les trois routes de calibration, l'upload OTA, `/save-config`, `/tune-ph-offset`, `/ws` (« Jeton rejeté », [ws-manager.md](ws-manager.md)). États **indépendants**.
- ⭐ **Borner ≠ authentifier.** Un dixième émetteur, `POST /debug/sensor_trace_clear` (un INFO par requête, sans auth), n'est **pas borné** : il a été **retiré du chemin non authentifié** (`REQUIRE_AUTH(WRITE)`). Ce n'est pas un refus d'authentification (un site de throttle l'aurait annoncé comme tel) et l'action efface une fenêtre de diagnostic (décision A1 de `feature-070`).
- **Clé globale par émetteur** — ni par route, ni par IP : le 401 est **un seul état** pour toutes les routes protégées. Conséquence : seule l'URL de la requête qui a pu parler est nommée ; les autres refus sont comptés dans « et N refus non journalisés », avec les drapeaux `(autres routes)` et `(autres IP)`. ⚠️ Entre clients **IPv6** le drapeau « autres IP » n'est jamais levé.
- **Recul exponentiel** : fenêtre `kUnauthWarnThrottleMs << shift` (60 s de base), `shift` +1 par ligne émise sauf la première, plafond `kUnauthWarnMaxShift` (6 = 64 min), −1 par `kUnauthWarnDecayMs` (1 h) sans tentative. Lignes à t = 0, 1, 3, 7, 15, 31, 63 min.
- **Drain** : `AuthManager::drainUnauthWarnLogs()` (appelé par `loop()`, ainsi que les drains calibration, OTA et config) annonce les occultés après `kUnauthWarnDrainQuietMs` (120 s) de silence, borné par la même fenêtre. Préfixes : `Accès non autorisé :`, `Rate limit dépassé :`, `[OTA] POST /update :`, `[Calibration] /calibrate_x :`, `[Config] /save-config :`, `[pH] /tune-ph-offset :`. Le drain ne remet pas à zéro la date de dernière tentative.
- **Une tentative isolée reste journalisée.** Le 401/429 HTTP part toujours : seul le journal est borné.
- **Contrat** : `sendRateLimitExceeded` ne journalise plus ; l'appelant (`checkRateLimit`) l'a déjà fait. `checkRateLimit` retourne `true` si `client()` est nul (non exploitable : sans connexion aucune réponse ne revient ; l'ancien code déréférençait nul).
- **Chiffres** : pire cas **mesuré** 38 lignes/24 h par site, **342 (9 × 38)** pour neuf sites — somme de maxima mesurés, aucun scénario joint mesuré, aucun majorant établi (38 est le pire **trouvé**, un 39 reste possible) ⇒ le journal reste purgeable à vitesse lente, assumé.
- **Garde CI** : `tools/check_unauth_log_guard.sh` (job `unauth-log-guard`), **cinq règles**, chacune validée par mutation : (1) aucun `systemLogger` dans la branche de refus d'une garde `if (!…Authorized(` de `web_routes_*.cpp` ; (2) aucun `systemLogger` entre l'en-tête d'un handler et sa garde ; (3) dans tout **lambda handler** de `web_routes_*.cpp`, un `systemLogger` ne vient qu'**après** un `REQUIRE_AUTH`, un `…Authorized(` ou un `checkAuth(` ; (4) aucun `systemLogger` dans `ws_manager.cpp::_onData` ni `_onClientConnect` ; (5) aucun `systemLogger` dans `checkAuth`, `checkTokenAuth`, `checkRateLimit`, `sendAuthRequired`, `sendRateLimitExceeded`. ⚠️ **Limite** : la règle 3 ne juge que les lambdas — un lambda sans garde qui appelle une **fonction nommée** contenant un `systemLogger` n'est pas vu (juger aussi les fonctions nommées donnait 48 faux positifs dans `web_routes_ota.cpp`). Exemption unique : `AUTH-LOG-EXEMPT` sur l'INFO de `/auth/complete-wizard` (écrit une seule fois : `clearFirstBootFlag()` rend `isFirstBootDetected()` faux). Code : `auth_log_throttle_logic.h` (pur), `auth_log_throttle_site.h` (coquille atomique).

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
| `GET /auth/token` | Obtient le token API (CRITICAL : exige déjà un jeton valide ; sert à relire un jeton connu, pas à en récupérer un perdu) |
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

⚠️ Le mot de passe est stocké **en clair** en NVS : `setPassword()` ([`auth.cpp:147`](../../src/auth.cpp:147)) fait `adminPassword = pwd;` sans aucune transformation, et `config.cpp` l'écrit tel quel (`prefs.putString("auth_password", …)`). Il n'est **jamais** renvoyé par l'API (`"******"`). (Une version antérieure de ce fichier affirmait à tort qu'il était hashé.)

## Cas limites

- **Auth désactivée** (`authEnabled = false`) : toutes les routes sont accessibles sans creds — déconseillé, mais possible pour debug local.
- **Token perdu** : `POST /auth/regenerate-token` est `CRITICAL` ([`web_routes_auth.cpp:165`](../../src/web_routes_auth.cpp:165)) : **elle exige le jeton qu'on vient de perdre**, donc elle ne sert pas à le récupérer. ⚠️ Et il ne se retrouve **pas** dans l'interface non plus : **aucun écran de `data/` n'affiche le jeton** (vérifié le 2026-11-17). ⇒ Le **seul** chemin est `POST /auth/login` avec le mot de passe administrateur — donc hors premier démarrage, où cette route rend `403`. ⚠️ Sur un appareil en premier démarrage, le seul fournisseur de jeton est `POST /auth/change-password` (cf. [`bug-185`](../../specs/features/todo/bug-185-fenetre-de-premier-demarrage-ouverte-indefiniment.md)).
- **Rate limit atteint** : réponse 429, pas de décompte de la requête elle-même.
- **Token en query param** (`?token=...`) : **refusé** — seul le header `X-Auth-Token` est accepté (fuite via logs, historique navigateur, Referer).
- **CORS** : mécanisme **retiré** en v2.11.2 — politique même-origine stricte, voir [ADR-0023](../adr/0023-politique-cors-retrait.md).

## Fichiers liés

- [`src/auth.h`](../../src/auth.h), [`src/auth.cpp`](../../src/auth.cpp)
- [`src/web_routes_auth.cpp`](../../src/)
- [`src/constants.h:37`](../../src/constants.h:37) — rate limits
- [docs/API.md](../API.md) — matrice complète des niveaux d'auth par route
