# Subsystem — `oom-guard` (parachute OOM global)

- **Fichiers** : [`src/oom_guard_logic.h`](../../src/oom_guard_logic.h) (logique pure, header-only), [`src/oom_guard.h`](../../src/oom_guard.h) / [`src/oom_guard.cpp`](../../src/oom_guard.cpp) (coquille Arduino), [`src/web_routes_ota.cpp`](../../src/web_routes_ota.cpp) (garde TLS sortante partagée `/check-update` + `/download-update`)
- **Pas de singleton** : fonctions libres (`initOomGuard()`, `oomGuardLoopTick()`, `oomGuardReserveArmed()`, `oomGuardReserveArmedBytes()`, `oomGuardHandlerRegistered()`, `oomGuardNoteHeapSample()`, `oomPostMortemDumpIfBoot()`, `oomGuardIntegrityTick()`, `oomGuardNoteIntegrityResult()`) + fonctions pures inline.
- **Décisions architecturales** : [ADR-0035](../adr/0035-parachute-oom-new-handler.md) (parachute OOM), [ADR-0037](../adr/0037-postmortem-oom-et-integrite-tas.md) (post-mortem d'allocation RTC + contrôle d'intégrité du tas, v2.18.8), [ADR-0038](../adr/0038-metrique-memoire-allouable-8-bits.md) (**mesure 8 bits = mémoire réellement allouable**, v2.19.0), [ADR-0058](../adr/0058-gardes-heap-metrique-8-bits.md) (**bascule des 4 gardes sur la métrique 8 bits, échelle unique**, v2.33.0 — supersede le régime bi-métrique d'[ADR-0040](../adr/0040-pas-de-calibrage-sur-fragmentation-pathologique.md)), [ADR-0036](../adr/0036-psram-requise-mise-a-jour-en-ligne.md) (PSRAM libre requise pour toute session TLS sortante), [ADR-0017](../adr/0017-logique-metier-pure-humble-object-testabilite.md) (logique pure testable), [ADR-0029](../adr/0029-auto-guerison-heap.md) (auto-guérison heap = containment de fond).

> ✅ **v2.33.0 — fin du régime bi-métrique ([ADR-0058](../adr/0058-gardes-heap-metrique-8-bits.md)).** Depuis bug-080 lot A, **toutes les gardes décisionnelles heap** lisent `heapLargestFreeBlock8Bit()` ([`heap_metrics.h`](../../src/heap_metrics.h)) : paliers de réarmement du parachute, auto-guérison (`kRestartMaxAllocHeapBytes` = 4 096), WARN (`kLowHeapWarnBytes` = 12 288), skip des pushes WS (`kWsPushMinAllocBytes` = 6 144) et garde 503 de `/get-logs`. Le NO GO d'[ADR-0040](../adr/0040-pas-de-calibrage-sur-fragmentation-pathologique.md) (v2.20.2) est **révoqué** par le gate `pool-chemistry` du 02/09/2026 : les données nouvelles montrent un nominal fragmenté **stable à 21 492 o** (2.26→2.32) et des creux transitoires de quelques secondes seulement — **l'immunité aux faux positifs vient du débounce temporel** (3 mesures consécutives, famine continue ≥ 10-15 s), pas du niveau du seuil. Restent en métrique **brute**, hors périmètre consigné : `web_routes_debug.cpp:55` (503 trace capteurs) et `web_routes_ota.cpp` (garde TLS `kCheckUpdateMin*`) — suivi lot B ou spec dédiée.

## Rôle

Transformer une **famine heap ponctuelle** (échec d'un `operator new`) en épisode **absorbé et journalisé** au lieu d'un abort sec : sur core Arduino (exceptions C++ activées), un `new` qui échoue lève `std::bad_alloc` — non rattrapé dans une tâche (ex. `async_tcp`), c'est `std::terminate` → abort → reboot sans aucune trace applicative.

Déclencheur : crash de production du 2026-08-11 (v2.18.4) — `operator new(356)` (`new AsyncWebServerRequest`, fork ESP32Async) à l'**acceptation d'une connexion HTTP**, après un effondrement du heap de ~60 Ko en ~5 s dont la cause reste **non attribuée** (spec `bug-oom-accept-http-durcissement`). La garde `if (r == NULL)` de la lib est **morte** sur exceptions activées : `new` lance, ne retourne jamais `nullptr` — et la lib externe n'est pas patchable. Le parachute est **trigger-agnostique** : il protège quelle que soit l'allocation qui échoue.

## Fonctionnement

### Réserve d'urgence — 12 Ko (`kOomReserveBytes`)

`initOomGuard()` fait `malloc(12288)` **au plus tôt du `setup()`** (avant `Serial`/logger/FS : bloc contigu pris avant toute fragmentation) — et depuis la v2.18.8, **à défaut**, `malloc(kOomFallbackReserveBytes)` (palier de repli, voir ci-dessous). Il prend ensuite l'**instantané** de la zone post-mortem RTC du boot précédent, l'invalide, puis enregistre `std::set_new_handler(oomNewHandler)` et `heap_caps_register_failed_alloc_callback(oomFailedAllocHook)`. Aucun log à ce stade (logger non initialisé) — le `setup()` journalise l'armement juste après, avec le **palier réellement obtenu** (`oomGuardReserveArmedBytes()`) et le `maxBloc` du moment :

```
Parachute OOM armé : réserve de 12288 o (palier nominal) pré-allouée, maxBloc8=36852 o
Parachute OOM : réserve non allouée au boot (nominal ET repli refusés) — armement différé en loop
```

> Le log d'armement affiche **`maxBloc8=`** depuis la v2.33.0 (réserve du contre-visa `pool-chemistry`, bug-080) : il affichait la mesure brute alors que les seuils du parachute lisent la mesure 8 bits — un chiffre juste mais trompeur au diagnostic.

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

> **L'immunité aux creux transitoires vient du DÉBOUNCE, pas du niveau des seuils** ([ADR-0058](../adr/0058-gardes-heap-metrique-8-bits.md)) : des creux à 1 268 o de quelques secondes existent en régime normal — aucun seuil statique > 1,3 Ko ne passe dessous. L'auto-guérison exige 3 mesures consécutives (60 s nominale, 5 s sous WARN) : seule une famine **continue ≥ 10-15 s** déclenche. Hors périmètre (encore en métrique brute) : `web_routes_debug.cpp:55` et la garde TLS de `web_routes_ota.cpp` — suivi lot B.

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

Le **post-mortem RTC n'est PAS agrandi** pour les accueillir : la zone de 32 octets est pleine, et l'étendre imposerait un nouveau bump de magic (donc la perte d'une trace) pour une information déjà présente dans le journal.

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

### Contrainte structurante — aucun `heap_caps_*` depuis le chemin d'échec

`heap_caps_register_failed_alloc_callback` est appelé **depuis l'intérieur de l'allocateur**, qui **détient déjà le verrou du composant heap**. Tout appel réentrant (`heap_caps_get_free_size`, `heap_caps_get_largest_free_block`, `ESP.getFreeHeap()`, `ESP.getMaxAllocHeap()`) y est donc un **deadlock** — interdiction absolue, au même titre que les allocations, les mutex et les logs déjà proscrits par [ADR-0035](../adr/0035-parachute-oom-new-handler.md).

Conséquence : le post-mortem ne mesure **jamais** le tas. Sa seule source « free / maxBloc » est un **miroir atomique du dernier échantillon de l'anneau heap** (`oomGuardNoteHeapSample()`, alimenté par `heapRingSample()`) : valeurs **déjà mesurées** à la cadence inchangée de `kHeapRingSampleIntervalMs` = 2 s, pré-encodées pour que le chemin d'échec n'ait qu'un unique store aligné à faire. **Aucun parcours de tas supplémentaire** n'est introduit. L'horloge du chemin d'échec est `xTaskGetTickCount()` (simple lecture de `xTickCount`, sans verrou) — le miroir est horodaté avec **la même** horloge pour que le calcul d'âge reste juste.

### Format de la zone — 32 octets, magic `0xF00DFA12` (bumpé en v2.19.0)

3ᵉ zone RTC noinit du projet, magic **distinct** de l'anneau heap (`0xB10C4EAA`) et de l'auto-guérison (`0xA5C0FFEE`). Vue comme 8 mots de 32 bits **alignés** ; les chemins d'échec n'y font que des **stores bruts**.

> **Pourquoi le magic a été bumpé** (`0xF00DFA11` → `0xF00DFA12`, et `0xB10C4EA9` → `0xB10C4EAA` pour l'anneau, [ADR-0038](../adr/0038-metrique-memoire-allouable-8-bits.md)) : depuis la v2.19.0, les champs « ring » de cette zone portent la mesure **8 bits**. Une zone écrite par un firmware **2.18.x** contient l'ancienne métrique (IRAM 32 bits comprise) ; la relire avec les nouveaux libellés (`free8=`, `maxB8=`) afficherait un chiffre **faux et crédible**. Conséquence assumée : **le post-mortem et la cinétique du boot de bascule 2.18.x → 2.19.0 sont perdus**. Perdre une trace est acceptable, afficher une valeur trompeuse ne l'est pas. Les quatre magics `0xA5C0FFEE`, `0xB10C4EA9`, `0xB10C4EAA`, `0xF00DFA11`, `0xF00DFA12` sont désormais **tous réservés** — aucun ne doit être réemployé.

| Offset | Champ | Description |
|---|---|---|
| 0-3 | `magic` | `kOomPostMortemMagic` (`0xF00DFA12` depuis v2.19.0), écrit **en dernier** à l'initialisation |
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

> ⚠️ **Ce verdict était faux, et on sait pourquoi depuis le 15/08** ([ADR-0038](../adr/0038-metrique-memoire-allouable-8-bits.md)). Le `maxB = 27 584` sur lequel il repose venait de `ESP.getMaxAllocHeap()`, qui compte l'**IRAM 32 bits inutilisable** : c'était un reliquat d'IRAM figé, pas de la mémoire allouable. La comptabilité n'était donc pas « saine », elle était **fausse** — il n'y a jamais eu de corruption (l'intégrité du tas est vérifiée OK toutes les 5 min), la **famine 8 bits était réelle**. Depuis la v2.19.0, le miroir qui alimente ces champs porte la mesure 8 bits : **la règle du verdict, inchangée, redevient exploitable**. Ses paramètres (`kOomCorruptionMaxFails`, `kOomCorruptionBurstSpanS`, `kOomCorruptionSaneBlockRatio`) seront réévalués avec les relevés on-target de la 2.19.1.

### Ligne de dump au boot

`oomPostMortemDumpIfBoot()` est appelée dans `setup()` **juste après** `heapRingDumpIfCrash()` : les deux traces se lisent ensemble (l'anneau donne la **cinétique**, le post-mortem le **point d'échec**). Elle lit l'**instantané** pris par `initOomGuard()` (la zone RTC, elle, a déjà été invalidée et appartient au boot courant). Rien n'est émis si la zone est invalide, ou si elle ne contient ni échec ni corruption. Une **ligne unique**, construite dans un tampon de pile (aucune allocation) :

```
Post-mortem OOM : échecs=2 sur 41 s (1er 100 o, dernier 156 o caps=0x0404, uptime=46512 s),
tas comptabilisé t-2s free8=81408 maxB8=27584, réserve=nominale consommée, handler=désenregistré,
intégrité=jamais exécutée → verdict SUSPICION CORRUPTION
```

> **Libellés suffixés `8` depuis la v2.19.0** (`free8=`, `maxB8=`, idem dans le dump de l'anneau) : le journal doit être **auto-descriptif**, pour qu'en relisant d'anciens logs on ne confonde jamais la mesure 8 bits (réellement allouable) avec l'ancienne mesure « interne totale ». Un log portant `free=` / `maxB=` vient d'un firmware **≤ 2.18.9** et contient la métrique surestimée.

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
| `kOomFailWarnDelayInitialMs` | `30000` (30 s) | Backoff du WARN d'échec — 1ᵉʳ palier (v2.20.2, remplace `kOomFailWarnThrottleMs`) |
| `kOomFailWarnDelayMidMs` | `300000` (5 min) | Backoff — 2ᵉ palier |
| `kOomFailWarnDelayMaxMs` | `1800000` (30 min) | Backoff — palier de **saturation** |
| `kOomFailWarnDelayResetMs` | `3600000` (1 h) | Sans nouvel échec pendant ce délai → retour au palier initial |
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

82 tests natifs dédiés (`test/test_native_oom_guard/`, **+16 en v2.20.2** : `oomShouldRegisterHandler` sous condition C1, escalade et saturation de `oomFailWarnNextDelayMs`, exclusion `oomShouldCountFailure`, paliers de repli recalibrés) : machine à états du handler (libération / désenregistrement), hystérésis et critère pointeur du réarmement (course handler/réarmement incluse), **paliers nominal/repli** de `oomRearmTargetBytes` (surclassement autorisé, déclassement interdit, tas trop dégradé), one-shot du log par épisode, **encodage/décodage/validité/span** du post-mortem (aller-retour, saturations, repliement des uptimes sur 16 bits), **verdict** (frontière stricte de `kOomCorruptionMaxFails`, fenêtre de rafale, ratio de comptabilité saine, cas indéterminés), cadence et autolimitation du contrôle d'intégrité, garde `oomTlsGateReason` — motifs et **priorité PSRAM sur heap**, PSRAM saturée, frontières inclusives des trois seuils — **582 tests au total**.

## Fichiers liés

- [`src/main.cpp`](../../src/main.cpp) — `initOomGuard()` en tête de `setup()`, `oomPostMortemDumpIfBoot()` après le dump de l'anneau, `oomGuardLoopTick()` et `oomGuardIntegrityTick()` (sous gardes chimiques) dans `loop()`, `handleHeapCorruptionDetected()`
- [`src/heap_metrics.h`](../../src/heap_metrics.h) / [`src/heap_metrics.cpp`](../../src/heap_metrics.cpp) — **source unique** de la mesure « mémoire réellement allouable » (`heapFree8Bit()`, `heapLargestFreeBlock8Bit()`, `heapMetricsSnapshot()`), v2.19.0. **Interdit d'appel depuis `oomFailedAllocHook` et depuis le `new_handler`** (réentrance allocateur) — au même titre que tout `heap_caps_*`.
- [`src/heap_ring.cpp`](../../src/heap_ring.cpp) — source unique des mesures : alimente l'anneau **et** le miroir `oomGuardNoteHeapSample()` (cadence 2 s inchangée ; mesure 8 bits depuis v2.19.0)
- [system-health.md](system-health.md) — auto-guérison heap (containment de fond), ring de télémétrie (cinétique pré-crash), contrôle d'intégrité et gardes chimiques, budget RAM RTC noinit consolidé
- [ADR-0035](../adr/0035-parachute-oom-new-handler.md) — décision et alternatives écartées (parachute)
- [ADR-0037](../adr/0037-postmortem-oom-et-integrite-tas.md) — post-mortem RTC + miroir de l'anneau + verdict + intégrité périodique + recalibrage du réarmement (v2.18.8)
- [ADR-0036](../adr/0036-psram-requise-mise-a-jour-en-ligne.md) — PSRAM libre requise pour toute session TLS sortante (alternatives écartées : seuil maxBloc relevé, tentative rattrapée, `/download-update` non gardée)
- [ADR-0038](../adr/0038-metrique-memoire-allouable-8-bits.md) — **cause racine des 3 crashs** : mesure 8 bits obligatoire dans les gardes, helper centralisé, double exposition, découpage mesure (2.19.0) puis bascule (2.19.1), bump des deux magics RTC
- [ADR-0040](../adr/0040-pas-de-calibrage-sur-fragmentation-pathologique.md) — **on ne calibre pas des seuils sur une fragmentation pathologique** : régime bi-métrique assumé en v2.20.2, règle de recalibrage d'ADR-0038 déclarée inapplicable, mesures du 15/08 invalidées — **superseded par ADR-0058** (v2.33.0)
- [ADR-0058](../adr/0058-gardes-heap-metrique-8-bits.md) — **bascule des 4 gardes sur la métrique 8 bits** (v2.33.0) : échelle unique 2 048 < 4 096 < 6 144 < 12 288 < 18 432, immunité par débounce temporel, réarmement 18 432 verrouillé par test, révocation du NO GO v2.20.2
