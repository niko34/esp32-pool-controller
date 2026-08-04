# ADR-0031 — Disjoncteur de boot-loop sur l'init de la partition history (détection inter-boot NVS + effacement au seuil)

- **Statut** : Accepté
- **Date** : 2026-07-22

> ℹ️ **Note (2026-08-04, feature-063 / [ADR-0034](0034-migration-core3-idf5.md)) — défense CONSERVÉE (décision utilisateur 2026-08-03)** : le déclencheur amont (bug littlefs div/0 dans `lfs_alloc`) est **corrigé sous IDF 5** (littlefs 2.11.2). Après la bascule core 3, ce disjoncteur devient une **défense dormante** — il reste en place, il n'est **PAS retiré**.
- **Décideurs** : Nicolas Philippe (architect), debugger (diagnostic du boot-loop), embedded-developer
- **Spec(s) liée(s)** : feature-059-disjoncteur-bootloop-history

## Contexte

**Incident réel (2026-07-22, v2.9.2)** : après ~1 semaine sur site, l'appareil est entré en **boot-loop** `IntegerDivideByZero` dans `lfs_alloc` (littlefs) à la **première écriture** dans `/system.log` :

```
HistoryManager::begin() → Logger::setPersistenceFs() (f.close())
 → fclose → lfs_ctz_extend → lfs_alloc (lfs.c:689) → division par zéro (panic CPU)
```

Cause : superblock/métadonnées de la partition `history` (64 Ko) corrompus. LittleFS **monte sans erreur** un filesystem corrompu puis panique à l'écriture. Le crash survenant dans `setup()` **avant la montée du WiFi**, aucun des mécanismes existants ne pouvait agir : l'appareil est resté **injoignable une semaine**, jusqu'à récupération manuelle en USB (`esptool erase_region 0x3E0000 0x10000`).

C'est le **5ᵉ mode de défaillance « appareil injoignable »** traité :

1. `std::bad_alloc` sur `/logs` (2.8.2) — PANIC → reboot.
2. Assert lwIP `tcp_recved` WebSocket (2.9.0, [ADR-0028](0028-migration-fork-esp32async.md)) — PANIC → reboot.
3. Fragmentation du tas sans crash (2.9.1, [ADR-0029](0029-auto-guerison-heap.md)) — gel silencieux, heap.
4. Gel de la pile lwIP avec heap sain (2.9.2, [ADR-0030](0030-watchdog-connectivite.md)) — gel silencieux, réseau.
5. **Boot-loop sur partition history corrompue** (ce ADR) — panic à chaque boot, avant WiFi.

Contrainte structurante : une division par zéro est une **exception CPU non rattrapable** — aucun test d'écriture in-process ne peut « attraper » ce cas. La seule défense viable est de détecter la boucle de panics **entre** les boots.

## Décision

**Détection inter-boot en NVS + effacement de la partition au seuil de 2 crashs consécutifs**, implémentée dans `HistoryManager::begin()` avec décision pure dans le module `history_guard_logic` ([ADR-0017](0017-logique-metier-pure-humble-object-testabilite.md)) :

