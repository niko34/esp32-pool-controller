# Subsystem — `system-health` (santé système & auto-guérison)

- **Fichiers** : [`src/main.cpp`](../../src/main.cpp) (`checkSystemHealth`, `performFailSafeRestart`, `performAutoHealRestart`, `enterMaintenanceMode`, `initAutoHealRtcState`, `ensureNetworkProbe`, `onNetPingSuccess`), [`src/self_heal_logic.h`](../../src/self_heal_logic.h) / [`.cpp`](../../src/self_heal_logic.cpp), [`src/net_health_logic.h`](../../src/net_health_logic.h) / [`.cpp`](../../src/net_health_logic.cpp), [`src/history_guard_logic.h`](../../src/history_guard_logic.h) / [`.cpp`](../../src/history_guard_logic.cpp) (+ coquille dans [`src/history.cpp`](../../src/history.cpp))
- **Pas de singleton** : logique portée par des fonctions libres de `main.cpp` + trois modules purs `self_heal_logic` (heap), `net_health_logic` (connectivité) et `history_guard_logic` (disjoncteur de boot-loop history).
- **Décisions architecturales** : [ADR-0029](../adr/0029-auto-guerison-heap.md) (auto-guérison mémoire), [ADR-0030](../adr/0030-watchdog-connectivite.md) (watchdog de connectivité), [ADR-0031](../adr/0031-disjoncteur-bootloop-history.md) (disjoncteur de boot-loop history), [ADR-0037](../adr/0037-postmortem-oom-et-integrite-tas.md) (post-mortem d'allocation RTC + contrôle d'intégrité périodique du tas, v2.18.8), [ADR-0038](../adr/0038-metrique-memoire-allouable-8-bits.md) (**mesure 8 bits = mémoire réellement allouable**, v2.19.0), [ADR-0058](../adr/0058-gardes-heap-metrique-8-bits.md) (**bascule des gardes sur la métrique 8 bits**, v2.33.0), [ADR-0077](../adr/0077-metrique-de-garde-exclut-les-pools-inatteignables.md) (**une métrique de garde exclut les pools qu'un `malloc()` ne peut pas atteindre**, v2.49.0).

> ✅ **v2.33.0 — l'auto-guérison heap et le WARN lisent enfin la métrique 8 bits** ([ADR-0058](../adr/0058-gardes-heap-metrique-8-bits.md), bug-080 lot A). L'ancienne mesure `ESP.getMaxAllocHeap()` (pool `MALLOC_CAP_INTERNAL`, IRAM 32 bits comprise) possède un **plancher infranchissable ~27 636 o** : les gardes calées dessus n'ont **jamais déclenché** — le PANIC du 29/08 et la famine de 9 min du 02/09 (1 466 échecs d'allocation) sont passés sans une seule alerte. `checkHeapHealth()` échantillonne désormais `heapLargestFreeBlock8Bit()`, avec les seuils recalibrés : **auto-guérison à 4 096 o** (`kRestartMaxAllocHeapBytes`), **WARN + flush + cadence rapprochée à 12 288 o** (`kLowHeapWarnBytes`). L'immunité aux creux transitoires (jusqu'à 1 268 o pendant quelques secondes en régime normal) vient du **débounce temporel** — 3 mesures consécutives, cadence 5 s sous le WARN, soit une famine continue ≥ 10-15 s avant restart — pas du niveau du seuil. Les logs disent `maxBloc8=`.

## Rôle

Surveiller périodiquement la santé du firmware et **sortir automatiquement l'appareil d'un gel ou d'une boucle « appareil injoignable »**. Trois volets d'auto-guérison **distincts** cohabitent :

