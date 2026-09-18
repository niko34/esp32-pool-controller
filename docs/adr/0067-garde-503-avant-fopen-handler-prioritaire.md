# ADR-0067 — Le refus 503 précède le premier `fopen` : handler prioritaire, et l'ordre d'enregistrement FAIT la politique d'exemption

- **Statut** : Accepté
- **Date** : 2026-09-05
- **Décideurs** : `architect`, `code-reviewer`, Nicolas — **pas de gate `pool-chemistry`**, vérifié indépendamment : [`web_routes_control.cpp`](../../src/web_routes_control.cpp) contient **0 route `HTTP_GET`** et 19 `HTTP_POST`, donc aucune commande de dosage ne peut être interceptée par une garde GET-only
- **Spec(s) liée(s)** : `bug-080-redemarrage-panic-et-fragmentation-heap` (lot B-3, v2.41.0)

## Contexte

### Un chemin de crash SANS RECOURS

Le PANIC du 29/08 (coredump décodé) se lit ainsi :

```
abort() ← lock_init_generic (newlib locks.c:77) ← __retarget_lock_init_recursive
       ← __sfp ← fopen ← VFSFileImpl ← LittleFS
       ← AsyncWebServerRequest::send(fs, "/index.html", "text/html")
```

Servir une page statique demande à la libc un **verrou récursif interne** (~88 o). Si ce
`malloc` échoue, `lock_init_generic` **aborte** : il n'existe **aucun code de retour à
tester**, aucune branche d'erreur à écrire. La seule défense possible est de **ne pas entrer
dans `fopen`**.

### ⚠️ Le parachute OOM est inopérant par construction sur cette classe de crashs

Ce chemin passe par **`malloc` (C)**, pas par `operator new`. Le parachute
([ADR-0035](0035-parachute-oom-new-handler.md)) est un `std::set_new_handler` : il ne voit
jamais cette allocation. Aucun réglage de sa réserve, aucune correction de son emplacement
(lot B-1) ne peut fermer ce chemin. **Cette garde en est la seule défense.**

### Ce que les lots précédents ont fait — et ce qu'ils n'ont pas fait

- **B-1** ([ADR-0061](0061-bascule-psram-au-dela-de-4095-octets-reserve-parachute-en-interne.md))
  a déporté en PSRAM les allocations > 4095 o, donc le tampon stdio de 4096 o de chaque
  fichier LittleFS ouvert : la **cause principale de la rafale** de RAM interne au chargement
  d'une page. Le chemin abort-prone du verrou newlib, lui, est **inchangé** — il reste en
  RAM interne et reste sans recours.
- **B-2** ([ADR-0066](0066-sonde-du-tas-au-moment-du-refus.md)) a rendu le verdict
  post-mortem **juste**. Il **diagnostique** ; il n'empêche rien.
- **A** ([ADR-0058](0058-gardes-heap-metrique-8-bits.md)) a rendu les gardes heap opérantes
  en basculant sur `heapLargestFreeBlock8Bit()`. Aucune de ces gardes ne couvre le service
  des fichiers.

Le régime terrain rend la question urgente : le plateau de plus gros bloc 8 bits a décru de
façon **monotone** au fil des versions — **42 996 → 26 612 → 20 468 → 15 860 → 13 300 o** —
et n'est **jamais** remonté hors reboot.

## Décision

### (a) Un handler PRIORITAIRE unique, pas un filtre sur `serveStatic`

Un seul handler est enregistré dans [`setupRoutes()`](../../src/web_server.cpp), à une
position **délibérée** : **après `wsManager.begin(server)`, avant `/login.html`**.

```cpp
server->on(AsyncURIMatcher::all(), HTTP_GET, [](AsyncWebServerRequest* req) {
    sStaticRefusalCount.fetch_add(1, std::memory_order_relaxed);
    AsyncWebServerResponse* res =
      req->beginResponse(503, "text/plain", "Memoire insuffisante, reessayez");
    res->addHeader("Retry-After", "5");
    req->send(res);
  }).setFilter([](AsyncWebServerRequest*) {
    const uint32_t maxAlloc8 = heapLargestFreeBlock8Bit();
    if (!httpLowHeapShouldRefuse(maxAlloc8, kStaticFileMinAllocBytes)) return false;
    sStaticRefusalMaxAlloc.store(maxAlloc8, std::memory_order_relaxed);
    return true;
  });
```

