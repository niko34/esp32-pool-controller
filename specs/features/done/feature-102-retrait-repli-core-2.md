## Métadonnées
- **ID** : feature-102
- **Slug** : retrait-repli-core-2
- **Type** : feature (retrait — précédent : `feature-065-retrait-ecran-uart`)
- **Statut** : done — livré en v2.42.1 (tests verts, revue OK)
- **Priorité** : haute
- **Version cible** : 2.42.1
- **Dernière mise à jour** : 2026-09-06
- **Spec(s) liée(s)** : ADR-0034 (migration core 3), ADR-0042 (core 3 par défaut), ADR-0068 (layout v5), `bug-080` (dont le lot B-2 a introduit le défaut)

## Domaines touchés
- [ ] Capteurs (pH / ORP / température)
- [x] Régulation (pump_controller, PID) → **passage obligatoire par `pool-chemistry`**
- [ ] Filtration / Éclairage
- [ ] Web — routes HTTP / WebSocket
- [ ] MQTT / Home Assistant
- [ ] UART / écran LVGL
- [ ] OTA
- [ ] Authentification
- [ ] Historique / persistance
- [ ] Frontend (`data/`)

> Régulation cochée pour une raison de forme, pas de fond : 6 des 8 gardes retirées sont
> dans `pump_controller.*`. Aucun algorithme de dosage n'est modifié — seules disparaissent
> des branches `#else` du préprocesseur jamais compilées en core 3.

## Agents requis
- [x] spec-reader
- [x] architect
- [x] pool-chemistry — **obligatoire** : chemin de code touchant `pump_controller`
- [ ] ux-ui-designer
- [x] embedded-developer
- [ ] web-ui-developer
- [x] test-engineer
- [x] code-reviewer
- [x] doc-writer

## Description
Retirer définitivement le repli core Arduino 2.x (`[env:esp32dev]`, gardes bi-core,
chemins d'outillage, documentation) et consigner le retrait comme décision
d'architecture amendant la clause de repli de l'ADR-0042.

## Contexte
L'ADR-0042 (22/08/2026) a fait du core 3 l'environnement de production, bootloader
compris, tout en conservant `[env:esp32dev]` comme **assurance** : « un repli compilable
est une assurance tant que le core 3 n'a pas une saison complète de production derrière
lui ». Cette condition est aujourd'hui jugée remplie par l'utilisateur.

Sept justifications, toutes vérifiées sauf la première qui relève de son appréciation :

1. **Saison de production accomplie** — décision de l'utilisateur, non vérifiable par le code.
2. **L'argument « marge flash » est caduc.** Il plaidait pour garder core 2 (94,3 %
   d'occupation en core 3 contre 84,5 % en core 2). Depuis le layout v5 (ADR-0068),
   `app0` = `0x390000` = 3 735 552 o et l'occupation est de 1 825 717 o, soit **48,9 %**,
   marge 1,82 Mo. Les chiffres de l'ADR-0042 portaient sur un slot de 1 835 008 o.
3. **Le repli n'est plus compilable depuis la v2.40.0.** `src/oom_guard.cpp` lignes 371
   et 374 appellent `esp_cpu_get_cycle_count()` (API IDF 5) avec un `#include <esp_cpu.h>`
   ligne 9 **hors garde**. Introduit par `b09fc8f` (bug-080 lot B-2). Invisible pendant
   deux versions : la CI ne construit que l'env `native`.
4. **La lignée core 2 est abandonnée en amont** (wedge lwIP, div/0 littlefs jamais
   corrigés, plus de correctifs de sécurité) : valeur décroissante, coût d'entretien constant.
5. **Le repli n'était plus exerçable** : bootloader en core 3 depuis l'ADR-0042, retour
   arrière = flash USB, et la machine de développement est désormais un conteneur LXC
   sans port série.
6. **Fin du conflit de paquet** `framework-arduinoespressif32` : les deux plateformes
   l'installent dans le même répertoire et s'écrasent mutuellement, imposant un
   `pio pkg install` à chaque bascule — commande qui réécrit `platformio.ini` en
   supprimant tous ses commentaires.
7. **Aucun binaire core 2 n'est archivé.** `builds/` ne contient que
   `2.42.0/esp32dev_core3/`. La case ouverte de l'ADR-0034 (« Binaires de retour arrière
   archivés + sha256, dernière version core 2 déployée ») n'a **jamais** été honorée.
   Le repli était donc **triplement inopérant** : non compilable, non flashable, sans
   image de secours.