- **Watchdog mémoire (heap)** — gel par **fragmentation du tas** : web injoignable + logging arrêté, mais heap **total** correct (le plus gros bloc contigu s'effondre). Voir [ADR-0029](../adr/0029-auto-guerison-heap.md).
- **Watchdog de connectivité (réseau)** — gel de la **pile lwIP** : web/mDNS/ICMP morts alors que le WiFi reste **associé** et que le **heap est sain** (maxBloc ≈ 37 Ko lors du gel diagnostiqué). Le thread `tcpip`/lwIP n'est pas surveillé par la TWDT. Voir [ADR-0030](../adr/0030-watchdog-connectivite.md).
- **Disjoncteur de boot-loop history (v2.10.0)** — boot-loop par **partition history corrompue** : panic CPU non rattrapable (`IntegerDivideByZero` dans `lfs_alloc`) à la 1ʳᵉ écriture dans `/system.log`, **avant la montée du WiFi** → appareil définitivement injoignable. Détection **inter-boot** via NVS + effacement automatique de la partition. Voir [ADR-0031](../adr/0031-disjoncteur-bootloop-history.md) et la [section dédiée](#disjoncteur-de-boot-loop-de-linit-history-v2100).

Pour les deux watchdogs (heap, réseau), les tâches surveillées par le watchdog matériel restent **vivantes**, donc la TWDT ne se déclenche jamais ; pour le boot-loop history, le crash a lieu **pendant `setup()`**, avant tout mécanisme runtime.

Responsabilités :

1. **Instrumentation** du tas (trace persistée + flush one-shot).
2. **Auto-guérison mémoire** : redémarrage propre fail-safe sur fragmentation critique confirmée.
3. **Watchdog de connectivité** : sonde ICMP passerelle + réponse graduée (reconnexion WiFi → reboot fail-safe) sur gel de la pile réseau.
4. **Disjoncteur de boot-loop history** : effacement automatique de la partition `history` après 2 crashs consécutifs pendant sa fenêtre d'init.
5. **Mode maintenance** anti-boucle : au-delà d'un plafond de reboots **partagé** (heap + réseau), on cesse d'enchaîner et on dégrade proprement en restant joignable.

Deux fonctions distinctes portent les watchdogs runtime depuis `loop()`, en contexte `loopTask` :

- **`checkHeapHealth()`** (v2.11.1, extraite de `checkSystemHealth()` — [ADR-0029 §Amendement](../adr/0029-auto-guerison-heap.md#amendement-2026-07-28--échantillonnage-adaptatif-v2111)) — porte **tout le volet heap** (échantillonnage `maxAllocHeap`, flush one-shot, débounce, décision auto-guérison) sur un **timer propre à cadence adaptative** : 60 s en régime sain, `kHeapCheckFastIntervalMs = 5 s` dès que le plus gros bloc passe sous `kLowHeapWarnBytes` (voir [section dédiée](#cadence-adaptative-du-watchdog-heap-v2111)).
- **`checkSystemHealth()`** — toutes les `kHealthCheckIntervalMs = 60 s` : alerte heap **total** (`low_memory`, en **transition** depuis v2.11.1), watchdog de **connectivité**, ré-annonce mDNS debouncée.

Le disjoncteur history, lui, s'exécute **une seule fois au boot**, dans `HistoryManager::begin()`.

## Pourquoi deux gardes dédiées

| Mécanisme existant | Ce qu'il détecte | Pourquoi il ne voyait ni le gel heap, ni le gel réseau |
|---|---|---|
| Watchdog TWDT (`kWatchdogTimeoutSec`) | Tâche bloquée qui ne reset pas le WDT | `loopTask` / `mqttTask` **tournaient** pendant les deux gels → WDT jamais déclenché. Le thread `tcpip`/lwIP n'est de toute façon **pas** surveillé par la TWDT. |
| Alerte `low_memory` (`kMinFreeHeapBytes = 10 Ko`) | Heap **total** trop bas | La fragmentation frappe le **plus gros bloc contigu**, pas le total. Et lors du gel réseau, le heap était **sain**. |
| Coredump | Crash / panic | **Aucun panic** dans les deux cas : pas d'exception levée, blocage silencieux. |
| Auto-guérison mémoire (watchdog heap) | Fragmentation du tas | Ne voit rien lors du gel réseau : `maxBloc` ≈ 37 Ko, loin du seuil de 8 Ko. |

D'où **deux** gardes complémentaires, chacune sur sa vraie métrique :

- **Heap** : plus gros bloc contigu **réellement allouable** — `heapLargestFreeBlock8Bit()` depuis la **v2.33.0** ([ADR-0058](../adr/0058-gardes-heap-metrique-8-bits.md)) ; l'ancienne `ESP.getMaxAllocHeap()` interrogeait `MALLOC_CAP_INTERNAL`, IRAM 32 bits comprise ([ADR-0038](../adr/0038-metrique-memoire-allouable-8-bits.md)) et rendait la garde aveugle. **Distinct** de l'alerte heap total.
- **Connectivité** : sonde ICMP active vers la passerelle (le réseau répond-il de bout en bout ?), **indépendante de MQTT**.

Historique des modes de défaillance « appareil injoignable » (5 traités) : `std::bad_alloc` sur `/logs` (2.8.2, PANIC) → assert lwIP WebSocket (2.9.0, [ADR-0028](../adr/0028-migration-fork-esp32async.md), PANIC) → fragmentation du tas sans crash (2.9.1, [ADR-0029](../adr/0029-auto-guerison-heap.md), gel heap) → gel de la pile lwIP avec heap sain (2.9.2, [ADR-0030](../adr/0030-watchdog-connectivite.md), gel réseau) → **boot-loop sur partition history corrompue** (2.10.0, [ADR-0031](../adr/0031-disjoncteur-bootloop-history.md), panic à chaque boot avant WiFi).

## Cadence adaptative du watchdog heap (v2.11.1)

Coredump 2026-07-28 (v2.11.0, `bad_alloc` dans un broadcast WS) : le plus gros bloc peut s'effondrer sous ~2 Ko **entre deux ticks de 60 s** — à cadence fixe, le débounce (3 ticks = **~180 s**) ne confirme jamais avant le crash. Correctif ([ADR-0029 §Amendement](../adr/0029-auto-guerison-heap.md#amendement-2026-07-28--échantillonnage-adaptatif-v2111)) :

- Le bloc heap est **extrait** de `checkSystemHealth()` vers `checkHeapHealth()` ([`main.cpp:793`](../../src/main.cpp)) — déplacement à **diff logique nul** (une seule ligne ajoutée : export de l'échantillon `lastHeapMaxAllocSample` ; sentinelle `UINT32_MAX` avant le premier échantillon → cadence normale au boot). `checkHeapHealth()` est la **source unique** de la mesure du plus gros bloc (`heapLargestFreeBlock8Bit()` depuis v2.33.0) et du compteur `consecutiveBelow`.
- Son timer dans `loop()` ([`main.cpp:329`](../../src/main.cpp)) est cadencé par la fonction **pure** `heapSampleIntervalMs(maxAllocHeap, warnThreshold, normalMs, fastMs)` (`self_heal_logic`, +5 tests natifs) : **60 s** (`kHealthCheckIntervalMs`) si `maxAllocHeap ≥ kLowHeapWarnBytes`, **5 s** (`kHeapCheckFastIntervalMs`) en dessous → le débounce `kAutoHealDebounceChecks` (3 ticks) confirme un heap critique en **~15 s au lieu de ~180 s**.
- **Invariants ADR-0029 inchangés** : seuils, débounce, uptime minimal, budget partagé, mode maintenance, garde OTA — seule la fréquence d'observation change.

## Instrumentation du tas (v2.9.1 — trace INFO désactivée en 2.10.2, portée par `checkHeapHealth()` depuis 2.11.1)

À **chaque** tick, `maxAllocHeap = heapLargestFreeBlock8Bit()` (`ESP.getMaxAllocHeap()` jusqu'à la v2.32.x — [ADR-0058](../adr/0058-gardes-heap-metrique-8-bits.md)) est **échantillonné** — cet échantillonnage reste actif : il alimente la décision d'auto-guérison (debounce), le flush one-shot ci-dessous et, depuis v2.11.1, la cadence adaptative du timer.

La trace INFO périodique persistée :

```
Heap: libre=<getFreeHeap> maxBloc=<getMaxAllocHeap> minLibre=<getMinFreeHeap>
```

(émise toutes les `kHeapLogIntervalMs = 5 min`, ou sur chute de `maxBloc` > `kHeapLogDropBytes = 8 Ko` depuis la dernière émission — elle visualisait la fragmentation : `maxBloc` s'effondre alors que `libre` reste correct) est **désactivée depuis la v2.10.2** : le bloc d'émission est **mis en commentaire** dans `checkHeapHealth()` ([`main.cpp:805`](../../src/main.cpp)), pour ne plus consommer le journal 64 Ko avec de la télémétrie de routine. Pour ré-instrumenter (diagnostic d'un nouvel épisode de fragmentation), **décommenter le bloc** — le commentaire sur place documente la marche à suivre.

**Restent actifs** : au **premier** franchissement à la baisse de `kLowHeapWarnBytes = 12 288 o` (plus gros bloc 8 bits — était 20 000 sur la métrique brute jusqu'à la v2.32.x), un WARN « Heap fragmenté sous seuil d'alerte: maxBloc8=… » est émis **et** un `flushToDisk()` **one-shot** capture la descente sur disque même si le redémarrage n'est jamais atteint. Le déclencheur est **ré-armé** dès que `maxBloc8` repasse au-dessus du seuil ([`main.cpp:826`](../../src/main.cpp)).

### Alerte `low_memory` en transition (v2.11.1, churn-heap Q4)

L'alerte heap **total** (CRIT « Mémoire faible » + MQTT `low_memory`, seuil `kMinFreeHeapBytes`, restée dans `checkSystemHealth()` à 60 s) est passée d'une **ré-émission à chaque tick** tant que le heap restait sous le seuil (chaque CRITICAL déclenchant un `flushToDisk()` immédiat — re-flush disque toutes les 60 s au pire moment) à une émission **sur transition** uniquement (pattern edge, [`main.cpp:879`](../../src/main.cpp)) : une alerte CRIT + MQTT à l'**entrée** sous le seuil, une INFO « Mémoire rétablie » au **retour** au-dessus.

> ⚠️ **Limite assumée (revue 2.11.1)** : `publishAlert()` publie en **non-retained** — un abonné MQTT hors ligne au moment de la transition **rate l'épisode** (il n'y a plus de ré-émission périodique pour le rattraper). Le risque est borné par l'INFO de rétablissement (l'épisode est encadré dans le journal) et par le fait qu'un heap total durablement bas finit en auto-guérison, qui émet ses propres alertes.

## Trace de calibrage des seuils mémoire (v2.19.0)

Trace **INFO persistée**, émise par `checkHeapHealth()` toutes les **`kHeap8LogIntervalMs` = 30 min** (`kHeapLogIntervalMs` = 5 min jusqu'à la v2.20.1 — voir l'encadré ci-dessous). Elle **ne remplace pas** la trace « Heap: … » historique, restée en commentaire depuis la 2.10.2 : elle a une autre finalité. Format depuis la v2.20.2, en octets bruts, sans arrondi :

```
Heap 8bits: libre8=46088 maxBloc8=4596 minBoot=3060 minFen=3060 ws=1 psram=4174848
```

| Champ | Signification |
|---|---|
| `libre8` | Mémoire libre **réellement allouable** — `heap_caps_get_free_size(MALLOC_CAP_INTERNAL \| MALLOC_CAP_8BIT)` |
| `maxBloc8` | Plus gros bloc contigu **réellement allouable** (la grandeur qui compte pour un `new` de 356 o) |
| `minBoot` | **Minimum de `maxBloc8` depuis le boot** — c'est LA valeur qui sert à recalibrer les seuils (anciennement `minMaxBloc8_boot`) |
| `minFen` | Minimum de `maxBloc8` sur la **fenêtre** écoulée depuis la trace précédente : montre si les creux s'aggravent (anciennement `minMaxBloc8_fenetre`) |
| `ws` | Clients WebSocket connectés — contexte de charge, distingue régime chargé et régime calme |
| `psram` | **PSRAM libre en octets** (`ESP.getFreePsram()`), ajouté en **v2.35.0** ([ADR-0061](../adr/0061-bascule-psram-au-dela-de-4095-octets-reserve-parachute-en-interne.md)) — `0` sur module WROOM |

> **Pourquoi `psram=` a été ajouté** (bug-080 lot B-1) : depuis la bascule des allocations dépassant le seuil PSRAM (4 095 o alors, **1 024 o** depuis la v2.50.0), la **prédiction falsifiable** du correctif est que la PSRAM libre **chute à chaque chargement d'interface**. Sans ce chiffre **sur la même ligne** que `libre8` et `minFen`, la corrélation n'est pas observable.
>
> **Ce qu'il faut y lire pendant les 48 h d'observation** : `minFen` ne doit plus descendre sous **~30 000 o**, et `psram` doit **baisser de plusieurs dizaines de Ko** lors des chargements d'UI puis remonter. Une baisse de `psram` est le signe **attendu**, pas une anomalie : la PSRAM est devenue un **pool de travail**. En revanche, **si `minFen` replonge à 16-20 Ko, l'hypothèse du tampon stdio est fausse** et le diagnostic repart aux mesures.
>
> **Mise en œuvre** : `ESP.getFreePsram()` est appelé **localement** dans la trace, surtout **pas** ajouté à `HeapMetrics` — `heapMetricsSnapshot()` est appelé par `WsManager::_heapAllowsPush()` à **chaque trame**, et les `heap_caps_*` s'exécutent **interruptions coupées**. Le tampon de pile reste dimensionné à 128 caractères (pire cas ~117 avec le nouveau champ) : aucune `String` ici, cette trace s'émet **pendant** une pénurie.

> **v2.20.2 — cadence 5 min → 30 min et partie `interne:` RETIRÉE.** Les champs `interne: libre=… maxBloc=…` (mesure brute) étaient **figés** — `maxBloc = 27 636` à **chaque** relevé pendant 5 h. Ils ont servi **une fois**, à prouver l'écart entre les deux métriques ([ADR-0038](../adr/0038-metrique-memoire-allouable-8-bits.md)) ; la preuve est faite et consignée. Les conserver coûtait ~70 octets par ligne sur un journal persistant de 8 Ko dont la rétention était tombée à ~30 min — c'est ce qui a **détruit le post-mortem du crash du 15/08**. L'écart entre les deux familles reste consultable à tout instant via `/get-system-info` (`max_alloc_8bit` / `max_alloc_internal`). Bilan chiffré de la rétention : [logger.md](logger.md#rétention-réelle-du-journal-persistant-v2202).

Deux points de mise en œuvre à connaître :

- **Le minimum glissant n'est pas échantillonné à la cadence de la trace.** `minBoot` / `minFen` sont mis à jour à **chaque tick** de `checkHeapHealth()` (60 s en régime sain, `kHeapCheckFastIntervalMs` = 5 s sous seuil d'alerte), pas toutes les 30 min : un creux transitoire entre deux traces serait sinon **invisible**, et c'est précisément le creux qui dimensionne le seuil de redémarrage.
- **Aucune décision ne dépend de cette trace.** C'est un instrument de calibrage et de surveillance de dérive, pas une garde. Depuis la v2.33.0, les gardes lisent **la même métrique 8 bits** que la trace ([ADR-0058](../adr/0058-gardes-heap-metrique-8-bits.md)) — c'est cette trace qui a fourni les valeurs du recalibrage (nominal fragmenté stable 21 492 o sur 2.26→2.32, creux transitoires 1 268 o).

> ⚠️ **Ne pas confondre les deux colonnes en lisant le journal.** Un écart d'une dizaine de kilo-octets entre `maxBloc8` et `maxBloc` est **normal** : c'est le reliquat d'IRAM 32 bits que l'ancienne métrique comptait à tort. Un `maxBloc` figé à la même valeur pendant des heures ou à travers un redémarrage n'est **pas** un signe de stabilité, c'est la signature du piège ([ADR-0038](../adr/0038-metrique-memoire-allouable-8-bits.md)).

**La bascule des gardes a été faite en v2.33.0** ([ADR-0058](../adr/0058-gardes-heap-metrique-8-bits.md)) sur les relevés propres 2.26→2.32 de cette trace. L'échelle contractuelle en métrique 8 bits : `2 048 (repli) < 4 096 (auto-heal) < 6 144 (skip WS) < 12 288 (WARN) < 18 432 (réarmement nominal) < 21 492 (nominal terrain)`, débounce maintenu à 3. La trace reste émise pour surveiller la **dérive** du régime : si `minFen` s'installe durablement sous 12 288, les creux s'aggravent.

> **Historique** : la règle `clamp(0,40 × min(maxBloc8) ; 8192 ; 16384)` d'[ADR-0038](../adr/0038-metrique-memoire-allouable-8-bits.md) avait été déclarée **inapplicable** en v2.20.2 (min mesuré 3 060 o → plancher 8 192 au-dessus du régime normal → mode maintenance atteignable) et la bascule refusée par NO GO ([ADR-0040](../adr/0040-pas-de-calibrage-sur-fragmentation-pathologique.md), superseded). La révocation du NO GO repose sur un autre argument : **l'immunité vient du débounce temporel, pas du niveau du seuil** — voir [ADR-0058](../adr/0058-gardes-heap-metrique-8-bits.md).

## Politique de bascule PSRAM des allocations ordinaires (v2.50.0, feature-124 / S7)

`heap_caps_malloc_extmem_enable(kPsramExtmemThresholdBytes)` est appelé au
`setup()`, **juste après `initOomGuard()`** ([`main.cpp`](../../src/main.cpp)) et
avant tout log (`systemLogger.begin()` n'a pas encore eu lieu). Il fixe le seuil
au-delà duquel les allocations `malloc` / `new` **ordinaires** préfèrent la PSRAM.

| Version | Seuil | Classe effectivement déportée |
|---|---|---|
| v2.35.0 → v2.49.3 (lot B-1, [ADR-0061](../adr/0061-bascule-psram-au-dela-de-4095-octets-reserve-parachute-en-interne.md)) | **4 095 o** | « exactement 4 096 o » — le tampon stdio VFS, et rien d'autre |
| **v2.50.0** ([ADR-0078](../adr/0078-bascule-psram-cesse-d-etre-chirurgicale.md)) | **1 024 o** | tout ce qui **dépasse** 1 024 o, dont les **pbufs lwIP** (~1 600 o) |

**La règle codée est INCLUSIVE** : « taille **≤** seuil ⇒ RAM interne ». Seul ce
qui **dépasse** le seuil part en PSRAM d'abord (désassemblage de la `libheap.a`
liée, `bgeu limit, size`). Un raisonnement qui perd ce sens conclut le contraire
de ce qu'il énonce : c'est ce qui avait d'abord fait retenir 2 048, où
`1 622 ≤ 2 048` serait resté **interne** et le lot n'aurait rien déporté.

La bascule reste une **préférence, pas une contrainte** : si la PSRAM refuse,
l'allocateur retombe en RAM interne — aucun échec supplémentaire n'est créé. Sur
un module sans PSRAM (WROOM), `psramFound()` est faux et l'appel est **sauté** :
binaire unique, comportement inchangé.

### L'encadrement de la valeur — les deux bornes et leur motif

- **Borne haute — strictement sous 1 600.** Les pbufs lwIP pèsent ~1 600 o, à
  raison d'**une structure par connexion TCP** : c'est la classe qui fragmente le
  tas interne sous six requêtes parallèles. Elle ne bascule que si
  `1 600 > seuil`.
- **Borne basse — strictement au-dessus de 512.** Les caches de LittleFS (512 o)
  et le pool FILE de la libc (428 o) doivent **rester en RAM interne** : ce sont
  eux qui côtoient les **écritures flash**, pendant lesquelles le cache
  flash/PSRAM est **coupé** — le vrai danger de ce lot. Au-dessus de 512
  l'exposition est **quantifiée** ; en dessous elle change de **nature**. Le gate
  `pool-chemistry` interdit donc de planifier un palier sous 512.
- **Fenêtre utile : [512 ; 1 599] → 1 024.**

Cinq `static_assert` de [`constants.h`](../../src/constants.h) verrouillent ces
relations à la **compilation** :

| Prédicat | Ce qu'il empêche |
|---|---|
| `seuil < kVfsStdioBufferBytes` | un seuil ≥ 4 096 ramènerait le tampon stdio VFS en RAM interne et rouvrirait `bug-080` à l'identique (acquis du lot B-1). ⚠️ L'ancienne forme `seuil + 1 == kVfsStdioBufferBytes` épinglait le seuil à 4 095 : **abandonnée à dessein** |
| `seuil >= 512` | le franchissement de la borne basse (caches LittleFS / pool FILE en PSRAM) |
| `kOomReserveBytes > seuil` | rend **nécessaire** le `MALLOC_CAP_INTERNAL` explicite d'`oom_guard.cpp` : une réserve de 12 288 o prise par `malloc()` partirait en PSRAM et ne sauverait rien d'une famine de RAM interne |
| `kOomFallbackReserveBytes <= seuil` | condition **C3** du gate : la réserve de **repli** vaut 1 024 o, soit **exactement** le seuil — plus aucune marge. Un palier abaissé la ferait basculer sans alerte |
| `kHistoryImportChunkBudgetBytes > seuil` | découper l'import d'historique « plus fin pour faire plus sûr » ferait repartir les lots dans le tas interne affamé |

### Pourquoi lwIP est soumis à CE seuil

`lwipopts.h:1712` retient `#define mem_clib_malloc malloc`, avec
`MEM_LIBC_MALLOC=1` et `MEMP_MEM_MALLOC=1`, parce que
`CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP` n'est **pas** activé. Toutes les
structures lwIP passent donc par le `malloc` ordinaire — donc par la politique
`extmem`.

### ⚠️ Ce que ce lot NE fait PAS

Il ne déplace **aucun tampon WiFi**. Les allocations qui échouent demandent
`caps=0x080C` (`8BIT|DMA|INTERNAL`) **explicitement**, pour **1 622 o** : elles
**contournent** ce seuil, et le contourneront toujours. Le lot ne les déplace pas,
il leur **fait de la place** en déportant tout ce qui passe par le `malloc`
ordinaire. C'est une promesse plus faible qu'un correctif, et elle doit être
énoncée ainsi.

### Le critère de succès est binaire — et non vérifié à ce jour

Référence relevée le **09/09/2026 à 07:10** sur la **v2.49.3**, par reproduction
volontaire de l'utilisateur, instrument complet pour la première fois :

| Grandeur | Valeur |
|---|---|
| **Plancher fin `hf.i8.mb`** | **1 076 o** |
| **Régime au creux** (`ctxAtMin`) | **`0x02` = rafale HTTP** — attribution sans ambiguïté |
| Sondes d'échec | **948**, 1 204, 1 268 o |
| Planchers `0x080C` / `0x1804` (résolution 8 s) | **20 468 o** — ils n'ont **rien vu** (facteur **19**) |
| Épisode 503 | déclenché à **2 932**, **24 refus en 35 s** |

⭐ **Le chiffre de 2 932 n'était pas le creux.** C'est le **seuil de
déclenchement** que la garde 503 observe **au moment où elle refuse**. Le creux
réel est **presque trois fois plus bas**, et c'est lui qui explique le défaut :
**1 076 < 1 622**, la taille du tampon TX WiFi — l'échec est arithmétique.

**Critère de succès** : faire remonter le plancher fin `hf.i8.mb`
**au-dessus de 1 622 o** sous une reproduction volontaire identique. Ce n'est pas
une amélioration à apprécier, c'est un seuil franchi ou non.

> ⚠️ **Non vérifié à ce jour.** Ce critère ne se mesure **que sur cible**, après
> déploiement. L'**échec est un résultat possible et prévu** : le seuil ne
> gouverne pas les allocations à caps explicites. Protocole :
> [§ Protocole de relevé sur cible](#protocole-de-relevé-sur-cible).


## Planchers glissants multi-masques (v2.49.0, feature-122 lot C-1a)

> ⚠️ **C'est un INSTRUMENT DE MESURE, pas un correctif.** Aucune réserve DMA
> n'est posée, aucune garde n'est modifiée, aucun seuil ne bouge. La métrique de
> toutes les gardes du projet — refus 503 des fichiers statiques
> ([`web_server.cpp`](../../src/web_server.cpp)), garde d'ouverture de fichiers
> ([`fs_guard.cpp`](../../src/fs_guard.cpp)), parachute OOM, gardes TLS — **reste
> `0x0804`**. La bascule éventuelle vers `0x1804` appartient à `feature-123`
> (lot C-1b), sous [ADR-0077](../adr/0077-metrique-de-garde-exclut-les-pools-inatteignables.md).

Modules : [`src/heap_caps_logic.h`](../../src/heap_caps_logic.h) (**pur** —
masques, `heapMaskSelectsHeap`, plancher glissant, `fetch_min`, seaux de durée,
fenêtre de contexte, cadence du tour de rôle) + coquille impure dans
[`src/heap_metrics.cpp`](../../src/heap_metrics.cpp) (atomiques, tour de rôle,
chronométrage) et [`src/heap_ring.cpp`](../../src/heap_ring.cpp) (cadence,
composition du contexte). Tests natifs : `test/test_native_heap_caps_table/` et
`test/test_native_heap_floors/`.

### La table des cinq masques — et le piège qu'elle sert à mesurer

**Le point central, sans détour : `0x0804`, la métrique ACTUELLE des gardes,
VOIT une réserve DMA.** C'est exactement ce que ce lot sert à mesurer, et la
raison pour laquelle poser une réserve sans basculer la métrique **aveuglerait la
garde 503** — elle lirait le gros bloc libre de la réserve pendant que le pool
général resterait affamé.

| Masque | DRAM interne | PSRAM | Réserve DMA | Plus gros bloc mesuré ? |
|---|---|---|---|---|
| `0x0804` `INTERNAL\|8BIT` — métrique **actuelle** des gardes | voit | ignore | **voit** ⚠️ | oui (O(blocs)) |
| `0x080C` `INTERNAL\|DMA\|8BIT` — classe d'échec observée (ce que le WiFi demande) | voit | ignore | voit | oui (O(blocs)) |
| `0x1000` `DEFAULT` seul | voit | **voit** ⚠️ | ignore | **non** |
| `0x1804` `INTERNAL\|8BIT\|DEFAULT` — candidate de `feature-123` | voit | ignore | ignore ✅ | oui (O(blocs)) |
| `0x0400` `MALLOC_CAP_SPIRAM` — plancher PSRAM (A6) | ignore | voit | ignore | **non** |

Cette table est corroborée par **deux voies indépendantes** :

1. **Dérivation** de la règle de l'IDF `(all_caps & caps) == caps`, déjà
   documentée dans [`src/oom_guard_logic.h`](../../src/oom_guard_logic.h) et mise
   sous test à 12 cas (4 masques × 3 tas) dans
   `test/test_native_heap_caps_table/`.
2. **Source IDF** de `esp_psram_extram_reserve_dma_pool()` :
   `caps[] = {0, MALLOC_CAP_DMA|MALLOC_CAP_INTERNAL, MALLOC_CAP_8BIT|MALLOC_CAP_32BIT}`
   → `all_caps = 0x080E`, **sans `DEFAULT`** — d'où « `0x0804` voit,
   `0x1804` ignore ».

Elle est **figée en compilation** : `heapFloorsPredMask()` recalcule les 12 bits
et `static_assert(… == 0x02ED)` casse le **build** si `== caps` devient `!= 0`,
ou si `0x1804` redevient `0x0804`.

> ⚠️ **`0x1000` et `MALLOC_CAP_SPIRAM` n'ont PAS de « plus gros bloc ».** Sans bit
> `INTERNAL`, `heapProbeForCaps()` prend la branche **O(régions)** imposée par
> `oomProbeUsesRegionScan` (condition 2 du gate `pool-chemistry`, bug-080 lot B-2
> — borne *a priori* contre un parcours de 4 Mo interruptions coupées). Leur
> `largestBlockBytes` vaut la sentinelle `kOomProbeBlockUnknown` (4 194 240), qui
> n'entre **jamais** dans un plancher et sort en **`null`** côté HTTP
> ([ADR-0072](../adr/0072-absence-de-valeur-hors-du-domaine.md)) : publiée telle
> quelle, elle afficherait un bloc libre de 4 Mo, faux et parfaitement crédible —
> le mode de défaillance exact que ce dossier combat.

### Ce qui est retenu par emplacement

Pour chacun des **cinq** emplacements : le dernier échantillon, le **minimum
depuis le démarrage** et le **minimum de fenêtre** (réarmé après chaque report),
**sur le libre ET sur le plus gros bloc**, plus l'**octet de contexte au creux**
et le nombre d'échantillons pris pour ce masque.

Un minimum n'est **jamais** un instantané : un échantillonnage à cadence fixe
rate par construction une pointe de quelques millisecondes (cas limite explicite
de la spec). D'où deux dispositifs :

- **Repliement par `min`** (`heapFloorUpdate`, pur), jamais un `store` ;
- **`fetch_min` atomique** (`heapAtomicFetchMin`, composé à la main —
  `<atomic>` n'en fournit pas) pour le plancher `0x0804`, qui a désormais **trois
  écrivains** : le filtre de la garde 503 sur `async_tcp`, la garde d'ouverture
  de fichiers depuis n'importe quelle tâche, et la loop. Un `load`-`compare`-
  `store` y **perdrait** un minimum (A charge 30 Ko, B charge 30 Ko, A stocke
  12 Ko, B stocke 20 Ko → le creux de 12 Ko a disparu).

**Résolutions temporelles — à ne pas confondre :**

| Grandeur | Cadence | Où la lire |
|---|---|---|
| Plancher `0x0804` à la **requête** (chaque évaluation du filtre 503 et de la garde `fopen`) | par requête | `minBoot` / `minFen` de la ligne `Heap 8bits:` |
| Planchers `hf` de `0x0804`, `0x1000`, SPIRAM | **2 s** (`kHeapRingSampleIntervalMs`) | `?hf=1`, lignes `Heap masq …` |
| Planchers `hf` de `0x080C` et `0x1804` | **8 s** (tour de rôle mesuré **un échantillon sur deux** depuis la v2.49.1 — `kHeapFloorWalkEverySamples = 2`) | idem |

> ⚠️ **La résolution du tour de rôle est passée de 4 à 8 s en v2.49.1**, en
> réponse à la durée réelle des parcours mesurée le 08/09 (**516 µs**, six fois
> le repère de ~84 µs sur lequel le gate avait raisonné, et au-dessus des 300 µs
> de sa condition 5). Le masque supplémentaire n'est plus mesuré qu'**un
> échantillon sur deux** : cadence du tour de rôle **2 s → 4 s**, période de
> revisite d'un masque donné **4 s → 8 s**. Conséquence de lecture, à connaître
> avant de dimensionner `feature-123` : **le plancher `0x080C` a une résolution
> de 8 s**. Atténuation — `heaps(0x080C) ⊆ heaps(0x0804)`, dont le plancher reste
> mesuré à **chaque** échantillon (et à chaque requête) : il **borne
> supérieurement** celui de `0x080C`, donc dimensionner sur `0x0804` reste
> **conservateur**.

### Contexte au creux (A5) — quel événement tenait la machine

Bits `kHeapCtx*`, **purement observationnels** : aucune décision ne les relit
(ni dosage, ni reboot, ni refus 503).

| Bit | Valeur | Signification | Source |
|---|---|---|---|
| `kHeapCtxWsClients` | `0x01` | au moins un client WebSocket connecté | `wsManager.clientCount()` |
| `kHeapCtxHttpBurst` | `0x02` | des GET ont été servis depuis l'échantillon précédent | delta de `heapHttpGetCount()` |
| `kHeapCtxDosing` | `0x04` | au moins une pompe doseuse **sous tension** | `PumpController.isAnyPumpEnergized()` (lecture seule) |
| `kHeapCtxMqttDiscovery` | `0x08` | rafale de découverte HA **en cours ou récente** | `mqttManager.lastDiscoveryMarkMs()` + `kMqttDiscoveryCtxWindowMs` = **60 s** |

La fenêtre de 60 s existe parce qu'une rafale republie ~54 entités et dure bien
plus qu'un échantillon : le repère est posé à l'**ouverture** *et* à la
**fermeture** de `publishDiscovery()`, la fenêtre courant depuis la fin — sans
quoi un creux causé par la rafale serait attribué au régime calme qui la suit.
Voir [mqtt-manager.md](mqtt-manager.md#lastdiscoverymarkms--repère-de-rafale-pour-le-contexte-mémoire-v2490).

#### ⚠️ DEUX accumulateurs de contexte, et le nom seul ne les distinguait pas

| Champ | Portée | Réarmé ? | À quoi il répond |
|---|---|---|---|
| `ctxSeen` → `hf.ctx`, `ctxVu=` du journal | **fenêtre courante** | **oui**, par `heapFloorsRearmWindow()` à chaque trace agrégée (30 min) | « quels régimes depuis le dernier report ? » |
| `ctxSeenBoot` → `hf.ctxb` (**v2.49.1**) | **depuis le démarrage** | **jamais** | « quels régimes cet appareil a-t-il connus ? » |

Le second existe à cause d'un **contresens de lecture réel** : au relevé du
08/09, `hf.ctx` valait **1** après 8 h 49 de fonctionnement, ce qui se lisait
comme « un seul régime jamais rencontré en 8 h 49 » — **faux**. Le champ ne
portait que la fenêtre de 30 minutes en cours. Un test interdit désormais au
réarmement de toucher `ctxSeenBoot`.

**Pour la question « quels régimes cet appareil a-t-il vus », lire `ctxb`, pas
`ctx`.**

Le refus `fs_guard` n'a pas de bit propre : il **alimente** le plancher `0x0804`
(`heapNoteMaxAlloc8Sample` depuis `fsGuardAdmit` / `fsGuardDrain`, sans parcours
neuf), et sa trace reste celle de [acces-fichiers.md](acces-fichiers.md).

### Témoin `malloc(seuil + 1)` + témoin de contrôle `malloc(seuil)` (A7)

Deux `malloc()` au `setup()`, **libérés aussitôt** (aucune rétention), pour
trancher une question ouverte : **la bascule PSRAM est-elle opérante ou
décorative ?** `heap_caps_malloc_extmem_enable(kPsramExtmemThresholdBytes)`
(v2.35.0, [ADR-0061](../adr/0061-bascule-psram-au-dela-de-4095-octets-reserve-parachute-en-interne.md))
devait envoyer les tampons stdio de 4 096 o en PSRAM — or `psram_free` est
**immobile à 4 180 340 dans 152 relevés**.

- `malloc(kPsramExtmemThresholdBytes + 1)` = la **plus petite** classe que la
  politique doit déporter, donc la seule qui change de camp **exactement au
  seuil** — **attendue en PSRAM** ;
- `malloc(kPsramExtmemThresholdBytes)` = témoin de **contrôle**, attendu
  **interne** dans les deux configurations (test inclusif).

> ⚠️ **Les tailles sont RELATIVES au seuil depuis la v2.50.0** (condition **C4**
> du gate `pool-chemistry`, feature-124). Le couple s'appelait « 4096 / 4095 »
> parce que le seuil valait 4 095 et que `4 096 = seuil + 1` par coïncidence de
> calibrage. **À seuil 1 024, un `malloc(4095)` part en PSRAM** : le témoin de
> contrôle aurait affiché « externe » et le lecteur y aurait lu une **anomalie
> inexistante**. Un témoin mensonger est pire qu'un témoin absent.
> L'ancien couple 4096/4095 est donc le **cas particulier** de la forme relative,
> à l'ancien seuil : la nouvelle expression est exactement l'ancienne,
> **généralisée**, et elle garde son pouvoir discriminant à **tout** seuil futur.
> La classe historique du tampon stdio VFS (4 096 o) reste couverte **a
> fortiori**, puisque `4 096 > seuil`.

Le couple donne une signature non ambiguë : **si les deux tombent du même côté,
la politique d'allocation ne fait rien**. `malloc()` et jamais
`heap_caps_malloc()` avec des caps explicites — celles-ci contourneraient la
politique qu'on éprouve. Le classement d'adresse croise **deux** avis
(`heapPtrIsExternal` sur `SOC_EXTRAM_DATA_LOW/HIGH`, `esp_ptr_external_ram`, plus
`esp_psram_check_ptr_addr` si disponible) et leur **discordance est un résultat
rapporté**, pas une erreur masquée.

| Bit | Valeur | Signification | Nom jusqu'en v2.49.3 |
|---|---|---|---|
| `kHeapWitnessSkipped` | `0x01` | pas de PSRAM (module WROOM) → témoin sauté | inchangé |
| `kHeapWitnessAboveExternal` | `0x02` | le `malloc(seuil + 1)` est tombé en **RAM externe** (attendu) | `kHeapWitness4096External` |
| `kHeapWitnessAtThresholdExternal` | `0x04` | le `malloc(seuil)` aussi — **ANOMALIE** (le contrôle a bougé) | `kHeapWitness4095External` |
| `kHeapWitnessDisagreement` | `0x08` | les deux API de classement ne s'accordent pas | inchangé |
| `kHeapWitnessAboveFailed` / `…AtThresholdFailed` | `0x10` / `0x20` | l'allocation a échoué → **aucun verdict** | `kHeapWitness4096Failed` / `…4095Failed` |
| `kHeapWitnessNoPsramApi` | `0x40` | `esp_psram_check_ptr_addr` indisponible (qualifie la confiance, ne contredit pas le verdict) | inchangé |

**Lecture attendue si la politique est opérante** : `w = 0x02` (`w96 = true`,
`w95 = false`). **Si décorative** : `w = 0x00` (les deux internes).

> ✅ **Les clés JSON `w96` / `w95` NE BOUGENT PAS** — ni les positions de bits.
> Seuls les **noms des constantes C++** ont changé, ce qui est interne et sans
> effet sur le fil. Motif explicite : le relevé du **09/09/2026** a été pris
> **avant** ce lot, et tout l'intérêt du comparatif avant/après tient à ce que les
> bits n'aient pas changé de place et que les clés n'aient pas été renommées. Un
> décalage de bit rendrait les deux relevés **incomparables sans rien casser par
> ailleurs**. `w96` reste le verdict de la classe **déportée** (`seuil + 1`),
> `w95` celui du témoin de **contrôle** (`seuil`). Un test natif épingle les sept
> positions de bits **en littéraux** et le câblage des deux clés.

> ✅ **VERDICT RELEVÉ LE 08/09/2026 : le lot B-1 est CONFIRMÉ OPÉRANT.** Le
> `malloc(4096)` est tombé en **PSRAM**, le témoin de contrôle `malloc(4095)` en
> **interne**, et **aucune discordance** n'a été rapportée entre
> `esp_psram_check_ptr_addr` et `esp_ptr_external_ram`.
> `heap_caps_malloc_extmem_enable(4095)` (v2.35.0,
> [ADR-0061](../adr/0061-bascule-psram-au-dela-de-4095-octets-reserve-parachute-en-interne.md))
> fait donc bien ce pour quoi il a été livré : le soupçon de « correctif
> décoratif » est **levé**, après être resté ouvert depuis le 05/09.
>
> **Et `psram_free` était bien un mauvais témoin** : son immobilité à 4 180 340
> sur 152 relevés ne disait rien. Le **plancher SPIRAM** — un minimum, pas un
> instantané — montre un creux de **5 764 o** que l'échantillonnage à 30 s de la
> trace ne pouvait pas voir. C'est l'illustration exacte du motif de A6.
>
> ⚠️ La réserve de portée reste entière : le témoin tranche où tombe un
> `malloc(seuil + 1)` **au démarrage, PSRAM quasi vide** — pas où tombe le tampon
> stdio en pleine rafale, sous pression PSRAM, quand la préférence `extmem` peut
> retomber silencieusement en interne.
>
> 📋 **Ce verdict a été relevé à seuil 4 095** : les tailles allouées valaient
> alors 4 096 et 4 095 o, soit `seuil + 1` et `seuil`. Il reste donc directement
> comparable au verdict d'après la v2.50.0, où les mêmes bits portent 1 025 et
> 1 024 o.

### Coût réel — énoncé sans arrondi

Le plan annonçait un « coût net nul ». **C'était faux** : il supposait
`heapFree8Bit()` coûteuse en O(blocs), alors qu'elle est O(régions). La
comptabilité honnête :

| | Parcours **O(blocs)** par échantillon | Agrégats O(régions) |
|---|---|---|
| Avant ce lot | **1** (`heapLargestFreeBlock8Bit`) | 1 (`heapFree8Bit`) |
| Après ce lot (v2.49.0) | **2** (référence `0x0804` + un masque en tour de rôle) | 2 (`0x1000`, SPIRAM) |
| Depuis la **v2.49.1** | **1,5 en moyenne** (référence à chaque échantillon + tour de rôle un échantillon sur deux) | 2 (`0x1000`, SPIRAM) |

À la cadence de 2 s, la v2.49.0 faisait **1 parcours O(blocs) par seconde —
exactement le plafond** fixé par la condition 3 du gate `pool-chemistry`, **sans
marge**. La **v2.49.1** divise la cadence du tour de rôle par deux
(`kHeapFloorWalkEverySamples = 2`) : coût moyen **1,5 parcours par échantillon,
soit 0,75/s** — sous le plafond, **avec de la marge pour la première fois**.
Verrouillé par deux `static_assert` (`kHeapFloorWalksPerSample × 1000 ≤
kHeapRingSampleIntervalMs × kHeapFloorWalkEverySamples` dans
[`constants.h`](../../src/constants.h) — le plafond est désormais exprimé **en
moyenne** —, et `kHeapFloorWalksPerSample == 2` dans `heap_metrics.cpp`) : ni
accélérer la cadence, ni ajouter un 3ᵉ masque à parcours complet. Un
`static_assert` interdit par ailleurs un diviseur **nul**, qui ferait disparaître
le tour de rôle — `0x080C` et `0x1804` ne seraient plus **jamais** mesurés.

> **Le gate a RETIRÉ sa validation initiale et maintenu son GO — pour une
> meilleure raison.** Ce qui menace la régulation n'est pas le **taux
> d'occupation** du CPU par la mesure, c'est la **durée d'une occurrence
> interruptions coupées** (~84 µs **supposés** à l'époque du gate — la mesure sur
> cible a donné **516 µs**, cf. ci-dessous). Un plafond de taux ne dit rien de
> cette durée : c'est elle qu'il faut mesurer, d'où le chronométrage ci-dessous.

### Durée des parcours — `walkMaxUs` / `walkMedianUs` (condition 5 du gate)

Le parcours de **référence** `0x0804` est chronométré à chaque échantillon.

- **Horloge : `esp_timer_get_time()` et NON `CCOUNT`.** Le compteur de cycles est
  **par cœur** : une migration de tâche entre les deux lectures rendrait la
  soustraction absurde (piège déjà rencontré, `kOomProbeDurationInvalid`).
- ⚠️ **La médiane est APPROCHÉE, et son biais doit être lu comme tel.**
  Histogramme à **7 seaux dyadiques** (bornes supérieures 32 / 64 / 128 / 256 /
  512 / 1024 µs, puis **hors échelle** = `65535`), sans tri, sans allocation,
  sans flottant. La valeur rendue est la **borne supérieure du seau contenant le
  rang médian** ⇒ **surestimation bornée par la largeur du seau**, donc
  **conservatrice** pour un plafond de coût. Le seau rendu doit **toujours** se
  lire « **≤ valeur** », jamais comme une durée exacte.
- **Jamais réarmées** : `walkMaxUs` et l'histogramme gagnent en sens avec le
  nombre d'échantillons (contrairement aux planchers de fenêtre).
- **Publiées par la route depuis la v2.49.1** : clés `hf.wmax` et `hf.wmed`.
  Elles étaient **mesurées dès le premier jour mais jamais exposées** — le relevé
  du 08/09 n'a pu les lire que dans le journal, donc la condition 5 du gate
  n'était satisfaite que **par accident**. Voir
  [API.md](../API.md#paramètre-hf--planchers-mémoire-multi-masques-v2490-additif-conditionnel).

> ⚠️ **MESURE SUR CIBLE DU 08/09/2026 — le repère de ~84 µs était faux d'un
> facteur six.** Sur 8 h 49 et 12 673 échantillons : **`walkMaxUs` = 516 µs**,
> médiane **≤ 512 µs**. C'est **au-dessus des 300 µs** de
> `kOomProbeWarnDurationUs`, donc la condition 5 du gate **se déclenche**. La
> réponse livrée en v2.49.1 n'est pas d'accélérer ni de renoncer à un masque,
> mais de **baisser la fréquence** : la durée d'**une** occurrence reste ce
> qu'elle est (elle est imposée par le nombre de blocs du tas), c'est le **nombre
> d'occurrences par seconde** que le gate demandait de réduire.

> ⚠️ **Ne pas confondre avec `oom_probe_max_us`.** Celui-ci mesure la sonde du
> **chemin d'échec d'allocation** — autre fréquence, autre état de tas, autre
> grandeur. Voir [oom-guard.md](oom-guard.md).

### ⚠️ `hf.pred` ne porte AUCUNE information d'exécution

`pred` publie les 12 bits de la table de sélection, valeur attendue **`0x02ED`
(749)**. **Réserve explicite de la revue** : cette valeur est *constant-folded*
par le compilateur — l'objet contient littéralement `movi a8, 0x2ed`. Elle est
donc **constante par build** et ne peut ni confirmer ni infirmer quoi que ce soit
sur cible.

**Ne jamais la lire comme une observation.** Ce qui la protège n'est pas son
affichage mais `static_assert(heapFloorsPredMask() == 0x02ED)` : **le build est
l'organe d'application**. Un `pred` différent de 749 dans une réponse signifie
qu'on interroge un firmware dont la table a changé — pas qu'un tas s'est
comporté autrement.

### Répartition des deux chemins de lecture

| Chemin | Rôle | Coût mémoire | Contenu |
|---|---|---|---|
| `GET /get-system-info?hf=1` | **confort de temps calme** — relevé complet, demandé à la main | **1 741 o** (v2.49.1 ; 1 694 o en v2.49.0) sérialisés dans une `String` ⇒ **exige un bloc contigu** du pool 8 bits | les 5 emplacements, planchers boot **et** fenêtre, contextes (`ctx` **et** `ctxb`), durées (`wmax`/`wmed`), `pred`, `hg`, témoin |
| Les **deux lignes de journal** `Heap masq …` | **dernier recours en pleine famine** | `snprintf` sur `char[160]` de **pile** ⇒ **aucun bloc contigu requis** | planchers de `0x0804`, `0x080C`, `0x1804`, PSRAM + contexte + témoin + durées |

**Motif chiffré, et il n'est pas théorique** : le 08/09/2026,
`/get-system-info` répondait encore en **200** alors que la page statique tombait
en **503** — parce que **964 o** passaient encore avec un `maxBloc8` à **5 108 o**.
Et le `maxBloc8` observé descend jusqu'à **852 o** au pire. À 1 741 o publiés par
défaut, il existerait une bande de fragmentation où la route de diagnostic
échouerait **alors qu'elle passait avant ce lot** : l'instrument rendu
indisponible exactement dans le régime qu'il sert à étudier. D'où le
conditionnement par `?hf=1` — **à ne pas « simplifier » en inconditionnel**.
Sans le paramètre, la réponse est identique **à l'octet près** à ce qu'elle était
avant ce lot (964 o).

### Les deux lignes de journal agrégées

Émises par `checkHeapHealth()` à la **même cadence** que la trace
`Heap 8bits:` (`kHeap8LogIntervalMs` = **30 min**), soit **~+0,55 Ko/h** —
jamais une ligne par échantillon ([ADR-0033](../adr/0033-budget-espace-partition-history.md) :
la partition `history` n'offre que ~25-35 min de rétention utile au journal).

```
Heap masq boot: m804=26112/852 c=0x03 m80C=24576/1024 c=0x03 m1804=25600/900 c=0x0B mPS=4180340 tem=0x00
Heap masq fen: m804=31744/4096 m80C=30720/3584 m1804=31232/3900 ctxVu=0x0F walk=132/128us
```

| Champ | Signification |
|---|---|
| `m804` / `m80C` / `m1804` | plancher `libre`/`plus gros bloc` du masque — `boot` = depuis le démarrage, `fen` = depuis le report précédent |
| `c=0x..` | bits `kHeapCtx*` **au creux du plus gros bloc** de ce masque |
| `mPS` | plancher du **libre PSRAM** (A6) — même grandeur que `psram_free`, mais en **minimum** : une allocation PSRAM transitoire est invisible sur un instantané |
| `tem` | bits `kHeapWitness*` du témoin de bascule PSRAM (`seuil + 1` / `seuil`, v2.50.0 — voir A7) |
| `ctxVu` | **OU** de tous les contextes vus dans la fenêtre |
| `walk=max/med us` | durée observée du parcours `0x0804` (médiane **surestimée**, cf. ci-dessus) |
| `4294967295` | **aucun échantillon** pour ce plancher (`kHeapFloorEmpty`) |

Le « plus gros bloc » de `0x1000` et de la PSRAM n'est **jamais** affiché : ces
masques n'en mesurent pas.

> **Pourquoi deux lignes neuves et pas l'élargissement de `Heap 8bits:`** : ce
> tampon est à ~117 caractères sur 128 dans son pire cas, et cette ligne est la
> **seule série longue** dont dispose le projet (152 relevés historiques). La
> tronquer pour y ajouter des champs coûterait la série de référence.

> ⚠️ **Fragilité à connaître avant de toucher ces lignes.** Le pire cas de
> `ligne2` est **142 caractères** sur 160 (**143 octets**, NUL compris) — et il ne
> tient **que** parce que les octets de contexte sont des `uint8_t`, imprimés en
> `%02X`. Élargis à 32 bits, la ligne passerait à **167 octets** → **troncature
> silencieuse**, et le champ perdu serait `tem=`, **le plus à droite** : le témoin
> A7, c'est-à-dire précisément le résultat qu'on attend de ce lot. Une
> **sentinelle de types** verrouille ces champs en `uint8_t`
> (`test_journal_context_fields_are_bytes`, sur la source **dépouillée de ses
> commentaires** — la version brute était satisfiable par un simple commentaire).

### ⚠️ RELEVÉ SUR CIBLE DU 08/09/2026 (21:33) — ce que l'instrument a rapporté

**Conditions** : 8 h 49 de fonctionnement continu, **12 673 échantillons**,
**zéro redémarrage**, journal du 08/09 **12:44 → 21:16**.

| Grandeur | Relevé | Lecture |
|---|---|---|
| Témoin A7 | `malloc(4096)` → **PSRAM**, `malloc(4095)` → **interne**, **aucune discordance** | lot B-1 **confirmé opérant** (cf. section A7 ci-dessus). Relevé **à seuil 4 095**, donc `seuil + 1` / `seuil` — comparable au relevé d'après la v2.50.0 |
| `walkMaxUs` / médiane | **516 µs** / **≤ 512 µs** | **six fois** le repère de ~84 µs, **au-dessus des 300 µs** de la condition 5 du gate ⇒ elle **se déclenche** |
| Épisodes 503 / échecs d'allocation | **zéro** / **zéro** | contre **3 épisodes** et **46 échecs** sur les 23 h précédentes |
| `minBloc8` | **7 924 o** | **sous** le seuil de garde de **8 192 o**, et **sans un seul 503** |
| `ctxAtMin` au creux | **`0x01`** | seul le bit **client WebSocket** était levé : ni dosage, ni rafale HTTP, ni découverte MQTT |
| Plancher SPIRAM | creux à **5 764 o** | `psram_free`, immobile sur 152 relevés, ne pouvait pas le voir |
| Trajectoire de `maxBloc8` | **42 996** (12 h) → **24 564** (13 h) → **21 492** (14–18 h) → **12 276** (19–21 h) | décroissance par paliers, sur une seule journée |
| Déconnexions MQTT | **5** : 12:57 (`-3`), 14:04 (`-3`), 14:59 (`-4`), 16:53 (`-3`) | ⚠️ voir la réserve ci-dessous |

#### Le résultat qui valide l'instrument : `minBloc8 = 7 924` SANS 503

`minBloc8` est descendu **sous le seuil de garde** de 8 192 o sans qu'aucun refus
503 ne soit émis. Ce n'est pas une contradiction : **la garde ne mesure qu'au
moment d'une requête** de fichier statique. Un creux qui se produit entre deux
requêtes n'est vu par personne — sauf par l'instrument, qui échantillonne
indépendamment du trafic.

⇒ **L'instrument voit des creux sur lesquels la garde n'agit jamais.** C'est
exactement ce qu'on lui demandait : le seuil de la garde et le plancher de
l'instrument ne mesurent pas la même chose, et ne doivent pas être comparés comme
si un franchissement du second impliquait un déclenchement du premier.

#### ⚠️ Ce que ce relevé NE reproduit PAS : la corrélation au niveau

L'hypothèse issue de `bug-121` — les déconnexions MQTT suivraient le **niveau**
de fragmentation — **n'est pas reproduite ici** :

- les 5 déconnexions sont survenues aux niveaux **hauts** (`maxBloc8` entre
  **21 492** et **24 564**) ;
- il y en a eu **zéro** pendant la période la **plus basse** (**12 276**).

**Rien n'est tranché dans un sens ni dans l'autre.** L'effet de seuil décrit
autour de 7 924 n'est **ni confirmé ni infirmé** — 12 276 reste peut-être un
niveau « sûr », et le relevé n'est jamais descendu jusqu'aux régimes de la veille.
Mais la corrélation **ne se reproduit pas telle quelle** : le lien
fragmentation → déconnexions reste une **hypothèse de travail**, appuyée par le
journal de 21 h et **non reproduite** par celui de 8 h 49. Consigné dans
`bug-080` § LOT C et dans `bug-121`.

#### Ce que le relevé a changé dans le firmware (v2.49.1)

1. **Cadence du tour de rôle divisée par deux** (`kHeapFloorWalkEverySamples = 2`)
   — cf. la table des résolutions et la section « Coût réel » ci-dessus.
2. **`hf.wmax` / `hf.wmed` publiés par la route** : ils étaient mesurés sans être
   exposés, et la condition 5 du gate n'était donc satisfaite que par accident.
3. **`hf.ctxb` ajouté** : `hf.ctx` seul a produit un contresens de lecture
   documenté ci-dessus.

### Protocole de relevé sur cible

Sans ce protocole, le lot livre un instrument que personne ne sait lire.
L'appareil doit tourner **au moins 30 min** après le déploiement (première trace
agrégée) ; les planchers `boot` ne se rattrapent pas.

1. **Ligne de base, appareil au calme** (aucun navigateur ouvert) :

   ```bash
   curl -s -u admin:MDP 'http://<ip>/get-system-info?hf=1' | python3 -m json.tool
   ```

   Vérifier d'abord **la validité de l'instrument** : `hf.n` > 0 (échantillons
   pris), `hf.i8.n` ≈ `hf.n`, `hf.dma.n` ≈ **`hf.n / 4`** (tour de rôle sur
   **deux** masques, mesuré **un échantillon sur deux** depuis la v2.49.1),
   `hf.pred = 749` (**constante de build — ne rien en déduire**).
   ⚠️ **`hf.wmax` est relevé à 516 µs, donc au-dessus des 300 µs** de
   `kOomProbeWarnDurationUs` : ce n'est **plus** un critère de validité mais un
   **fait connu**, traité en v2.49.1 par la baisse de fréquence. Et **`hf.wmed`
   se lit « ≤ wmed »** — c'est la **borne supérieure** du seau dyadique contenant
   le rang médian, jamais une valeur exacte.
   ⚠️ **Pour « quels régimes cet appareil a-t-il vus », lire `hf.ctxb` et NON
   `hf.ctx`** : `ctx` est remis à zéro toutes les 30 min par le réarmement de
   fenêtre, et le lire comme un cumul depuis le démarrage a déjà produit un
   contresens le 08/09 (`ctx = 1` après 8 h 49, lu à tort comme « un seul régime
   en 8 h 49 »).
2. **Trancher A7, tout de suite** : lire `hf.w`, `hf.w96`, `hf.w95`.
   *(Déjà tranché le 08/09 — `w96 = true`, `w95 = false`, B-1 **opérant**. Le
   point reste dans le protocole : c'est la première chose à revérifier après
   tout changement de politique d'allocation ou de configuration PSRAM.)*
   `w96 = true` / `w95 = false` ⇒ le lot B-1 **est opérant** ;
   `w96 = false` / `w95 = false` ⇒ **décoratif** ;
   `w96 = null` ⇒ aucun verdict (pas de PSRAM, ou `malloc()` refusé) — ne rien
   conclure. `hf.wdis = true` ⇒ les deux API de classement se contredisent :
   **suspendre l'interprétation** et rouvrir la question.
3. **Chargement d'interface** (le déclencheur historique des 503) : ouvrir la
   page principale, forcer un rechargement complet, puis re-interroger.
   Attendre `hf.i8.cf` / `hf.i8.cb` portant `0x02` (`kHeapCtxHttpBurst`) et
   souvent `0x01`. C'est le creux qui **dimensionne la garde 503**.
4. **Session de dosage** : pendant une injection pH ou ORP réelle, re-interroger.
   Le contexte attendu au creux porte `0x04`. Comparer `hf.dma.mb` avec et sans
   dosage — la régulation ne doit **pas** être le facteur dominant.
5. **Rafale de découverte HA** : provoquer une reconnexion MQTT (redémarrer le
   broker, ou couper/rétablir le réseau), puis re-interroger dans les **60 s**.
   Contexte attendu `0x08`. C'est le scénario de `bug-121` (~54 entités
   republiées).
6. **Le chiffre qui compte pour `feature-123`** : `hf.dma.mb`
   (`min(plus gros bloc, 0x080C)` depuis le démarrage) et son contexte
   `hf.dma.cb`. C'est lui qui dimensionne la réserve — **mais lire d'abord
   l'avertissement de résolution ci-dessous**.
7. **En pleine famine** (interface en 503, `?hf=1` inaccessible) : ne pas
   insister sur HTTP. Récupérer les **deux lignes de journal** (`Heap masq boot:`
   / `Heap masq fen:`) via `GET /get-logs` — ou après coup dans `/system.log`
   — c'est le **chemin de dernier recours**, sans bloc contigu requis.

> ⚠️ **Ne pas lire `hf.dma.mb` comme une pointe.** Le plancher `0x080C` n'a
> qu'une résolution de **8 s** depuis la v2.49.1 (tour de rôle mesuré un
> échantillon sur deux ; **4 s** en v2.49.0), là où `0x0804` est mesuré **à
> chaque requête**. Comme `heaps(0x080C) ⊆ heaps(0x0804)`, le plancher fin de
> `0x0804` (`minBoot` de la ligne `Heap 8bits:`) le **borne supérieurement** :
> dimensionner dessus reste **conservateur**. Le chiffre `0x080C` sert à
> confirmer l'**ordre de grandeur** et le **contexte**, pas à capturer la pointe.

> ⚠️ **Un plancher sous le seuil de garde n'implique PAS un 503.** La garde ne
> mesure qu'**au moment d'une requête** de fichier statique ; l'instrument
> échantillonne indépendamment du trafic. Le 08/09, `minBloc8` est descendu à
> **7 924 o** — sous les **8 192 o** de la garde — **sans un seul refus**. Ne pas
> lire un franchissement du plancher comme un déclenchement manqué.

> ⚠️ **Ce que le témoin A7 ne dit pas.** Il tranche où tombe un
> `malloc(seuil + 1)` **au démarrage, PSRAM quasi vide** — pas où tombe le tampon stdio **en pleine
> rafale**, sous pression PSRAM, quand la préférence `extmem` peut retomber
> silencieusement en interne (c'est une **préférence**, pas une contrainte).

## Ring de télémétrie heap en RAM RTC noinit (v2.14.0, bug-heap-toctou A4)

Le tick santé (5-60 s) rate la **cinétique d'effondrement** du heap (quelques secondes entre « sain » et `bad_alloc`). Depuis la v2.14.0, un ring d'instrumentation en **RAM RTC noinit** (survit à `ESP.restart()`/panic/WDT, **pas** au power-on) capture cette cinétique et la restitue dans le journal au boot suivant. Module pur [`src/heap_ring_logic.h`](../../src/heap_ring_logic.h) (encodage + arithmétique d'index, testé en natif) + coquille impure [`src/heap_ring.cpp`](../../src/heap_ring.cpp).

- **Budget** : magic (4 o) + tête/remplissage (2+2 o) + ring `kHeapRingEntries = 128` × 8 o = **1032 o** RTC noinit (vérifié à l'ELF). Magic `0xB10C4EAA` (**bumpé en v2.19.0**, était `0xB10C4EA9`), **distinct** de `kAutoHealRtcMagic` (`0xA5C0FFEE`, compteur anti-boucle) et de `kOomPostMortemMagic` (`0xF00DFA13`, **re-bumpé en bug-080 B-2**, était `0xF00DFA12` en v2.19.0 — post-mortem d'allocation) — les trois états RTC sont indépendants (voir [budget consolidé](#budget-ram-rtc-noinit-consolidé-3-zones)).
- **Échantillonnage** : `heapRingSample()` depuis `loop()` toutes les `kHeapRingSampleIntervalMs = 2 s`. Format d'une entrée (8 o, little-endian, quantifié) : uptime s (u16, modulo ~18,2 h), maxBloc ÷ 64 (u16), heap libre ÷ 256 (u16), clients WS (u8, saturé 255), **delta** de pushes WS sautés depuis l'échantillon précédent (u8, saturé 255).
- **Dump au boot** — `heapRingDumpIfCrash()`, appelé dans `setup()` après `history.begin()` (persistance des logs active) et **avant** le WiFi : si le magic est valide **et** que `esp_reset_reason() != POWERON`, les `kHeapRingDumpMaxEntries = 32` dernières entrées (**~64 s de cinétique**) sont émises dans le journal — un WARN d'en-tête `Cinétique heap avant reset (<motif>, reset_reason=<valeur brute>) : N échantillon(s), pas ~2 s (t-0s = dernier avant reset, uptime=<u> s mod 65536)` puis des INFO compactes, 4 entrées par ligne (**8 lignes** pour 32 entrées), de la plus ancienne à la plus récente. Le WARN est **enrichi depuis la v2.18.5** (spec `bug-oom-accept-http-durcissement`) de deux champs : la valeur **brute** d'`esp_reset_reason()` (en plus du libellé court) et l'**uptime absolu** du dernier échantillon avant reset (modulo 65 536 s ≈ 18,2 h — quantum d'encodage du ring) — pour la corrélation temporelle a posteriori avec le journal persistant et les coredumps :

  ```
  t-62s maxB8=18624 free8=94208 ws=2 skip=0 | t-60s maxB8=12800 free8=93952 ws=2 skip=1 | …
  ```

  > **Libellés `maxB8=` / `free8=` depuis la v2.19.0** : l'anneau enregistre la mesure **8 bits** (mémoire réellement allouable, [ADR-0038](../adr/0038-metrique-memoire-allouable-8-bits.md)). Un dump portant `maxB=` / `free=` provient d'un firmware **≤ 2.18.9** et contient la métrique surestimée. Le **magic a été bumpé en conséquence** : la cinétique du boot de bascule 2.18.x → 2.19.0 est **perdue**, ce qui est préférable à l'affichage d'un chiffre faux et crédible. Ce changement est **purement observationnel** — aucune décision de redémarrage ne dépend de l'anneau.

- **Invalidation du magic** : le magic est invalidé **dans tous les cas** au boot (dump émis ou non) — le ring appartient au boot précédent ; il est **ré-armé** au 1ᵉʳ `heapRingSample()` du boot courant. Bornes insensées (RAM RTC corrompue) → dump abandonné / ring réinitialisé.

> ⚠️ **Deux limites assumées** :
> 1. `ESP_RST_SW` inclut les reboots **volontaires** (OTA, reboot manuel, factory reset) → le dump est émis aussi dans ces cas — bruit accepté (8 lignes par reboot volontaire), le motif en en-tête (`SW_RESET` vs `PANIC`/`WATCHDOG`) permet de trier.
> 2. Les 8 lignes partent en **RAM + Serial** au moment du dump et ne sont **flushées sur disque que plus tard** (flush périodique / ERROR) — un **re-crash immédiat** après le boot peut les perdre. La cinétique du re-crash lui-même sera de toute façon recapturée par le ring.

### Deux dumps consécutifs au boot depuis la v2.18.8

`setup()` appelle **`oomPostMortemDumpIfBoot()` juste après `heapRingDumpIfCrash()`** : les deux traces se lisent ensemble — l'anneau donne la **cinétique** (~64 s avant le reset, pas de 2 s), le post-mortem donne le **point d'échec exact** (taille demandée, `caps`, uptime, état du tas comptabilisé, drapeaux, verdict). Format, champs et limites : [oom-guard.md](oom-guard.md#post-mortem-dallocation-en-ram-rtc-noinit-v2188).

Depuis la v2.18.8, `heapRingSample()` prend la mesure **une seule fois** par échantillon et alimente **à la fois** l'anneau et le miroir atomique consommé par le chemin d'échec d'allocation (`oomGuardNoteHeapSample()`) — depuis la **v2.19.0**, cette mesure est celle du helper [`heap_metrics.h`](../../src/heap_metrics.h) (`heapLargestFreeBlock8Bit()` / `heapFree8Bit()`) et non plus `ESP.getMaxAllocHeap()` / `ESP.getFreeHeap()`. La **cadence de 2 s est inchangée** : accélérer l'échantillonnage injecterait de la gigue dans le phénomène mesuré (ces appels s'exécutent interruptions coupées), pour zéro pouvoir discriminant ([ADR-0037](../adr/0037-postmortem-oom-et-integrite-tas.md)).

## Budget RAM RTC noinit consolidé (3 zones)

La RAM RTC « slow » noinit survit à `ESP.restart()`, aux panics et au watchdog — **pas** au power-on. Trois zones **indépendantes**, chacune avec son propre magic :

| Zone | Magic | Taille | Rôle | Fichier |
|---|---|---|---|---|
| Compteur anti-boucle auto-guérison | `0xA5C0FFEE` (`kAutoHealRtcMagic`) | **9 o** | `rtcAutoHealRebootCount` + ancre de fenêtre glissante | [`main.cpp`](../../src/main.cpp) |
| Anneau de télémétrie heap | `0xB10C4EAA` (v2.19.0 ; était `0xB10C4EA9`) | **1032 o** | 128 × 8 o + tête/remplissage + magic (cinétique pré-crash) | [`heap_ring.cpp`](../../src/heap_ring.cpp) |
| Post-mortem d'allocation | `0xF00DFA13` (`kOomPostMortemMagic`, **bug-080 B-2** ; était `0xF00DFA12` en v2.19.0, `0xF00DFA11` avant) | **48 o** (**32 o** avant B-2) | État au moment exact d'un échec d'allocation + drapeaux + verdict (v2.18.8) + **sonde du refus** : 4 mots neufs, double mesure caps de l'échec / référence 8 bits ([ADR-0066](../adr/0066-sonde-du-tas-au-moment-du-refus.md)) | [`oom_guard.cpp`](../../src/oom_guard.cpp) |

**Total : 1089 o sur 8192 disponibles** (≈ 13 %) — **1073 o avant bug-080 B-2**, qui a porté la seule zone post-mortem de 32 à 48 o. Aucune 4ᵉ zone n'a été créée. Les magics sont volontairement distincts : une zone corrompue n'invalide jamais les deux autres.

> **Règle posée par [ADR-0038](../adr/0038-metrique-memoire-allouable-8-bits.md)** : **tout changement de la métrique stockée dans une zone RTC noinit impose un bump de son magic.** Relire une trame écrite par un firmware antérieur avec une nouvelle grille de lecture produirait un chiffre faux **et crédible** — pire qu'une trace absente. Les valeurs `0xA5C0FFEE`, `0xB10C4EA9`, `0xB10C4EAA`, `0xF00DFA11`, `0xF00DFA12` et `0xF00DFA13` sont désormais **toutes réservées**, y compris les anciennes.
>
> **La règle vaut a fortiori pour un changement de TAILLE** (bug-080 B-2) : sans bump, les 4 mots neufs auraient été lus dans une zone de 32 o écrite par l'ancien firmware — c'est-à-dire dans de la RAM RTC **non initialisée**, produisant des valeurs de sonde **inventées et crédibles**.

## Contrôle d'intégrité périodique du tas (v2.18.8)

Détecteur **direct** de la corruption du tas — l'hypothèse laissée ouverte par le crash du 13/08 (la comptabilité annonçait 81 Ko libres et 27,5 Ko de plus gros bloc pendant que l'allocateur refusait 156 octets). Mécanisme : `oomGuardIntegrityTick()` ([`oom_guard.cpp`](../../src/oom_guard.cpp)) ; **appel et gardes** : `loop()` ([`main.cpp`](../../src/main.cpp)).

### Cadence, durée et autolimitation

- `heap_caps_check_integrity_all()` toutes les `kHeapIntegrityCheckIntervalMs` = **5 min** (premier passage dès que l'appelant est éligible).
- **Durée attendue : 1-3 ms.** Le framework est compilé avec `CONFIG_HEAP_POISONING_LIGHT` : le contrôle **parcourt les blocs** (en-têtes, chaînage, canaris) et ne scanne **pas** la mémoire octet par octet.
- La durée est **mesurée à chaque passage** (`esp_timer_get_time`). La **1ʳᵉ durée est journalisée en INFO** : `Contrôle d'intégrité du tas : 1er passage en <n> µs (cadence 300 s, plafond 20000 µs)`.
- **WARN** au-delà de `kHeapIntegrityWarnDurationUs` = 5 ms (contrôle conservé).
- **Désactivation DÉFINITIVE** (+ WARN one-shot) au-delà de `kHeapIntegrityMaxDurationUs` = 20 ms : le contrôle ne doit jamais devenir lui-même une source de gigue.
- Le parcours s'exécute **interruptions coupées** et prend les verrous de l'allocateur : **aucun `esp_task_wdt_reset()` à l'intérieur** — il est appelé **juste après**.

### Les 6 gardes de sécurité chimique (exigence `pool-chemistry`)

Le contrôle est appelé **après `PumpController.update()`** en fin de `loop()` — jamais entre `filtration.update()` et `PumpController.update()`, qui doivent voir le même tour. Il n'est **JAMAIS** exécuté si l'une de ces conditions n'est pas remplie (gardes **cumulatives, fail-closed**) :

| # | Garde | Source |
|---|---|---|
| 1 | Pas en **mode maintenance** | `maintenanceMode` |
| 2 | Pas d'**OTA en cours** | `PumpController.isOtaInProgress()` |
| 3 | Aucune **injection manuelle pH** | `manualInjectPh.active` |
| 4 | Aucune **injection manuelle ORP** | `manualInjectOrp.active` |
| 5 | **Aucune pompe énergisée** | `PumpController.isAnyPumpEnergized()` (v2.18.8) |
| 6 | **Uptime** ≥ `kAutoHealMinUptimeMs` (3 min) | `millis()` |

> ⚠️ **v2.36.0 (feature-094) — `isPhDosing()` et `isOrpDosing()` ont été RETIRÉS de cette conjonction** (ils en étaient les gardes 3 et 4). Motif : depuis la [porte de session pH](pump-controller.md#porte-de-session-ph-robuste-au-bruit-feature-094-v2360), ces prédicats suivent la **porte** et non le débit — ils restent donc vrais **pompe à l'arrêt**, jusqu'à `kPhSessionMaxWallMs` = **3 h**. Les conserver aurait rendu le détecteur de corruption du tas **aveugle des heures d'affilée**, dans un projet qui a déjà connu une boot-loop `lfs_alloc`. Ce qu'il faut protéger, c'est la **cadence des impulsions pendant qu'une pompe tourne** : `isAnyPumpEnergized()` (garde 5) le couvre **exactement**, et couvre en plus le test manuel armé depuis la tâche web, invisible des deux autres. **Confirmé par `pool-chemistry`**, dont le visa d'origine portait sur « une pompe tourne », jamais sur « une porte est ouverte ». Amendement porté à [ADR-0037](../adr/0037-postmortem-oom-et-integrite-tas.md).

La garde 5 est un **accesseur en lecture seule** ajouté à [`pump_controller.h`](../../src/pump_controller.h), fondé sur les **rapports cycliques PWM réels** (`pumpDuty[]`) : les indicateurs de dosage ne couvraient **que la régulation automatique** — une **injection manuelle** ou un **test de pompe** armé depuis la tâche web passait au travers. Aucun setter associé : cet accesseur n'influence **aucune** décision de dosage. Course bénigne assumée (lecture d'un octet, atomique sur Xtensa) : le pire cas est un contrôle **différé d'un tour de loop** — fail-closed.

**Retard maximal admissible** établi par `pool-chemistry` : **65 ms** (tolérance de 0,1 mL à 90 mL/min). Le plafond retenu est **20 ms** = **0,03 mL** — et, pompes à l'arrêt, ce retard n'affecte de toute façon aucun volume injecté.

**Verrou de compilation neuf (v2.36.0)** : le parcours doit rester **sous 1 % du pouls minimal** — `static_assert(kHeapIntegrityMaxDurationUs × 100 ≤ kPulseMinMs × 1000)`, soit **20 ms contre 10 s = 0,2 %** (5× de marge sous la règle, 500× en ratio brut). L'argument chimique : le **rendement de pouls** ([ADR-0051](../adr/0051-pouls-minimal-double-rendement-avant-comptage.md)) se mesure sur `usedMs`, compté **à partir de l'énergisation effective** — un pouls décalé dure la même chose et crédite le même volume. **Un retard décale, il ne sur-injecte pas.** Si le parcours s'approchait un jour de cette borne, le retard redeviendrait un sujet chimique.

### Action en cas de corruption détectée

`oomGuardIntegrityTick()` retourne `true` **une seule fois** (tout contrôle ultérieur est définitivement désactivé) ; `handleHeapCorruptionDetected()` ([`main.cpp`](../../src/main.cpp)) exécute alors une séquence **imposée, à ne pas réordonner** :

1. `PumpController.stopAll()` + `PumpController.setMaintenanceInhibit(true)` — **sécurité chimique avant tout**, y compris dans la branche « maintenance » ;
2. si le plafond de reboots de la fenêtre n'est pas atteint → **`performFailSafeRestart()`** avec l'alerte MQTT **`heap_corruption`** : la séquence propre existante (coupure des pompes, persistance des cumuls journaliers, CRITICAL, alerte, flush, restart) n'est **pas doublée** ici ;
3. sinon (plafond `kAutoHealMaxReboots` atteint) → CRITICAL + `flushToDisk()` + alerte MQTT **`heap_corruption`** (message « mode maintenance ») + `enterMaintenanceMode()` : l'appareil reste **joignable**, dosage suspendu.

**Jamais d'`ESP.restart()` nu** : un tas corrompu impose de couper le dosage et de persister les cumuls avant toute autre action.

Le résultat du contrôle est aussi consigné dans les **drapeaux du post-mortem RTC** (bit 3 « intégrité échouée », bit 4 « jamais exécutée ») — écrits en RTC **uniquement en cas d'échec**, pour que l'information survive au redémarrage propre qui suit.

## Machine à états auto-guérison / maintenance

```
                 maxBloc8 < kRestartMaxAllocHeapBytes (4 096 o, 8 bits)
                 pendant kAutoHealDebounceChecks (3) ticks consécutifs
   [ NORMAL ] ───ET uptime ≥ kAutoHealMinUptimeMs (3 min)──────────► [ RESTART propre ]
       ▲                                                                     │
       │                                                                     │ reboots comptés
       │ maxBloc8 ≥ seuil (compteur remis à 0)                              │ en RAM RTC
       │                                                                     ▼
       │                          rebootCountInWindow ≥ kAutoHealMaxReboots (3)
       └──────────────────────────────────────────────────► [ MAINTENANCE (joignable) ]
                                   (fenêtre glissante kAutoHealRebootWindowMs = 15 min)
```

### Redémarrage propre — séquence partagée `performFailSafeRestart()` ([`main.cpp:632`](../../src/main.cpp:632))

Depuis v2.9.2, la séquence de reboot est **factorisée** dans `performFailSafeRestart(reason, alertType, alertMsg)`, appelée par **les deux** watchdogs :

- `performAutoHealRestart(maxAllocHeap)` ([`main.cpp:656`](../../src/main.cpp:656)) → délègue avec `alertType = "auto_reboot"` (gel heap).
- watchdog de connectivité → appelle directement avec `alertType = "network_stall_reboot"` (gel réseau).

Séquence **fail-safe, ordre imposé par pool-chemistry (condition C1) — NE PAS réordonner** :

1. `PumpController.stopAll()` — coupe les 2 pompes (sécurité chimique avant tout).
2. `saveDailyCounters()` — persiste les cumuls journaliers de dosage (anti-sur-dosage au reboot).
3. Incrément du compteur RTC + ancrage de la fenêtre glissante au 1er reboot.
4. `systemLogger.critical(reason)` — log persisté.
5. `mqttManager.publishAlert(alertType, alertMsg)` — alerte MQTT (tolère l'échec, ne bloque pas).
6. `systemLogger.flushToDisk()` — flush (tolère un échec silencieux si heap trop bas).
7. `ESP.restart()` — **ne retourne jamais**.

Le compteur RTC incrémenté à l'étape 3 est **partagé** entre heap et réseau : tout reboot fail-safe, quelle qu'en soit la cause, rapproche du plafond `kAutoHealMaxReboots`.

> **Reboot = pompes OFF = état fail-safe.** Le fail-safe **total** au boot suppose un **pulldown sur la gate des MOSFET** (GPIO25 pH / GPIO33 ORP) — condition matérielle hors périmètre logiciel ([ADR-0029](../adr/0029-auto-guerison-heap.md)).

### Mode maintenance — `enterMaintenanceMode()` ([`main.cpp:626`](../../src/main.cpp:626))

Condition pool-chemistry **C2** : quand `kAutoHealMaxReboots` (3) reboots ont eu lieu dans la fenêtre — **toutes causes confondues** (heap et/ou réseau, cf. budget partagé) — on **n'enchaîne plus** de reboot. Idempotent (flag `maintenanceMode`) :

- `PumpController.stopAll()` + `PumpController.setMaintenanceInhibit(true)` → dosage inhibé sur **toutes** les voies (voir [pump-controller.md](pump-controller.md)).
- `systemLogger.critical(...)` + `mqttManager.publishAlert("maintenance_mode", ...)`.
- **Aucun restart** : l'appareil reste **joignable** (web + MQTT) pour diagnostic.
- **Sortie** : redémarrage **manuel** du contrôleur.

### Compteur anti-boucle en RAM RTC — `initAutoHealRtcState()` ([`main.cpp:570`](../../src/main.cpp:570))

- `rtcAutoHealRebootCount` / `rtcAutoHealWindowStartEpoch` en **RAM RTC** (survivent au reboot logiciel), protégés par un magic word `kAutoHealRtcMagic`.
- **Power-on / brownout** (magic absent) → compteur remis à zéro : un vrai power-cycle ne verrouille jamais en maintenance.
- **Reboot logiciel** → compteur conservé, mais **purgé** si la fenêtre glissante (`kAutoHealRebootWindowMs`) a expiré → autorise à nouveau l'auto-guérison après une période saine.

## Décision pure — `self_heal_logic`

Style Humble Object ([ADR-0017](../adr/0017-logique-metier-pure-humble-object-testabilite.md)) : la **décision** est extraite dans un module pur (`<stdint.h>` seul, sans Arduino/FreeRTOS/String) → **testable en natif**. La collecte (`heapLargestFreeBlock8Bit`, `millis`, RAM RTC) et les effets restent dans la coquille `main.cpp`.

```cpp
enum class SelfHealDecision : uint8_t { Continue, Restart, EnterMaintenance };

struct SelfHealInputs {
  uint32_t maxAllocHeap;        // plus gros bloc allouable 8 bits (heapLargestFreeBlock8Bit, bug-080)
  uint32_t restartThreshold;    // kRestartMaxAllocHeapBytes
  uint8_t  consecutiveBelow;    // ticks consécutifs sous le seuil
  uint8_t  debounceChecks;      // kAutoHealDebounceChecks
  uint32_t uptimeMs;            // millis()
  uint32_t minUptimeMs;         // kAutoHealMinUptimeMs
  uint8_t  rebootCountInWindow; // reboots auto déjà faits dans la fenêtre
  uint8_t  maxReboots;          // kAutoHealMaxReboots
};

SelfHealDecision evaluateSelfHeal(const SelfHealInputs& in);
```

**Ordre des règles (priorité décroissante)** :

1. `rebootCountInWindow >= maxReboots` → `EnterMaintenance` (**priorité HAUTE** : jamais un reboot de plus une fois le plafond atteint).
2. `uptimeMs >= minUptimeMs` **ET** `maxAllocHeap < restartThreshold` **ET** `consecutiveBelow >= debounceChecks` → `Restart`.
3. sinon → `Continue`.

Le compteur `consecutiveBelow` (statique dans `checkHeapHealth` depuis v2.11.1) est incrémenté tant que `maxBloc < seuil`, remis à 0 sinon. Aucune décision n'est évaluée en mode maintenance ni pendant une OTA (`if (!maintenanceMode && !isOtaInProgress())`, [`main.cpp:849`](../../src/main.cpp)).

Le module expose aussi la **cadence d'échantillonnage adaptative** (v2.11.1) :

```cpp
// 60 s en régime sain, 5 s sous le seuil d'alerte (frontière : maxAllocHeap < warnThreshold)
uint32_t heapSampleIntervalMs(uint32_t maxAllocHeap, uint32_t warnThreshold,
                              uint32_t normalIntervalMs, uint32_t fastIntervalMs);
```

**Tests** : 22 tests natifs dédiés (seuils, debounce, uptime minimal, plafond de reboots, priorité maintenance, +5 sur `heapSampleIntervalMs` : frontière du seuil, extrêmes 0/`UINT32_MAX`). Voir [BUILD.md](../BUILD.md) pour `pio test -e native`.

## Watchdog de connectivité (v2.9.2)

Second watchdog applicatif, **indépendant du watchdog heap**, ciblant le gel de la **pile lwIP** (web/mDNS/ICMP morts, WiFi associé, heap sain). Décision architecturale : [ADR-0030](../adr/0030-watchdog-connectivite.md).

### Signe de vie — sonde ICMP passerelle

Le signe de vie **primaire** est une **sonde ICMP active** vers la passerelle par défaut (`esp_ping`, header `ping/ping_sock.h`), **indépendante de MQTT** (contrainte : MQTT peut être désactivé) :

- **Session persistante unique** (`netPingHandle`), (re)créée de façon **idempotente** par `ensureNetworkProbe()` ([`main.cpp:673`](../../src/main.cpp:673)) tant que le WiFi est associé. Cadence `kNetworkProbeIntervalMs = 30 s`, timeout `kNetworkProbeTimeoutMs = 2 s`.
- Chaque echo reply prouve que **toute** la chaîne (RX → lwIP → TX) fonctionne. Le callback `onNetPingSuccess()` ([`main.cpp:666`](../../src/main.cpp:666)), invoqué par le thread interne `esp_ping` (**hors** `loopTask`), rafraîchit l'horloge `lastNetworkOkMs` (`volatile`, accès 32 bits aligné).
- L'horloge de stall mesurée dans `checkSystemHealth()` est `msSinceNetOk = millis() - lastNetworkOkMs`.

### Anti-faux-positif — gating WiFi

Tant que `WiFi.isConnected()` est **faux**, l'horloge de stall est **gelée** (`lastNetworkOkMs = millis()`) et le palier soft **réarmé** ([`main.cpp:812`](../../src/main.cpp:812)). Conséquence : une coupure WiFi légitime, un broker MQTT down ou un reboot de box **ne déclenchent rien**. La perte de WiFi reste gérée par la logique de reconnexion / AP de secours existante, **pas** par ce watchdog.

### Machine à états `net_health` — réponse graduée

```
        WiFi associé + pile muette (aucun echo ICMP)
                          │
   msSinceNetOk ≥ kNetworkStallSoftMs (2 min) ET soft pas encore tenté
                          ▼
              [ SoftRecover ] ── WiFi.disconnect(false) + WiFi.reconnect()
                          │       alerte MQTT network_soft_recover  (1×/épisode)
   (le soft n'a pas débloqué)
                          │
   msSinceNetOk ≥ kNetworkStallRebootMs (5 min)
                          ▼
              [ Restart ] ── performFailSafeRestart(..., "network_stall_reboot", ...)

   rebootCountInWindow ≥ kAutoHealMaxReboots (3, PARTAGÉ heap+réseau)
                          ▼
              [ EnterMaintenance ]  (priorité HAUTE)
```

Le flag `softRecoverAttempted` (statique dans `checkSystemHealth`) borne le SoftRecover à **une tentative par épisode** ; il est réarmé dès que le réseau redevient sain (`msSinceNetOk < kNetworkStallSoftMs`) ou que le WiFi tombe. Aucune décision réseau n'est évaluée en mode maintenance (`if (!maintenanceMode)`).

### Décision pure — `net_health_logic`

Même style Humble Object ([ADR-0017](../adr/0017-logique-metier-pure-humble-object-testabilite.md)) que `self_heal_logic` : la **décision** est extraite dans un module pur (`<stdint.h>` seul), la collecte (`esp_ping`, `millis`, RAM RTC) et les effets (`WiFi.reconnect`, `performFailSafeRestart`, `enterMaintenanceMode`) restent dans `main.cpp`.

```cpp
enum class NetHealthDecision : uint8_t { Continue, SoftRecover, Restart, EnterMaintenance };

struct NetHealthInputs {
  bool     wifiAssociated;       // WiFi.isConnected()
  uint32_t msSinceLastNetOk;     // ms depuis le dernier signe de vie réseau
  uint32_t softStallMs;          // kNetworkStallSoftMs
  uint32_t hardStallMs;          // kNetworkStallRebootMs
  bool     softRecoverAttempted; // soft déjà tenté sur cet épisode
  uint32_t uptimeMs;             // millis()
  uint32_t minUptimeMs;          // kAutoHealMinUptimeMs
  uint8_t  rebootCountInWindow;  // reboots auto dans la fenêtre (PARTAGÉ heap+réseau)
  uint8_t  maxReboots;           // kAutoHealMaxReboots
};

NetHealthDecision evaluateNetHealth(const NetHealthInputs& in);
```

**Ordre des règles (priorité décroissante)** :

1. `!wifiAssociated` → `Continue` (horloge de stall gelée par la coquille : on ne juge pas la pile lwIP sans lien WiFi).
2. `uptimeMs < minUptimeMs` → `Continue` (laisser le démarrage se terminer).
3. `rebootCountInWindow >= maxReboots` → `EnterMaintenance` (**anti-boucle prioritaire**).
4. `msSinceLastNetOk >= hardStallMs` → `Restart` (gel confirmé).
5. `msSinceLastNetOk >= softStallMs` ET `!softRecoverAttempted` → `SoftRecover`.
6. sinon → `Continue`.

**Tests** : 25 tests natifs dédiés (gating WiFi, paliers soft/hard, uptime minimal, plafond partagé, réarmement du soft).

## Budget anti-boucle PARTAGÉ (heap + réseau)

Les deux watchdogs partagent **le même** compteur RTC `rtcAutoHealRebootCount` et la même fenêtre `kAutoHealRebootWindowMs`. Un reboot heap **et** un reboot réseau incrémentent le **même** quota (`kAutoHealMaxReboots = 3`). Conséquence assumée ([ADR-0030](../adr/0030-watchdog-connectivite.md)) : **un épisode combinant fragmentation mémoire ET gel réseau bascule en maintenance plus tôt** qu'un mode de défaillance isolé — comportement conservateur volontaire (dégrader proprement tôt plutôt que boucler). À connaître au diagnostic.

## Disjoncteur de boot-loop de l'init history (v2.10.0)

Troisième volet d'auto-guérison, **hors** `checkSystemHealth()` : il s'exécute une seule fois par boot, au début de `HistoryManager::begin()` ([`src/history.cpp`](../../src/history.cpp)), **avant** le montage de la partition `history`. Décision architecturale : [ADR-0031](../adr/0031-disjoncteur-bootloop-history.md).

### Déclencheur — incident réel (2026-07-22, v2.9.2)

Boot-loop `IntegerDivideByZero` dans `lfs_alloc` (littlefs) à la **première écriture** dans `/system.log` : superblock/métadonnées de la partition `history` corrompus. LittleFS **monte sans erreur** un filesystem corrompu puis panique à l'écriture — une **exception CPU non rattrapable** : aucun `try`/test d'écriture ne peut l'attraper. Le crash survenant dans `setup()` **avant la montée du WiFi**, l'appareil est resté injoignable **une semaine**, jusqu'à récupération manuelle en USB. Le garde-fou `.fscheck` existant (write/read 2 octets post-montage) ne voyait rien : un fichier de 2 octets est stocké *inline* dans les métadonnées littlefs et n'exerce **jamais** `lfs_alloc`. La seule défense viable est de détecter la boucle de panics **entre** les boots.

### Machine drapeau / compteur NVS

Le drapeau est un compteur `crash_cnt` dans le namespace NVS `hist_meta` (déjà utilisé pour `part_sz`) :

1. **Pose** : `crash_cnt` est écrit (≥ 1) **juste avant** `historyFs.begin()`. La fenêtre de détection couvre montage + `.fscheck` + `setPersistenceFs()` + `loadFromFile()`.
2. **Levée** : `crash_cnt = 0` sur **toutes** les sorties de `HistoryManager::begin()` (init complète, échec de montage, échec de remontage). Un panic **ailleurs** dans le firmware ne peut donc jamais déclencher l'effacement : le drapeau n'existe que pendant la fenêtre d'init.
3. **Au boot suivant**, avant le montage, la décision pure `evaluateHistoryGuard()` est évaluée à partir de `crash_cnt` (NVS) et de `esp_reset_reason()`.

```
   [ boot ]
      │  crash_cnt == 0 (init précédente propre / 1er boot)
      ├────────────────────────────────► Proceed (réarmement : crash_cnt→1, montage)
      │  crash_cnt > 0 mais reset PROPRE (POWERON/SW/EXT/BROWNOUT/DEEPSLEEP)
      ├────────────────────────────────► Proceed (réarmement SANS compter)
      │  crash_cnt ≥ kHistoryInitCrashThreshold (2) et reset CRASH
      ├────────────────────────────────► ErasePartition (esp_partition_erase_range
      │                                   vérifié ESP_OK + log CRIT, puis montage
      │                                   normal qui reformate — le boot CONTINUE)
      │  sinon (crash sous le seuil)
      └────────────────────────────────► CountCrash (crash_cnt+1, WARN
                                          « x/2 — nouvelle tentative », montage)
```

### Préservation de preuve avant effacement (v2.11.0)

Depuis la v2.11.0 (spec `bug-nospc-partition-history`, [ADR-0033](../adr/0033-budget-espace-partition-history.md)), la branche `ErasePartition` **préserve la preuve AVANT** `esp_partition_erase_range` via `dumpHistoryPartitionEvidence()` ([`src/history.cpp`](../../src/history.cpp)) :

- **Dump hex Serial** des blocs 0-1 de la partition (8 Ko = superblocs + métadonnées littlefs), lignes préfixées `[HISTDUMP]` (`BEGIN size=` … `0xNNNN: <32 octets hex>` … `END crc0= crc1=`). Durée ≈ 2,4 s à 115200 bauds → `esp_task_wdt_reset()` toutes les 64 lignes. **Dérogation assumée** au principe « jamais de `Serial.printf` pour des logs applicatifs » : la persistance des logs n'est pas encore active à ce stade du boot, et il est interdit d'écrire sur le FS en cours d'autopsie (un `ERROR` déclencherait un flush disque). Lecture seule sur la partition.
- **Empreinte courte en NVS** `hist_meta`, récupérable après reboot même sans capture Serial : CRC32 de chaque bloc (`dmp_crc0`, `dmp_crc1`) + 16 premiers octets du bloc 0 (`dmp_head`). Échec NVS silencieux (la preuve principale est déjà partie sur Serial).

**Rôle : discriminant H1/H2/H3** de l'investigation `investigation-corruption-recurrente-history` pour une éventuelle prochaine occurrence du div/0 : chaîne de métadonnées pleine/incohérente = H1 (saturation — cause racine traitée par l'ADR-0033), superblock à CRC valide mais contenu insensé = H2 (corruption RAM `lfs_t` persistée, hypothèse résiduelle), paire de métadonnées déchirée = H3 (écriture interrompue).

### Mapping des causes de reset

`esp_reset_reason()` est mappé vers l'enum pur `HistoryResetKind` par la coquille (`mapResetReason()`, [`src/history.cpp`](../../src/history.cpp)) :

| Comptés comme **crash** | Exclus (reset **propre**, réarmement sans compter) |
|---|---|
| `PANIC` (le cas `IntegerDivideByZero`), `INT_WDT`, `TASK_WDT`, `WDT` | `POWERON`, `SW` (ESP.restart, OTA), `EXT`, **`BROWNOUT`**, `DEEPSLEEP` |

Le **brownout est exclu volontairement** : c'est une cause électrique (alimentation faible), pas un symptôme de corruption FS — le compter risquerait d'effacer l'historique sur une simple alim défaillante.

### Indépendance vis-à-vis du budget anti-boucle et du mode maintenance

Le disjoncteur est **indépendant** du budget `kAutoHealMaxReboots` et du mode maintenance — c'est voulu, pour deux raisons :

- **Il ne redémarre jamais** : il répare (efface) puis **laisse le boot continuer**. Il ne consomme donc aucun quota de reboot ; c'est le crash lui-même qui reboote, pas le disjoncteur.
- **Il agit avant tout le reste** : pendant `setup()`, avant WiFi/MQTT/maintenance. Le bloquer en mode maintenance n'aurait aucun sens — le mode maintenance existe pour garder un appareil **joignable** ; ici l'appareil est **mort avant d'être joignable**. Conditionner la réparation au mode maintenance laisserait l'appareil définitivement injoignable, exactement ce qu'on corrige.

### Alerte MQTT différée

L'effacement a lieu dans `history.begin()`, **avant** `mqttManager.begin()` (la file de publication n'existe pas encore). Un flag `_partitionAutoErased` + getter `HistoryManager::partitionWasAutoErased()` permettent à `setup()` ([`src/main.cpp`](../../src/main.cpp)) de publier l'alerte `history_partition_reset` de manière **différée**, après `mqttManager.begin()`, via le `publishAlert()` existant. De même, la persistance des logs n'étant pas encore active à cet instant, le log CRIT de l'effacement part en **RAM + Serial uniquement** (voir [logger.md](logger.md#émission-console)) — c'est suffisant et c'est justement ce qu'on protège.

### Décision pure — `history_guard_logic`

Même style Humble Object ([ADR-0017](../adr/0017-logique-metier-pure-humble-object-testabilite.md)) que `self_heal_logic` / `net_health_logic` : la décision est extraite dans un module pur (`<stdint.h>` seul) ; la collecte (`esp_reset_reason`, lecture NVS) et les effets (`esp_partition_erase_range`, écriture NVS, `systemLogger.critical`) restent dans la coquille `history.cpp`.

```cpp
enum class HistoryGuardDecision : uint8_t { Proceed, CountCrash, ErasePartition };

struct HistoryGuardInputs {
  uint8_t crashCount;      // crash_cnt lu en NVS ; 0 = pas de drapeau
  bool    resetWasCrash;   // isCrashResetKind(reset précédent)
  uint8_t eraseThreshold;  // kHistoryInitCrashThreshold
};

bool isCrashResetKind(HistoryResetKind kind);
HistoryGuardDecision evaluateHistoryGuard(const HistoryGuardInputs& in);
```

**Ordre des règles (priorité décroissante)** : 1. `crashCount == 0` → `Proceed` ; 2. `!resetWasCrash` → `Proceed` (réarmement sans compter) ; 3. `crashCount >= eraseThreshold` → `ErasePartition` ; 4. sinon → `CountCrash`.

**Tests** : 24 tests natifs dédiés (`test/test_native_history_guard/` — mapping des reset reasons, seuil, réarmement, cas limites) — **289 tests au total**.

### Trade-off assumé et récupération manuelle

- **Disponibilité > données** : 2 crashs consécutifs pendant la fenêtre d'init — même de cause **non liée** au FS — effacent l'historique. Coût accepté : l'historique repart de zéro, mais l'appareil redevient joignable sans intervention.
- **NVS indisponible** : dégradation silencieuse vers le comportement historique (pas de disjoncteur, jamais de blocage du boot).
- **Récupération manuelle de secours** (reste valable si le disjoncteur lui-même est hors jeu) :

```bash
esptool.py erase_region 0x7B0000 0x40000   # efface la partition history (256 Ko, layout v5)
```

> ⛔ **L'ancienne commande `erase_region 0x3E0000 0x10000` (layout v4) est PÉRIMÉE et DANGEREUSE.** Depuis le passage au layout v5 ([ADR-0068](../adr/0068-partitions-layout-v5-flash-8-mo.md), 2026-09-05), `0x3E0000` tombe **au milieu de `app1`** : la commande n'effacerait pas l'historique mais **détruirait le slot OTA de secours**. Elle figure encore dans [ADR-0031](../adr/0031-disjoncteur-bootloop-history.md), qui est immuable — c'est cette page qui fait foi. Source de vérité des offsets : [`partitions.csv`](../../partitions.csv).

## Debounce de la ré-annonce mDNS (v2.9.1)

`checkSystemHealth()` consomme aussi le drapeau `mdnsReannounceRequested` (posé par le handler d'événement WiFi `STA_GOT_IP`) pour ré-annoncer `poolcontroller.local` en contexte `loopTask` (mDNS/lwIP non thread-safe). Depuis v2.9.1, la ré-annonce est **debouncée** ([`main.cpp:708`](../../src/main.cpp:708)) :

- exécutée **uniquement** si l'IP a **réellement** changé (`currentIp != mdnsLastAnnouncedIp`),
- **et** au plus une fois toutes les `kMdnsReannounceMinIntervalMs = 30 s`.

Sinon, un `debug(...)` « Ré-annonce mDNS débouncée » trace l'oscillation ignorée. Objectif : supprimer le churn `MDNS.end()/begin()` provoqué par les oscillations d'IP du **double NAT CPL**, qui accélérait la fragmentation du tas. Détail des logs mDNS : [logger.md](logger.md#ré-annonce-mdns-sur-changement-dip).

## Constantes ([`constants.h`](../../src/constants.h))

| Constante | Valeur | Rôle |
|---|---|---|
| `kHealthCheckIntervalMs` | `60000` (60 s) | Période d'appel de `checkSystemHealth()` + cadence **nominale** de `checkHeapHealth()` |
| `kHeapCheckFastIntervalMs` | `5000` (5 s) | Cadence **rapprochée** de `checkHeapHealth()` quand `maxBloc8 < kLowHeapWarnBytes` (v2.11.1) |
| `kMinFreeHeapBytes` | `10000` (10 Ko) | Seuil heap **total** → alerte `low_memory` en **transition** (distinct de l'auto-guérison ; toujours sur `ESP.getFreeHeap()`) |
| `kLowHeapWarnBytes` | `12288` (12 Ko) | Plus gros bloc **8 bits** sous ce seuil → WARN + flush one-shot + cadence rapprochée — **20 000 (brute) → 12 288 (8 bits) en v2.33.0** ([ADR-0058](../adr/0058-gardes-heap-metrique-8-bits.md)) : = `kOomReserveBytes`, sous ce niveau la réserve nominale du parachute n'est plus allouable |
| `kRestartMaxAllocHeapBytes` | `4096` (4 Ko) | Plus gros bloc **8 bits** sous ce seuil → candidat au restart — **8 192 (brute) → 4 096 (8 bits) en v2.33.0** (palier auto-heal de l'échelle [ADR-0058](../adr/0058-gardes-heap-metrique-8-bits.md)) |
| `kAutoHealDebounceChecks` | `3` | Ticks consécutifs sous seuil avant restart (anti-oscillation ; ≈ 3 min à cadence nominale, **≈ 15 s** en cadence rapprochée) |
| `kAutoHealMinUptimeMs` | `180000` (3 min) | Uptime minimal avant tout restart auto |
| `kAutoHealMaxReboots` | `3` | Plafond de reboots dans la fenêtre → mode maintenance (**partagé heap + réseau**) |
| `kAutoHealRebootWindowMs` | `900000` (15 min) | Fenêtre glissante anti-boucle de reboot (partagée) |
| `kNetworkProbeIntervalMs` | `30000` (30 s) | Cadence de la sonde ICMP vers la passerelle |
| `kNetworkProbeTimeoutMs` | `2000` (2 s) | Timeout d'un echo ICMP |
| `kNetworkStallSoftMs` | `120000` (2 min) | Pile muette depuis ce délai → SoftRecover (reconnexion WiFi) |
| `kNetworkStallRebootMs` | `300000` (5 min) | Pile muette depuis ce délai → Restart fail-safe |
| `kMdnsReannounceMinIntervalMs` | `30000` (30 s) | Intervalle mini entre 2 ré-annonces mDNS |
| `kHistoryInitCrashThreshold` | `2` | Crashs consécutifs pendant l'init history → effacement de la partition (**hors** budget `kAutoHealMaxReboots`) |
| `kHistoryCrashCountNvsKey` | `"crash_cnt"` | Clé NVS du compteur du disjoncteur (namespace `hist_meta`, clé `part_sz` intacte) |
| `kHistoryDumpCrc0NvsKey` / `kHistoryDumpCrc1NvsKey` / `kHistoryDumpHeadNvsKey` | `"dmp_crc0"` / `"dmp_crc1"` / `"dmp_head"` | Empreinte de la preuve `[HISTDUMP]` (CRC32 blocs 0-1 + 16 premiers octets, namespace `hist_meta`, v2.11.0) |
| `kHeapRingEntries` | `128` | Capacité du ring de télémétrie heap (128 × 8 o = 1 Ko RTC noinit, v2.14.0) |
| `kHeapRingSampleIntervalMs` | `2000` (2 s) | Cadence d'échantillonnage du ring depuis `loop()` |
| `kHeapRingDumpMaxEntries` | `32` | Entrées dumpées au boot post-reset anormal (~64 s de cinétique, ~8 lignes) |
| `kHeapIntegrityCheckIntervalMs` | `300000` (5 min) | Cadence du contrôle d'intégrité du tas (v2.18.8, sous les **6** gardes chimiques — 8 avant la v2.36.0) |
| `kHeapIntegrityWarnDurationUs` | `5000` (5 ms) | Durée au-delà de laquelle un WARN est émis (contrôle conservé) |
| `kHeapIntegrityMaxDurationUs` | `20000` (20 ms) | Durée au-delà de laquelle le contrôle est **désactivé définitivement** |
| `kHeap8LogIntervalMs` | `1800000` (30 min) | Cadence de la [trace de calibrage](#trace-de-calibrage-des-seuils-mémoire-v2190) « Heap 8bits: … » — **était `kHeapLogIntervalMs` = 5 min jusqu'en 2.20.1** (rétention du journal ramenée à ~30 min) |
| `kHeapLogIntervalMs` | `300000` (5 min) | Cadence de l'ancienne trace « Heap: … », restée en commentaire depuis la 2.10.2 |

> ✅ **Depuis la v2.33.0, `kLowHeapWarnBytes` et `kRestartMaxAllocHeapBytes` sont comparées à `heapLargestFreeBlock8Bit()`** avec les valeurs recalibrées ci-dessus ([ADR-0058](../adr/0058-gardes-heap-metrique-8-bits.md)). Jusqu'à la v2.32.x elles lisaient la mesure brute (plancher ~27 636 o) : le seuil de redémarrage n'était matériellement jamais atteignable — le watchdog heap n'avait **jamais déclenché un seul redémarrage** depuis sa création (preuve : PANIC du 29/08 et famine du 02/09, silencieux).

## Alertes MQTT émises

| Type (`{base}/alerts`) | Quand |
|---|---|
| `auto_reboot` | Juste avant le redémarrage propre pour **heap critique** (watchdog mémoire) |
| `network_soft_recover` | Pile réseau muette ≥ 2 min, WiFi associé → reconnexion WiFi tentée (watchdog connectivité, 1×/épisode) |
| `network_stall_reboot` | Juste avant le redémarrage propre pour **gel réseau** confirmé ≥ 5 min (watchdog connectivité) |
| `maintenance_mode` | Bascule en mode maintenance (plafond de reboots **partagé** atteint) |
| `low_memory` | **Entrée** du heap **total** sous `kMinFreeHeapBytes` (émission sur **transition** depuis v2.11.1 — non-retained : un abonné hors ligne à cet instant rate l'épisode ; INFO « Mémoire rétablie » au retour au-dessus, journal uniquement) |
| `history_partition_reset` | Partition history effacée automatiquement par le disjoncteur de boot-loop — publication **différée** depuis `setup()` après `mqttManager.begin()` (v2.10.0) |
| `heap_corruption` | **Corruption du tas détectée** par `heap_caps_check_integrity_all()` (v2.18.8) — **type unique** publié par les **deux** branches : redémarrage propre (`performFailSafeRestart`) et, si le plafond de reboots est atteint, mode maintenance. Dosage coupé et inhibé avant la publication. |

Voir [MQTT.md](../MQTT.md#alertes).

## Cas limites

- **MQTT injoignable au moment du reboot** : `publishAlert(...)` échoue silencieusement (heap comme réseau), le reboot a lieu quand même (best-effort).
- **Heap trop bas pour le flush** : `flushToDisk()` peut échouer sans bloquer la séquence de reboot.
- **Power-cycle manuel** : la RAM RTC est réinitialisée → l'appareil ne repart jamais directement en maintenance après une coupure secteur.
- **Fenêtre expirée** : après 15 min sans reboot, le compteur (partagé) est purgé au boot suivant → les deux auto-guérisons redeviennent disponibles.
- **Coupure WiFi légitime** : ne déclenche **aucune** action du watchdog de connectivité (horloge de stall gelée tant que le WiFi n'est pas associé).
- **Passerelle qui filtre l'ICMP echo** : le signe de vie primaire serait perdu → risque de reboot à tort. Sur LAN domestique standard, la passerelle répond ([ADR-0030](../adr/0030-watchdog-connectivite.md)).
- **Épisode combiné heap + réseau** : les reboots des deux causes se cumulent dans le **même** budget → bascule en maintenance plus tôt (voulu).
- **NVS indisponible au boot** : le disjoncteur history dégrade silencieusement (compteur lu à 0 → `Proceed`) — jamais de blocage du boot.
- **Échec de l'effacement de la partition** (`esp_partition_erase_range != ESP_OK`) : log CRIT « échec de l'effacement », pas d'alerte MQTT (flag non posé), le montage est tenté quand même.
- **Crash non-FS pendant la fenêtre d'init history** : 2 occurrences consécutives effacent l'historique à tort — trade-off assumé ([ADR-0031](../adr/0031-disjoncteur-bootloop-history.md)).
- **Pompe énergisée en continu** : le contrôle d'intégrité du tas n'est **jamais** exécuté tant qu'une des 6 gardes est fermée (fail-closed). ⚠️ Depuis la v2.36.0, une **session de dosage ouverte ne le bloque plus** — seule une pompe **réellement énergisée** le fait — la détection de corruption est **différée**, jamais forcée. Le bit 4 « intégrité jamais exécutée » du post-mortem le signale au boot suivant.
- **Contrôle d'intégrité trop long** (> `kHeapIntegrityMaxDurationUs`) : désactivation **définitive** jusqu'au reboot, avec WARN one-shot — le contrôle ne devient jamais lui-même une source de gigue.

## Fichiers liés

- [`src/self_heal_logic.h`](../../src/self_heal_logic.h) / [`.cpp`](../../src/self_heal_logic.cpp) — décision pure watchdog **heap**
- [`src/net_health_logic.h`](../../src/net_health_logic.h) / [`.cpp`](../../src/net_health_logic.cpp) — décision pure watchdog **connectivité**
- [`src/history_guard_logic.h`](../../src/history_guard_logic.h) / [`.cpp`](../../src/history_guard_logic.cpp) — décision pure disjoncteur **boot-loop history**
- [`src/heap_ring_logic.h`](../../src/heap_ring_logic.h) / [`src/heap_ring.h`](../../src/heap_ring.h) / [`src/heap_ring.cpp`](../../src/heap_ring.cpp) — ring de **télémétrie heap** RTC noinit (v2.14.0, tests `test/test_native_heap_ring/`)
- [`src/heap_metrics.h`](../../src/heap_metrics.h) / [`src/heap_metrics.cpp`](../../src/heap_metrics.cpp) — **source unique** de la mesure « mémoire réellement allouable » (v2.19.0, [ADR-0038](../adr/0038-metrique-memoire-allouable-8-bits.md))
- [`src/heap_caps_logic.h`](../../src/heap_caps_logic.h) — module **pur** des planchers multi-masques (v2.49.0, feature-122) : masques, `heapMaskSelectsHeap`, plancher glissant, `fetch_min`, seaux de durée, fenêtre de contexte, et depuis la **v2.49.1** la règle de cadence du tour de rôle (`heapFloorWalkDue`, `heapFloorWalkNextDivider`, testée par table de vérité **replis compris** : diviseur nul et valeur hors bornes se replient sur « **mesurer** », parce que perdre une mesure est pire que la refaire) — tests `test/test_native_heap_caps_table/` + `test/test_native_heap_floors/`
- [`src/main.cpp`](../../src/main.cpp) — coquille impure (collecte ICMP/heap + effets, `checkHeapHealth` et sa trace de calibrage v2.19.0, `performFailSafeRestart`, alerte différée `history_partition_reset`)
- [`src/history.cpp`](../../src/history.cpp) — coquille impure du disjoncteur (NVS `hist_meta`, `esp_reset_reason`, `esp_partition_erase_range`)
- [`src/pump_controller.h`](../../src/pump_controller.h) — `setMaintenanceInhibit()` / `isMaintenanceInhibited()`
- [`src/oom_guard.cpp`](../../src/oom_guard.cpp) — post-mortem d'allocation RTC (3ᵉ zone noinit) + `oomGuardIntegrityTick()` (mécanisme du contrôle d'intégrité ; **les gardes sont dans `main.cpp`**)
- [oom-guard.md](oom-guard.md) — parachute OOM global (v2.18.5, [ADR-0035](../adr/0035-parachute-oom-new-handler.md)) : absorbe l'échec ponctuel d'un `new` (bad_alloc → abort) que les watchdogs ci-dessus ne voient qu'après coup — l'auto-guérison reste le containment de fond
- [ADR-0029](../adr/0029-auto-guerison-heap.md) — auto-guérison mémoire
- [ADR-0030](../adr/0030-watchdog-connectivite.md) — watchdog de connectivité
- [ADR-0031](../adr/0031-disjoncteur-bootloop-history.md) — disjoncteur de boot-loop history
- [ADR-0037](../adr/0037-postmortem-oom-et-integrite-tas.md) — post-mortem d'allocation RTC + contrôle d'intégrité périodique du tas (alternatives écartées : échantillonnage rapproché du tas, poisoning comprehensive, `ESP.restart()` nu)
- [ADR-0038](../adr/0038-metrique-memoire-allouable-8-bits.md) — **cause racine des 3 crashs** : la métrique heap incluait l'IRAM 32 bits, le seuil d'auto-guérison n'a jamais pu être franchi ; mesure 8 bits obligatoire dans les gardes, découpage mesure (2.19.0) / bascule (2.19.1)
- [ADR-0077](../adr/0077-metrique-de-garde-exclut-les-pools-inatteignables.md) — **une métrique de garde doit exclure les pools qu'un `malloc()` ne peut pas atteindre** (feature-122, v2.49.0) : 3ᵉ occurrence du piège (parachute en PSRAM, métrique IRAM-inclusive, réserve DMA visible par `0x0804`) ; table des masques figée en compilation, bascule vers `0x1804` reportée à `feature-123`
- [ADR-0058](../adr/0058-gardes-heap-metrique-8-bits.md) — **bascule effective des gardes en v2.33.0** (bug-080 lot A) : auto-heal 4 096, WARN 12 288, immunité par débounce temporel, révocation du NO GO d'ADR-0040