1. **Drapeau/compteur** `crash_cnt` en NVS (namespace `hist_meta` existant), **posé juste avant** `historyFs.begin()` et **remis à 0 sur toutes les sorties** de `begin()`. La fenêtre de détection est strictement limitée à l'init history : un panic ailleurs dans le firmware ne déclenche jamais rien.
2. **Au boot**, avant montage, `evaluateHistoryGuard()` évalue : `crashCount == 0` → `Proceed` ; reset **propre** → `Proceed` (réarmement **sans** compter) ; `crashCount >= kHistoryInitCrashThreshold (2)` → `ErasePartition` (`esp_partition_erase_range` vérifié `ESP_OK` + log CRIT) ; sinon → `CountCrash` (compteur+1, WARN).
3. **Resets comptés comme crash** : `PANIC`, `INT_WDT`, `TASK_WDT`, `WDT`. **Exclus** : `POWERON`, `SW`, `EXT`, `BROWNOUT`, `DEEPSLEEP` — le **brownout est exclu volontairement** : cause électrique (alim faible), pas un symptôme de corruption FS ; le compter risquerait d'effacer l'historique sur une simple alimentation défaillante.
4. **Indépendant** du budget `kAutoHealMaxReboots` et du mode maintenance : le disjoncteur **ne reboote jamais** (il répare puis laisse le boot continuer — c'est le crash qui reboote) et agit **avant** tout mécanisme runtime, pendant `setup()`. Bloquer la réparation en mode maintenance laisserait l'appareil mort avant d'être joignable.
5. **Alerte MQTT `history_partition_reset` différée** : l'effacement a lieu avant `mqttManager.begin()` → flag `_partitionAutoErased` + getter `partitionWasAutoErased()`, publication depuis `setup()` via le `publishAlert()` existant.
6. **NVS indisponible** → dégradation silencieuse vers le comportement historique (jamais de blocage du boot).

## Alternatives considérées

- **Option A (rejetée) — Test d'écriture élargi au boot** (exercer `lfs_alloc` avant usage réel). Impossible : la corruption produit une **exception CPU non rattrapable** — le test d'écriture *devient* le point de crash, la boucle persiste. Le `.fscheck` existant (2 octets) est de toute façon stocké *inline* dans les métadonnées littlefs et n'exerce jamais `lfs_alloc`.
- **Option B (rejetée) — Validation du superblock/métadonnées avant montage.** Parser les structures littlefs à la main pour décider si la partition est saine. Rejeté : dépend des **internals** de littlefs (format non contractuel), fragile à chaque upgrade du framework, et ne garantit pas de couvrir toutes les corruptions menant au panic.
- **Option C (rejetée) — Compteur en RAM RTC au lieu de NVS** (comme le budget anti-boucle heap/réseau). Rejeté : la RAM RTC est **effacée au power-on**, alors que la corruption FS **survit aux coupures secteur** — un utilisateur qui coupe/remet le courant (réflexe naturel face à un appareil mort) réinitialiserait le compteur et la boucle reprendrait indéfiniment. Le NVS survit au power-cycle.
- **Option D (retenue) — Détection inter-boot NVS + effacement au seuil 2.** Seule approche qui fonctionne face à une exception non rattrapable, robuste aux power-cycles, fenêtre strictement bornée à l'init history, décision pure testable en natif.

## Conséquences

### Positives

- **Fin du boot-loop définitif** : au 2ᵉ crash consécutif pendant l'init history, la partition est effacée et le boot se termine normalement — plus de récupération USB, plus de semaine d'indisponibilité.
- **Fenêtre strictement bornée** : le drapeau n'existe que pendant l'init history ; aucun panic ailleurs ne peut provoquer l'effacement.
- **Robuste aux power-cycles** : compteur en NVS (pas en RAM RTC), la défense survit aux coupures secteur — comme la corruption qu'elle combat.
- **Observabilité** : log CRIT explicite + alerte MQTT `history_partition_reset` (différée) → l'effacement est traçable.
- **Décision testable** : `history_guard_logic` pur, 24 tests natifs (289 au total).

### Négatives / dette assumée

- **Perte de l'historique sur faux positif** : 2 crashs consécutifs pendant la fenêtre d'init — même de cause **non liée** au FS (bug transitoire) — effacent l'historique et `/system.log`. Trade-off assumé : **disponibilité de l'appareil > conservation de l'historique**.
- **Le log CRIT de l'effacement n'est pas persisté** : au moment du disjoncteur, la persistance des logs n'est pas encore active (c'est précisément ce qu'on protège) — il part en RAM + Serial uniquement.
- **La cause racine de la corruption n'est pas traitée ici** (piste : martèlement de `/system.log` — spec `bug-spam-sensor-problem-serial`, verdict : improbable ; surveillance à poursuivre).

### Ce que ça verrouille

- Le compteur du disjoncteur reste en **NVS** (namespace `hist_meta`, clé `crash_cnt`) : ne pas le migrer vers la RAM RTC (perdrait la robustesse au power-cycle).
- Le disjoncteur reste **indépendant** du budget `kAutoHealMaxReboots` et du mode maintenance : ne pas le raccorder à la machine d'auto-guérison runtime (il ne reboote jamais, il répare avant boot).
- Le **brownout ne compte pas** comme crash : toute évolution du mapping des reset reasons doit préserver cette exclusion (ou la superséder par un nouvel ADR).
- La levée du drapeau (`crash_cnt = 0`) doit couvrir **toutes** les sorties de `HistoryManager::begin()` : tout nouveau chemin de sortie doit lever le drapeau.
- La logique de décision reste **pure** (`history_guard_logic`, [ADR-0017](0017-logique-metier-pure-humble-object-testabilite.md)) : toute évolution du seuil/des règles passe par ce module et ses tests natifs.

## Références

- Code : [`src/history_guard_logic.h`](../../src/history_guard_logic.h) / [`src/history_guard_logic.cpp`](../../src/history_guard_logic.cpp), [`src/history.cpp`](../../src/history.cpp) (`mapResetReason`, `readHistoryCrashCount`, `writeHistoryCrashCount`, intégration dans `begin()`), [`src/history.h`](../../src/history.h) (`partitionWasAutoErased()`), [`src/main.cpp`](../../src/main.cpp) (alerte différée), [`src/constants.h`](../../src/constants.h) (`kHistoryInitCrashThreshold`, `kHistoryCrashCountNvsKey`)
- Tests : `test/test_native_history_guard/` — 24 tests dédiés (289 au total)
- Spec : `specs/features/done/feature-059-disjoncteur-bootloop-history.md`
- Récupération manuelle de secours : `esptool.py erase_region 0x3E0000 0x10000`
- Docs : [subsystems/system-health.md](../subsystems/system-health.md), [MQTT.md](../MQTT.md)
- Antécédents : [ADR-0009](0009-partition-coredump.md) (partition history 64 Ko), [ADR-0029](0029-auto-guerison-heap.md) / [ADR-0030](0030-watchdog-connectivite.md) (auto-guérisons runtime, indépendantes de ce disjoncteur)