**Pourquoi pas un filtre posé sur `serveStatic`** — deux raisons, toutes deux vérifiées à la
source, et chacune suffisante :

1. `AsyncStaticWebHandler::canHandle` (`WebHandlers.cpp:105-107`) appelle `_getFile()` →
   `_searchFile()` : **`serveStatic` touche le système de fichiers dès `canHandle`**. Un
   filtre posé là serait arrivé **après** le `fopen` qu'on veut éviter.
2. Le filtre existant de `serveStatic` **rejette les `.html`** (les 7 pages sont servies par
   des routes explicites). Il n'aurait donc **jamais couvert le chemin du PANIC**.

Seul un handler **antérieur dans l'ordre d'enregistrement** est réellement en amont.

### L'ordre d'enregistrement EST la politique d'exemption

`_attachHandler` (`WebServer.cpp:145-152`) parcourt `_handlers` **dans l'ordre
d'enregistrement** et s'arrête au **premier** `filter() && canHandle()`. Il en découle,
sans une ligne de code d'exemption :

| Statut | Routes | Mécanisme |
|---|---|---|
| **Exempt** (servi même en famine) | les **19 routes `HTTP_GET`** déclarées au-dessus : `/auth/status`, `/auth/token`, `/auth/ap-password`, `/data`, `/get-config`, `/get-logs`, `/download-logs`, `/get-history`, `/get-kpi`, `/get-system-info`, `/time-now`, `/wifi/status`, `/wifi/scan`, `/check-update`, `/coredump/info`, `/coredump/download`, `/sensors/onewire/scan`, `/debug/sensor_filter_state`, `/debug/sensor_trace` | déclarées **avant** → premier match gagne |
| **Exempt** | `/ws` | déclaré avant **et** son `canHandle` exige `isHTTP()` |
| **Hors matcher** | routes dynamiques des pompes (`onNotFound`) | **`HTTP_POST`** uniquement |
| **Couvert** | les 7 routes HTML explicites (`/`, `/index.html`, `/login.html`, `/wifi.html`, `/wizard.html`, `/config`, `/config.html`) et `serveStatic` | déclarées **après** |

**Aucune liste d'exemption n'est maintenue** : une liste s'oublie, l'ordre d'enregistrement
se voit. C'est le cœur de la décision, et sa contrepartie est explicite : **toute route
`HTTP_GET` ajoutée après ce point sera couverte par la garde**.

> ⚠️ **Nuance à ne pas perdre** : le matcher protège de l'**interception**, pas de
> l'**évaluation**. `_attachHandler` (`WebServer.cpp:147`) teste
> `h->filter(request) && h->canHandle(request)` — le filtre s'exécute **avant** le contrôle
> de méthode. Un `POST` qui remonte jusqu'à cette position (les routes pompes servies par
> `onNotFound`, par exemple) **exécute le filtre**, donc le parcours de tas et le store de
> `sStaticRefusalMaxAlloc`, alors qu'il ne sera **jamais refusé**. La valeur journalisée
> n'est donc pas « la mesure du dernier refus » mais « la mesure de la dernière évaluation
> sous le seuil ».

### (b) 6ᵉ barreau de l'échelle [ADR-0058](0058-gardes-heap-metrique-8-bits.md) : `kStaticFileMinAllocBytes = 8192`

```
 2 048  kOomFallbackRearmMinMaxAllocBytes  réarmement réserve de repli
 4 096  kRestartMaxAllocHeapBytes          reboot d'auto-guérison
 6 144  kWsPushMinAllocBytes               pushes WS sautés
 8 192  kStaticFileMinAllocBytes           ← NOUVEAU : service de fichiers refusé (503)
12 288  kLowHeapWarnBytes                  WARN heap + flush disque
18 432  kOomRearmMinMaxAllocBytes          réarmement réserve nominale
```

**Les 5 barreaux existants sont INCHANGÉS** (condition C5 des gates B-1 et B-2, reconduite
et respectée ici).

Coût réel d'un service de fichier statique, poste par poste :

