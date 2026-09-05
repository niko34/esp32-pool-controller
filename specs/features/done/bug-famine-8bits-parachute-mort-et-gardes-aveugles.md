# Bug — famine 8 bits : parachute définitivement mort, gardes aveugles, journal détruit

- **Type** : bug critique (mémoire / disponibilité / forensique)
- **Statut** : **livré — tests verts, revue OK (Approved après 3 passes)** — blocs A, C, D, E1, F livrés en **v2.20.2** ; **bloc E (bascule des 5 gardes restantes + recalibrage) DIFFÉRÉ sur NO GO du gate `pool-chemistry`** → `specs/features/todo/bug-bascule-gardes-8bits-recalibrage.md`
- **Priorité** : critique — l'appareil tournait SANS filet anti-`bad_alloc` (corrigé)
- **Version livrée** : **2.20.2** (le plan parlait de « 2.19.1 » ; la numérotation réelle a suivi la branche 2.20.x)
- **Créée** : 2026-08-15, après le 4ᵉ crash de production (16:43:46)
- **Domaine** : Mémoire, OOM guard, Journal, et indirectement **Régulation** (mode maintenance)
- **Specs liées** : `bug-metrique-heap-inclut-iram-32bits` (2.19.0, la mesure), ADR-0035, ADR-0037, ADR-0038

## Mesures on-target (firmware 2.20.0 core 3, 5 h d'uptime)

```
libre8 = 46 480      maxBloc8 = 4 596      minMaxBloc8_boot = 3 060
interne: libre = 74 472   maxBloc = 27 636   ← FIGÉ (reliquat IRAM 32 bits)
```

Traces de calibrage sur 1 h (`Heap 8bits:` toutes les 5 min) :

| heure | libre8 | maxBloc8 | minMaxBloc8_boot | maxBloc interne |
|---|---|---|---|---|
| 20:40 | 47 240 | 9 204 | 3 060 | 27 636 |
| 20:45 | 46 088 | 4 596 | 3 060 | 27 636 |
| 21:20 | 40 324 | 4 084 | 3 060 | 27 636 |
| 21:25 | 49 780 | 4 596 | 3 060 | 27 636 |
| 21:30 | 44 960 | 4 596 | 3 060 | 27 636 |
| 21:40 | 47 776 | 4 596 | 3 060 | 27 636 |

Et **toutes les 30 s depuis le boot** : `WARN Allocation heap échouée ×3 (dernière taille
demandée=12288 o, cumul=1025)`.

Lecture : **ce n'est pas un épuisement, c'est une fragmentation à 10:1.** `libre8` est
stable et sain (40-50 Ko) ; aucun bloc contigu ne dépasse 4,6 Ko.

## Les quatre défauts établis

### 1. La boucle de réarmement infinie (PROUVÉ par lecture de code)

`oomGuardLoopTick()` (`src/oom_guard.cpp`, cadence `kOomRearmCheckIntervalMs = 10 s`) lit
`ESP.getMaxAllocHeap()` — la métrique fausse, **figée à 27 636**. Comme
`27 636 ≥ kOomRearmMinMaxAllocBytes (24 576)`, `oomRearmTargetBytes` retourne le palier
nominal **12 288**, et `malloc(12288)` échoue à jamais (plus gros bloc réel : 4 596).
Retenté toutes les 10 s, soit **3 échecs par fenêtre de 30 s** — exactement le « ×3 »
observé. La garde fabrique elle-même 99 % de son propre signal d'alarme.

### 2. Le parachute est DÉFINITIVEMENT MORT (PROUVÉ) — le plus grave

La ré-inscription de `std::set_new_handler(oomNewHandler)` se trouve **après** le `return`
de la branche d'échec de réarmement. Or le handler se désinscrit lui-même à la 2ᵉ famine.
Conséquence : depuis ~2 h après le boot, **le `new_handler` est absent et le parachute
inopérant**. L'appareil tourne sans aucune protection contre un `bad_alloc`.

