# ESP32 Pool Controller — Guide Claude

Contrôleur automatique de piscine sur ESP32 (mesure température/pH/ORP, régulation pH/ORP par pompes doseuses, filtration, éclairage, interface web, MQTT/Home Assistant, OTA).

## Stack technique

- **Plateforme** : ESP32 (`board = esp32dev`, module WROVER-E avec PSRAM), flash **8 Mo**
- **Build** : PlatformIO — **env matériel unique `esp32dev_core3`**, plateforme **pioarduino épinglée `55.03.37`** (core Arduino 3.3.7 / IDF 5.5.0). ADR-0042 (core 3 par défaut) et ADR-0069 (retrait du repli core 2, v2.42.1). ⚠️ `espressif32@6.9.0` reste **installée** sans être référencée — elle fournit `tool-mklittlefs` à `build_fs.sh` : ne pas la désinstaller. ⚠️ Ne jamais lancer `pio pkg install` / `uninstall` sans nécessité : la commande réécrit `platformio.ini` en supprimant tous ses commentaires.
- **Framework** : **Arduino** (pas ESP-IDF pur, même si on utilise certaines API ESP-IDF comme `esp_task_wdt`)
- **Filesystem** : LittleFS, 2 partitions — **layout v5, ADR-0068** (`spiffs` **512 KB** = 0x80000 à l'offset 0x730000 pour `data/` gzippé, `history` **256 KB** = 0x40000 à l'offset 0x7B0000 pour `/history.json` + `/system.log`). Voir `partitions.csv` et `build_fs.sh` (taille FS = **524288** octets, alignée sur `board_build.filesystem_size` dans `platformio.ini`).
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
    _template.md        # Template UNIQUE, obligatoire pour toute spec
