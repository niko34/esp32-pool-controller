# Subsystem — `oom-guard` (parachute OOM global)

- **Fichiers** : [`src/oom_guard_logic.h`](../../src/oom_guard_logic.h) (logique pure, header-only), [`src/oom_guard.h`](../../src/oom_guard.h) / [`src/oom_guard.cpp`](../../src/oom_guard.cpp) (coquille Arduino), [`src/web_routes_ota.cpp`](../../src/web_routes_ota.cpp) (garde TLS sortante partagée `/check-update` + `/download-update`)
- **Pas de singleton** : fonctions libres (`initOomGuard()`, `oomGuardLoopTick()`, `oomGuardReserveArmed()`, `oomGuardReserveArmedBytes()`, `oomGuardNoteHeapSample()`, `oomPostMortemDumpIfBoot()`, `oomGuardIntegrityTick()`, `oomGuardNoteIntegrityResult()`) + fonctions pures inline.
- **Décisions architecturales** : [ADR-0035](../adr/0035-parachute-oom-new-handler.md) (parachute OOM), [ADR-0037](../adr/0037-postmortem-oom-et-integrite-tas.md) (post-mortem d'allocation RTC + contrôle d'intégrité du tas, v2.18.8), [ADR-0036](../adr/0036-psram-requise-mise-a-jour-en-ligne.md) (PSRAM libre requise pour toute session TLS sortante), [ADR-0017](../adr/0017-logique-metier-pure-humble-object-testabilite.md) (logique pure testable), [ADR-0029](../adr/0029-auto-guerison-heap.md) (auto-guérison heap = containment de fond).

## Rôle

