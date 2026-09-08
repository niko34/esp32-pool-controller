# ADR-0077 — Une métrique de garde doit exclure les pools qu'un `malloc()` ne peut pas atteindre

- **Statut** : Accepté
- **Date** : 2026-09-08
- **Décideurs** : utilisateur, `architect`, `code-reviewer`, `doc-writer`
- **Spec(s) liée(s)** : `feature-122` (lot C-1a — instrumentation), `bug-080` lot C (cause racine), `feature-123` (lot C-1b — la bascule, conséquence de cet ADR)

## Contexte

Ce dépôt a livré **trois fois** le même mode de défaillance : une garde mémoire
qui mesure un pool où l'allocation qu'elle prétend protéger **ne peut pas
puiser**. La garde ne se déclenche pas, elle n'émet aucune alerte, et rien ne la
contredit — parce que le chiffre qu'elle lit est **faux et parfaitement
crédible**.

1. **Le parachute anti-OOM logé en PSRAM** (feature-066, corrigé en v2.35.0 —
   [ADR-0061](0061-bascule-psram-au-dela-de-4095-octets-reserve-parachute-en-interne.md)).
   Une réserve d'urgence en RAM externe ne sauve **rien** d'une famine de RAM
   interne : le parachute était **décoratif**. Prouvé le 05/09/2026 par
   `+12 324 o` de `psram_free` au déploiement.
2. **Quatre gardes rendues mortes par une métrique IRAM-inclusive**
   ([ADR-0058](0058-gardes-heap-metrique-8-bits.md), bug-080 lot A).
   `ESP.getMaxAllocHeap()` mesure `MALLOC_CAP_INTERNAL`, IRAM 32 bits comprise —
   un plancher infranchissable à ~27 636 o. Le PANIC du 29/08 et la famine de
   9 min du 02/09 (1 466 échecs) sont passés **sans une seule alerte**.
3. **La réserve DMA, visible par `0x0804`** (bug-080 lot C, ce présent ADR). Les
   allocations qui échouent en production portent
   `caps = 0x080C = INTERNAL|DMA|8BIT`. Le correctif candidat est
   `esp_psram_extram_reserve_dma_pool()`, qui **ré-enregistre une région** avec
   ses propres capacités. Or la métrique **actuelle** de toutes les gardes du
   projet est `0x0804 = INTERNAL|8BIT` — et par la règle de l'IDF
   `(all_caps & caps) == caps`, elle **voit** cette réserve.

Le troisième cas se lit dans la table dérivée, corroborée par deux voies
indépendantes : la règle de sélection (documentée dans
[`src/oom_guard_logic.h`](../../src/oom_guard_logic.h) et mise sous test dans
`test/test_native_heap_caps_table/`) **et** la source IDF
(`caps[] = {0, DMA|INTERNAL, 8BIT|32BIT}` → `all_caps = 0x080E`).

| Masque | DRAM interne | PSRAM | Réserve DMA |
|---|---|---|---|
| `0x0804` `INTERNAL\|8BIT` — métrique **actuelle** des gardes | voit | ignore | **voit** ⚠️ |
| `0x080C` `INTERNAL\|DMA\|8BIT` — ce que le WiFi demande | voit | ignore | voit |
| `0x1000` `DEFAULT` seul | voit | **voit** ⚠️ | ignore |
| `0x1804` `INTERNAL\|8BIT\|DEFAULT` | voit | ignore | ignore ✅ |

⇒ Poser la réserve **sans** basculer la métrique ferait afficher aux gardes un
gros bloc libre — celui de la réserve — pendant que le pool général resterait
affamé. La garde 503 des fichiers statiques **cesserait de se déclencher**, et
l'appareil planterait exactement là où elle protège aujourd'hui : une coupure
d'interface de 50 s deviendrait un plantage.

Il faut trancher **maintenant**, avant de poser quoi que ce soit : la réserve est
**irréversible jusqu'au redémarrage** (aucune API ne retire une région
enregistrée) et l'OTA est lui-même gardé par des seuils mémoire — une réserve mal
calibrée peut fermer la porte par laquelle on viendrait la corriger.

## Décision

**La métrique d'une garde mémoire mesure exactement le pool où l'allocation
qu'elle protège puise réellement — jamais un surensemble.** Tout pool
inatteignable par le `malloc()`/`new` concerné (IRAM 32 bits, PSRAM, région
réservée) est **exclu du masque**, et non « pris en compte » ni « corrigé après
coup ».

Corollaires normatifs :

1. **La table ci-dessus est figée** par cet ADR et **épinglée en compilation** :
   `static_assert(heapFloorsPredMask() == 0x02ED)`
   ([`src/heap_metrics.cpp`](../../src/heap_metrics.cpp)). Remplacer `== caps`
   par `!= 0`, ou `0x1804` par `0x0804`, casse le **build** — pas seulement un
   test.
2. **Poser une région réservée et basculer la métrique appartiennent au même
   lot.** L'un sans l'autre est une régression de sûreté, pas une amélioration
   partielle.
3. **Une métrique de garde ne se choisit pas sur un indicateur d'agrégat.**
   `psram_free`, `ESP.getFreeHeap()`, `ESP.getMaxAllocHeap()` sont des indices ;
   la preuve est le masque de capacités de l'allocation qui échoue
   (`oom_probe_last_caps`, [ADR-0066](0066-sonde-du-tas-au-moment-du-refus.md)).
