# Feature — Disjoncteur de boot-loop : auto-réparation de la partition history corrompue

## Métadonnées
- **ID** : feature-059
- **Slug** : disjoncteur-bootloop-history
- **Type** : feature
- **Statut** : livré — tests verts (289/289 dont 24 nouveaux), revue OK, docs à jour (déplacement vers `done/` par l'agent principal)
- **Priorité** : haute
- **Version cible** : 2.10.0
- **Dernière mise à jour** : 2026-07-22
- **Spec(s) liée(s)** : ADR-0029 (auto-guérison heap), ADR-0030 (watchdog connectivité), bug-spam-sensor-problem-serial (cause racine possible de la corruption)

## Domaines touchés
- [ ] Capteurs (pH / ORP / température)
- [ ] Régulation (pump_controller, PID)
- [ ] Filtration / Éclairage
- [ ] Web — routes HTTP / WebSocket
- [ ] MQTT / Home Assistant
- [ ] UART / écran LVGL
- [ ] OTA
- [ ] Authentification
- [x] Historique / persistance
- [ ] Frontend (`data/`)

## Agents requis
- [x] spec-reader
- [x] architect
- [ ] pool-chemistry
- [ ] ux-ui-designer
- [x] embedded-developer
- [ ] web-ui-developer
- [x] test-engineer
- [x] code-reviewer
- [x] doc-writer

## Description
Quand la partition LittleFS `history` est corrompue au point de faire paniquer littlefs à la première écriture (boot-loop), l'appareil doit détecter la boucle et effacer/reformater lui-même la partition, puis redémarrer normalement — sans intervention USB.

## Contexte
**Incident réel (2026-07-22, v2.9.2)** : après ~1 semaine sur site, boot-loop `IntegerDivideByZero` dans `lfs_alloc` (littlefs) à la première écriture dans `/system.log` :
```
HistoryManager::begin() (history.cpp:152)
 → Logger::setPersistenceFs() (logger.cpp:205, f.close())
   → fclose → lfs_ctz_extend → lfs_alloc (lfs.c:689) → division par zéro (panic CPU)
```
Cause : superblock/métadonnées de la partition `history` corrompus. L'appareil est resté injoignable (boot-loop avant montée du WiFi) jusqu'à une récupération manuelle en USB (`esptool erase_region 0x3E0000 0x10000`).

**Pourquoi le garde-fou existant ne suffit pas** : le test `.fscheck` post-montage (history.cpp:112-150) écrit un fichier de 2 octets — stocké *inline* dans les métadonnées littlefs, il n'exerce jamais `lfs_alloc`. Et une division par zéro est une **exception CPU non récupérable** : aucun test d'écriture ne peut « attraper » ce cas proprement. La seule défense possible est de détecter la boucle de panics *entre* les boots et d'effacer la partition avant de la remonter.

## Comportement attendu

Machine à disjoncteur autour de l'init history, persistée en NVS (namespace `hist_meta` déjà existant) :

1. **Avant le montage** de la partition history dans `HistoryManager::begin()` : poser un drapeau NVS « init history en cours » (avec compteur de tentatives).
2. **Après init réussie** (montage + `.fscheck` + `setPersistenceFs` + `loadFromFile` terminés sans crash) : effacer le drapeau et remettre le compteur à zéro.
3. **Au boot, avant le montage** : si le drapeau est présent **et** que la cause du reset précédent est un crash (`esp_reset_reason()` ∈ {panic, watchdog interrompu/tâche}), incrémenter le compteur.
4. **Si le compteur atteint le seuil** (2 crashs consécutifs pendant la fenêtre d'init) : effacer la partition (`esp_partition_erase_range`), log CRIT explicite, remettre le compteur à zéro, puis procéder au montage normal (qui reformate).
5. La fenêtre de détection est **strictement limitée à l'init history** : un panic n'importe où ailleurs dans le firmware ne doit jamais déclencher l'effacement (le drapeau n'est posé que pendant l'init).

### Cas limites
- Panic ailleurs dans setup() avant l'init history → drapeau absent → aucun effet.
- Reset normal (POWERON, soft reset, OTA) avec drapeau resté posé (crash très précoce sans panic ?) → ne compter que si `esp_reset_reason()` indique un crash ; sinon réarmer sans incrémenter.
- Crash pendant l'init pour une cause NON liée au FS (ex. bug transitoire) → coût acceptable : perte de l'historique après 2 crashs consécutifs au même endroit ; c'est le trade-off assumé (disponibilité de l'appareil > conservation de l'historique).
- NVS indisponible → dégrader silencieusement vers le comportement actuel (pas de disjoncteur, jamais de blocage).
- Interaction avec le budget anti-boucle partagé (`kAutoHealMaxReboots`, mode maintenance) : à arbitrer par l'architect — le disjoncteur history agit AVANT la boucle de reboots ; il ne doit pas consommer le budget des auto-guérisons heap/réseau ni être bloqué par le mode maintenance.

## Comportement actuel
- Test `.fscheck` write/read post-montage + reformatage si échec (insuffisant : n'exerce pas `lfs_alloc`).
- Effacement automatique uniquement si la partition a été **redimensionnée** (`part_sz` en NVS `hist_meta`).
- Aucune détection de boot-loop : une corruption qui panique à l'écriture rend l'appareil définitivement injoignable.

## Contraintes techniques
- ESP32, offline first
- Logique de décision **pure et testable en natif** (pattern ADR-0017, comme `self_heal_logic` / `net_health_logic`) : la décision (incrémenter / déclencher / réarmer) doit être extraite dans un module sans dépendance Arduino, testé dans `test/test_native_*`.
- NVS : réutiliser le namespace `hist_meta` (déjà utilisé pour `part_sz` dans history.cpp).
- `esp_reset_reason()` (API ESP-IDF, dispo sous Arduino).
- Aucun impact sur le chemin de dosage ; aucune nouvelle dépendance.
- Logs via `systemLogger` — mais attention : au moment du disjoncteur, la persistance des logs n'est pas encore active (c'est justement ce qu'on protège) → les logs partent en RAM/Serial, c'est suffisant.

## Fichiers potentiellement concernés
- `src/history.cpp` — intégration du disjoncteur autour de begin() (pose/levée du drapeau, effacement conditionnel)
- `src/history_guard_logic.cpp/.h` **[nouveau]** (nom indicatif, architect tranche) — logique pure de décision
- `src/constants.h` — seuil (`kHistoryInitCrashThreshold = 2` indicatif), clés NVS
- `test/test_native_history_guard/` **[nouveau]** — tests natifs de la logique pure
- `docs/subsystems/system-health.md` — nouveau volet d'auto-guérison
- `docs/subsystems/logger.md` / `docs/adr/` — selon décisions

## Livrables — check-list
- [x] A1 — Module de logique pure de décision du disjoncteur (compteur, reset reason, verdict : rien / incrémenter / effacer) → `src/history_guard_logic.{h,cpp}`
- [x] A2 — Intégration dans `HistoryManager::begin()` : drapeau NVS posé avant montage, levé après init complète réussie (levée sur **toutes** les sorties de `begin()`)
- [x] A3 — Effacement `esp_partition_erase_range` + log CRIT quand le verdict est « effacer »
- [x] A4 — Constantes dans `constants.h` (`kHistoryInitCrashThreshold = 2`, `kHistoryCrashCountNvsKey = "crash_cnt"`)
- [x] B1 — Alerte MQTT `history_partition_reset` (arbitrage architect : OUI) : flag `_partitionAutoErased` + getter dans history, publication différée depuis `setup()` après `mqttManager.begin()` via `publishAlert` existant (aucune modif de mqtt_manager.*)
- [x] D1 — Tests natifs de la logique pure (nominal, cas limites reset reasons, seuil, réarmement) → `test/test_native_history_guard/`, 24 tests
- [x] D2 — `pio run` sans warning nouveau, suites natives existantes vertes (289/289)
- [x] C1 — `docs/subsystems/system-health.md` mis à jour (3ᵉ volet d'auto-guérison : partition history)
- [x] C2 — ADR confirmé → `docs/adr/0031-disjoncteur-bootloop-history.md` (trade-off : perte d'historique vs disponibilité)
- [x] C3 — `CHANGELOG.md` entrée 2.10.0
- [x] C4 — `docs/MQTT.md` : alerte `history_partition_reset`

## Nouvelles signatures / API (optionnelle)
- Fonctions publiques : module logique pure (signatures à définir par l'architect)
- Constantes : `constexpr uint8_t kHistoryInitCrashThreshold = 2;` (indicatif)
- Aucun endpoint HTTP / champ WS / topic MQTT nouveau (une alerte MQTT `history_partition_reset` est envisageable — à arbitrer, cohérence avec `network_stall_reboot`)

## ADR attendu ?
- **Probable** : oui
- **Critère(s) candidat(s)** : 2 (compromis disponibilité vs conservation des données), 3 (alternatives : test d'écriture élargi — impossible à cause de l'exception CPU ; validation du superblock avant montage — fragile/dépendant des internals littlefs)
- **Sujet pressenti** : « Disjoncteur de boot-loop sur l'init de la partition history »

## Éléments visibles (pour doc-writer)

### Endpoints HTTP concernés
- aucun

### Champs WebSocket
- aucun

### Topics MQTT
- éventuellement une alerte `history_partition_reset` (à arbitrer pendant le cycle) → `docs/MQTT.md` si retenue

### Documentation à produire / mettre à jour
#### Docs vivantes
- `docs/subsystems/system-health.md` — 3ᵉ volet d'auto-guérison (partition history)
- `docs/subsystems/logger.md` — si le comportement de persistance au boot change

#### Docs transverses
- `CHANGELOG.md` — entrée sous [2.10.0]
- `docs/MQTT.md` — seulement si alerte MQTT retenue

#### Docs immuables
- `docs/adr/0031-disjoncteur-bootloop-history.md` — si ADR confirmé

## Critères d'acceptation
- [x] Quand le drapeau init est présent et que le reset précédent est un panic ×2 consécutifs, alors la partition est effacée avant montage et le boot se termine normalement (vérifiable en test natif sur la logique + revue de l'intégration).
- [x] Quand le reset précédent est un POWERON/soft reset, le compteur n'est PAS incrémenté même si le drapeau est présent.
- [x] Un panic hors fenêtre d'init history ne déclenche jamais l'effacement (drapeau posé uniquement pendant la fenêtre, levé sur toutes les sorties de `begin()`).
- [x] Après une init réussie, compteur remis à zéro et drapeau levé.
- [x] Tous les libellés/logs en français.
- [x] `pio run` compile sans nouveau warning ; suites natives existantes + nouvelle suite vertes (289/289, dont 24 nouveaux).

## Plan de test manuel
1. Firmware flashé sain : vérifier au boot que l'init history réussit et qu'aucun log de disjoncteur n'apparaît (drapeau levé silencieusement).
2. Simuler la corruption : écrire des octets aléatoires dans la partition history (esptool write_flash à 0x3E0000) puis observer : boot 1 → panic éventuel compté, boot 2 → effacement + log CRIT + démarrage complet, appareil joignable.
3. Vérifier que l'historique repart de zéro proprement et que `/system.log` se remplit.

## Notes pour agent
- La récupération manuelle documentée reste valable en secours : `esptool.py erase_region 0x3E0000 0x10000`.
- Le crash réel de référence est décrit dans le Contexte ; ne pas essayer de « rattraper » la division par zéro (impossible), la détection inter-boot est la seule approche viable.
- S'aligner sur le style des modules purs existants (`self_heal_logic`, `net_health_logic`) et sur `docs/subsystems/system-health.md`.

## Hors périmètre
- La cause racine de la corruption (spam d'écritures dans `/system.log`) → spec séparée `bug-spam-sensor-problem-serial.md`.
- Toute rotation/limitation de taille de `/system.log` (si nécessaire, sera traitée par la spec bug ou une spec dédiée).
- Le mode maintenance et le budget anti-boucle existants (pas de refonte, seulement un arbitrage d'interaction).

## Notes d'implémentation (v2.10.0, 2026-07-22)

- **Itération unique** : cycle complet orchestrator → spec-reader → architect → embedded-developer → test-engineer → code-reviewer (**Approved**, 2 suggestions mineures appliquées) → doc-writer, sans reboucle.
- **Fichiers créés** : `src/history_guard_logic.{h,cpp}` (module pur ADR-0017 : `HistoryResetKind`, `HistoryGuardDecision` {Proceed/CountCrash/ErasePartition}, `isCrashResetKind()`, `evaluateHistoryGuard()` — ordre des règles : crashCount==0 → Proceed ; reset propre → Proceed ; crashCount ≥ seuil → ErasePartition ; sinon CountCrash), `test/test_native_history_guard/` (24 tests : mapping exhaustif 10 reset reasons, frontières ≥, seuils dégénérés 0/1/255).
- **Fichiers modifiés** : `src/history.cpp` (bloc disjoncteur avant le garde-fou `part_sz` ; drapeau `crash_cnt` posé avant `historyFs.begin()`, levé sur les 3 sorties de `begin()` ; helpers NVS à échec silencieux), `src/history.h` (`partitionWasAutoErased()`), `src/constants.h` (`kHistoryInitCrashThreshold=2`, `kHistoryCrashCountNvsKey="crash_cnt"`, namespace NVS `hist_meta` réutilisé), `src/main.cpp` (alerte différée `history_partition_reset` après `mqttManager.begin()`), `platformio.ini` (module pur dans `build_src_filter` env:native), `src/version.h` (2.10.0).
- **Décisions architect** : brownout exclu du mapping crash (cause électrique — éviter l'effacement sur alim faible) ; tous les WDT inclus ; budget `kAutoHealMaxReboots` non consommé (le disjoncteur ne reboote jamais, il répare puis laisse booter) ; NVS retenu contre RAM RTC (la corruption FS survit aux coupures secteur) ; alerte MQTT différée (outQueue inexistante pendant `history.begin()`).
- **Revue** : retour d'`esp_partition_erase_range` vérifié (échec → CRIT explicite, pas de fausse alerte MQTT) ; log warning « x/2 » sur CountCrash validé.
- **Validation** : `pio run` SUCCESS 0 warning nouveau, `pio test -e native` 289/289 (9 suites).
- **Docs** : `docs/adr/0031-disjoncteur-bootloop-history.md` (+ index ADR, où 0030 manquait — corrigé au passage), `docs/subsystems/system-health.md` (3ᵉ volet), `docs/MQTT.md` (alerte), `docs/subsystems/logger.md` (note pré-persistance), `CHANGELOG.md` [2.10.0].
- **Reste on-target** (plan de test manuel non exécuté — matériel remis en service) : simuler une corruption (`esptool write_flash 0x3E0000`) et vérifier effacement au 2ᵉ boot + alerte MQTT.
