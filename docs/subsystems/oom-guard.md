# Subsystem — `oom-guard` (parachute OOM global)

- **Fichiers** : [`src/oom_guard_logic.h`](../../src/oom_guard_logic.h) (logique pure, header-only), [`src/oom_guard.h`](../../src/oom_guard.h) / [`src/oom_guard.cpp`](../../src/oom_guard.cpp) (coquille Arduino), [`src/web_routes_ota.cpp`](../../src/web_routes_ota.cpp) (garde TLS sortante partagée `/check-update` + `/download-update`)
- **Pas de singleton** : trois fonctions libres (`initOomGuard()`, `oomGuardLoopTick()`, `oomGuardReserveArmed()`) + fonctions pures inline.
- **Décisions architecturales** : [ADR-0035](../adr/0035-parachute-oom-new-handler.md) (parachute OOM), [ADR-0036](../adr/0036-psram-requise-mise-a-jour-en-ligne.md) (PSRAM libre requise pour toute session TLS sortante), [ADR-0017](../adr/0017-logique-metier-pure-humble-object-testabilite.md) (logique pure testable), [ADR-0029](../adr/0029-auto-guerison-heap.md) (auto-guérison heap = containment de fond).

## Rôle

Transformer une **famine heap ponctuelle** (échec d'un `operator new`) en épisode **absorbé et journalisé** au lieu d'un abort sec : sur core Arduino (exceptions C++ activées), un `new` qui échoue lève `std::bad_alloc` — non rattrapé dans une tâche (ex. `async_tcp`), c'est `std::terminate` → abort → reboot sans aucune trace applicative.

Déclencheur : crash de production du 2026-08-11 (v2.18.4) — `operator new(356)` (`new AsyncWebServerRequest`, fork ESP32Async) à l'**acceptation d'une connexion HTTP**, après un effondrement du heap de ~60 Ko en ~5 s dont la cause reste **non attribuée** (spec `bug-oom-accept-http-durcissement`). La garde `if (r == NULL)` de la lib est **morte** sur exceptions activées : `new` lance, ne retourne jamais `nullptr` — et la lib externe n'est pas patchable. Le parachute est **trigger-agnostique** : il protège quelle que soit l'allocation qui échoue.

## Fonctionnement

### Réserve d'urgence — 12 Ko (`kOomReserveBytes`)

`initOomGuard()` fait `malloc(12288)` **au plus tôt du `setup()`** (avant `Serial`/logger/FS : bloc contigu pris avant toute fragmentation), puis enregistre `std::set_new_handler(oomNewHandler)` et `heap_caps_register_failed_alloc_callback(oomFailedAllocHook)`. Aucun log à ce stade (logger non initialisé) — le `setup()` journalise l'armement juste après, conditionné par `oomGuardReserveArmed()` (INFO « Parachute OOM armé » ou WARN « armement différé en loop »).

Dimensionnement des 12 Ko :

- **≈ 34 allocations vitales** de la taille du crash de référence (12 288 / 356 o ≈ 34) — accepts HTTP, trames WS, `String` de logs : de quoi tenir la phase critique ;
- **1,5× le seuil d'auto-guérison** `kRestartMaxAllocHeapBytes` (8 192 o, [ADR-0029](../adr/0029-auto-guerison-heap.md)) : la libération redonne au système assez de marge pour que le watchdog heap (débounce ~15 s en cadence rapprochée) déclenche un **reboot propre** (pompes coupées, cumuls persistés, alerte MQTT) pendant la famine, au lieu d'un abort.

### Contrat du handler (`oomNewHandler`) — chemin d'urgence

Le new_handler est appelé par libstdc++ quand le `malloc` interne d'`operator new` échoue, puis **le `new` en cours est RE-TENTÉ** (contrat `std::set_new_handler`). Il peut être invoqué depuis **n'importe quelle tâche** : le chemin est réduit au strict minimum — **aucune allocation, aucun log, aucun mutex** (réentrance allocateur interdite), état partagé 100 % atomique (`seq_cst`, chemins froids).

1. **Extraction atomique** de la réserve (`exchange(nullptr)`) ;
2. réserve disponible → `free()` (le `new` re-tenté réussit) + compteur d'épisodes → état `Released` ;
3. réserve **déjà consommée** → `std::set_new_handler(nullptr)` (désenregistrement) → état `Depleted` : le `new` suivant lève `bad_alloc` = **statu quo ante** (comportement d'avant le parachute), le **coredump reste le filet ultime**.

Le log CRITICAL « **Réserve OOM consommée** (épisode n°N) » est émis en **DIFFÉRÉ** depuis la loop, **one-shot par épisode** (comparaison de compteurs monotones, `oomShouldLogRelease`), avec maxBloc/libre au moment du drain. Pas de `flushToDisk()` dans ce chemin (une écriture LittleFS en pleine famine aggraverait la pression) — la persistance est couverte par le flush one-shot de `checkHeapHealth()` (`kLowHeapWarnBytes`).

### Réarmement — hystérésis 32 Ko / 10 s

`oomGuardLoopTick()` (appelé depuis `loop()`) re-alloue la réserve à cadence lente (`kOomRearmCheckIntervalMs` = 10 s) quand le plus gros bloc contigu repasse au-dessus de `kOomRearmMinMaxAllocBytes` = 32 768 o — bien au-dessus de `kLowHeapWarnBytes` (20 Ko) pour ne pas osciller libération/réallocation autour du seuil de famine. Le ré-enregistrement du handler est inconditionnel (idempotent — couvre l'état `Depleted`), et le succès est journalisé en INFO (« Parachute OOM réarmé »).

**Critère de réarmement = ABSENCE DE RÉSERVE (pointeur nul), pas l'état** — durcissement de revue v2.18.5 : si le handler consomme la nouvelle réserve entre l'`exchange` du réarmement et le `store(Armed)`, l'état peut afficher `Armed` avec une réserve nulle ; fonder `oomShouldRearm` sur le pointeur garantit la convergence au tick suivant. L'`exchange` à la ré-allocation (libération d'une réserve résiduelle théorique) ferme le double-free/fuite en ceinture-bretelles.

### Machine à états (logique pure `oom_guard_logic.h`)

```
             new échoue, réserve dispo (exchange non nul)
 [ Armed ] ────────────────────────────────────────────► [ Released ]
     ▲          free(réserve) + épisode++ (le new re-tenté réussit)         │
     │                                                                      │
     │  oomShouldRearm : réserve ABSENTE ET maxBloc ≥ 32 Ko (tick 10 s)     │
     ├──────────────────────────────────────────────────────────────────────┤
     │                                                                      ▼
     │          new échoue, réserve déjà consommée                  [ Depleted ]
     └──────── (même chemin de réarmement) ◄──── set_new_handler(nullptr) :
                                                 bad_alloc suit son cours
                                                 (statu quo ante, coredump)
```

### Callback IDF diagnostic (`heap_caps_register_failed_alloc_callback`) — flag-seul

**Complément d'attribution** : appelé depuis l'**intérieur de l'allocateur** à CHAQUE échec d'allocation — y compris les **`malloc` C que le new_handler ne voit pas** (lwIP, mbedTLS, drivers). Mode **flag-seul** : le callback ne fait que poser taille demandée + compteur atomiques (pas de free, pas de log) ; la loop draine en WARN « Allocation heap échouée ×N (dernière taille demandée=…) » **throttlé** `kOomFailWarnThrottleMs` = 30 s.

### Garde TLS sortante partagée — `oomTlsGateReason` (v2.18.6)

Prédicat pur **partagé par les deux routes qui ouvrent une session TLS sortante** — `/check-update` **et** `/download-update` ([`src/web_routes_ota.cpp`](../../src/web_routes_ota.cpp), fonction commune `refuseIfTlsGateClosed`) : aucune comparaison de seuil dans les handlers.

Il retourne un **motif** (`uint8_t`) et non plus un booléen — l'appelant doit distinguer un refus **matériel définitif** d'un refus **temporaire réessayable** :

| Motif | Condition | Réponse HTTP (`code`) |
|---|---|---|
| `kOomTlsGateOk` | tous les critères passés | session TLS autorisée |
| `kOomTlsGateNoPsram` | PSRAM **libre** < `kOtaTlsMinPsramBytes` (1 Mo) | **503** — `psram_required` **ou** `psram_low` selon la nullité de la PSRAM libre (voir ci-dessous) |
| `kOomTlsGateLowMemory` | heap libre < `kCheckUpdateMinFreeHeapBytes` **OU** maxBloc < `kCheckUpdateMinMaxAllocBytes` | **503** `{"error":"Mémoire insuffisante pour TLS, réessayez","code":"tls_low_memory"}` |

#### Mapping motif → code HTTP (v2.18.7)

Le prédicat pur reste à **trois motifs** ; c'est la **route** (`refuseIfTlsGateClosed` dans [`src/web_routes_ota.cpp`](../../src/web_routes_ota.cpp)) qui scinde `kOomTlsGateNoPsram` en deux codes, à partir de la mesure de PSRAM libre **qu'elle a déjà en main** — un simple test de nullité, **aucun seuil dupliqué hors du prédicat** :

| Motif | Test dans la route | `code` renvoyé | Nature |
|---|---|---|---|
| `kOomTlsGateNoPsram` | `psramFreeBytes == 0` | `psram_required` | **Permanent** — module sans PSRAM (WROOM) : le front neutralise la carte de mise à jour en ligne |
| `kOomTlsGateNoPsram` | `psramFreeBytes > 0` | `psram_low` | **Temporaire** — PSRAM installée mais saturée (WROVER chargé) : réessayable, aucune neutralisation |
| `kOomTlsGateLowMemory` | — | `tls_low_memory` | **Temporaire** — heap interne dégradé : réessayable |

Pourquoi ici et pas dans le prédicat : la distinction « absente » / « saturée » n'est pas une **décision de garde** (le refus est identique dans les deux cas et se fait sur le même seuil), c'est une **information de présentation**. La garder dans la route évite d'introduire un quatrième motif et un second seuil dans la logique pure testée. Corollaire : le firmware ne doit jamais affirmer l'absence de PSRAM sur une PSRAM simplement saturée, et le front discrimine sur `code` — **jamais sur le texte** du champ `error`.

**La PSRAM est évaluée EN PREMIER, fail-closed** ([ADR-0036](../adr/0036-psram-requise-mise-a-jour-en-ligne.md)) : mbedTLS réclame plusieurs blocs contigus d'environ 16 Ko. La mesure terrain du 12/08 (v2.18.5, WROOM) a montré que le bicritère heap seul laissait passer le régime **normal** d'un WROOM (free=87 708, maxBloc=34 804), après quoi le handshake affamait le tas jusqu'à refuser une allocation de **100 o** — la fenêtre exacte du crash `bad_alloc` du 11/08. Relever le seuil maxBloc à 48-50 Ko n'aurait rien protégé : la réserve de 12 Ko du parachute fait tomber le maxBloc nominal d'un WROOM de ~49 à ~37 Ko, donc un tel seuil ne passerait **que parachute désarmé**.

**Critère = PSRAM LIBRE** (`ESP.getFreePsram()` mesurée par la route et passée en paramètre — le header pur ne lit aucun `ESP.*`), **pas la taille installée** (`getPsramSize()`, constante et jamais décroissante) : une PSRAM installée mais **saturée** ferait retomber mbedTLS sur la RAM interne, exactement le régime de crash. Exiger 1 Mo libre couvre à la fois l'absence de PSRAM et sa saturation.

Dans `/download-update`, la garde est placée **après** la validation url/allowlist/digest et **avant** `WiFiClientSecure`, `setOtaInProgress()`, `LittleFS.end()` et tout effacement de partition : un refus n'a **aucun effet de bord**.

Chaque refus émet un WARN auto-explicatif : motif, PSRAM libre, les deux mesures heap, les trois seuils et l'**IP client**. Voir [API.md](../API.md#garde-tls-sortante-psram-libre--heap-v2186).

> Le **texte** du refus `kOomTlsGateNoPsram` a été reformulé en v2.18.7 (destiné à l'utilisateur, vocabulaire aligné sur la carte « Mise à jour manuelle » de l'interface) et le refus s'y scinde en deux codes (`psram_required` / `psram_low`) ; le refus heap porte désormais `code: "tls_low_memory"`. Les **seuils**, l'**ordre** d'évaluation et la **logique de garde** sont **inchangés** — le prédicat pur n'a pas bougé. Les WARN/INFO restent techniques (un seul libellé « PSRAM libre absente/insuffisante » pour les deux codes PSRAM) : ce sont des messages de diagnostic, ils ne suivent ni les reformulations du champ `error` ni la scission des codes.

## Limites assumées

- **Le parachute n'est PAS une solution à la famine** : il absorbe l'allocation qui aurait aborté et achète du temps pour un diagnostic + reboot propre. Le **containment de fond reste l'auto-guérison heap** ([ADR-0029](../adr/0029-auto-guerison-heap.md)) : si la famine persiste, `checkHeapHealth()` (débounce ~15 s) déclenche `performFailSafeRestart()`.
- **Seuls les `new` C++ sont couverts** par la re-tentative : les `malloc` internes (lwIP, mbedTLS, drivers) échouent toujours en `NULL` — le callback IDF ne fait que les **compter** (diagnostic).
- **Réserve épuisée** = retour exact au comportement d'avant le parachute (`bad_alloc` → coredump) — jamais pire.
- La cause du pic de −60 Ko du 11/08 reste **non attribuée** (candidat principal : gonflement des buffers RX du driver WiFi pendant une tempête/réassociation) — l'attribution attend la récidive instrumentée (IP client, WARN heap_caps, ring enrichi).

## Constantes ([`constants.h`](../../src/constants.h))

| Constante | Valeur | Rôle |
|---|---|---|
| `kOomReserveBytes` | `12288` (12 Ko) | Réserve d'urgence (≈ 34 allocs vitales de 356 o ; 1,5× le seuil auto-guérison 8 192 o) |
| `kOomRearmMinMaxAllocBytes` | `32768` (32 Ko) | Hystérésis : maxBloc minimal pour ré-allouer la réserve |
| `kOomRearmCheckIntervalMs` | `10000` (10 s) | Cadence du contrôle de réarmement dans la loop |
| `kOomFailWarnThrottleMs` | `30000` (30 s) | Max 1 WARN « allocation heap échouée » par fenêtre |
| `kOtaTlsMinPsramBytes` | `1048576` (1 Mo) | Garde TLS : **PSRAM libre** minimale (v2.18.6, critère prioritaire fail-closed) |
| `kCheckUpdateMinFreeHeapBytes` | `65536` (64 Ko) | Garde TLS : heap libre total minimal (**inchangé** en v2.18.6) |
| `kCheckUpdateMinMaxAllocBytes` | `20480` (20 Ko) | Garde TLS : plus gros bloc contigu minimal (**inchangé** en v2.18.6) |

> **Note WROVER/PSRAM** : sur module PSRAM (WROVER-E), les buffers mbedTLS > 4 Ko partent automatiquement en PSRAM (`SPIRAM_USE_MALLOC`, seuil 4096) — les deux seuils **heap** restent dimensionnés pour la RAM interne d'un WROOM et deviennent conservateurs sur WROVER (cf. [ADR-0034](../adr/0034-migration-core3-idf5.md)). `kOtaTlsMinPsramBytes` = 1 Mo n'a **jamais été mesuré sur WROVER réel** : valeur volontairement large, **à re-mesurer à la mise en service** du premier module PSRAM.

## Tests

25 tests natifs dédiés (`test/test_native_oom_guard/`, +7 en v2.18.6) : machine à états du handler (libération / désenregistrement), hystérésis et critère pointeur du réarmement (course handler/réarmement incluse), one-shot du log par épisode, garde `oomTlsGateReason` — motifs et **priorité PSRAM sur heap** (PSRAM absente avec heap sain → `kOomTlsGateNoPsram`), PSRAM saturée, frontières inclusives des trois seuils — **466 tests au total**.

## Fichiers liés

- [`src/main.cpp`](../../src/main.cpp) — `initOomGuard()` en tête de `setup()`, `oomGuardLoopTick()` dans `loop()`, log de boot conditionnel
- [system-health.md](system-health.md) — auto-guérison heap (containment de fond) + ring de télémétrie (cinétique pré-crash)
- [ADR-0035](../adr/0035-parachute-oom-new-handler.md) — décision et alternatives écartées (parachute)
- [ADR-0036](../adr/0036-psram-requise-mise-a-jour-en-ligne.md) — PSRAM libre requise pour toute session TLS sortante (alternatives écartées : seuil maxBloc relevé, tentative rattrapée, `/download-update` non gardée)