C'est la cause directe de la classe de crash observée : `ESPAsyncWebServer` fait des `new`
**lançants** à l'accept HTTP (`WebServer.cpp:50`) et par trame WebSocket
(`AsyncWebSocket.cpp:818/844/1333/1371`), sur la tâche `async_tcp` → `bad_alloc` non
rattrapé → `std::terminate` → `abort()`. Le dump du crash de 16:43 porte exactement
`abort() was called ... on core 1`, tâche `async_tcp`.

### 3. Le journal se détruit lui-même (PROUVÉ)

`kMaxLogFileBytes = 8192`, `kRotateKeepBytes = 6144`. Le WARN (~100 o × 2/min = 12 Ko/h)
plus la trace de calibrage donnent une **rétention réelle de 25 à 35 minutes**, dont ~90 %
la même ligne. Le post-mortem du crash de 16:43 a été détruit par notre propre
journalisation avant d'avoir pu être lu. (Usure flash : ~25 Ko/h sur 64 Ko ≈ 3·10³
effacements/bloc/an contre 10⁵ garantis → **non problématique**, seule la perte
forensique compte.)

### 4. L'origine de la fragmentation n'est PAS identifiée

Deux gisements relevés par lecture de code, **aucun démontré** : les ~300 `String`
persistantes du logger (`kMaxLogEntries = 200` + `_persistBuffer` de 100), et les
`new char[len+1]` par trame WebSocket. Le demandeur des allocations de **4096 o** n'est pas
identifié non plus. `oomFailedAllocHook` reçoit déjà le nom de l'appelant depuis l'IDF et
le **jette** (`(void)function_name;`).

## ⚠️ DANGER — la formule de recalibrage validée en 2.19.0 BRIQUERAIT l'appareil

Le plan de 2.19.1 prévoyait `kRestartMaxAllocHeapBytes = clamp(0,40 × min24h ; 8192 ; 16384)`,
validé par le gate chimique **en supposant un `min24h` de l'ordre de plusieurs dizaines de Ko**.

Avec le minimum réellement mesuré, `minMaxBloc8_boot = 3 060` :
`0,40 × 3 060 = 1 224` → **écrêté au plancher 8 192**. Or `8 192 > 3 060` : le seuil de
redémarrage serait **au-dessus du minimum observé en fonctionnement normal**. L'appareil
redémarrerait en boucle, puis basculerait en **MODE MAINTENANCE = DOSAGE SUSPENDU** au bout
de 3 redémarrages, en quelques minutes.

**La formule ne peut donc PAS être appliquée telle quelle.** L'invariant
`restart < rearm ≤ wsPush < warn < min_observé` impose que TOUTES les valeurs passent sous
3 060 — ce qui est si bas que la question se pose : faut-il calibrer des seuils sur une
fragmentation pathologique, ou traiter d'abord la fragmentation ? **Le gate `pool-chemistry`
doit trancher ce point AVANT tout codage.**

Réserve supplémentaire à porter au gate : le plan supposait **≥ 24 h de relevés**, nous n'en
avons que **5 h**. L'utilisateur a explicitement demandé le cycle complet malgré cette
réserve, qui est donc assumée et doit être consignée.

## Correctif attendu

1. **Filet d'urgence** — ré-inscrire `oomNewHandler` **inconditionnellement**, en tête de
   `oomGuardLoopTick()`, avant toute branche susceptible de sortir. Indépendant du
   recalibrage, doit pouvoir être livré même si le reste est différé.
2. **Basculer le réarmement** (et les autres gardes) sur `heapLargestFreeBlock8Bit()` /
   `heapFree8Bit()`. Ordre de bascule imposé par le risque croissant, **auto-guérison en
   DERNIER**.
3. **Recalibrer les seuils** sur les mesures réelles, sous contrainte de l'invariant
   ci-dessus et du verdict du gate chimique.
4. **Backoff du WARN d'échec d'allocation** (30 s → 5 min → 30 min) pour restaurer la
   fenêtre forensique du journal persistant.
