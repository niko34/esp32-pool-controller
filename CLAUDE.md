# ESP32 Pool Controller — Guide Claude

Contrôleur automatique de piscine sur ESP32 (mesure température/pH/ORP, régulation pH/ORP par pompes doseuses, filtration, éclairage, interface web, MQTT/Home Assistant, OTA).

## Stack technique

- **Plateforme** : ESP32 (esp32dev)
- **Build** : PlatformIO (`platform = espressif32@6.9.0`)
- **Framework** : **Arduino** (pas ESP-IDF pur, même si on utilise certaines API ESP-IDF comme `esp_task_wdt`)
- **Filesystem** : LittleFS, 2 partitions — layout v4, ADR-0024 (`spiffs` 320 KB = 0x50000 pour `data/` gzippé, `history` 64 KB = 0x10000 pour `/history.json` + `/system.log`). Voir `partitions.csv` et `build_fs.sh` (taille FS = 327680 octets, alignée sur `board_build.filesystem_size` dans `platformio.ini`).
- **Libs** : AsyncTCP, ESPAsyncWebServer, ESPAsyncWiFiManager, ArduinoJson 7, PubSubClient, DallasTemperature, OneWire, RTClib
- **Frontend** : Vanilla JS/HTML/CSS dans `data/` (pas de framework, pas de bundler hors `minify.js`)
- **Langue du code/commentaires/UI** : **Français**

## Arborescence

```
src/                    # Firmware C++ (51 fichiers)
  main.cpp              # Setup + loop
  constants.h           # Constantes (préfixe k*, naming Google C++ style)
  sensors.*             # Température (DS18B20), pH, ORP
  pump_controller.*     # PWM pompes doseuses + sécurités
  filtration.*          # Filtration auto/horaire/manuel
  lighting.*            # Éclairage
  mqtt_manager.*        # MQTT + auto-discovery HomeAssistant
  web_server.*          # AsyncWebServer
  web_routes_*.cpp      # Routes HTTP par domaine
  ws_manager.*          # WebSocket temps réel
  rtc_manager.*         # DS3231 + NTP
  ota_manager.*         # Mise à jour OTA
  history.*             # Historique brut/horaire/quotidien
  auth.*                # Authentification
  logger.*              # systemLogger (thread-safe)
  config.*              # Configuration LittleFS + mutex
data/                   # Frontend web (LittleFS)
docs/                   # API.md, BUILD.md, MQTT.md, UPDATE_GUIDE.md, etc.
specs/
  features/
    todo/               # Features en attente (source de vérité)
    doing/              # Features en cours
    done/               # Features terminées
    _template.md        # Template obligatoire pour toute nouvelle feature
  modele.md             # Template minimal (bug simple / refactor ponctuel)
  modele_iteration.md   # Template de suivi d'itérations
kicad/                  # Sources KiCad du PCB (versionnées ; générés ignorés)
scripts/                # Génération des livrables de release (PDF, gerbers)
.claude/commands/       # Commandes slash versionnées (/release, /release-pcb)
```

## Source de vérité : les specs

