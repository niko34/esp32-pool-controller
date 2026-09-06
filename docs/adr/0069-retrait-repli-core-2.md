# ADR-0069 — Retrait définitif du repli core Arduino 2.x : une assurance triplement inopérante coûtait plus cher que le risque qu'elle prétendait couvrir

- **Statut** : Accepté — **referme la clause de repli de l'[ADR-0042](0042-core3-environnement-par-defaut.md)** (point 4 de sa Décision, alternative écartée « Supprimer `[env:esp32dev]` », dette « Conflit de paquet framework », conséquence « les gardes bi-core restent obligatoires ») et **clôt la phase code bi-core de l'[ADR-0034](0034-migration-core3-idf5.md)**. Le reste des deux ADR est **inchangé** : le core 3 reste l'environnement de production bootloader compris, `PIO_ENV` reste l'unique point d'entrée de l'outillage, l'épinglage `55.03.37` reste un contrat de production.
- **Date** : 2026-09-06
- **Décideurs** : utilisateur (constat « la saison de production est faite »), `architect`, gate `pool-chemistry` (**GO** — le retrait durcit `isPumpUsable()`, n'assouplit rien), `test-engineer` (**PASS** — équivalence binaire), `code-reviewer` (**Approved**)
- **Spec(s) liée(s)** : [feature-102](../../specs/features/done/feature-102-retrait-repli-core-2.md) ; origine du défaut de compilation : `bug-080` (lot B-2)

## Contexte

L'[ADR-0042](0042-core3-environnement-par-defaut.md) (22/08/2026) a fait du core 3 l'environnement
de production, **bootloader compris**, tout en conservant `[env:esp32dev]` comme **assurance**.
La formule exacte de son alternative écartée :

> « **Supprimer `[env:esp32dev]`** (rejetée) — les gardes bi-core `#if ESP_ARDUINO_VERSION_MAJOR >= 3`
> ne coûtent presque rien, et un repli compilable est une assurance tant que le core 3 n'a pas une
> **saison complète de production** derrière lui. »

Cette clause portait sa propre date d'expiration. Elle est arrivée, et l'inventaire fait à cette
occasion montre que l'assurance avait cessé de couvrir quoi que ce soit bien avant.

### Sept justifications

**1. La saison de production est accomplie.** Constat de l'utilisateur, non vérifiable par le code :
l'appareil tourne en core 3 (application depuis des semaines avant l'ADR-0042, bootloader compris
depuis la v2.20.4) sans défaut imputable à la plateforme. C'est exactement la condition posée par
l'ADR-0042.

**2. L'argument « marge flash » est caduc.** Il plaidait pour garder le core 2 : l'ADR-0042
consigne **94,3 %** d'occupation en core 3 (1 730 053 o) contre **84,5 %** en core 2, sur un slot
applicatif de **1 835 008 o**. Depuis le layout v5 ([ADR-0068](0068-partitions-layout-v5-flash-8-mo.md)),
`app0` fait `0x390000` = **3 735 552 o** et l'occupation mesurée est de **1 825 717 o**, soit
**48,9 %** — marge **1,82 Mo**. Le dénominateur a doublé ; l'argument s'est évaporé avec lui.

**3. Le repli n'était plus compilable depuis la v2.40.0.** [`src/oom_guard.cpp`](../../src/oom_guard.cpp)
appelait `esp_cpu_get_cycle_count()` (API IDF 5) aux lignes 371 et 374, avec un `#include <esp_cpu.h>`
placé **hors garde** ligne 9. Introduit par le commit `b09fc8f` (bug-080, lot B-2). Le défaut est
resté **invisible pendant deux versions** parce que
[`.github/workflows/tests-natifs.yml`](../../.github/workflows/tests-natifs.yml) ne construit que
l'env `native` : **aucun env matériel n'est bâti en CI**. Autrement dit, l'assurance était morte
depuis deux versions et personne ne pouvait le savoir.

