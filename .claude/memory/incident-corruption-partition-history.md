---
name: incident-corruption-partition-history
description: "Corruption LittleFS de la partition history (juillet 2026) — boot-loop IntegerDivideByZero, récupération par erase_region, correctif disjoncteur de boot-loop à implémenter"
metadata: 
  node_type: memory
  type: project
  originSessionId: 82048c5b-7e8f-4cec-8242-ad9b5ed01f91
  modified: 2026-07-30T11:26:31.083Z
---

**Incident (2026-07-22, firmware v2.9.2)** : après ~1 semaine sur site, boot-loop `IntegerDivideByZero` dans `lfs_alloc` (littlefs) à la première écriture dans `/system.log` (`Logger::setPersistenceFs` → `fclose`), appelé depuis `HistoryManager::begin()`. Cause : superblock/métadonnées de la partition `history` (0x3E0000, 64 KB) corrompus.

**Pourquoi le garde-fou `.fscheck` n'a pas suffi** : un fichier de 2 octets est stocké *inline* dans les métadonnées littlefs et n'exerce jamais `lfs_alloc` ; et une division par zéro est une exception CPU non récupérable — aucun test d'écriture ne peut l'attraper proprement.

**Récupération** : `esptool.py --port /dev/cu.usbserial-0001 erase_region 0x3E0000 0x10000` → reformatage auto au boot suivant (`historyFs.begin(true, ...)`). Config NVS et FS web intacts.

**Correctif durable IMPLÉMENTÉ en v2.10.0 (2026-07-22, feature-059, ADR-0031)** : disjoncteur de boot-loop — compteur `crash_cnt` en NVS (`hist_meta`) posé avant montage, remis à 0 sur toutes les sorties de `begin()` ; 2 crashs consécutifs (PANIC/WDT, brownout exclu) pendant l'init → effacement auto de la partition + alerte MQTT `history_partition_reset`. Module pur `src/history_guard_logic.*`, suite `test_native_history_guard`. Doc : `docs/subsystems/system-health.md` (3ᵉ volet). Reste à tester on-target (corruption simulée `esptool write_flash 0x3E0000`).

**Cause racine du « spam » élucidée (investigation debugger 2026-07-22)** : le spam apparent (~250 lignes/s) était un artefact de rejeu du FIFO UART0 (preuve : 6 286 × 11 octets = les 70 Ko capturés ; plafond structurel du code 400× inférieur). Le dédup MQTT est correct. Lien avec la corruption : improbable. Correctif collatéral : émission console du logger en un seul `Serial.write` (buffer pile). Bug latent réel découvert : course `refreshTopics()` (UAF) → **corrigé en v2.10.0 (2026-07-23)** : guard « rebuild seulement si préfixe changé » + `topicsMutex` à l'entonnoir `enqueueOutbound` (spec en done/). Resterait à faire un jour : `reconnectRequested` en `std::atomic<bool>` (race bénigne, notée en revue).

**CAUSE RACINE ÉLUCIDÉE (investigation 27/07/2026, ADR-0033)** : les « corruptions » de la partition history étaient une **saturation d'espace structurelle** (pire cas 22 blocs demandés / 16 dispo : réécriture COW de history.json + log 16 Ko + tmp rotation) déclenchant le bug littlefs amont div/0-sur-FS-plein (issue #1063, version figée par la plateforme). Récidive #2 le 26/07 (v2.10.4) : **le disjoncteur feature-059 a fonctionné** (auto-réparation ~1 min, validation on-target). Correctif v2.11.0 : garde d'espace `historySaveAdmit` + remove-avant-write + log 8/6 Ko → pire cas 13/16 blocs ; preuve `[HISTDUMP]` préservée avant tout futur effacement (discriminant de l'hypothèse résiduelle H2 corruption RAM). Si récidive après 2.11.0 → chercher `[HISTDUMP]` sur Serial + CRC en NVS hist_meta.

**Classe de crash récurrente identifiée (2 coredumps : 23 et 24/07/2026)** : `std::bad_alloc` non rattrapé dans `cbuf::resize` (`AsyncResponseStream` non pré-dimensionné) → `abort()` → reboot, sur `/debug/sensor_trace` (fix 2.10.1) puis `/get-logs` (fix 2.10.4 + audit complet). Règle gravée dans **ADR-0032** : toute réponse HTTP > ~4 Ko doit être paginée+pré-dimensionnée, chunked, ou allocation-unique-testée ; seuils heap fixes interdits. **Méthode de décodage coredump** : `dd bs=1 skip=20` sur coredump.bin → ELF core → `xtensa-esp32-elf-gdb <firmware.elf> -c core.elf -ex bt` ; si l'ELF local ne correspond pas au firmware flashé, reconstruire depuis le commit exact dans un worktree (`pio run`, ~35 s).

**Coredump #4 (28/07, v2.11.0) : bad_alloc broadcast WS** — heap effondré < 2 Ko plus vite que la fenêtre 60 s de l'auto-heal. Cause n°1 (investigation churn-heap, spec done/) : **client WS zombie** (téléphone verrouillé sans fermer le TCP → 32 buffers ~1,8 Ko épinglés). Corrigé v2.11.1 : garde heap sur les broadcasts, file WS bornée à 5 trames (`-DWS_MAX_QUEUED_MESSAGES=5` + closeOnQueueFull, fermeture par le fork sous SON verrou — ne JAMAIS itérer getClients() depuis loopTask, érase concurrent), keepalive 30 s, auto-heal adaptatif 5 s sous seuil (~15 s de réaction). Reportés si récidive : /get-history chunked (String 40 Ko épinglée), instrumentation ring RTC noinit, purge code mort broadcastLog.

**Coredump #5 (30/07, v2.13.0) : la garde WS 2.11.1 contournée par TOCTOU** (contrôle 8192 == seuil restart → zone grise) + générateur G3 (/get-history 40 Ko) toujours actif. **Corrigé v2.14.0** : garde 16384 + try/catch bad_alloc sur textAll (filet absolu), buffer statique .bss pour la trame WS (idée utilisateur « réserver une fois »), /get-history chunked zéro-buffer (G3 soldé — plus AUCUNE grosse allocation contiguë au firmware), **ring télémétrie heap RTC noinit** (échantillon 2 s, dump [ring] des ~64 dernières secondes au boot post-panic, magic 0xB10C4EA9) — si un crash mémoire réapparaît, la cinétique sera dans le journal au boot suivant.

**How to apply:** si un boot-loop `IntegerDivideByZero` post-`Partition historique dédiée montée` réapparaît sur firmware < 2.10.0 → erase_region ci-dessus ; sur ≥ 2.10.0 l'appareil se répare seul en 1 cycle de reboot. Pour tout nouveau coredump `abort()` : chercher D'ABORD le dump [ring] de cinétique heap dans le journal post-reboot (≥ 2.14.0), puis suspecter une allocation non gardée hors ADR-0032. refreshTopics UAF : corrigé en 2.10.0.