## Comportement attendu
Aucun changement de comportement à l'exécution. C'est le **critère d'acceptation
principal** : le binaire produit doit être fonctionnellement identique, sections
`text`/`data`/`bss` inchangées à l'octet.

Un seul fait devient visible côté utilisateur : la procédure de retour arrière vers le
core 2 publiée dans `docs/UPDATE_GUIDE.md` **disparaît**. Elle est aujourd'hui fausse et
dangereuse — elle ne compile même pas depuis la v2.40.0.

### Cas limites
- Le chemin d'échec de `ledcAttachChannel` (pose `pumpUsable[i] = false`) n'était pas
  testé avant ce cycle et ne l'est pas davantage après. Voir « Hors périmètre ».

## Comportement actuel
`pio run -e esp32dev` échoue à la compilation. `pio run -e esp32dev_core3` (défaut) est
SUCCESS. Les 8 gardes `#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3`
compilent systématiquement leur branche core 3 ; les branches `#else` sont du code mort.

## Contraintes techniques
- ESP32, offline first
- **Conteneur LXC sans port série** : `pio run` et `pio test -e native` possibles ;
  aucun flash, aucun test matériel.
- **Interdit dans ce cycle** : `pio run -e esp32dev` (détruirait le framework core 3
  installé) et tout `pio pkg install` / `pio pkg uninstall` (réécrit `platformio.ini`
  en supprimant ses commentaires — c'est exactement ce que l'item A3 s'emploie à préserver).
- La non-compilabilité du repli est vérifiée **statiquement**, jamais par un build.

## Fichiers potentiellement concernés
- `platformio.ini` — env `[env:esp32dev]`, avertissement de conflit de paquet, renvois croisés
- `src/pump_controller.cpp` (l. 213, 262, 274, 288, 4437) et `.h` (l. 96) — 6 gardes
- `src/main.cpp` (l. 190) — garde watchdog
- `src/oom_guard.cpp` (l. 10) — garde d'include ; le défaut des l. 371/374 disparaît par suppression
- `src/ota_integrity.cpp` (l. 8) — garde `MBEDTLS_VERSION_NUMBER`, décision à trancher (A15)
- `deploy.sh`, `ota_update.sh`, `tools/archive_build.py` — chemins et justifications
- `docs/BUILD.md`, `docs/UPDATE_GUIDE.md`, `docs/subsystems/{pump-controller,mqtt-manager,ws-manager}.md`, `README.md`, `CHANGELOG.md`
- `docs/adr/0069-retrait-repli-core-2.md` [nouveau], amendements ADR-0034 et ADR-0042
- `CLAUDE.md`, `.claude/skills/platformio-workflow/SKILL.md`, `.claude/commands/release.md`, `.claude/agents/{embedded-developer,debugger}.md`

## Livrables — check-list

### A — `platformio.ini` et sources firmware
- [x] A1 — supprimer intégralement le bloc `[env:esp32dev]`
- [x] A2 — supprimer la section « ⚠️ CONFLIT DE PAQUET » en tête de fichier et réécrire la bannière d'en-tête (elle annonce encore un repli disponible)
- [x] A3 — **MIGRER** dans `[env:esp32dev_core3]` tout commentaire n'existant que dans `[env:esp32dev]` et référencé par renvoi. **Zéro perte de contenu.** Renvois à résoudre : PSRAM feature-066 (WROVER-E, `-mfix-*`, CONFIG_SPIRAM_CACHE_WORKAROUND) ; archivage des artefacts (bug-famine-8bits, ELF du 02/08, coredump du 15/08) ; `lib_deps` « MÊMES versions ÉPINGLÉES » (fork ESP32Async : race close/timeout WS, assert lwIP `tcp_update_rcv_ann_wnd` ; note feature-021 Atlas EZO) ; note feature-063 sur les flags `CONFIG_LWIP_*` fantômes (ADR-0011) ; note sur `./build_fs.sh`
- [x] A4 — zéro occurrence résiduelle de `esp32dev` hors `esp32dev_core3` ; `default_envs` conservé ; **l'env n'est pas renommé**
- [x] A5 — `src/pump_controller.cpp:213` (`begin`) : conserver `ledcAttachChannel` + `pumpUsable[i]=false` + CRITICAL + `ledcWriteChannel(0)` ; supprimer `ledcSetup`/`ledcAttachPin`/`ledcWrite`
- [x] A6 — `:262` (`isPumpUsable`) : supprimer la branche « core 2 : toujours utilisable »
- [x] A7 — `:274` (`applyPumpDuty`, entrée) : `if (!pumpUsable[index]) return;` devient inconditionnel et **reste en première ligne**
- [x] A8 — `:288` (`applyPumpDuty`, écriture) : conserver `ledcWriteChannel`, supprimer `ledcWrite`
- [x] A9 — `:4437` (`setManualPump`) : conserver le refus + log throttlé
- [x] A10 — `src/pump_controller.h:96` : `volatile bool pumpUsable[2]` devient membre inconditionnel (commentaire de justification conservé)
- [x] A11 — `src/main.cpp:190` : conserver `esp_task_wdt_reconfigure`, supprimer `esp_task_wdt_init`
- [x] A12 — `src/oom_guard.cpp:10` : conserver `<esp_memory_utils.h>`, supprimer `<soc/soc_memory_layout.h>`. **Ne pas réparer** les l. 371/374 : elles disparaissent par suppression
- [x] A13 — réécrire les commentaires survivants décrivant une alternance disparue, **sans perdre** la justification pool-chemistry P1 du drapeau `pumpUsable`
- [x] A14 — garde de sortie : `grep -rn ESP_ARDUINO_VERSION_MAJOR src/` → 0 ligne
- [x] A15 — trancher le sort de la garde `#if MBEDTLS_VERSION_NUMBER >= 0x03000000` (`src/ota_integrity.cpp:8`). Recommandation : **conserver** (indexée sur une bibliothèque, pas sur l'env abandonné). Décision à consigner dans l'ADR
- [x] A16 — `src/version.h` : `FIRMWARE_VERSION` → `2.42.1`

### B — Outillage et scripts
- [x] B1 — `deploy.sh:15` : l'exemple `PIO_ENV=esp32dev ./deploy.sh all` n'est plus vrai
- [x] B2 — `ota_update.sh:23` : idem
- [x] B3 — vérifier `build_all.sh:6`, `build_fs.sh:11`, `tools/decode_coredump.sh:16` : défaut `esp32dev_core3` déjà correct → ne corriger que les commentaires faux
- [x] B4 — `tools/archive_build.py` (l. 21, 65, 94) : **le cloisonnement par env reste** (ADR-0042) ; seule la justification au présent passe au registre historique
- [x] B5 — supprimer `.pio/build/esp32dev` (11 Mo, non suivi par git)
- [x] B6 — aucun `pio pkg` exécuté ; contrôler `git diff platformio.ini` après toute commande pio
- [x] B7 — consigner que `.github/workflows/tests-natifs.yml` ne construit aucun env matériel → aucune modification CI

### C — Documentation
- [x] C1 — `docs/BUILD.md` §« Les deux environnements firmware » → env unique
- [x] C2 — `docs/BUILD.md:30` : la phrase citant « 3 fichiers (pump_controller.cpp, main.cpp, ota_integrity.cpp) » est **déjà fausse** → réécrire selon l'état final
- [x] C3 — supprimer la section « ⚠️ Conflit de paquet » et ses renvois (`BUILD.md:364`, `README.md:176-178`, `README.md` ~l. 226)
- [x] C4 — `BUILD.md:77` : retirer la ligne `esp32dev (repli)` du tableau de marge flash
- [x] C5 — `:98` « Les deux envs portent les flags PSRAM » → env unique
- [x] C6 — `:139` retirer `PIO_ENV=esp32dev ./build_all.sh`
- [x] C7 — `:198` retirer `PIO_ENV=esp32dev ./deploy.sh all`
- [x] C8 — `:250` `tool-esptoolpy` : la version ne bascule plus ; **conserver** la doc de la sonde `--help` de `deploy.sh`, réécrire la cause
- [x] C9 — `:272, 275-276, 291` cloisonnement par env → registre historique, mécanisme maintenu
- [x] C10 — `:332` retirer le conseil `PIO_ENV=esp32dev` ; **conserver** l'avertissement « un ELF qui ne correspond pas produit des symboles plausibles et FAUX »
- [x] C11 — `:387` note fork ESP32Async → fait de v2.9.0
- [x] C12 — `README.md:166` ; C13 — `:176-178` ; C14 — `:218-226` ; C15 — `:440` ; C16 — `:459`
- [x] C17 — **`docs/UPDATE_GUIDE.md` l. 561, 569, 571-574** : la procédure « Retour arrière vers le core 2 » est fausse et dangereuse → **supprimer**. Conserver et renforcer « changer de bootloader = flash USB » et « le filet otadata a disparu ». **Priorité haute**
- [x] C18 — `docs/subsystems/pump-controller.md:218` : chemin PWM unique. L'invariant « 20 kHz / 8 bits, le duty ne change pas d'échelle » **doit survivre**, sans être formulé comme une égalité au core 2
- [x] C19 — `:236` supprimer « core 2 : retourne toujours true »
- [x] C20 — `:241` « dormante en repli core 2 » → alerte `pump_unusable` armée sans condition
- [x] C21 — `docs/subsystems/mqtt-manager.md:194` supprimer « Reste dormant en repli »
- [x] C22 — `:456` « si une migration future vers ESP-IDF 5.x » : la migration a **eu lieu**. Requalifier le fait, ne rien changer au comportement, noter la ré-isolation du timeout comme piste ouverte hors périmètre
- [x] C23 — `docs/subsystems/ws-manager.md:152` → registre historique (contexte v2.9.0)
- [x] C24 — `CHANGELOG.md` : entrée sous **2.42.1**, rubrique « Interne / maintenance », mention explicite « aucun changement de comportement : binaire fonctionnellement identique, sections `text`/`data`/`bss` inchangées à l'octet », renvoi à l'ADR-0069, et mention de la **disparition de la procédure de retour arrière**
- [x] C25 — vérifier et consigner que `docs/user/src/*.html` ne mentionne ni core 2 ni `esp32dev` → **aucun PDF à régénérer, aucune capture d'écran à refaire**

### D — ADR (immuables : bannière d'amendement, jamais de réécriture)
- [x] D1 — créer `docs/adr/0069-retrait-repli-core-2.md` : les 7 justifications avec chiffres et références de commit ; alternatives écartées (réparer `oom_guard` pour garder le repli compilable ; garder l'env non compilable comme documentation ; renommer `esp32dev_core3`) ; décision A15 ; décision F7 avec son motif technique ; ce que la décision verrouille
- [x] D2 — ADR-0042 : bannière — point 4 de la Décision, alternative « Supprimer `[env:esp32dev]` », dette « Conflit de paquet » et conséquence « gardes bi-core obligatoires » **refermées**
- [x] D3 — ADR-0034 : bannière — phase code bi-core **close** ; case « Binaires de retour arrière archivés » close **sans avoir été honorée**
- [x] D4 — `docs/adr/README.md` : entrée d'index

### E — Specs
- [x] E1 — cette spec, créée puis déplacée en `doing/` par `spec-reader`
- [x] E2 — `specs/features/doing/bug-080-*.md` l. 698-700 : la note « à vérifier si le repli core 2 devait un jour resservir » reçoit sa réponse → **ajouter** une note datée renvoyant à l'ADR-0069, sans réécrire l'existant
- [x] E3 — clôture : déplacer cette spec vers `done/` avec « Notes d'implémentation »
- [ ] E4 — **non-item** : `specs/plan-2026-09-regulation-et-robustesse.md:399` et toutes les specs de `done/` sont des comptes rendus historiques → ne pas y toucher

### F — Configuration du projet (arbitrages tranchés par l'utilisateur le 2026-09-06)
- [x] F1 — `CLAUDE.md` : corriger « Build : PlatformIO (`platform = espressif32@6.9.0`) » qui désigne la plateforme **core 2** ; corriger « 24 suites natives (849 tests) » → 33 suites, 1286 cas ; traiter la référence à `specs/modele.md` **qui n'existe pas dans le dépôt** (créer le fichier ou retirer la mention) ; corriger la description du filesystem (layout v5, 512 Ko) si périmée
- [x] F2 — `.claude/skills/platformio-workflow/SKILL.md` l. 10, 68, 83 : « env unique `[env:esp32dev]` », `pio test -e esp32dev`, chemin `.pio/build/esp32dev/firmware.elf`
- [x] F3 — `.claude/commands/release.md` l. 40-42 : avertissement sur les deux envs
- [x] F4 — `.claude/agents/embedded-developer.md:11` : « PlatformIO espressif32@6.9.0 »
- [x] F5 — `.claude/agents/debugger.md:44` : chemin `.pio/build/esp32dev/firmware.elf`
- [x] F6 — **tranché** : bump `2.42.0` → `2.42.1` (voir A16 et C24)
- [x] F7 — **tranché** : **ne pas désinstaller** `espressif32@6.9.0`. Motif technique dur : `build_fs.sh:22` code en dur `~/.platformio/packages/tool-mklittlefs/mklittlefs`, paquet déclaré par `platforms/espressif32@6.9.0/platform.json` et **absent** de pioarduino 55.03.37. La désinstaller casserait `./build_fs.sh`, `./build_all.sh`, `./deploy.sh fs` et `./deploy.sh ota-fs`. Le ménage disque (~404 Mo) devient une spec séparée dont le prérequis est de rendre `build_fs.sh` indépendant de la plateforme core 2
- [x] F8 — `README.md` : vérifier l'annonce d'un filesystem de « 832 KB » (réalité : 512 Ko, layout v5)

## Sécurité chimique
- Dosage impliqué : **non** — aucun algorithme de dosage modifié
- Limites journalières / horaires concernées : **aucune** (`kMaxRequestsPerMinute`, budget horaire `usedMs`, présence d'eau : intacts)
- Filtration requise avant action : inchangé
- Calibration capteur modifiée : non
- Algorithme PID touché : non
- **Risque identifié** : le retrait supprime la branche core 2 d'`isPumpUsable()`, où une
  pompe était « toujours réputée utilisable, aucune alerte possible ». Le comportement est
  donc **durci**, jamais assoupli. `pool-chemistry` doit confirmer qu'aucune logique ne
  dépendait de cette asymétrie et que l'échelle du duty (20 kHz / 8 bits, 0-255) est
  strictement préservée.

## ADR attendu ?
- **Probable** : oui
- **Critère(s) candidat(s)** : 1, 3, 4
- **Sujet pressenti** : `docs/adr/0069-retrait-repli-core-2.md`

## Éléments visibles (pour doc-writer)

### Endpoints HTTP concernés
- aucun

### Champs WebSocket
- aucun

### Topics MQTT
- aucun changement de contrat. L'alerte `pump_unusable` (feature-064) passe de « armée en
  core 3, dormante en core 2 » à « armée sans condition » — topic, charge utile et cadence
  de republication inchangés

### Documentation à produire / mettre à jour
#### Docs vivantes
- `docs/subsystems/pump-controller.md`, `docs/subsystems/mqtt-manager.md`, `docs/subsystems/ws-manager.md`
- aucune page UI touchée

#### Docs transverses
- `docs/BUILD.md`, `docs/UPDATE_GUIDE.md`, `README.md`, `CHANGELOG.md`
- `docs/API.md` et `docs/MQTT.md` : aucun changement

#### Docs immuables
- `docs/adr/0069-retrait-repli-core-2.md` [nouveau] ; bannières sur ADR-0034 et ADR-0042

## Critères d'acceptation
- [x] `pio run -e esp32dev_core3` **SUCCESS**, 0 nouveau warning
- [x] **Équivalence binaire** : `xtensa-esp32-elf-size` rend exactement
      `text=1463804  data=385478  bss=46459` (baseline du commit `b86813d`). Tout écart,
      même d'un octet, signifie qu'autre chose que du code mort a été retiré → **arrêt**
      et retour à `embedded-developer`. Ne pas comparer `firmware.bin` octet à octet :
      `__DATE__`/`__TIME__` le font varier légitimement — et `A16` change la version
- [x] `grep -rn ESP_ARDUINO_VERSION_MAJOR src/` → **0 ligne**
- [x] `pio test -e native` → **1286 cas verts**, 0 régression
- [x] `git diff platformio.ini` : **aucun commentaire perdu**, renvois inlinés
- [x] Aucun `pio pkg install` / `uninstall` exécuté
- [x] `docs/user/` non régénéré, aucune capture d'écran signalée
- [x] Tous les libellés utilisateur restent en français

## Plan de test manuel
Aucun test matériel possible (LXC sans port série). L'équivalence binaire remplace le
test au banc. Si un flash devient possible plus tard : vérifier au boot que les deux
pompes sont déclarées utilisables et qu'un dosage manuel de test respecte l'échelle de duty.

## Notes pour agent

**Invariants non négociables**
- `PUMP_PWM_FREQ` (20 kHz) et `PUMP_PWM_RES_BITS` (8 bits) inchangés ; échelle du duty
  0-255 et `MAX_PWM_DUTY` strictement préservés
- `if (!pumpUsable[index]) return;` reste en **première ligne** d'`applyPumpDuty`, avant
  le cache `pumpDuty` et toute mutation
- `pumpUsable[]` garde sa sémantique **fail-closed** : posé à `false` uniquement dans
  `begin()`, jamais réarmé à chaud, aucun nouveau chemin d'écriture, index hors bornes
  toujours fail-closed ; `volatile` conservé
- Défense en profondeur de `setManualPump` conservée (refus `maintenanceInhibit`, puis
  refus pompe inutilisable, log throttlé)
- Watchdog : `kWatchdogTimeoutSec = 30`, `trigger_panic = true`, `idle_core_mask = 0`,
  `esp_task_wdt_add(NULL)` — inchangés
- `PIO_ENV` reste le **point d'entrée unique** de l'outillage (ADR-0042) : aucun chemin
  `.pio/build/...` recodé en dur ; cloisonnement par env de `archive_build.py` maintenu
- Aucune ligne de la branche core 3 conservée n'est modifiée « au passage »
- Contrats HTTP / WebSocket / MQTT inchangés ; `data/`, `src/web_*`, `ws_manager.*`,
  `mqtt_manager.*` non modifiés (seule leur doc l'est)

**Sur les tests** — la **validation par mutation est inapplicable** ici et ne doit pas
être simulée : le code retiré n'est que des branches `#else` du préprocesseur, jamais
compilées dans le binaire core 3 ; casser une branche non compilée ne tue aucun test.
Constat vérifié : **aucune suite native ne compile `pump_controller.cpp`** — le
`build_src_filter` de `[env:native]` ne liste que 13 modules de logique pure
(conséquence assumée de l'ADR-0017). Les 1286 tests jouent un rôle de non-régression sur
les modules non touchés, pas de preuve de correction du retrait. La garde rigoureuse est
l'**équivalence binaire**.

**Risque n°1 du cycle** : la perte silencieuse de commentaires dans `platformio.ini`
(item A3). L'env core 3 renvoie explicitement à l'env core 2 pour la PSRAM, l'archivage
et les versions épinglées. Supprimer le bloc source sans inliner d'abord détruirait la
justification du fork ESP32Async, celle des flags `-mfix-*` et celle de l'archivage.

## Hors périmètre
- **Le chemin d'échec de `ledcAttachChannel`** posant `pumpUsable[i] = false` : non testé
  avant ce cycle, non testé après. Le retrait ne dégrade rien mais ne comble pas ce trou.
- **Le trou de détection en CI** : la raison pour laquelle la casse du core 2 est passée
  inaperçue deux versions est que `.github/workflows/tests-natifs.yml` ne construit aucun
  env matériel. Après ce cycle cette classe de défaut disparaît pour le core 2, mais l'env
  core 3 reste sans garde CI. Rattaché au constat ouvert du 2026-09-06 ; à traiter par une
  spec propre, pas à absorber ici.
- **Le ménage disque** des ~404 Mo du core 2 : prérequis = rendre `build_fs.sh`
  indépendant de `tool-mklittlefs` (voir F7).
- **La ré-isolation de `TCP_USER_TIMEOUT`** rendue possible par IDF 5 (voir C22) : piste
  ouverte, aucun changement de comportement dans ce cycle.

## Notes d'implémentation (clôture — 2026-09-06, v2.42.1)

**Statut** : livré — `pool-chemistry` **GO**, `test-engineer` **PASS**, `code-reviewer` **Approved**.

### Ce qui a été livré

| Bloc | État | Détail |
|---|---|---|
| A — `platformio.ini` + sources | ✅ | Bloc `[env:esp32dev]` et section « CONFLIT DE PAQUET » supprimés, commentaires migrés dans `[env:esp32dev_core3]` (PSRAM feature-066, archivage, versions épinglées ESP32Async, note `build_fs.sh`) ; 8 gardes retirées ; `FIRMWARE_VERSION` = `2.42.1` |
| B — outillage | ✅ | `deploy.sh`, `ota_update.sh`, `tools/archive_build.py`, `tools/decode_coredump.sh` ; cloisonnement d'archivage par env **maintenu** ; CI non modifiée (elle ne bâtit aucun env matériel) |
| C — documentation | ✅ | `BUILD.md`, `UPDATE_GUIDE.md` (**procédure de retour arrière supprimée**), `README.md`, `CHANGELOG.md` (2.42.1), `subsystems/{pump-controller,mqtt-manager,ws-manager}.md` |
| D — ADR | ✅ | [ADR-0069](../../../docs/adr/0069-retrait-repli-core-2.md) créé ; bannières d'amendement sur [ADR-0042](../../../docs/adr/0042-core3-environnement-par-defaut.md) et [ADR-0034](../../../docs/adr/0034-migration-core3-idf5.md) ; index `docs/adr/README.md` |
| E — specs | ✅ | Note datée ajoutée dans `bug-080` § « Branche non couverte par le build » ; `specs/plan-2026-09-*` et `done/` **non touchés** |
| F — configuration | ✅ | `CLAUDE.md`, `.claude/skills/platformio-workflow/SKILL.md`, `.claude/commands/release.md`, `.claude/agents/{embedded-developer,debugger}.md`, `README.md` (taille FS) |

### Décisions tranchées et consignées dans l'ADR-0069

- **A15** — la garde `#if MBEDTLS_VERSION_NUMBER >= 0x03000000` d'`ota_integrity.cpp` est **conservée** : indexée sur le contrat d'une bibliothèque tierce, pas sur l'environnement retiré.
- **F7** — `espressif32@6.9.0` **n'est pas désinstallée** : elle fournit `tool-mklittlefs`, déclaré `null` par pioarduino 55.03.37 et référencé en dur par `build_fs.sh:22`. Le ménage disque (~404 Mo) est une tâche séparée, prérequis = rendre `build_fs.sh` indépendant.
- **Méthode de validation** — la mutation est **inapplicable** (branches `#else` jamais compilées) et **n'a pas été simulée**. Garde retenue : **équivalence binaire** (`text`/`data`/`bss` identiques à l'octet) + diff de table des symboles `nm --print-size` contre l'ELF archivé : **11 124 symboles, diff vide**. Les 77 octets d'écart du `.bin` sont tous imputés. ⚠️ Mesure valable **uniquement après un build propre**.

### Constats ouverts, hors périmètre

- **L'env core 3 reste sans garde CI** : le trou de détection qui a laissé passer la casse du core 2 pendant deux versions existe encore. Spec dédiée à écrire.
- **Ménage disque** de la plateforme core 2 (~404 Mo), conditionné à F7.
- **Ré-isolation de `TCP_USER_TIMEOUT`** rendue possible par IDF 5 : piste ouverte, aucun changement de comportement.
- **Chemin d'échec de `ledcAttachChannel`** (`pumpUsable[i] = false`) : toujours non testé, ni avant ni après.
- ~~**`specs/modele.md` et `specs/modele_iteration.md`** référencés mais absents~~ → **tranché le 2026-09-06 : les mentions sont retirées définitivement.** Il n'existe plus qu'un template, `specs/features/_template.md`, allégé pour un sujet petit. Nettoyé dans `CLAUDE.md` (arborescence, § « Source de vérité », § « Workflow imposé »), `.claude/agents/spec-reader.md` (front-matter et critères de complétude) et `.claude/skills/spec-workflow/SKILL.md` (arborescence et procédure bug/refactor). Les 24 specs de `done/` qui citent l'ancien chemin sont des **comptes rendus historiques** : intactes.
