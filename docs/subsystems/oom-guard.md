# Subsystem — `oom-guard` (parachute OOM global)

- **Fichiers** : [`src/oom_guard_logic.h`](../../src/oom_guard_logic.h) (logique pure, header-only), [`src/oom_guard.h`](../../src/oom_guard.h) / [`src/oom_guard.cpp`](../../src/oom_guard.cpp) (coquille Arduino), [`src/heap_metrics.h`](../../src/heap_metrics.h) / [`src/heap_metrics.cpp`](../../src/heap_metrics.cpp) (mesures, dont `heapProbeForCaps()`), [`src/web_routes_ota.cpp`](../../src/web_routes_ota.cpp) (garde TLS sortante partagée `/check-update` + `/download-update`), [`src/mbedtls_psram_alloc.h`](../../src/mbedtls_psram_alloc.h) / [`.cpp`](../../src/mbedtls_psram_alloc.cpp) (bug-165/bug-108, v2.81.0 — mbedtls obéit à la politique PSRAM du projet)
- **Pas de singleton** : fonctions libres (`initOomGuard()`, `oomGuardLoopTick()`, `oomGuardReserveArmed()`, `oomGuardReserveArmedBytes()`, `oomGuardReserveIsInternalRam()`, `oomGuardConsumeNoReserveAlert()`, `oomGuardHandlerRegistered()`, `oomGuardNoteHeapSample()`, `oomGuardProbeStats()`, `oomPostMortemDumpIfBoot()`, `oomGuardIntegrityTick()`, `oomGuardNoteIntegrityResult()`) + fonctions pures inline.
- **Décisions architecturales** : [ADR-0035](../adr/0035-parachute-oom-new-handler.md) (parachute OOM), [ADR-0037](../adr/0037-postmortem-oom-et-integrite-tas.md) (post-mortem d'allocation RTC + contrôle d'intégrité du tas, v2.18.8), [ADR-0038](../adr/0038-metrique-memoire-allouable-8-bits.md) (**mesure 8 bits = mémoire réellement allouable**, v2.19.0), [ADR-0058](../adr/0058-gardes-heap-metrique-8-bits.md) (**bascule des 4 gardes sur la métrique 8 bits, échelle unique**, v2.33.0 — supersede le régime bi-métrique d'[ADR-0040](../adr/0040-pas-de-calibrage-sur-fragmentation-pathologique.md)), [ADR-0061](../adr/0061-bascule-psram-au-dela-de-4095-octets-reserve-parachute-en-interne.md) (**bascule PSRAM au-delà de 4095 o + réserve du parachute en RAM interne explicite**, v2.35.0), [ADR-0036](../adr/0036-psram-requise-mise-a-jour-en-ligne.md) (PSRAM libre requise pour toute session TLS sortante), [ADR-0017](../adr/0017-logique-metier-pure-humble-object-testabilite.md) (logique pure testable), [ADR-0066](../adr/0066-sonde-du-tas-au-moment-du-refus.md) (**sonde du tas au moment exact du refus : requalification de l'interdit `heap_caps`, double mesure, verdict `FAMINE DMA`**, bug-080 B-2), [ADR-0029](../adr/0029-auto-guerison-heap.md) (auto-guérison heap = containment de fond).

> ✅ **v2.33.0 — fin du régime bi-métrique ([ADR-0058](../adr/0058-gardes-heap-metrique-8-bits.md)).** Depuis bug-080 lot A, **toutes les gardes décisionnelles heap** lisent `heapLargestFreeBlock8Bit()` ([`heap_metrics.h`](../../src/heap_metrics.h)) : paliers de réarmement du parachute, auto-guérison (`kRestartMaxAllocHeapBytes` = 4 096), WARN (`kLowHeapWarnBytes` = 12 288), skip des pushes WS (`kWsPushMinAllocBytes` = 6 144) et garde 503 de `/get-logs`. Le NO GO d'[ADR-0040](../adr/0040-pas-de-calibrage-sur-fragmentation-pathologique.md) (v2.20.2) est **révoqué** par le gate `pool-chemistry` du 02/09/2026 : les données nouvelles montrent un nominal fragmenté **stable à 21 492 o** (2.26→2.32) et des creux transitoires de quelques secondes seulement — **l'immunité aux faux positifs vient du débounce temporel** (3 mesures consécutives, famine continue ≥ 10-15 s), pas du niveau du seuil. Restait en métrique **brute** : `web_routes_debug.cpp` (503 trace capteurs) — **basculé en bug-080 lot B-2** (voir ci-dessous). ✅ **Dette SOLDÉE en v2.81.0** (bug-108, absorbé par bug-165) : `web_routes_ota.cpp` (garde TLS) bascule à son tour sur `heapFree8Bit()`/`heapLargestFreeBlock8Bit()` — plus aucune garde décisionnelle du projet ne lit `ESP.getFreeHeap()`/`ESP.getMaxAllocHeap()`. Voir la section dédiée plus bas.

> ✅ **B-2 — la garde 503 de `GET /debug/sensor_trace` bascule à son tour.** Elle comparait `taille + kSensorTraceHeapMarginBytes` à `ESP.getMaxAllocHeap()`, qui compte l'**IRAM 32 bits inutilisable** par le `new` du cbuf d'`AsyncResponseStream`. Elle lit désormais `heapLargestFreeBlock8Bit()` — exactement ce que ce `new` peut obtenir. **Le seuil est INCHANGÉ : on corrige la mesure, pas la marge.** Écart quasi nul en régime normal (**27 636 vs 26 612 o**, soit 1 024 o mesurés le 05/09), mais la brute **reste figée en famine** pendant que le 8 bits s'effondre (plancher relevé sous rafale d'UI : **14 324 o**, et le firmware a vu **1 908 o**) : la garde cessait alors de protéger, tout en paraissant fonctionner.

## Rôle

