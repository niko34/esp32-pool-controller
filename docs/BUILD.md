# Guide de compilation et déploiement

Guide **opérationnel** : quelles commandes lancer pour compiler et déployer. Pour la structure des partitions et le pourquoi du layout courant (**v5, flash 8 Mo**), voir [ADR-0068](adr/0068-partitions-layout-v5-flash-8-mo.md) et la source de vérité [`partitions.csv`](../partitions.csv).

## Compilation

### Firmware seul

```bash
pio run
```

Génère `.pio/build/esp32dev_core3/firmware.bin` (env par défaut depuis la v2.20.4 — voir ci-dessous).

### L'environnement firmware unique

`platformio.ini` déclare **un seul** environnement matériel. Depuis la **v2.20.4**, le core 3 est l'environnement **de production**, bootloader inclus ([ADR-0042](adr/0042-core3-environnement-par-defaut.md) — la porte de bascule laissée ouverte par [ADR-0034](adr/0034-migration-core3-idf5.md) est franchie) ; depuis la **v2.42.1**, il est le **seul** ([ADR-0069](adr/0069-retrait-repli-core-2.md)) :

| Env | Plateforme | Rôle |
|-----|-----------|------|
| `esp32dev_core3` *(défaut)* | pioarduino **release taguée `55.03.37`** (core Arduino 3.3.7 / IDF 5.5.0) | **Production** — `default_envs`, c'est le binaire buildé par `pio run` et déployé par `deploy.sh` (bootloader **et** application) |

```bash
pio run                              # core 3 (unique env matériel)
./deploy.sh all                      # déploiement complet
```

