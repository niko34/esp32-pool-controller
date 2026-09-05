# Investigation — Corruption récurrente de la partition littlefs `history`

- **Type** : investigation (pas de correctif défini — l'issue déterminera la suite)
- **Statut** : done
- **Version cible** : à déterminer
- **Créée** : 2026-07-27
- **Domaine** : persistance (history/logger/littlefs) — aucun chemin de dosage
- **Specs liées** : feature-059 (disjoncteur, confinement opérationnel), bug-spam-sensor-problem-serial (hypothèse martèlement réfutée)

## Faits

1. **2 corruptions en 14 jours** de la partition `history` (64 Ko, 16 blocs de 4 Ko), signature identique : superblock/métadonnées corrompus → `IntegerDivideByZero` dans `lfs_alloc` (lfs.c:689) à la première écriture post-montage (le `.fscheck` inline passe).
   - #1 : semaine du 15-22/07 (v2.9.2, période de gels lwIP + panics multiples), récupération manuelle USB.
   - #2 : nuit du 25-26/07 vers 00:24 (v2.10.4), auto-réparée par le disjoncteur feature-059 (~1 min, 2 panics).
2. **Contexte #2** : la corruption est apparue autour de minuit — heure de la consolidation quotidienne de l'historique (agrégation daily + réécriture de `/history.json`).
3. Hypothèse « martèlement /system.log par spam de logs » : **réfutée** (investigation du 22/07 — plafond structurel des écritures).
4. littlefs est conçu power-fail safe (copy-on-write) : une corruption de superblock ne devrait arriver ni sur coupure ni sur panic.

## Pistes à investiguer

1. **Séquence de minuit** : ordre exact des écritures du rollover quotidien (consolidation, `saveToFile`, taille de `/history.json`, espace libre requis vs 64 Ko, comportement littlefs quand le FS est PLEIN pendant une réécriture).
2. **Accès concurrents au FS history** : inventaire des écrivains (logger `flushToDisk` + rotation, history `saveToFile`) et lecteurs (`/download-logs` chunked depuis async_tcp — NOUVEAU en 2.10.4, antérieur à la corruption #2 de 2 jours, `/get-logs`… ) ; le verrou VFS d'esp_littlefs (`efs->lock`) couvre-t-il tout ? Partage de `File` handles entre tâches ?
3. **Version littlefs embarquée** (core Arduino 2.0.x / esp_littlefs) : bugs de corruption connus et corrigés en amont (petites partitions, lookahead, ctz), applicabilité à notre profil (16 blocs).
4. **Événement déclencheur de #2** : que s'est-il passé juste avant (reboot ? panic pendant écriture ?) — traces MQTT/HA de la nuit du 25-26 (alertes auto_reboot / network_stall_reboot) à demander à l'utilisateur ; le journal d'avant-corruption est perdu (partition effacée).

## Investigation — analyse statique (2026-07-27, agent debugger)

### 1. Séquence de « minuit » : il n'y a PAS de job de minuit sur la partition history

- `history.cpp` : `update()` (history.cpp:268-297) enregistre un point toutes les 5 min et lance `consolidateData()` + `saveToFile()` toutes les 5 min, **uniformément** — aucune logique spécifique au changement de jour. Les buckets daily (`bucketTimestamp(..., 86400)`) basculent à minuit UTC (02:00 locales en juillet), pas à 00:24.
- Ce qui se passe réellement à minuit local : reset des compteurs journaliers (`pump_controller.cpp:615-625`, `saveDailyCounters()` → NVS/partition principale, pas history) + logs INFO (pas de flush immédiat). **La corrélation « minuit » de #2 pointe donc plutôt vers un reboot ~00:2x** (net_health / auto-heal) suivi du crash à la 1ʳᵉ écriture allocante.
- **Pourquoi `.fscheck` passe puis la 1ʳᵉ vraie écriture crashe** : `.fscheck` écrit 2 octets (history.cpp:216-218) → fichier *inline* littlefs stocké dans la paire de métadonnées, **aucun appel à `lfs_alloc`**. Le div/0 dans `lfs_alloc` ne peut se déclencher qu'à la première écriture nécessitant une allocation de bloc (saveToFile ~15-22 Ko ou flush log). Le check inline est donc structurellement aveugle à cette corruption.

### 2. Bilan d'espace : le FS PEUT être plein pendant la réécriture de /history.json

- Taille de `/history.json` à capacité : 72 RAW + 168 HOURLY + N DAILY points × ~69 o/point (format history.cpp:375-384) ≈ **17-22 Ko** (nota : le cap `kMaxHourlyDataPoints=168` = 7 j est ATTEINT avant `HOURLY_MAX_AGE`=15 j → les hourly en excès sont supprimés sans jamais devenir daily ; le régime permanent est ~245 pts ≈ 17 Ko).
- `saveToFile()` ouvre en `"w"` (history.cpp:365) : en littlefs (copy-on-write), les blocs de l'ANCIEN fichier restent committés jusqu'au close du nouveau → pic = ancien (5-6 blocs) + nouveau (5-6 blocs).
- Budget pire cas sur 16 blocs de 4 Ko : 2 superblocs/racine + 10-12 (history ×2) + 4-5 (`/system.log` ≤ 16 Ko + ε) + 3 (`/system.log.tmp` de rotation, 12 Ko) = **19-22 blocs demandés > 16 disponibles**. Le NOSPC est structurellement atteignable dès ~5-7 jours d'accumulation avec un log proche du plafond.
- Chemins d'erreur : `saveToFile` ne teste que `serializeJson(...) == 0` (history.cpp:386) — une **écriture partielle** (NOSPC en cours de route, `serializeJson` retourne les octets écrits > 0) n'est PAS détectée ; `errno`/`getWriteError` jamais lus ; aucun log de l'espace libre. Pire : le `systemLogger.error(...)` du chemin d'échec déclenche un **flush immédiat** (logger.cpp:72-74) → nouvelle écriture sur FS plein, toutes les 5 min.
- Aggravant : pendant un `/download-logs` (File ouvert), la rotation échoue proprement (`unlink` refusé, voir §3) → `/system.log` reste > 16 Ko ET `/system.log.tmp` peut rester orphelin → +4 blocs occupés.

### 3. Accès concurrents : sérialisés au niveau VFS (vérifié) — hypothèse écartée

- Écrivains réels sur la partition history : **loopTask** (`history.saveToFile`, `logger.update→flushToDisk`), **async_tcp** (`systemLogger.error/critical` dans les handlers web → flush immédiat logger.cpp:72-74 ; `clearAll()` via `/logs` DELETE, web_routes_data.cpp:423-426), **mqttTask** (ADR-0011, `systemLogger.error` → flush). Lecteur : `/download-logs` chunked (async_tcp), `File` conservé dans un `shared_ptr` entre chunks (web_routes_data.cpp:127-137).
- **Vérifié dans `libesp_littlefs.a`** (framework 3.20017 / core 2.0.17) : symboles `_efs_lock`, `sem_take`/`sem_give` — chaque opération VFS prend le sémaphore de l'instance → deux écritures simultanées sur le même `lfs_t` sont sérialisées appel par appel. Pas de fenêtre de corruption API-level.
- **Fichier ouvert vs remove/rename** : esp_littlefs REFUSE `unlink`/`rename` sur un fichier ouvert (chaînes « Failed to unlink path "%s". Has open FD. », « Cannot rename; src/dst is open »). Donc `clearAll()` ou la rotation pendant un download ne corrompent pas — ils échouent silencieusement (bug fonctionnel mineur : le fichier survit au clear ; la rotation saute).
- Résidu fonctionnel (pas corruption) : deux `flushToDisk()` concurrents peuvent ouvrir 2 handles en append sur `/system.log` (entrelacement de lignes) et lancer 2 rotations croisées (perte de lignes, tmp orphelin).

### 4. Version littlefs embarquée et config

- `libesp_littlefs.a` expose `lfs_fs_gc`, `lfs_fs_mkconsistent`, `lfs_fs_grow`, `lfs_fs_prepsuperblock` → **littlefs v2.9.x** (récent ; la plupart des bugs de corruption anciens sont corrigés — version exacte d'esp_littlefs du lib-builder IDF 4.4 à épingler, piste upstream « NOSPC répété / compaction near-full sur très petit FS » à vérifier).
- Le mount VALIDE la géométrie (« Invalid block count (%u != %u) », « Invalid block size ») → un superblock avec `block_count=0` serait rejeté au mount (puis reformaté par `begin(true)`). Le div/0 observé implique donc un `lfs->block_count` (ou état allocateur) devenu 0 **après** un mount accepté — cohérent avec des métadonnées à CRC valide mais contenu insensé.
- Config sdk : `CONFIG_LITTLEFS_ASSERTS=y`, READ/WRITE/LOOKAHEAD=128, CACHE=512, BLOCK_CYCLES=512. Pas de LFS_THREADSAFE (verrouillage fait au niveau VFS, cf. §3). Pas de multiversion.

### 5. Hypothèses classées

1. **H1 — Saturation NOSPC récurrente pendant la réécriture de /history.json** (vraisemblance haute comme *condition nécessaire/aggravante*) : bilan d'espace §2, chemins d'erreur ignorés, réécritures toutes les 5 min qui martèlent la paire superblock {0,1} (compactions fréquentes, BLOCK_CYCLES=512). Mécanisme final de corruption = bug littlefs near-full (à confirmer upstream pour v2.9.x) — littlefs est censé rendre un NOSPC propre.
2. **H2 — Corruption RAM de l'état lfs_t/caches (heap corruption ailleurs) persistée sur flash avec CRC valides** (vraisemblance moyenne) : colle parfaitement à la signature (mount OK, `.fscheck` inline OK, div/0 au 1er `lfs_alloc`) et à la période #1 (gels lwIP + panics multiples). littlefs COW ne protège PAS contre ses propres structures RAM écrasées.
3. **H3 — Écriture interrompue par panic/ESP.restart** (vraisemblance faible) : COW → soit état valide, soit paire détectée corrompue au mount (→ reformatage auto par `begin(true)`), pas la signature observée.
4. **H4 — Accès concurrents non sérialisés** : **écartée** (verrou VFS vérifié, §3).

### 6. Instrumentation / correctifs recommandés (discriminants pour la prochaine occurrence)

1. **Préserver la preuve avant destruction** : dans le disjoncteur feature-059 (history.cpp:138-155), AVANT `esp_partition_erase_range`, dumper en hex sur Serial les blocs 0-1 (8 Ko) de la partition (+ empreinte courte en NVS). Discriminant décisif H1/H2/H3 (superblock CRC-valide insensé = H2 ; chaîne métadonnées pleine/incohérente = H1 ; paire déchirée = H3).
2. **saveToFile robuste** : comparer `serializeJson(doc, f)` à `measureJson(doc)` (écriture partielle), lire `errno`/`getWriteError`, logger `historyFs.usedBytes()/totalBytes()` à chaque save ; compteur NOSPC + epoch du dernier save réussi en NVS.
3. **Pré-check espace** : si `total - used < taille prévue + marge` → `remove(historyFilePath)` AVANT d'écrire (perdre l'ancien historique vaut mieux qu'écrire sur FS plein) — supprime la fenêtre « ancien + nouveau ».
4. **Réduire l'empreinte pire cas** sous 16 blocs garantis : `kMaxLogFileBytes` 16→8 Ko (keep 6 Ko) et/ou `kMaxHourlyDataPoints` 168→120.
5. **Différer le flush ERROR/CRITICAL** (flag consommé par loopTask) : supprime toutes les écritures history-FS depuis async_tcp/mqttTask (défense en profondeur même si H4 est écartée, et casse la boucle « error sur FS plein → flush sur FS plein »).

## Critères de clôture

- [ ] Mécanisme de corruption identifié OU liste des hypothèses résiduelles avec plan de discrimination (instrumentation à ajouter pour la prochaine occurrence).
- [ ] Décision documentée : correctif / instrumentation / confinement accepté (le disjoncteur borne le coût à la perte de l'historique).

## Clôture (2026-07-27)

- [x] Mécanisme identifié : **saturation structurelle de la partition (NOSPC)** — pire cas 19-22 blocs demandés vs 16 disponibles (réécriture COW de /history.json 17-22 Ko toutes les 5 min + log 16 Ko + tmp rotation 12 Ko) ; div/0 `lfs_alloc` sur FS plein = bug littlefs amont répertorié (issue #1063, esp_littlefs #183), version embarquée figée par la plateforme. Hypothèse résiduelle H2 (corruption RAM lfs_t persistée) discriminée par l'instrumentation ci-dessous à la prochaine occurrence. Concurrence écartée (verrou VFS vérifié). Pas de job de minuit (saves uniformes 5 min ; le 00:24 = reboot + crash 1ʳᵉ écriture allocante).
- [x] Décision : **correctif** → spec `bug-nospc-partition-history.md` (v2.11.0) : préservation de preuve dans le disjoncteur (dump blocs 0-1 avant effacement), garde d'espace + remove-avant-write dans saveToFile, détection écriture partielle, log plafonné 16→8 Ko, instrumentation NVS.