| Allocation | Taille | Échec = |
|---|---|---|
| `__sfp` → `xSemaphoreCreateRecursiveMutex` | ~88 o | **`abort()`** ← le PANIC du 29/08 |
| `__sfmoreglue` (pool de 4 `FILE` épuisé) | ~440 o | `NULL` propre |
| `esp_littlefs_open` + cache lfs | ~190 o | `NULL` propre |
| `new VFSFileImpl` + bloc `shared_ptr` | ~64 o | `bad_alloc` |
| `new AsyncFileResponse` (mesuré le 13/08) | 156 o | `bad_alloc` |
| `String` de chemin (×2-3) | ~100 o | — |
| tampon stdio `setvbuf` 4096 | **PSRAM** depuis B-1 | hors budget interne |
| pbuf lwip TCP, 1-3 en vol | 1 622 o pièce | retransmission |

- **Besoin dur** (chemin abort-prone) : ~**600 o**, plus gros bloc contigu exigé ~**440 o**.
- **Besoin sans bégaiement TCP** : 600 + 2 × 1 622 ≈ **3 850 o**.

`8192` vaut **×13,6** le besoin dur et **×2,1** le besoin complet. Face au terrain du 05/09 :
plateau calme post-B-2 **13 300 o** (+38 % au-dessus du seuil), plancher de rafale 6 requêtes
**14 324 o**, plancher absolu firmware **1 908 o** (là, la garde **doit** mordre).

Frontière **stricte** : `< seuil` refuse, `>= seuil` sert.

> 🔒 **Interdiction portée par cet ADR** : **ne jamais descendre ce seuil sous 6 144 o** pour
> « faire taire » la garde. En dessous, il devient indistinguable du barreau des pushes WS au
> diagnostic, et la marge tombe sous ×1,6 du besoin réel. Verrouillé à la **compilation** par
> deux `static_assert` ([`constants.h:276-281`](../../src/constants.h:276)) : l'un impose
> `kWsPushMinAllocBytes < kStaticFileMinAllocBytes < kLowHeapWarnBytes`, l'autre le plancher
> de 6 144. Une échelle mal ordonnée doit casser le **build**, pas un test.

Corollaire assumé : si la décroissance monotone du plateau reprend, **la garde deviendra
visible en régime calme**. C'est le **signal voulu**, pas un défaut à corriger en relevant le
seuil — un refus hors PANIC prouve que le lot C (cause de la fragmentation) est dû.

### (c) Le coût du parcours de tas est arbitré ACCEPTABLE, sans cache ni bornage

Le filtre appelle `heapLargestFreeBlock8Bit()` sur la tâche `async_tcp` à chaque GET
atteignant la garde : **70-250 µs par appel**, **1-3 ms par chargement de page**, soit
**≲ 5-10 %** du coût d'un service de fichier qui existe déjà.

Les garde-fous exigés en B-2 pour la sonde du chemin d'échec ne s'appliquent **pas** ici, et
il importe de dire **pourquoi**, plutôt que de les recopier :

- la **borne a priori** (condition C2 du gate B-2) est satisfaite **par construction**, pas
  par autolimitation : le masque est **figé** à `MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT`, donc
  le pool parcouru est fini et petit (**plafond pathologique ~800 µs**) — il n'y a pas de
  masque hostile possible, contrairement au hook d'échec où les caps sont ceux de l'appelant ;
- l'**autolimitation** (condition C3) est **sans objet** : elle existait parce que le hook
  d'échec est déclenchable **par rafales depuis une ISR**. Ici, la fréquence d'appel est
  bornée par l'**arrivée de requêtes TCP** — il n'y a ni ISR, ni rafale auto-entretenue.

**Un cache de la mesure est refusé fermement.** Le faux négatif d'une garde anti-crash n'est
pas une ligne de journal imprécise : c'est un **`abort()`**. Et
[`heap_metrics.h:133-135`](../../src/heap_metrics.h:133) documente des oscillations de
**~45 Ko entre deux échantillons distants de 2 s** : une valeur mise en cache serait
exactement le genre de mesure « fraîche à 2 s près » que ce chantier a déjà écartée deux
fois. Chaque garde mesure **au moment où elle décide**.

