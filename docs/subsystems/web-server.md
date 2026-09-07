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
void update();                          // reboot différé + drain de la garde 503 (v2.41.0)
bool isRestartApRequested() const;
void clearRestartRequest();
```

Fonction libre ([`src/web_helpers.h`](../../src/web_helpers.h), v2.48.0, bug-113) :

```cpp
// Garde prioritaire de corps JSON : 413 (corps > capBytes) / 503 (famine) rendus
// AVANT le tamponnage de AsyncCallbackJsonWebHandler. À enregistrer AVANT le
// handler gardé — l'ordre EST le mécanisme (voir section dédiée).
void registerJsonBodyGuard(AsyncWebServer* server, const char* uri, size_t capBytes);
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

## Garde 503 « fichiers statiques » (v2.41.0, bug-080 lot B-3, [ADR-0067](../adr/0067-garde-503-avant-fopen-handler-prioritaire.md))

### Ce qu'elle ferme

Servir une page statique (`req->send(LittleFS, …)` ou `serveStatic`) finit par appeler `fopen`, qui demande à la libc un **verrou récursif interne** (**92 o** — `sizeof(Queue_t)`, IDF 5.5 ; l'ADR-0067 écrivait « ~88 o », valeur corrigée par le désassemblage du cycle 1 de `bug-100`). Si ce `malloc` échoue, `lock_init_generic` (`newlib locks.c:77`) **aborte** : il n'y a **aucun code de retour à tester**. C'est le PANIC du 29/08.

> ⚠️ **Ce chemin passe par `malloc` (C), pas par `operator new`.** Le parachute OOM ([ADR-0035](../adr/0035-parachute-oom-new-handler.md)) est un `std::set_new_handler` : il est **inopérant par construction** sur cette classe de crashs. Cette garde en est la **seule** défense.


### Ce qu'elle NE ferme PAS

La garde est **`HTTP_GET`-only** et ne couvre **que le service de fichiers statiques**. Elle ne garde **rien de ce que le firmware ouvre pour son propre compte** : le recensement de `bug-100` dénombre **14 sites** internes, dont **zéro** est couvert ici ([`acces-fichiers.md`](acces-fichiers.md)).

Trois raisons distinctes, aucune accidentelle :

1. les 13 sites non-HTTP ne passent par **aucun matcher** ;
2. **`/download-logs` est exempt par l'ordre d'enregistrement** — c'est un choix délibéré d'ADR-0067 (garder l'appareil observable en famine), pas un oubli ;
3. **`POST /history/import` est hors matcher** (garde GET-only), alors qu'il atteint les deux `fopen` de `saveToFile()` depuis `async_tcp`. ✅ **Couvert depuis la v2.48.0** par une garde distincte, `registerJsonBodyGuard()` (section ci-dessous) : la route répond **503 + `Retry-After`** en famine, sur la **même** métrique et le **même** seuil. La garde statique reste `HTTP_GET`-only, elle n'a pas bougé.

Le PANIC du 05/09 (`loopTask`, sauvegarde périodique de l'historique) est exactement ce trou. Stratégie retenue : [ADR-0070](../adr/0070-fopen-interne-helper-unique-et-classes-de-contention.md) — point d'admission unique, deux classes de contention.

> ✅ **Implémenté en v2.43.0** (cycle 2 de `bug-100`). Les **13 sites** internes restants passent par [`src/fs_guard.cpp`](../../src/fs_guard.cpp) ; un contrôle CI ([`tools/check_fopen_guard.sh`](../../tools/check_fopen_guard.sh)) échoue si un autre fichier de `src/` ouvre un fichier lui-même. **Rien ne change côté routage** : la garde 503 reste `HTTP_GET`-only, et l'exemption de `/download-logs` par l'ordre d'enregistrement est **conservée telle quelle**. Ce qui change, c'est que la route exempte garde désormais son `fopen` **de l'intérieur** : sous 8 192 o, elle répond **200 avec un export tronqué et annoncé**, jamais un `abort()` — cf. [`docs/API.md`](../API.md#get-download-logs--write). L'appareil reste observable en famine, ce qui était **la raison** de l'exemption. ⚠️ **Non validé sur matériel.**

### Mécanisme

Un **handler prioritaire unique** est enregistré dans `setupRoutes()` :

```cpp
server->on(AsyncURIMatcher::all(), HTTP_GET, /* → 503 + Retry-After: 5 */)
      .setFilter(/* heapLargestFreeBlock8Bit() < kStaticFileMinAllocBytes */);
```

Il répond **`503 Service Unavailable`**, corps `text/plain` `Memoire insuffisante, reessayez` (~30 o), en-tête **`Retry-After: 5`** — sous le débounce d'auto-guérison (~15 s), au-dessus de l'échelle d'une rafale de chargement de page.

Un filtre posé sur `serveStatic` **n'aurait pas marché**, pour deux raisons indépendantes :

1. `AsyncStaticWebHandler::canHandle` (`WebHandlers.cpp:105-107`) appelle `_getFile()` → `_searchFile()` : **`serveStatic` touche le FS dès `canHandle`**, donc le filtre serait arrivé **après** `fopen` ;
2. le filtre de `serveStatic` **rejette les `.html`** — il n'aurait jamais couvert les 7 routes HTML explicites, c'est-à-dire le chemin exact du PANIC.

### ⚠️ L'ORDRE D'ENREGISTREMENT EST LA POLITIQUE D'EXEMPTION

`_attachHandler` (`WebServer.cpp:145-152`) parcourt `_handlers` **dans l'ordre d'enregistrement** et s'arrête au **premier** `filter() && canHandle()`. La position du handler — **après `wsManager.begin(server)`, avant `/login.html`** — est donc **délibérée et structurante** :

| Statut en famine | Routes | Mécanisme |
|---|---|---|
| **Servi** | les **19 routes `HTTP_GET`** déclarées avant : `/auth/status`, `/auth/token`, `/auth/ap-password`, `/data`, `/get-config`, `/get-logs`, `/download-logs`, `/get-history`, `/get-kpi`, `/get-system-info`, `/time-now`, `/wifi/status`, `/wifi/scan`, `/check-update`, `/coredump/info`, `/coredump/download`, `/sensors/onewire/scan`, `/debug/sensor_filter_state`, `/debug/sensor_trace` | premier match gagne |
| **Servi** | `/ws` | déclaré avant **et** `canHandle` exige `isHTTP()` |
| **Servi** | toutes les routes `HTTP_POST`, dont les routes pompes dynamiques (`onNotFound`) | hors du matcher `HTTP_GET` |
| **Refusé (503)** | `/`, `/index.html`, `/login.html`, `/wifi.html`, `/wizard.html`, `/config`, `/config.html` et tout `serveStatic` (CSS, JS, images, polices) | déclarées après |

**Aucune liste d'exemption n'est maintenue** : une liste s'oublie, l'ordre d'enregistrement se voit en lisant `setupRoutes()` de haut en bas.

> 🔒 **TOUTE ROUTE `HTTP_GET` AJOUTÉE APRÈS CE POINT SERA COUVERTE PAR LA GARDE** et répondra 503 sous 8 192 o de plus gros bloc 8 bits. Une route GET qui doit rester servie en famine (diagnostic, secours) **doit** être enregistrée **avant**, avec les routes JSON.

> **Nuance à ne pas perdre** : le matcher protège de l'**interception**, pas de l'**évaluation**. `_attachHandler` teste `h->filter(request) && h->canHandle(request)` — le filtre passe **avant** le contrôle de méthode. Un `POST` qui remonte jusqu'à cette position **exécute le filtre** (parcours de tas compris) sans jamais être refusé.

### Seuil et coût

`kStaticFileMinAllocBytes = 8192` ([`constants.h:272`](../../src/constants.h:272)) — **6ᵉ barreau** de l'échelle 8 bits d'[ADR-0058](../adr/0058-gardes-heap-metrique-8-bits.md), les 5 autres inchangés. Deux `static_assert` verrouillent sa position (`kWsPushMinAllocBytes < seuil < kLowHeapWarnBytes`) et son **plancher de 6 144 o** : une échelle mal ordonnée casse le **build**, pas un test.

Le filtre appelle `heapLargestFreeBlock8Bit()` **à chaque GET atteignant la garde**, sur la tâche `async_tcp` : **70-250 µs** par appel, **1-3 ms** par chargement de page — **≲ 5-10 %** du coût d'un service de fichier. Arbitré acceptable **sans cache ni bornage** : la borne est acquise **par construction** (masque figé `INTERNAL|8BIT`, plafond pathologique ~800 µs), et un cache est **refusé** — le faux négatif d'une garde anti-crash vaut un `abort()`, alors que [`heap_metrics.h:133-135`](../../src/heap_metrics.h:133) documente ~45 Ko d'oscillation entre deux échantillons distants de 2 s. ⚠️ Le coût d'un parcours TLSF **croît avec la fragmentation** : il est maximal exactement dans le régime pour lequel la garde existe.

### Journal — 2 lignes par épisode, WARN/INFO exclusivement

Le callback `async_tcp` ne fait qu'un `fetch_add` atomique puis `send` : **zéro `String`, zéro log, zéro allocation** hors la réponse. Le drain est fait depuis la loop par `WebServerManager::drainLowHeapGuard()` (appelée par `update()`), via le module pur [`src/http_lowheap_logic.h`](../../src/http_lowheap_logic.h) :

| Front | Niveau | Ligne |
|---|---|---|
| Montant (1ᵉʳ refus drainé) | `WARN` | `Service fichiers suspendu (503) — maxBloc8=X o < 8192 o` |
| Descendant | `INFO` | `Service fichiers rétabli — N refus en T s` |

La fermeture exige **deux** conditions cumulées : `kHttpLowHeapEpisodeQuietMs` = **30 000 ms** sans nouveau refus **et** tas revenu `>= 8192`. Cette constante est **d'abord une garde de coût** — sans elle, `update()` parcourrait le tas **à la fréquence de la loop pendant toute la famine** ; c'est **ensuite** un anti-spam (un tas oscillant autour du seuil produirait sinon une paire de lignes par oscillation).

> 🔒 **`ERROR` et `CRITICAL` sont INTERDITS dans tout ce chemin.** [`logger.cpp:71-73`](../../src/logger.cpp:71) flushe en **synchrone, dans la tâche appelante**, sur `ERROR`/`CRITICAL` → `fopen` → **l'`abort()` que cette garde prévient**. Voir [logger.md](logger.md#-un-log-error-depuis-un-chemin-anti-fopen-déclenche-labort-quon-prévient-v2410).

### Ce qu'il faut savoir en exploitation

- **Bande de verrouillage stable** : l'auto-guérison mord à **4 096 o**, la garde à **8 192**. Entre les deux, l'interface web est **entièrement refusée** et **aucun reboot ne survient** — le régime est **stable**, pas transitoire.
- **En famine, `/wifi.html` et `/` sont refusés** : portail AP de secours et page OTA **inaccessibles par le navigateur**. Chemins de secours réels : `POST /update` (hors matcher), `./deploy.sh ota-firmware`, et les routes de diagnostic ci-dessus.
- Un **WARN sans son INFO** signifie que l'épisode est **toujours en cours** (famine persistante sans visiteur pour l'alimenter), ou qu'un **reboot** l'a interrompu — auquel cas la bannière de démarrage clôt l'épisode.

## Garde de corps JSON `registerJsonBodyGuard()` (bug-113, v2.48.0)

- **Fichiers** : [`src/web_helpers.h`](../../src/web_helpers.h), [`src/web_helpers.cpp`](../../src/web_helpers.cpp)
- **Patron** : [ADR-0067](../adr/0067-garde-503-avant-fopen-handler-prioritaire.md) — handler prioritaire enregistré **avant** celui qu'il garde
- **Décision d'ensemble** : [ADR-0075](../adr/0075-decoupage-client-contre-streaming-firmware.md) — découpage client contre streaming firmware, et pourquoi le plafond **descend**

```cpp
void registerJsonBodyGuard(AsyncWebServer* server, const char* uri, size_t capBytes);
```

### Ce qu'elle ferme

`AsyncCallbackJsonWebHandler` **tamponne le corps entier** (`calloc(total + 1)`) avant
d'appeler son callback, donc **avant `REQUIRE_AUTH`**. Trois routes l'utilisent, et son
plafond par défaut (16 384 o) n'était **jamais surchargé** : **49 152 octets
réclamables sans identifiants**. La garde impose un plafond **par route**, et rend le
refus **en JSON maison** au lieu du `413` nu de la bibliothèque.

| Route | Plafond | Constante |
|---|---|---|
| `POST /history/import` | **8 192 o** | `kHistoryImportMaxBodyBytes` |
| `POST /wifi/connect` | **1 024 o** | `kWifiConnectMaxBodyBytes` |
| `POST /sensors/onewire/identify` | **256 o** | `kSensorIdentifyMaxBodyBytes` |
| **Total exposé sans authentification** | **9 472 o** | contre 49 152 — **−80,73 %** |

Le total, l'écart au défaut de la bibliothèque et la position du budget client par
rapport au seuil PSRAM sont **assertés à la compilation**
([`constants.h`](../../src/constants.h)) : réviser un plafond **casse le build** au lieu
de périmer cette page.

> ⚠️ **Le motif n'est pas « ça va planter ».** La prémisse d'origine de `bug-113` est
> **fausse** au-delà de 4 095 o : `calloc` → `heap_caps_malloc_default`, qui charge
> `MALLOC_CAP_DEFAULT|MALLOC_CAP_SPIRAM` dès que `size > malloc_alwaysinternal_limit`
> — donc **PSRAM d'abord**, repli interne seulement si la PSRAM rend `NULL` (établi
> **par désassemblage** de `libheap.a`, section `.iram1.2`, le 2026-09-07 ;
> [ADR-0061](../adr/0061-bascule-psram-au-dela-de-4095-octets-reserve-parachute-en-interne.md)).
> Un tampon de 16 Ko ne partait **pas** dans le tas 8 bits affamé. Le plafond bas reste
> juste, mais pour une raison **plus solide** : sans lui, la sûreté de ce chemin
> reposerait **entièrement sur une préférence d'allocateur**, dont le repli interne est
> le contre-exemple. Le dépôt s'est déjà trompé **dans l'autre sens** — le parachute
> anti-OOM a vécu **en PSRAM**, donc décoratif, depuis `feature-066`.
>
> **Corollaire contre-intuitif** : découper les lots **plus fin** ramènerait
> l'allocation **dans** le tas affamé (≤ 4 095 o ⇒ interne). Le budget de lot client
> (`kHistoryImportChunkBudgetBytes` = **7 168 o**) est **au-dessus** de ce seuil, à
> dessein, et un `static_assert` l'interdit de descendre.

### Mécanisme — l'ordre d'enregistrement EST la garde

```cpp
registerJsonBodyGuard(server, "/history/import", kHistoryImportMaxBodyBytes);
importHandler->setMaxContentLength(kHistoryImportMaxBodyBytes);
server->addHandler(importHandler);   // ⚠️ APRÈS la garde, jamais avant
```

`AsyncWebServer::_attachHandler` retient le **premier** handler dont `filter()` **et**
`canHandle()` passent (`WebServer.cpp:147`), et le handler est attaché **en fin
d'en-têtes** (`WebRequest.cpp:997`) : le `Content-Length` est donc connu **avant le
premier octet de corps**, et *a fortiori* avant tout `calloc`. Permuter les deux lignes
rendrait la garde **inerte sans rien casser visiblement** — c'est la mutation M7 de la
campagne de `bug-101`, tuée par une sentinelle structurelle dédiée.

**Ordre des tests dans le filtre — méthode → URI → longueur → tas en DERNIER.** Le
filtre est appelé **avant `canHandle()`**, donc pour toute requête qui atteint cette
position de la chaîne : les deux court-circuits gratuits passent d'abord, sans quoi
chaque `GET` de page parcourrait le tas **trois fois de plus** (une par route gardée).

**Filtre sans aucune allocation.** La comparaison d'URI **reproduit** la correspondance
**souple** de la bibliothèque (`AsyncURIMatcher` en `BackwardCompatible` :
`_value == path || path.startsWith(_value + "/")`) au lieu d'appeler `matches()`, qui
copie `request->url()` **puis** concatène `_value + "/"` — deux allocations sur
`async_tcp`. ⚠️ **Une égalité stricte n'aurait pas suffi** : le handler JSON gardé est
lui-même en `BackwardCompatible`, donc `/history/import/` aurait **filé jusqu'au 413
nu** de la bibliothèque.

### Les deux réponses

| Cas | Réponse |
|---|---|
| `contentLength() > capBytes` | **413** `{"error":"Corps JSON trop volumineux : N octets reçus, plafond M octets pour cette route."}` |
| famine mémoire | **503** `{"error":"Mémoire insuffisante, réessayez dans quelques secondes"}` + `Retry-After: 5` |

- **Comparaison strictement `>`, deux fois** (filtre *et* handler), identique à
  `AsyncJson.cpp:158/203` : le plafond est le nombre d'octets **autorisés**. Un `>=`
  ferait diverger garde et bibliothèque **d'un octet** — un `413` inexplicable en
  production, introuvable en recette. Verrouillé par test (mutation M5).
- **Verdict déterministe, sans seconde lecture du tas** : le filtre n'accepte que si le
  corps dépasse le plafond **ou** si le tas est en famine ; dans le handler, « corps
  sous le plafond » **implique** donc la famine. Aucun risque de troisième cas si le tas
  s'est rétabli entre les deux.
- **Famine mesurée sur la métrique 8 bits** :
  `httpLowHeapShouldRefuse(heapLargestFreeBlock8Bit(), kStaticFileMinAllocBytes)` —
  prédicat et constante **existants**, aucun seuil inventé
  ([ADR-0058](../adr/0058-gardes-heap-metrique-8-bits.md),
  [ADR-0066](../adr/0066-sonde-du-tas-au-moment-du-refus.md)). Jamais
  `ESP.getFreeHeap()` / `getMaxAllocHeap()`, la métrique fausse du projet.
- **`Retry-After: 5`** : même valeur que la garde 503 des fichiers statiques — sous le
  débounce d'auto-guérison (~15 s), au-dessus d'une rafale de chargement.
- **Zéro log, zéro `String`.** Le garde tourne sur `async_tcp` : un `WARNING`
  déclencherait un flush synchrone du journal, donc un `fopen`, **l'`abort()` même
  qu'il prévient** (cf. [logger.md](logger.md)). Le corps du 413 est composé par
  `snprintf` dans un tampon de **pile** de 224 o.
- ⚠️ **Message 413 volontairement GÉNÉRIQUE** : le garde sert **trois** routes. Une
  mention de « découpez votre import » y serait absurde sur `/wifi/connect` et
  `/sensors/onewire/identify` (relevé en revue). Le conseil actionnable propre à
  l'import vit **côté interface**, qui affiche la limite *avant* l'envoi et découpe
  seule.
- **`setMaxContentLength()` reste posé EN PLUS du garde**, et ce n'est pas redondant :
  le garde lit le `Content-Length` **annoncé**, la bibliothèque compte les octets
  **réellement reçus**. Un client qui sous-déclare son corps n'est arrêté que par la
  seconde.

> **Ce que la garde ne fait pas** : elle ne déplace pas l'authentification. Le `413` et
> le `503` sont rendus **avant `REQUIRE_AUTH`** — structurel, le `Content-Length` étant
> connu bien avant le callback de corps. D'où le message générique, qui **ne divulgue
> rien** sur l'appareil.

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
