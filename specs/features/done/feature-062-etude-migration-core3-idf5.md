# Feature — Étude de migration : core Arduino 2.0.17/IDF 4.4.7 → core 3.x/IDF 5.x via pioarduino

## Métadonnées
- **ID** : feature-062
- **Slug** : etude-migration-core3-idf5
- **Type** : feature (étude — AUCUNE modification du firmware de production dans ce cycle)
- **Statut** : done (étude livrée le 2026-08-03 — verdict : GO TECHNIQUE CONDITIONNEL, voir « Synthèse de l'étude »)
- **Priorité** : basse (chantier « hiver » — le confinement du wedge lwIP couvre le besoin en saison)
- **Version cible** : N/A (livrable = rapport + plan, pas de release)
- **Dernière mise à jour** : 2026-08-03
- **Spec(s) liée(s)** : ADR-0028 (migration fork ESP32Async), ADR-0030 (confinement wedge lwIP — la motivation racine), ADR-0033 (littlefs figé par la plateforme — le div/0 amont serait corrigé par IDF 5)

## Domaines touchés
*(étude : lecture seule du firmware ; un banc d'essai compile hors arbre de production)*
- [x] Tous les domaines sont AUDITÉS, aucun n'est MODIFIÉ

## Agents requis
- [x] spec-reader
- [ ] architect — c'est l'étude elle-même qui produit l'architecture de migration
- [ ] pool-chemistry — non requis (aucun chemin modifié) ; sera OBLIGATOIRE dans le futur cycle d'exécution (LEDC = pompes doseuses)
- [x] embedded-developer (inventaire d'API + banc d'essai de compilation)
- [ ] web-ui-developer
- [x] test-engineer (stratégie de validation)
- [x] code-reviewer (revue du rapport, pas de code)
- [x] doc-writer (rapport final + éventuel ADR de décision)

## Description
Étudier la migration de la plateforme de build du contrôleur vers le core Arduino 3.x / ESP-IDF 5.x via pioarduino, sans toucher au firmware de production : inventaire exhaustif des ruptures, matrice de compatibilité des dépendances, banc d'essai de compilation, plan de bascule avec critères GO/NO-GO.

## Contexte
- Plateforme actuelle : `espressif32@6.9.0` = core Arduino **2.0.17** / IDF **4.4.7** — lignée abandonnée (Espressif ne supporte plus PlatformIO ; plus aucun correctif).
- Motivations : (1) le **wedge lwIP** (gels réseau, confinement ADR-0030 — IDF 5 embarque un lwIP nettement plus récent) ; (2) le **bug littlefs div/0 amont** (ADR-0033 — corrigé dans les versions livrées avec IDF 5) ; (3) sécurité long terme (correctifs).
- Décision de véhicule DÉJÀ prise (discussion 2026-08-03) : **pioarduino** (empaquetage communautaire du core officiel — le code embarqué reste 100 % Espressif), déjà en production sur `esp32-pool-controller-screen`. Plan B documenté : « Arduino comme composant ESP-IDF » (officiel, accès sdkconfig/lwIP) — même dette d'API, payable plus tard sans surcoût.
- Acquis réutilisables du projet écran : URL plateforme pioarduino, particularité `~/.platformio/penv/bin/pio` (le pio système Python 3.12 échoue sur fatfs/littlefs).

## Comportement attendu (livrables de l'étude)

### L1 — Inventaire des ruptures d'API core 2 → 3 (rapport chiffré, fichier:ligne)
Balayer NOS usages réels contre les breaking changes documentés du core 3.x, au minimum :
- **LEDC** (`ledcSetup`/`ledcAttachPin`/`ledcWrite` → nouvelle API `ledcAttach`) — **pump_controller = chemin de dosage** → le futur cycle d'exécution exigera pool-chemistry ;
- **Watchdog** : `esp_task_wdt_init(timeout, panic)` → `esp_task_wdt_config_t` (IDF 5) ;
- **Événements WiFi** (`ARDUINO_EVENT_*`, handlers), `WiFi.onEvent`, comportement reconnexion/AP ;
- **esp_ping** (sonde ICMP du watchdog connectivité) : API `ping_sock.h` sous IDF 5 ;
- **RTC_NOINIT_ATTR**, RAM RTC (auto-heal + heap ring) : sémantique conservée ? ;
- **LittleFS/esp_littlefs** : version embarquée IDF 5 (le div/0 amont y est-il corrigé ? — vérifier le commit 749a456 dans la version livrée) ; **compatibilité du filesystem existant sur la partition** (format littlefs 2.x stable, à confirmer) ;
- **NVS** : compatibilité du contenu existant (config, compteurs, hist_meta) après flash IDF 4.4 → 5.x ;
- **Preferences, Wire/I2C (EZO), OneWire/DallasTemperature, RTClib, PubSubClient** : compat déclarée core 3 ;
- **AsyncTCP 3.4.10 / ESPAsyncWebServer 3.11.2** (fork ESP32Async) : compat 3.x déclarée au README — à VALIDER en compilation réelle, y compris nos build_flags (`WS_MAX_QUEUED_MESSAGES`, `CONFIG_LWIP_*` — ces overrides lwIP passent-ils encore ?) ;
- **Exceptions C++** (`-fexceptions` — nos try/catch bad_alloc), **partitions.csv**, **build_fs.sh**, **esp_rom_crc32_le**, **esp_partition_***, coredump (format ELF conservé ? nos scripts GDB), **UART/Serial2** (écran).

### L2 — Banc d'essai de compilation (hors production)
`[env:esp32dev_core3]` dans un platformio.ini d'essai (ou branche dédiée) pointant pioarduino : compiler, lister TOUTES les erreurs, prototyper les corrections minimales (sans les merger) → chiffrage réel du diff (nb fichiers/lignes).

### L3 — Stratégie de validation
- Les **415 tests natifs** sont insensibles à la plateforme (env native) — acquis ;
- Plan de test on-target sur **banc d'essai matériel** (un 2ᵉ ESP32 hors piscine, config AP/WiFi de test) : checklist par sous-système (pompes PWM à vide !, sondes I2C, OneWire, RTC, WiFi/AP secours, MQTT/HA, OTA, LittleFS + partition history, disjoncteur, auto-heal, watchdog connectivité, heap ring, écran UART) ;
- Critère chimique : AUCUNE bascule production sans validation pool-chemistry du comportement PWM/dosage sur banc (mesure au verre doseur avant/après).
- Période de cohabitation : durée, critères de retour arrière (le flash IDF5→IDF4 doit être testé aussi — retour possible ?).

### L4 — Décision documentée
Rapport final + **ADR de décision de migration** (GO avec plan et fenêtre, ou NO-GO/report motivé). Critères GO : diff maîtrisé, banc d'essai 100 % vert, littlefs/NVS compatibles, fork Async validé, retour arrière possible.

## Contraintes techniques
- **INTERDIT de modifier le firmware de production** dans ce cycle (pas un seul fichier src/ de main) — l'étude produit un rapport + un banc d'essai isolé (branche `etude/core3` ou worktree).
- Le banc d'essai matériel ne pilote JAMAIS de vraies pompes en eau (PWM à vide ou LED témoin).
- Budget : l'étude s'arrête au rapport — l'exécution fera l'objet d'une spec séparée avec son propre cycle complet (orchestrator + pool-chemistry obligatoires).

## Livrables — check-list
- [x] A1 — Inventaire L1 : tableau exhaustif usage → rupture → correction type → fichier(s):ligne(s), avec les points « chemin de dosage » marqués
- [x] A2 — Banc L2 : env core3 qui compile (SUCCESS) — ⚠️ avec caveat : banc déroulé sur un arbre périmé, à re-dérouler sur HEAD (voir « Caveat majeur » dans la synthèse)
- [x] A3 — Vérifications critiques : littlefs (div/0 corrigé, format compatible), NVS préservé, retour arrière sûr ; fork Async : compat déclarée mais NON prouvée au banc (caveat A2)
- [x] D1 — Plan de validation L3 (checklist banc matériel + critères chimiques + procédure de retour arrière)
- [x] C1 — Rapport de synthèse + projet d'ADR de décision (GO/NO-GO argumenté) — voir « Synthèse de l'étude » ci-dessous + `docs/adr/DRAFT-migration-core3-idf5.md` *(renommé `0034-migration-core3-idf5.md` et accepté par feature-063)*

## Critères d'acceptation
- [x] Chaque API utilisée par le firmware est classée : inchangée / rupture documentée avec correction / à investiguer (liste vide ou justifiée).
- [x] Le banc de compilation core 3 aboutit (SUCCESS) ou tous les blocants restants sont nommés avec piste. *(SUCCESS en 3 passes — mais sur arbre périmé, cf. caveat : la re-passe sur HEAD est la 1ʳᵉ tâche du cycle d'exécution)*
- [x] Les 4 questions critiques ont une réponse sourcée : littlefs corrigé ?, FS compatible ?, NVS compatible ?, retour arrière possible ? *(4/4 favorables, voir Q1-Q4)*
- [x] Aucun fichier du firmware de production modifié (`git status` du tronc vierge). *(L'étude n'a touché aucun fichier de production — le banc vit dans un worktree isolé. Les modifications non commitées présentes sur le tronc préexistent à l'étude et relèvent d'un autre chantier.)*
- [x] Le rapport permet de décider GO/NO-GO sans investigation supplémentaire.

## Plan de test manuel
N/A pour l'étude (le plan de test EST un livrable — L3).

## Hors périmètre
- L'exécution de la migration (spec séparée, cycle complet, pool-chemistry obligatoire).
- Le tuning lwIP/sdkconfig (plan B « Arduino en composant IDF » — seulement documenté).
- La migration du projet écran (déjà sur pioarduino).

---

## Synthèse de l'étude (2026-08-03)

### Verdict : GO TECHNIQUE CONDITIONNEL

La faisabilité est **démontrée** : le diff firmware attendu est minuscule (~20-25 lignes sur 3 fichiers), le banc de compilation aboutit (SUCCESS), et les 4 questions critiques ont toutes une réponse favorable et sourcée. Le GO définitif d'exécution reste suspendu aux **conditions** suivantes :

1. **Re-dérouler le banc L2 sur HEAD** avec le fork ESP32Async réellement épinglé (AsyncTCP 3.4.10 / ESPAsyncWebServer 3.11.2) — première tâche du cycle d'exécution (chemin prouvé, ~1 h) ;
2. **Surveillance de la taille Flash** : ~92 % d'app0 attendus sur l'arbre actuel (marge ~140-170 Ko) ;
3. **Validation pool-chemistry** de la transposition LEDC (pompes doseuses = chemin de dosage) sur banc matériel ;
4. **Fenêtre hors saison**, pompes débranchées, avec le plan de validation L3 100 % vert.

Le projet d'ADR de décision est rédigé : `docs/adr/DRAFT-migration-core3-idf5.md` (préfixe `DRAFT-` : il ne sera numéroté et accepté qu'à la décision d'exécution). *(Fait : renommé [`docs/adr/0034-migration-core3-idf5.md`](../../../docs/adr/0034-migration-core3-idf5.md) et accepté — feature-063, 2026-08-04.)*

### A1/A3 — Inventaire des ruptures (embedded-developer, sourcé)

**Diff attendu : ~20-25 lignes / 3 fichiers.** Ruptures identifiées :

| API | Rupture | Correction | Note |
|---|---|---|---|
| **LEDC** (pompes doseuses) | `ledcSetup`/`ledcAttachPin`/`ledcWrite` → nouvelle API | Transposition `ledcAttachChannel`/`ledcWriteChannel` écrite ; canaux, 20 kHz, 8 bits conservés | ⚠️ chemin de dosage → **pool-chemistry obligatoire** au cycle d'exécution |
| **Watchdog** | `esp_task_wdt_init(timeout, panic)` → `esp_task_wdt_config_t` | Struct + reconfigure | |
| **mbedtls 3.x** | API sha256 modifiée | Correction prototypée | Découverte par le banc (absente des breaking changes balayés en L1) |

**Inchangés** (vérifié) : événements WiFi (`ARDUINO_EVENT_*`, `WiFi.onEvent`), esp_ping, `RTC_NOINIT_ATTR`, `esp_partition_*`/CRC, exceptions C++ (`-fexceptions` confirmé), Serial2/UART écran, LittleFS/Preferences/Wire, OneWire 2.3.8.

**Les 4 questions critiques (toutes favorables)** :
- **Q1 — littlefs div/0** : fix **PRÉSENT** (littlefs 2.11.2 livré avec IDF 5, contre 2.9.0 chez nous) — le déclencheur amont de l'ADR-0033 disparaît ; les défenses ADR-0031/0033 deviennent une deuxième ligne dormante.
- **Q2 — format FS** : lfs 2.1, **compatible bi-directionnel** (la partition existante est relue telle quelle dans les deux sens).
- **Q3 — NVS** : contenu **préservé** (config, compteurs, hist_meta).
- **Q4 — retour arrière** : **sûr si bascule OTA-only** (le bootloader IDF 4.4 est conservé ; un OTA retour vers un binaire 2.14.x reste possible).

**Découverte collatérale** : les `-DCONFIG_LWIP_*` de feature-014/ADR-0011 (`MAX_SOCKETS=24`, `TCP_MAXRTX=5`) n'ont **JAMAIS été appliqués** — la lib lwIP est précompilée (16 sockets / 12 retransmissions effectifs, aujourd'hui comme après migration). L'ADR-0011 documente donc un effet fantôme → **correctif documentaire à prévoir** (note dans l'ADR-0011 ou ADR correcteur, hors périmètre de cette étude).

### A2 — Banc de compilation L2 (embedded-developer)

- **SUCCESS en 3 passes**, 7 erreurs au total : mbedtls ×3, WDT ×2, LEDC ×2+1.
- Corrections prototypées **bi-core** (`#if ESP_ARDUINO_VERSION_MAJOR >= 3`) — le même code compile sur core 2 et core 3.
- Worktree conservé : `.claude/worktrees/agent-af3208fa3eac7de23` (branche `worktree-agent-af3208fa3eac7de23`).
- Piège d'installation documenté : `pio pkg install` préalable requis + utiliser le pio du penv (`~/.platformio/penv/bin/pio`).
- Empreinte mesurée : Flash **90,4 % d'app0** (+191 Ko), RAM +3,3 Ko.

#### ⚠️ Caveat majeur (vérifié par l'agent principal)

Le worktree du banc était basé sur un **arbre PÉRIMÉ** (pré-migration fork, époque 2.9.0 : `lib_deps` Git me-no-dev master résolus en ESPAsyncWebServer 3.6.0 / AsyncTCP 3.3.2, 214 tests natifs au lieu de 415). Conséquences :

1. La compatibilité core 3 de **notre fork ESP32Async épinglé** (AsyncTCP 3.4.10 / ESPAsyncWebServer 3.11.2) reste **à prouver au banc** (déclarée au README amont seulement) ;
2. La taille **90,4 % sous-estime** l'arbre actuel (baseline banc 80,0 % vs 81,9 % réel → **~92 % attendus**, marge ~140-170 Ko) ;
3. ESPAsyncWiFiManager 0.31 a compilé, mais sur l'ancien arbre.

Le **catalogue de ruptures src reste valide** (mbedtls/WDT/LEDC : fichiers présents à l'identique dans les deux arbres). **Conséquence opérationnelle : re-dérouler le banc sur HEAD = première tâche du cycle d'exécution** (chemin prouvé, ~1 h).

### D1 — Plan de validation L3 (test-engineer)

- **Checklist banc matériel complète** : PWM vérifié à l'oscilloscope + mesure au verre doseur (écart < 5 %), OTA **bi-directionnel** (le véhicule ET le retour), FS/NVS testés dans les deux sens, AP secours ESPAsyncWiFiManager testé explicitement.
- **Bascule production hors saison**, pompes débranchées.
- **Observation 14 jours** post-bascule.
- **Procédure de retour arrière archivée** : binaires 2.14.x + empreintes sha256.

### Clôture

- CHANGELOG : **aucune entrée** — l'étude ne change aucun comportement du firmware (critère du projet : entrée CHANGELOG seulement si changement visible utilisateur).
- Spec : statut **done**. Le déplacement vers `specs/features/done/` se fera au commit (aucun commit sans demande explicite de l'utilisateur).
- Suite : ouvrir une spec d'exécution dédiée (cycle complet orchestrator + pool-chemistry obligatoires), démarrant par le re-banc sur HEAD.
