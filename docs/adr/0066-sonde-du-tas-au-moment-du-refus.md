# ADR-0066 — Le tas est mesuré au moment exact du refus, sur les caps de l'échec ET sur une référence 8 bits

- **Statut** : Accepté
- **Date** : 2026-09-05
- **Décideurs** : `architect`, gate `pool-chemistry` (GO sous 6 conditions), `code-reviewer` (point B5), Nicolas
- **Spec(s) liée(s)** : `bug-080-redemarrage-panic-et-fragmentation-heap` (lot B-2)

## Contexte

Depuis la v2.18.8, le post-mortem d'allocation en RAM RTC rend un **verdict** au boot suivant :
`FAMINE MÉMOIRE`, `SUSPICION CORRUPTION` ou `INDÉTERMINÉ`. Ce verdict confrontait la taille
refusée à un **échantillon de l'anneau heap**, qui a deux défauts :

1. il est **périmé de 0 à 2 s** (cadence `kHeapRingSampleIntervalMs`) ;
2. il est mesuré sur des **capacités FIXES** `MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT`.

Les relevés du 05/09/2026 (v2.39.0) ont rendu ces défauts décisifs. **Onze échecs
d'allocation en 1 h 12**, sans PANIC, tous sur **1 622 octets** demandés en
**`caps=0x080C`** (`8BIT|DMA|INTERNAL`). La contrainte **DMA est strictement plus
restrictive** que `INTERNAL|8BIT` : au même instant, le tas 8 bits annonçait un plus gros
bloc de **26 612 o**, soit 16× la taille refusée. La règle historique concluait donc
`SUSPICION CORRUPTION` — un **CRITICAL faux**, sur un chemin où la mémoire n'était ni
corrompue ni globalement épuisée : c'est le **pool DMA** qui était à sec.

Un second obstacle bloquait l'instrumentation qui aurait tranché. `heap_metrics.h` et
`docs/subsystems/oom-guard.md` interdisaient tout `heap_caps_*` depuis le chemin d'échec en
invoquant un **deadlock** par réentrance : « le callback s'exécute alors que l'allocateur
détient le verrou du tas ». Cette justification a **empêché la mesure décisive pendant
trois cycles de diagnostic**. Le désassemblage de `libheap.a` (revue B-1) la réfute :

- `heap_caps.c.obj` ne contient **aucune relocation** vers `multi_heap_internal_lock` /
  `_unlock` — le verrou vit entièrement dans `multi_heap.c.obj` ;