> ⚠️ **Couplage consigné, à ne pas découvrir plus tard** : le coût d'un parcours TLSF
> **croît avec la fragmentation** (il est en O(blocs libres)). Il est donc **maximal
> exactement dans le régime pour lequel la garde existe**. Il reste borné (le pool est fini),
> mais ce couplage est réel et doit être présent à l'esprit de qui voudra ajouter un second
> point de mesure sur ce chemin.

### (d) Journal borné à DEUX lignes par épisode, WARN/INFO exclusivement

> 🔒 **Interdit général découvert par ce lot** — il ne concerne pas que B-3.
> [`logger.cpp:71-73`](../../src/logger.cpp:71) flushe **en synchrone, dans la tâche
> appelante**, sur `ERROR` et `CRITICAL` → `_persistFs->open("/system.log", "a")`
> ([`logger.cpp:276-277`](../../src/logger.cpp:276)) → **`fopen`**.
> **Un log `ERROR` émis depuis un chemin anti-`fopen` déclencherait l'`abort()` que ce chemin
> prévient.** Tout code écrit pour éviter `fopen` en famine est donc contraint à
> **WARN / INFO**, et à **ne jamais journaliser depuis `async_tcp`**.

Répartition qui en découle :

- **callback `async_tcp`** : `fetch_add` atomique + `send`. **Zéro `String`, zéro log, zéro
  allocation hors la réponse** (littéraux en `.rodata`) ;
- **loop** (`WebServerManager::drainLowHeapGuard()`, appelée par `update()`) : drain du
  compteur, **2 lignes maximum par épisode** —
  - front montant → `WARN Service fichiers suspendu (503) — maxBloc8=X o < 8192 o`
  - front descendant → `INFO Service fichiers rétabli — N refus en T s`

La fermeture d'épisode exige **deux** conditions cumulées : `kHttpLowHeapEpisodeQuietMs`
(**30 000 ms**) sans nouveau refus **et** tas revenu `>= 8192`.

`kHttpLowHeapEpisodeQuietMs` est **d'abord une garde de coût**, pas un confort de lecture :
`httpLowHeapNeedsHeapSample()` n'autorise la loop à parcourir le tas que si un épisode est
ouvert **et** ce délai écoulé. Sans elle, `update()` parcourrait le tas **à la fréquence de
la loop pendant toute la famine** — au pire moment possible, et pour bien plus cher que le
filtre HTTP dont la fréquence, elle, est bornée par le trafic. C'est **ensuite** un anti-spam :
un tas oscillant autour du seuil produirait sinon une paire de lignes par oscillation, soit
la sur-journalisation qui a détruit la fenêtre forensique du 15/08 (journal persistant de
8 Ko, rétention 25-35 min).

`Retry-After: 5` (56 o de flash, conservé) : sous le débounce d'auto-guérison (~15 s), au-dessus
de l'échelle d'une rafale de chargement de page — c'est ce qui empêche un navigateur ou Home
Assistant de marteler l'appareil pendant la famine même qu'on essaie de laisser se résorber.

## Alternatives considérées

- **Option A — filtre sur `serveStatic`** (rejetée, **deux fois disqualifiée**) :
  `canHandle` touche déjà le FS (`WebHandlers.cpp:105-107`), donc le filtre arrive **après**
  `fopen` ; et le filtre `serveStatic` rejette les `.html`, donc il n'aurait **jamais couvert
  les 7 routes HTML** — c'est-à-dire précisément le chemin du PANIC du 29/08.
- **Option B — helper `heapAllowsFileServe()` appelé en tête des 7 routes HTML** (rejetée) :
  ne couvre pas `serveStatic`, coûte 7 sites d'appel (+150-250 o), et **s'oublie** au 8ᵉ.
  Un point de code unique vaut sept disciplines.
- **Option C — drapeau global armé par la loop + `onNotFound`** (rejetée) : état global
  fragile en multi-connexions (la décision serait prise à un instant différent de la
  requête), et `onNotFound` sert déjà les routes pompes dynamiques.
- **Option D — répondre depuis le filtre lui-même** (rejetée) : heurte le cycle de vie de
  `_response` dans ESPAsyncWebServer.
- **Option E — liste d'exemption explicite** (rejetée) : une liste à maintenir se désynchronise
  du code ; l'ordre d'enregistrement, lui, est vérifiable en lisant `setupRoutes()` de haut
  en bas.