Transformer une **famine heap ponctuelle** (échec d'un `operator new`) en épisode **absorbé et journalisé** au lieu d'un abort sec : sur core Arduino (exceptions C++ activées), un `new` qui échoue lève `std::bad_alloc` — non rattrapé dans une tâche (ex. `async_tcp`), c'est `std::terminate` → abort → reboot sans aucune trace applicative.

Déclencheur : crash de production du 2026-08-11 (v2.18.4) — `operator new(356)` (`new AsyncWebServerRequest`, fork ESP32Async) à l'**acceptation d'une connexion HTTP**, après un effondrement du heap de ~60 Ko en ~5 s dont la cause reste **non attribuée** (spec `bug-oom-accept-http-durcissement`). La garde `if (r == NULL)` de la lib est **morte** sur exceptions activées : `new` lance, ne retourne jamais `nullptr` — et la lib externe n'est pas patchable. Le parachute est **trigger-agnostique** : il protège quelle que soit l'allocation qui échoue.

## Fonctionnement

### Réserve d'urgence — 12 Ko (`kOomReserveBytes`)

`initOomGuard()` fait `malloc(12288)` **au plus tôt du `setup()`** (avant `Serial`/logger/FS : bloc contigu pris avant toute fragmentation) — et depuis la v2.18.8, **à défaut**, `malloc(kOomFallbackReserveBytes)` (palier de repli, voir ci-dessous). Il prend ensuite l'**instantané** de la zone post-mortem RTC du boot précédent, l'invalide, puis enregistre `std::set_new_handler(oomNewHandler)` et `heap_caps_register_failed_alloc_callback(oomFailedAllocHook)`. Aucun log à ce stade (logger non initialisé) — le `setup()` journalise l'armement juste après, avec le **palier réellement obtenu** (`oomGuardReserveArmedBytes()`) et le `maxBloc` du moment :

```
Parachute OOM armé : réserve de 12288 o (palier nominal) pré-allouée, maxBloc=48916 o
Parachute OOM : réserve non allouée au boot (nominal ET repli refusés) — armement différé en loop
```

Dimensionnement des 12 Ko :

- **≈ 34 allocations vitales** de la taille du crash de référence (12 288 / 356 o ≈ 34) — accepts HTTP, trames WS, `String` de logs : de quoi tenir la phase critique ;
- **1,5× le seuil d'auto-guérison** `kRestartMaxAllocHeapBytes` (8 192 o, [ADR-0029](../adr/0029-auto-guerison-heap.md)) : la libération redonne au système assez de marge pour que le watchdog heap (débounce ~15 s en cadence rapprochée) déclenche un **reboot propre** (pompes coupées, cumuls persistés, alerte MQTT) pendant la famine, au lieu d'un abort.

### Contrat du handler (`oomNewHandler`) — chemin d'urgence

Le new_handler est appelé par libstdc++ quand le `malloc` interne d'`operator new` échoue, puis **le `new` en cours est RE-TENTÉ** (contrat `std::set_new_handler`). Il peut être invoqué depuis **n'importe quelle tâche** : le chemin est réduit au strict minimum — **aucune allocation, aucun log, aucun mutex** (réentrance allocateur interdite), état partagé 100 % atomique (`seq_cst`, chemins froids).

1. **Extraction atomique** de la réserve (`exchange(nullptr)`) ;
2. réserve disponible → `free()` (le `new` re-tenté réussit) + compteur d'épisodes → état `Released` ;
3. réserve **déjà consommée** → `std::set_new_handler(nullptr)` (désenregistrement) → état `Depleted` : le `new` suivant lève `bad_alloc` = **statu quo ante** (comportement d'avant le parachute), le **coredump reste le filet ultime**.

Le log CRITICAL « **Réserve OOM consommée** (épisode n°N) » est émis en **DIFFÉRÉ** depuis la loop, **one-shot par épisode** (comparaison de compteurs monotones, `oomShouldLogRelease`), avec maxBloc/libre au moment du drain. Pas de `flushToDisk()` dans ce chemin (une écriture LittleFS en pleine famine aggraverait la pression) — la persistance est couverte par le flush one-shot de `checkHeapHealth()` (`kLowHeapWarnBytes`).

### Réarmement — deux paliers, 24 Ko / 12 Ko, tick 10 s (recalibré en v2.18.8)

`oomGuardLoopTick()` (appelé depuis `loop()`) re-alloue la réserve à cadence lente (`kOomRearmCheckIntervalMs` = 10 s). Le ré-enregistrement du handler est inconditionnel (idempotent — couvre l'état `Depleted`), et le succès est journalisé en INFO (« Parachute OOM réarmé » / « Parachute OOM surclassé », avec le palier obtenu et le `maxBloc`).

#### Pourquoi le seuil est passé de 32 768 à 24 576 o

**Le parachute livré en v2.18.5 ne se réarmait JAMAIS** — défaut confirmé par le 2ᵉ crash de production (13/08/2026, v2.18.7, uptime 12,9 h). Pour que libstdc++ lève à `new_op.cc:54`, il faut `get_new_handler() == nullptr` : le handler s'était donc **désenregistré** (état `Depleted`) lors d'un épisode antérieur. Or le réarmement exigeait `maxBloc ≥ 32 768 o`, alors que le plus gros bloc contigu **réel** de cet appareil vaut :

| Mesure terrain (anneau RTC) | Plus gros bloc |
|---|---|
| uptime 33 min | **29 684 o** |
| uptime 12,9 h | **27 584 o** |

Le seuil était donc **au-dessus du régime normal de l'appareil** : une fois la réserve consommée, elle n'était plus jamais ré-allouée et le handler restait désenregistré → **parachute one-shot en pratique**, inopérant au moment du crash du 13/08 (`new AsyncFileResponse`, 156 o, refusé alors que l'anneau annonçait `free=81 408` / `maxB=27 584`, stables depuis 78 s).

Nouvelle valeur : `kOomRearmMinMaxAllocBytes = 24576` = **2 × `kOomReserveBytes`** — atteignable dans le régime réel, tout en gardant une marge d'un bloc entier au-dessus de la taille à allouer (pas d'oscillation libération/réallocation autour du seuil de famine).

#### Palier de repli — 2 Ko dès 12 Ko de plus gros bloc

« Mieux vaut un petit parachute qu'aucun parachute. » Si le bloc **nominal** de 12 Ko n'est pas allouable, le guard s'arme quand même avec `kOomFallbackReserveBytes = 2048` o dès que `maxBloc ≥ kOomFallbackRearmMinMaxAllocBytes = 12 288` o. 2 Ko couvrent **8×** le duo fautif du 13/08 (156 o `AsyncFileResponse` + 100 o `__cxa_allocate_exception`) et largement l'accept HTTP de 356 o du 11/08.

Règle de transition (fonction pure `oomRearmTargetBytes`) :

- **surclassement** repli → nominal **autorisé** : dès que `maxBloc ≥ 24 576`, la réserve de 2 Ko est remplacée par 12 Ko (l'ancienne est libérée par l'`exchange`) ;
- **déclassement** nominal → repli **INTERDIT** : jamais d'oscillation ni de dégradation silencieuse ;
- réserve déjà au palier nominal → aucune action.

#### Échelle complète des seuils « plus gros bloc contigu »

Ces cinq seuils sont **interdépendants** : ils décrivent une dégradation ordonnée du tas et ne se modifient pas isolément ([ADR-0037](../adr/0037-postmortem-oom-et-integrite-tas.md)).

| Seuil | Constante | Effet |
|---|---|---|
| 8 192 o | `kRestartMaxAllocHeapBytes` | Reboot d'auto-guérison ([ADR-0029](../adr/0029-auto-guerison-heap.md)) |
| 12 288 o | `kOomFallbackRearmMinMaxAllocBytes` | Réarmement de la réserve de **repli** (2 Ko) |
| 16 384 o | `kWsPushMinAllocBytes` | Pushes WebSocket sautés |
| 20 000 o | `kLowHeapWarnBytes` | WARN heap + flush disque one-shot |
| 24 576 o | `kOomRearmMinMaxAllocBytes` | Réarmement de la réserve **nominale** (12 Ko) |

Ceinture supplémentaire v2.18.8 : le handler remet la taille à 0 **avant** d'extraire le pointeur ; lire « pointeur vivant + taille 0 » dans cette fenêtre ferait passer une réserve nominale pour non armée et autoriserait un déclassement — la loop suppose donc le palier nominal dans ce cas.

**Critère de réarmement = ABSENCE DE RÉSERVE (pointeur nul), pas l'état** — durcissement de revue v2.18.5 : si le handler consomme la nouvelle réserve entre l'`exchange` du réarmement et le `store(Armed)`, l'état peut afficher `Armed` avec une réserve nulle ; fonder `oomShouldRearm` sur le pointeur garantit la convergence au tick suivant. L'`exchange` à la ré-allocation (libération d'une réserve résiduelle théorique) ferme le double-free/fuite en ceinture-bretelles.

### Machine à états (logique pure `oom_guard_logic.h`)

```
             new échoue, réserve dispo (exchange non nul)
 [ Armed ] ────────────────────────────────────────────► [ Released ]
     ▲          free(réserve) + épisode++ (le new re-tenté réussit)         │
     │                                                                      │
     │  réserve ABSENTE ET maxBloc ≥ 24 Ko (nominal) ou ≥ 12 Ko (repli)    │
     │  — tick 10 s, surclassement repli → nominal autorisé                 │
     ├──────────────────────────────────────────────────────────────────────┤
     │                                                                      ▼
     │          new échoue, réserve déjà consommée                  [ Depleted ]
     └──────── (même chemin de réarmement) ◄──── set_new_handler(nullptr) :
                                                 bad_alloc suit son cours
                                                 (statu quo ante, coredump)
```

### Callback IDF diagnostic (`heap_caps_register_failed_alloc_callback`) — flag-seul

**Complément d'attribution** : appelé depuis l'**intérieur de l'allocateur** à CHAQUE échec d'allocation — y compris les **`malloc` C que le new_handler ne voit pas** (lwIP, mbedTLS, drivers). Mode **flag-seul** : le callback ne fait que poser taille demandée + compteur atomiques (pas de free, pas de log) ; la loop draine en WARN « Allocation heap échouée ×N (dernière taille demandée=…) » **throttlé** `kOomFailWarnThrottleMs` = 30 s. Depuis la v2.18.8, il écrit **en plus** le post-mortem RTC ci-dessous (stores bruts uniquement).

## Post-mortem d'allocation en RAM RTC noinit (v2.18.8)

Défaut n°2 du crash du 13/08 : le CRITICAL « Réserve OOM consommée » est émis **en différé** par la loop → **perdu** quand l'abort suit immédiatement, et le journal persistant d'avant crash l'est aussi (flush toutes les 10 min). Seule la RAM RTC noinit survit. Le firmware y enregistre donc l'état **au moment exact de l'échec** et le **dumpe au boot suivant**, juste après le dump de l'anneau heap.

### Contrainte structurante — aucun `heap_caps_*` depuis le chemin d'échec

`heap_caps_register_failed_alloc_callback` est appelé **depuis l'intérieur de l'allocateur**, qui **détient déjà le verrou du composant heap**. Tout appel réentrant (`heap_caps_get_free_size`, `heap_caps_get_largest_free_block`, `ESP.getFreeHeap()`, `ESP.getMaxAllocHeap()`) y est donc un **deadlock** — interdiction absolue, au même titre que les allocations, les mutex et les logs déjà proscrits par [ADR-0035](../adr/0035-parachute-oom-new-handler.md).

Conséquence : le post-mortem ne mesure **jamais** le tas. Sa seule source « free / maxBloc » est un **miroir atomique du dernier échantillon de l'anneau heap** (`oomGuardNoteHeapSample()`, alimenté par `heapRingSample()`) : valeurs **déjà mesurées** à la cadence inchangée de `kHeapRingSampleIntervalMs` = 2 s, pré-encodées pour que le chemin d'échec n'ait qu'un unique store aligné à faire. **Aucun parcours de tas supplémentaire** n'est introduit. L'horloge du chemin d'échec est `xTaskGetTickCount()` (simple lecture de `xTickCount`, sans verrou) — le miroir est horodaté avec **la même** horloge pour que le calcul d'âge reste juste.

### Format de la zone — 32 octets, magic `0xF00DFA11`

3ᵉ zone RTC noinit du projet, magic **distinct** de l'anneau heap (`0xB10C4EA9`) et de l'auto-guérison (`0xA5C0FFEE`). Vue comme 8 mots de 32 bits **alignés** ; les chemins d'échec n'y font que des **stores bruts**.

| Offset | Champ | Description |
|---|---|---|
| 0-3 | `magic` | `kOomPostMortemMagic` (`0xF00DFA11`), écrit **en dernier** à l'initialisation |
| 4-7 | `failCount` | Échecs d'allocation cumulés du boot |
| 8-11 | `firstSize` | Taille demandée au 1ᵉʳ échec (octets) |
| 12-15 | `lastSize` | Taille demandée au dernier échec |
| 16-19 | `lastCaps` | Masque `heap_caps` du dernier échec |
| 20-21 | `firstUptimeS` | Uptime (s) du 1ᵉʳ échec, **modulo 65 536** |
| 22-23 | `lastUptimeS` | Uptime (s) du dernier échec, modulo 65 536 |
| 24-25 | `ringFree >> 8` | Heap libre du dernier échantillon d'anneau (quantum 256 o) |
| 26-27 | `ringMaxB >> 6` | Plus gros bloc du même échantillon (quantum 64 o) |
| 28-29 | `ringUptimeS` | Uptime (s) de cet échantillon → **âge** = `lastUptimeS − ringUptimeS` |
| 30 | `flags` | Drapeaux (ci-dessous) |
| 31 | `releaseCount` | Épisodes de libération de la réserve (saturé à 255) |

### Drapeaux (octet 30) — le « drapeau réserve consommée » demandé par la spec

Il n'y a **pas** de variable RTC séparée : le drapeau **est** cet octet. Il est posé **immédiatement** par le handler, donc il survit à un abort qui suit dans la microseconde — là où le CRITICAL différé par la loop est perdu.

| Bit | Constante | Signification |
|---|---|---|
| 0 (`0x01`) | `kOomPmFlagNominalUsed` | Réserve **nominale** (12 Ko) consommée |
| 1 (`0x02`) | `kOomPmFlagFallbackUsed` | Réserve de **repli** (2 Ko) consommée |
| 2 (`0x04`) | `kOomPmFlagHandlerUnregistered` | `new_handler` désenregistré (le `bad_alloc` a suivi son cours) |
| 3 (`0x08`) | `kOomPmFlagIntegrityFailed` | Contrôle d'intégrité du tas **ÉCHOUÉ** |
| 4 (`0x10`) | `kOomPmFlagIntegrityNeverRun` | Contrôle d'intégrité **jamais exécuté** (valeur initiale) |

### Verdict — règle exacte

`oomPostMortemVerdict()` (fonction pure) départage les deux hypothèses laissées ouvertes par le 13/08 : **(a)** consommateur très rapide dans l'angle mort de 2 s de l'anneau, **(b)** corruption du tas (comptabilité saine, allocateur qui refuse 156 o).

1. `failCount == 0` **ou** `lastSize == 0` **ou** aucun échantillon d'anneau → `INDÉTERMINÉ` ;
2. `failCount > kOomCorruptionMaxFails` (**comparaison stricte** : 9 échecs déclenchent, pas 8) **ET** rafale, c'est-à-dire span `lastUptimeS − firstUptimeS` ≤ `kOomCorruptionBurstSpanS` (60 s) → **`FAMINE MÉMOIRE`** ;
3. sinon, comptabilité **non** saine (`ringMaxB / kOomCorruptionSaneBlockRatio < lastSize`, soit maxBloc < 4 × taille demandée) → **`FAMINE MÉMOIRE`** ;
4. sinon (comptabilité saine : maxBloc ≥ 4 × la taille demandée) → **`SUSPICION CORRUPTION`**.

> **La fenêtre de rafale vient d'une revue.** Le compteur d'échecs est cumulé sur **tout le boot** : sans borne temporelle, quelques échecs bénins étalés sur des heures auraient conclu « famine » sur le scénario **même** que ce verdict doit trancher (13/08 : 2 échecs, tas comptabilisé sain). Une vraie famine refuse tout en quelques secondes — 60 s couvrent largement l'épisode du 11/08 (~14 s).

Sur le cas du 13/08, la règle conclut donc `SUSPICION CORRUPTION` : 156 o refusés avec `maxB = 27 584` annoncé, soit **176×** la taille demandée.

### Ligne de dump au boot

`oomPostMortemDumpIfBoot()` est appelée dans `setup()` **juste après** `heapRingDumpIfCrash()` : les deux traces se lisent ensemble (l'anneau donne la **cinétique**, le post-mortem le **point d'échec**). Elle lit l'**instantané** pris par `initOomGuard()` (la zone RTC, elle, a déjà été invalidée et appartient au boot courant). Rien n'est émis si la zone est invalide, ou si elle ne contient ni échec ni corruption. Une **ligne unique**, construite dans un tampon de pile (aucune allocation) :

```
Post-mortem OOM : échecs=2 sur 41 s (1er 100 o, dernier 156 o caps=0x0404, uptime=46512 s),
tas comptabilisé t-2s free=81408 maxB=27584, réserve=nominale consommée, handler=désenregistré,
intégrité=jamais exécutée → verdict SUSPICION CORRUPTION
```

Niveau : **CRITICAL** si le verdict est `SUSPICION CORRUPTION` ou si le bit « intégrité échouée » est posé (information décisive, persistée), **WARN** sinon — même politique que le dump de l'anneau. Libellés : `réserve` = `intacte` / `nominale consommée` / `repli consommée` / `nominale+repli consommées` ; `handler` = `actif` / `désenregistré` ; `intégrité` = `OK` / `ÉCHEC` / `jamais exécutée`.

### Limites assumées (à connaître au diagnostic)

1. **Le post-mortem est une trace INDICATIVE, jamais un compteur exact.** Les mots `failCount` et `uptimes` sont modifiés en **lecture-modification-écriture sans verrou** (aucun verrou n'est autorisé sur ce chemin) : deux échecs simultanés sur les deux cœurs peuvent perdre un incrément.
2. **L'instantané pris au démarrage vit en RAM ordinaire** (`.bss`) : un crash survenant **entre** `initOomGuard()` et le dump fait perdre le post-mortem du boot **n-2** (celui du boot n-1 reste intact). Fenêtre de quelques centaines de millisecondes de `setup()`. Ce compromis est le prix de la protection inverse : sans instantané, un échec d'allocation pendant le `setup()` — le moment le plus fragile, juste après un crash mémoire — polluerait la zone du boot précédent avant qu'elle ait été lue.
3. **Les uptimes sont tronqués sur 16 bits** (repliement toutes les **18 h 12**). Les durées (span, âge de l'échantillon) sont calculées en **arithmétique modulo** ; surtout, la **validité** de la zone ne dépend **pas** de l'ordre des uptimes. Une revue avait trouvé ce défaut : un contrôle `lastUptimeS ≥ firstUptimeS` rendait le dump **muet** dès qu'un repliement s'intercalait, c'est-à-dire sur tout appareil tournant plus de 18 h — précisément le régime des deux crashs.

## Contrôle d'intégrité périodique du tas (v2.18.8)

`oomGuardIntegrityTick()` exécute `heap_caps_check_integrity_all()` toutes les `kHeapIntegrityCheckIntervalMs` = 5 min : **détecteur direct** de l'hypothèse (b). Il retourne `true` **une seule fois**, au tick où une corruption est détectée ; l'appelant (`main.cpp`) exécute alors le chemin d'arrêt propre. Les **gardes de sécurité chimique**, la cadence d'appel et l'action sur échec sont documentées dans [system-health.md](system-health.md#contrôle-dintégrité-périodique-du-tas-v2188) — ce module n'expose que le mécanisme, **l'appelant est responsable des gardes**.

## Garde TLS sortante partagée — `oomTlsGateReason` (v2.18.6)

Prédicat pur **partagé par les deux routes qui ouvrent une session TLS sortante** — `/check-update` **et** `/download-update` ([`src/web_routes_ota.cpp`](../../src/web_routes_ota.cpp), fonction commune `refuseIfTlsGateClosed`) : aucune comparaison de seuil dans les handlers.

Il retourne un **motif** (`uint8_t`) et non plus un booléen — l'appelant doit distinguer un refus **matériel définitif** d'un refus **temporaire réessayable** :

| Motif | Condition | Réponse HTTP (`code`) |
|---|---|---|
| `kOomTlsGateOk` | tous les critères passés | session TLS autorisée |
| `kOomTlsGateNoPsram` | PSRAM **libre** < `kOtaTlsMinPsramBytes` (1 Mo) | **503** — `psram_required` **ou** `psram_low` selon la nullité de la PSRAM libre (voir ci-dessous) |
| `kOomTlsGateLowMemory` | heap libre < `kCheckUpdateMinFreeHeapBytes` **OU** maxBloc < `kCheckUpdateMinMaxAllocBytes` | **503** `{"error":"Mémoire insuffisante pour TLS, réessayez","code":"tls_low_memory"}` |

### Mapping motif → code HTTP (v2.18.7)

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
- **La cause racine du crash du 13/08 reste INCONNUE.** La v2.18.8 **instrumente**, elle ne répare pas : elle rétablit une protection qui était inopérante et met en place de quoi **trancher famine / corruption** à la prochaine occurrence. Les trois limites du post-mortem ci-dessus s'appliquent à cette trace.

## Constantes ([`constants.h`](../../src/constants.h))

| Constante | Valeur | Rôle |
|---|---|---|
| `kOomReserveBytes` | `12288` (12 Ko) | Réserve d'urgence **nominale** (≈ 34 allocs vitales de 356 o ; 1,5× le seuil auto-guérison 8 192 o) |
| `kOomRearmMinMaxAllocBytes` | `24576` (24 Ko) | Hystérésis : maxBloc minimal pour ré-allouer la réserve nominale — **recalibré en v2.18.8** (était 32 768, au-dessus du régime réel 27-30 Ko → parachute jamais réarmé) ; = 2 × `kOomReserveBytes` |
| `kOomFallbackReserveBytes` | `2048` (2 Ko) | Réserve de **repli** quand le nominal n'est pas allouable (v2.18.8) |
| `kOomFallbackRearmMinMaxAllocBytes` | `12288` (12 Ko) | maxBloc minimal pour armer le palier de repli (v2.18.8) |
| `kOomRearmCheckIntervalMs` | `10000` (10 s) | Cadence du contrôle de réarmement dans la loop |
| `kOomFailWarnThrottleMs` | `30000` (30 s) | Max 1 WARN « allocation heap échouée » par fenêtre |
| `kOomCorruptionMaxFails` | `8` | Au-delà (comparaison **stricte**) **et** en rafale → verdict `FAMINE MÉMOIRE` (v2.18.8) |
| `kOomCorruptionBurstSpanS` | `60` (60 s) | Fenêtre de **rafale** du verdict : span entre 1ᵉʳ et dernier échec (v2.18.8) |
| `kOomCorruptionSaneBlockRatio` | `4` | maxBloc ≥ 4 × taille demandée = comptabilité **saine** → `SUSPICION CORRUPTION` (v2.18.8) |
| `kHeapIntegrityCheckIntervalMs` | `300000` (5 min) | Cadence du contrôle d'intégrité du tas (v2.18.8, gardes chimiques côté appelant) |
| `kHeapIntegrityWarnDurationUs` | `5000` (5 ms) | Au-delà : WARN (contrôle conservé) |
| `kHeapIntegrityMaxDurationUs` | `20000` (20 ms) | Au-delà : contrôle **désactivé définitivement** |
| `kOtaTlsMinPsramBytes` | `1048576` (1 Mo) | Garde TLS : **PSRAM libre** minimale (v2.18.6, critère prioritaire fail-closed) |
| `kCheckUpdateMinFreeHeapBytes` | `65536` (64 Ko) | Garde TLS : heap libre total minimal (**inchangé** en v2.18.6) |
| `kCheckUpdateMinMaxAllocBytes` | `20480` (20 Ko) | Garde TLS : plus gros bloc contigu minimal (**inchangé** en v2.18.6) |

> **Note WROVER/PSRAM** : sur module PSRAM (WROVER-E), les buffers mbedTLS > 4 Ko partent automatiquement en PSRAM (`SPIRAM_USE_MALLOC`, seuil 4096) — les deux seuils **heap** restent dimensionnés pour la RAM interne d'un WROOM et deviennent conservateurs sur WROVER (cf. [ADR-0034](../adr/0034-migration-core3-idf5.md)). `kOtaTlsMinPsramBytes` = 1 Mo n'a **jamais été mesuré sur WROVER réel** : valeur volontairement large, **à re-mesurer à la mise en service** du premier module PSRAM.

## Tests

66 tests natifs dédiés (`test/test_native_oom_guard/`, **+41 en v2.18.8**) : machine à états du handler (libération / désenregistrement), hystérésis et critère pointeur du réarmement (course handler/réarmement incluse), **paliers nominal/repli** de `oomRearmTargetBytes` (surclassement autorisé, déclassement interdit, tas trop dégradé), one-shot du log par épisode, **encodage/décodage/validité/span** du post-mortem (aller-retour, saturations, repliement des uptimes sur 16 bits), **verdict** (frontière stricte de `kOomCorruptionMaxFails`, fenêtre de rafale, ratio de comptabilité saine, cas indéterminés), cadence et autolimitation du contrôle d'intégrité, garde `oomTlsGateReason` — motifs et **priorité PSRAM sur heap**, PSRAM saturée, frontières inclusives des trois seuils — **507 tests au total**.

## Fichiers liés

- [`src/main.cpp`](../../src/main.cpp) — `initOomGuard()` en tête de `setup()`, `oomPostMortemDumpIfBoot()` après le dump de l'anneau, `oomGuardLoopTick()` et `oomGuardIntegrityTick()` (sous gardes chimiques) dans `loop()`, `handleHeapCorruptionDetected()`
- [`src/heap_ring.cpp`](../../src/heap_ring.cpp) — source unique des mesures : alimente l'anneau **et** le miroir `oomGuardNoteHeapSample()` (cadence 2 s inchangée)
- [system-health.md](system-health.md) — auto-guérison heap (containment de fond), ring de télémétrie (cinétique pré-crash), contrôle d'intégrité et gardes chimiques, budget RAM RTC noinit consolidé
- [ADR-0035](../adr/0035-parachute-oom-new-handler.md) — décision et alternatives écartées (parachute)
- [ADR-0037](../adr/0037-postmortem-oom-et-integrite-tas.md) — post-mortem RTC + miroir de l'anneau + verdict + intégrité périodique + recalibrage du réarmement (v2.18.8)
- [ADR-0036](../adr/0036-psram-requise-mise-a-jour-en-ligne.md) — PSRAM libre requise pour toute session TLS sortante (alternatives écartées : seuil maxBloc relevé, tentative rattrapée, `/download-update` non gardée)