5. **Journaliser `function_name` et `caps`** dans `oomFailedAllocHook` (stockage d'un
   `const char*`, drainé en `loop()` — **jamais de journalisation depuis le hook**, verrou
   de l'allocateur détenu). Seule voie vers l'identification du fragmenteur et des 4096.
6. **Ne PAS toucher** à `kMaxLogEntries` ni à la structure `LogEntry` tant que le point 5
   n'a pas désigné le coupable.

## Contraintes non négociables

- Aucune journalisation, aucune allocation, aucun `String` depuis `oomFailedAllocHook` ni
  depuis le `new_handler` (verrou du tas détenu → mesurer = deadlock). Déjà documenté
  INTERDIT dans `src/heap_metrics.h`.
- Le report d'un redémarrage pendant une injection reste **REFUSÉ** (PWM matériel : une
  boucle figée laisserait la pompe énergisée — différer transformerait une dose tronquée,
  bénigne, en surdosage).
- Débounce du redémarrage maintenu à 3.
- Le mode maintenance est un cul-de-sac connu (sortie seulement par crash ou reboot manuel,
  alerte fugace) : tout seuil qui le rend atteignable en régime normal est inacceptable.

## Critères d'acceptation

- [ ] Le parachute est réarmé et le `new_handler` présent en permanence, **vérifié on-target**
      — **NON VÉRIFIÉ** : l'appareil de production tourne toujours en 2.20.0. Implémenté
      (ré-inscription hissée en tête de `oomGuardLoopTick()`) et observable via le témoin
      `oom_handler_registered` de `/get-system-info`, qui doit valoir `true` en régime établi.
      **Ce critère ne peut être coché qu'après flash de la 2.20.2.**
- [~] Le WARN d'échec d'allocation ne sature plus le journal persistant (rétention > 12 h)
      — **14,6 à 19,5 h** calculées (14,1 Ko/h → 0,41 Ko/h, `kMaxLogFileBytes` inchangé),
      recalculées indépendamment par `test-engineer`. **Calcul, pas mesure on-target** : la
      marge réelle dépend du trafic événementiel (filtration, MQTT, dosage), non modélisé.
- [~] Le nom de l'appelant des allocations échouées apparaît dans le journal
      — `appelant=<function_name>` + `caps=0x…`, drainés depuis le tick. **Implémenté, non
      vérifié on-target** — c'est pourtant LA donnée attendue de ce cycle (identifier le
      fragmenteur et le demandeur des allocations de 4096 o).
- [x] Aucun seuil ne rend le mode maintenance atteignable en régime normal — **verdict
      `pool-chemistry` GO explicite sur ce point** : GO sur le bloc E1 (réarmement seul, sous
      invariant C2 vérifié), **NO GO** sur la bascule des 5 gardes restantes — aucune garde de
      sécurité n'a changé de grandeur comparée dans cette version
- [x] `pio run` bi-core verts, `pio test -e native` sans régression — **582/582** (+16),
      flash 83,8 % (esp32dev) / 93,6 % (esp32dev_core3)
- [x] **Archivage** : le `.elf` est archivé avec le `.bin` (l'ELF de 2.20.0 a été perdu,
      rendant le coredump du 16:43 inexploitable) — `tools/archive_build.py` en `post:` sur les
      deux envs, `builds/<VERSION>/<PIOENV>/`, rétention 5 par version **et** par env

## Suivi / itérations

### Itération 1 — v2.20.2 (2026-08-15)

**Statut** : livré — tests verts, revue OK (`code-reviewer` **Approved** après trois passes).

**Blocs livrés** : A (filet d'urgence), C (fenêtre forensique), D (identification du
fragmenteur), **E1 seulement** (réarmement 8 bits + paliers recalibrés), F (archivage).
**Bloc E différé** : bascule des 5 gardes restantes + recalibrage → NO GO du gate.

**Fichiers modifiés**