4. **Cet ADR ne bascule aucune garde.** feature-122 (v2.49.0) livre un
   **instrument** : cinq planchers glissants, l'octet de contexte au creux, le
   témoin 4096/4095, la durée des parcours. Les gardes restent sur `0x0804`. La
   bascule vers `0x1804` est la **conséquence** de cet ADR et sera exécutée en
   C-1b / C-3 (`feature-123`), sur des planchers **relevés sur cible**.

## Alternatives considérées

- **Garder `0x0804` comme métrique de garde après la pose de la réserve**
  (rejetée) — c'est précisément le piège : le masque **voit** la réserve, donc la
  garde 503 lirait un gros bloc qu'aucune allocation ordinaire ne peut obtenir.
  Troisième récidive du mode de défaillance décrit en contexte.
- **`MALLOC_CAP_DEFAULT` (`0x1000`) seul** (rejetée) — piste initialement
  écrite dans `bug-080` lot C §4. Ce masque ignore bien la réserve, mais il
  **voit la PSRAM** : 4 Mo de faux confort adossés à une mémoire que le DMA ne
  sait pas adresser. On échangerait une aveuglement contre un autre, en pire
  (l'écart n'est plus de 27 Ko mais de 4 Mo).
- **Corriger après coup** : mesurer `0x0804` puis soustraire la taille de la
  réserve (rejetée) — la soustraction porterait sur le **libre**, jamais sur le
  **plus gros bloc contigu**, qui est la grandeur dont dépendent les gardes. Un
  correctif arithmétique sur la mauvaise grandeur.
- **`0x1804` `INTERNAL|8BIT|DEFAULT`** (retenue comme cible) — ignore la PSRAM
  (absence d'`INTERNAL` dans `all_caps` du tas externe) **et** la réserve
  (absence de `DEFAULT` dans `all_caps` de la région ré-enregistrée). Ne garde
  que le pool où `malloc()`/`new` puisent réellement.
- **Ne rien décider avant les relevés sur cible** (rejetée pour l'ADR, retenue
  pour la bascule) — le principe ne dépend d'aucune mesure : il est vrai
  indépendamment des chiffres. Ce sont le **dimensionnement** et le **moment**
  de la bascule qui en dépendent.

## Conséquences

### Positives

- Le **motif** est nommé une fois pour trois occurrences : la prochaine
  proposition de garde mémoire se relit contre cette règle avant d'être écrite.
- La table de sélection devient un **invariant de compilation**, pas une note de
  spec — le seul chemin pour la changer est explicite et bruyant.
- `feature-123` hérite d'un cadre : la réserve **et** la métrique dans le même
  lot, sur des planchers relevés, avec le verdict A7 tranché.

### Négatives / dette assumée

- La table `0x080E` de la réserve reste une **hypothèse** (source IDF +
  désassemblage), pas une mesure. Elle ne sera confirmée qu'en `feature-123`,
  après pose réelle de la réserve.
- Les gardes tournent encore sur `0x0804` en v2.49.0 : le piège est **documenté
  et mesuré**, il n'est pas encore désarmé. C'est un choix de séquence (mesurer
  avant de corriger l'irréversible), pas un oubli.
- Une métrique par pool exact multiplie les masques à suivre — coût payé en
  instrumentation (2 parcours O(blocs) par échantillon, cf. la section
  [Planchers multi-masques](../subsystems/system-health.md#planchers-glissants-multi-masques-v2490-feature-122-lot-c-1a)).

### Ce que ça verrouille

- **Aucune garde de ce projet ne peut plus être calée sur un masque
  surensemble** du pool qu'elle protège — ni IRAM-inclusive
  ([ADR-0058](0058-gardes-heap-metrique-8-bits.md)), ni PSRAM-inclusive, ni
  réserve-inclusive.
- **Toute pose de région réservée** (`heap_caps_add_region_with_caps`,
  `esp_psram_extram_reserve_dma_pool`) impose de **recalculer la table** et de
  basculer les gardes dans le même lot.
- **`0x1000` seul est proscrit** comme métrique de garde sur cette plateforme,
  tant que la PSRAM est montée dans l'allocateur.

## Références

- Code : [`src/heap_caps_logic.h`](../../src/heap_caps_logic.h) (module pur —
  masques, `heapMaskSelectsHeap`, planchers, `fetch_min`),
  [`src/heap_metrics.cpp`](../../src/heap_metrics.cpp)
  (`heapFloorsPredMask()` + `static_assert(… == 0x02ED)`),
  [`src/oom_guard_logic.h`](../../src/oom_guard_logic.h) (règle
  `(all_caps & caps) == caps`)
- Tests : `test/test_native_heap_caps_table/` (les 12 cas de la table,
  le mutant `!= 0` pris de face), `test/test_native_heap_floors/`
- Spec : `specs/features/doing/feature-122-instrumentation-reserve-dma-et-metrique-du-pool-general.md`,
  `specs/features/todo/feature-123-experience-de-reserve-dma-au-demarrage.md`,
  `specs/features/doing/bug-080-redemarrage-panic-et-fragmentation-heap.md` (lot C)
- Doc : [`docs/subsystems/system-health.md`](../subsystems/system-health.md#planchers-glissants-multi-masques-v2490-feature-122-lot-c-1a),
  [`docs/API.md`](../API.md#paramètre-hf--planchers-mémoire-multi-masques-v2490-additif-conditionnel),
  [ADR-0058](0058-gardes-heap-metrique-8-bits.md),
  [ADR-0061](0061-bascule-psram-au-dela-de-4095-octets-reserve-parachute-en-interne.md),
  [ADR-0066](0066-sonde-du-tas-au-moment-du-refus.md),
  [ADR-0072](0072-absence-de-valeur-hors-du-domaine.md)