- `heap_caps_alloc_failed` (d'où part le callback) est **statique en `.iram1.0`** et n'est
  atteinte qu'**après le retour complet** de `heap_caps_malloc_base`.

Aucun verrou de tas n'est détenu quand le callback s'exécute. L'interdit était **juste dans
sa conclusion, faux dans sa justification** — et cette erreur de qualification, invisible
dans le code, se transmettait à chaque contributeur.

## Décision

**Trois décisions liées, indissociables.**

### 1. L'interdit `heap_caps_*` depuis le chemin d'échec est requalifié : coût interruptions coupées, borné, pas deadlock

L'interdit **demeure**, mais sous sa vraie nature : un parcours TLSF (`largest_free_block`)
s'exécute **interruptions coupées**, le chemin d'échec peut être déclenché **par rafales**
et il est **appelable depuis une ISR**. Mesurer sans borne y transformerait un diagnostic en
source de gigue pour la régulation et le watchdog.

Il est assorti d'une **exception unique et bornée par quatre garde-fous cumulés** :

| Garde-fou | Valeur | Ce qu'il empêche |
|---|---|---|
| Jeton « un échantillon par **épisode** » | `exchange` atomique, réarmé par la loop au drain du WARN d'échec | Deux cœurs qui sondent en même temps ; une rafale qui sonde à chaque refus |
| **Plancher temporel absolu** | `kOomProbeMinIntervalMs` = **1 000 ms** | Le drain **immédiat** du backoff (qui démarre à 0 ms) réarmant le jeton quelques ms après la sonde précédente |
| **Jamais depuis une ISR** | `xPortInIsrContext()` | Un parcours de tas dans un contexte d'interruption |
| **Désactivation définitive** | durée du couple > `kOomProbeMaxDurationUs` = **1 500 µs** | Qu'une plateforme lente paie indéfiniment le coût |

Plus un **WARN** de conservation à `kOomProbeWarnDurationUs` = **300 µs**, et une borne
**a priori** (condition 2 du gate, ci-dessous) parce que l'autolimitation est **post-hoc par
construction** : le dépassement qui la déclenche a **déjà eu lieu**.

Le seul point d'entrée autorisé est **`heapProbeForCaps()`**. `heapLargestFreeBlock8Bit()`,
`heapFree8Bit()` et `heapMetricsSnapshot()` restent **interdites** dans ces chemins : elles
ne sont bornées par rien.

**Formulation retenue, honnête** : « sans verrou pris **par nous** » — car
`multi_heap_get_info_impl` prend bien un spinlock, en interne et brièvement. Écrire « sans
verrou » tout court reproduirait le défaut que cet ADR corrige : une affirmation qui se
transmet mieux qu'elle ne se vérifie.

### 2. La sonde fait une DOUBLE mesure : caps de l'échec + référence `INTERNAL|8BIT`

Un seul parcours par mesure (`heap_caps_get_info()` livre `total_free_bytes` **et**
`largest_free_block`), deux mesures par sonde :

- **caps de l'échec** (`0x080C` dans le cas réel) → répond à la question effectivement
  posée à l'allocateur ;
- **référence `MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT`** → point de comparaison, qui rend le
  verdict **attribuable**.

Le couple donne, sur le cas du 05/09 : `bloc(0x080C) ≈ 1 Ko` **et** `réf8 = 26 612 o` →
verdict **`FAMINE DMA (pool restreint)`**, journalisé en **WARN** (ce n'est pas une
corruption).

**Borne a priori du pire cas PSRAM** (condition 2 du gate) : le parcours O(blocs) n'est pris
que si le masque **garantit** la RAM interne (`caps & MALLOC_CAP_INTERNAL`). Sinon, la sonde
**change d'algorithme** — agrégats O(régions) — et le champ « plus gros bloc » est marqué
`kOomProbeBlockUnknown`. La sonde n'est **jamais sautée** : les 12 sites d'appel du hook
doivent tous rendre un verdict.

> ⚠️ **Le test porte sur `MALLOC_CAP_INTERNAL`, pas sur la présence de `MALLOC_CAP_SPIRAM`.**
> La première écriture du lot testait la présence du bit SPIRAM — défaut **bloquant** relevé
> en revue. Tout échec de `malloc()` / `operator new` ordinaire remonte
> `caps = MALLOC_CAP_DEFAULT` (`0x1000`), **sans** bit SPIRAM (désassemblage :
> `heap_caps.c.obj`, `.iram1.2`, `.literal+0x8 = 0x00001000`) — c'est la classe d'échec des
> crashs du 11/08 et du 13/08, celle qui fonde tout ce chantier. Or le tas PSRAM est
> enregistré avec `DEFAULT|SPIRAM|8BIT|32BIT` : `heap_caps_match()` l'aurait sélectionné et
> `tlsf_walk_pool` aurait parcouru 4 Mo interruptions coupées. La sonde se serait
> **désactivée dès le premier `new` refusé**, morte sur son cas d'usage principal.

Le format de la zone RTC est **verrouillé** par cette décision : **48 octets, 12 mots**,
magic **`0xF00DFA13`** (voir [oom-guard.md](../subsystems/oom-guard.md#format-de-la-zone--48-octets-magic-0xf00dfa13-bumpé-en-bug-080-b-2)).

### 3. `SUSPICION CORRUPTION` n'est plus atteignable sans sonde contemporaine

La règle du verdict est révisée :

1. `failCount == 0 || lastSize == 0` → `INDÉTERMINÉ` *(inchangé)* ;
2. **sonde représentative** (prise **et** `probeSize == lastSize` **et**
   `probeCaps == lastCaps`) → elle **fait autorité**, l'anneau est **ignoré** ;
3. sonde absente / non représentative → règle historique **amputée de sa conclusion** : son
   dernier cas rend désormais `INDÉTERMINÉ` **au lieu de** `SUSPICION CORRUPTION`.

Corollaire de niveau de log : `FAMINE DMA` est un **WARN**, et le `CRITICAL` du post-mortem
n'est plus émis que sur une corruption fondée sur une mesure **contemporaine** (ou sur un
échec du contrôle d'intégrité du tas).

## Alternatives considérées

- **Option A — garder l'anneau seul** (rejetée) : c'est l'état d'avant. Il produit
  littéralement le faux `CRITICAL SUSPICION CORRUPTION` du 05/09, et aucune évolution de
  seuil ne le corrige : le problème n'est pas le niveau, c'est que la grandeur mesurée ne
  répond pas à la question posée.
- **Option B — sonder sur des caps FIXES `INTERNAL|8BIT`** (rejetée, 8 o de RTC) :
  contemporain, donc mieux ; mais sur le cas réel, `maxBloc = 26 612` face à 1 622 o refusés
  → **reproduit exactement le faux verdict** que ce lot doit supprimer. Une mesure fraîche
  d'une mauvaise grandeur ne vaut pas mieux qu'une mesure périmée.
- **Option C — sonder sur les caps de l'échec seules** (rejetée, 8 o de RTC) : rend bien
  « famine », mais **indiscernable d'une famine générale** — aucun point de comparaison,
  donc aucune attribution. Le diagnostic aurait relancé un cycle de mesures.
- **Option D — RETENUE : les deux** (16 o de RTC) : `bloc(caps de l'échec)` **et**
  `réf8` → `FAMINE DMA (pool restreint)` distinguée de `FAMINE MÉMOIRE`. Coût : 16 octets de
  RAM RTC noinit sur 8 192.
- **Option E — lever l'interdit `heap_caps_*` sans bornage** (rejetée) : le désassemblage
  autorise techniquement l'appel, mais le coût interruptions coupées est réel et le chemin
  est appelable en ISR et en rafale. Un diagnostic ne doit pas devenir une source de gigue
  pour la régulation.
- **Option F — sauter la sonde quand les caps peuvent atteindre la PSRAM** (rejetée par le
  gate, condition 2) : sauter est interdit — les 12 sites d'appel doivent rendre un verdict.
  On change d'**algorithme**, on ne renonce pas à mesurer.
- **Option G — ne pas bumper le magic** (rejetée) : sans bump, `w8..w11` seraient lus dans
  une zone de 32 o écrite par l'ancien firmware, c'est-à-dire dans de la RAM RTC non
  initialisée : des **valeurs de sonde inventées et crédibles**. Perdre le post-mortem du
  boot de bascule est acceptable ; afficher une valeur trompeuse ne l'est pas (même
  raisonnement qu'au bump de la v2.19.0).
- **Option H — `esp_timer_get_time()` comme horloge de la sonde** (rejetée) : elle prend un
  `portMUX` dans certaines implémentations ESP32 — **rédhibitoire** dans un chemin appelable
  en ISR. **CCOUNT** (`esp_cpu_get_cycle_count()`) est **une instruction** (`rsr.ccount`),
  **aucun verrou**. Son repliement à 17,9 s à 240 MHz est sans effet (soustraction
  `uint32_t`, sonde ≤ 1,5 ms). Le compteur étant **par cœur**, `xPortGetCoreID()` est lu
  avant et après : si la tâche a migré, la durée est marquée invalide (`0xFFFF`) et
  **l'autolimitation n'est pas appliquée sur cet échantillon** — une migration ne doit
  jamais désactiver la sonde à vie.
- **Option I — journaliser depuis le hook** (rejetée) : le chemin d'échec n'a le droit ni
  de logger ni d'allouer. Les trois messages d'observabilité (1ᵉʳ échantillon en INFO, WARN
  « lente », WARN one-shot de désactivation) sont drainés **depuis la loop**.

## Conséquences

### Positives

- Le verdict post-mortem repose enfin sur une mesure **contemporaine** et sur la **grandeur
  effectivement refusée** ; un `CRITICAL` faux disparaît d'un chemin où il mentait.
- Une famine de **pool restreint** (DMA) est nommée comme telle — classe d'échec qui
  représentait **11 échecs sur 11** dans le relevé du 05/09.
- L'indétermination devient **attribuable** : `sonde=oui / absente / ISR / désactivée /
  autre refus`.
- L'instrumentation bloquée par une justification fausse est **débloquée**, et l'interdit
  qui la bloquait est corrigé **au bon endroit** (sa justification), pas supprimé.
- Le protocole de vérification est **observable** en vol : quatre champs additifs sur
  [`/get-system-info`](../API.md#oom_probe_--observabilité-de-la-sonde-du-refus-v2400-additifs).

### Négatives / dette assumée

- **Le post-mortem du boot de bascule est perdu** (bump de magic `0xF00DFA12` →
  `0xF00DFA13`). Accepté.
- **L'octet `flags` de la zone RTC est SATURÉ (8/8 bits)** après ce lot. Tout drapeau futur
  exigera un **mot supplémentaire** — ce n'est plus une extension gratuite.
- **`probeCaps` est tronqué à 16 bits.** L'IDF 5.5 déclare déjà des bits ≥ 16 (`TCM` 1<<16,
  `DMA_DESC_AHB` 1<<17, `CACHE_ALIGNED` 1<<19, `SIMD` 1<<20). Si l'un d'eux apparaissait
  dans un masque, `oomPostMortemProbeRepresentative()` resterait cohérente mais **moins
  discriminante**. Un champ 32 bits exigerait un 13ᵉ mot.
- **L'autolimitation reste post-hoc** : le dépassement qui désactive la sonde a déjà eu
  lieu. C'est précisément pourquoi la borne a priori (option F) est exigée **en plus**.
- **Pas d'`esp_task_wdt_reset()`** après la section critique — divergence assumée avec le
  modèle du contrôle d'intégrité (qui vise 5-20 ms dans la loop) : ici 1,5 ms plafonné, et
  prendre le verrou TWDT depuis un chemin appelable en contexte non-loop serait un risque
  net pour un bénéfice nul. **Validé par le gate**, commenté en clair dans le code.
- **Coût flash** : 1 821 797 → **1 824 545 o** (+2 748), marge **10 463 o** (99,4 %
  occupés). Le budget flash devient le facteur limitant du prochain lot.

### Ce que ça verrouille

- **Le format de la zone RTC post-mortem** : 48 o / 12 mots / magic `0xF00DFA13`. Tout
  champ supplémentaire exige un nouveau bump (donc la perte d'une trace) et une taille
  multiple de 4 (stores bruts alignés).
- **Le point d'entrée unique** depuis les chemins d'échec : `heapProbeForCaps()`, via
  l'entonnoir borné d'`oom_guard.cpp`. Aucun autre `heap_caps_*` n'y est autorisé.
- **Les quatre garde-fous** de l'exception : les retirer, même un seul, rouvre l'interdit
  dans sa forme absolue.
- **La politique de verdict** : plus aucun chemin ne conclut `SUSPICION CORRUPTION` sans
  mesure contemporaine représentative.
- **Le prédicat de sélection de branche** doit rester **fail-safe** (test sur
  `MALLOC_CAP_INTERNAL`, jamais sur `MALLOC_CAP_SPIRAM`), verrouillé par un `static_assert`
  qui casse la **compilation du firmware** si le bit IDF change.

## Sans impact

- **[ADR-0058](0058-gardes-heap-metrique-8-bits.md)** : aucun seuil de l'échelle 8 bits
  n'est touché (condition C5 du gate B-1 reconduite et respectée). La bascule de la garde
  503 de `/debug/sensor_trace` sur `heapLargestFreeBlock8Bit()` **applique** ADR-0058 au
  dernier site resté hors périmètre — elle ne le modifie pas.
- **[ADR-0061](0061-bascule-psram-au-dela-de-4095-octets-reserve-parachute-en-interne.md)**
  (lot B-1) : inchangé. Ce lot **mesure** les conséquences de la bascule, il ne la révise
  pas.

## Références

- Code : `src/heap_metrics.h` / `src/heap_metrics.cpp` (`heapProbeForCaps`,
  `heapCaps8BitMask`, `static_assert` sur `MALLOC_CAP_INTERNAL`),
  `src/oom_guard_logic.h` (`oomProbeUsesRegionScan`, `oomProbeFloorElapsed`,
  `oomProbeSaturateDurationUs`, `oomPostMortemProbeRepresentative`,
  `oomPostMortemVerdict`, `kOomVerdictDmaStarvation`, layout 48 o),
  `src/oom_guard.cpp` (entonnoir de la sonde, dump 2 lignes, drains d'observabilité),
  `src/constants.h` (`kOomProbe*`), `src/web_routes_config.cpp` (champs `oom_probe_*`),
  `src/web_routes_debug.cpp` (garde 503)
- Tests : `test/test_native_oom_guard/`
- Spec : `specs/features/doing/bug-080-redemarrage-panic-et-fragmentation-heap.md` (lot B-2,
  et section « VERDICT DE LA PRÉDICTION FALSIFIABLE DE B-1 »)
- Doc : [`docs/subsystems/oom-guard.md`](../subsystems/oom-guard.md),
  [`docs/API.md`](../API.md#get-get-system-info--write)
