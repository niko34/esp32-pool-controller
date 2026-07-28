# ADR-0033 — Budget d'espace garanti sur la partition history (garde d'admission + remove-avant-write + plafonds log réduits)

- **Statut** : Accepté
- **Date** : 2026-07-27
- **Décideurs** : Nicolas Philippe (architect), debugger (investigation NOSPC), embedded-developer
- **Spec(s) liée(s)** : bug-nospc-partition-history, investigation-corruption-recurrente-history

## Contexte

**2 « corruptions » de la partition `history` en 14 jours** (semaine du 15-22/07 en v2.9.2, nuit du 25-26/07 en v2.10.4), signature identique : `IntegerDivideByZero` dans `lfs_alloc` à la première écriture allocante post-montage. L'investigation (`specs/features/done/investigation-corruption-recurrente-history.md`) a établi que ce n'est **pas** une corruption aléatoire mais une **saturation structurelle** de la partition 64 Ko (16 blocs de 4096 o) :

| Consommateur (pire cas, avant correctif) | Blocs |
|---|---|
| Métadonnées littlefs (superblocs/racine) | 2 |
| Réécriture copy-on-write de `/history.json` (17-22 Ko, `open("w")` → ancien **et** nouveau coexistent jusqu'au close) | 10-12 |
| `/system.log` ≤ 16 Ko (`kMaxLogFileBytes`) | 4-5 |
| `/system.log.tmp` de rotation (12 Ko, `kRotateKeepBytes`) | 3 |
| **Total demandé** | **19-22 > 16** |

Le NOSPC est structurellement atteignable après ~5-7 jours d'accumulation. Or le div/0 dans `lfs_alloc` sur FS plein est un **bug littlefs amont répertorié** ([littlefs #1063](https://github.com/littlefs-project/littlefs/issues/1063), [esp_littlefs #183](https://github.com/joltwallet/esp_littlefs/issues/183)) — et la version embarquée est **figée** par la plateforme (`espressif32@6.9.0` / core Arduino 2.0.17) : impossible d'attendre un fix amont. Aggravants côté firmware : `saveToFile()` ouvrait `"w"` d'entrée (troncature de l'ancien fichier **avant** toute vérification), ne détectait pas l'écriture partielle, et son chemin d'échec en `ERROR` déclenchait un **flush disque immédiat** sur le FS déjà plein, toutes les 5 min (flush-storm).

Contrainte : on ne peut pas empêcher le bug amont de diviser par zéro ; on peut en revanche **garantir structurellement que le FS n'atteint jamais l'état plein** qui le déclenche.

## Décision

**Budget d'espace garanti** : le pire cas d'occupation simultanée de la partition ne peut plus dépasser **13 blocs sur 16** (marge 3 blocs). Trois mesures conjointes :