- **Option F — garde sur le libre TOTAL** (rejetée) : **aveugle à ce crash**. Terrain du
  05/09, t = 5474 s : `libre8 = 58 424 o` (« plein ») alors que `maxBloc8 = 15 860 o` et que
  11 allocations échouaient. Le mode de défaillance est l'échec d'**une** allocation de ~88 o
  dans un tas **fragmenté**, pas l'épuisement.
- **Option G — seuil à 6 144** (rejetée) : identique au barreau des pushes WS, donc
  **ambigu au diagnostic** (un même chiffre pour deux effets), et marge ×1,6 seulement.
- **Option H — seuil à 12 288** (rejetée) : à **1 012 o** du plateau calme mesuré — l'UI
  serait perdue sur une mauvaise minute, sans qu'aucun crash ne menace.
- **Option I — mettre la mesure en cache** (rejetée fermement) : cf. décision (c). ~45 Ko
  d'oscillation entre deux échantillons de 2 s, et un faux négatif vaut un `abort()`.
- **Option J — compter sur le parachute OOM** (rejetée, **impossible**) : le chemin passe par
  `malloc` (C), le parachute est un `new_handler`. Il n'est pas insuffisant, il est
  **structurellement absent** de ce chemin.

## Conséquences

### Positives

- Le seul chemin de crash **sans code de retour à tester** est fermé en amont : l'appareil
  répond **503** au lieu d'`abort()`.
- **Un point de code unique** couvre les deux familles (7 routes HTML explicites +
  `serveStatic`) ; aucune liste, aucune discipline à répéter.
- Le premier refus hors PANIC devient un **signal de diagnostic daté** : il prouve que la
  fragmentation a franchi un palier, et il est journalisé **2 lignes par épisode**, donc sans
  détruire la fenêtre forensique.
- Les routes de diagnostic (`/get-logs`, `/get-system-info`, `/data`, `/get-config`), le
  **WebSocket** et **MQTT** restent servis pendant la famine : l'appareil reste
  **observable** au moment précis où on a besoin de l'observer.
- L'**interdit `ERROR`/`CRITICAL` dans un chemin anti-`fopen`** est désormais écrit, et
  généralisable au-delà de ce lot.

### Négatives / dette assumée

- ⚠️ **Il existe une bande de verrouillage STABLE : `4 096 ≤ maxBloc8 < 8 192`.**
  Il aurait été confortable d'écrire « on ne peut pas rester verrouillé dehors durablement,
  puisque l'auto-guérison reboote ». **C'est faux, et il ne faut pas le recopier** :
  l'auto-guérison mord à **4 096**. Dans la bande ci-dessus, **toute l'interface web est
  morte et aucun reboot ne survient** — le régime est stable. Vu la décroissance monotone du
  plateau (42 996 → 26 612 → 20 468 → 15 860 → 13 300), cette bande est **atteignable**.
  La conclusion opérationnelle ne change pas — mieux vaut un 503 qu'un `abort()` — mais la
  justification, elle, doit être honnête : ce projet s'est déjà fait piéger par une
  justification confortable et fausse ([ADR-0066](0066-sonde-du-tas-au-moment-du-refus.md),
  interdit « deadlock »).
- **En famine, `/wifi.html` et `/` sont refusés** : le **portail AP de secours** et la page
  **OTA** deviennent inaccessibles **par le navigateur**. Les chemins de secours réels
  survivent : `POST /update` est **hors matcher** (méthode POST), `./deploy.sh ota-firmware`
  fonctionne, et les routes de diagnostic répondent. C'est une conséquence **assumée**, pas
  un oubli.
- **Un WARN peut rester « orphelin » de son INFO** si la famine persiste **sans visiteur** :
  le compteur cesse de bouger, mais le tas n'est pas rétabli, donc l'épisode ne se ferme pas.
  Ce n'est **pas un défaut** — c'est l'énoncé vrai : l'épisode est réellement en cours. Le
  seul WARN réellement orphelin est celui **interrompu par un reboot** ; lors d'un
  post-mortem, **une bannière de démarrage clôt un épisode au même titre qu'un INFO**.
- Une requête **POST** qui remonte jusqu'à la position de la garde **paie le parcours de tas**
  sans jamais être refusée (le filtre précède le contrôle de méthode), et écrit
  `sStaticRefusalMaxAlloc`. Coût arbitré, effet de bord documenté à la déclaration de
  l'atomique.