Transformer une **famine heap ponctuelle** (échec d'un `operator new`) en épisode **absorbé et journalisé** au lieu d'un abort sec : sur core Arduino (exceptions C++ activées), un `new` qui échoue lève `std::bad_alloc` — non rattrapé dans une tâche (ex. `async_tcp`), c'est `std::terminate` → abort → reboot sans aucune trace applicative.

Déclencheur : crash de production du 2026-08-11 (v2.18.4) — `operator new(356)` (`new AsyncWebServerRequest`, fork ESP32Async) à l'**acceptation d'une connexion HTTP**, après un effondrement du heap de ~60 Ko en ~5 s dont la cause reste **non attribuée** (spec `bug-oom-accept-http-durcissement`). La garde `if (r == NULL)` de la lib est **morte** sur exceptions activées : `new` lance, ne retourne jamais `nullptr` — et la lib externe n'est pas patchable. Le parachute est **trigger-agnostique** : il protège quelle que soit l'allocation qui échoue.

## Politique d'allocation par défaut — bascule PSRAM (v2.35.0)

> 🔎 **bug-080 lot B-1, [ADR-0061](../adr/0061-bascule-psram-au-dela-de-4095-octets-reserve-parachute-en-interne.md).** Depuis la v2.35.0, `setup()` appelle **une seule fois**, juste après `initOomGuard()` et sous garde `psramFound()` :
>
> ```cpp
> heap_caps_malloc_extmem_enable(kPsramExtmemThresholdBytes);  // 4095 jusqu'en v2.49.3, **1024** depuis la v2.50.0
> ```
>
> Toute allocation **par défaut** (`malloc`, `new`, `calloc`, `realloc`, `strdup`) **dépassant le seuil** est tentée **en PSRAM d'abord**. Le seuil valait **4 095 o** de la v2.35.0 à la v2.49.3 ; il vaut **1 024 o** depuis la **v2.50.0** ([ADR-0078](../adr/0078-bascule-psram-cesse-d-etre-chirurgicale.md), feature-124) — voir [system-health.md](system-health.md#politique-de-bascule-psram-des-allocations-ordinaires-v2500-feature-124--s7) pour l'encadrement de cette valeur.

### Pourquoi : le tampon stdio de chaque fichier ouvert restait interne à un octet près

`vfs_api.cpp:22` définit `DEFAULT_FILE_BUFFER_SIZE 4096` et la ligne `:250` attache ce tampon par `setvbuf(_f, NULL, _IOFBF, …)` à **chaque** fichier LittleFS ouvert. `VFSImpl::exists()` **ouvre réellement** le fichier, et `AsyncStaticWebHandler::_searchFile` enchaîne `exists()` **puis** `open()` : **2 × (4096 o + ~90 o de mutex newlib) par fichier statique servi**, soit jusqu'à **~49 Ko de RAM interne 8 bits en rafale** par chargement d'interface (≈ 8 requêtes, jusqu'à 6 en parallèle) — sur un `free8` mesuré à 42-59 Ko.

Le seuil IDF des libs précompilées vaut `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=4096` et son test est **inclusif** (`taille <= seuil ⇒ RAM interne`, désassemblage de `libheap.a`) : un tampon de **exactement 4096 o** restait donc **interne**. **Il s'en fallait d'un octet.** 4095 était chirurgical — la seule classe d'allocations qui changeait de camp était « exactement 4096 o » ; tout ce qui est plus grand partait **déjà** en PSRAM, tout ce qui faisait 4095 o ou moins restait interne.

> ⚠️ **Cette doctrine « chirurgicale » est RÉVISÉE depuis la v2.50.0** ([ADR-0078](../adr/0078-bascule-psram-cesse-d-etre-chirurgicale.md)). Le seuil descend à **1 024 o** pour déporter aussi les **pbufs lwIP** (~1 600 o, une structure par connexion TCP) : le plancher fin mesuré le 09/09/2026 (**1 076 o**) passe **sous** le tampon TX WiFi de 1 622 o, donc le tas interne 8 bits est la ressource rare. ⚠️ Le lot ne déplace **aucun tampon WiFi** — ces allocations demandent `caps=0x080C` **explicitement** et contournent le seuil : il leur **fait de la place**.

C'est le déclencheur **commun** des deux PANIC (29/08 et 03/09), tous deux survenus sur un chargement d'interface web.

> **Fail-safe** : la bascule est une **préférence**, pas une contrainte. L'allocateur tente les capacités choisies, **se replie sur `MALLOC_CAP_DEFAULT`** si la PSRAM refuse, et n'appelle `heap_caps_alloc_failed` qu'**après** ce repli — **aucun échec nouveau n'est créé** et les **compteurs OOM ne sont pas pollués** par une tentative PSRAM infructueuse.

> ⚠️ **Appel UNIQUE au setup, jamais à l'exécution.** `heap_caps_malloc_extmem_enable` écrit une **variable globale de l'allocateur**, relue par **chaque** `malloc()` du système. L'appeler depuis une route, un tick ou un handler changerait la politique d'allocation de **toutes les tâches** sans qu'on puisse raisonner sur les allocations en vol.

### Les trois sites de la réserve passent par un entonnoir `MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT`

`kOomReserveBytes` vaut **12 288 o**, donc **plus** que 4095 : un `malloc()` ordinaire enverrait la réserve du parachute **en PSRAM**. Or **libérer 12 Ko de PSRAM pendant une famine de RAM interne ne sauve rien** — tout en journalisant « palier nominal » de façon parfaitement crédible.

> ⚠️ **Hypothèse forte, à confirmer : la réserve part probablement en PSRAM DEPUIS l'activation de la PSRAM (feature-066), sans attendre ce lot** — `malloc(12288)` dépasse déjà le seuil de 4096 aujourd'hui. Le coredump du 03/09 ne capture aucune variable globale (seules les piles de tâches) : il ne permet pas de le prouver. **C'est la ligne d'armement du prochain boot qui tranchera.** Ce lot ne crée pas ce défaut : il le **révèle** et le corrige.
>
> **Incohérence adjacente, désormais fermée** : le réarmement mesure **déjà** `heapLargestFreeBlock8Bit()` (RAM interne) et exige 18 432 o **avant** d'appeler l'allocation — la garde vérifiait l'interne puis allouait peut-être de l'externe. Le correctif rend **l'allocation conforme à la garde qui la gouverne**.

Les **trois** sites d'allocation de la réserve — **nominale** (12 288 o, `initOomGuard`), **de repli** (1 024 o, `initOomGuard`) et **réarmement** (`oomGuardLoopTick`, le plus important : il s'exécute **après** la bascule du setup) — passent donc par un entonnoir unique :

```cpp
static void* oomAllocReserveInternal(size_t bytes) {
  return heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}
```

- `MALLOC_CAP_INTERNAL` : jamais de PSRAM, **quelle que soit la politique globale** ;
- `MALLOC_CAP_8BIT` : mémoire adressable octet par octet — la seule que `malloc`/`new` ordinaires savent servir, et exactement celle dont la famine nous prive (l'IRAM 32 bits gonflait la métrique historique, [ADR-0038](../adr/0038-metrique-memoire-allouable-8-bits.md)).

### `#pragma GCC poison malloc calloc realloc strdup`

Placé dans `oom_guard.cpp` **après tous les `#include`** (un header du core qui déclare `malloc` casserait le build sinon), il rend l'erreur **impossible à recommettre** : tout retour d'une allocation par défaut dans cette unité de compilation **casse la compilation**.

- `calloc` / `realloc` / `strdup` sont empoisonnés **aussi** (demande `test-engineer` + revue) : ils passent par le **même** allocateur par défaut et contourneraient l'entonnoir tout aussi bien — `calloc(1, target)` compilait sans broncher avant ce durcissement ;
- **`free` n'est PAS empoisonné** : la réserve est libérée par `free()` — équivalent à `heap_caps_free` sur un pointeur issu de `heap_caps_malloc` — y compris depuis le `new_handler`, où l'on ne veut **aucun appel non trivial** ;
- `new` est un **mot-clé**, non empoisonnable : assumé (aucun `new` dans ce fichier).

Deux `static_assert` de [`constants.h`](../../src/constants.h) verrouillent le couple : `kPsramExtmemThresholdBytes + 1 == kVfsStdioBufferBytes` (si un futur core change `DEFAULT_FILE_BUFFER_SIZE`, le build casse — c'est voulu) et `kOomReserveBytes > kPsramExtmemThresholdBytes` (sentinelle : la réserve dépasse le seuil, elle **doit** donc être allouée en capacités explicites).

### L'invariant est OBSERVÉ, pas supposé

`oomGuardReserveIsInternalRam()` lit `esp_ptr_internal()` **sur le pointeur publié** (comparaison d'adresse *inline* : ni allocation, ni verrou, ni parcours de tas — appelable depuis la loop comme depuis un handler AsyncWebServer). Il retourne `false` s'il n'y a **aucune** réserve armée **ou** si elle a atterri en PSRAM. Il alimente les deux lignes de journal ci-dessous et le champ `oom_reserve_internal` de [`/get-system-info`](../API.md#get-get-system-info--write).

### Logs modifiés ou ajoutés en v2.35.0

| Ligne | Niveau | Nouveauté |
|---|---|---|
| Message PSRAM du boot | INFO | **Réécrit en octets**, avec sa direction |
| Armement du parachute (boot) | INFO | Mention de l'**emplacement** : `RAM interne` / `PSRAM (ANOMALIE)` |
| Réarmement / surclassement | INFO | Idem — seul chemin d'allocation **postérieur** à la bascule, donc le seul qui pourrait dériver |
| Parachute **DÉCLASSÉ** | WARN | **Nouveau**, one-shot : le palier nominal a été refusé, le parachute tourne sur son repli |
| Parachute **SANS RÉSERVE** | CRITICAL | **Nouveau**, one-shot par épisode : aucun palier armable depuis `kOomNoReserveCriticalDelayMs` |

```
PSRAM : 4 Mo détectés, 4076 Ko libres — allocations de PLUS de 1024 o tentées en PSRAM
  d'abord (repli RAM interne si refus) ; 1024 o et moins : RAM interne
PSRAM absente — RAM interne seule (module WROOM), politique d'allocation inchangée

Parachute OOM armé : réserve d'urgence de 12288 o (palier nominal) pré-allouée en
  RAM interne, maxBloc8=36852 o
Parachute OOM réarmé : réserve de 12288 o (palier nominal) allouée en RAM interne, maxBloc8=21492 o

Parachute OOM DÉCLASSÉ : palier de repli de 1024 o armé (nominal 12288 o refusé en RAM
  interne 8 bits), maxBloc8=3060 o — surclassement auto dès que le tas le permet
Parachute OOM SANS RÉSERVE depuis 5 min (aucun palier armable) — plus aucun filet
  anti-bad_alloc, maxBloc8=1268 o, libre8=42752 o
```

> **Pourquoi l'emplacement est journalisé et non déduit du code** (condition **C1** du gate `pool-chemistry`) : « la réserve vit en RAM interne » n'est plus une évidence de lecture depuis la bascule, c'est un **invariant à observer**. Sans cette mention, un parachute décoratif annoncerait « palier nominal » sans que rien ne le dénonce.
>
> **Pourquoi le message PSRAM du boot a été réécrit** : il disait « les allocations > 4 Ko y sont placées automatiquement ». C'était **faux**, et cet arrondi en kilo-octets a **masqué le défaut pendant trois cycles de diagnostic** — le test de l'allocateur étant inclusif, une allocation de 4096 o restait interne. Le seuil est désormais énoncé **en octets, avec sa direction**.

### Alerte MQTT `oom_reserve_absent` — le drapeau, pas la publication

Le CRITICAL « sans réserve » lève un drapeau, drainé par `mqtt_manager` **depuis sa propre tâche** (`oomGuardConsumeNoReserveAlert()`, drain one-shot) qui publie l'alerte [`oom_reserve_absent`](../MQTT.md#alertes).

`oom_guard` **ne publie pas lui-même**, pour trois raisons cumulées :

1. `publishAlert()` **alloue** (deux `String` au site d'appel, message, charge utile, JsonDocument) — or la précondition d'arrivée ici est que le tas a refusé 1 024 o **trente fois de suite** ;
2. `publishAlert()` se termine par un `systemLogger.warning()` qui écrirait le **même** message une **seconde** fois, dans un journal de 8 Ko dont la rétention est de 25-35 min : la fenêtre forensique de l'épisode documenté serait divisée par deux ;
3. `oom_guard` est une **feuille** (`constants` / `oom_guard_logic` / `heap_metrics` / `logger`) et `initOomGuard()` est le **tout premier** appel de `setup()` : le faire dépendre du sous-système applicatif le plus haut inverserait les couches.

**Conséquence assumée** : si MQTT est désactivé ou déconnecté, l'alerte est **perdue** — le `critical()` du journal reste la source de vérité.

### Ce qu'il faut lire pendant les 48 h d'observation

Le champ **`psram=`** ajouté à la trace « Heap 8bits: » (cadence 30 min, voir [system-health.md](system-health.md#trace-de-calibrage-des-seuils-mémoire-v2190)) porte la **prédiction falsifiable** du lot :

- `minFen` ne doit plus descendre sous **~30 000 o** ;
- `psram=` doit **chuter de plusieurs dizaines de Ko** à chaque chargement d'interface, puis remonter.

**Si `minFen` replonge à 16-20 Ko, l'hypothèse du tampon stdio est FAUSSE** et le cycle repart aux **mesures**, pas à un correctif de plus.

> ⚠️ **VERDICT rendu le 05/09/2026 — prédiction n°2 CONFIRMÉE, prédiction n°1 RÉFUTÉE.** `psram_free` chute bien à chaque chargement d'interface (creux de **16 912 o = 4,13 tampons de 4 096**) : le levier de B-1 fonctionne. Mais `minFen` est descendu à **1 908 o** — deux ordres de grandeur sous la prédiction, et **bien en dessous des 16 372 o d'AVANT** B-1. Deux faits s'y ajoutent : (1) la fragmentation est **permanente et progressive**, le plateau de `maxBloc8` décroissant de façon **monotone** 42 996 → 26 612 → 20 468 → 15 860 sans jamais remonter ; (2) **le plateau est passé sous `kOomRearmMinMaxAllocBytes` = 18 432** — le palier nominal du parachute ne peut donc plus se réarmer, **en régime calme**, sans qu'aucune alerte ne le dise. Conformément à la règle ci-dessus, la réfutation **n'autorise pas un correctif de plus** : elle a renvoyé aux **mesures**, c'est-à-dire au lot **B-2** (la sonde du refus).
>
> **Fait neuf décisif** : le journal portait **11 échecs d'allocation en 1 h 12**, sans PANIC, tous sur **1 622 o** en **`caps=0x080C`** (`8BIT|DMA|INTERNAL`). La contrainte **DMA est strictement plus restrictive** que `INTERNAL|8BIT` : toute mesure à caps fixes répond **à côté de la question**. C'est ce relevé qui a imposé la **double mesure** de B-2 ([ADR-0066](../adr/0066-sonde-du-tas-au-moment-du-refus.md)).

> Les **seuils de l'échelle 8 bits ne sont PAS retouchés** par ce lot ([ADR-0058](../adr/0058-gardes-heap-metrique-8-bits.md) reste en vigueur) : un seul levier, un seul effet à mesurer. L'assiette de calibrage ayant changé, ils seront réévalués **après** ces 48 h.

## Fonctionnement

### Réserve d'urgence — 12 Ko (`kOomReserveBytes`)

`initOomGuard()` alloue **12 288 o en RAM interne explicite** (`oomAllocReserveInternal`, cf. [politique d'allocation](#politique-dallocation-par-défaut--bascule-psram-v2350)) **au plus tôt du `setup()`** — avant `Serial`/logger/FS **et avant la bascule PSRAM** : bloc contigu interne pris avant toute fragmentation — et depuis la v2.18.8, **à défaut**, `kOomFallbackReserveBytes` (palier de repli, voir ci-dessous). Il prend ensuite l'**instantané** de la zone post-mortem RTC du boot précédent, l'invalide, puis enregistre `std::set_new_handler(oomNewHandler)` et `heap_caps_register_failed_alloc_callback(oomFailedAllocHook)`. Aucun log à ce stade (logger non initialisé) — le `setup()` journalise l'armement juste après, avec le **palier réellement obtenu** (`oomGuardReserveArmedBytes()`), l'**emplacement réel** (`oomGuardReserveIsInternalRam()`) et le `maxBloc8` du moment :

```
Parachute OOM armé : réserve d'urgence de 12288 o (palier nominal) pré-allouée en RAM interne, maxBloc8=36852 o
Parachute OOM : réserve non allouée au boot (nominal ET repli refusés) — armement différé en loop
```

> Le log d'armement affiche **`maxBloc8=`** depuis la v2.33.0 (réserve du contre-visa `pool-chemistry`, bug-080) : il affichait la mesure brute alors que les seuils du parachute lisent la mesure 8 bits — un chiffre juste mais trompeur au diagnostic. Depuis la **v2.35.0** il affiche en plus l'**emplacement** de la réserve (`RAM interne` / `PSRAM (ANOMALIE)`), condition C1 du gate ([ADR-0061](../adr/0061-bascule-psram-au-dela-de-4095-octets-reserve-parachute-en-interne.md)).

Dimensionnement des 12 Ko :

- **≈ 34 allocations vitales** de la taille du crash de référence (12 288 / 356 o ≈ 34) — accepts HTTP, trames WS, `String` de logs : de quoi tenir la phase critique ;
- **3× le seuil d'auto-guérison** `kRestartMaxAllocHeapBytes` (4 096 o depuis v2.33.0 — était 8 192 en métrique brute, [ADR-0029](../adr/0029-auto-guerison-heap.md) / [ADR-0058](../adr/0058-gardes-heap-metrique-8-bits.md)) : la libération redonne au système assez de marge pour que le watchdog heap (débounce ~15 s en cadence rapprochée) déclenche un **reboot propre** (pompes coupées, cumuls persistés, alerte MQTT) pendant la famine, au lieu d'un abort.

### Contrat du handler (`oomNewHandler`) — chemin d'urgence

Le new_handler est appelé par libstdc++ quand le `malloc` interne d'`operator new` échoue, puis **le `new` en cours est RE-TENTÉ** (contrat `std::set_new_handler`). Il peut être invoqué depuis **n'importe quelle tâche** : le chemin est réduit au strict minimum — **aucune allocation, aucun log, aucun mutex** (réentrance allocateur interdite), état partagé 100 % atomique (`seq_cst`, chemins froids).

1. **Extraction atomique** de la réserve (`exchange(nullptr)`) ;
2. réserve disponible → `free()` (le `new` re-tenté réussit) + compteur d'épisodes → état `Released` ;
3. réserve **déjà consommée** → `std::set_new_handler(nullptr)` (désenregistrement) → état `Depleted` : le `new` suivant lève `bad_alloc` = **statu quo ante** (comportement d'avant le parachute), le **coredump reste le filet ultime**.

Le log CRITICAL « **Réserve OOM consommée** (épisode n°N) » est émis en **DIFFÉRÉ** depuis la loop, **one-shot par épisode** (comparaison de compteurs monotones, `oomShouldLogRelease`), avec maxBloc/libre au moment du drain. Pas de `flushToDisk()` dans ce chemin (une écriture LittleFS en pleine famine aggraverait la pression) — la persistance est couverte par le flush one-shot de `checkHeapHealth()` (`kLowHeapWarnBytes`).

### Ré-inscription du `new_handler` — filet d'urgence en TÊTE de tick (v2.20.2)

**Le parachute était mort ~2 h après chaque boot.** Le `std::set_new_handler(oomNewHandler)` du tick vivait en **fin** de fonction, donc **derrière le `return` de la branche d'échec de réarmement**. Or le handler **se désinscrit lui-même** à la 2ᵉ famine (état `Depleted`). Passé ce point, tout tick qui échouait à réarmer sortait avant la ligne de ré-inscription : plus aucun chemin ne rétablissait le handler, et l'appareil tournait **sans filet anti-`bad_alloc`** jusqu'au redémarrage. C'est la cause directe des **4 crashs `abort()` sur la tâche `async_tcp`** (`ESPAsyncWebServer` fait des `new` **lançants** à l'accept HTTP et par trame WS).

La (ré)inscription est désormais faite **en tête de `oomGuardLoopTick()`, avant tout `return`**, sous la seule condition **C1** du gate `pool-chemistry` : *jamais de handler sans réserve armée* (un handler sans réserve ne libère rien — il ne ferait que retarder d'un tour le `bad_alloc` en masquant l'état réel). Prédicat pur : `oomShouldRegisterHandler(reservePresent, handlerRegistered)`.

#### Pourquoi la ré-inscription ne dépend PAS du miroir

`sHandlerRegistered` est un **miroir atomique** (libstdc++ n'offre aucun moyen de lire le handler courant sans l'écraser). La ré-inscription est appelée **à chaque tick tant qu'une réserve est armée**, **sans** être gardée par `!sHandlerRegistered` — et c'est **délibéré** :

- gardée par le miroir, une course avec `oomNewHandler` sur `async_tcp` pouvait laisser l'état **« handler nul + miroir `true` »**, dont **plus aucun chemin ne sort** : parachute mort jusqu'au reboot **et** champ `oom_handler_registered` **menteur** ;
- `std::set_new_handler` est **idempotent** : le rappeler toutes les 10 s coûte un store et **ferme la fenêtre**.

Le miroir ne sert donc plus qu'au **log one-shot de transition** (`Parachute OOM : new_handler ré-inscrit (réserve de N o armée)`, émis sur la transition seule, jamais à chaque tick) et au **diagnostic** (`oomGuardHandlerRegistered()` → champ `oom_handler_registered` de [`/get-system-info`](../API.md#get-get-system-info--write)).

#### Invariant d'ordre — miroir AVANT inscription, sur les TROIS sites

Les trois sites qui inscrivent le handler (`initOomGuard()`, filet d'urgence en tête de tick, fin de réarmement réussi) posent le miroir **AVANT** `set_new_handler`. Conséquence : **le seul état atteignable en cas d'entrelacement est « miroir `false` / handler présent »**, qui est **auto-réparé** au tick suivant (le log de transition sera simplement ré-émis une fois). L'état inverse, « miroir `true` / handler nul », est rendu **inatteignable**.

Dans l'ordre inverse, l'entrelacement suivant était possible : le tick appelle `set_new_handler()` → `async_tcp` consomme la réserve (1ʳᵉ famine) puis désinscrit le handler (2ᵉ famine, miroir → `false`) → le tick fait `exchange(true)`. Résultat : handler nul, réserve nulle, **miroir `true`**. Au tick suivant, `reservePresent` est faux : rien ne ré-inscrit, et le mensonge dure jusqu'à un réarmement réussi — c'est-à-dire **indéfiniment dans le régime de famine que l'on cherche justement à diagnostiquer**.

### Réarmement — deux paliers, tick 10 s, sur la mesure 8 bits (recalibré en v2.20.2)

`oomGuardLoopTick()` (appelé depuis `loop()`) re-alloue la réserve à cadence lente (`kOomRearmCheckIntervalMs` = 10 s). Le ré-enregistrement du handler après un réarmement réussi est **conservé** en ceinture (idempotent) — il évite d'attendre le tick suivant quand la réserve vient d'être armée dans ce même tick. Le succès est journalisé en INFO (« Parachute OOM réarmé » / « Parachute OOM surclassé », avec le palier obtenu et `maxBloc8`).

**Depuis la v2.20.2, le critère lu est `heapLargestFreeBlock8Bit()`**, plus `ESP.getMaxAllocHeap()`. L'ancienne mesure renvoyait **27 636 o FIGÉS** (reliquat d'IRAM 32 bits, [ADR-0038](../adr/0038-metrique-memoire-allouable-8-bits.md)) alors que le plus gros bloc réellement allouable valait **4 596 o** : la condition `27 636 ≥ 24 576` était **toujours vraie**, le tick retentait donc un `malloc(12288)` voué à l'échec **toutes les 10 s** — soit exactement les **3 « échecs d'allocation » par fenêtre de 30 s** observés en production. La garde fabriquait ~100 % de son propre signal d'alarme.

#### Exclusion des échecs AUTO-INFLIGÉS

Sur les **1 025** échecs d'allocation comptés le 15/08, **~100 %** étaient ce `malloc` de réarmement. `oomFailedAllocHook` interroge donc un drapeau `sSelfAllocInFlight` (prédicat pur `oomShouldCountFailure`) posé par la loop **autour du seul `malloc` de réarmement**, et ne comptabilise ni ne post-mortemise ces échecs.

> **Limite assumée** : le drapeau est **global**, pas par tâche. Un échec **réel** survenant sur l'autre cœur pendant la fenêtre (quelques microsecondes toutes les 10 s) ne serait pas comptabilisé. Compromis très favorable face au bruit supprimé.

#### Pourquoi le seuil est passé de 32 768 à 24 576 (v2.18.8), puis à 18 432 o (v2.33.0)

**Le parachute livré en v2.18.5 ne se réarmait JAMAIS** — défaut confirmé par le 2ᵉ crash de production (13/08/2026, v2.18.7, uptime 12,9 h). Pour que libstdc++ lève à `new_op.cc:54`, il faut `get_new_handler() == nullptr` : le handler s'était donc **désenregistré** (état `Depleted`) lors d'un épisode antérieur. Or le réarmement exigeait `maxBloc ≥ 32 768 o`, alors que le plus gros bloc contigu **réel** de cet appareil vaut :

| Mesure terrain (anneau RTC) | Plus gros bloc |
|---|---|
| uptime 33 min | **29 684 o** |
| uptime 12,9 h | **27 584 o** |

Le seuil était donc **au-dessus du régime normal de l'appareil** : une fois la réserve consommée, elle n'était plus jamais ré-allouée et le handler restait désenregistré → **parachute one-shot en pratique**, inopérant au moment du crash du 13/08 (`new AsyncFileResponse`, 156 o, refusé alors que l'anneau annonçait `free=81 408` / `maxB=27 584`, stables depuis 78 s).

Valeur v2.18.8 : `kOomRearmMinMaxAllocBytes = 24576` = **2 × `kOomReserveBytes`** — atteignable dans le régime **de l'époque** (maxBloc brut 27-30 Ko), tout en gardant une marge d'un bloc entier au-dessus de la taille à allouer (pas d'oscillation libération/réallocation autour du seuil de famine).

**v2.33.0 (bug-080, [ADR-0058](../adr/0058-gardes-heap-metrique-8-bits.md)) : 24 576 → 18 432.** Le seuil, désormais comparé à `maxBloc8`, était retombé dans le défaut exact du 13/08 : **24 576 > nominal fragmenté réel 21 492 o** → après une première famine, plus jamais de réserve nominale (repli 1 024 o définitif). L'arithmétique de la nouvelle valeur, imposée par le gate `pool-chemistry` du 02/09 (qui a **refusé 16 384** : allouer 12 288 depuis 16 384 laisse un reliquat < 4 096 — le parachute sèmerait son propre auto-heal) :

```
18 432 = kOomReserveBytes (12 288)
       + kRestartMaxAllocHeapBytes (4 096)
       + kOomFallbackRearmMinMaxAllocBytes (2 048)
```

Après armement nominal, il reste **au moins** la marge d'auto-guérison plus le palier de repli — l'armement ne peut jamais déclencher lui-même un reboot. Cette somme est **verrouillée par un test natif d'arithmétique sur les constantes** (`test/test_native_oom_guard/`). Et `18 432 < 21 492` (nominal terrain) : en régime fragmenté **stable** observé depuis la 2.26, le réarmement nominal — impossible depuis le 13/08 — **redevient automatique** au tick suivant la résorption d'une famine, de même que le surclassement repli → nominal.

#### Palier de repli — 1 Ko dès 2 Ko de plus gros bloc **8 bits** (recalibré v2.20.2)

« Mieux vaut un petit parachute qu'aucun parachute. » Si le bloc **nominal** de 12 Ko n'est pas allouable, le guard s'arme quand même avec `kOomFallbackReserveBytes` o dès que `maxBloc8 ≥ kOomFallbackRearmMinMaxAllocBytes`. 1 Ko couvre encore **4×** le duo fautif du 13/08 (156 o `AsyncFileResponse` + 100 o `__cxa_allocate_exception`) et près de **3×** l'accept HTTP de 356 o du 11/08.

Les deux valeurs ont été **recalibrées** parce que le tick lit désormais la mesure 8 bits : seuil **12 288 → 2 048**, taille **2 048 → 1 024**. Régime 8 bits réel mesuré sur 5 h : `maxBloc8 ∈ [4 084 ; 9 204]`, `minMaxBloc8_boot = 3 060`. Invariant **C2** imposé par le gate `pool-chemistry` et vérifié dans [`constants.h`](../../src/constants.h) :

| # | Contrainte | Vérification |
|---|---|---|
| a | seuil de réarmement < pire creux observé depuis le boot | `2 048 < 3 060` → le palier de repli reste armable même au creux (aucun trou de protection) |
| b | taille allouée ≤ moitié du seuil | `1 024 ≤ 2 048 / 2` → allouer la réserve ne peut pas à soi seul faire retomber le tas sous son propre seuil de réarmement |
| c | seuil ≤ minimum du régime établi | `2 048 ≤ 4 084` → armable en pratique |

`kOomRearmMinMaxAllocBytes` était **resté à 24 576** en v2.20.2 (palier nominal délibérément inatteignable dans le régime fragmenté d'alors — plus aucun `malloc(12288)` répété toutes les 10 s). **Depuis la v2.33.0 il vaut 18 432** ([ADR-0058](../adr/0058-gardes-heap-metrique-8-bits.md), arithmétique ci-dessus) : sous le nominal terrain 21 492 o, le palier nominal est de nouveau atteignable en régime réel.

Règle de transition (fonction pure `oomRearmTargetBytes`) :

- **surclassement** repli → nominal **autorisé** : dès que `maxBloc8 ≥ 18 432`, la réserve de 1 Ko est remplacée par 12 Ko (l'ancienne est libérée par l'`exchange`) ;
- **déclassement** nominal → repli **INTERDIT** : jamais d'oscillation ni de dégradation silencieuse ;
- réserve déjà au palier nominal → aucune action.

#### Échelle des seuils « plus gros bloc contigu » — MÉTRIQUE 8 BITS UNIQUE depuis la v2.33.0

Ces seuils décrivent une dégradation ordonnée du tas et ne se modifient pas isolément ([ADR-0037](../adr/0037-postmortem-oom-et-integrite-tas.md)). Depuis bug-080 ([ADR-0058](../adr/0058-gardes-heap-metrique-8-bits.md)), **tous lisent `heapLargestFreeBlock8Bit()`** — le régime bi-métrique de la v2.20.2 est clos, l'échelle est de nouveau comparable de bout en bout :

| Seuil | Constante | Effet |
|---|---|---|
| 2 048 o | `kOomFallbackRearmMinMaxAllocBytes` | Réarmement de la réserve de **repli** (1 Ko) |
| 4 096 o | `kRestartMaxAllocHeapBytes` | Reboot d'**auto-guérison** ([ADR-0029](../adr/0029-auto-guerison-heap.md)), débounce 3 mesures |
| 6 144 o | `kWsPushMinAllocBytes` | Pushes WebSocket sautés (≈ 2,4× la trame pire cas 2 552 o) |
| 12 288 o | `kLowHeapWarnBytes` | WARN heap + flush disque one-shot + cadence rapprochée 5 s |
| 18 432 o | `kOomRearmMinMaxAllocBytes` | Réarmement de la réserve **nominale** (12 Ko) — = 12 288 + 4 096 + 2 048, verrouillé par test |
| 21 492 o | — (documentaire) | **Nominal terrain** en régime fragmenté stable (2.26→2.32) |

> **L'immunité aux creux transitoires vient du DÉBOUNCE, pas du niveau des seuils** ([ADR-0058](../adr/0058-gardes-heap-metrique-8-bits.md)) : des creux à 1 268 o de quelques secondes existent en régime normal — aucun seuil statique > 1,3 Ko ne passe dessous. L'auto-guérison exige 3 mesures consécutives (60 s nominale, 5 s sous WARN) : seule une famine **continue ≥ 10-15 s** déclenche. Hors périmètre (encore en métrique brute) : la seule garde TLS de `web_routes_ota.cpp` — `web_routes_debug.cpp` a basculé en lot B-2.

Ceinture supplémentaire v2.18.8 : le handler remet la taille à 0 **avant** d'extraire le pointeur ; lire « pointeur vivant + taille 0 » dans cette fenêtre ferait passer une réserve nominale pour non armée et autoriserait un déclassement — la loop suppose donc le palier nominal dans ce cas.

**Critère de réarmement = ABSENCE DE RÉSERVE (pointeur nul), pas l'état** — durcissement de revue v2.18.5 : si le handler consomme la nouvelle réserve entre l'`exchange` du réarmement et le `store(Armed)`, l'état peut afficher `Armed` avec une réserve nulle ; fonder `oomShouldRearm` sur le pointeur garantit la convergence au tick suivant. L'`exchange` à la ré-allocation (libération d'une réserve résiduelle théorique) ferme le double-free/fuite en ceinture-bretelles.

### Machine à états (logique pure `oom_guard_logic.h`)

```
             new échoue, réserve dispo (exchange non nul)
 [ Armed ] ────────────────────────────────────────────► [ Released ]
     ▲          free(réserve) + épisode++ (le new re-tenté réussit)         │
     │                                                                      │
     │  réserve ABSENTE ET maxBloc8 ≥ 18 Ko (nominal) ou ≥ 2 Ko (repli)     │
     │  — tick 10 s, surclassement repli → nominal autorisé                 │
     ├──────────────────────────────────────────────────────────────────────┤
     │                                                                      ▼
     │          new échoue, réserve déjà consommée                  [ Depleted ]
     └──────── (même chemin de réarmement) ◄──── set_new_handler(nullptr) :
                                                 bad_alloc suit son cours
                                                 (statu quo ante, coredump)
```

### Callback IDF diagnostic (`heap_caps_register_failed_alloc_callback`) — flag-seul

**Complément d'attribution** : appelé depuis l'**intérieur de l'allocateur** à CHAQUE échec d'allocation — y compris les **`malloc` C que le new_handler ne voit pas** (lwIP, mbedTLS, drivers). Mode **flag-seul** : le callback ne fait que poser des atomiques (pas de free, pas de log) ; la loop draine en WARN. Depuis la v2.18.8, il écrit **en plus** le post-mortem RTC ci-dessous (stores bruts uniquement).

#### Capture de l'appelant — `function_name` + `caps` (v2.20.2)

L'IDF passe au callback un `function_name` que le firmware **jetait** (`(void)function_name;`). C'est pourtant la **seule voie** vers l'identification du fragmenteur, resté non identifié après trois cycles. Il est désormais capturé, avec le masque `caps` :

- `function_name` est un **littéral de `.rodata`** (immortel, jamais libéré) → on publie le **pointeur** dans un `std::atomic<const char*>`, **jamais une copie** : un `strncpy` coûterait une écriture non triviale **sous le verrou du tas** et autoriserait une lecture déchirée depuis la loop, pour aucun gain ;
- les deux valeurs sont **drainées et journalisées depuis le tick**, **JAMAIS depuis le hook** (verrou du composant heap détenu — cf. contrainte structurante ci-dessous).

Le **post-mortem RTC n'est PAS agrandi** pour les accueillir : l'étendre imposerait un bump de magic (donc la perte d'une trace) pour une information déjà présente dans le journal.

> 🔎 **Depuis bug-080 B-2, la zone fait 48 octets** (magic `0xF00DFA13`) — mais pour accueillir la **sonde du refus**, pas `function_name`, qui reste journal-seul. Le raisonnement ci-dessus est donc intact : on n'agrandit pas la zone pour une donnée déjà présente dans le journal, on l'agrandit pour une **mesure qu'aucune autre trace ne porte**.

#### Backoff du WARN d'échec — restaurer la fenêtre forensique (v2.20.2)

Le throttle **fixe** de 30 s (`kOomFailWarnThrottleMs`, supprimé) produisait **~120 WARN/h** — ~90 % la même ligne — et faisait tourner le journal persistant de 8 Ko en **25-35 min**. **C'est ce flot qui a détruit le post-mortem du crash du 15/08 (16:43) avant qu'il puisse être lu.**

Le délai est désormais **croissant** (fonction pure `oomFailWarnNextDelayMs`) : **immédiat** (1ᵉʳ échec d'un épisode) → **30 s** → **5 min** → **30 min** (saturation). Après `kOomFailWarnDelayResetMs` = **1 h sans nouvel échec**, l'épisode est réputé clos et le palier **revient à l'initial** — le prochain incident est signalé sans délai résiduel de 30 min.

Condition **C4** du gate : **aucune `String` supplémentaire par échec** — la ligne est construite en **un seul `snprintf` sur tampon de pile** (le seul temporaire restant est celui qu'impose l'API du logger).

```
WARN  Alloc heap échouée x3 (dernière=4096 o caps=0x0404 appelant=lwip_alloc, cumul=1028)
```

Effet mesuré sur la **rétention du journal persistant** : voir [logger.md](logger.md#rétention-réelle-du-journal-persistant-v2202).

## Post-mortem d'allocation en RAM RTC noinit (v2.18.8)

Défaut n°2 du crash du 13/08 : le CRITICAL « Réserve OOM consommée » est émis **en différé** par la loop → **perdu** quand l'abort suit immédiatement, et le journal persistant d'avant crash l'est aussi (flush toutes les 10 min). Seule la RAM RTC noinit survit. Le firmware y enregistre donc l'état **au moment exact de l'échec** et le **dumpe au boot suivant**, juste après le dump de l'anneau heap.

### Contrainte structurante — `heap_caps_*` depuis le chemin d'échec : REQUALIFIÉE en v2.40.0

> ⚠️ **Ce paragraphe disait « deadlock ». C'était FACTUELLEMENT FAUX** — et cette erreur de justification a **empêché l'instrumentation décisive pendant trois cycles de diagnostic**. Corrigé par bug-080 B-2, [ADR-0066](../adr/0066-sonde-du-tas-au-moment-du-refus.md).

Ce que dit le **désassemblage de `libheap.a`** (revue B-1) :

- `heap_caps.c.obj` ne contient **aucune relocation** vers `multi_heap_internal_lock` / `_unlock` — le verrou du tas vit **entièrement** dans `multi_heap.c.obj` ;
- `heap_caps_alloc_failed`, d'où part le callback, est **statique en `.iram1.0`** et n'est atteinte qu'**après le retour complet** de `heap_caps_malloc_base`.

**Aucun verrou de tas n'est détenu** quand le callback s'exécute : mesurer depuis là n'est **pas réentrant**.

L'interdit **reste néanmoins valable**, sous sa vraie qualification : c'est un **COÛT INTERRUPTIONS COUPÉES**, pas un risque de réentrance. Un parcours TLSF (`largest_free_block`) s'exécute interruptions coupées ; le chemin d'échec peut être déclenché **par rafales** ; et il est **appelable depuis une ISR**. Mesurer sans borne y transformerait un diagnostic en **source de gigue** pour la régulation et le watchdog.

**Exception unique et bornée** (depuis B-2) : `heapProbeForCaps()`, et **seulement** via l'entonnoir d'`oom_guard.cpp`, sous **quatre garde-fous cumulés** :

| Garde-fou | Valeur | Ce qu'il empêche |
|---|---|---|
| Jeton « un échantillon par **épisode** » | `exchange` atomique, réarmé par la loop au drain du WARN d'échec (donc suit le backoff immédiat → 30 s → 5 min → 30 min) | Deux cœurs qui sondent simultanément ; une rafale qui sonde à chaque refus |
| **Plancher temporel absolu** | `kOomProbeMinIntervalMs` = **1 000 ms** | Le drain **immédiat** du backoff (`sFailWarnDelayMs` démarre à 0) réarmant le jeton quelques ms après la sonde précédente |
| **Jamais depuis une ISR** | `xPortInIsrContext()` | Un parcours de tas en contexte d'interruption |
| **Désactivation DÉFINITIVE** | durée du **couple** > `kOomProbeMaxDurationUs` = **1 500 µs** | Qu'une plateforme lente paie le coût indéfiniment |

Plus un **WARN** de conservation à `kOomProbeWarnDurationUs` = **300 µs**, et une **borne a priori** sur le pire cas PSRAM (ci-dessous) — parce que l'autolimitation est **post-hoc par construction** : le dépassement qui la déclenche a **déjà eu lieu**.

> **Formulation honnête, à reprendre telle quelle** (déjà employée dans [`heap_metrics.h`](../../src/heap_metrics.h)) : « **sans verrou pris par nous** ». `multi_heap_get_info_impl` prend bien un spinlock, en interne et brièvement. Écrire « sans verrou » tout court reproduirait exactement le défaut que ce lot corrige : une affirmation qui se transmet mieux qu'elle ne se vérifie.

`heapLargestFreeBlock8Bit()`, `heapFree8Bit()` et `heapMetricsSnapshot()` restent **INTERDITES** dans ces chemins : elles ne sont bornées par rien.

#### La sonde du refus — double mesure, un parcours chacune (bug-080 B-2)

`heapProbeForCaps(caps)` utilise `heap_caps_get_info()`, qui livre `total_free_bytes` **et** `largest_free_block` en **UN** parcours (là où `get_free_size()` + `get_largest_free_block()` en feraient deux). Le hook en fait **deux** appels :

1. sur les **caps DE L'ÉCHEC** (`0x080C` dans le cas réel du 05/09) — la question effectivement posée à l'allocateur ;
2. sur la **référence `MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT`** (`heapCaps8BitMask()`) — le point de comparaison qui rend le verdict **attribuable**.

**Borne a priori du pire cas PSRAM** (condition 2 du gate `pool-chemistry`) : le parcours O(blocs) n'est pris que si le masque **GARANTIT la RAM interne** (`caps & MALLOC_CAP_INTERNAL`, prédicat pur `oomProbeUsesRegionScan`). Sinon, la sonde **change d'algorithme** — agrégats O(régions) via `heap_caps_get_free_size` — et « plus gros bloc » vaut la sentinelle `kOomProbeBlockUnknown`. **La sonde n'est JAMAIS sautée** : les 12 sites d'appel du hook doivent tous rendre un verdict.

> ⚠️ **Le test porte sur `MALLOC_CAP_INTERNAL`, jamais sur la présence de `MALLOC_CAP_SPIRAM`** — la première écriture du lot faisait l'inverse, défaut **bloquant** relevé en revue. Tout `malloc()` / `operator new` ordinaire remonte `caps = MALLOC_CAP_DEFAULT` (`0x1000`), **sans** bit SPIRAM (désassemblage : `heap_caps.c.obj`, `.iram1.2`, `.literal+0x8 = 0x00001000`) — c'est la classe d'échec des crashs du 11/08 et du 13/08. Or le tas PSRAM est enregistré `DEFAULT|SPIRAM|8BIT|32BIT` : `heap_caps_match()` l'aurait sélectionné et `tlsf_walk_pool` aurait parcouru **4 Mo interruptions coupées**. La sonde se serait **désactivée dès le premier `new` refusé**, morte sur son cas d'usage principal.

**Horloge = CCOUNT** (`esp_cpu_get_cycle_count()`) : une instruction, **aucun verrou** — `esp_timer_get_time()` prend un `portMUX` dans certaines implémentations ESP32, rédhibitoire dans un chemin appelable en ISR. Le compteur étant **par cœur**, `xPortGetCoreID()` est lu avant et après : si la tâche a migré, la durée est marquée invalide (`kOomProbeDurationInvalid` = `0xFFFF`) et **l'autolimitation n'est pas appliquée sur cet échantillon** — une migration ne doit jamais désactiver la sonde à vie. Une durée réelle sature à `0xFFFE`, pour que « très lente » et « non mesurable » restent discernables. La zone RTC stocke des **µs**, pas des cycles (`sProbeCyclesPerUs` calculé **une fois** dans `initOomGuard()`, repli `kOomProbeDefaultCpuMhz` = 240).

**Pas d'`esp_task_wdt_reset()`** après la section critique — divergence assumée et **validée par le gate** : le modèle du contrôle d'intégrité vise un parcours de 5-20 ms dans la loop ; ici 1,5 ms plafonné, et prendre le verrou TWDT depuis un chemin appelable en contexte non-loop serait un risque net pour un bénéfice nul.

**Condition 5 du gate** : aucune sonde tant que `oomGuardLoopTick()` n'a pas tourné **une fois** (le jeton y est armé, pas dans `initOomGuard()`) — un échec d'allocation pendant le `setup()`, le moment le plus fragile, ne doit pas déclencher un parcours de tas avant que la loop vive.

Le miroir de l'anneau (`oomGuardNoteHeapSample()`, alimenté par `heapRingSample()` à `kHeapRingSampleIntervalMs` = 2 s, pré-encodé pour un unique store aligné) **subsiste** et reste la source des champs `ringFree` / `ringMaxB` : la sonde ne le remplace pas, elle **le prime** quand elle est représentative. L'horloge du chemin d'échec pour l'horodatage reste `xTaskGetTickCount()` (lecture de `xTickCount`, sans verrou), la même que celle du miroir, pour que le calcul d'âge reste juste.

### Format de la zone — 48 octets, magic `0xF00DFA13` (bumpé en bug-080 B-2)

3ᵉ zone RTC noinit du projet, magic **distinct** de l'anneau heap (`0xB10C4EAA`) et de l'auto-guérison (`0xA5C0FFEE`). Vue comme **12 mots** de 32 bits **alignés** ; les chemins d'échec n'y font que des **stores bruts**. Budget RAM RTC noinit du projet : auto-heal 9 o + anneau heap 1 032 o + post-mortem **48 o** = **1 089 / 8 192 o** (était 1 073).

> **Pourquoi le magic a été bumpé** (`0xF00DFA11` → `0xF00DFA12`, et `0xB10C4EA9` → `0xB10C4EAA` pour l'anneau, [ADR-0038](../adr/0038-metrique-memoire-allouable-8-bits.md)) : depuis la v2.19.0, les champs « ring » de cette zone portent la mesure **8 bits**. Une zone écrite par un firmware **2.18.x** contient l'ancienne métrique (IRAM 32 bits comprise) ; la relire avec les nouveaux libellés (`free8=`, `maxB8=`) afficherait un chiffre **faux et crédible**. Conséquence assumée : **le post-mortem et la cinétique du boot de bascule 2.18.x → 2.19.0 sont perdus**. Perdre une trace est acceptable, afficher une valeur trompeuse ne l'est pas.

> **Re-bumpé en bug-080 B-2** (`0xF00DFA12` → `0xF00DFA13`) : la zone passe de **32 à 48 octets**. Même raisonnement, en plus grave — sans bump, les mots `w8..w11` seraient lus dans une zone de 32 o écrite par l'ancien firmware, c'est-à-dire dans de la **RAM RTC non initialisée** : des **valeurs de sonde inventées et crédibles**. Le post-mortem du boot de bascule est perdu, c'est accepté ([ADR-0066](../adr/0066-sonde-du-tas-au-moment-du-refus.md)).

Les magics `0xA5C0FFEE`, `0xB10C4EA9`, `0xB10C4EAA`, `0xF00DFA11`, `0xF00DFA12` et `0xF00DFA13` sont **tous réservés** — aucun ne doit être réemployé. `0xF00DFA12` en particulier reste **réservé et REJETÉ** : une zone qui le porte vient d'un firmware ≤ 2.39.x, ne fait que 32 o et n'est pas relue.

| Offset | Champ | Description |
|---|---|---|
| 0-3 | `magic` | `kOomPostMortemMagic` (`0xF00DFA13` depuis bug-080 B-2), écrit **en dernier** à l'initialisation |
| 4-7 | `failCount` | Échecs d'allocation cumulés du boot |
| 8-11 | `firstSize` | Taille demandée au 1ᵉʳ échec (octets) |
| 12-15 | `lastSize` | Taille demandée au dernier échec |
| 16-19 | `lastCaps` | Masque `heap_caps` du dernier échec |
| 20-21 | `firstUptimeS` | Uptime (s) du 1ᵉʳ échec, **modulo 65 536** |
| 22-23 | `lastUptimeS` | Uptime (s) du dernier échec, modulo 65 536 |
| 24-25 | `ringFree >> 8` | Heap libre **8 bits** du dernier échantillon d'anneau (quantum 256 o) — mesure brute avant v2.19.0 |
| 26-27 | `ringMaxB >> 6` | Plus gros bloc **8 bits** du même échantillon (quantum 64 o) — mesure brute avant v2.19.0 |
| 28-29 | `ringUptimeS` | Uptime (s) de cet échantillon → **âge** = `lastUptimeS − ringUptimeS` |
| 30 | `flags` | Drapeaux (ci-dessous) |
| 31 | `releaseCount` | Épisodes de libération de la réserve (saturé à 255) |
| 32-35 | `probeSize` | **Sonde** — taille refusée au moment de la sonde, **EXACTE** (non quantifiée : 1 622 o ne tolère aucun arrondi) |
| 36-37 | `probeCapsMaxB >> 6` | Plus gros bloc pour les **caps DE L'ÉCHEC** (quantum 64 o), ou `kOomProbeBlockUnknown` |
| 38-39 | `probeCapsFree >> 8` | Libre total pour les caps de l'échec (quantum 256 o) |
| 40-41 | `probeRefMaxB >> 6` | Plus gros bloc de **RÉFÉRENCE `INTERNAL\|8BIT`** (quantum 64 o) |
| 42-43 | `probeRefFree >> 8` | Libre total de référence `INTERNAL\|8BIT` (quantum 256 o) |
| 44-45 | `probeCaps` | Masque `heap_caps` sondé, **tronqué à 16 bits** |
| 46-47 | `probeDurationUs` | Durée du **COUPLE** des 2 parcours, saturée (`0xFFFF` = non mesurable, `0xFFFE` = plafond réel) |

**Quanta inchangés** (bloc 64 o, libre 256 o) : aucune règle de décodage neuve. Les champs `ring*` sont **périmés de 0 à 2 s** ; les champs `probe*` sont mesurés **au moment exact du refus**.

> ⚠️ **`probeCaps` est tronqué à 16 bits.** La ligne 1 du dump imprime `lastCaps` (32 bits), la ligne 2 `probeCaps` (16 bits) : identiques sur les caps rencontrées ici, **mais** l'IDF 5.5 déclare déjà des bits ≥ 16 (`TCM` 1<<16, `DMA_DESC_AHB` 1<<17, `CACHE_ALIGNED` 1<<19, `SIMD` 1<<20). Si l'un d'eux apparaissait, `probeCaps` le perdrait et `oomPostMortemProbeRepresentative()` resterait cohérente mais **moins discriminante**. Un champ de 32 bits exigerait un **13ᵉ mot**.

### Drapeaux (octet 30) — le « drapeau réserve consommée » demandé par la spec

Il n'y a **pas** de variable RTC séparée : le drapeau **est** cet octet. Il est posé **immédiatement** par le handler, donc il survit à un abort qui suit dans la microseconde — là où le CRITICAL différé par la loop est perdu.

| Bit | Constante | Signification |
|---|---|---|
| 0 (`0x01`) | `kOomPmFlagNominalUsed` | Réserve **nominale** (12 Ko) consommée |
| 1 (`0x02`) | `kOomPmFlagFallbackUsed` | Réserve de **repli** (2 Ko) consommée |
| 2 (`0x04`) | `kOomPmFlagHandlerUnregistered` | `new_handler` désenregistré (le `bad_alloc` a suivi son cours) |
| 3 (`0x08`) | `kOomPmFlagIntegrityFailed` | Contrôle d'intégrité du tas **ÉCHOUÉ** |
| 4 (`0x10`) | `kOomPmFlagIntegrityNeverRun` | Contrôle d'intégrité **jamais exécuté** (valeur initiale) |
| 5 (`0x20`) | `kOomPmFlagProbeTaken` | **Une sonde au refus a été prise** — les mots `w8..w11` sont renseignés. Ne **jamais** les lire sans ce drapeau |
| 6 (`0x40`) | `kOomPmFlagProbeIsrSkipped` | Au moins un refus est survenu **en ISR** (sonde interdite dans ce contexte) |
| 7 (`0x80`) | `kOomPmFlagProbeDisabled` | Sonde **DÉSACTIVÉE définitivement** (durée du couple > `kOomProbeMaxDurationUs`) |

> ⚠️ **L'octet `flags` est SATURÉ — 8 bits sur 8 — depuis bug-080 B-2.** Tout drapeau futur exigera un **mot supplémentaire** dans la zone (donc un nouveau bump de magic, donc la perte d'une trace). Ce n'est plus une extension gratuite. Consigné dans [ADR-0066](../adr/0066-sonde-du-tas-au-moment-du-refus.md).

### Verdict — règle exacte (révisée en bug-080 B-2)

`oomPostMortemVerdict()` (fonction pure) départage désormais **trois** hypothèses : **(a)** famine générale, **(b)** corruption du tas, **(c)** famine d'un **pool restreint** (DMA) alors que le tas 8 bits reste sain — cas neuf, et cas **majoritaire** en pratique (11 échecs sur 11 le 05/09).

1. `failCount == 0` **ou** `lastSize == 0` → `INDÉTERMINÉ` ;
2. **sonde REPRÉSENTATIVE** — `kOomPmFlagProbeTaken` **ET** `probeSize == lastSize` **ET** `probeCaps == (lastCaps & 0xFFFF)` — elle **fait autorité, l'anneau est IGNORÉ** (il est périmé de 0 à 2 s et mesuré sur des caps fixes) :
   - `probeCapsMaxB == kOomProbeBlockUnknown` (sonde O(régions), masque pouvant atteindre la PSRAM) → on tranche sur le **libre seul** : `probeCapsFree < probeSize` → **`FAMINE MÉMOIRE`**, sinon **`INDÉTERMINÉ`** (abstention assumée) ;
   - `probeCapsMaxB < probeSize` → **l'allocateur disait vrai** :
     - référence saine (`probeRefMaxB / kOomCorruptionSaneBlockRatio >= probeSize`) → **`FAMINE DMA (pool restreint)`** ;
     - sinon → **`FAMINE MÉMOIRE`** (tout le tas est à sec) ;
   - `probeCapsMaxB >= probeSize` → un bloc **existait à l'instant du refus** → **`SUSPICION CORRUPTION`**. C'est désormais **le SEUL chemin** vers ce verdict, et il repose enfin sur une mesure **contemporaine**.
3. **sonde absente ou non représentative** (jamais prise, sautée en ISR, désactivée, ou classe d'échec différente) → règle historique de la v2.18.8, **amputée de sa conclusion** :
   - aucun échantillon d'anneau → `INDÉTERMINÉ` ;
   - `failCount > kOomCorruptionMaxFails` (**comparaison stricte** : 9 échecs déclenchent, pas 8) **ET** rafale, c'est-à-dire span `lastUptimeS − firstUptimeS` ≤ `kOomCorruptionBurstSpanS` (60 s) → **`FAMINE MÉMOIRE`** ;
   - comptabilité **non** saine (`ringMaxB / kOomCorruptionSaneBlockRatio < lastSize`) → **`FAMINE MÉMOIRE`** ;
   - sinon → **`INDÉTERMINÉ`**.

> ⚠️ **`SUSPICION CORRUPTION` a DISPARU de la branche 3.** C'était sa conclusion par défaut, et elle était **fausse sur le régime réel de l'appareil** : le 05/09, 1 622 o refusés en `caps=0x080C` face à un anneau annonçant 26 612 o de plus gros bloc **8 bits** — deux grandeurs qui ne parlent pas de la même mémoire. Sans mesure contemporaine, la règle **s'abstient** désormais, et le libellé du dump dit **pourquoi** (`sonde=absente` / `ISR` / `désactivée` / `autre refus`). Un `CRITICAL` faux coûte plus cher qu'un `INDÉTERMINÉ` honnête.

> **La fenêtre de rafale vient d'une revue.** Le compteur d'échecs est cumulé sur **tout le boot** : sans borne temporelle, quelques échecs bénins étalés sur des heures auraient conclu « famine » sur le scénario **même** que ce verdict doit trancher (13/08 : 2 échecs, tas comptabilisé sain). Une vraie famine refuse tout en quelques secondes — 60 s couvrent largement l'épisode du 11/08 (~14 s).

#### Les sous-cas de la sonde — l'indétermination est toujours ATTRIBUABLE

Quatre sous-cas d'**indétermination attribuable** (`absente`, `ISR`, `désactivée`, `autre refus`), plus le cas nominal `oui` :

| `sonde=` | Signification | Que faire |
|---|---|---|
| `oui` | Sonde prise **et** représentative → le verdict repose sur elle | Lire la **ligne 2** du dump : elle porte les quatre mesures |
| `absente` | Aucune sonde sur ce boot (jeton jamais consommé, plancher temporel, ou aucun échec hors ISR) | Vérifier `oom_probe_count` sur `/get-system-info` |
| `ISR` | Au moins un refus est survenu **en ISR** — la sonde y est interdite | Attendu ; l'échec vient d'un chemin d'interruption |
| `désactivée` | La sonde s'est **auto-désactivée** (couple > 1 500 µs) | Chercher le WARN `Sonde OOM DÉSACTIVÉE définitivement` ; `oom_probe_enabled=false` ; **ne plus interpréter les statistiques**, elles sont figées |
| `autre refus` | Sonde prise, mais sur une **classe d'échec différente** du dernier échec enregistré (taille ou caps ≠) — le jeton « un échantillon par épisode » rend ce cas possible | La ligne 2 décrit un **autre** refus : ne pas la croiser avec la ligne 1 |

Sur le cas du 13/08, l'**ancienne** règle concluait `SUSPICION CORRUPTION` : 156 o refusés avec `maxB = 27 584` annoncé, soit **176×** la taille demandée. **La règle révisée rendrait `INDÉTERMINÉ`** sur ce même jeu de données (aucune sonde : firmware 2.18.8).

> ⚠️ **Ce verdict était faux, et on sait pourquoi depuis le 15/08** ([ADR-0038](../adr/0038-metrique-memoire-allouable-8-bits.md)). Le `maxB = 27 584` sur lequel il repose venait de `ESP.getMaxAllocHeap()`, qui compte l'**IRAM 32 bits inutilisable** : c'était un reliquat d'IRAM figé, pas de la mémoire allouable. La comptabilité n'était donc pas « saine », elle était **fausse** — il n'y a jamais eu de corruption (l'intégrité du tas est vérifiée OK toutes les 5 min), la **famine 8 bits était réelle**. Depuis la v2.19.0, le miroir qui alimente ces champs porte la mesure 8 bits : **la règle du verdict, inchangée, redevient exploitable**. Ses paramètres (`kOomCorruptionMaxFails`, `kOomCorruptionBurstSpanS`, `kOomCorruptionSaneBlockRatio`) seront réévalués avec les relevés on-target de la 2.19.1.

### Lignes de dump au boot — DEUX lignes depuis bug-080 B-2

`oomPostMortemDumpIfBoot()` est appelée dans `setup()` **juste après** `heapRingDumpIfCrash()` : les traces se lisent ensemble (l'anneau donne la **cinétique**, le post-mortem le **point d'échec**). Elle lit l'**instantané** pris par `initOomGuard()` (la zone RTC, elle, a déjà été invalidée et appartient au boot courant). Rien n'est émis si la zone est invalide, ou si elle ne contient ni échec ni corruption. Chaque ligne est construite en **un `snprintf` sur tampon de pile** (aucune allocation).

**Ligne 1 — comptabilité de l'épisode** (toujours émise) :

```
Post-mortem OOM : échecs=11 sur 3077 s (1er 1622 o, dernier 1622 o caps=0x080C, uptime=4313 s),
tas comptabilisé t-2s free8=58872 maxB8=20468, réserve=intacte, handler=actif,
intégrité=OK, sonde=oui → verdict FAMINE DMA (pool restreint)
```

**Ligne 2 — la sonde du refus**, émise **uniquement si `kOomPmFlagProbeTaken` est posé** :

```
Post-mortem OOM (sonde oui) : au refus de 1622 o caps=0x080C → maxBloc=1024 o libre=8192 o ;
réf8 maxBloc=26612 o libre=59384 o, en 84 µs
```

> **Pourquoi DEUX lignes et non une seule fusionnée.** Fusionner économiserait ~120 o de flash mais ramènerait le **risque de troncature silencieuse** corrigé en B-1 — et la troncature frapperait la **fin** de la ligne, c'est-à-dire le verdict. Les tampons ont été recalculés au pire cas en **octets UTF-8** (accents 2 o, `—` et `→` **3 o**) : ligne 1 `char[384]` (pire cas 323 o, `sonde=désactivée` et `verdict=FAMINE DMA (pool restreint)` étant **co-occurrents**), ligne 2 `char[224]` (pire cas 181 o).
>
> La ligne 2 n'est **jamais** émise sans sonde : afficher `maxBloc=0 libre=0 … en 0 µs` ferait passer des zéros pour une mesure. Quand elle manque, la ligne 1 dit déjà **pourquoi** (`sonde=absente` / `ISR` / `désactivée`).

> **Libellés suffixés `8` depuis la v2.19.0** (`free8=`, `maxB8=`, idem dans le dump de l'anneau) : le journal doit être **auto-descriptif**, pour qu'en relisant d'anciens logs on ne confonde jamais la mesure 8 bits (réellement allouable) avec l'ancienne mesure « interne totale ». Un log portant `free=` / `maxB=` vient d'un firmware **≤ 2.18.9** et contient la métrique surestimée.

**Politique de niveau** (révisée en B-2) :

| Situation | Niveau |
|---|---|
| Verdict `SUSPICION CORRUPTION` **ou** bit « intégrité échouée » posé | **CRITICAL** (information décisive, persistée) |
| Verdict **`FAMINE DMA (pool restreint)`** | **WARN** — ce n'est **pas** une corruption |
| `FAMINE MÉMOIRE`, `INDÉTERMINÉ` | **WARN** |
| Ligne 2 (sonde) | **WARN** dans tous les cas |

Libellés : `réserve` = `intacte` / `nominale consommée` / `repli consommée` / `nominale+repli consommées` ; `handler` = `actif` / `désenregistré` ; `intégrité` = `OK` / `ÉCHEC` / `jamais exécutée` ; `sonde` = `oui` / `absente` / `ISR` / `désactivée` / `autre refus`.

### Observabilité de la sonde en vol — trois lignes drainées par la loop

**Jamais journalisé depuis le hook** (qui n'a le droit ni de logger ni d'allouer) : les trois messages partagent **un seul site d'émission** dans `oomGuardLoopTick()`, par ordre de priorité décroissante.

```
INFO  Sonde OOM 1er échantillon (couple de 2 parcours) : 84 µs (seuil 1500 µs)
WARN  Sonde OOM LENTE, mais CONSERVÉE : 412 µs (seuil 300 µs)
WARN  Sonde OOM DÉSACTIVÉE définitivement : 1742 µs (seuil 1500 µs)
WARN  Sonde OOM 1er échantillon (couple de 2 parcours) : durée non mesurable (migration de cœur)
```

- **1ᵉʳ échantillon en INFO, one-shot** (condition 4 du gate) : le **chiffre réel** qui valide — ou non — l'innocuité de la sonde. Il arrive gratuitement, le 1ᵉʳ WARN d'échec d'un épisode étant immédiat.
- **« LENTE, mais CONSERVÉE » est ONE-SHOT PAR BOOT**, volontairement : le plancher temporel autorise **une sonde par seconde**, donc un WARN par sonde lente ferait **3 600 lignes/h** dans un journal de 8 Ko dont la rétention est de 25-35 min — la fenêtre forensique détruite **par notre propre instrumentation** (le défaut n°3 du 15/08). Les durées restent lisibles via `/get-system-info`.
- Une **durée invalide** (migration de cœur entre les deux lectures de CCOUNT) n'affiche **pas** « 65535 µs », qui serait un chiffre inventé et alarmant : elle dit ce qui s'est passé.

Le **WARN d'échec vivant** porte lui aussi la sonde, en queue de ligne — ou dit explicitement qu'elle manque :

```
WARN  Alloc heap échouée x5 (dernière=1622 o caps=0x080C appelant=heap_caps_malloc, cumul=11)
      — sonde(0x080C) maxBloc=1024 libre=8192, réf8 maxBloc=26612 libre=59384, en 84 µs
WARN  Alloc heap échouée x1 (dernière=356 o caps=0x1000 appelant=?, cumul=1) — sonde=absente
```

Tampon porté `[160]` → **`[256]`** (pire cas 241 o) : un `[160]` aurait tronqué **silencieusement** le WARN de diagnostic — précisément le défaut que ce lot supprime.

Les statistiques de la sonde sont par ailleurs exposées **hors journal**, en quatre champs additifs de [`/get-system-info`](../API.md#oom_probe_--observabilité-de-la-sonde-du-refus-v2400-additifs) : `oom_probe_enabled`, `oom_probe_count`, `oom_probe_max_us`, `oom_probe_last_caps` (quatre lectures d'atomiques : ni allocation, ni log, ni parcours de tas).

### Limites assumées (à connaître au diagnostic)

1. **Le post-mortem est une trace INDICATIVE, jamais un compteur exact.** Les mots `failCount` et `uptimes` sont modifiés en **lecture-modification-écriture sans verrou** (aucun verrou n'est autorisé sur ce chemin) : deux échecs simultanés sur les deux cœurs peuvent perdre un incrément.
2. **L'instantané pris au démarrage vit en RAM ordinaire** (`.bss`) : un crash survenant **entre** `initOomGuard()` et le dump fait perdre le post-mortem du boot **n-2** (celui du boot n-1 reste intact). Fenêtre de quelques centaines de millisecondes de `setup()`. Ce compromis est le prix de la protection inverse : sans instantané, un échec d'allocation pendant le `setup()` — le moment le plus fragile, juste après un crash mémoire — polluerait la zone du boot précédent avant qu'elle ait été lue.
3. **Les uptimes sont tronqués sur 16 bits** (repliement toutes les **18 h 12**). Les durées (span, âge de l'échantillon) sont calculées en **arithmétique modulo** ; surtout, la **validité** de la zone ne dépend **pas** de l'ordre des uptimes. Une revue avait trouvé ce défaut : un contrôle `lastUptimeS ≥ firstUptimeS` rendait le dump **muet** dès qu'un repliement s'intercalait, c'est-à-dire sur tout appareil tournant plus de 18 h — précisément le régime des deux crashs.

## Contrôle d'intégrité périodique du tas (v2.18.8)

`oomGuardIntegrityTick()` exécute `heap_caps_check_integrity_all()` toutes les `kHeapIntegrityCheckIntervalMs` = 5 min : **détecteur direct** de l'hypothèse (b). Il retourne `true` **une seule fois**, au tick où une corruption est détectée ; l'appelant (`main.cpp`) exécute alors le chemin d'arrêt propre. Les **gardes de sécurité chimique**, la cadence d'appel et l'action sur échec sont documentées dans [system-health.md](system-health.md#contrôle-dintégrité-périodique-du-tas-v2188) — ce module n'expose que le mécanisme, **l'appelant est responsable des gardes**.

> ⚠️ **v2.36.0 (feature-094) — la conjonction de gardes de l'appelant a changé.** `PumpController.isPhDosing()` et `isOrpDosing()` en ont été **retirés** : depuis la [porte de session pH](pump-controller.md#porte-de-session-ph-robuste-au-bruit-feature-094-v2360), ces prédicats suivent la **porte** et non le débit, donc ils restent vrais **pompe à l'arrêt** jusqu'à `kPhSessionMaxWallMs` = **3 h** — le détecteur de corruption du tas serait devenu **aveugle des heures d'affilée**, dans un projet qui a déjà connu une boot-loop `lfs_alloc`. Ce qu'il faut protéger, c'est la **cadence des impulsions pendant qu'une pompe tourne**, et `isAnyPumpEnergized()` le couvre exactement (il couvre en plus le test manuel armé depuis la tâche web, invisible des deux autres). Gardes conservées : `!maintenanceMode`, `!isOtaInProgress()`, `!manualInjectPh.active`, `!manualInjectOrp.active`, `!isAnyPumpEnergized()`, `now >= kAutoHealMinUptimeMs`. **Confirmé par `pool-chemistry`** ; marge verrouillée par un `static_assert` neuf (parcours < 1 % du pouls minimal : 20 ms contre 10 s). Voir [system-health.md](system-health.md#les-6-gardes-de-sécurité-chimique-exigence-pool-chemistry) et l'amendement d'[ADR-0037](../adr/0037-postmortem-oom-et-integrite-tas.md).

## Garde TLS sortante partagée — `oomTlsGateReason` (v2.18.6 ; **refondue v2.81.0, bug-165/bug-108**)

Prédicat pur **partagé par les deux routes qui ouvrent une session TLS sortante** — `/check-update` **et** `/download-update` ([`src/web_routes_ota.cpp`](../../src/web_routes_ota.cpp), fonction commune `refuseIfTlsGateClosed`) : aucune comparaison de seuil dans les handlers. **Signature et logique internes inchangées** (ADR-0017) — seule la **sémantique de ses trois entrées**, et les **seuils**, ont changé.

🚨 **Ce que cette garde mesurait jusqu'à la v2.80.x était la MAUVAISE grandeur, et c'est ce que bug-108 nomme.** Elle lisait `ESP.getFreeHeap()` (IRAM-inclusive) = **82 796 o**, bien au-dessus de l'ancien seuil 65 536 → **autorisait**. Au même instant, mesuré sur le chemin de production, le plus gros bloc **8 bits réellement allouable** valait **3 316 o** — le handshake a échoué faute de mémoire, dans une fenêtre que la garde n'a jamais vue. **Une garde qui mesure la mauvaise grandeur ne protège de rien.**

Il retourne un **motif** (`uint8_t`) et non plus un booléen — l'appelant doit distinguer un refus **matériel définitif** d'un refus **temporaire réessayable** :

| Motif | Condition (v2.81.0) | Réponse HTTP (`code`) |
|---|---|---|
| `kOomTlsGateOk` | tous les critères passés | session TLS autorisée |
| `kOomTlsGateNoPsram` | **plus gros bloc PSRAM contigu** < `kOtaTlsMinPsramLargestBlockBytes` (40 960 o) | **503** — `psram_required` **ou** `psram_low` selon la nullité du plus gros bloc PSRAM (voir ci-dessous) |
| `kOomTlsGateLowMemory` | heap **interne 8 bits** libre < `kCheckUpdateMinFreeHeapBytes` (32 768) **OU** plus gros bloc **interne 8 bits** < `kCheckUpdateMinMaxAllocBytes` (24 576) | **503** `{"error":"Mémoire insuffisante pour TLS, réessayez","code":"tls_low_memory"}` |

### 🚨 Cause racine réelle (bug-165, 2026-10-15) : mbedtls forçait ses allocations en RAM INTERNE

**Ce n'était pas un problème de certificats** — huit hypothèses l'ont supposé, toutes écartées (voir [ota-manager.md](ota-manager.md)). mbedtls 3.6.5 (libs pioarduino précompilées) porte
`CONFIG_MBEDTLS_INTERNAL_MEM_ALLOC=1` : il **force** `calloc`/`free` en RAM interne, quelle que soit la politique PSRAM que le projet a déjà posée (`heap_caps_malloc_extmem_enable`, `kPsramExtmemThresholdBytes` = 1 024 o). Mesuré sur le chemin de production, pendant un `/check-update` réel :

| Instant | `free8` | `maxAlloc8` |
|---|---|---|
| Avant ouverture TLS | 60 760 | 57 332 |
| Juste avant le handshake | 11 296 | 10 740 |
| **Pendant la vérification du certificat (ECDSA P-384)** | 4 852 | 🚨 **3 316** |
| 13 ms après l'échec | 59 672 | 47 092 |

**Confirmé indépendamment** : `fs_guard` (mécanisme préexistant, sans aucun rapport avec cette investigation) a relevé **le même 3 316** au même instant et suspendu ses ouvertures de fichiers 30 s — deux systèmes différents, la même pénurie.

**Correctif** ([`src/mbedtls_psram_alloc.{h,cpp}`](../../src/mbedtls_psram_alloc.h), [ADR-0112](../adr/0112-mbedtls-obeit-a-la-politique-psram-du-projet.md)) : ⛔ **aucun allocateur écrit**. `mbedtls_platform_set_calloc_free(calloc, free)` substitue, à l'exécution, les `calloc`/`free` de la **libc** (déjà PSRAM-aware) à ceux de mbedtls — sans recompiler la bibliothèque (impossible, libs précompilées pioarduino). Posé **une seule fois**, dans `psramFound()` de `main.cpp`, **avant toute utilisation de mbedtls** (Wi-Fi compris). **Vérifié par un témoin anti-décoratif réel** (le projet s'est déjà fait avoir une fois par un parachute OOM décoratif, cf. [page dédiée](#témoin-réel-vs-crochet-décoratif) ci-dessous) : une allocation **via le pointeur mbedtls** doit être vue en PSRAM, sinon l'état est `kMbedtlsPsramAllocDecorative`, pas `Active`.

### Mapping motif → code HTTP (v2.18.7, inchangé par la refonte v2.81.0)

Le prédicat pur reste à **trois motifs** ; c'est la **route** (`refuseIfTlsGateClosed` dans [`src/web_routes_ota.cpp`](../../src/web_routes_ota.cpp)) qui scinde `kOomTlsGateNoPsram` en deux codes, à partir de la mesure **qu'elle a déjà en main** — un simple test de nullité, **aucun seuil dupliqué hors du prédicat** :

| Motif | Test dans la route | `code` renvoyé | Nature |
|---|---|---|---|
| `kOomTlsGateNoPsram` | plus gros bloc PSRAM `== 0` | `psram_required` | **Permanent** — module sans PSRAM (WROOM) : le front neutralise la carte de mise à jour en ligne |
| `kOomTlsGateNoPsram` | plus gros bloc PSRAM `> 0` | `psram_low` | **Temporaire** — PSRAM installée mais fragmentée/saturée : réessayable, aucune neutralisation |
| `kOomTlsGateLowMemory` | — | `tls_low_memory` | **Temporaire** — heap interne 8 bits dégradé : réessayable |

Pourquoi ici et pas dans le prédicat : la distinction « absente » / « saturée » n'est pas une **décision de garde** (le refus est identique dans les deux cas et se fait sur le même seuil), c'est une **information de présentation**. La garder dans la route évite d'introduire un quatrième motif et un second seuil dans la logique pure testée. Corollaire : le firmware ne doit jamais affirmer l'absence de PSRAM sur une PSRAM simplement saturée, et le front discrimine sur `code` — **jamais sur le texte** du champ `error`.

**La PSRAM est évaluée EN PREMIER, fail-closed** ([ADR-0036](../adr/0036-psram-requise-mise-a-jour-en-ligne.md), amendé par la note additive 2026-10-15) : mbedTLS réclame plusieurs blocs contigus d'environ 16 Ko. ⚠️ **Le critère n'est plus la PSRAM LIBRE TOTALE mais son PLUS GROS BLOC CONTIGU** (`heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)`) : un total élevé mais fragmenté n'accueillerait pas un tampon TLS de 16 Ko. Seuil `kOtaTlsMinPsramLargestBlockBytes` = **40 960 o** = 2 × 16 384 (les deux tampons d'enregistrement SSL, `CONFIG_MBEDTLS_SSL_MAX_CONTENT_LEN`) + 8 192 (marge du budget JSON de `/check-update`, qui peut emprunter au même pool juste après le handshake) — dérivation verrouillée par `static_assert` dans `constants.h`.

Dans `/download-update`, la garde est placée **après** la validation url/allowlist/digest et **avant** `WiFiClientSecure`, `setOtaInProgress()`, `LittleFS.end()` et tout effacement de partition : un refus n'a **aucun effet de bord**.

Chaque refus émet un WARN auto-explicatif : motif, plus gros bloc PSRAM, les deux mesures heap **interne 8 bits**, les trois seuils et l'**IP client**. Voir [API.md](../API.md#garde-tls-sortante-psram-libre--heap-v2186).

> Le **texte** du refus `kOomTlsGateNoPsram` a été reformulé en v2.18.7 (destiné à l'utilisateur, vocabulaire aligné sur la carte « Mise à jour manuelle » de l'interface) et le refus s'y scinde en deux codes (`psram_required` / `psram_low`) ; le refus heap porte désormais `code: "tls_low_memory"`. Les **codes HTTP** et l'**ordre** d'évaluation sont **inchangés** par la refonte v2.81.0 — seuls les seuils et la métrique sous-jacente ont changé (voir ci-dessus).

### Observabilité du repli PSRAM→interne (condition C4, v2.81.0)

La PSRAM reste un **pool de travail partagé** (ADR-0061) : si elle sature au moment précis d'une session TLS, les allocations mbedtls retombent **silencieusement** en RAM interne — sans rien pour le signaler, une PSRAM saturée ramènerait l'appareil au défaut d'origine. `logTlsHeapWindow()` ([`src/web_routes_ota.cpp`](../../src/web_routes_ota.cpp)) journalise en `INFO` un instantané des trois mesures **avant** l'ouverture de la session **et** immédiatement **après** le retour de `HTTPClient::GET()` (le point le plus proche de « pendant » qu'un appel bloquant permette d'observer sans ré-instrumenter mbedtls) ; si le plus gros bloc **interne** descend sous `kOtaTlsInternalFallbackWarnBytes` (16 384 o, un seul tampon SSL tombé en interne) pendant cette fenêtre, un `WARN` signale le repli probable.

### Témoin réel vs. crochet décoratif

`mbedtlsPsramAllocInstall()` ([`src/mbedtls_psram_alloc.cpp`](../../src/mbedtls_psram_alloc.cpp)) pose le crochet **puis** alloue un témoin de 2 048 o **via le pointeur mbedtls** (`mbedtls_calloc()`, jamais `calloc()` directement — sinon le témoin ne prouverait que la politique d'allocation du projet, pas que mbedtls l'emprunte réellement) et vérifie qu'il atterrit en PSRAM (`esp_ptr_external_ram()`). Trois états, exposés par `/get-system-info` (`mbedtls_psram_alloc` : 0/1/2) :

| État | Valeur | Signification |
|---|---|---|
| `kMbedtlsPsramAllocAbsent` | 0 | Pose jamais tentée (pas de PSRAM) ou refusée par mbedtls |
| `kMbedtlsPsramAllocActive` | 1 | Crochet posé **et** témoin confirmé en PSRAM — régime nominal attendu |
| `kMbedtlsPsramAllocDecorative` | 2 | 🚨 **ANOMALIE** — crochet posé mais le témoin est retombé en RAM interne : la famine du handshake TLS persiste malgré le correctif |

Décision pure `mbedtlsPsramAllocVerdict(hookInstalled, probeInPsram)`, testée nativement et **par mutation** (T4, `test/test_native_oom_guard/`) — même idiome que le parachute anti-OOM, qui a déjà été décoratif une fois sans que rien ne le signale avant une mesure directe.

## Limites assumées

- **Le parachute n'est PAS une solution à la famine** : il absorbe l'allocation qui aurait aborté et achète du temps pour un diagnostic + reboot propre. Le **containment de fond reste l'auto-guérison heap** ([ADR-0029](../adr/0029-auto-guerison-heap.md)) : si la famine persiste, `checkHeapHealth()` (débounce ~15 s) déclenche `performFailSafeRestart()`.
- **Seuls les `new` C++ sont couverts** par la re-tentative : les `malloc` internes (lwIP, mbedTLS, drivers) échouent toujours en `NULL` — le callback IDF ne fait que les **compter** (diagnostic).
- **Réserve épuisée** = retour exact au comportement d'avant le parachute (`bad_alloc` → coredump) — jamais pire.
- La cause du pic de −60 Ko du 11/08 reste **non attribuée** (candidat principal : gonflement des buffers RX du driver WiFi pendant une tempête/réassociation) — l'attribution attend la récidive instrumentée (IP client, WARN heap_caps, ring enrichi).
- ✅ **La cause racine est trouvée depuis le 15/08 (3ᵉ crash) : la métrique était fausse.** `ESP.getFreeHeap()` / `ESP.getMaxAllocHeap()` mesuraient un pool incluant l'IRAM 32 bits inutilisable ; la famine 8 bits était réelle et **aucune garde ne pouvait la voir** ([ADR-0038](../adr/0038-metrique-memoire-allouable-8-bits.md)). Ce qui reste inconnu, c'est **quel consommateur** épuise la mémoire 8 bits (phase 2 : `AsyncStaticWebHandler`, `LittleFS.begin(false)` limité à 10 fichiers ouverts, fuite éventuelle de `File`) — cette recherche n'avait aucun sens tant que l'instrument mentait.
- **La cause racine du crash du 13/08 était réputée INCONNUE à la v2.18.8.** Cette version **instrumente**, elle ne répare pas : elle rétablit une protection qui était inopérante et met en place de quoi **trancher famine / corruption** à la prochaine occurrence. Les trois limites du post-mortem ci-dessus s'appliquent à cette trace.

## Constantes ([`constants.h`](../../src/constants.h))

| Constante | Valeur | Rôle |
|---|---|---|
| `kOomReserveBytes` | `12288` (12 Ko) | Réserve d'urgence **nominale** (≈ 34 allocs vitales de 356 o ; 3× le seuil auto-guérison 4 096 o) |
| `kOomRearmMinMaxAllocBytes` | `18432` (18 Ko) | Hystérésis : **`maxBloc8`** minimal pour ré-allouer la réserve nominale — **24 576 → 18 432 en v2.33.0** ([ADR-0058](../adr/0058-gardes-heap-metrique-8-bits.md)) : = 12 288 + 4 096 + 2 048 (16 384 refusé par le gate : reliquat post-armement < seuil auto-heal), atteignable sous le nominal terrain 21 492 o |
| `kOomFallbackReserveBytes` | `1024` (1 Ko) | Réserve de **repli** quand le nominal n'est pas allouable — **2048 → 1024 en v2.20.2** (invariant C2-b : `1024 ≤ 2048/2`) |
| `kOomFallbackRearmMinMaxAllocBytes` | `2048` (2 Ko) | **`maxBloc8`** minimal pour armer le palier de repli — **12288 → 2048 en v2.20.2** (invariant C2-a : `2048 < 3060` = pire creux observé) |
| `kOomRearmCheckIntervalMs` | `10000` (10 s) | Cadence du contrôle de réarmement dans la loop |
| `kPsramExtmemThresholdBytes` | `4095` | **v2.35.0** ([ADR-0061](../adr/0061-bascule-psram-au-dela-de-4095-octets-reserve-parachute-en-interne.md)) — seuil passé à `heap_caps_malloc_extmem_enable()` : au-delà, une allocation **par défaut** est tentée en PSRAM d'abord. Le test de l'allocateur est **inclusif** (`taille <= seuil ⇒ interne`), d'où `4095` et non `4096` |
| `kVfsStdioBufferBytes` | `4096` | **v2.35.0 — miroir documentaire** de `DEFAULT_FILE_BUFFER_SIZE` (`vfs_api.cpp:22`), le tampon stdio attaché à chaque fichier LittleFS ouvert. Verrouillé par `static_assert` : `kPsramExtmemThresholdBytes + 1 == kVfsStdioBufferBytes` |
| `kOomNoReserveCriticalDelayMs` | `300000` (5 min) | **v2.35.0, condition C2 du gate** — sans **aucune** réserve armée pendant ce délai : `critical()` + alerte MQTT [`oom_reserve_absent`](../MQTT.md#alertes). 5 min = **30 tentatives** de réarmement (10 s) : bien au-delà d'un creux transitoire, très en deçà des **heures** de vol à nu des 13 et 15/08/2026 |
| `kOomFailWarnDelayInitialMs` | `30000` (30 s) | Backoff du WARN d'échec — 1ᵉʳ palier (v2.20.2, remplace `kOomFailWarnThrottleMs`) |
| `kOomFailWarnDelayMidMs` | `300000` (5 min) | Backoff — 2ᵉ palier |
| `kOomFailWarnDelayMaxMs` | `1800000` (30 min) | Backoff — palier de **saturation** |
| `kOomFailWarnDelayResetMs` | `3600000` (1 h) | Sans nouvel échec pendant ce délai → retour au palier initial |
| `kOomCorruptionMaxFails` | `8` | Au-delà (comparaison **stricte**) **et** en rafale → verdict `FAMINE MÉMOIRE` (v2.18.8) |
| `kOomCorruptionBurstSpanS` | `60` (60 s) | Fenêtre de **rafale** du verdict : span entre 1ᵉʳ et dernier échec (v2.18.8) |
| `kOomCorruptionSaneBlockRatio` | `4` | **Sémantique élargie en bug-080 B-2** : le ratio « maxBloc ≥ 4 × taille demandée = comptabilité saine » sert désormais **deux** tests. **(1)** Sur la **référence `INTERNAL\|8BIT` de la sonde** — `probeRefMaxB / 4 >= probeSize` — il départage `FAMINE DMA (pool restreint)` (référence saine : seul le pool restreint est à sec) de `FAMINE MÉMOIRE` (tout le tas est à sec). **(2)** Sur l'**anneau**, en branche « sonde absente », il ne conclut plus que `FAMINE MÉMOIRE` **ou** `INDÉTERMINÉ` — **plus jamais `SUSPICION CORRUPTION`** |
| `kOomProbeWarnDurationUs` | `300` (300 µs) | **B-2** — durée du **COUPLE** des 2 parcours au-delà de laquelle un WARN one-shot par boot est émis. **Sonde CONSERVÉE** |
| `kOomProbeMaxDurationUs` | `1500` (1,5 ms) | **B-2** — au-delà : sonde **DÉSACTIVÉE définitivement** + WARN one-shot. Verrouillé sous `kHeapIntegrityWarnDurationUs` (5 ms) par `static_assert` : une sonde dans un chemin d'échec ne doit jamais coûter l'ordre de grandeur d'un parcours toléré dans la loop |
| `kOomProbeMinIntervalMs` | `1000` (1 s) | **B-2, condition 3 du gate** — plancher temporel **absolu** entre deux sondes, **en plus** du jeton par épisode (dont le drain immédiat pourrait sinon réarmer à quelques ms d'intervalle). La **1ʳᵉ** sonde du boot n'est jamais bloquée |
| `kOomProbeDefaultCpuMhz` | `240` | **B-2** — repli de conversion cycles CCOUNT → µs si `getCpuFrequencyMhz()` renvoie 0 (calculé **une fois** au setup ; le hook ne fait qu'une division entière) |
| `kOomProbeBlockUnknown` | `0xFFFF << 6` (4 194 240) | **B-2** — sentinelle « plus gros bloc **non renseigné** », valeur produite naturellement par la quantification du champ RTC saturé : aucun encodage spécial, aucune règle de décodage neuve |
| `kHeapIntegrityCheckIntervalMs` | `300000` (5 min) | Cadence du contrôle d'intégrité du tas (v2.18.8, gardes chimiques côté appelant) |
| `kHeapIntegrityWarnDurationUs` | `5000` (5 ms) | Au-delà : WARN (contrôle conservé) |
| `kHeapIntegrityMaxDurationUs` | `20000` (20 ms) | Au-delà : contrôle **désactivé définitivement** |
| `kOtaTlsMinPsramLargestBlockBytes` | `40960` (40 Ko) | Garde TLS : **plus gros bloc PSRAM contigu** minimal — **refondu v2.81.0** (était `kOtaTlsMinPsramBytes` = 1 048 576 sur la PSRAM libre **totale**, jamais mesuré sur WROVER réel). Dérivation : 2 × 16 384 (tampons SSL) + 8 192 (marge JSON) |
| `kCheckUpdateMinFreeHeapBytes` | `32768` (32 Ko) | Garde TLS : heap **interne 8 bits** libre minimal — **refondu v2.81.0** (était 65 536 sur `ESP.getFreeHeap()`, IRAM-inclusive). Dérivation : `kCheckUpdateMinMaxAllocBytes` + budget JSON (8 192) |
| `kCheckUpdateMinMaxAllocBytes` | `24576` (24 Ko) | Garde TLS : plus gros bloc **interne 8 bits** contigu minimal — **refondu v2.81.0** (était 20 480 sur `ESP.getMaxAllocHeap()`, IRAM-inclusive). Dérivation : `kOomRearmMinMaxAllocBytes` (18 432, sommet de l'échelle ADR-0058) + 6 144 — volontairement **au-dessus** du sommet, verrouillé par `static_assert` (ne s'intercale PAS dans l'échelle : garde dédiée OTA, pas un barreau de plus) |
| `kOtaTlsInternalFallbackWarnBytes` | `16384` (16 Ko) | **v2.81.0** — seuil de WARN d'observation (pas une garde) : signale qu'une allocation mbedtls semble être retombée en interne pendant une session TLS (`logTlsHeapWindow()`) |

> ⚠️ **Précision v2.35.0 sur la note ci-dessous** : « > 4 Ko » était un **arrondi faux**. Le test de l'allocateur est `taille <= seuil ⇒ RAM interne` — à seuil 4096, une allocation de **exactement 4096 o** restait **interne**. Depuis la bascule ([ADR-0061](../adr/0061-bascule-psram-au-dela-de-4095-octets-reserve-parachute-en-interne.md)), le seuil effectif est **4095** : « plus de 4095 o ⇒ PSRAM d'abord ».

> **🚨 Note WROVER/PSRAM — RÉVISÉE v2.81.0 (bug-165/bug-108)** : la note précédente affirmait que « les buffers mbedTLS > 4 Ko partent automatiquement en PSRAM ». **C'était faux** : mbedtls 3.6.5 (libs pioarduino précompilées) porte `CONFIG_MBEDTLS_INTERNAL_MEM_ALLOC=1` et **forçait** ses allocations en RAM interne, quelle que soit la politique PSRAM du projet — mesuré, pas supposé : `maxAlloc8` s'effondrait à **3 316 o** pendant un handshake réel, pendant que 4,17 Mo de PSRAM dormaient. **Corrigé** par `mbedtls_platform_set_calloc_free()` (voir section dédiée ci-dessus, [ADR-0112](../adr/0112-mbedtls-obeit-a-la-politique-psram-du-projet.md)) : les buffers mbedTLS partent **réellement** en PSRAM depuis ce correctif, et seulement depuis lui. `kOtaTlsMinPsramLargestBlockBytes` = 40 960 o est désormais **mesuré sur WROVER réel** (plus gros bloc PSRAM observé très au-dessus, ~4 Mo).

## Tests

Tests natifs dédiés (`test/test_native_oom_guard/`) : machine à états du handler (libération / désenregistrement), hystérésis et critère pointeur du réarmement (course handler/réarmement incluse), **paliers nominal/repli** de `oomRearmTargetBytes` (surclassement autorisé, déclassement interdit, tas trop dégradé), one-shot du log par épisode, **encodage/décodage/validité/span** du post-mortem (aller-retour, saturations, repliement des uptimes sur 16 bits), **verdict** (frontière stricte de `kOomCorruptionMaxFails`, fenêtre de rafale, ratio de comptabilité saine, cas indéterminés), cadence et autolimitation du contrôle d'intégrité, garde `oomTlsGateReason` — motifs et **priorité PSRAM sur heap**, PSRAM saturée, frontières inclusives des trois seuils **recalibrés** (bug-165/bug-108, v2.81.0), et la décision pure `mbedtlsPsramAllocVerdict()` (T4, validée par mutation) — **120 `RUN_TEST` mesurés dans ce seul fichier au 2026-10-15** (grep statique, compilation non relancée dans cette passe de documentation — à recompter plutôt qu'à recopier, ADR-0073).

**v2.35.0 (bug-080 B-1)** : `test_psram_extmem_threshold_arithmetic_v2350` fige les **valeurs absolues** `kVfsStdioBufferBytes = 4096` et `kPsramExtmemThresholdBytes = 4095`, la relation `seuil + 1 == tampon`, `kOomReserveBytes > seuil` (c'est cette relation, et elle seule, qui rend **nécessaire** le `MALLOC_CAP_INTERNAL` explicite), `kOomFallbackReserveBytes <= seuil` et `kOomNoReserveCriticalDelayMs = 300 000`. **Ce que les `static_assert` ne couvrent pas** : ils verrouillent des *relations*, donc ils sont indifférents à une **translation** des deux constantes (8192 / 8191 compile sans un mot et ramène le tampon réel de 4096 o en RAM interne) — seules les valeurs absolues du test refusent cette mutation.

**bug-080 B-2** : la suite passe à **113 cas** (`test/test_native_oom_guard/`). Nouveaux invariants couverts — aller-retour d'encodage/décodage de la zone **48 o** (les 4 mots neufs, quanta et saturations comprises), `oomProbeUsesRegionScan` (**`0x1000` = `MALLOC_CAP_DEFAULT` prend bien les agrégats**, `0x080C` et `0x0804` gardent le parcours complet), `oomProbeFloorElapsed` (1ʳᵉ sonde jamais bloquée, repliement de `millis()`), `oomProbeSaturateDurationUs` (`0xFFFE` distinct de `0xFFFF`), `oomPostMortemProbeRepresentative` (taille ≠, caps ≠, drapeau absent), et les **quatre branches** du verdict révisé, dont `FAMINE DMA` sur les valeurs réelles du 05/09 et la **disparition de `SUSPICION CORRUPTION`** en branche « sonde absente ». Total projet : **32 suites / 1 261 cas**, **26 mutants tués sur 26**.

## Fichiers liés

- [`src/main.cpp`](../../src/main.cpp) — `initOomGuard()` en tête de `setup()`, `oomPostMortemDumpIfBoot()` après le dump de l'anneau, `oomGuardLoopTick()` et `oomGuardIntegrityTick()` (sous gardes chimiques) dans `loop()`, `handleHeapCorruptionDetected()`
- [`src/heap_metrics.h`](../../src/heap_metrics.h) / [`src/heap_metrics.cpp`](../../src/heap_metrics.cpp) — **source unique** de la mesure « mémoire réellement allouable » (`heapFree8Bit()`, `heapLargestFreeBlock8Bit()`, `heapMetricsSnapshot()`), v2.19.0. Ces trois-là restent **interdites d'appel depuis `oomFailedAllocHook` et depuis le `new_handler`** — non pour cause de réentrance ([requalifié en B-2](#contrainte-structurante--heap_caps_-depuis-le-chemin-déchec--requalifiée-en-v2400)), mais parce qu'elles ne sont **bornées par rien**. Seule `heapProbeForCaps()` (+ `heapCaps8BitMask()`) y est autorisée, via l'entonnoir borné d'`oom_guard.cpp`.
- [`src/heap_ring.cpp`](../../src/heap_ring.cpp) — source unique des mesures : alimente l'anneau **et** le miroir `oomGuardNoteHeapSample()` (cadence 2 s inchangée ; mesure 8 bits depuis v2.19.0)
- [system-health.md](system-health.md) — auto-guérison heap (containment de fond), ring de télémétrie (cinétique pré-crash), contrôle d'intégrité et gardes chimiques, budget RAM RTC noinit consolidé
- [ADR-0035](../adr/0035-parachute-oom-new-handler.md) — décision et alternatives écartées (parachute)
- [ADR-0037](../adr/0037-postmortem-oom-et-integrite-tas.md) — post-mortem RTC + miroir de l'anneau + verdict + intégrité périodique + recalibrage du réarmement (v2.18.8)
- [ADR-0036](../adr/0036-psram-requise-mise-a-jour-en-ligne.md) — PSRAM libre requise pour toute session TLS sortante (alternatives écartées : seuil maxBloc relevé, tentative rattrapée, `/download-update` non gardée) — **amendé par une note additive 2026-10-15**, voir [ADR-0112](../adr/0112-mbedtls-obeit-a-la-politique-psram-du-projet.md)
- [ADR-0112](../adr/0112-mbedtls-obeit-a-la-politique-psram-du-projet.md) — **mbedtls obéit à la politique PSRAM du projet** (bug-165/bug-108, v2.81.0) : cause racine réelle (allocation interne forcée), correctif (`mbedtls_platform_set_calloc_free`), dépendance désormais structurante à la PSRAM
- [ADR-0038](../adr/0038-metrique-memoire-allouable-8-bits.md) — **cause racine des 3 crashs** : mesure 8 bits obligatoire dans les gardes, helper centralisé, double exposition, découpage mesure (2.19.0) puis bascule (2.19.1), bump des deux magics RTC
- [ADR-0040](../adr/0040-pas-de-calibrage-sur-fragmentation-pathologique.md) — **on ne calibre pas des seuils sur une fragmentation pathologique** : régime bi-métrique assumé en v2.20.2, règle de recalibrage d'ADR-0038 déclarée inapplicable, mesures du 15/08 invalidées — **superseded par ADR-0058** (v2.33.0)
- [ADR-0058](../adr/0058-gardes-heap-metrique-8-bits.md) — **bascule des 4 gardes sur la métrique 8 bits** (v2.33.0) : échelle unique 2 048 < 4 096 < 6 144 < 12 288 < 18 432, immunité par débounce temporel, réarmement 18 432 verrouillé par test, révocation du NO GO v2.20.2
- [ADR-0066](../adr/0066-sonde-du-tas-au-moment-du-refus.md) — **sonde du tas au moment exact du refus** (bug-080 B-2) : requalification de l'interdit `heap_caps_*` (coût interruptions coupées, **pas** deadlock — preuve par désassemblage) et son exception à 4 garde-fous, **double mesure** caps de l'échec + référence `INTERNAL|8BIT` (zone RTC 48 o, magic `0xF00DFA13`), verdict neuf `FAMINE DMA (pool restreint)` en WARN, et `SUSPICION CORRUPTION` rendu inatteignable sans mesure contemporaine