| Fichier | Nature |
|---|---|
| `src/oom_guard.cpp` | Ré-inscription en tête de tick, réarmement sur `heapLargestFreeBlock8Bit()`, drapeau `sSelfAllocInFlight`, backoff du WARN en `snprintf` pile, capture `function_name`/`caps`, miroir `sHandlerRegistered` |
| `src/oom_guard.h` | `oomGuardHandlerRegistered()` (lecture seule, documentée) |
| `src/oom_guard_logic.h` | `oomShouldRegisterHandler()`, `oomFailWarnNextDelayMs()`, `oomShouldCountFailure()` (logique pure) ; inclusion de `constants.h` |
| `src/constants.h` | `kOomFallbackRearmMinMaxAllocBytes` 12288→2048, `kOomFallbackReserveBytes` 2048→1024, 4 constantes de backoff (remplacent `kOomFailWarnThrottleMs`), `kHeap8LogIntervalMs`, échelle des seuils annotée **par métrique** |
| `src/main.cpp` | `checkHeapHealth()` — cadence 5 min → 30 min, partie `interne:` retirée |
| `src/web_routes_config.cpp` | Champ additif `oom_handler_registered` dans `/get-system-info` |
| `src/version.h` | 2.20.2 |
| `platformio.ini` | `extra_scripts = post:tools/archive_build.py` sur les deux envs matériels |
| `tools/archive_build.py` | **Nouveau** — archivage ELF+BIN par version et par env, rétention 5 |
| `test/test_native_oom_guard/test_oom_guard.cpp` | +16 tests |

**Décisions de conception à retenir**

1. **La ré-inscription du handler ne dépend PAS du miroir** `sHandlerRegistered`
   (`set_new_handler` est idempotent, rappelé à chaque tick tant qu'une réserve est armée).
   Gardée par le miroir, une course avec `oomNewHandler` sur `async_tcp` pouvait laisser
   « handler nul + miroir `true` » — état **définitif**, dont plus aucun chemin ne sort.
2. **Invariant d'ordre : miroir AVANT `set_new_handler`, sur les trois sites d'inscription.**
   Le seul état atteignable devient « miroir `false` / handler présent », auto-réparé au tick
   suivant. Défaut trouvé en **contre-revue** (l'ordre inverse produisait le mensonge durable).
3. **Condition C1 du gate** : jamais de handler sans réserve armée.
4. **Départ du backoff à 0 et non au palier initial** (revue) : sinon le palier « 30 s »
   documenté n'existait pas et l'échelle réelle était « immédiat → 5 min → 30 min ».
5. **Purge des archives triée par NOM et non par `mtime`** (revue) : `shutil.copy2` recopie la
   `mtime` de la source, un rebuild d'une révision ancienne passait pour l'archive la plus
   récente. L'horodatage en tête du nom rend l'ordre lexicographique chronologique.
6. **Cloisonnement des archives par environnement** (revue, bloquant) : 5 builds core 2
   auraient évincé l'ELF core 3 déployé — la panne forensique du 15/08 reconduite.
7. **Le post-mortem RTC n'est pas agrandi** pour `function_name` : zone de 32 o pleine, et
   l'étendre imposerait un bump de magic (perte d'une trace) pour une information déjà
   présente dans le journal.

**Limite assumée** : le drapeau `sSelfAllocInFlight` est **global**, pas par tâche — un échec
réel survenant sur l'autre cœur pendant la fenêtre (quelques µs toutes les 10 s) ne serait pas
comptabilisé. Compromis très favorable face aux ~1 025 échecs de bruit supprimés.

**Doc produite** : [ADR-0040](../../../docs/adr/0040-pas-de-calibrage-sur-fragmentation-pathologique.md)
(nouveau), notes additives datées sur [ADR-0035](../../../docs/adr/0035-parachute-oom-new-handler.md)
et [ADR-0038](../../../docs/adr/0038-metrique-memoire-allouable-8-bits.md), mises à jour de
`docs/subsystems/oom-guard.md`, `docs/subsystems/logger.md`,
`docs/subsystems/system-health.md`, `docs/API.md`, `docs/BUILD.md`, `CHANGELOG.md`.

### Reste à faire (hors périmètre de cette spec)

Repris intégralement dans `specs/features/todo/bug-bascule-gardes-8bits-recalibrage.md` :
bascule des 5 gardes restantes, re-dérivation de la règle de calibrage (celle d'ADR-0038 est
**inapplicable**), identification du fragmenteur, sort de `kOomRearmMinMaxAllocBytes`.
Condition d'entrée : **≥ 24 h de relevés propres sous 2.20.2** + fragmenteur identifié.