- Le coût du parcours **croît avec la fragmentation** — maximal dans le régime même où la
  garde sert (cf. décision (c)).
- **`Logger::flushToDisk()` ([`logger.cpp:276-277`](../../src/logger.cpp:276)) reste un
  second `fopen` abort-prone**, non couvrable par un 503 (il n'y a pas de requête à refuser).
  Identifié, **hors périmètre**, lot ultérieur.
- **Coût flash** : 1 824 525 → **1 825 717 o** (**+1 192**), marge **9 291 o** — **99,5 %
  occupés**. La cible annoncée au plan était de **600 o** : le **dépassement est assumé et
  consigné**, les trims restants valaient ~90 o et **dégradaient la garde**. Le budget flash
  est le facteur limitant du prochain lot.
- Les atomiques sont en `memory_order_relaxed` : au sens strict du modèle mémoire, la loop
  pourrait voir l'incrément sans le `maxAlloc8` apparié. Le pire cas est une ligne de journal
  affichant la mesure d'une évaluation voisine — **jamais une décision fausse**, le prédicat
  de refus ne lisant aucune de ces deux variables.

### Ce que ça verrouille

- **La position d'enregistrement du handler** (après `wsManager.begin()`, avant
  `/login.html`) **est** la politique d'exemption. La déplacer change silencieusement la liste
  des routes protégées **et** la liste des routes servies en famine.
- **Toute route `HTTP_GET` ajoutée après ce point sera couverte par la garde.** Une route GET
  qui doit rester servie en famine (diagnostic, secours) **doit** être enregistrée **avant**,
  avec les routes JSON.
- **`ERROR` et `CRITICAL` sont interdits dans tout chemin écrit pour éviter `fopen`** —
  interdit général, pas particularité de B-3.
- **`kStaticFileMinAllocBytes >= 6144`**, verrouillé par `static_assert` : le build casse.
- **Une garde anti-crash mesure au moment où elle décide.** Pas de cache, pas de valeur
  d'anneau, pas d'échantillon périmé — règle déjà posée par
  [ADR-0058](0058-gardes-heap-metrique-8-bits.md), ici reconduite sur un chemin où le faux
  négatif vaut un `abort()`.
- **L'échelle ADR-0058 est un tout ordonné à SIX barreaux.** Aucun ne se modifie isolément.

## Références

- Code : [`src/web_server.cpp`](../../src/web_server.cpp) (handler prioritaire, filtre,
  `drainLowHeapGuard()`), [`src/web_server.h`](../../src/web_server.h),
  [`src/http_lowheap_logic.h`](../../src/http_lowheap_logic.h) (décision pure, ADR-0017),
  [`src/constants.h`](../../src/constants.h) (`kStaticFileMinAllocBytes`,
  `kHttpLowHeapEpisodeQuietMs`, bloc « ÉCHELLE COMPLÈTE », 2 `static_assert`)
- Tests : `test/test_native_http_lowheap_guard/` — **33 suites / 1 286 cas**, **7 mutants
  tués sur 7**
- Spec : `specs/features/done/bug-080-redemarrage-panic-et-fragmentation-heap.md` (lot B-3)
- Doc : [`docs/subsystems/web-server.md`](../subsystems/web-server.md),
  [`docs/subsystems/logger.md`](../subsystems/logger.md),
  [`docs/API.md`](../API.md#codes-derreur)
- ADR liés : [ADR-0058](0058-gardes-heap-metrique-8-bits.md) (échelle 8 bits — **6ᵉ barreau
  ajouté ici, les 5 existants inchangés**),
  [ADR-0061](0061-bascule-psram-au-dela-de-4095-octets-reserve-parachute-en-interne.md)
  (lot B-1), [ADR-0066](0066-sonde-du-tas-au-moment-du-refus.md) (lot B-2),
  [ADR-0035](0035-parachute-oom-new-handler.md) (parachute — **inopérant sur ce chemin**),
  [ADR-0032](0032-reponses-http-bornees.md) (réponses HTTP bornées),
  [ADR-0017](0017-logique-metier-pure-humble-object-testabilite.md) (logique pure)
