# ADR-0042 — Le core Arduino 3.x / IDF 5 devient l'environnement par défaut (bootloader ET application)

- **Statut** : Accepté — ⚠️ **la clause de repli est REFERMÉE par l'[ADR-0069](0069-retrait-repli-core-2.md)** (2026-09-06, v2.42.1). Sont **caducs** : le **point 4 de la Décision** (« `[env:esp32dev]` reste disponible comme repli explicite »), l'**alternative écartée** « Supprimer `[env:esp32dev]` » (sa condition — « tant que le core 3 n'a pas une saison complète de production derrière lui » — est remplie), la **dette « Conflit de paquet framework »** (sans objet, il n'y a plus qu'un env) et la **conséquence** « les gardes bi-core `#if ESP_ARDUINO_VERSION_MAJOR >= 3` restent obligatoires » (les 8 gardes sont supprimées, `grep` → 0 ligne). **Tout le reste est inchangé** : le core 3 est l'environnement de production bootloader compris, `PIO_ENV` reste l'unique point d'entrée de l'outillage, le cloisonnement d'archivage par env est maintenu, l'épinglage `55.03.37` reste un contrat de production, la perte du filet `otadata` reste entière.
- **Date** : 2026-08-22
- **Décideurs** : Nicolas
- **Spec(s) liée(s)** : aucune spec dédiée — franchissement de la « porte de bascule production » ouverte par [ADR-0034](0034-migration-core3-idf5.md) (études/exécution : feature-062, feature-063)

> 📋 **Amendement du 2026-09-06 (feature-103)** : la conséquence « **la CI n'est pas concernée** : `tests-natifs.yml` ne construit que l'env `native` » (§ Ce que ça verrouille) est **devenue fausse**. La CI construit désormais l'env matériel : [`.github/workflows/build-firmware.yml`](../../.github/workflows/build-firmware.yml) lance `pio run -e esp32dev_core3` sur push `main` et sur PR touchant les entrées de build. Un troisième workflow, [`hygiene-specs.yml`](../../.github/workflows/hygiene-specs.yml), contrôle l'unicité des specs. Cet ADR reste **Accepté**, son corps est **inchangé** — voir [BUILD.md § Intégration continue](../BUILD.md#intégration-continue-github-actions) et [feature-103](../../specs/features/done/feature-103-gardes-ci-build-firmware-et-hygiene-specs.md).

## Contexte

[ADR-0034](0034-migration-core3-idf5.md) a livré la **phase code bi-core** (le même arbre compile en core 2 et en core 3) et a explicitement laissé la **bascule production** comme une **porte séparée**, conditionnelle : l'env `[env:esp32dev_core3]` y était déclaré **BUILD-ONLY**, « jamais flashé sur la production », et le bootloader IDF 4.4 devait être conservé pour garantir un retour arrière **OTA-only**.

Trois éléments ont changé depuis, et rendent la porte franchissable :

1. **Le matériel a la PSRAM.** Le module **ESP32-DevKitC-VE (WROVER-E, 8 Mo de PSRAM)** est en place. C'était le verrou dur : sans PSRAM, core 3 signifie perdre tout TLS sortant, donc la vérification et l'installation de mises à jour depuis GitHub (note additive du 2026-08-08 sur l'ADR-0034, formalisée par [ADR-0036](0036-psram-requise-mise-a-jour-en-ligne.md)). Le prérequis GPIO 16/17 avait été levé par feature-065.
2. **L'image core 3 est éprouvée en production.** Elle tourne depuis des semaines sur l'appareil, poussée en OTA — donc **application core 3 sur bootloader core 2**. Ce montage a fonctionné, mais laissait le projet dans un état bâtard : `default_envs` désignait `esp32dev` (core 2) alors que le binaire en service était core 3.
3. **L'accès USB est disponible**, ce qui est la seule voie pour réécrire le bootloader.

Cet état bâtard a un coût mesuré, pas théorique : le binaire produit par un `pio run` nu n'était **pas** celui qui tournait sur l'appareil. C'est ce décalage qui a rendu le **coredump du 15/08 inexploitable** — un ELF core 2 confronté à un dump core 3 ne produit pas une erreur franche, il produit une pile de **noms de fonctions plausibles et faux**. Le même décalage se propageait aux scripts (`deploy.sh`, `build_fs.sh`, `ota_update.sh`, `build_all.sh`, `tools/decode_coredump.sh`), tous câblés en dur sur `.pio/build/esp32dev/`.

## Décision

Le **core Arduino 3.x / ESP-IDF 5** (pioarduino, release taguée `55.03.37` = core 3.3.7 / IDF 5.5.0) devient **l'environnement par défaut du projet**, pour le **bootloader** comme pour l'**application**, à partir de la **v2.20.4** :

1. `platformio.ini` : `default_envs = esp32dev_core3`. Les mentions « BUILD-ONLY » / « ne flashe jamais la production » de l'env core 3 sont **retirées** — elles sont désormais fausses.
2. Le **bootloader** est passé en core 3 par **flash USB** (`./deploy.sh all` / `factory`). L'OTA n'y suffit pas : il n'écrit que l'application.
3. Toute la chaîne d'outillage s'aligne sur **une variable unique `PIO_ENV`** (défaut `esp32dev_core3`, surchargeable), au lieu de dépendre du `default_envs` ou de chemins codés en dur : `deploy.sh` (passe `-e "$PIO_ENV"` à `pio run`, `pio run -t upload` et `pio device monitor`, exporte `PIO_ENV`/`BUILD_DIR`), `build_fs.sh` (écrit `littlefs.bin` dans `$BUILD_DIR`), `ota_update.sh` et `build_all.sh` (chemins d'artefacts dérivés), `tools/decode_coredump.sh` (ELF par défaut dérivé).
4. `[env:esp32dev]` (core 2) **reste disponible** comme **repli explicite**, jamais implicite :

```bash
pio run -e esp32dev                  # build core 2
PIO_ENV=esp32dev ./deploy.sh all     # déploiement complet core 2
```

Le point 3 est ce qui fait tenir le point 2 : `tools/decode_coredump.sh` prend l'ELF de l'env **effectivement déployé**, ce qui ferme la panne forensique décrite ci-dessus.

## ⚠️ Conflit de paquet entre les deux plateformes (à connaître avant de jongler)

Les deux plateformes installent leur framework sous le **même nom de paquet** `framework-arduinoespressif32` — version `3.20017.x` pour le core 2, `3.3.7` pour le core 3 — donc dans le **même répertoire** : **elles s'écrasent mutuellement**. Un build core 2 détruit le framework core 3, et réciproquement.

Symptôme : échec **immédiat** (~2 s, avant toute compilation) du build de l'autre env :

```
TypeError: expected str, bytes or os.PathLike object, not NoneType
  ... platforms/espressif32/builder/frameworks/arduino.py
```

`FRAMEWORK_DIR` vaut `None` parce que le paquet attendu n'est plus là. **Ce n'est pas un aléa** : c'est systématiquement un build de l'autre env qui vient de passer. Remède :

```bash
pio pkg install -e <env>     # retéléchargement du framework de cet env
```

Ce défaut a été pris pour un aléa pendant tout un cycle de travail, les échecs étant contournés par simple relance — d'où sa consignation ici et dans [BUILD.md](../BUILD.md).

Le paquet `tool-esptoolpy` est **partagé de la même façon** (4.5.1 côté core 2, 5.1.2 côté core 3) et bascule au gré du dernier build. Ses conséquences sont neutralisées à l'exécution : `deploy.sh` **sonde `--help`** pour choisir l'orthographe des sous-commandes (`erase_region` vs `erase-region`, `no_reset` vs `no-reset`) au lieu de supposer une version.

## Alternatives considérées

- **Rester en `default_envs = esp32dev` (core 2) et pousser l'application core 3 en OTA au cas par cas** (rejetée) — c'est l'état bâtard des dernières semaines. Le binaire par défaut n'est pas celui qui tourne : ELF archivé du mauvais environnement, coredump décodé avec des symboles faux (incident du 15/08), et chaque opération demande de se souvenir d'ajouter `-e esp32dev_core3`. Le « filet de repli » que cet état préservait (bootloader IDF 4.4) n'a **jamais** été utilisé en plusieurs semaines de fonctionnement.
- **Bootloader core 3 mais `default_envs` maintenu en core 2** (rejetée) — incohérence maximale : le build par défaut produirait une image core 2 pour un bootloader core 3, sans garde-fou.
- **Revenir intégralement en core 2** (rejetée) — lignée abandonnée (wedge lwIP, div/0 littlefs jamais corrigés en amont, plus de correctifs de sécurité) ; arbitrage déjà tranché par [ADR-0034](0034-migration-core3-idf5.md).
- **Supprimer `[env:esp32dev]`** (rejetée) — les gardes bi-core `#if ESP_ARDUINO_VERSION_MAJOR >= 3` ne coûtent presque rien, et un repli compilable est une assurance tant que le core 3 n'a pas une saison complète de production derrière lui.
- **Core 3 par défaut, bootloader inclus, avec `PIO_ENV` comme seul point d'entrée** (retenue) — cohérence build / déploiement / forensique, au prix de la perte du filet `otadata` et d'une exigence d'accès USB en cas d'échec.

## Conséquences

### Positives

- **Ce qui est buildé est ce qui tourne.** Les ELF/BIN archivés par `tools/archive_build.py` sous `builds/<VERSION>/<PIOENV>/` correspondent au firmware en service : un coredump redevient décodable sans manœuvre.
- **Un seul levier** (`PIO_ENV`) commande build, flash, FS, OTA et décodage de coredump. Les scripts ne dépendent plus du `default_envs` du fichier de config, ni de chemins `.pio/build/esp32dev/` en dur.
- **Bénéfices core 3 effectifs jusqu'au bootloader** : lwIP et littlefs 2.11.2 récents, PSRAM et donc TLS sortant fonctionnel ([ADR-0036](0036-psram-requise-mise-a-jour-en-ligne.md)), correctifs de sécurité de nouveau disponibles.

### Négatives / dette assumée

- **Perte du filet de repli `otadata`.** Tant que le bootloader restait en IDF 4.4, une image core 3 défaillante se rattrapait en repointant `otadata` (`esptool erase_region 0xE000 0x2000` → retour sur le slot précédent). Avec un bootloader core 3, ce filet disparaît : **tout échec impose un flash USB**. Une intervention de secours exige donc un **accès physique** au boîtier.
- **Marge flash resserrée** : le core 3 occupe **1 730 053 o** sur une partition applicative de **1 835 008 o**, soit **94,3 %** (marge ≈ 102 Ko), contre **84,5 %** en core 2. À surveiller à chaque nouvelle fonctionnalité ; le plafond de vigilance de 95 % fixé par feature-063 est presque atteint.
- **Conflit de paquet framework** (section dédiée ci-dessus) : alterner entre les deux envs exige un `pio pkg install -e <env>`. Coût permanent du maintien du repli core 2.
- **`tool-esptoolpy` partagé** : sa version bascule au gré du dernier build ; l'écart d'orthographe esptool 4/5 est absorbé par la sonde `--help` de `deploy.sh`, mais tout nouvel appel direct à esptool doit s'en soucier.

### Ce que ça verrouille

- **pioarduino est la plateforme de production** du contrôleur, plus seulement une cible de build. L'accès au sdkconfig complet continue d'exiger le plan B « Arduino composant ESP-IDF » ([ADR-0034](0034-migration-core3-idf5.md)).
- **L'épinglage `55.03.37` devient un contrat de production** : toute montée de version de la plateforme est un choix explicite, à re-valider au banc avant flash.
- **Le retour arrière n'est plus OTA-only** : la clause « bascule et retour arrière OTA-only » de l'ADR-0034 est **caduque** pour le bootloader. Repasser le bootloader en core 2 exige un flash USB.
- **`PIO_ENV` est l'unique point d'entrée** de la sélection d'environnement pour l'outillage. Tout nouveau script de build/flash/diagnostic doit le lire, jamais coder `.pio/build/esp32dev*` en dur.
- **Les gardes bi-core `#if ESP_ARDUINO_VERSION_MAJOR >= 3` restent obligatoires** tant que `[env:esp32dev]` est maintenu comme repli — pas de fork de fichier, pas de branche longue durée.
- La **CI n'est pas concernée** : [`.github/workflows/tests-natifs.yml`](../../.github/workflows/tests-natifs.yml) ne construit que l'env `native`.

## Références

- Config : [`platformio.ini`](../../platformio.ini) (`default_envs`, `[env:esp32dev_core3]`, `[env:esp32dev]`)
- Scripts : [`deploy.sh`](../../deploy.sh), [`build_fs.sh`](../../build_fs.sh), [`ota_update.sh`](../../ota_update.sh), [`build_all.sh`](../../build_all.sh), [`tools/decode_coredump.sh`](../../tools/decode_coredump.sh), [`tools/archive_build.py`](../../tools/archive_build.py)
- ADR liés : [ADR-0034](0034-migration-core3-idf5.md) (migration core 3, porte de bascule — **franchie par le présent ADR**), [ADR-0036](0036-psram-requise-mise-a-jour-en-ligne.md) (PSRAM requise pour le TLS sortant), [ADR-0024](0024-partitions-layout-v4.md) (taille de la partition applicative)
- Docs : [BUILD.md](../BUILD.md) (environnements, `PIO_ENV`, conflit de paquet), [UPDATE_GUIDE.md](../UPDATE_GUIDE.md) (changement de bootloader = flash USB)