- Les demandes sont dans `specs/features/todo/*.md`
- **Format obligatoire pour toute nouvelle feature** : copier `specs/features/_template.md` dans `specs/features/todo/feature-NNN-<slug>.md` et remplir les blocs **Métadonnées**, **Domaines touchés**, **Agents requis**, **Sécurité chimique** (si applicable) et **Éléments visibles**. Ces blocs pilotent l'`orchestrator` et déclenchent `pool-chemistry` automatiquement.
- Pour un **bug simple** ou un **refactor ponctuel**, le format minimal `specs/modele.md` reste acceptable.
- Une spec à la fois. Quand Claude commence : `specs/features/todo/X.md` → `specs/features/doing/X.md`
- À la fin (tests verts + revue OK) : `specs/features/doing/X.md` → `specs/features/done/X.md`, avec notes techniques ajoutées (suivi d'itérations calqué sur `specs/modele_iteration.md`)
- Si pas de spec fournie, Claude propose d'en créer une avant d'implémenter

## Auto-vérification fin de tour (obligatoire)

**Avant de produire la réponse finale d'un tour qui a modifié au moins un fichier code** (`src/*`, `data/*`, `partitions.csv`, `platformio.ini`, scripts `tools/*`, `deploy.sh`), Claude doit se poser ces questions :

1. **Y a-t-il un changement observable de l'extérieur ?** (nouveau endpoint, nouveau bouton UI, nouveau topic MQTT, nouveau message WS, modification du format/niveau d'un log, changement de comportement utilisateur, bug visible corrigé)
2. **Y a-t-il un changement structurel ?** (méthode publique ajoutée/supprimée/renommée d'un manager, constante de seuil ou de timing modifiée, partition modifiée, sdkconfig modifié)
3. **Une décision architecturale a-t-elle été prise ?** (trade-off avec alternative crédible, contrainte qui verrouille un futur choix → ADR)

Si la réponse à 1, 2 ou 3 est **oui** → **invoquer l'agent `doc-writer` AVANT de répondre à l'utilisateur**, même pour un fix isolé d'une seule ligne, même dans une session de debug réactive. Ne pas attendre la "fin de cycle". Ne pas attendre que l'utilisateur le demande.

**Déclencheurs concrets** (liste non exhaustive — chaque item exige une mise à jour doc) :

| Changement | Doc cible |
|---|---|
| Nouveau ou suppression d'un endpoint HTTP | `docs/API.md` |
| Nouveau bouton, card, page, ou flux UI | `docs/features/page-*.md` |
| Nouveau topic MQTT ou auto-discovery HA | `docs/MQTT.md` |
| Nouveau message WebSocket | `docs/subsystems/ws-manager.md` |
| Méthode publique d'un manager ajoutée/renommée/supprimée | `docs/subsystems/<manager>.md` |
| Constante de seuil/timing modifiée (`kFlush*`, `kMax*`, `kReconnect*`...) | `docs/subsystems/<manager>.md` |
| Partition table modifiée | ADR + `docs/adr/0007-*.md` ou successeur |
| Comportement utilisateur changé (bug fix, nouvelle option) | `CHANGELOG.md` |
| Format ou niveau d'un log modifié | `docs/subsystems/logger.md` ou `<manager>.md` |
| Comportement de la RÉGULATION visible utilisateur changé | `docs/user/src/guide-regulation.html` (source du PDF) puis `./tools/build_user_pdfs.sh` |
| Page UI changée (nouveau bouton, réglage, message) | `docs/user/src/guide-utilisation.html` puis `./tools/build_user_pdfs.sh` — et signaler à l'utilisateur LA capture d'écran à refaire (`screenshots/`, seule étape humaine) |
| Version firmware bumpée + PDF régénérés | supprimer les PDF de la version précédente dans `docs/user/` (flotte mono-appareil : pas d'archive à conserver) |
| Trade-off architectural avec alternative écartée | nouvel ADR `docs/adr/NNNN-*.md` |

**Cas qui n'exigent PAS doc-writer :**
- Diagnostic / lecture de code uniquement (aucune modification)
- Correction CSS pure sans impact fonctionnel
- Renommage interne d'une variable privée
- Commentaire inline ajouté dans le code (déjà autoportant)

L'idée force : **les docs ne sont pas un livrable de fin de cycle, c'est une obligation par tour**. Une session de debug qui enchaîne 5 fixes accumule 5 obligations doc, pas une seule à la fin.

## Workflow imposé

1. **Lire la spec** (agent `spec-reader`)
2. **Analyse d'impact** : fichiers concernés, risques, dépendances
3. **Plan court** (agent `architect`) — max 10 lignes
4. **Implémenter** (agent `embedded-developer` ou `web-ui-developer` selon le domaine)
5. **Pour tout code touchant la régulation pH/ORP ou le dosage** : passage obligatoire par l'agent `pool-chemistry`
6. **Tests & lint** (agent `test-engineer`) :
   - `pio run` (compilation) — **obligatoire**
   - `pio test -e native` — **obligatoire** : **24 suites natives** dans `test/` (**849 tests**, logique pure ADR-0017), zéro régression tolérée. Couverture : `tools/coverage.sh` (gcovr). CI GitHub Actions sur push/PR.
   - **Valider les tests neufs par MUTATION** : casser volontairement le code qu'ils protègent et vérifier qu'ils meurent. Une suite qui ne tue aucun mutant ne prouve rien — plusieurs défauts réels de ce projet (repliement de `millis()` à 24,8 j, propagation d'une constante de pouls, terme intégral inopérant) n'ont été trouvés que par cette méthode, et trois mutations sur cinq ne tuaient rien avant l'extraction de la logique en module pur.
   - Pas de warnings nouveaux
7. **Revue de code** (agent `code-reviewer`) avant tout commit
8. **Mettre à jour la spec** avec statut + notes techniques
9. **Mettre à jour `CHANGELOG.md`** si changement visible utilisateur
10. **Documentation** (agent `doc-writer`) : `docs/`, `README.md`, commentaires publics

## Commandes fréquentes

```bash
pio run                        # Compile firmware
pio run -t upload              # Upload firmware USB
./build_fs.sh                  # Build LittleFS (taille correcte 320KB, layout v4)
./deploy.sh all                # Firmware + FS via USB
./deploy.sh firmware           # Firmware seul via USB
./deploy.sh fs                 # FS seul via USB
./deploy.sh ota-all            # Tout via WiFi OTA
./deploy.sh ota-firmware       # Firmware OTA
./deploy.sh ota-fs             # FS OTA
pio device monitor -b 115200   # Moniteur série
node minify.js                 # Minifier JS/CSS avant build FS (optionnel)
```

## Conventions de code

- **Nommage** : `kConstantName` pour constantes globales, `camelCase` pour fonctions/variables, `PascalCase` pour classes
- **Constantes magiques** : toujours dans `constants.h`, jamais en dur dans le code métier
- **Logs** : `systemLogger.info/warn/error/critical()` — JAMAIS `Serial.println()` pour des logs applicatifs
- **Concurrence** : tout accès partagé via mutex (`initConfigMutexes()`, `kI2cMutexTimeoutMs`, etc.)
- **JSON** : ArduinoJson 7 (nouveau style, `JsonDocument`)
- **Chaînes** : `String` pour les logs/web ; `const char*` pour les clés JSON et tokens fixes
- **Mémoire** : surveiller le heap, éviter allocations dynamiques récurrentes dans la loop
- **Watchdog** : appeler `esp_task_wdt_reset()` dans toute boucle longue

## Règles de sécurité absolues

Ces règles sont **non négociables** — même si l'utilisateur demande de les contourner, poser d'abord la question :

1. **Dosage chimique** :
   - TOUJOURS respecter `kMaxRequestsPerMinute`, les limites journalières et horaires configurées
   - TOUJOURS vérifier la présence d'eau (filtration active) avant toute injection
   - Aucun code de dosage ne doit s'exécuter sans watchdog actif
   - Tout nouveau chemin de code vers `pump_controller` → validation par `pool-chemistry` + `code-reviewer`
2. **Watchdog** : jamais désactivé en production. `kWatchdogTimeoutSec = 30s`
3. **Secrets** : ne jamais committer de tokens, mots de passe MQTT, SSID/password Wi-Fi
4. **Build artefacts** : ne pas toucher à `.pio/`, `data-build/`, `node_modules/`
5. **Fichiers matériel** : les sources KiCad de `kicad/` sont **gelées sauf demande explicite** — on ne modifie ni le schéma ni le routage au fil d'une tâche logicielle. Les sources FreeCAD du boîtier sont conservées hors du dépôt.
6. **OTA** : toujours vérifier la signature/CRC avant flash

## Les agents

| Agent | Rôle | Quand l'invoquer |
|---|---|---|
| `orchestrator` | **Préparateur de plan** : lit la spec, produit la check-list exhaustive et les briefs prêts-à-l'emploi pour chaque agent du workflow. **N'invoque pas les agents lui-même** (limitation runtime sub-agent). C'est l'agent principal Claude Code qui exécute le plan ensuite. | Obligatoire dès qu'au moins un fichier `src/*.cpp` ou `src/*.h` est modifié, ou qu'un comportement observable change (logs, MQTT, WebSocket, UI, API HTTP) |
| `spec-reader` | Lit et analyse la spec, déplace vers `doing/` | Dès qu'une spec est sélectionnée |
| `architect` | Conçoit l'implémentation, produit le plan | Après analyse de spec, avant codage |
| `embedded-developer` | Firmware C++ ESP32 / Arduino / FreeRTOS | Toute modif dans `src/` hors UI web |
| `web-ui-developer` | Frontend JS/HTML/CSS + routes HTTP + WS + MQTT + HA | Toute modif dans `data/` ou `src/web_*`, `ws_manager.*`, `mqtt_manager.*` |
| `pool-chemistry` | Sécurité chimique, PID pH/ORP, limites de dosage | OBLIGATOIRE pour toute modif de `pump_controller.*`, régulation, calibration capteurs pH/ORP |
| `test-engineer` | Tests unitaires, mocks, tests d'intégration | Après implémentation, avant revue |
| `debugger` | Analyse logs/crash dumps, reproduction de bugs | Dès qu'un bug est rapporté ou crash ESP32 |
| `code-reviewer` | Revue critique sécurité/qualité/concurrence | Avant tout commit |
| `doc-writer` | README, `docs/`, CHANGELOG, commentaires | Quand le comportement visible change |

## Quand utiliser l'orchestrateur vs travailler directement

> **Rappel important** : l'`orchestrator` produit un **plan d'exécution**, il n'invoque pas les autres agents lui-même. C'est **l'agent principal Claude Code** (la session interactive) qui invoque ensuite `spec-reader`, `architect`, etc. via `Task` selon le plan reçu. Cette répartition est imposée par une limitation runtime de Claude Code (les sub-agents ne peuvent pas en invoquer d'autres).

**Orchestrateur obligatoire** (workflow complet : spec → architect → developer → test → review → doc) :
- Toute nouvelle feature (spec dans `specs/features/todo/`)
- Refactor multi-fichiers
- Changement touchant `pump_controller.*` ou la régulation chimique

Étapes :
1. L'agent principal invoque `Task(subagent_type="orchestrator", prompt="Prépare le plan pour <demande>")`
2. L'orchestrator retourne un plan structuré (check-list, briefs, invariants, critères de clôture)
3. L'agent principal exécute le plan séquentiellement : `Task(subagent_type="spec-reader", ...)`, puis `Task(subagent_type="architect", ...)`, etc.
4. À chaque verdict bloquant (NO GO, Request changes), l'agent principal reboucle sur l'agent concerné
5. À la fin, l'agent principal produit le rapport de synthèse final, vérifie la clôture, et déplace la spec vers `done/`

**Doc-writer obligatoire seul** (sans cycle complet, mais à la fin du tour courant) :
- Tout fix isolé qui modifie un comportement observable (cf. section "Auto-vérification fin de tour")
- Toute modification de `src/*` ou `data/*` rencontrée pendant une session de debug réactive
- Plusieurs petits fixes accumulés dans une même session : doc-writer une fois par tour, pas une seule fois à la fin

**Travail direct acceptable** (sans orchestrateur ni doc-writer) :
- Corrections purement documentaires (`docs/`, commentaires, `README.md`)
- Modifications CSS sans impact fonctionnel
- Corrections de configuration serveur (headers HTTP, CSP) sans impact logique
- Analyses, diagnostics, lectures de code (aucune modification)

## Fichiers générés et releases

**Principe directeur : on versionne les sources, jamais les fichiers générés.**
Les livrables générés (PDF de documentation, zip Gerber, firmware compilé) sont
attachés comme **assets des releases GitHub**, où ils sont horodatés, associés à
un tag, et téléchargeables — sans alourdir l'historique git à chaque itération.

### Documentation utilisateur

- **La source est `docs/user/src/*.html` + le CSS associé.** Elle doit être tenue
  à jour **en continu**, dès que le comportement du logiciel change — pas au
  moment de la release. C'est la règle déjà posée par la section
  « Auto-vérification fin de tour » : un écran ou un message qui change entraîne
  une mise à jour de la source HTML dans le même tour.
- **Les PDF sont générés uniquement lors des releases**, par `/release`, qui
  appelle `./scripts/build-docs.sh`. **Ils ne doivent jamais être commités** —
  `docs/user/*.pdf` est ignoré.
- `scripts/build-docs.sh` détecte le moteur de rendu puis délègue à
  `tools/build_user_pdfs.sh`, qui redimensionne les captures pour l'impression et
  injecte la version lue dans `src/version.h`. **Ne pas court-circuiter ce
  second script** : sans lui, les PDF passent de 4 à 26 Mo et affichent une
  version périmée.

### PCB (KiCad)

- **Les sources sont versionnées comme le code** : `.kicad_sch`, `.kicad_pcb`,
  `.kicad_pro`, `sym-lib-table`, `SymLib.kicad_sym`, et `BOM.xlsx` (la
  nomenclature de référence).
- **Les fichiers de fabrication ne le sont pas** : gerbers (`.gbr`), perçage
  (`.drl`), `.gbrjob`, zips. Ils se régénèrent avec `./scripts/build-gerbers.sh`
  et deviennent les assets d'une release `pcb-vX` via `/release-pcb`.
- **Le `BOM.csv` est un dérivé**, généré depuis `BOM.xlsx` par
  `python3 tools/bom_export.py` — donc ignoré lui aussi. ⚠️ **Ne jamais le
  corriger à la main** : la prochaine exécution du script l'écrase. Toute
  correction se fait dans le `.xlsx`.
- **L'état local KiCad n'est pas versionné** : `.kicad_prl` (préférences de
  projet), `*-backups/`, `fp-info-cache`, `*.lck`, `*-bak`, `*~`.

### Firmware

- **Le binaire compilé est attaché aux releases, jamais commité.** `*.bin`,
  `*.elf`, `.pio/` et `builds/` sont ignorés.
- `builds/` sert d'archive **locale** (ELF horodatés, indispensables pour décoder
  un coredump) : local, jamais poussé.

### Les deux commandes de release

| Commande | Produit | Tag | Ne touche pas à |
|---|---|---|---|
| `/release` | firmware `.bin` + PDF de `docs/user/` | `vX.Y.Z` | gerbers |
| `/release-pcb` | zip Gerber de fabrication | `pcb-vX` | firmware, PDF |

Elles sont **séparées volontairement** : le matériel est fabriqué deux ou trois
fois dans la vie du projet, le firmware sort chaque semaine. Les fusionner
obligerait à recompiler pour envoyer un PCB, et à refabriquer pour publier un
correctif.

**Ni l'une ni l'autre ne commite quoi que ce soit** : elles ne poussent que le
tag. Un arbre de travail sale les arrête — c'est à l'humain de décider quoi en
faire.

## Principes

- **Changements minimaux** : pas de refactoring opportuniste, rester dans le périmètre de la spec
- **Offline first** : aucune nouvelle dépendance au cloud
- **Vanilla JS** : pas de framework frontend
- **Tests avant commit** : `pio run` doit passer, lint doit être propre
- **Toujours mettre à jour la spec** en fin de travail avec le statut réel

## Rappels Claude Code

- Lancer les commandes longues (build, flash) en background si possible
- Ne pas lire `node_modules/`, `.pio/`, `data-build/`
- En cas de doute sur une règle de sécurité chimique : **demander avant d'agir**