kicad/                  # Sources KiCad du PCB (versionnées ; générés ignorés)
scripts/                # Génération des livrables de release (PDF, gerbers)
.claude/commands/       # Commandes slash versionnées (/release, /release-pcb)
```

## Source de vérité : les specs

- Les demandes sont dans `specs/features/todo/*.md`
- **Format obligatoire pour toute nouvelle feature** : copier `specs/features/_template.md` dans `specs/features/todo/feature-NNN-<slug>.md` et remplir les blocs **Métadonnées**, **Domaines touchés**, **Agents requis**, **Sécurité chimique** (si applicable) et **Éléments visibles**. Ces blocs pilotent l'`orchestrator` et déclenchent `pool-chemistry` automatiquement.
- **Il n'y a qu'un seul template**, `specs/features/_template.md`, et il vaut pour tout : feature, bug, refactor. Pour un sujet petit, on l'allège — les sections marquées optionnelles se suppriment, les blocs pilotant l'`orchestrator` restent. Aucun format minimal séparé n'existe (décision du 2026-09-06 : les renvois vers un `specs/modele.md` jamais créé sont retirés).
- Une spec à la fois. Quand Claude commence : `specs/features/todo/X.md` → `specs/features/doing/X.md`
- À la fin (tests verts + revue OK) : `specs/features/doing/X.md` → `specs/features/done/X.md`, avec notes techniques ajoutées. Pour un travail mené en plusieurs itérations, se calquer sur une spec à itérations déjà close de `specs/features/done/`
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
| Composant visuel ajouté, modifié, **ou réemployé dans un contexte nouveau** | `docs/DESIGN_SYSTEM.md` |
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
   - `pio test -e native` — **obligatoire** : les suites natives de `test/` (logique pure ADR-0017), **zéro régression tolérée**.
     ⚠️ **Ne recopie pas un compteur depuis cette ligne** (ADR-0073) — il pourrit à chaque
     livraison : ce chiffre a été faux pendant des semaines, corrigé le 2026-09-07, et
     périmé le soir même. **Mesure-le** :
     `ls -d test/test_native_* | wc -l` pour les suites, et le total de cas est la dernière
     ligne de `pio test -e native`. Repère au **2026-10-11 (v2.69.0) : 67 suites / 2 098 cas**.
     ⚠️ Le repère précédent (« 39 suites / 1 451 cas », 2026-09-07) a survécu **un mois** à
     côté de l'avertissement qui dit de ne pas le recopier. Celui-ci périmera aussi :
     **mesure, ne lis pas cette ligne.**
     Couverture : `tools/coverage.sh` (gcovr). CI GitHub Actions sur push/PR — **trois workflows** : `tests-natifs.yml` (tests + couverture), `build-firmware.yml` (**`pio run -e esp32dev_core3`** — la compilation est un gate depuis feature-103) et `hygiene-specs.yml` (**12 jobs mesurés au 2026-10-11** — en compter le nombre réel, ne pas recopier ce chiffre ; `ls tools/check_*.sh` en donne l'essentiel. Tous lançables en local avant de committer, dont : `./tools/check_specs.sh` unicité d'une spec entre `todo/` · `doing/` · `done/` · `./tools/check_doc_links.sh` liens relatifs des `.md` · `./tools/check_fopen_guard.sh` aucune ouverture de fichier hors du helper unique, ADR-0070 · `./tools/check_memory_discipline.sh` ADR-0082 · `./tools/check_removed_measurement_vocabulary.sh` feature-152 · les harnais de vecteurs JS, ADR-0099). Voir [`docs/BUILD.md`](docs/BUILD.md#intégration-continue-github-actions).
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
- **Mémoire** — trois règles apprises d'incidents réels (ADR-0082, feature-129/feature-130), pas une simple consigne de vigilance :
  1. **Le chemin de journalisation n'alloue pas.** Ni `String`, ni `new`, ni conteneur qui croît, dans `logger.{h,cpp}` / `log_arena_logic.{h,cpp}` — c'est l'acquis de feature-129, vérifié par `./tools/check_memory_discipline.sh` (CI : `hygiene-specs.yml`, job `memory-discipline`).
  2. **Toute allocation de plus de quelques kilooctets sur un chemin périodique doit être justifiée, ou diffusée.** C'est l'objet de feature-131.
  3. **La métrique qui compte est `max_alloc_8bit` (le plus gros bloc contigu du tas 8 bits), pas la mémoire libre.** Le projet s'est déjà trompé dessus : la mémoire libre peut monter pendant que la contiguïté s'effondre — c'est très exactement ce qui s'est produit pendant la fenêtre v2.51.0, et la cause racine du 15/08/2026 était une métrique heap qui mesurait la mauvaise chose (`MALLOC_CAP_INTERNAL`, IRAM 32 bits comprise). Une spec qui « surveille le heap » sans préciser *lequel* ne surveille rien.
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
| `ux-ui-designer` | Ergonomie et mise en page du frontend (`data/`) | **AVANT** `web-ui-developer` dès qu'une surface UI est créée, refondue, ou **qu'un écran reçoit son 3ᵉ ajout successif** (voir la règle d'accumulation ci-dessous) |
| `pool-chemistry` | Sécurité chimique, PID pH/ORP, limites de dosage | OBLIGATOIRE pour toute modif de `pump_controller.*`, régulation, calibration capteurs pH/ORP |
| `test-engineer` | Tests unitaires, mocks, tests d'intégration | Après implémentation, avant revue |
| `debugger` | Analyse logs/crash dumps, reproduction de bugs | Dès qu'un bug est rapporté ou crash ESP32 |
| `code-reviewer` | Revue critique sécurité/qualité/concurrence | Avant tout commit |
| `doc-writer` | README, `docs/`, CHANGELOG, commentaires | Quand le comportement visible change |

### ⚠️ La règle d'ACCUMULATION (`ux-ui-designer`)

**Constat du 2026-10-09, à l'origine de cette règle.** La page Produits est devenue illisible
— écran très haut, deux boutons par produit dont la distinction a fait trébucher l'utilisateur,
champs pleine largeur pour saisir quatre chiffres. Enquête faite : **aucune de ces décisions
n'était mauvaise.** `ux-ui-designer` avait même été coché dans les specs concernées. Deux
causes :

1. **Il ne figurait pas dans ce tableau** — sa seule mention vivait dans
   `specs/features/_template.md`, sous forme de case conditionnelle. Une session qui lit
   CLAUDE.md ignorait son existence. **Corrigé : il y est désormais.**
2. **Son mandat était à chaque fois limité à l'ajout du jour** (« refonte de vocabulaire et
   d'interaction » pour feature-143). Personne ne lui a jamais demandé : *à quoi ressemble
   cette page DANS SON ENSEMBLE ?*

**La règle qui en découle** : aucun ajout ne rend un écran trop chargé à lui seul. C'est leur
SOMME que personne ne regarde.

⇒ **Dès qu'un écran reçoit son 3ᵉ ajout successif** (champ, bouton, carte, ligne d'état),
`ux-ui-designer` est invoqué **sur l'ÉCRAN ENTIER**, pas sur l'ajout. Son brief doit dire
explicitement : *évalue la page complète, propose ce qui doit DISPARAÎTRE*.
⇒ Le compteur se lit dans l'historique git du fichier concerné, ou dans la doc
`docs/features/page-*.md` si elle liste les évolutions.
⇒ En cas de doute, invoquer : une étude d'ergonomie coûte une passe d'agent, une page dégradée
coûte une refonte.

### ⚠️ L'UX N'EST PAS OPTIONNEL — et son mandat NE SE DÉCOUPE PAS

**Règle posée par l'utilisateur le 2026-10-11 : « l'ux n'est pas optionnel quand on touche à
l'interface ».** Dès qu'un lot modifie `data/`, `ux-ui-designer` passe **avant**
`web-ui-developer`. Aucune dérogation pour « c'est petit », « c'est juste un ajout » ou
« la forme est évidente ».

**Origine, et c'est la nuance qui compte.** Sur la carte « Signaux capteurs » (feature-153),
`ux-ui-designer` **avait bien été invoqué**. Le défaut est venu d'un **mandat découpé** : on
lui avait demandé la densité, les suppressions, le sélecteur, le responsive et les messages —
**pas la palette**, alors que la carte passait de 2 à 3 courbes par graphique. Les couleurs ont
donc été choisies par défaut, et l'utilisateur a livré le verdict : *« les couleurs des données
brut et lissés sont tellement proches qu'on ne voit rien »*. C'est **le travers de la page
Produits une seconde fois**, appliqué cette fois au rendu au lieu de la structure.

⇒ **Son mandat n'est pas la liste des questions qu'on a pensé à lui poser.** Tout ce qui se
voit lui revient : **couleurs et palette, épaisseur et style de trait**, typographie, densité,
hiérarchie, états vides et d'erreur, cibles tactiles, contraste, **daltonisme**, mode sombre,
responsive. Tout brief qui énumère des points se termine par : *« et tout autre choix visuel
que ce lot impose — dis-le même si je ne l'ai pas demandé »*.
⇒ **Un défaut visuel qui remonte APRÈS livraison est le signe d'un mandat trop étroit**, pas
d'un agent défaillant. Chercher ce qu'on a oublié de lui confier, pas ce qu'il a raté.

### ⚠️ Le DESIGN SYSTEM est un document vivant, et l'`ux-ui-designer` en est le gardien

**Règle posée par l'utilisateur le 2026-10-14**, après avoir constaté sur son téléphone que les
cartes du tableau de bord ne se ressemblaient pas : *« l'agent UX est toujours incompétent. Il ne
produit pas un résultat cohérent entre les cartes. C'est quand même la base… »*

**Cause réelle, établie dans le code** : les cartes pH et ORP réemployaient `.segmented` — le
composant des **pages de réglage** — tel quel sur une carte d'accueil. Il porte un cadre, un fond
et une ombre destinés à un autre contexte, ses boutons inactifs sont transparents, il passe à la
ligne, et il n'a aucune marge haute. La carte Filtration, elle, utilise de vrais boutons en
grille. **Deux composants pour le même geste, sur le même écran.** Personne n'avait comparé les
deux cartes **rendues**.

⇒ **`docs/DESIGN_SYSTEM.md` est une documentation de référence, PAS une spec.** Il ne passe jamais
par `todo/ · doing/ · done/`, il n'a ni critères d'acceptation ni version cible : il se maintient
**en continu**, comme `docs/API.md` ou la documentation utilisateur. Chaque composant y déclare
**son contexte d'emploi** (carte d'accueil / page de réglage / les deux) — c'est la règle dont
l'absence a produit le défaut.

**L'`ux-ui-designer` en est le gardien** : il **part de lui** à chaque évolution d'interface, et il
**a le droit de le faire évoluer**. ⛔ Mais une évolution de composant **partagé** n'est jamais
locale. Elle oblige à **trois choses ensemble** :

1. **MESURER l'impact** — l'inventaire **exhaustif** des emplois (fichiers, sélecteurs, écrans),
   en comptes **réels**, jamais « quelques pages ».
2. **Garder l'ensemble cohérent** — dire ce que deviennent les emplois qui ne correspondent plus.
3. **Faire réaliser TOUS les impacts par les développeurs, dans le MÊME lot.**

⛔ **L'application partielle est interdite.** ⚠️ Un design system appliqué à moitié rend
l'interface **MOINS cohérente qu'avant** : il ajoute une troisième forme au lieu d'en retirer une.
Si un impact ne peut pas être traité dans le lot, **on l'écrit** — on ne le passe pas sous silence.

⚠️ **Et l'agent doit VOIR ce qu'il conçoit.** Le défaut du jour est né de ce qu'il lisait du HTML et
du CSS dans des fichiers séparés en imaginant le rendu. **`./tools/ui_shot.sh`** photographie
n'importe quelle page à n'importe quelle largeur, sans passer par l'appareil (`-f` force un champ
de `/data`, `-c` un champ de `/get-config`, pour atteindre un état rare).

⛔ **MAIS `ux-ui-designer` N'A PAS D'ACCÈS AU SHELL** (Read/Grep/Glob seulement) : il **ne peut pas**
lancer cet outil. ⇒ **C'est l'agent principal qui capture AVANT de l'invoquer**, et qui **joint les
chemins des PNG dans le brief** — l'agent sait les lire. Un brief d'ergonomie sur une surface
existante **sans images jointes** est un brief incomplet, et le résultat sera une déduction, pas une
observation. *(Constaté le 2026-10-14 : l'agent l'a signalé lui-même au lieu d'affirmer un rendu
qu'il n'avait pas vu — c'est le bon comportement, mais l'omission était la mienne.)*

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
- **L'ELF est attaché aux releases lui aussi** (~29 Mo). Ce n'est pas un confort :
  `esp-coredump` vérifie le **SHA256 de l'image applicative** et refuse tout ELF
  qui ne correspond pas exactement. Sans l'ELF de la version qui tournait, un
  coredump est **indécodable** — le projet en a déjà perdu un (15/08/2026).
- `builds/` sert d'archive **locale** (2,6 Go d'ELF horodatés, un par build) :
  local, jamais poussé. Ce qui doit survivre à un clone va dans les releases.

### Incidents

- **`docs/incidents/` est versionné** : les coredumps de production et leurs
  traces décodées (163 Ko). Un coredump n'existe **qu'une fois** — il est écrasé
  au crash suivant — et plusieurs specs de diagnostic s'appuient dessus.
- C'est l'exception assumée à la règle « pas de binaire dans le dépôt » : ces
  fichiers ne sont pas *générés*, ils sont **relevés**. Rien ne les régénère.
- Y verser tout nouveau coredump analysé, avec sa trace décodée, et le référencer
  depuis la spec concernée. Voir `docs/incidents/README.md` pour la procédure de
  décodage.

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
- ⚠️ **RESPONSIVE — règle permanente, non négociable.** L'interface doit fonctionner sur
  **téléphone, tablette ET ordinateur**. Règle posée par l'utilisateur le 2026-10-10.
  - **Le téléphone est l'écran de référence**, pas un cas dégradé : l'appareil se consulte au
    bord du bassin. Toute mise en page se conçoit **à 390 px D'ABORD**, puis s'élargit.
  - ⚠️ **Un `flex-wrap` n'est PAS un repli, c'est le cas NORMAL sur téléphone.** Une
    disposition qui « tient sur une ligne » en vue large et passe à la ligne en mobile **n'a
    pas été conçue pour mobile** — elle a été conçue pour desktop avec un rattrapage.
  - **Vérifier, pas affirmer** : pour tout libellé placé sur une ligne partagée, **compter les
    caractères** et dire si ça tient à 390 px. Un libellé trop long se **raccourcit** ; ce
    n'est pas un tabou.
  - **Origine de la règle** : refonte de la page Produits (2026-10-09). La maquette plaçait un
    champ et son bouton sur une même ligne, avec `flex-wrap` « en repli ». Sur téléphone le
    bouton passait **toujours** à la ligne en pleine largeur, « perdu entre les 2 champs »
    (mots de l'utilisateur). Le rapport d'ergonomie disait « mobile d'abord » — sans l'avoir
    vérifié, et **la règle n'était écrite nulle part**, donc non opposable.
- **Tests avant commit** : `pio run` doit passer, lint doit être propre
- **Toujours mettre à jour la spec** en fin de travail avec le statut réel

## Rappels Claude Code

- Lancer les commandes longues (build, flash) en background si possible
- Ne pas lire `node_modules/`, `.pio/`, `data-build/`
- En cas de doute sur une règle de sécurité chimique : **demander avant d'agir**
