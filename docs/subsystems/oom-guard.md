# Subsystem — `oom-guard` (parachute OOM global)

- **Fichiers** : [`src/oom_guard_logic.h`](../../src/oom_guard_logic.h) (logique pure, header-only), [`src/oom_guard.h`](../../src/oom_guard.h) / [`src/oom_guard.cpp`](../../src/oom_guard.cpp) (coquille Arduino), [`src/web_routes_ota.cpp`](../../src/web_routes_ota.cpp) (garde bicritère `/check-update`)
- **Pas de singleton** : trois fonctions libres (`initOomGuard()`, `oomGuardLoopTick()`, `oomGuardReserveArmed()`) + fonctions pures inline.
- **Décisions architecturales** : [ADR-0035](../adr/0035-parachute-oom-new-handler.md) (parachute OOM), [ADR-0017](../adr/0017-logique-metier-pure-humble-object-testabilite.md) (logique pure testable), [ADR-0029](../adr/0029-auto-guerison-heap.md) (auto-guérison heap = containment de fond).

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

### Garde bicritère TLS partagée — `oomTlsGateOk`

Prédicat pur **partagé** avec `/check-update` ([`src/web_routes_ota.cpp`](../../src/web_routes_ota.cpp)) : une session TLS GitHub coûte ~58 Ko répartis → refus si heap libre < `kCheckUpdateMinFreeHeapBytes` (65 536 o) **OU** maxBloc < `kCheckUpdateMinMaxAllocBytes` (20 480 o) → **503** `{"error":"Mémoire insuffisante pour TLS, réessayez"}` + WARN avec les deux mesures et l'**IP client**. Remplace l'échec lent et aléatoire en plein handshake (mesuré le 11/08 : échec BIGNUM en 0,14 s sur heap dégradé). Voir [API.md](../API.md#get-check-update--write).

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
| `kCheckUpdateMinFreeHeapBytes` | `65536` (64 Ko) | Garde TLS : heap libre total minimal |
| `kCheckUpdateMinMaxAllocBytes` | `20480` (20 Ko) | Garde TLS : plus gros bloc contigu minimal |

> **Note WROVER/PSRAM** : sur module PSRAM (WROVER-E), les buffers mbedTLS > 4 Ko partent automatiquement en PSRAM (`SPIRAM_USE_MALLOC`, seuil 4096) — les deux seuils de garde TLS sont **conservateurs** et à réévaluer si la production migre (cf. [ADR-0034](../adr/0034-migration-core3-idf5.md)).

## Tests

18 tests natifs dédiés (`test/test_native_oom_guard/`) : machine à états du handler (libération / désenregistrement), hystérésis et critère pointeur du réarmement (course handler/réarmement incluse), one-shot du log par épisode, garde bicritère `oomTlsGateOk` (frontières des deux seuils) — **459 tests au total**.

## Fichiers liés

- [`src/main.cpp`](../../src/main.cpp) — `initOomGuard()` en tête de `setup()`, `oomGuardLoopTick()` dans `loop()`, log de boot conditionnel
- [system-health.md](system-health.md) — auto-guérison heap (containment de fond) + ring de télémétrie (cinétique pré-crash)
- [ADR-0035](../adr/0035-parachute-oom-new-handler.md) — décision et alternatives écartées