> ℹ️ **L'env de repli `[env:esp32dev]` (core Arduino 2.0.17 / IDF 4.4.7) a été retiré en v2.42.1.** Il n'était plus compilable depuis la v2.40.0 (appel `esp_cpu_get_cycle_count()` hors garde dans `oom_guard.cpp`, resté invisible deux versions parce que la CI ne construisait alors **aucun env matériel** — trou comblé depuis par [`build-firmware.yml`](#intégration-continue-github-actions)), plus flashable (bootloader core 3 depuis l'ADR-0042 ⇒ retour arrière = flash USB) et **aucun binaire core 2 n'avait jamais été archivé**. Voir [ADR-0069](adr/0069-retrait-repli-core-2.md). Avec lui disparaissent les 8 gardes `#if ESP_ARDUINO_VERSION_MAJOR >= 3` du code source : elles étaient réparties sur `pump_controller.cpp` **et** `pump_controller.h`, `main.cpp` et `oom_guard.cpp` (LEDC, watchdog, en-têtes mémoire). La garde `#if MBEDTLS_VERSION_NUMBER >= 0x03000000` de `ota_integrity.cpp` **reste** : elle est indexée sur la version d'une bibliothèque, pas sur l'environnement retiré.

> ⚠️ **La plateforme `espressif32@6.9.0` reste installée**, bien qu'aucun env ne l'utilise : elle fournit le paquet `tool-mklittlefs`, que [`build_fs.sh`](../build_fs.sh) référence en dur (`~/.platformio/packages/tool-mklittlefs/mklittlefs`) et que pioarduino 55.03.37 **ne déclare pas** (`"tool-mklittlefs": null` dans son `platform.json`). La désinstaller casserait `./build_fs.sh`, `./build_all.sh`, `./deploy.sh fs` et `./deploy.sh ota-fs`. Le ménage disque (~404 Mo) suppose d'abord de rendre `build_fs.sh` indépendant de cette plateforme.

> ⚠️ **Changer de bootloader exige un flash USB.** L'OTA n'écrit que l'application : une image core 3 démarre parfaitement sur un bootloader core 2 (montage qui a tourné plusieurs semaines en production). Le passage du **bootloader** en core 3 s'est donc fait par `./deploy.sh all` en USB, et le filet de repli d'autrefois (repointer `otadata` : `erase_region 0xE000 0x2000`) **n'existe plus** — voir [UPDATE_GUIDE.md](UPDATE_GUIDE.md#changer-de-bootloader--flash-usb-obligatoire-core-3-par-défaut-v2204).

#### `PIO_ENV` — sélection d'environnement pour tout l'outillage

Les scripts ne dépendent **plus** du `default_envs` de `platformio.ini` ni de chemins `.pio/build/esp32dev/` codés en dur : ils lisent une **variable unique `PIO_ENV`** (défaut `esp32dev_core3`, surchargeable) :

| Script | Usage de `PIO_ENV` |
|---|---|
| [`deploy.sh`](../deploy.sh) | `BUILD_DIR=".pio/build/$PIO_ENV"` ; `-e "$PIO_ENV"` passé explicitement à `pio run`, `pio run -t upload` et `pio device monitor`. Exporte `PIO_ENV` et `BUILD_DIR` pour `build_fs.sh` |
| [`build_fs.sh`](../build_fs.sh) | écrit `littlefs.bin` dans `$BUILD_DIR` (le FS est indépendant du core, mais il doit atterrir là où le déploiement le cherche) |
| [`ota_update.sh`](../ota_update.sh) | `FIRMWARE_PATH` / `FILESYSTEM_PATH` dérivés de `PIO_ENV` (toujours surchargeables individuellement) |
| [`build_all.sh`](../build_all.sh) | idem `deploy.sh` |
| [`tools/decode_coredump.sh`](../tools/decode_coredump.sh) | ELF par défaut dérivé de `PIO_ENV` |

**Pourquoi `decode_coredump.sh` suit `PIO_ENV`** — motif de sûreté, pas de confort : un ELF qui ne correspond pas au dump ne produit pas d'erreur franche, il produit une pile de **noms de fonctions plausibles et FAUX**. C'est exactement ce qui a rendu le coredump de production du 15/08 inexploitable (ELF core 2, dump core 3).

#### Pièges pioarduino

- Utiliser le **pio du penv** : `~/.platformio/penv/bin/pio` (le pio système Python 3.12 échoue avec `ModuleNotFoundError: fatfs/littlefs`).
- La plateforme est **épinglée sur la release taguée `55.03.37`** (2026-08-04) : pioarduino 55.3.37, core arduino-esp32 **3.3.7**, IDF 5.5.0 — les versions exactes validées au banc feature-063. Depuis la bascule, cet épinglage est un **contrat de production** : **toute montée de version est un choix explicite** exigeant une re-validation au banc avant flash — l'URL `stable`, utilisée avant l'épinglage, avait déjà dérivé vers 55.03.311 (core 3.3.11).

#### Marge flash — depuis le layout v5 (2026-09-05)

| Env | Occupation app0 (**3 735 552 o**) |
|---|---|
| `esp32dev_core3` *(production, env unique)* | **1 825 717 o = 48,9 %** — marge **1 909 835 o ≈ 1,82 Mo** |

> ℹ️ **Le plafond de vigilance de 95 % n'est plus une contrainte de conception.** Avant le
> layout v5, l'occupation était de **99,5 %** d'un slot de 1 835 008 o — soit **9 291 octets
> de marge**, moins que le coût d'une fonctionnalité moyenne. C'est ce qui imposait à chaque
> lot d'« arriver avec son propre poste de récupération flash ». La table de partitions
> couvrant désormais les 8 Mo réellement présents sur la puce
> ([ADR-0068](adr/0068-partitions-layout-v5-flash-8-mo.md)), cette contrainte disparaît :
> **aucun arbitrage de conception ne doit plus invoquer la place flash sans remesurer d'abord.**

> ℹ️ **Cette occupation est vérifiée à chaque push** : depuis le 2026-09-06, [`build-firmware.yml`](../.github/workflows/build-firmware.yml) compile l'env `esp32dev_core3` en CI et `pio run` y affiche RAM / Flash — voir [Intégration continue](#intégration-continue-github-actions).

> ℹ️ **Cible matérielle** : le BOM du PCB v2 spécifie un **ESP32-DevKitC-VE**
> (module WROVER-E, **8 Mo de PSRAM**) — voir la section PSRAM ci-dessous. L'env
> produit un **binaire unique** qui tourne sur WROVER-E comme sur WROOM-32 ;
> la PSRAM est requise pour la fiabilité de la mise à jour en ligne (TLS GitHub).
> L'**ESP32-S3 n'est pas supporté** (architecture Xtensa LX7 : cet env ne
> produit pas de binaire S3, brochage incompatible avec le PCB v2).

### Support PSRAM — module WROVER-E (feature-066, v2.18.0)

L'env `esp32dev_core3` porte depuis v2.18.0 les flags PSRAM (ils étaient alors présents sur les deux envs) :

```ini
-DBOARD_HAS_PSRAM
-mfix-esp32-psram-cache-issue
-mfix-esp32-psram-cache-strategy=memw
```

- `-DBOARD_HAS_PSRAM` : active l'init PSRAM au démarrage (avant `setup()`). Sur un module **sans** PSRAM (WROOM), l'init échoue **proprement** et le firmware continue à l'identique — **binaire unique** WROOM/WROVER, dégradation gracieuse validée au banc.
- Les deux `-mfix-esp32-psram-cache-issue` / `-mfix-esp32-psram-cache-strategy=memw` sont **obligatoires avec `BOARD_HAS_PSRAM`** : ils alignent la génération de code de **nos** sources sur les **libs précompilées** du core, qui sont buildées avec le workaround silicium PSRAM (`CONFIG_SPIRAM_CACHE_WORKAROUND=y`, stratégie MEMW) — même combinaison que les cartes WROVER officielles. Sans eux, mélange de code compilé avec/sans workaround → corruptions aléatoires.
- Aucun placement explicite d'allocations : `SPIRAM_USE_MALLOC` (précompilé, seuil 4096) envoie **automatiquement** les allocations > 4 Ko en PSRAM quand elle est présente. C'est le levier qui restaure le TLS sortant sous core 3 (voir [ADR-0034](adr/0034-migration-core3-idf5.md), note du 2026-08-08).
- Visibilité runtime : log de boot (« PSRAM : X Mo détectés » / « PSRAM absente ») + champs `psram_size` / `psram_free` de `/get-system-info` ([API.md](API.md)).

### Filesystem (LittleFS) seul

```bash
./build_fs.sh
```

Ce script :
1. Minifie HTML / CSS / JS **puis pré-compresse en gzip** (via `minify.js`, sortie dans `data-build/`, ignoré par git) ;
2. Construit `littlefs.bin` avec la **taille exacte de la partition `spiffs`** (**524 288 octets = 512 KB**, layout v5 — voir [ADR-0068](adr/0068-partitions-layout-v5-flash-8-mo.md)).

⚠️ **Ne pas utiliser `pio run -t buildfs`** : PlatformIO recalcule une taille incorrecte (128 KB), ce qui produit un `littlefs.bin` non conforme à la partition `spiffs`.

### Pré-compression gzip des assets (feature-048, v2.12.1)

Après minification, `minify.js` gzippe (zlib **niveau 9**) chaque fichier texte (`.js`, `.html`, `.css`) et n'écrit dans `data-build/` **que la variante `.gz`** (ex. `app.js` → `app.js.gz`). Les images et icônes sont copiées telles quelles, sans compression. ESPAsyncWebServer résout nativement `<fichier>.gz` et sert avec `Content-Encoding: gzip` — aucun changement firmware requis (voir [subsystems/web-server.md](subsystems/web-server.md#assets-statiques-pré-compressés-gzip)).

**Gain mesuré** : payload FS **443 → 155 KB** (occupation partition `spiffs` 78 % → 27 % du layout v3 de l'époque ; ≈48 % de la partition 320 KB du layout v4), chargement des pages ~4× plus rapide (le débit de l'ESP32 est le facteur limitant sur mobile). Ce gain est ce qui a rendu possible la réduction de `spiffs` à 320 KB au profit des slots app ([ADR-0024](adr/0024-partitions-layout-v4.md)).

> Depuis le layout v5, `spiffs` fait **512 KB** : **221 184 o utilisés, 303 104 o libres**. Le pipeline gzip reste la règle (voir ci-dessous) mais il n'est plus une **condition d'existence** du frontend — la contrainte « frontend limité à 320 KB » de l'ADR-0024 est levée.

Règles :
- ⚠️ **Ne jamais placer de fichier texte non compressé dans `data-build/`** : le dossier est entièrement régénéré par `minify.js` à chaque build — tout ajout d'asset texte se fait dans `data/`, le pipeline produit le `.gz`.
- **Caveat `curl`** : un `curl` brut sur un fichier statique reçoit l'octet-stream gzip (le navigateur, lui, décompresse de façon transparente). Utiliser `curl --compressed http://<ip>/app.js`. Les routes API/JSON ne sont **pas** concernées (réponses générées, jamais compressées).

### Tout en une commande

```bash
./build_all.sh                       # env par défaut (esp32dev_core3)
```

Enchaîne `pio run -e "$PIO_ENV"` puis `build_fs.sh` (qui écrit le `littlefs.bin` dans `.pio/build/$PIO_ENV/`).

## Tests natifs (hors matériel)

L'environnement `native` (`platform = native` dans [`platformio.ini`](../platformio.ini)) compile et exécute la **logique pure** du firmware sur le PC, sans ESP32. Il ne se lance qu'explicitement :

```bash
pio test -e native
```

PlatformIO compile **un binaire par dossier `test/test_*`**. Le nombre de suites a beaucoup augmenté depuis la rédaction de cette section : **`ls test/` fait foi** — la table ci-dessous n'est qu'un extrait des suites historiques et de la plus récente.

| Dossier | Couvre | Source testée |
|---------|--------|---------------|
| `test/test_native_sensor_filter/` | tendance centrale (médiane longue + EMA), rejet aberrations, dispersion/latch, capteur figé (feature-025 → feature-058) | `src/sensor_filter.cpp` |
| `test/test_native_dosing/` | décision de dosage (`evaluateDose`, hystérésis start/stop, non-régression pause-mélange) (feature-036) | `src/dosing_logic.cpp` |
| `test/test_native_ph_integral/` | terme intégral pH (v2.26.0, [ADR-0052](adr/0052-terme-integral-ph-dimensionne-par-autorite.md)) : charge signée, oubli, reset sec, péremption, hystérésis de saturation, **test de droop** (un P pur ne converge pas, un PI converge) | `src/ph_integral_logic.h` |

> ⚠️ **Section partiellement périmée** : le reste de ce paragraphe (« les deux modules purs », `build_src_filter`) décrit un état antérieur du projet. Se référer à [`platformio.ini`](../platformio.ini) pour le filtre de sources réel.

Le `build_src_filter` de l'env `native` inclut les deux modules purs :

```ini
build_src_filter = +<sensor_filter.cpp> +<dosing_logic.cpp>
```

Ces deux sources ne dépendent **ni d'Arduino, ni de FreeRTOS, ni d'I²C** ; seul un shim minimal (`test/native_shim/`) fournit `NAN` / `isnan` / types entiers. Voir [ADR-0017](adr/0017-logique-metier-pure-humble-object-testabilite.md) pour la convention « logique pure séparée de la couche hardware ».

### Couverture de tests

Le script [`tools/coverage.sh`](../tools/coverage.sh) mesure la couverture de **tous** les modules purs compilés par `[env:native]`, via l'env dédié `native_coverage` (env `native` + instrumentation `--coverage`) et **gcovr** :

```bash
./tools/coverage.sh          # résumé console (lignes + branches) + rapport HTML
./tools/coverage.sh --open   # idem + ouvre coverage/index.html (macOS)
```

Prérequis : `gcovr` dans le venv PlatformIO (`~/.platformio/penv/bin/pip install gcovr`) et `gcov` (`/usr/bin/gcov` par défaut, surchargeable : `GCOV=llvm-cov-gcov ./tools/coverage.sh`). Le rapport HTML est généré dans `coverage/` (ignoré par git).

#### Périmètre : `src/`, et non une liste de modules nommés

Jusqu'au 2026-09-06, le filtre `gcovr` **nommait** les modules mesurés (`sensor_filter`, `dosing_logic` en CI ; quatre modules en local — les deux périmètres divergeaient déjà entre eux). Un filtre qui nomme ses modules **ne surveille que ce qu'on a pensé à lui nommer** : un module pur ajouté au `build_src_filter` sans test n'apparaissait nulle part et **ne faisait pas bouger le chiffre**. C'est le même angle mort que la CI qui ne compilait aucun firmware (voir [Pourquoi la compilation est devenue un gate](#pourquoi-la-compilation-est-devenue-un-gate)) : un contrôle dont le périmètre est une liste manuelle finit par mentir par omission.

Le filtre est donc `--filter 'src/'`, **à l'identique** dans [`tools/coverage.sh`](../tools/coverage.sh) et dans [`tests-natifs.yml`](../.github/workflows/tests-natifs.yml). Ces deux filtres doivent rester identiques : sinon le chiffre local et le chiffre CI divergent — et c'est le local qu'on croit.

Le script local applique **la même garde de seuil que la CI** (`--fail-under-line 90`). Sans elle, il affichait un chiffre sans jamais échouer, et une chute sous le seuil ne se découvrait qu'**après** le push. Le rapport HTML est produit **même en cas d'échec** : le code de sortie de `gcovr` est retenu, puis rendu en fin de script.

#### Référence de couverture (2026-09-06, feature-105)

| Indicateur | Valeur |
|---|---|
| Modules mesurés | **34** |
| Lignes | **100 % (1810/1810)** |
| Fonctions | **100 % (161/161)** |
| Branches | **86,8 %** |
| Suites (`test/test_*`) | ⚠️ **à mesurer** — `ls -d test/test_native_* \| wc -l` |
| Cas de test (`pio test -e native`) | ⚠️ **à mesurer** — dernière ligne de `pio test -e native` |

> ⚠️ **NE DÉDUIS AUCUN COMPTEUR DE CE TABLEAU** ([ADR-0073](adr/0073-chiffre-consigne-porte-sa-methode.md)).
> Les deux lignes de compteurs ont été **retirées** le 2026-09-07 plutôt que corrigées : elles
> annonçaient « 34 suites / 1 325 cas » pour **39 / 1 458** réels, et elles avaient déjà été
> fausses une fois. Un compteur recopié ici pourrit à chaque livraison — deux agents distincts
> ont d'ailleurs cru le lire dans `CLAUDE.md`, qui ne le contient pas. **Mesure, ne recopie
> pas.** Les colonnes de couverture, elles, restent celles de la mesure du 06/09 (`tools/coverage.sh`).
>
> Repère daté, **non normatif** : au 2026-09-07 (v2.47.1), **39 suites / 1 458 cas**, couverture
> lignes et fonctions 100 %.
>
> Deux suites structurantes, **sans compteur** — leur contenu bouge, `pio test` fait foi :
> [`test_native_dose_composition/`](../test/test_native_dose_composition/) (`doseComposition`,
> `phFeedforwardLearnedFlow`, `phFeedforwardServedStatus` ; validée par mutation) et
> [`test_native_ws_data_parity/`](../test/test_native_ws_data_parity/) (sentinelle structurelle du
> second axe de parité, [ADR-0071](adr/0071-parite-data-trame-capteurs.md) ; deux axes depuis
> `bug-082` — premier niveau et receveur imbriqué `faObj`).
>
> 🪞 **Cette note a elle-même porté deux compteurs périmés** (« 38 dossiers » pour 39, « 9 cas »
> pour 16), écrits le 2026-09-07 **dans le paragraphe qui interdit de recopier des compteurs**, et
> relevés en revue le même jour. C'est le meilleur argument pour ADR-0073 : l'erreur ne vient pas
> de la négligence, elle vient de ce qu'un chiffre est **plus court à écrire qu'une méthode**.
> D'où le choix, ici, de citer les suites **sans les compter**.

Référence d'avant le cycle, mesurée sur **2 modules seulement** : lignes 99,8 %, branches 86,5 %. L'élargissement du filtre a révélé 4 lignes non couvertes jusque-là **invisibles**, dont une garde de sécurité chimique (`pulse_dosing_logic`).

Le seuil de 90 % de lignes est un **plancher de non-régression**, pas un objectif. **Aucun seuil de branches n'est posé** : les 13,2 % restants sont des demi-branches défensives, dont plusieurs sont inatteignables par entrée réaliste (motifs écrits dans les tests concernés). Les relever serait une décision séparée.

> **Portée** : seules les **classes pures** sont mesurées. La coquille `pump_controller.cpp` (collecte des globals + mapping énum→chaîne) est **exclue du build natif** — son équivalence est validée par revue, pas par couverture. Voir [ADR-0017](adr/0017-logique-metier-pure-humble-object-testabilite.md).

> **Deuxième limite assumée (2026-09-06, `bug-100` cycle 2)** : **trois mutants de `history.cpp` ne sont pas tués** — le tripwire de boot, le tripwire de `loadFromFile()` et la seconde admission de `saveToFile()`. Ils vivent dans la **coquille**, hors du `build_src_filter` natif : c'est la frontière de l'[ADR-0017](adr/0017-logique-metier-pure-humble-object-testabilite.md), pas un oubli. Les **prédicats** qu'ils invoquent sont couverts et tuent leurs mutants. Lever la dette exigerait un double de `fs::FS` en natif, ou l'extraction des trois séquences en fonctions pures — voir [`docs/subsystems/acces-fichiers.md` § Dette de test assumée](subsystems/acces-fichiers.md#-dette-de-test-assumée--trois-mutants-non-tués).

> **Limite assumée et datée (2026-09-06, feature-105)** : le chemin d'échec de `ledcAttachChannel` (`pump_controller.cpp`, qui pose `pumpUsable[i] = false`) reste **non mesuré et non testé — c'est un choix**, pas un constat ouvert. Le tester exigerait d'introduire un point de couture dans un fichier de sécurité chimique, au seul bénéfice d'une branche de deux lignes déjà relue par `pool-chemistry` **et** par `code-reviewer` (feature-102) ; `pool-chemistry` l'a qualifié de recommandation **non bloquante**. S'il devait être levé, la voie serait l'extraction d'un module pur au sens de l'[ADR-0017](adr/0017-logique-metier-pure-humble-object-testabilite.md), dans une spec dédiée — pas un ajout de test opportuniste.

### Validation on-target (manuelle)

Les tests natifs ne couvrent que la **logique pure**. L'**intégration matériel + réseau** (I²C, PWM, RTC, LittleFS, WiFi, WebSocket, MQTT) et les **scénarios complets de régulation** se valident à la main sur la carte, via la check-list [`docs/test-on-target.md`](test-on-target.md) — à dérouler après un `./deploy.sh` touchant la régulation, la filtration, les capteurs ou la persistance.

## Intégration continue (GitHub Actions)

**Trois** workflows, **trois signaux distincts**. Ils sont séparés à dessein : un échec de compilation, un échec de test et une incohérence de specs ne se diagnostiquent pas de la même façon, et les réunir sous un nom unique rendrait le résultat ambigu.

| Workflow | Ce qu'il garde | Déclencheurs | Coût |
|---|---|---|---|
| [`tests-natifs.yml`](../.github/workflows/tests-natifs.yml) | `pio test -e native`, puis couverture `gcovr` avec seuil **lignes ≥ 90 %** sur **tout `src/`** (filtre identique à `tools/coverage.sh`, cf. [Couverture de tests](#couverture-de-tests)) ; rapport HTML publié en artefact | push `main` + PR, **sans** filtre de chemins | env `native` seul (quelques Mo) |
| [`build-firmware.yml`](../.github/workflows/build-firmware.yml) *(feature-103, 2026-09-06)* | **`pio run -e esp32dev_core3`** — la compilation du firmware. Étape informative supplémentaire : sections du binaire (`xtensa-esp32-elf-size`), `continue-on-error` | push `main` + PR, **avec** filtre de chemins (voir plus bas) | toolchain pioarduino ≈ **2 Go**, mise en cache |
| [`hygiene-specs.yml`](../.github/workflows/hygiene-specs.yml) — **« Hygiène du dépôt »** *(feature-103, 2026-09-06 ; renommé et élargi par `bug-100` cycle 2, puis par feature-128 et feature-130)* | **Six jobs indépendants** — le nombre ne se recopie pas d'une revue à l'autre ([ADR-0073](adr/0073-chiffre-consigne-porte-sa-methode.md)), il se mesure : `grep -cE '^  [a-z0-9-]+:$' .github/workflows/hygiene-specs.yml` (ce comptage inclut aussi la clé `push:` de la section `on:` — vérifier à l'œil). État au 19/09/2026 : `unicite` → [`tools/check_specs.sh`](../tools/check_specs.sh) (un nom de spec ne vit que dans **un seul** des dossiers `todo/` · `doing/` · `done/`) ; `liens-doc` → [`tools/check_doc_links.sh`](../tools/check_doc_links.sh) (tout lien relatif de la doc résout — voir l'encadré ci-dessous) ; `fopen-guard` → [`tools/check_fopen_guard.sh`](../tools/check_fopen_guard.sh) (aucun fichier de `src/` autre que `fs_guard.cpp` n'appelle `open(` / `exists(` / `openNextFile(`, [ADR-0070](adr/0070-fopen-interne-helper-unique-et-classes-de-contention.md)) ; `no-peek-flag` → [`tools/check_no_peek_flag.sh`](../tools/check_no_peek_flag.sh) (`POOL_HEAP_PEEK` absent de tout fichier versionné hors `docs/`/`specs/`/`*.md`, [ADR-0081](adr/0081-recensement-du-tas-et-non-livraison-de-la-lecture-de-contenu.md)) ; `heap-routes-guard` → [`tools/check_heap_routes_guard.sh`](../tools/check_heap_routes_guard.sh) (la garde d'authentification est la **première** instruction de chaque route du tas, ADR-0081) ; `memory-discipline` → [`tools/check_memory_discipline.sh`](../tools/check_memory_discipline.sh) (la discipline mémoire du journal établie par feature-129 n'est pas contournée, [ADR-0082](adr/0082-arene-circulaire-du-journal-en-ram-interne-statique.md) — détail ci-dessous) | push `main` + PR, **sans** filtre de chemins | ~1 s chacun, ni Python, ni PlatformIO, ni cache |

> ⚠️ **Pourquoi un contrôle de liens** *(ajouté le 2026-09-06)*
>
> Le chemin d'un lien vers une spec **encode son état** dans le cycle :
> `specs/features/doing/…`. Or cet état change **par conception** — `todo` →
> `doing` → `done`. Chaque promotion casse donc, **silencieusement**, tous les
> liens qui citaient l'ancien emplacement. Relevé du jour : **27 liens morts sur
> 94** vers des specs (29 %), **tous** pointant vers `todo/` ou `doing/` pour des
> specs passées en `done/`. Ce n'étaient pas des fautes de frappe : c'étaient des
> liens **justes le jour où ils ont été écrits**.
>
> `check_specs.sh` garantit qu'une spec ne vit pas dans deux dossiers ; il ne dit
> rien des liens qui la citent. Le registre écrit doit rester **univoque** *et*
> **navigable**.
>
> Le script porte **trois exclusions**, sans lesquelles il serait bruyant donc
> ignoré : les blocs de code clôturés (`](AsyncWebServerRequest*)` en C++ n'est
> pas un lien), les segments entre backticks (ce document décrit la convention
> avec des exemples), et le suffixe `:123` — c'est un **numéro de ligne**, pas une
> partie du chemin.
>
> Lançable en local : `./tools/check_doc_links.sh` (0 si tout résout, 1 sinon).

Les trois ont un groupe de `concurrency` propre avec `cancel-in-progress` : un nouveau push annule le run précédent sur la même réf.

### Pourquoi la compilation est devenue un gate

Jusqu'au 2026-09-06, **aucun environnement matériel n'était bâti en CI**. « Le firmware ne compile plus » était donc un défaut que **rien ne signalait** : c'est ainsi que la casse de l'ancien env de repli core 2 est passée inaperçue pendant **deux versions** (v2.40.0 → v2.42.0), découverte par hasard. Le retrait de ce repli ([ADR-0069](adr/0069-retrait-repli-core-2.md)) a supprimé **une victime**, pas la **classe de défaut** — elle valait pour `esp32dev_core3`, seul env restant, et vaudrait pour tout successeur.

> ℹ️ Le build CI exécute `tools/archive_build.py` (hook `post:` de l'env) : il écrit dans `builds/` **sur le runner**, qui est détruit ensuite. Ce n'est pas une anomalie.

### `./tools/check_specs.sh` — le même contrôle en local

Le contrôle d'hygiène vit dans un **script du dépôt**, pas dans du shell inline : il tourne à l'identique en local, avant de committer, et peut servir de hook pre-commit.

```bash
./tools/check_specs.sh    # sortie 0 si tout va bien, 1 sinon
```

Deux règles, de sévérité différente :

1. **BLOQUANTE (sortie 1)** — un nom de fichier de spec présent dans plusieurs des trois dossiers. Le script liste les noms fautifs **et** leurs dossiers. Le cycle `todo/` → `doing/` → `done/` procède par **déplacement** (`git mv`) : une spec copiée, ce sont deux versions qui divergent et deux statuts qui se contredisent.
2. **AVERTISSEMENT (n'échoue pas)** — un même **numéro** porté par deux slugs différents. Non bloquant à dessein : corriger peut exiger de **renuméroter une spec close**, donc de réécrire de l'historique.

### `./tools/check_fopen_guard.sh` — le point d'admission unique reste unique

```bash
./tools/check_fopen_guard.sh    # sortie 0 si conforme, 1 sinon
```

Échoue si un `.cpp`/`.h` de `src/` **autre que `fs_guard.cpp`** appelle `open(`,
`exists(` ou `openNextFile(` — les trois construisent un `VFSFileImpl`, donc un
`fopen` complet, verrou newlib compris. Sur ESP32, l'échec du `malloc` de ce
verrou **aborte sans code de retour** : deux crashs de production viennent de là.

⛔ **Deux trous que ce contrôle ne peut pas fermer**, documentés dans l'entête du
script plutôt que passés sous silence :

1. `grep` travaille **ligne à ligne** — une coupure après le `->` ou le `.`
   (formatage d'un appel long) échappe au motif ;
2. toute API du framework construisant un `VFSFileImpl` **sous un autre nom**
   passe.

C'est pourquoi le recensement de
[`docs/subsystems/acces-fichiers.md`](subsystems/acces-fichiers.md) reste tenu
**à la main**. Le script tient l'**ordre** ; le document tient la **liste**.

> Premier cas réel, signalé par le script dès sa mise en service : `feature-067` désignait à la fois `todo/feature-067-etude-portage-esp32-s3.md` et `done/feature-067-offset-mesure-ph.md`. **Résolu par `feature-105`** (2026-09-06) : c'est l'**étude** qui a été renumérotée en `feature-104`, avec une bannière expliquant le motif — rien ne la référençait, alors que `feature-067-offset-mesure-ph` est citée par `CHANGELOG.md`, `data/app.js`, `src/main.cpp`, `src/constants.h` et `src/web_routes_debug.cpp`. Le script sort désormais **0 sans avertissement**.

Cas limites tranchés : dossier `todo/`/`doing/`/`done/` vide ou absent → **sortie 0** avec mention du dossier ignoré (un cycle sans spec en cours est légitime) ; `specs/features/` **entièrement absent** ou `mktemp` en échec → **sortie 1**, jamais 0 — un contrôle qui ne peut pas conclure ne doit pas être vert.

### `./tools/check_memory_discipline.sh` — la discipline mémoire du journal reste vraie (ADR-0082, feature-130)

```bash
./tools/check_memory_discipline.sh    # sortie 0 si conforme, 1 sinon
```

feature-129 a remplacé le `std::vector<LogEntry>` de `String` du journal —
168 octets alloués sur une seule ligne ont détruit 23 552 octets de
contiguïté du tas 8 bits à l'effondrement initial — par une arène circulaire
`.bss` de taille fixe. Rien ne garantissait que cet acquis reste vrai : un
`std::vector<String>` réintroduit demain dans `logger.h` compile, passe
`pio test -e native`, et ne rencontre aucun contrôle. Ce script ferme cette
brèche.

Motifs refusés :

1. **M1** — `String` **membre d'un tableau** (`String foo[N]`), sur tout
   `src/` : le motif exact de `_lastFilterPub` (feature-025).
2. **M2** — conteneur STL paramétré par `String` (`vector<String>`,
   `map<String, ...>`, etc.), sur tout `src/` : le motif exact de
   `_persistBuffer`/`toWrite`, retirés par ADR-0082.
3. **M3A** — toute déclaration `String` dans le chemin de journalisation
   lui-même (`logger.h`, `logger.cpp`, `log_arena_logic.h`,
   `log_arena_logic.cpp`) : plus strict que M1/M2, parce qu'aucun `String`
   n'y est légitime, même hors tableau ou conteneur.
4. **M3B** — toute allocation dynamique explicite (`new`, `malloc`/`calloc`/
   `realloc`/`strdup`, `std::vector`, `std::string`, `JsonDocument`) dans ces
   quatre mêmes fichiers.

Les lignes de commentaire sont exclues du filtrage. `grep -H` est
**obligatoire** (même piège que `check_fopen_guard.sh`) : sans lui, le
préfixe `fichier:` disparaît dès qu'un lot ne touche qu'un seul fichier, et
l'échappatoire ci-dessous devient inopérante en silence.

**Mode audit** — `POOL_MEM_LIST=1 ./tools/check_memory_discipline.sh` liste
les occurrences brutes, sans filtrer les exemptions. Indispensable pour
valider le script sur l'historique (`git show HEAD~n:...`), où les
commentaires d'exemption n'existent évidemment pas — c'est ainsi qu'il a été
vérifié qu'il aurait attrapé `_persistBuffer` et `toWrite` sur `HEAD~3`
(avant le commit qui a livré l'arène circulaire).

**Échappatoire** — un site hors chemin de journalisation peut être
légitime (borné, purgé périodiquement). Sur la ligne du hit ou celle juste
au-dessus :

```cpp
// MEM-EXEMPT(ADR-0082) : <motif d'au moins 20 caractères non blancs> [feature-131]
```

Le motif est **obligatoire** — un marqueur nu ne désarme rien : la partie
après le `:` doit compter au moins 20 caractères non blancs. Pas
d'exemption par nom de fichier entier (contrairement à
`check_fopen_guard.sh`, qui exempte `fs_guard.cpp` intégralement) : exempter
un fichier comme `mqtt_manager.h` par son nom aveuglerait des milliers de
lignes sans rapport avec le motif ponctuel qui le justifie.

⛔ **Ce que ce script ne ferme pas**, documenté en tête du script plutôt que
passé sous silence :

1. `grep` travaille **ligne à ligne** — une déclaration éclatée sur
   plusieurs lignes (formatage automatique d'un type long) échappe aux
   quatre motifs.
2. Il ne prouve **aucune** absence d'allocation dynamique en général — un
   `heap_caps_malloc(...)` légitime hors chemin de journalisation n'est pas
   son objet. Il ferme quatre motifs précis (feature-129/feature-130), pas
   la discipline mémoire entière (c'est l'objet de feature-131).
3. L'échappatoire `MEM-EXEMPT` protège de l'**inadvertance**, pas d'un
   contournement délibéré : quiconque peut committer peut poser un motif de
   complaisance — reste examiné par `code-reviewer`.

### ⚠️ Clé de cache cloisonnée `pio-fw-` — à ne pas « simplifier »

`build-firmware.yml` utilise la clé `pio-fw-${{ runner.os }}-${{ hashFiles('platformio.ini') }}`, **distincte** du `pio-…` de `tests-natifs.yml`. Ce n'est pas cosmétique.

Une clé de cache GitHub Actions est **immuable une fois créée**. Les deux workflows démarrent **en parallèle sur le même commit** : avec une clé partagée, si le job natif gagne la course, il écrit sous cette clé un cache ne contenant que l'env `native` (quelques Mo). Le job firmware le restaurerait, retéléchargerait ~2 Go de toolchain (`framework-arduinoespressif32-libs` 1,9 Go + `toolchain-xtensa-esp-elf` 1,2 Go), et **ne pourrait jamais sauvegarder le sien**, la clé étant prise. Résultat : 2 Go à chaque run, indéfiniment — et **silencieusement**, puisque le run reste vert.

La clé reste indexée sur `platformio.ini` : une montée de plateforme invalide le cache d'elle-même, ce qui est le comportement voulu.

### ⚠️ Contrepartie du filtre de chemins

`build-firmware.yml` ne se déclenche que sur `src/**`, `platformio.ini`, `partitions.csv`, `tools/archive_build.py` (script `post:` de l'env, donc véritable entrée de build) et **le workflow lui-même**. Un commit purement documentaire ne consomme donc pas plusieurs minutes de runner.

Le prix à connaître **avant** de le payer : si ce job est un jour déclaré ***required check*** sur les PR, une PR ne touchant **aucun** de ces chemins n'obtiendra **jamais** le statut attendu et restera **non fusionnable** — sans message explicite, la PR reste simplement bloquée sur un contrôle « en attente ». Les parades usuelles sont soit de ne pas le rendre requis, soit d'ajouter un job « bidon » toujours vert portant le même nom sur le chemin complémentaire. À trancher le jour où la protection de branche sera activée, pas avant.

### ⚠️ Pas d'ancre YAML dans GitHub Actions

Les ancres YAML (`&ancre` / `*ancre`) **ne sont pas supportées** par GitHub Actions. La liste de chemins de `build-firmware.yml` est donc **dupliquée littéralement** entre le bloc `push` et le bloc `pull_request` : **toute modification doit être faite deux fois**, sous peine d'un filtre asymétrique entre push et PR (un défaut discret — le workflow se déclencherait dans un cas et pas dans l'autre).

## Upload / Déploiement

Le script [`deploy.sh`](../deploy.sh) est l'entrée unique pour tous les modes de déploiement. Lancer sans argument pour l'aide :

```bash
./deploy.sh
```

Il builde et flashe l'environnement désigné par `PIO_ENV` (défaut `esp32dev_core3`, et désormais seul env matériel) — voir [`PIO_ENV`](#pio_env--sélection-denvironnement-pour-tout-loutillage).

### Modes USB (câble série)

| Commande | Action |
|----------|--------|
| `./deploy.sh firmware` | Compile + upload firmware uniquement |
| `./deploy.sh fs` | Build + upload filesystem uniquement |
| `./deploy.sh all` *(défaut)* | Firmware + filesystem |
| `./deploy.sh factory` | Efface toute la flash, upload complet, puis **ré-efface la NVS en dernier**. **Génère un nouveau mot de passe WiFi AP**, affiché dans le moniteur enchaîné |

#### Moniteur série enchaîné

Les **quatre** modes USB lancent `pio device monitor` à la fin de l'upload. Ce n'est pas un confort : l'ouverture du port bascule **DTR/RTS**, ce qui **redémarre l'ESP32** — le boot est donc capté **depuis sa première ligne**. Un moniteur lancé à la main dans un second terminal arrive toujours trop tard et manque les lignes qui ne sont émises qu'une fois (mot de passe AP, cause de reset, init des sondes).

```bash
NO_MONITOR=1 ./deploy.sh all    # ne pas enchaîner le moniteur (script non interactif, CI)
```

Sortir par `Ctrl+C` n'est **pas** une erreur de déploiement (le code de retour 130 est absorbé).

#### Port série détecté, pas codé en dur

Le port n'est plus figé sur `/dev/cu.usbserial-0001` : l'upload échouait dès qu'on branchait un autre module (un adaptateur neuf n'énumère pas sous le même nom). Ordre de résolution :

1. variable d'environnement `PORT` ;
2. `/dev/cu.usbserial-0001` s'il existe (port historique) ;
3. premier port `usbserial` / `wchusbserial` / `SLAB_USBtoUART` détecté.

```bash
PORT=/dev/cu.usbserial-10 ./deploy.sh all    # forcer le port
```

Si aucun port n'est trouvé, `deploy.sh` **s'arrête avant toute compilation** en listant les `/dev/cu.*` disponibles — plutôt que d'échouer après 2 min de build.

> ⚠️ `pio run -t upload` reçoit désormais explicitement `--upload-port "$PORT"`. Sans ce paramètre, PlatformIO retombe sur l'`upload_port` de [`platformio.ini`](../platformio.ini) et **ignore le port détecté** : l'upload échoue alors que le module est bien branché, ailleurs.

#### Ordre du mode `factory` (et pourquoi il compte)

`factory` efface la flash, uploade firmware + filesystem, **puis ré-efface la seule partition `nvs`** (`0x9000` / `0x5000`) avec `--after no_reset`, avant d'enchaîner le moniteur.

La raison : `upload_firmware` et `upload_filesystem` **redémarrent la puce**. Avec l'effacement NVS placé en tête (comportement précédent), le boot qui générait et affichait le mot de passe AP avait donc déjà eu lieu — sans personne pour le lire. Or [`auth.cpp`](../src/auth.cpp) ne journalise ce mot de passe qu'à sa **génération** (condition `apPassword.isEmpty()`) ; ensuite, seul le démarrage de l'AP le réaffiche, et uniquement tant que l'assistant n'est pas complété ([`main.cpp`](../src/main.cpp), `isFirstBootDetected()`). Le `--after no_reset` laisse le reset au moniteur, qui capte donc les lignes :

```
=== MOT DE PASSE AP WIFI: XXXXXXXX ===
```

C'est la **seule occasion de le lire** — le noter sur une étiquette. Voir [UPDATE_GUIDE.md](UPDATE_GUIDE.md#récupérer-le-mot-de-passe-wifi-ap).

#### Compatibilité esptool 4 / 5

Le paquet `tool-esptoolpy` est **partagé entre plateformes PlatformIO** et chacune impose sa version. Tant que le repli core 2 existait, elle basculait entre 4.5.1 et 5.1.2 au gré du dernier build ; avec un env unique, c'est la **5.1.2** de pioarduino qui est installée. Le risque n'a pas disparu pour autant : la plateforme `espressif32@6.9.0` reste sur disque (voir plus haut) et toute commande qui la réactiverait ferait redescendre le paquet en 4.5.1. Or esptool 5 a renommé sous-commandes et valeurs d'options en tirets (`erase-region`, `--after no-reset`) là où la 4.x utilise des underscores (`erase_region`, `--after no_reset`).

`deploy.sh` **sonde `--help`** et choisit l'orthographe correspondante, au lieu de supposer une version. Ce mécanisme est **conservé** : il ne coûte rien et rend le déploiement insensible à la version réellement installée.

### Modes OTA (WiFi)

| Commande | Action |
|----------|--------|
| `./deploy.sh ota-firmware` | Firmware uniquement en OTA |
| `./deploy.sh ota-fs` | Filesystem uniquement en OTA |
| `./deploy.sh ota-all` | Firmware + filesystem en OTA |

Les modes OTA utilisent par défaut `poolcontroller.local` (mDNS). Pour cibler une IP directe :

```bash
./ota_update.sh both 192.168.1.42 monmotdepasse
```

Voir aussi [UPDATE_GUIDE.md](UPDATE_GUIDE.md) pour la procédure détaillée.

## Archivage automatique des artefacts de build (v2.20.2)

L'archivage est branché **au niveau du build**, plus au niveau du script de déploiement : [`tools/archive_build.py`](../tools/archive_build.py) est déclaré en `extra_scripts = post:` sur l'env matériel `esp32dev_core3` dans [`platformio.ini`](../platformio.ini). **Chaque `pio run` réussi** archive donc `firmware.elf` **et** `firmware.bin` :

```
builds/2.42.1/esp32dev_core3/firmware_20260906_212339.elf
builds/2.42.1/esp32dev_core3/firmware_20260906_212339.bin
```

| Élément | Règle |
|---|---|
| Nommage | `firmware_<AAAAMMJJ_HHMMSS>.{elf,bin}` — l'horodatage en tête rend l'**ordre lexicographique = ordre chronologique** (le tri ne dépend donc pas de la `mtime`, que `shutil.copy2` recopie de la source) |
| Cloisonnement | `builds/<VERSION>/<PIOENV>/` — la version est lue dans [`src/version.h`](../src/version.h) |
| Rétention | **5 archives par version ET par environnement**, par extension |
| Robustesse | Le script **ne fait jamais échouer un build** : toute erreur est affichée et ignorée |
| Dossier `builds/` | Exclu de git (`.gitignore`), local uniquement |

**Pourquoi ELF *et* BIN** : le décodage d'un coredump exige l'ELF **exact** du firmware qui tournait (il porte les symboles) ; le BIN identifie l'image **effectivement flashée** et permet de rejouer un déploiement à l'identique.

**Le cloisonnement par environnement est conservé** (`builds/<VERSION>/<PIOENV>/`) alors qu'il n'y a plus qu'un env. Motif **historique** : `esp32dev` et `esp32dev_core3` produisaient des binaires **différents sous la même version**, et rangés ensemble une série de 5 builds core 2 aurait **évincé l'ELF core 3 réellement déployé** — c'est-à-dire reconduit exactement la panne forensique décrite ci-dessous. Le mécanisme reste en place parce qu'il est gratuit et qu'il redeviendrait nécessaire au premier second env ([ADR-0042](adr/0042-core3-environnement-par-defaut.md), [ADR-0069](adr/0069-retrait-repli-core-2.md)).

> ⚠️ **Pourquoi ce mécanisme a été déplacé** : `deploy.sh` archivait bien l'ELF, mais **`pio run` lancé directement et `./ota_update.sh` ne passent pas par lui** — et ce sont les chemins réellement utilisés au quotidien. Résultat : le dernier ELF présent dans `builds/` datait du **02/08**, et le coredump du crash de production du **15/08** a été **inexploitable** — `esp-coredump` refuse de décoder avec `coredump SHA256 ≠ app SHA256`.

> ℹ️ **`deploy.sh` n'archive plus rien** : sa fonction `archive_elf()` a été **supprimée**. Elle déversait des ELF non versionnés **à plat** dans `builds/`, en doublon du hook — un doublon qui repolluait le dossier après chaque ménage, et qui ne couvrait de toute façon qu'un seul des trois chemins de build. Le hook post-build est désormais le mécanisme **unique**.

> ℹ️ **Limite connue** : la liste affichée automatiquement par [`tools/decode_coredump.sh`](../tools/decode_coredump.sh) en cas de SHA mismatch ne parcourt que `builds/firmware_*.elf` (le niveau **plat**, désormais alimenté par plus rien — il ne contient que les archives héritées d'avant la v2.20.2). En pratique, retrouver l'ELF avec `ls builds/*/*/` et le passer **en 2ᵉ argument** du script.

## Décodage d'un coredump

Workflow complet pour diagnostiquer un crash `PANIC` en production.

### 1. Récupérer le token d'authentification

Dans la console du navigateur (page ouverte sur l'interface du contrôleur) :

```javascript
sessionStorage.getItem('authToken')
```

Ou via l'API :

```bash
curl -s -X POST http://<ip>/auth/login \
  -H "Content-Type: application/json" \
  -d '{"password":"<mot-de-passe>"}' | jq -r '.token'
```

### 2. Télécharger le coredump

```bash
curl -H "X-Auth-Token: <token>" \
  http://<ip>/coredump/download -o coredump.bin
```

### 3. Décoder

```bash
./tools/decode_coredump.sh coredump.bin
```

L'ELF par défaut est `.pio/build/$PIO_ENV/firmware.elf` — donc celui de l'env **réellement déployé**. ⚠️ Décoder un dump avec un ELF qui ne lui correspond pas **n'échoue pas franchement** : cela produit des noms de fonctions plausibles et **FAUX**. L'avertissement vaut toujours après le retrait du repli core 2 — il suffit d'un ELF d'une **autre version** du firmware.

Un deuxième argument optionnel permet de spécifier l'ELF à utiliser (utile si l'ELF courant ne correspond pas au firmware crashé) :

```bash
./tools/decode_coredump.sh coredump.bin builds/firmware_20260427_082810.elf
```

### 4. Récupérer d'un SHA mismatch

Si le coredump a été capturé avec un firmware différent de l'ELF courant, le script détecte le mismatch et affiche automatiquement les archives disponibles avec la commande exacte à relancer :

```
─────────────────────────────────────────────────────
Le coredump a été capturé avec un firmware différent.
SHA du coredump : 07b12255d2bba9c6

ELF archivés disponibles (du plus récent au plus ancien) :
  builds/firmware_20260427_143022.elf
  builds/firmware_20260427_082810.elf

Relancer avec l'ELF correspondant :
  ./tools/decode_coredump.sh coredump.bin builds/firmware_20260427_082810.elf
─────────────────────────────────────────────────────
```

Identifier l'archive dont la date/heure est antérieure au crash (visible dans les logs ou dans `reset_reason` WS), relancer avec cet ELF.

Voir aussi [ADR-0009](adr/0009-partition-coredump.md) pour la décision architecturale sur la partition `coredump`.

## Pièges connus

- **Changement de bootloader** : l'OTA n'écrit que l'application — réécrire le bootloader exige un **flash USB** (`./deploy.sh all`). Voir [ADR-0042](adr/0042-core3-environnement-par-defaut.md).
- **Ne pas réintroduire un second env matériel à la légère** : les deux plateformes installent leur framework sous le **même nom de paquet** `framework-arduinoespressif32`, donc dans le **même répertoire**, et s'écrasent mutuellement (symptôme : `TypeError ... NoneType` en ~2 s, remède `pio pkg install -e <env>`). C'est l'une des raisons du retrait du repli core 2 — voir [ADR-0069](adr/0069-retrait-repli-core-2.md).
- **`pio run -t buildfs`** : ne génère pas un `littlefs.bin` de la bonne taille → utiliser `./build_fs.sh`.
- **`pio run -t uploadfs`** : reconstruit le filesystem à la mauvaise taille avant upload → utiliser `./deploy.sh fs` ou `./deploy.sh ota-fs`.
- **Partition `history` préservée** : l'upload filesystem OTA n'écrase que la partition `spiffs`. L'historique des mesures n'est **pas** perdu à la mise à jour UI (voir [ADR-0068](adr/0068-partitions-layout-v5-flash-8-mo.md) et [history.md](subsystems/history.md)). ⚠️ Ceci vaut pour une mise à jour **à table constante** — un **changement de layout**, lui, déplace `history` et l'efface (point suivant).
- **Changement de table de partitions** : un nouveau layout (dernier en date : v4 → v5 en v2.42.0) exige un **flash USB une fois** — l'OTA ne réécrit ni la table ni le bootloader, et une image FS d'un layout ne doit **jamais** être poussée en OTA sur un appareil resté dans l'ancien (métadonnées LittleFS discordantes). Voir [UPDATE_GUIDE.md](UPDATE_GUIDE.md#migration-layout-v4--v5-flash-8-mo--depuis-2026-09-05).
- ⚠️ **`LITTLEFS_OFFSET` dans `deploy.sh` est le point de défaillance silencieux d'un changement de layout.** Il vaut **`0x730000`** depuis le v5 (était `0x390000`). Oublier de le mettre à jour écrit le filesystem **au milieu de `app1`**, sans message d'erreur : le flash « réussit », l'interface web reste morte, et le slot OTA de secours est corrompu. Le quatuor à maintenir aligné est `partitions.csv` / `platformio.ini` / `build_fs.sh` / `deploy.sh`.
- **La déclaration `board_build.flash_size` doit décrire le matériel, pas une habitude.** Elle a affirmé `4MB` pendant toute la vie des layouts v1 à v4 sur une puce qui en contient **8** — 4 Mo inutilisés, et une contrainte de conception imaginaire pendant des mois. Vérifier avec `esptool flash_id` (champ `Detected flash size`, ou l'octet de capacité du `Device ID`) **avant** de raisonner sur la place disponible.

## Dépendances PlatformIO

Liste des libs déclarées dans `platformio.ini` (`lib_deps`) — voir le fichier source pour les versions épinglées.

| Lib | Usage |
|-----|-------|
| `ESP32Async/AsyncTCP@3.4.10` | TCP asynchrone (base de ESPAsyncWebServer) — fork maintenu, version épinglée |
| `ESP32Async/ESPAsyncWebServer@3.11.2` | Serveur HTTP / WebSocket non bloquant — fork maintenu, version épinglée |
| `ESPAsyncWiFiManager` | Wizard Wi-Fi premier boot (dépendance conservée ; portail Wi-Fi géré nativement par le projet) |
| `ArduinoJson` (v7) | Sérialisation JSON |
| `PubSubClient` | Client MQTT |
| `OneWire` + `DallasTemperature` | Bus 1-Wire pour DS18B20 (eau + circuit) |
| `RTClib` | DS3231 RTC |

> **Migration fork ESP32Async (v2.9.0)** : `AsyncTCP` et `ESPAsyncWebServer` proviennent désormais du fork **maintenu** `ESP32Async` (ex-`mathieucarbou`), en **versions épinglées** (`@3.4.10` / `@3.11.2`), et non plus des dépôts Git `me-no-dev` non figés (abandonnés). Ce changement corrige un crash de course (assert lwIP au close/timeout d'un client WebSocket) diagnostiqué par coredump, **sans modification du code applicatif**. *(Contexte de l'époque : la plateforme était alors `espressif32@6.9.0`, core 2.0.x / IDF 4.4 — elle est restée inchangée par cette migration de libs. Le couple épinglé est aujourd'hui porté par le seul env core 3.)* Un seul `AsyncTCP` et un seul `ESPAsyncWebServer` dans l'arbre de deps (aucun duplicata `me-no-dev` transitif). Voir [ADR-0028](adr/0028-migration-fork-esp32async.md).

> **Libs supprimées en feature-021 (v2.0.0)** : `Adafruit ADS1X15` et `DFRobot_PH` ont été retirées des `lib_deps`. La chaîne pH/ORP est désormais entièrement portée par la mini-classe maison [`AtlasEzoSensor`](../src/atlas_ezo.h) qui pilote les modules Atlas EZO Embedded en I²C natif (`Wire`). Voir [ADR-0014](adr/0014-migration-atlas-ezo.md). Cette suppression libère ~12 KB de flash mais l'ajout de la queue + des routes de calibration consomme environ autant — le build tenait alors à 98.8 % flash (layout v1). Depuis le layout v4 ([ADR-0024](adr/0024-partitions-layout-v4.md), slots app 1792 KB), l'occupation firmware était redescendue à ~78,5 % (marge ~392 KB) — puis remontée à **99,5 %** au fil des versions, avant que le layout v5 ([ADR-0068](adr/0068-partitions-layout-v5-flash-8-mo.md), slots app **3648 KB**) ne la ramène à **48,9 %**.

## Bibliothèque de graphiques uPlot (frontend)

Les 6 graphiques de l'UI (3 mini-charts du dashboard, 3 historiques détail pH / ORP / Température) sont rendus par **uPlot** depuis feature-043, qui remplace Chart.js v4.5.1 (`chart.umd.min.js`, ~208 KB, **supprimé**). Voir [ADR-0018](adr/0018-migration-uplot.md). Un 7ᵉ graphique (debug oscillation pH du panel Avancé) a été retiré en v2.5.0 (feature-045).

### Provenance et version

- Paquet npm **`uplot@1.6.32`** — version **FIGÉE**, récupérée une fois puis committée. Aucune dépendance npm au build ni au runtime (offline-first, pas de CDN).
- Fichiers committés dans `data/` : [`uPlot.iife.min.js`](../data/uPlot.iife.min.js) (~50 KB) et [`uPlot.min.css`](../data/uPlot.min.css) (~2 KB) — copies de `dist/uPlot.iife.min.js` et `dist/uPlot.min.css` du paquet.
- Chargés par [`data/index.html`](../data/index.html) via `<script>` + `<link>` — 100 % vanilla, **aucune étape de build ajoutée** au déploiement (cohérent [ADR-0006](adr/0006-frontend-vanilla-js.md)).

### Mise à jour manuelle (si nécessaire)

```bash
npm pack uplot@<version>     # télécharge uplot-<version>.tgz
tar -xzf uplot-<version>.tgz
cp package/dist/uPlot.iife.min.js package/dist/uPlot.min.css data/
```

Puis `./build_fs.sh` et **vérification de parité en navigateur** (aucun test automatisé frontend — dashboard, détail pH/ORP/Température, console sans erreur).

### Gain mesuré (feature-043)

Suppression de `chart.umd.min.js` : payload FS 601 054 → 449 177 octets (**−148,3 KB**), soit ≈ 449 KB — gain qui a rendu possible la réduction de la partition `spiffs` à 576 KB au profit des slots app (layout v3, [ADR-0019](adr/0019-partition-app-1664k.md)).

> **Options non reprises** : l'ancienne usine Chart.js `createLineChart` exposait des options **mortes** (`hideYAxis`, `showYAxisGrid`, `extraPlugins`, `annotation`, `backgroundColor`), inutilisées aux call-sites. L'usine uPlot ([`data/app.js`](../data/app.js)) ne les reprend **pas** — réduction de périmètre assumée (feature-043).

## Structure des fichiers de build

- [`partitions.csv`](../partitions.csv) — source de vérité pour les offsets et tailles.
- [`platformio.ini`](../platformio.ini) — plateforme, libs, flags de compilation ; `default_envs = esp32dev_core3` ([ADR-0042](adr/0042-core3-environnement-par-defaut.md)).
- [`build_all.sh`](../build_all.sh) — firmware + filesystem en une commande (env = `PIO_ENV`).
- [`build_fs.sh`](../build_fs.sh) — filesystem uniquement (minification + taille correcte) ; sortie dans `$BUILD_DIR` = `.pio/build/$PIO_ENV/`.
- [`minify.js`](../minify.js) — minification HTML / CSS / JS + pré-compression gzip niveau 9 (seule la variante `.gz` est embarquée).
- [`deploy.sh`](../deploy.sh) — wrapper unique pour upload USB ou OTA ; sélectionne l'env via `PIO_ENV`, détecte le port série, enchaîne le moniteur après un flash USB (`NO_MONITOR=1` pour s'en passer). N'archive plus les artefacts (c'est le hook post-build).
- [`tools/archive_build.py`](../tools/archive_build.py) — `extra_scripts = post:` des envs matériels ; archive `firmware.elf` **et** `firmware.bin` à chaque `pio run` sous `builds/<VERSION>/<PIOENV>/`.
- [`ota_update.sh`](../ota_update.sh) — utilitaire OTA bas niveau (appelé par `deploy.sh`).
- [`tools/decode_coredump.sh`](../tools/decode_coredump.sh) — décodage d'un coredump avec `xtensa-esp32-elf-gdb`.
- `data/` — sources UI (HTML / CSS / JS / images).
- `data-build/` — UI minifiée + gzippée (généré automatiquement, ignoré par git).
- `.pio/build/$PIO_ENV/` — artéfacts de build (`firmware.bin`, `firmware.elf`, `littlefs.bin`) ; par défaut `.pio/build/esp32dev_core3/`.
- `builds/` — archives horodatées (généré automatiquement, ignoré par git) : `<VERSION>/<PIOENV>/firmware_*.{elf,bin}` (`tools/archive_build.py`). D'éventuels `firmware_*.elf` **à plat** sont des reliquats de l'ancien archivage `deploy.sh`, supprimé.