**4. La lignée core 2 est abandonnée en amont.** Espressif ne supporte plus PlatformIO pour cette
lignée : le wedge lwIP et la division par zéro littlefs ne seront jamais corrigés, et **aucun
correctif de sécurité n'arrivera** (constat déjà posé par l'[ADR-0034](0034-migration-core3-idf5.md)).
La valeur du repli décroissait ; son coût d'entretien, lui, était constant.

**5. Le repli n'était plus exerçable.** Le bootloader est en core 3 depuis l'ADR-0042 : un retour
arrière exige un **flash USB**. Or la machine de développement est désormais un **conteneur LXC sans
port série**. Même avec un binaire valide, personne n'aurait pu le poser sur l'appareil.

**6. Le conflit de paquet `framework-arduinoespressif32` disparaît.** Les deux plateformes installent
leur framework sous le **même nom de paquet**, donc dans le **même répertoire**, et s'écrasent
mutuellement (symptôme : `TypeError: expected str ... not NoneType` en ~2 s). Chaque bascule imposait
un `pio pkg install -e <env>` — commande qui **réécrit `platformio.ini` en supprimant tous ses
commentaires**. Le maintien du repli coûtait donc, à intervalles réguliers, la justification écrite
des choix de build.

**7. Aucun binaire core 2 n'était archivé.** `builds/` ne contient que `2.42.0/esp32dev_core3/` (et
désormais `2.42.1/esp32dev_core3/`). La case ouverte de l'[ADR-0034](0034-migration-core3-idf5.md)
— « **Binaires de retour arrière archivés + sha256**, dernière version core 2 déployée » — **n'a
jamais été honorée**.

### Le constat qui tranche

Le repli était **triplement inopérant** : **non compilable** (3), **non flashable** (5), **sans image
de secours** (7). Ce qui restait n'était pas une assurance mais son apparence — et cette apparence
était **publiée** : [`docs/UPDATE_GUIDE.md`](../UPDATE_GUIDE.md) proposait encore
`PIO_ENV=esp32dev ./deploy.sh all` comme procédure de retour arrière. C'est la seule conséquence
visible utilisateur du présent ADR : **cette procédure disparaît**, parce qu'elle était fausse et
dangereuse.

## Décision

Le **repli core Arduino 2.x est retiré définitivement** à partir de la **v2.42.1**. Le projet a un
**environnement matériel unique**, `[env:esp32dev_core3]`.

1. Le bloc `[env:esp32dev]` est supprimé de [`platformio.ini`](../../platformio.ini), ainsi que la
   section « ⚠️ CONFLIT DE PAQUET ». **Tout commentaire qui n'existait que dans le bloc supprimé et
   qui était référencé par renvoi depuis l'env core 3 a été migré** dans `[env:esp32dev_core3]`
   (flags PSRAM WROVER-E de feature-066, justification de l'archivage des artefacts, versions
   épinglées du fork ESP32Async, note `./build_fs.sh`).
