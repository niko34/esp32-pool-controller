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

- [ ] Le parachute est réarmé et le `new_handler` présent en permanence, vérifié on-target
- [ ] Le WARN d'échec d'allocation ne sature plus le journal persistant (rétention > 12 h)
- [ ] Le nom de l'appelant des allocations échouées apparaît dans le journal
- [ ] Aucun seuil ne rend le mode maintenance atteignable en régime normal — **verdict
      `pool-chemistry` GO explicite sur ce point**
- [ ] `pio run` bi-core verts, `pio test -e native` sans régression
- [ ] **Archivage** : le `.elf` est archivé avec le `.bin` (l'ELF de 2.20.0 a été perdu,
      rendant le coredump du 16:43 inexploitable)