1. **`saveToFile()` restructuré** — mesure → garde → remove → écriture → vérification :
   - le JSON est construit et **mesuré** (`measureJson`) avant de toucher au FS ;
   - **garde d'admission pure** `historySaveAdmit(freeBytes, oldFileBytes, jsonBytes, marginBytes)` (module `history_logic`, [ADR-0017](0017-logique-metier-pure-humble-object-testabilite.md)) : admis si `libre + taille de l'ancien fichier ≥ JSON + marge` (`kHistorySaveSpaceMarginBytes = 8192` = 2 blocs : compaction near-full des métadonnées + arrondi bloc). Implémentation par soustractions gardées, sans addition (anti-overflow). Refus → save sauté, **l'ancien fichier reste intact**, retentative naturelle au cycle suivant (5 min) ;
   - si admis, **`remove()` de l'ancien fichier AVANT la réécriture** : ancien et nouveau ne coexistent plus, le pic copy-on-write est **divisé par 2** (12 → 6 blocs) ;
   - **détection d'écriture partielle** : le retour de `serializeJson` doit être exactement la taille mesurée, sinon le fichier partiel est supprimé (jamais de JSON tronqué silencieux sur le FS).
2. **Plafonds log réduits** (`logger.h`) : `kMaxLogFileBytes` 16 384 → **8192** (pire cas 2-3 blocs), `kRotateKeepBytes` 12 288 → **6144** (tmp de rotation 2 blocs).
3. **Anti-flush-storm + instrumentation** : tous les chemins d'échec de `saveToFile` loggent en **WARNING** (jamais `ERROR` — un ERROR flusherait immédiatement sur le FS plein), throttlé à 1/h (`kHistorySaveWarnThrottleMs`). Chaque refus/échec incrémente `nospc_cnt` et persiste `last_ok` (epoch du dernier save réussi) en NVS `hist_meta` — écriture NVS **uniquement sur échec** (usure). INFO d'occupation FS (`used/total`) throttlé 1/h sur save réussi.

Budget pire cas résultant : 2 (méta) + 6 (history sans COW ×2) + 3 (log ≤ 8 Ko + ε) + 2 (tmp 6 Ko) = **13 blocs ≤ 16**.

En complément (même spec, volet A1) : **préservation de preuve** dans le disjoncteur feature-059 avant `esp_partition_erase_range` — dump hex Serial des blocs 0-1 (`[HISTDUMP]`, 8 Ko) + CRC32 des 2 blocs et 16 premiers octets en NVS, pour discriminer l'hypothèse résiduelle H2 (corruption RAM `lfs_t` persistée) à une éventuelle prochaine occurrence. Voir [system-health.md](../subsystems/system-health.md).

## Alternatives considérées

- **Option A (rejetée) — write-tmp-puis-rename (écriture atomique)** : écrire `/history.json.tmp` puis `rename()`. Élimine la fenêtre de perte sur crash, mais le pic d'occupation devient **3×** (ancien + tmp + copies du rename) au lieu de 2× — c'est l'inverse de l'objectif, ça **aggrave** le déclencheur du bug amont. Rejetée.
- **Option B (rejetée) — attendre/patcher littlefs amont** : la version est figée par `espressif32@6.9.0` (lib précompilée du framework) ; un patch local de `libesp_littlefs.a` serait invérifiable et perdu à chaque upgrade de plateforme. Rejetée.
- **Option C (rejetée) — réduire la rétention de l'historique** (`kMaxHourlyDataPoints` 168 → 120) : perte de fonctionnalité visible utilisateur (7 j de données horaires) alors que la réduction du log + le remove-avant-write suffisent largement à tenir le budget. Non retenue.
- **Option D (retenue) — garde d'admission + remove-avant-write + plafonds log réduits** : budget garanti par construction, logique de décision pure testable en natif, aucune perte de fonctionnalité utilisateur.

## Conséquences

### Positives

- **Le FS ne peut structurellement plus être plein** au moment d'une allocation : pire cas 13/16 blocs, marge 3 blocs — le déclencheur du div/0 amont disparaît.
- **Jamais d'écriture partielle silencieuse** : refus AVANT troncature de l'ancien fichier (il reste lisible), fichier partiel supprimé après coup.
- **Fin du flush-storm** : plus aucune écriture disque déclenchée par l'échec d'une écriture disque.
- **Observabilité** : `nospc_cnt`/`last_ok` en NVS + WARN/INFO throttlés — un épisode de saturation résiduel serait visible et daté.
- **Décision testable** : `historySaveAdmit` pur, 10 tests natifs dédiés (frontières exactes, overflow `SIZE_MAX`) — 363 tests au total.

### Négatives / dette assumée

- **Fenêtre remove→write** : un crash/coupure entre le `remove()` et la fin de l'écriture perd **tout** l'historique persisté (le prochain boot ne trouve pas de fichier). Fenêtre de quelques centaines de ms toutes les 5 min ; coût borné à ce que le disjoncteur feature-059 ([ADR-0031](0031-disjoncteur-bootloop-history.md)) assume déjà (perte de l'historique contre disponibilité) et aux 5 min de données du cycle courant. Trade-off accepté contre l'option A qui aggravait la saturation.
- **Sur refus prolongé** (FS durablement plein malgré le budget — ne devrait pas arriver), l'historique sur flash **vieillit** silencieusement entre deux WARN throttlés 1 h (la RAM reste à jour).
- **Journal persisté réduit de moitié** (8 Ko ≈ dernières ~80-100 lignes) : profondeur de diagnostic post-mortem moindre dans `/system.log`.
- Le bug amont n'est **pas corrigé**, seulement privé de son déclencheur ; l'hypothèse H2 reste ouverte (discriminée par la preuve `[HISTDUMP]` le cas échéant).

### Ce que ça verrouille

- **Toute nouvelle écriture sur la partition `history` doit passer par la garde d'admission** (ou démontrer qu'elle respecte le budget 16 blocs pire cas) : ajouter un fichier ou grossir un existant sans recalculer le budget est interdit.
- `kMaxLogFileBytes` (8 Ko) et `kRotateKeepBytes` (6 Ko) ne doivent **pas être ré-augmentés** sans refaire le calcul du budget (ils en font partie).
- Les chemins d'échec de `saveToFile` restent en **WARNING** (jamais ERROR/CRITICAL) : le contrat « échec d'écriture disque ⇒ aucune écriture disque induite » ne doit pas être cassé.
- La décision d'admission reste **pure** (`history_logic`, [ADR-0017](0017-logique-metier-pure-humble-object-testabilite.md)) : toute évolution de la marge/règle passe par ce module et ses tests natifs.
- Les clés NVS `nospc_cnt`/`last_ok` (namespace `hist_meta`) ne sont écrites **que sur échec** : ne pas les transformer en télémétrie à chaque save (usure NVS).

## Références

- Code : [`src/history.cpp`](../../src/history.cpp) (`saveToFile`, `recordHistorySaveFailureNvs`, `dumpHistoryPartitionEvidence`), [`src/history_logic.h`](../../src/history_logic.h) / [`.cpp`](../../src/history_logic.cpp) (`historySaveAdmit`), [`src/logger.h`](../../src/logger.h) (`kMaxLogFileBytes`, `kRotateKeepBytes`), [`src/constants.h`](../../src/constants.h) (`kHistorySaveSpaceMarginBytes`, `kHistorySaveWarnThrottleMs`, clés NVS `nospc_cnt`/`last_ok`/`dmp_*`)
- Tests : `test/test_native_history/` — 10 tests `historySaveAdmit` (363 au total)
- Specs : `specs/features/doing/bug-nospc-partition-history.md`, `specs/features/done/investigation-corruption-recurrente-history.md` (bilan d'espace chiffré, hypothèses H1-H4)
- Bug amont : [littlefs #1063](https://github.com/littlefs-project/littlefs/issues/1063), [esp_littlefs #183](https://github.com/joltwallet/esp_littlefs/issues/183) — version figée par `espressif32@6.9.0`
- Docs : [subsystems/history.md](../subsystems/history.md), [subsystems/logger.md](../subsystems/logger.md), [subsystems/system-health.md](../subsystems/system-health.md)
- Antécédents : [ADR-0009](0009-partition-coredump.md) (partition history 64 Ko), [ADR-0031](0031-disjoncteur-bootloop-history.md) (disjoncteur de boot-loop — confinement, ce ADR traite la cause racine)