2. `default_envs = esp32dev_core3` est **conservé** et **l'env n'est pas renommé**.
3. Les **8 gardes** `#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3` sont
   supprimées, la branche core 3 étant conservée telle quelle : `pump_controller.cpp` (6 sites :
   `begin`, `isPumpUsable`, entrée et écriture d'`applyPumpDuty`, `setManualPump`) et
   `pump_controller.h` (le membre `volatile bool pumpUsable[2]` devient inconditionnel),
   `main.cpp` (`esp_task_wdt_reconfigure` conservé, `esp_task_wdt_init` supprimé) et
   `oom_guard.cpp` (`<esp_memory_utils.h>` conservé, `<soc/soc_memory_layout.h>` supprimé — le défaut
   des lignes 371/374 **disparaît par suppression**, il n'est pas réparé). Garde de sortie :
   `grep -rn ESP_ARDUINO_VERSION_MAJOR src/` → **0 ligne**.
4. **`FIRMWARE_VERSION` passe à `2.42.1`.**

### Décision A15 — la garde `MBEDTLS_VERSION_NUMBER` est CONSERVÉE

La garde `#if MBEDTLS_VERSION_NUMBER >= 0x03000000` de
[`src/ota_integrity.cpp:8`](../../src/ota_integrity.cpp) **reste en place**, alors même qu'elle
faisait partie des « 3 familles de gardes » de la phase bi-core.

Motif : elle n'est **pas indexée sur l'environnement retiré**, mais sur le **contrat d'une
bibliothèque tierce** (mbedTLS 2 vs 3 : les fonctions `mbedtls_sha256_*` ont changé de signature).
Cette version peut bouger lors d'une **montée** de plateforme comme d'une **descente**, sans qu'un
seul env soit en cause. La retirer ne supprimerait pas du risque, elle troquerait quelques lignes de
code mort contre une **rupture de compilation silencieuse** au premier changement de version de la
bibliothèque. Le critère est net et vaut règle : *on retire une garde indexée sur un environnement
qu'on supprime ; on garde une garde indexée sur un contrat qu'on ne contrôle pas.*

### Décision F7 — `espressif32@6.9.0` n'est PAS désinstallée

La plateforme core 2 **reste installée sur disque** (~404 Mo) bien qu'aucun env ne la référence.

Motif technique dur, vérifié : le `platform.json` de **pioarduino 55.03.37** déclare
`"tool-mklittlefs": null` — retrait **explicite** du paquet — tandis qu'**espressif32@6.9.0** le
déclare en `~1.203.0`. Or [`build_fs.sh:22`](../../build_fs.sh) code en dur le chemin
`~/.platformio/packages/tool-mklittlefs/mklittlefs`. Désinstaller la plateforme core 2 casserait
donc **`./build_fs.sh`, `./build_all.sh`, `./deploy.sh fs` et `./deploy.sh ota-fs`**.

Le ménage disque devient une **tâche séparée**, dont le **prérequis** est de rendre `build_fs.sh`
indépendant de cette plateforme. Consigné ici pour que personne ne « nettoie » cette plateforme en
croyant achever le présent retrait.

### Méthode de validation employée — précédent à retenir

Le retrait ne portait que sur des **branches `#else` du préprocesseur jamais compilées** dans le
binaire core 3. Deux conséquences de méthode :

- **La validation par mutation est inapplicable** — casser volontairement une branche qui n'est pas
  compilée ne tue aucun test. Elle **n'a pas été simulée** pour autant : produire un rapport de
  mutation sans mutant réel aurait été pire que son absence. Constat associé : **aucune suite native
  ne compile `pump_controller.cpp`** (le `build_src_filter` de `[env:native]` ne liste que 13 modules
  de logique pure, conséquence assumée de l'[ADR-0017](0017-logique-metier-pure-humble-object-testabilite.md)).
  Les 1286 tests natifs jouent ici un rôle de **non-régression** sur les modules non touchés, pas de
  preuve de correction du retrait.
- **La garde rigoureuse est l'équivalence binaire** : sections `text` / `data` / `bss` **identiques à
  l'octet** à la baseline du commit `b86813d`, **plus** une comparaison de la **table des symboles**
  (`nm --print-size`) contre l'ELF archivé de la version précédente — **11 124 symboles, diff vide,
  adresses comprises**. Les **77 octets** d'écart du `.bin` sont tous **imputés** : `__TIME__`
  (5 occurrences), la chaîne de version, le descripteur d'application et le SHA256 d'image.

> ⚠️ **Cette mesure n'a de valeur qu'après un build propre.** Un build incrémental peut conserver des
> objets compilés avant le retrait et rendre l'égalité triviale — donc vide de sens. Toute
> reproduction de cette validation doit partir d'un répertoire de build effacé.

Ce couple *(équivalence binaire + diff de symboles)* est le **précédent** à réutiliser pour tout
retrait de code mort du préprocesseur, là où la mutation ne mord pas.

## Alternatives considérées

- **Réparer `oom_guard.cpp` pour garder le repli compilable** (rejetée) — techniquement à portée
  (deux lignes sous garde). Mais réparer la compilabilité n'aurait traité que **la première** des
  trois causes d'inopérance : il resterait un repli **non flashable** (bootloader core 3, LXC sans
  port série) et **sans image de secours** (aucun binaire core 2 archivé, case ADR-0034 jamais
  honorée). On aurait payé le coût d'entretien complet — gardes, conflit de paquet, `pio pkg install`
  qui décommente `platformio.ini` — pour une assurance qui reste inexerçable.
- **Garder l'env non compilable, à titre de documentation** (rejetée) — un env qui échoue au build
  n'est pas de la documentation, c'est un **piège**. Il annonce une capacité qui n'existe pas, et il
  l'annonçait jusque dans le guide utilisateur. Ce que le repli avait à dire est mieux dit ici, dans
  un document qui ne prétend pas s'exécuter.
- **Renommer `esp32dev_core3` en `esp32dev`** (rejetée) — le suffixe `_core3` n'a plus d'utilité
  discriminante une fois le second env parti, et le nom est devenu redondant. Mais renommer casserait
  tous les chemins `builds/<VERSION>/esp32dev_core3/` **déjà archivés** (dont les ELF nécessaires au
  décodage des coredumps de production), la valeur par défaut de `PIO_ENV` dans cinq scripts, et les
  références de plusieurs docs et ADR. Coût réel, bénéfice cosmétique : le nom reste.
- **Retirer le repli** (retenue) — le seul choix qui aligne ce que le projet **prétend** pouvoir faire
  sur ce qu'il **peut** faire.

## Conséquences

### Positives

- **La documentation cesse de mentir sur une capacité de secours.** Le fait visible utilisateur du
  cycle est la **disparition de la procédure de retour arrière core 2** de
  [`UPDATE_GUIDE.md`](../UPDATE_GUIDE.md), remplacée par ce qui marche : revenir à une **version
  antérieure du firmware core 3** (image archivée ou asset de release), en OTA si l'appareil répond,
  par flash USB sinon.
- **`isPumpUsable()` n'a plus de branche fail-open.** La sémantique « en core 2, une pompe est
  toujours réputée utilisable » disparaît : le drapeau `pumpUsable[]` est inconditionnel, l'alerte
  MQTT `pump_unusable` est **armée sans condition**. Durcissement unilatéral, gate `pool-chemistry`
  **GO**.
- **Fin du conflit de paquet framework**, donc fin des `pio pkg install` à répétition — et donc fin
  du risque de perdre les commentaires de `platformio.ini` à chaque bascule.
- **Une classe de défaut disparaît** : plus aucun code ne peut être cassé « côté core 2 » sans que
  quiconque s'en aperçoive.
- **Aucun changement de comportement à l'exécution.** Binaire fonctionnellement identique, prouvé par
  équivalence binaire.

### Négatives / dette assumée

- **Il n'existe plus aucun retour vers le core 2.** Une régression imputable au core 3 se traite
  **en avant** (correctif, ou retour à une version antérieure du firmware core 3), jamais en arrière.
  C'est la formalisation d'un état de fait, pas une perte nouvelle — mais c'est désormais définitif.
- **~404 Mo de plateforme core 2 restent sur disque** (décision F7), sans env qui la référence. Le
  chemin `tool-mklittlefs` codé en dur dans `build_fs.sh` est une **dépendance cachée** entre le
  build du filesystem et une plateforme officiellement abandonnée. Tant qu'elle existe, une commande
  `pio` qui réactiverait cette plateforme peut faire **redescendre `tool-esptoolpy` en 4.5.1** — la
  sonde `--help` de `deploy.sh` reste donc utile et ne doit pas être retirée.
- **L'env core 3 reste sans garde CI.** La CI ne construit toujours aucun env matériel : le trou de
  détection qui a laissé passer la casse du core 2 pendant deux versions **existe encore** pour le
  core 3. Rattaché au constat ouvert du 2026-09-06 ; à traiter par une spec dédiée, délibérément
  **non absorbé** dans ce cycle.
- **La ré-isolation de `TCP_USER_TIMEOUT`**, rendue possible par IDF 5, reste une **piste ouverte non
  instruite** : le paramètre lwIP **global** est inchangé (voir
  [mqtt-manager.md](../subsystems/mqtt-manager.md)).

### Ce que ça verrouille

- **Un seul environnement matériel.** Ajouter un second env de plateforme redevient une décision
  d'architecture à part entière : il faudrait rouvrir le conflit de paquet `framework-arduinoespressif32`
  et le cloisonnement d'archivage — tous deux conservés en l'état pour cette raison.
- **Aucune divergence de core n'est autorisée dans le code.** Plus de garde
  `#if ESP_ARDUINO_VERSION_MAJOR`, plus de fork de fichier, plus de branche longue durée. Une
  divergence de plateforme se traite par une **migration**, pas par une alternance.
- **Le cloisonnement par environnement de `tools/archive_build.py`** (`builds/<VERSION>/<PIOENV>/`)
  et **`PIO_ENV` comme unique point d'entrée** de l'outillage ([ADR-0042](0042-core3-environnement-par-defaut.md))
  sont **maintenus** malgré l'env unique : ils sont gratuits et redeviendraient nécessaires au premier
  second env.
- **Le ménage disque de la plateforme core 2 a un prérequis nommé** : rendre `build_fs.sh` indépendant
  de `tool-mklittlefs` fourni par `espressif32@6.9.0`.
- **La garde `MBEDTLS_VERSION_NUMBER` ne se retire pas « par cohérence »** avec le présent ADR :
  elle relève d'un contrat de bibliothèque, pas d'un environnement.

## Références

- Config : [`platformio.ini`](../../platformio.ini) (env unique, commentaires migrés), [`src/version.h`](../../src/version.h) (`2.42.1`)
- Code : [`src/pump_controller.cpp`](../../src/pump_controller.cpp), [`src/pump_controller.h`](../../src/pump_controller.h), [`src/main.cpp`](../../src/main.cpp), [`src/oom_guard.cpp`](../../src/oom_guard.cpp), [`src/ota_integrity.cpp`](../../src/ota_integrity.cpp) (garde mbedtls **conservée**)
- Scripts : [`deploy.sh`](../../deploy.sh), [`ota_update.sh`](../../ota_update.sh), [`build_fs.sh`](../../build_fs.sh) (dépendance `tool-mklittlefs`), [`tools/archive_build.py`](../../tools/archive_build.py), [`tools/decode_coredump.sh`](../../tools/decode_coredump.sh)
- ADR liés : [ADR-0042](0042-core3-environnement-par-defaut.md) (**clause de repli refermée**), [ADR-0034](0034-migration-core3-idf5.md) (**phase code bi-core close**), [ADR-0068](0068-partitions-layout-v5-flash-8-mo.md) (marge flash qui périme l'argument n° 2), [ADR-0017](0017-logique-metier-pure-humble-object-testabilite.md) (pourquoi `pump_controller.cpp` n'est pas couvert en natif)
- Spec : [feature-102](../../specs/features/done/feature-102-retrait-repli-core-2.md)
- Docs : [BUILD.md](../BUILD.md), [UPDATE_GUIDE.md](../UPDATE_GUIDE.md), [pump-controller.md](../subsystems/pump-controller.md), [mqtt-manager.md](../subsystems/mqtt-manager.md)
