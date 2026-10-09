# Guide de Mise à Jour

> ⚠️ **Première installation obligatoire via USB** : le bootloader et la table de partitions personnalisée (`partitions.csv`) ne peuvent être flashés que via USB. Une fois cette installation initiale effectuée, toutes les mises à jour suivantes peuvent se faire en OTA (WiFi) — **sauf un changement de bootloader ou de table de partitions**, qui repasse par l'USB (voir [Changer de bootloader](#changer-de-bootloader--flash-usb-obligatoire-core-3-par-défaut-v2204)).

---

## Méthodes de mise à jour

| Méthode | Compile | USB requis | WiFi requis | PSRAM requise |
|---------|:-------:|:----------:|:-----------:|:-------------:|
| `deploy.sh [firmware\|fs\|all]` | ✅ | ✅ | ❌ | ❌ |
| `deploy.sh [ota-firmware\|ota-fs\|ota-all]` | ✅ | ❌ | ✅ | ❌ |
| Interface web (OTA) | ❌ | ❌ | ✅ | ❌ |
| Mise à jour automatique GitHub | ❌ | ❌ | ✅ | ✅ *(voir ci-dessous)* |

---

## Mise à Jour via `deploy.sh` — compile + envoie en une commande

`deploy.sh` compile le firmware et le filesystem, puis envoie la mise à jour. Il supporte deux modes d'envoi.

### Mode USB — connexion série requise

Utile pour la première installation ou si l'ESP32 n'est pas accessible sur le réseau.

```bash
./deploy.sh all       # Firmware + filesystem
./deploy.sh firmware  # Firmware uniquement
./deploy.sh fs        # Filesystem uniquement
```

**Le port série est détecté automatiquement** : port historique `/dev/cu.usbserial-0001` s'il existe, sinon le premier port USB-série trouvé (`usbserial`, `wchusbserial`, `SLAB_USBtoUART`). Pour forcer un port précis (plusieurs modules branchés, adaptateur qui n'énumère pas sous le nom attendu) :

```bash
PORT=/dev/cu.usbserial-10 ./deploy.sh all
```

Si aucun port n'est détecté, le script s'arrête **avant de compiler** et liste les ports disponibles.

**Le moniteur série est enchaîné après l'upload.** L'ouverture du port redémarre l'ESP32 (bascule DTR/RTS) : le boot est donc visible **depuis sa première ligne** — utile pour lire l'IP obtenue, la cause du dernier reset ou une erreur d'init de sonde. `Ctrl+C` pour quitter, ou pour ne pas l'enchaîner du tout :

```bash
NO_MONITOR=1 ./deploy.sh all
```

### Récupérer le mot de passe WiFi AP

Le mot de passe du point d'accès `PoolControllerAP` est généré au premier boot puis stocké en NVS. Il est affiché en clair dans les logs **à sa génération**, puis à chaque démarrage de l'AP **tant que l'assistant de configuration n'est pas complété**. Une fois l'assistant terminé, les logs n'affichent plus que :

```
AP Password: [voir etiquette ou GET /auth/ap-password]
```

Autrement dit, sur un appareil déjà configuré, ouvrir `pio device monitor` et redémarrer **ne le fera jamais réapparaître**.

Deux voies selon la situation :

| Situation | Marche à suivre |
|---|---|
| L'appareil est **joignable** sur le réseau | `curl http://poolcontroller.local/auth/ap-password` (voir [API.md](API.md)) |
| Étiquette perdue **et** appareil injoignable | `./deploy.sh factory` — régénère le mot de passe et l'affiche |

```bash
./deploy.sh factory
```

Le script demande confirmation, efface la flash, réinstalle firmware + filesystem, **ré-efface la NVS en dernier**, puis ouvre le moniteur. Le mot de passe s'affiche dans les premières lignes de boot :

```
=== MOT DE PASSE AP WIFI: XXXXXXXX ===
```

⚠️ **C'est la seule occasion de le lire** — le noter immédiatement sur une étiquette collée au boîtier.

⚠️ `factory` efface **toute** la configuration : WiFi, MQTT, mot de passe admin, offset pH, calibrations pH/ORP, historique. À réserver à un appareil qu'on réinitialise volontairement. Pour une simple mise à jour, utiliser `./deploy.sh all`, qui préserve la NVS.

> Détail du séquencement (et pourquoi la NVS est effacée en **dernier**) : voir [BUILD.md](BUILD.md#ordre-du-mode-factory-et-pourquoi-il-compte).

### Mode OTA — WiFi, pas d'USB

```bash
./deploy.sh ota-all       # Firmware + filesystem via WiFi
./deploy.sh ota-firmware  # Firmware uniquement via WiFi
./deploy.sh ota-fs        # Filesystem uniquement via WiFi
```

Le script demande le mot de passe admin au lancement (ou via `POOL_PASSWORD=... ./deploy.sh ota-all`).

Par défaut, le script contacte l'ESP32 via mDNS (`poolcontroller.local`). Si le mDNS ne fonctionne pas sur votre réseau (certains CPL ou routeurs ne le relaient pas), utilisez directement l'adresse IP :

```bash
./ota_update.sh both 192.168.1.42 monmotdepasse
./ota_update.sh firmware 192.168.1.42 monmotdepasse
```

> L'adresse IP de l'ESP32 est visible dans **Paramètres > Système** ou dans les logs au démarrage (`INFO: IP: 192.168.x.x`).

---

## Mise à Jour via Interface Web — WiFi uniquement

1. Compiler les fichiers avec `./build_all.sh`
2. Accéder à http://poolcontroller.local
3. Aller dans **Paramètres > Système**, carte **Mise à jour manuelle**
4. Sélectionner le fichier (`firmware.bin` ou `littlefs.bin`)
5. **Vérifier le type annoncé sous le sélecteur** — depuis la v2.20.1, le type est déduit
   automatiquement du nom du fichier et le sélecteur est pré-réglé. Une notice bleue le
   confirme (« Type réglé sur « Système de fichiers » d'après le nom du fichier
   (littlefs.bin) »). Si vous changez le type et qu'il ne correspond plus au fichier, la
   notice passe en **orange** et une confirmation est demandée au moment d'envoyer — vous
   pouvez toujours passer outre en un clic, rien n'est bloqué.
6. Cliquer sur **Mettre à jour**
7. Attendre le redémarrage automatique

> Si vous modifiez le code ET les fichiers web, faire 2 mises à jour séparées.

> ⚠️ **Pourquoi cette vérification du type existe** : jusqu'à la v2.20.0, le sélecteur
> revenait à « Firmware » à **chaque rechargement de page** — donc juste après un flash de
> firmware réussi, qui redémarre l'appareil et recharge l'interface. Envoyer `littlefs.bin`
> dans la foulée l'expédiait déclaré « firmware », et l'échec était **indéchiffrable**
> (voir ci-dessous).

### Lire un échec de mise à jour manuelle — depuis v2.20.1

Un échec est désormais **nommé**, à deux endroits, avec le même texte :

- **Dans l'interface**, sous la barre de progression (`Échec : …`), en rouge.
- **Dans le Journal** (Paramètres > Avancé > Logs) : **une seule** ligne
  `ERROR Échec mise à jour OTA : <cause>` (ou `CRITICAL Mise à jour OTA refusée : <cause>`
  pour un refus d'intégrité).

> ℹ️ **Le journal n'est plus noyé.** Avant la v2.20.1, une tentative ratée écrivait la
> même ligne ~200 fois et **effaçait tout l'historique du journal** (anneau de 200 entrées),
> y compris les lignes qui désignaient le coupable — et y compris les traces d'un tout autre
> incident en cours d'analyse. Une tentative ratée écrit désormais **une** ligne.

**Causes les plus fréquentes :**

| Ce que vous lisez | Ce que ça veut dire | Que faire |
|---|---|---|
| « l'image reçue n'est pas un firmware ESP32 (octet magique 0x… au lieu de 0xE9) — type « Système de fichiers » attendu ? » | Vous avez envoyé une image **filesystem** déclarée **firmware** | Repasser le type sur **Système de fichiers** et renvoyer. **Aucun effet de bord** : rien n'a été écrit, les pompes n'ont pas été inhibées, l'appareil n'a pas redémarré. |
| « démarrage de la mise à jour impossible (…) » | La partition cible refuse le flash (place insuffisante, image trop grosse) | Vérifier la taille du binaire ; pour le FS, utiliser `./build_fs.sh` (taille alignée sur la partition) |
| « échec d'écriture de l'image … (…) » | Erreur de flash en cours de transfert | Relancer ; si cela se répète, passer par l'USB (`./deploy.sh fs` / `firmware`) |
| « empreinte SHA-256 non conforme … » | L'image reçue ne correspond pas à l'empreinte fournie | Retélécharger le binaire, relancer |
| « finalisation de la mise à jour impossible (…) » | Image incomplète ou tronquée à la clôture | Relancer le transfert |

**Après un échec de mise à jour du *filesystem*** : le contrôleur **remonte** la partition
automatiquement pour que l'interface web reste servie. Si vous voyez dans le journal
`CRITICAL Filesystem remonté après une mise à jour FS interrompue : contenu potentiellement
incohérent, relancer la mise à jour`, **relancez la mise à jour du filesystem** : une partie
de l'ancienne image a pu être écrasée, l'interface peut être partiellement incohérente
(pages anciennes et nouvelles mélangées). Le firmware et la régulation, eux, ne sont pas
affectés. Si le message est `WARN Impossible de remonter le filesystem … — redémarrage
requis`, redémarrer le contrôleur puis relancer la mise à jour.

**En ligne de commande**, `./ota_update.sh` affiche le corps de la réponse en cas d'échec
et ne considère un succès que sur un **HTTP 200 avec un corps exactement `OK`** (le test
précédent, non ancré, laissait passer pour un succès tout corps contenant « OK »).

---

## Mise à Jour Automatique depuis GitHub — WiFi uniquement

L'interface web peut vérifier et télécharger automatiquement la dernière version depuis GitHub :

1. Accéder à **Paramètres > Système**
2. Cliquer sur **Vérifier les mises à jour** — consulte la dernière release GitHub
3. Si une nouvelle version est disponible, cliquer sur **Mettre à jour** — télécharge et installe automatiquement

> Le contrôleur se connecte directement à GitHub en HTTPS. Aucun USB, aucune manipulation de fichiers.

> ⚠️ **Module avec PSRAM requis — sans PSRAM, la méthode est ACTIVEMENT REFUSÉE
> (depuis la v2.18.6)** : la session TLS vers GitHub réclame plusieurs blocs de
> mémoire contigus d'environ 16 Ko. Sur un module **sans PSRAM** (WROOM-32), la RAM
> interne ne les fournit plus de façon fiable après le démarrage — pire, la
> tentative *affamait* la mémoire au point de mettre tout le contrôleur en risque de
> redémarrage pendant environ une seconde (mesuré en production le 12/08/2026).
>
> Les **deux étapes** sont désormais refusées immédiatement, sans qu'aucune connexion
> ne soit ouverte : **« Vérifier les mises à jour »** (`/check-update`) **et**
> **« Mettre à jour »** (`/download-update`). Un refus n'a **aucun effet de bord** :
> rien n'est effacé, la version installée reste intacte.
>
> **Trois situations distinctes** (v2.18.7) — le message affiché indique laquelle :
>
> | Ce que vous lisez | Ce que ça veut dire | Que faire |
> |---|---|---|
> | « La mise à jour en ligne n'est pas disponible. Votre matériel ne dispose pas d'une mémoire PSRAM (module ESP32-DevKitC-VE nécessaire). Utilisez la mise à jour manuelle. » | **Définitif** : votre module n'a physiquement pas de PSRAM (WROOM-32). Réessayer ne changera rien. | Mise à jour **manuelle** (ci-dessous), ou remplacer le module par un ESP32-DevKitC-VE / WROVER-E |
> | « La mise à jour en ligne est momentanément indisponible : la mémoire PSRAM est saturée. Réessayez dans quelques minutes ou utilisez la mise à jour manuelle. » | **Temporaire** : le module a bien de la PSRAM, mais il en reste moins de 1 Mo de libre à cet instant. | **Réessayer** dans quelques minutes (un redémarrage du contrôleur libère la PSRAM), sinon mise à jour manuelle |
> | « Mémoire insuffisante pour TLS, réessayez » | **Temporaire** : la mémoire interne du contrôleur est momentanément fragmentée/chargée. | **Réessayer** un peu plus tard, sinon mise à jour manuelle |
>
> Dans les trois cas la réponse est un **503** affiché en toast **informatif** (ce n'est
> pas une panne du contrôleur). Codes machine correspondants, pour un client HTTP :
> `psram_required`, `psram_low`, `tls_low_memory`. Journal : `WARN /check-update refusé
> (PSRAM libre absente/insuffisante) : PSRAM libre=0 o (min 1048576), …` — les logs
> restent techniques, destinés au diagnostic.
>
> **Depuis la v2.18.7, l'indisponibilité permanente est annoncée AVANT tout clic** : à
> l'ouverture de **Paramètres > Système**, l'interface lit la PSRAM du module et, si
> elle est **absente**, **grise le bouton « Vérifier »** et affiche une explication
> permanente dans la carte « Mise à jour en ligne (GitHub) » qui renvoie vers la carte
> « Mise à jour manuelle ». Il n'est donc plus nécessaire de déclencher un refus pour
> savoir que la méthode n'est pas disponible ; le toast ne subsiste que comme filet de
> sécurité. Les deux refus **temporaires** ne grisent rien : le bouton reste
> utilisable pour réessayer.
>
> **Repli commun aux trois cas** — utiliser la **mise à jour manuelle par upload**
> (section précédente : Paramètres > Système, `firmware.bin` / `littlefs.bin`) ou
> `./deploy.sh ota-firmware` / `./deploy.sh ota-fs`. Ces méthodes n'ouvrent **aucune
> session TLS sortante** et fonctionnent quel que soit le module. Les binaires de
> chaque release restent téléchargeables depuis GitHub sur un ordinateur.
>
> Le module du BOM PCB v2 (**ESP32-DevKitC-VE / WROVER-E, 8 Mo de PSRAM**) place ces
> allocations en PSRAM et rend la mise à jour en ligne disponible. Détail de la garde
> et des seuils : [ADR-0036](adr/0036-psram-requise-mise-a-jour-en-ligne.md),
> [docs/API.md](API.md#garde-tls-sortante-psram-libre--heap-v2186).
>
> Si le message affiché est « **Mémoire insuffisante pour TLS, réessayez** » (sans
> mention de PSRAM), il s'agit d'un refus **temporaire** sur module PSRAM : la mémoire
> était momentanément dégradée, réessayer plus tard suffit.

---

## Vérification d'intégrité SHA-256 — depuis v2.11.0

Depuis la v2.11.0, **toute image téléchargée depuis GitHub est vérifiée par empreinte SHA-256 avant d'être activée** ([ADR-0022](adr/0022-verification-integrite-ota.md)). L'empreinte de référence est le champ `digest` publié par GitHub pour chaque asset de release — aucune manipulation supplémentaire n'est requise pour un OTA nominal.

**Cas d'un échec de vérification (image tronquée ou corrompue, coupure WiFi pendant le téléchargement) :**

- **Firmware** : le flash est refusé **avant activation** — la carte reste sur la version actuelle, aucun redémarrage sur l'image suspecte. Message d'erreur explicite dans l'UI, log `CRITICAL` dans la page Logs. Relancer simplement l'installation.
- **Filesystem** : la partition de l'interface web contient une image suspecte — **pas de redémarrage automatique**, le firmware reste intact et bootable. **Réinstaller le filesystem** (relancer la mise à jour GitHub, ou `./deploy.sh ota-fs` / `./deploy.sh fs`). Tant que ce n'est pas fait, l'interface web peut être partiellement indisponible (le firmware et la régulation continuent de fonctionner).

**Cas d'une release sans empreinte (anciennes releases) :** le bouton **Installer** est **désactivé** avec le message « Cette release ne fournit pas d'empreinte d'intégrité — installation refusée (sécurité). » (comportement fail-closed voulu). Utiliser l'upload manuel ou `deploy.sh` pour installer une telle release.

**Upload manuel (`/update`, interface web ou curl) :** l'empreinte SHA-256 de l'image reçue est toujours calculée et loggée. Il est possible de fournir l'empreinte attendue via le champ POST optionnel `sha256` (voir [docs/API.md](API.md#post-update--critical)) — en cas de différence, le flash est refusé. Sans empreinte fournie, le flash reste autorisé (vous êtes physiquement présent). Un seul upload à la fois : un second upload lancé pendant qu'un premier est en cours est refusé (`409`).

---

## Notes de migration

### Migration layout v4 → v5 (flash 8 Mo) — depuis 2026-09-05

⛔ **Mise à jour par câble USB obligatoire. L'OTA ne peut PAS appliquer ce changement.**
⚠️ **L'historique des mesures est effacé. Le sauvegarder AVANT le flash, appareil encore en ligne.**

#### Ce qui change et pourquoi

La table de partitions couvre désormais **la totalité de la flash**
([ADR-0068](adr/0068-partitions-layout-v5-flash-8-mo.md)). Jusqu'ici elle s'arrêtait à
`0x400000` et `platformio.ini` déclarait `flash_size = 4MB` — **mais la puce est une 8 Mo**
(`esptool flash_id` : ESP32-D0WD-V3, `Device 4017`, octet de capacité `0x17` = 2²³ =
8 388 608 o). **4 Mo n'étaient référencés par aucune partition** : ni lus, ni écrits, ni
comptés.

| Partition | v4 | v5 |
|---|---|---|
| `app0` | `0x010000`, 1792 Ko | `0x010000`, **3648 Ko** |
| `app1` | `0x1D0000`, 1792 Ko | **`0x3A0000`**, **3648 Ko** |
| `spiffs` | `0x390000`, 320 Ko | **`0x730000`**, **512 Ko** |
| `history` | `0x3E0000`, 64 Ko | **`0x7B0000`**, **256 Ko** |
| `coredump` | `0x3F0000`, 64 Ko | **`0x7F0000`**, 64 Ko |
| Fin de table | `0x400000` (4 Mo) | **`0x800000` (8 Mo, pile)** |

Effet mesuré : l'occupation du firmware passe de **99,5 %** (9 291 octets de marge) à
**48,9 %** (1 909 835 octets). La partition `history` quadruple.

#### Ce qui est préservé, ce qui ne l'est pas

✅ **La configuration est intégralement préservée.** La partition `nvs` reste à `0x9000`,
**hors de la zone déplacée** : consignes pH/ORP, offset pH, taille de bassin, calibrations
des sondes, identification des DS18B20, WiFi, compteurs journaliers, mot de passe.
**Rien à reconfigurer.**

❌ **`spiffs`, `history` et `coredump` changent d'offset : leur contenu est perdu.** Pour
`history`, l'effacement est en plus **explicite** — `HistoryManager` compare la taille de
partition à la valeur mémorisée en NVS et efface sur écart, pour éviter un crash LittleFS.
L'historique des mesures **repart de zéro**.

#### Procédure

**1. Sauvegarder l'historique, appareil encore en ligne.** Cette étape est irréversible si
on l'oublie. Depuis Paramètres → Système, exporter `history.json` (et, tant qu'à faire,
`config.json` et le journal système).

**2. Vérifier la taille physique de la flash** (ne pas se fier à la déclaration) :

```bash
esptool.py flash_id
# Attendu : Detected flash size: 8MB
```

**3. Flasher par USB :**

```bash
./deploy.sh all
```

Recompile firmware + filesystem, réécrit bootloader + table + application, flashe le FS au
nouvel offset `0x730000`, et efface `otadata` pour forcer le démarrage sur `app0` — étape
**indispensable** après un déplacement de partitions, l'`otadata` héritée pointant vers un
slot qui n'est plus au même endroit.

> ⚠️ **NE PAS utiliser `./deploy.sh factory`** : il efface toute la flash, **NVS comprise**
> (perte de la config, des calibrations, du WiFi, régénération du mot de passe AP).
> `./deploy.sh all` suffit et préserve la NVS.

> ⛔ **Ne JAMAIS pousser ce firmware ou ce filesystem par OTA sur un appareil encore en
> table v4.** L'OTA n'écrit ni la table ni le bootloader : elle resterait v4 même après une
> « réussite » apparente, et une image FS construite pour 512 Ko sur une partition de
> 320 Ko présente des métadonnées LittleFS discordantes — interface web morte ou corrompue.

**4. Réimporter l'historique — depuis l'interface, quelle que soit sa taille (v2.48.0).**

Paramètres → **Avancé** → « Historique des mesures » → choisir le fichier CSV, puis
**« Remplacer l'historique »**. L'interface **découpe elle-même** l'envoi en lots et
affiche une barre de progression : il n'y a plus de taille maximale de sauvegarde.

> 🛑 **Ne PAS découper le fichier à la main, et surtout pas avec `curl` sur
> `/history/import` sans `mode`.** L'ancienne version de ce guide conseillait des
> « lots de 145 points qui se cumulent » : **les lots ne se cumulaient pas**, chacun
> **écrasait** le précédent (`memoryBuffer = dataPoints;`) et seul le **dernier**
> survivait, sans aucun message. C'est la cause des **100 points perdus** de la
> migration du 05/09 (245 sauvegardés, 145 réimportés, les plus anciens du 23 au
> 31 août restés dans le fichier).
>
> Depuis la v2.48.0 un découpage manuel est possible, mais **uniquement** en posant
> `"mode":"append"` sur les lots suivants, et sous **8 192 octets** de corps par
> requête (le plafond a été **abaissé**, cf. [API.md](API.md#post-historyimport--write)).
> L'interface fait tout cela seule ; s'en passer n'apporte rien.

> ⚠️ **Rechargez la page (Ctrl-Maj-R) après avoir flashé le filesystem** avant
> d'importer. Une interface restée en cache ignore le découpage et se heurte au
> nouveau plafond de 8 192 o — elle recevra un `413`.

**Si l'import s'interrompt** (coupure Wi-Fi) : les lots déjà envoyés **restent** dans
l'appareil. Un bouton **« Reprendre l'import »** apparaît et repart du lot interrompu ;
les créneaux déjà présents sont réécrits à l'identique, donc **aucun doublon**. Tant
qu'une reprise est en attente, « Remplacer » et « Compléter » sont **désactivés** —
cliquer « Remplacer » à cet instant détruirait précisément l'import partiel qu'on
cherche à finir. **« Abandonner la reprise »** est la sortie explicite : elle
n'efface rien, elle renonce seulement aux lots restants.

#### Vérifications post-flash

Paramètres → Système → Infos système :

- **Flash** : `8 388 608` octets ;
- ligne **Firmware (flash)** : partition de **3 648 Ko**, occupation ≈ **49 %** ;
- ligne **FS** : **512 Ko** ;
- journal de démarrage : `LittleFS monté avec succès`, `Partition historique dédiée montée`,
  `Historique chargé (N points)` ;
- l'OTA (firmware et filesystem) refonctionne normalement dès le premier boot.

#### Une commande de dépannage a changé d'adresse

L'effacement manuel de secours de la partition `history` devient :

```bash
esptool.py erase_region 0x7B0000 0x40000   # partition history (256 Ko), layout v5
```

⛔ **Ne plus lancer `erase_region 0x3E0000 0x10000`** (ancienne commande, layout v4) : cette
zone est désormais **au milieu de `app1`**. La commande n'effacerait pas l'historique mais
détruirait le slot OTA de secours.

---

### L'attente après un arrêt d'eau ne juge plus la stabilité de la mesure — depuis v2.62.0 (2026-10-08)

**Aucune action requise.** Pas de changement de partition ni de format de configuration.

Depuis la v2.34.0, la purge de reprise d'eau se terminait quand les mesures pH/ORP **cessaient de bouger** (plancher 10 min, plafond 90 min). Ce mécanisme est **retiré** ([ADR-0092](adr/0092-retrait-analyse-stabilite-porte-mesures.md)) : l'appareil juge désormais le **temps**, pas la variation de la mesure.

- **Au redémarrage de la filtration**, l'injection reprend après un plancher **fixe** de 10 minutes — sauf si l'arrêt d'eau qui précède a duré **moins de 3 minutes** (nettoyage rapide, petite coupure), auquel cas l'injection reprend **immédiatement**.
- **Ce qui disparaît** : la re-fermeture en cours de route si la mesure recommençait à bouger, et le plafond de 90 minutes avec son alerte. Avant ce correctif, une mesure qui continuait de fluctuer légèrement pouvait refermer la porte pour 20 minutes **sans aucun arrêt de filtration à l'origine** — mesuré à **65 % du temps de blocage** d'une demi-journée observée le 08/10/2026.
- **Ce qui ne change pas** : les butées de sécurité pH (brute et corrigée), les limites journalière et horaire, l'inhibition du chlore sur pH haut, la pause de mélange.
- **Le bandeau « Stabilisation des mesures non atteinte »** des pages pH et ORP, et l'alerte MQTT `measures_purge_timeout` qui l'accompagnait, **disparaissent** — ce cas ne peut plus se produire. Voir [MQTT.md](MQTT.md#alertes) pour l'impact sur une automation Home Assistant existante.
- **Les messages affichés annoncent désormais quand l'injection reprend** (« Reprise automatique dans N min ») plutôt que depuis quand elle est suspendue — et la chloration de secours n'est plus proposée pendant cette attente ordinaire (elle reste proposée sur les vraies pannes de sonde).

Détail des décisions : [ADR-0092](adr/0092-retrait-analyse-stabilite-porte-mesures.md) (retrait de l'analyse de stabilité, supersède l'[ADR-0059](adr/0059-purge-adaptative-fenetre-de-medianes.md) ci-dessous).

---

### La régulation attend que l'eau soit rincée, et un redémarrage ne coûte plus l'apprentissage — depuis v2.34.0 (2026-09-04)

> ⚠️ **Section historique, partiellement obsolète depuis la v2.62.0 ci-dessus.** Le mécanisme de « stabilité » décrit ici (plancher 10 min / plafond 90 min sur la variation de la mesure) a été **retiré**. Le bandeau « Stabilisation des mesures non atteinte » et l'alerte `measures_purge_timeout` mentionnés plus bas **ne se produisent plus**. Conservé pour mémoire du changement v2.34.0 (le plancher de 10 min et le principe « redémarrage ne coûte pas l'apprentissage » restent valables).

**Aucune action requise.** Pas de changement de partition ni de format de configuration. **Un seul effet de bord, une seule fois** : voir l'encadré ci-dessous.

> ⚠️ **Au tout premier démarrage de la v2.34.0, la « réserve de correction » du pH repart de zéro.**
>
> Le format de sa sauvegarde a changé (un champ s'y ajoute) et l'ancienne sauvegarde est **volontairement rejetée** plutôt que réinterprétée — une donnée relue avec le mauvais format vaudrait n'importe quoi. Le journal l'indique en clair (« aucun instantané NVS exploitable … départ de zéro »). La réserve se reconstruit ensuite normalement, et **à partir du redémarrage suivant, elle survit** (c'est tout l'objet de cette version). Aucun réglage, aucune calibration, aucun historique n'est touché.

#### Le réglage « délai de stabilisation » a disparu

Il n'apparaît plus dans **Paramètres → Régulation** ni dans l'assistant de premier démarrage : la purge des mesures se termine désormais **quand la mesure est stable**, entre un plancher de **10 min** et un plafond de **90 min**.

- **Si vous aviez modifié ce délai** (par exemple porté à 15 min en septembre), votre valeur est **simplement ignorée** — elle reste stockée mais sans effet. Rien à faire.
- **Un client ancien qui enverrait encore ce réglage** ne reçoit pas d'erreur : la clé est acceptée sans effet.
- **À la place**, la ligne **« Dernière purge »** (Paramètres → Régulation) indique la durée **mesurée** de la dernière purge et son motif de fin :

| Ce qui s'affiche | Ce que ça veut dire |
|---|---|
| `12 min — stabilité atteinte` | cas nominal |
| `dispensée — coupure brève (≤ 10 s)` | *(historique v2.34.0, voir double correction ci-dessous)* l'appareil a redémarré tout seul en moins de 10 s, l'eau n'a pas stagné |
| `8 min — relancée, la mesure a bougé` | la mesure est redevenue instable après ouverture, une nouvelle purge a été faite |
| `90 min — plafond atteint, mesures non stabilisées` | ⚠️ la mesure n'a jamais cessé de bouger — voir ci-dessous |

> ⚠️ **Double correction depuis la v2.62.0** (ce tableau décrit l'état v2.34.0, conservé pour
> mémoire) : (1) le seuil de 10 s n'est plus d'actualité — il vaut désormais **180 s (3 min)**
> (`kWaterStopGraceS`) ; (2) la dispense ne dépend plus d'un **redémarrage** : un simple arrêt
> d'eau court en marche continue (filtration jamais coupée) dispense aussi, via le même chemin
> de décision. Voir la section « [L'attente après un arrêt d'eau ne juge plus la stabilité de
> la mesure](#lattente-après-un-arrêt-deau-ne-juge-plus-la-stabilité-de-la-mesure--depuis-v2620-2026-10-08) »
> plus haut pour le comportement actuel. Les lignes `90 min — plafond atteint` et
> `relancée, la mesure a bougé` de ce tableau **ne peuvent plus se produire** depuis la v2.62.0
> (motifs retirés, réservés).

#### Ce qu'il faut observer le premier matin

1. **Le badge des cartes pH et chlore** affiche « **Stabilisation depuis N min** » (et non plus un compte à rebours) tant que l'eau se rince. Comptez **10 à 50 minutes** après le démarrage de la filtration selon la durée de stagnation de la nuit — c'est **normal et voulu** : avant, l'appareil rouvrait les mesures **au milieu** du rinçage et déclenchait des alertes pH à tort.
2. **Aucune alerte pH ne devrait apparaître** pendant cette phase (c'était le symptôme des 02/09 et 04/09).
3. **La réserve de correction** doit être **retrouvée** (et non remise à zéro) après un redémarrage survenu pendant la filtration.

#### Si le bandeau « Stabilisation des mesures non atteinte » s'affiche

> ⚠️ **Ce bandeau, son déclencheur (plafond de 90 minutes) et l'alerte MQTT qu'il citait sont
> retirés depuis la v2.62.0** — ce sous-chapitre ne peut plus se produire (voir la section
> « L'attente après un arrêt d'eau ne juge plus la stabilité de la mesure » en tête de ce
> fichier). Conservé pour mémoire du comportement v2.34.0 uniquement.

Il apparaissait sur les pages **pH** et **ORP** quand la purge atteignait son plafond de 90 minutes sans que la mesure se stabilise. **La régulation reprenait** (l'appareil ne restait jamais bloqué en silence), mais sous surveillance :

- vérifier la **chambre de mesure** : bulles d'air, débit insuffisant, encrassement ;
- vérifier les **sondes** : ancienneté, calibration ;
- une alerte MQTT `measures_purge_timeout` était publiée en parallèle (utile pour une automation Home Assistant).

> Un bandeau isolé après une longue coupure n'a rien d'alarmant. **Répété plusieurs matins de suite, il signale un vrai problème d'hydraulique ou de sonde.**

#### Le reste est inchangé

Limites journalière et horaire, seuils de plancher pH, butées sur la mesure brute, inhibition du chlore sur pH haut, pause de mélange, plafond de la réserve de correction : **tout reste en place**. La valeur restaurée n'est **jamais** appliquée au démarrage — elle attend 5 minutes d'écart confirmé par la mesure, exactement comme depuis la v2.27.0. **Régulation ORP (chlore) : inchangée.**

> Firmware **et** filesystem doivent être mis à jour **ensemble** : la ligne « Dernière purge » et les bandeaux sont portés par l'interface. Un filesystem ancien sur firmware 2.34 affiche simplement « — » et masque les bandeaux (rien n'est inventé) ; un filesystem 2.34 sur firmware ancien fait de même.

Détail des décisions : [ADR-0059](adr/0059-purge-adaptative-fenetre-de-medianes.md) (purge adaptative, retrait du réglage) et [ADR-0060](adr/0060-fraicheur-en-temps-de-filtration.md) (fraîcheur en temps de filtration).

### Taille du bassin — depuis v2.32.0 (2026-08-31)

**Migration sans effet : rien ne change tant que vous ne touchez pas au nouveau réglage.** Le champ « Taille du bassin » (Paramètres → Régulation, et assistant de premier démarrage) présélectionne les réglages de régulation adaptés au volume. La clé NVS étant absente sur un appareil existant, la taille vaut **« 20 à 60 m³ »** (medium), dont les défauts sont **exactement** les valeurs v2.30.0 — égalité **verrouillée à la compilation** (`static_assert`, [ADR-0057](adr/0057-taille-bassin-preselection-jamais-entree-de-calcul.md)). Aucun réglage existant n'est modifié par la mise à jour seule ; la vitesse de correction (masquée de l'UI, dérivée de la taille) garde sa valeur NVS tant que la taille n'est pas changée.

- **Changer la taille** applique les 8 défauts de la nouvelle taille (récapitulatif avant/après affiché, annulation sans effet).
- **Bassin > 60 m³** : la colonne promet 15 min/h, mais le **quota journalier d'usine reste 300 mL/j** (protection, inchangé) — soit **≈ 11 min d'injection par jour à 28 mL/min**, atteint bien avant la limite horaire. Si le besoin réel le justifie, **relever « pH — volume max / jour »** (Paramètres → Régulation) ; l'alerte de sous-dimensionnement ([ADR-0056](adr/0056-sous-dimensionnement-actionneur-3-jours-80-pct.md)) guide.

### Nouvelles valeurs d'usine de régulation — depuis v2.30.0 (2026-08-31)

**Aucun changement pour les appareils existants : la configuration NVS prime toujours.** Un appareil déjà provisionné conserve ses réglages à l'identique après cette mise à jour. Les nouveaux défauts (limite pH **10 min/h**, consigne ORP **600 mV**, consigne pH **7,3**) ne s'appliquent qu'aux appareils **neufs** ou **réinitialisés** (`deploy.sh factory`).

⚠️ **Le visa `pool-chemistry` (GO, 31/08/2026, feature-087) est assorti de DEUX conditions** à vérifier lors d'une première mise en service ou d'une réinitialisation :

1. **Taille minimale de bassin ~20 m³** pour le défaut 10 min/h. Cette limite fixe le **volume de mélange aveugle** — le correcteur injecté pendant les 15 minutes où la mesure ne reflète pas encore les doses passées : ~280 mL/h, soit une excursion possible de **≈ 0,20 pH à 20 m³** mais **≈ 0,40 pH à 10 m³**, contre une marge de sécurité de **0,50**. Sur un bassin de moins de ~20 m³ : depuis la **v2.32.0**, **choisir « Moins de 20 m³ »** dans le sélecteur Taille du bassin (Paramètres → Régulation) avant la mise en route du dosage — la colonne applique 5 min/h et une correction douce (sur un firmware 2.30.x/2.31.x : redescendre la limite horaire d'injection pH à 5 min/h à la main).
2. **Bassin intérieur sans stabilisant** : le défaut 600 mV vise le cas commun du bassin extérieur stabilisé (CYA 30-50 mg/L, qui déprime l'ORP). Sans stabilisant, **relever la consigne ORP vers 650 mV** — la laisser à 600 sous-chlorerait l'eau.

Pourquoi ces valeurs changent : à 5 min/h le maximum délivrable (~911 mL/j) était **sous le besoin d'un bassin médian** (~1 800 mL/j) — pH bloqué au-dessus de la consigne sans explication ; depuis la v2.29.0, l'alerte de sous-dimensionnement guide un relèvement au-delà de 10 si nécessaire. Une consigne ORP de 650 quasi inatteignable sur bassin stabilisé provoquait un **dosage de chlore permanent silencieux** (mécanisme de l'épisode de 07/2026). Détail complet : [CHANGELOG.md](../CHANGELOG.md) et `specs/features/done/feature-087-valeurs-usine-regulation.md`.

### L'apprentissage de la régulation pH survit à un redémarrage — depuis v2.27.0 (2026-08-30)

**Aucune migration, aucune action requise.** Pas de changement de partition ni de format de configuration. Deux nouveaux enregistrements NVS apparaissent (instantané de l'intégrale pH, historique de charge sur 7 jours) ; leur **absence est le cas nominal** au premier démarrage et ne produit aucune erreur.

> ⚠️ **À lire d'abord : votre installation est limitée par la POMPE, pas par la régulation.**
>
> Avec la limite horaire d'injection réglée à **5 min/h** et une pompe qui débite ~26 mL/min, le maximum **physiquement délivrable** est de **~911 mL par jour** sur 7 h de filtration, pour un besoin d'environ **1 800 mL/jour**. **Aucune amélioration de la régulation ne peut combler cet écart.**
>
> **L'alerte « régulation saturée » est donc VRAIE et légitime** : elle signale que la **limite horaire d'injection** est atteinte, pas un bidon vide ni une sonde en panne. Ce n'est pas un faux positif.

- ⚠️ **Si vous relevez `phInjectionLimitMinutes`** (le seul levier réel), sachez qu'il augmente **proportionnellement** le volume injecté pendant les **15 minutes de retard de mélange**, période où la régulation dose sans voir encore l'effet des doses précédentes :

| Limite horaire | Volume injecté pendant les 15 min | Excursion pH |
|---|---:|---:|
| **5 min/h** (défaut) | 130 mL | 0,046 |
| 30 min/h | 780 mL | **0,28** |

  à comparer à la marge de sécurité de **0,50 pH**. **Augmenter par paliers et surveiller le pH.**

- **Ce que cette version change pour vous** : l'apprentissage de la régulation pH (la « réserve de correction » affichée en bas de la carte Régulation pH) **survit désormais à un redémarrage**. Un redémarrage coûtait **2,4 jours** de reconvergence ; il coûte maintenant **environ une heure**.
- **La valeur n'est JAMAIS appliquée au démarrage.** Elle est mise en attente et n'est restaurée que si le bassin confirme lui-même, pendant **5 minutes de régulation effective**, un écart au-dessus du seuil de démarrage. Un pH déjà à sa consigne au retour du courant ⇒ **rien n'est restauré**. Elle est de plus **plafonnée**, **périmée au bout de 6 h** *(amendé en v2.34.0 : 6 h de **filtration**, et non plus 6 h d'horloge — une nuit d'arrêt ne périme plus rien ; filet mural de 72 h)*, invalidée par tout changement de vitesse de correction, de produit ou de consigne, et **désarmée après 3 redémarrages rapprochés**.
- ⚠️ **Rien n'est restauré pendant les 3 premiers jours** suivant l'installation, ou après un effacement de la mémoire : le plafond de restauration se calcule sur la charge réellement observée du bassin, qui demande au moins 3 journées exploitables (≥ 2 h de filtration chacune). C'est volontaire.
- **L'apprentissage résiste mieux aux à-coups** : une pluie légèrement acide effaçait auparavant **deux jours et demi** d'apprentissage d'un coup. Il faut désormais que l'écart soit **soutenu 15 minutes** pour déclencher la remise à zéro — la protection n'est pas suspendue pendant l'attente, elle est **graduée**.
- **Les alertes de saturation arrivent au même moment qu'avant** en temps réel (~8 h, ~24 h, rappel ~48 h) : leurs seuils internes ont été recalibrés pour compenser le changement de cadence de calcul. **Rien à ajuster côté Home Assistant.**
- **Sécurité inchangée** : limites journalière et horaire, seuils de plancher pH, butées sur la mesure brute, inhibition du chlore sur pH haut, pause mélange, plafond de la réserve de correction (6 mL/min) — tout reste en place. **Régulation ORP (chlore) : strictement inchangée.**
- ⚠️ **Résultat pas encore vérifié sur le terrain.** Surveiller le pH et le compteur de dosage journalier pendant quelques jours.

Le message de journal de purge de l'apprentissage change de format (`… pH franchement au-delà de la consigne **depuis plus de 15 min** …`) : un filtre de journal sur l'ancienne phrase exacte ne matche plus. Voir [ADR-0053](adr/0053-persistance-conditionnelle-integrale-ph.md) et [ADR-0054](adr/0054-seuil-de-vraisemblance-fonction-de-la-configuration.md).

> Aucune mise à jour du filesystem n'est nécessaire pour cette version : l'interface est inchangée.

### La régulation pH vise enfin sa consigne — depuis v2.26.0 (2026-08-28)

**Aucune migration, aucune action requise.** Pas de changement de partition, de configuration NVS ni de format de données ; **aucun réglage utilisateur n'est modifié** (consigne, limites journalière et horaire, seuils de sécurité, modes : tout est inchangé).

Jusqu'ici, le pH se stabilisait systématiquement **au-dessus** de sa consigne — typiquement 7,50-7,66 pour une consigne à 7,30 — malgré près d'un litre d'acide injecté dans la journée. Ce n'était **ni une panne, ni un problème de dosage** : le correcteur utilisé était **proportionnel**, c'est-à-dire qu'il ne dose qu'en proportion de l'écart au but. Face à une dérive **permanente** (le CO₂ qui s'échappe de l'eau fait remonter le pH en continu), un tel correcteur s'arrête forcément **à un écart non nul** : celui où la dose produite compense exactement la dérive.

La correction ajoute un second terme, qui **accumule le retard** au lieu de ne regarder que l'écart instantané. Une fois la consigne atteinte, c'est lui qui fournit **toute** la dose d'entretien.

- **Ce que vous devriez voir** : le pH descend vers sa consigne au lieu de plafonner, et s'y maintient à **+0,01** près (la valeur exacte à laquelle la régulation coupe de toute façon). L'approche est **lente par conception** — comptez quelques heures à une journée, pas quelques minutes.
- ⚠️ **Résultat pas encore vérifié sur le terrain.** La correction est fondée sur la théorie du réglage et vérifiée en simulation, mais elle n'a pas encore tourné une saison sur un bassin réel. **Surveiller le pH et le compteur de dosage journalier pendant quelques jours.**
- **Nouvelle information dans l'interface** : en bas de la carte « Régulation pH », en mode Automatique, une ligne « **Rattrapage accumulé : N %** » indique le remplissage de cette réserve de correction. `0 %` = réserve vide (normal quand le pH est à sa consigne). Un encart d'avertissement apparaît au-delà de **95 %** : la régulation demande alors tout ce qu'elle peut sans que le pH bouge — vérifier le **niveau du bidon**, l'**amorçage de la pompe** et le **TAC** de l'eau (une alcalinité élevée bloque le pH).
- **Nouveauté Home Assistant** : trois entités `sensor` s'ajoutent automatiquement (« Piscine pH Rattrapage », « … Rattrapage Débit », « Piscine pH Intégrale »), plus une alerte `ph_integral_saturated` (avertissement après 2 h de saturation, critique après 6 h, rappelé toutes les 12 h — *seuils recalibrés en v2.27.0 à 2 h 20 / 7 h / 14 h, pour une fenêtre en temps réel identique*). **Aucune entité existante n'est renommée ni supprimée** : les automations en place continuent de fonctionner.
- **Sécurité inchangée** : limites journalière et horaire, seuils de plancher pH, butées sur la mesure brute, inhibition du chlore sur pH haut, pause mélange — tout reste en place. La réserve de correction est **plafonnée** (6 mL/min au maximum), **gelée** dès que le quota journalier dépasse 80 %, **purgée** après 24 h sans régulation, et **jamais conservée au redémarrage** *(ce dernier point est amendé en v2.27.0 : la réserve survit désormais à un redémarrage, sous conditions et avec confirmation par la mesure — voir la note ci-dessus)*.
- **Régulation ORP (chlore) : strictement inchangée.**

Le message de journal de configuration du correcteur change de format (`PID régulation: vitesse=… Ki_pH=… (Ti=…h, intMax=… pH·s, autorité=… mL/min) …`) : un filtre de journal sur « P temporisée » ou « Ki=0 » ne matche plus. Voir [ADR-0052](adr/0052-terme-integral-ph-dimensionne-par-autorite.md).

> Firmware **et** filesystem doivent être mis à jour **ensemble** pour voir la ligne « Rattrapage accumulé ». Un filesystem à jour sur un firmware antérieur reste fonctionnel : le bloc est simplement **masqué** (il ne montre jamais un « 0 % » qu'il ne connaît pas).

### Injections moins fréquentes mais deux fois plus longues — depuis v2.25.3 (2026-08-28)

**Aucune migration, aucune action requise.** Pas de changement de partition, de configuration NVS ni de format de données ; aucun réglage utilisateur n'est modifié.

La durée **minimale** d'une injection passe de **5 à 10 secondes**. **La quantité demandée par la régulation ne change pas** : ce sont les mêmes millilitres, servis en paquets deux fois plus gros et deux fois moins souvent.

- **Pourquoi** : sur une pompe péristaltique, chaque **démarrage** coûte du produit qui n'est pas compté (détente puis ré-occlusion du tube, ré-amorçage — 0,1 à 0,5 mL). Comme toutes les injections étaient à la durée minimale, cette perte portait sur **la totalité** du volume. Deux fois moins de démarrages pour le même volume, c'est cette perte amortie sur deux fois plus de produit utile.
- **Ce que vous verrez** : sur le tableau de bord, le compte à rebours « Prochaine impulsion » s'allonge (par exemple ~2,7 min au lieu de ~1,4 min sur une petite demande) et la pompe tourne 10 s d'affilée au lieu de 5.
- **Le pH devrait descendre plus efficacement** — c'est l'effet attendu, **pas encore vérifié sur la durée**. Suivre le pH sur quelques jours.
- ⚠️ **Surveiller le plafond journalier pendant 3 jours.** Le comptage du volume ignore toujours la perte au démarrage : à volume **compté** constant, on délivre désormais **réellement** un peu plus de produit (quelques dizaines de mL/jour). La limite journalière est donc légèrement **moins prudente** qu'avant. Aucun réglage n'est à modifier a priori.
- Aucune alerte MQTT n'est ajoutée, renommée ni supprimée ; le contrat `GET /data` et la trame WebSocket sont inchangés.

Le message de journal « Dosage par accumulation : impulsion minimale de 5 s » devient « … de 10 s » (à ajuster si vous filtrez le journal sur ce texte). Voir [ADR-0051](adr/0051-pouls-minimal-double-rendement-avant-comptage.md).

> Firmware seul : cette version ne modifie pas l'interface web. Mettre à jour le filesystem reste sans risque.

### Les compteurs de dosage comptent un peu plus vite — depuis v2.25.2 (2026-08-28)

**Aucune migration, aucune action requise.** Pas de changement de partition, de configuration NVS ni de format de données ; aucun réglage utilisateur n'est modifié.

Un point à connaître : le débit servant à créditer les brefs à-coups de démarrage des pompes (et les injections manuelles hors amplitude calibrée) était mesuré sur **un seul exemplaire de pompe**, puis figé sur un numéro de pompe. Il devient un **majorant unique de flotte** (74,0 mL/min). Conséquence : le volume **compté** augmente d'environ **+3,6 % côté pH** et **+1,9 % côté chlore** (~+47 mL et ~+25 mL par jour sur ~1 300 mL comptés).

- **Rien n'est injecté en plus** : c'est le comptage qui devient prudent. Les limites journalières et horaires mordent simplement un peu plus tôt.
- Si une limite journalière était déjà atteinte de justesse en fin de journée, elle peut l'être **quelques minutes plus tôt**. Relever la limite n'est pas nécessaire dans les réglages usuels ; le cas échéant, ajuster `max_ph_ml_per_day` / `max_orp_ml_per_day` avec la même prudence que d'habitude.
- La **durée annoncée** pour une injection manuelle volumée change également (chemin dégradé uniquement, amplitude ≠ duty calibré). Une pompe **mesurée** avec l'assistant de mesure du débit n'est pas concernée : elle utilise son débit propre.

Motif : sur un appareil dont la pompe pH aurait été de l'exemplaire le plus rapide, le firmware **sous-comptait de 11 %** — il injectait plus que ses propres limites ne le croyaient. Voir [ADR-0050](adr/0050-debit-pleine-puissance-constante-unique-de-flotte.md).

> Firmware et filesystem doivent être mis à jour **ensemble** : l'interface lit désormais ce débit depuis `GET /data` au lieu d'en garder une copie. Un filesystem à jour sur un firmware antérieur reste fonctionnel (repli sur les anciennes valeurs 61 / 67, qui sont exactement ce que dose ce firmware-là).

### Deux nouvelles alertes de panne de sonde — depuis v2.25.1 (2026-08-27)

**Aucune migration, aucune action requise.** Pas de changement de partition, de configuration NVS ni de format de données : la mise à jour est un simple flash firmware + filesystem.

Deux points à connaître :

1. **Deux types d'alerte MQTT sont ajoutés** : `ph_dose_sensor_fault` et `orp_dose_sensor_fault` (sévérité `warning`), émis **une fois par épisode** quand une sonde est **figée** ou **muette** depuis plus d'une minute et que la régulation correspondante est donc à l'arrêt. Aucun type existant n'est renommé ni supprimé. Une automation Home Assistant qui filtre les alertes par type peut les ajouter à sa liste — voir [docs/MQTT.md](MQTT.md#alertes).
2. **Ce que l'interface affiche change**, sans qu'aucun comportement de dosage ne change. Avant la v2.25.1, l'interface annonçait « EZO indisponible » pendant les ~6 premières minutes suivant un démarrage (sonde saine, filtre en cours de stabilisation) et, plus grave, affichait « Stabilisation du filtre » sur une **vraie panne de module EZO**. Ces deux affichages disparaissent : les pannes (« Sonde figée », « Sonde muette ») et les états d'attente (« Stabilisation… », « Mesures non représentatives ») sont désormais dans deux registres visuels distincts. Voir [ADR-0049](adr/0049-code-machine-de-cause-de-refus-et-etat-interne-du-filtre.md).

> Firmware et filesystem doivent être mis à jour **ensemble** : l'interface consomme deux nouveaux champs (`phDoseBlockedCode` / `orpDoseBlockedCode`). Un filesystem à jour sur un firmware antérieur reste fonctionnel (il retombe sur l'affichage du texte servi par le firmware), mais ne bénéficie pas du correctif.

### Consigne pH bornée à 7,20 – 7,50 et seuils de dosage refondus — depuis v2.25.0 (2026-08-27)

La consigne pH n'accepte plus que des valeurs comprises entre **7,20** et **7,50**, par pas de **0,10**, dans l'interface web comme depuis Home Assistant, et **pour les deux types de produit** (pH− comme pH+).

- **Borne basse 7,20** : sous 7,2 l'eau devient agressive pour le liner, les joints et les parties métalliques, elle est inconfortable et elle consomme le TAC.
- **Borne haute 7,50, et non 7,60** : au-dessus de 7,60 la fraction de chlore actif tombe sous 25 %. À **7,60 pile**, la consigne atteindrait le seuil d'inhibition du chlore : la désinfection serait suspendue **en permanence** dès que la régulation tient sa consigne — une panne sanitaire silencieuse (l'eau paraît bien régulée, le désinfectant ne monte jamais).

**Migration automatique, aucune action requise dans le cas nominal.** Au premier démarrage après mise à jour, une consigne enregistrée hors de cette plage est :

1. **ramenée** à la borne la plus proche (7,20 ou 7,50) ;
2. **journalisée** : `Consigne pH NVS hors plage (7.60) — clampée à 7.50 (plage autorisée 7.20 - 7.50).` ;
3. **réécrite en NVS** immédiatement, pour que la correction survive au redémarrage suivant.

> ⚠️ **Si votre bassin était régulé à 7,60**, la consigne effective **descend à 7,50** après mise à jour. C'est volontaire : à 7,60 le chlore aurait été inhibé en continu. Vérifier le réglage dans **pH → Régulation**.
>
> ⚠️ **Si votre bassin était régulé sous 7,2**, la consigne effective **remonte**. Si votre motivation était l'efficacité du chlore, agir plutôt sur le **TAC / stabilisant** — le gain d'acide hypochloreux entre 7,2 et 7,0 ne compense pas la corrosion permanente.

Une valeur hors plage envoyée après la mise à jour est **refusée** (HTTP `400` sur `/save-config`, refus + resynchronisation sur MQTT), jamais écrêtée en silence. Dans la plage, une valeur non alignée sur le pas de 0,10 est alignée au plus proche. Le `number` « Consigne pH » de Home Assistant est réaligné automatiquement sur les nouvelles bornes ; son `unique_id` est inchangé, **l'entité et son historique sont conservés**.

**L'offset pH passe à un pas de 0,10** (était 0,01) : une valeur enregistrée hors pas (0,47) est arrondie au plus proche (0,50) au premier démarrage. L'écart maximal introduit est de 0,05 pH, soit le bruit résiduel du filtre de mesure — aucun effet mesurable sur la régulation.

#### Seuils de sécurité du dosage pH : fixes, et sur le pH affiché

Les seuils qui refusent un dosage ne sont plus calculés en fonction de l'offset : ce sont désormais des **valeurs fixes**, portant sur le **pH affiché** (offset compris).

| Situation | Seuil |
|---|---:|
| Refus d'injecter de l'acide (pH−) | sous **6,70** |
| Refus d'injecter de la base (pH+) | au-dessus de **7,60** |
| Suspension du dosage de chlore (pH trop haut) | au-dessus de **7,60** |
| Avertissement d'eau acide, **sans blocage** | sous **7,00** |

Deux **butées de dernier recours** restent sur la mesure brute du capteur (sans offset), pour le cas où l'offset serait mal réglé : **6,20** et **8,10**.

Conséquences pour l'utilisateur :

- **Les installations à offset pH non nul qui subissaient des refus de dosage à répétition retrouvent une régulation normale** — aucune action requise. Avec un offset de +0,50 et une consigne de 7,20, la régulation dispose de 0,50 pH de marge avant tout refus.
- **Le dosage de pH+ est encadré pour la première fois** : si vous utilisez un produit basique, un refus peut désormais survenir au-dessus de 7,60 (auparavant aucun seuil ne s'appliquait).
- ⚠️ **Deux alertes MQTT sont renommées** : `ph_raw_floor` devient **`ph_guard`** et `orp_ph_raw_inhibit` devient **`orp_ph_inhibit`**. Deux alertes sont ajoutées (`ph_low_warning`, `ph_guard_scheduled`) et une est supprimée (`ph_target_unreachable`). **Si vous avez une automation Home Assistant filtrant sur les anciens noms, mettez-la à jour.**
- **Nouvel avertissement avant une injection manuelle qui contourne une sécurité** : quand une butée sur la mesure brute est active, un encart l'explique au-dessus du bouton et une confirmation est demandée. L'injection manuelle reste possible — c'est votre dernier recours — mais elle n'est plus silencieuse.

Décision, alternatives écartées et compromis assumés : [ADR-0048](adr/0048-seuils-ph-fixes-derives-de-la-plage-de-consigne.md).

### Changer de bootloader — flash USB obligatoire (core 3 par défaut, v2.20.4)

Depuis la **v2.20.4**, l'environnement de build par défaut est le **core Arduino 3.x / ESP-IDF 5** (`esp32dev_core3`), **bootloader ET application** — voir [ADR-0042](adr/0042-core3-environnement-par-defaut.md).

**Ce que l'OTA peut faire, et ce qu'il ne peut pas :**

| Élément | OTA (WiFi) | Flash USB |
|---|:---:|:---:|
| Application (firmware) | ✅ | ✅ |
| Filesystem (`littlefs.bin`) | ✅ | ✅ |
| **Bootloader** | ❌ | ✅ |
| Table de partitions | ❌ | ✅ |

L'OTA n'écrit **que** l'application, à l'intérieur des partitions existantes. Une application core 3 démarre parfaitement sur un bootloader core 2 — ce montage a tourné **plusieurs semaines en production**. Mais passer le **bootloader** en core 3 exige un **câble série** :

```bash
./deploy.sh all      # réécrit bootloader + table de partitions + firmware, puis le FS
```

> ⚠️ **Ne pas utiliser `./deploy.sh factory`** : il efface toute la flash, NVS comprise (config, calibrations, WiFi, mot de passe AP régénéré). `./deploy.sh all` suffit et préserve tout.

⚠️ **Le filet de repli a disparu.** Tant que le bootloader restait en IDF 4.4, une application core 3 défaillante se rattrapait **sans câble** en repointant `otadata` (`erase_region 0xE000 0x2000`), ce qui faisait redémarrer l'appareil sur le slot précédent. Avec un bootloader core 3, ce recours n'existe plus : **tout échec impose un flash USB**, donc un **accès physique** au boîtier. À prendre en compte avant d'intervenir sur un appareil difficile d'accès ou hors saison.

> ⚠️ **Il n'existe plus de retour arrière vers le core 2 — depuis la v2.42.1, l'environnement de repli est retiré** ([ADR-0069](adr/0069-retrait-repli-core-2.md)). Les versions antérieures de ce guide publiaient une procédure `PIO_ENV=esp32dev ./deploy.sh all` : elle était **fausse et dangereuse**. L'environnement core 2 n'était plus compilable depuis la **v2.40.0** (un appel `esp_cpu_get_cycle_count()` laissé hors garde dans `oom_guard.cpp`, resté invisible deux versions parce que la CI ne construisait alors **aucun env matériel** — trou comblé depuis par [`build-firmware.yml`](BUILD.md#intégration-continue-github-actions)), et **aucun binaire core 2 n'avait jamais été archivé**. La commande n'aurait donc produit aucune image : ni compilable, ni flashable, sans image de secours.
>
> **Ce qui remplace ce recours** : le retour arrière se fait sur une **version antérieure du firmware core 3** (image archivée dans `builds/<VERSION>/esp32dev_core3/`, ou asset d'une release GitHub), poussée en OTA si l'appareil répond, par flash USB sinon. Les deux conditions ci-dessus restent entières : **changer de bootloader ou de table de partitions exige toujours un câble série**, et **le filet `otadata` n'existe plus**.

> ℹ️ Config NVS, historique et coredump sont **préservés** par un simple changement d'application, dans les deux sens : la table de partitions ne change pas et le format LittleFS reste compatible. Un **changement de layout**, lui, déplace les partitions et efface leur contenu — voir la section « Migration layout v4 → v5 » ci-dessous. Détails outillage : [BUILD.md](BUILD.md#lenvironnement-firmware-unique).

### Mode d'installation (`install_mode`) — depuis v2.19.0 (2026-07-08)

La v2.19.0 fusionne les anciens réglages `regulation_mode` (Continu / Piloté) **et** « Gérer la filtration » (`filtration_enabled`) en un **mode d'installation** unique à 3 valeurs (`managed` / `powered` / `external`), exposé dans le nouvel onglet **Paramètres → Installation** et dans l'assistant de premier lancement. Décision structurante : [ADR-0026](adr/0026-mode-installation.md).

**Migration NVS automatique (one-shot, aucune action requise dans le cas nominal)** — à la première lecture de la config après mise à jour, si la clé `install_mode` est absente :

| Ancien `reg_mode` | Nouveau `install_mode` |
|-------------------|------------------------|
| `pilote` (ou toute valeur ≠ `continu`) | `managed` |
| `continu` | `powered` |

`external` n'est **jamais** produit par migration : c'est un mode nouveau, à choisir explicitement.

> ⚠️ **Cas de bord à vérifier — le relais de filtration peut changer de comportement.** La migration se base **uniquement** sur `reg_mode` et **ignore** l'ancien `filt_enabled`. Deux combinaisons héritées voient donc leur pilotage relais changer :
>
> | Ancienne config | Migre vers | Relais avant | Relais après |
> |-----------------|-----------|--------------|--------------|
> | `pilote` + « Gérer la filtration » **désactivé** | `managed` | inerte | **piloté** (GPIO 26) |
> | `continu` + « Gérer la filtration » **activé** | `powered` | piloté | **inerte** |
>
> C'est **conforme à l'intention** (la combinaison « alimenté par la filtration » + « PoolController pilote la filtration » est un non-sens matériel — un contrôleur alimenté par la pompe ne peut pas la couper sans se couper lui-même). **Après mise à jour, vérifier dans Paramètres → Installation que le mode choisi correspond bien à votre câblage réel**, et le corriger si besoin (notamment si vous utilisez une filtration tierce → choisir `external`).

**Mode `external` (filtration tierce)** : le contrôleur ne dose que si l'état réel de la filtration lui est signalé, via `POST /filtration/external-state?running=true|false` ou MQTT `{base}/filtration_external_state/set` (`ON`/`OFF`). **Sans signal depuis plus de 3 min** (ou au boot, aucun signal), l'eau est considérée absente et le dosage est **suspendu** (fail-safe). Prévoir une automatisation (Home Assistant, domotique) qui publie régulièrement cet état.

`regulation_mode` et `filtration_enabled` disparaissent de `/get-config`, `/save-config`, du WebSocket et de l'UART. Les automatisations qui les lisaient doivent basculer sur `install_mode`.

### Migration layout v3 → v4 (v2.13.0) — depuis 2026-07-06

⚠️ **Mise à jour par câble USB obligatoire, une seule fois.**

La version 2.13.0 change la **table de partitions** (layout v4, [ADR-0024](adr/0024-partitions-layout-v4.md)) : les slots firmware `app0`/`app1` passent de 1664 à **1792 KB** (+128 KB chacun), pris sur la partition `spiffs` qui passe de 576 à **320 KB** et se déplace à l'offset `0x390000` — rendu possible par la pré-compression gzip des assets (feature-048, payload FS 155 KB).

**Pourquoi l'USB est obligatoire :** l'OTA écrit uniquement à l'intérieur des partitions existantes — il ne peut **pas** modifier la table de partitions ni le bootloader. La migration se fait par câble série :

```bash
./deploy.sh all
```

Cette commande recompile firmware + filesystem, réécrit bootloader + table de partitions + firmware, puis flashe le FS au nouvel offset.

> ⛔ **Interdiction : ne JAMAIS pousser le firmware ou le filesystem v4 par OTA sur un appareil encore en table v3.** L'OTA ne change pas la table : elle resterait v3 même après « réussite » apparente. En particulier, une image FS construite pour 320 KB montée sur la partition `spiffs` de 576 KB présente des **métadonnées LittleFS discordantes** (taille d'image ≠ taille de partition) — interface web indisponible ou corrompue. La release 2.13.0 s'installe exclusivement par USB (`./deploy.sh all`).

> ⚠️ **NE PAS utiliser `./deploy.sh factory`** pour cette migration : il efface toute la flash, **y compris la NVS** (perte de la config, des calibrations, du WiFi, et régénération du mot de passe AP). `./deploy.sh all` suffit et préserve tout.

**Ce qui est préservé** (partitions inchangées en offset et en taille) :

- **Config NVS** : calibrations, identification des sondes, paramètres de régulation, WiFi, compteurs journaliers ;
- **Historique des mesures** (partition `history` à `0x3E0000`) ;
- **Coredump** (partition `coredump`).

**Vérifications post-flash** (Paramètres → Système → Infos système) :

- ligne **FS** : taille de partition **~320 KB** (occupation ≈48 %) ;
- **historique présent** : les graphiques du dashboard affichent les mesures antérieures à la migration ;
- les mises à jour OTA (firmware et filesystem) refonctionnent normalement avec les nouvelles bornes.

---

### Migration layout v2 → v3 (v2.4.0) — depuis 2026-07-04

⚠️ **Mise à jour par câble USB obligatoire, une seule fois.**

La version 2.4.0 change la **table de partitions** (layout v3, [ADR-0019](adr/0019-partition-app-1664k.md)) : les slots firmware `app0`/`app1` passent de 1536 à **1664 KB** (+128 KB chacun), pris sur la partition `spiffs` qui passe de 832 à **576 KB** et se déplace à l'offset `0x350000`.

**Pourquoi l'USB est obligatoire :** l'OTA écrit uniquement à l'intérieur des partitions existantes — il ne peut **pas** modifier la table de partitions ni le bootloader. Tenter un OTA firmware compilé pour le layout v3 sur un appareil encore en layout v2 est voué à l'échec. La migration se fait par câble série :

```bash
./deploy.sh all
```

Cette commande recompile firmware + filesystem, réécrit bootloader + table de partitions + firmware, puis flashe le FS au nouvel offset.

> ⚠️ **NE PAS utiliser `./deploy.sh factory`** pour cette migration : il efface toute la flash, **y compris la NVS** (perte de la config, des calibrations, du WiFi, et régénération du mot de passe AP). `./deploy.sh all` suffit et préserve tout.

**Ce qui est préservé** (partitions inchangées en offset et en taille) :

- **Config NVS** : calibrations, identification des sondes, paramètres de régulation, WiFi, compteurs journaliers ;
- **Historique des mesures** (partition `history` à `0x3E0000`) ;
- **Coredump** (partition `coredump`).

**Ce qui est réécrit :** le contenu de la partition `spiffs` (interface web) est re-flashé au nouvel offset — sans impact, il ne contient que l'UI.

**Après la migration :** les mises à jour OTA (firmware et filesystem) refonctionnent normalement avec les nouvelles bornes. Aucune autre action requise.

---

### Logs DEBUG désactivés par défaut — depuis 2026-04-30

**Aucune action utilisateur requise.**

Depuis cette version, les logs de niveau `DEBUG` sont **désactivés par défaut** côté firmware. L'objectif est d'alléger le buffer NVS (200 entrées max, partagé avec INFO/WARN/ERROR/CRITICAL) pendant le fonctionnement nominal et de faciliter la lecture de la page Logs.

**Comportement après mise à jour :**

- Default `false` après flash neuf ou OTA. Aucun nouveau log `DEBUG` n'apparaît dans la page Logs ni dans le buffer persistant.
- Les niveaux `INFO`, `WARN`, `ERROR`, `CRITICAL` ne sont **pas** affectés et continuent de s'afficher comme avant.

**Pour activer les logs DEBUG (par ex. en cas de diagnostic) :**

1. Aller dans **Paramètres → Avancé → card Logs**
2. Cocher le switch **« Logs DEBUG activés »** (placé immédiatement sous « Log des sondes »)
3. Cliquer **Enregistrer**

L'effet est immédiat (pas de redémarrage requis). La valeur est persistée en NVS sous la clé `debug_logs` et survit aux reboots. Pour désactiver à nouveau, décocher et enregistrer.

> Le filtre `DEBUG` de la barre de filtres de la page Logs reste indépendant : il pilote l'affichage navigateur uniquement. Quand le switch firmware est désactivé, ce filtre n'a plus rien à afficher (les entrées ne sont plus produites).

---

### Stabilité réseau MQTT — tâche dédiée — depuis 2026-04-27

**Aucune action utilisateur requise. Aucun changement de configuration.**

Avant cette version, une publication MQTT (par exemple un simple `OFF` de 33 octets sur un capteur de stock faible) pouvait bloquer la régulation pH/ORP plusieurs dizaines de secondes lorsque le réseau entre l'ESP32 et le routeur était lossy (typiquement sur un lien CPL/Powerline bruyant). Trois crashes `PANIC` watchdog ont été observés en production avec exactement cette signature.

Depuis cette version, **toute la communication MQTT s'exécute dans une tâche FreeRTOS dédiée** (`mqttTask`), totalement isolée de la régulation pH/ORP, de la filtration et du watchdog principal. Conséquences observables :

- **Aucun gel de la régulation** lors d'un broker injoignable ou d'une microcoupure réseau de plusieurs dizaines de secondes. Les compteurs `dailyPhInjectedMl`/`dailyOrpInjectedMl` continuent de s'incrémenter, les checks horaires/journaliers restent évalués, le PID continue de tourner.
- **Comportement utilisateur strictement identique** : mêmes topics MQTT, mêmes payloads, mêmes intervalles de publication, même auto-discovery Home Assistant. Aucune automation HA à reconfigurer.
- **`status=offline` publié immédiatement** lors d'un OTA / factory reset / redémarrage manuel, au lieu d'attendre les 90 s de timeout broker.
- **Coût RAM marginal** : ~16 KB de heap supplémentaire (8 KB stack `mqttTask` + 7 KB queues). RAM statique reste à 16.4 % (vs 16.4 % avant), bien sous le budget.

Détails techniques : voir [ADR-0011](adr/0011-mqtt-task-dediee.md) et [`docs/subsystems/mqtt-manager.md`](subsystems/mqtt-manager.md).

---

### Persistance des compteurs journaliers en NVS (`pool-daily`) — depuis 2026-04-24

**Contexte :** avant cette version, les compteurs `dailyPhInjectedMl` et `dailyOrpInjectedMl` étaient stockés uniquement en RAM. Tout reboot (watchdog, brownout, coupure secteur, OTA) les remettait silencieusement à zéro, contournant potentiellement la limite journalière de sécurité.

**Comportement après mise à jour :**

- Les compteurs sont persistés en NVS (namespace `pool-daily`, clés `ph_daily_ml`, `orp_daily_ml`, `daily_date`).
- Au boot, si la date locale (RTC/NTP) est la même que celle enregistrée en NVS, les compteurs sont restaurés.
- Si NTP/RTC n'est pas encore synchronisé au boot, les valeurs NVS sont restaurées de façon conservatrice (hypothèse même jour).

**Aucune migration nécessaire :** au premier boot après mise à jour, le namespace `pool-daily` n'existe pas encore — les compteurs démarrent à 0, ce qui est le comportement attendu.

**Reset journalier :** le reset est désormais aligné sur **minuit local** (heure RTC/NTP) et non sur une fenêtre glissante de 24 h depuis le dernier boot.

> ⚠️ **Recommandation (obsolète depuis la v2.34.0, conservée pour l'historique) :** il fallait configurer `stabilizationDelayMin ≥ 5 min` (Paramètres → Régulation) pour bénéficier d'une protection anti-double-quota au passage de minuit ; à `0`, le dosage pouvait reprendre immédiatement après le reset de minuit alors que la journée précédente venait d'atteindre son quota.
>
> **Depuis la v2.34.0, il n'y a plus rien à configurer** : le réglage a disparu ([feature-091](../specs/features/done/feature-091-purge-adaptative-stabilisation.md)) et le passage de minuit arme désormais une suspension d'injection de durée **gravée** — 5 min pour le pH, 3 min pour l'ORP (`kStabilizationDurationPhMs` / `kStabilizationDurationOrpMs`, les mêmes que le sas post-calibration). La protection est donc active en permanence, sans dépendre d'un réglage que l'utilisateur pouvait mettre à zéro.

---

### Mode de régulation ORP (`orp_regulation_mode`) — depuis 2026-04-24

**Contexte :** avant cette version, la régulation ORP était pilotée par un booléen `orp_enabled`. Ce champ est remplacé par un enum `orp_regulation_mode` à trois valeurs (`automatic`, `scheduled`, `manual`).

**Migration automatique au premier démarrage :**

| Ancienne valeur | Nouvelle valeur |
|----------------|----------------|
| `orp_enabled = true` | `orp_regulation_mode = "automatic"` |
| `orp_enabled = false` | `orp_regulation_mode = "manual"` |

Aucune action requise. Le firmware effectue la migration à la première lecture de la NVS si la clé `orp_reg_mode` est absente.

**Compatibilité MQTT / Home Assistant :** le champ `orp_enabled` est conservé comme miroir dérivé (`true` si mode ≠ `manual`). Les automations Home Assistant utilisant ce champ continuent de fonctionner sans modification.

**Nouveau mode Programmée :** en mode `scheduled`, le firmware injecte un volume fixe de chlore par jour (`orp_daily_target_ml` mL), indépendamment de la valeur mesurée par le capteur ORP. Ce mode est adapté aux situations où le capteur ORP est en cours de remplacement ou de calibration. La limite journalière (`max_chlorine_ml_per_day`) reste appliquée.

**Nouveau champ `orp_daily_target_ml` :** initialisé à `0` (aucune injection programmée). À configurer dans la page ORP, onglet Programmée, si vous souhaitez utiliser ce mode.

---

### Mode de régulation pH (`ph_regulation_mode`) — depuis 2026-04-23

**Contexte :** avant cette version, la régulation pH était pilotée par un booléen `ph_enabled`. Ce champ est remplacé par un enum `ph_regulation_mode` à trois valeurs (`automatic`, `scheduled`, `manual`).

**Migration automatique au premier démarrage :**

| Ancienne valeur | Nouvelle valeur |
|----------------|----------------|
| `ph_enabled = true` | `ph_regulation_mode = "automatic"` |
| `ph_enabled = false` | `ph_regulation_mode = "manual"` |

Aucune action requise. Le firmware effectue la migration à la première lecture de la NVS si la clé `ph_reg_mode` est absente.

**Compatibilité MQTT / Home Assistant :** le champ `ph_enabled` est conservé comme miroir dérivé (`true` si mode ≠ `manual`). Les automations Home Assistant utilisant le switch `ph_enabled` continuent de fonctionner sans modification.

**Nouveau champ `ph_daily_target_ml` :** initialisé à `0` (aucune injection programmée). À configurer dans la page pH, onglet Programmée, si vous souhaitez utiliser ce mode.

---

## Dépannage

| Problème | Solution |
|----------|----------|
| Update Failed | Vérifier la connexion WiFi et réessayer |
| « Échec de la vérification d'intégrité » | Image tronquée/corrompue refusée par sécurité — relancer l'installation (si c'était le filesystem, le réinstaller) |
| Bouton Installer grisé « pas d'empreinte d'intégrité » | Release GitHub sans champ `digest` — installer via upload manuel ou `deploy.sh` |
| ESP32 ne redémarre pas | Utiliser le bouton RESET ou débrancher |
| Interface inaccessible | `./deploy.sh fs` (USB) |
| Authentification refusée | Vérifier le mot de passe ou utiliser `POOL_PASSWORD=...` |
| `poolcontroller.local` inaccessible | mDNS non supporté par le réseau — utiliser l'IP directe : `./ota_update.sh both 192.168.1.x motdepasse` |
| « Aucun port série USB détecté » | Module non branché ou nom de port inattendu — le script liste les `/dev/cu.*` trouvés, relancer avec `PORT=/dev/cu.usbserial-XX ./deploy.sh all` |
| Upload USB échoue alors que le module est branché | Un autre module occupe le port historique — forcer le bon : `PORT=... ./deploy.sh firmware` |
| Mot de passe AP WiFi perdu | `curl http://poolcontroller.local/auth/ap-password`, ou `./deploy.sh factory` si l'appareil est injoignable (voir [ci-dessus](#récupérer-le-mot-de-passe-wifi-ap)) |

## Migration v2.0.0 — PCB v2

⚠️ La version 2.0.0 cible exclusivement le **PCB v2** et n'est plus compatible avec le PCB v1 (mapping GPIO entièrement réassigné, voir [ADR-0012](adr/0012-mapping-gpio-pcb-v2.md) ; chaîne pH/ORP refondue sur Atlas EZO, voir [ADR-0014](adr/0014-migration-atlas-ezo.md)). **Ne pas flasher la 2.0.0 sur un PCB v1** — les sondes analogiques pH/ORP du v1 ne sont plus supportées par le firmware.

### Étape post-update 1 : identification des 2 sondes DS18B20 (feature-020)

Le PCB v2 ajoute une 2ᵉ sonde DS18B20 (eau piscine + circuit électronique). Après la première mise à jour vers 2.0.0, l'utilisateur doit identifier ces 2 sondes via :

**Paramètres → Avancé → card « Identification des sondes de température »**

Une **chip de notification ambré** apparaît sur le Dashboard tant que l'identification n'est pas faite. Workflow :

1. Tenir l'une des 2 sondes dans la main pendant ~30 secondes
2. Observer dans la card laquelle voit sa T° monter en temps réel
3. Cliquer le bouton « C'est l'eau de la piscine » ou « C'est le circuit interne » sur la sonde correspondante
4. La 2ᵉ sonde est automatiquement identifiée comme l'autre rôle (auto-permutation)
5. La card affiche « 2/2 sondes identifiées ✓ »

Tant que l'identification n'est pas faite, **`getTemperature()` continue de fonctionner** via fallback gracieux sur la 1ʳᵉ sonde détectée — pas d'interruption de service côté MQTT/HA. La compensation de température du pH (feature-021 Atlas EZO) sera plus précise une fois l'identification effectuée.

Si une sonde est remplacée physiquement (adresse ROM différente), un warning est loggé et l'utilisateur doit refaire l'identification pour la sonde manquante.

### Étape post-update 2 : recalibration EZO pH + ORP — OBLIGATOIRE (feature-021)

⚠️ La version 2.0.0 remplace la chaîne pH/ORP analogique (ADS1115 + DFRobot_PH) par les modules **Atlas Scientific EZO Embedded I²C** (pH 0x63, ORP 0x62). Voir [ADR-0014](adr/0014-migration-atlas-ezo.md). La calibration est désormais **mémorisée dans le module EZO**, pas en NVS ESP32 — aucune migration de données n'est possible depuis v1.x.

**Tant que la calibration n'est pas faite, la régulation pH/ORP automatique est inhibée** (10 garde-fous fail-closed dans `canDose()`, voir [docs/subsystems/pump-controller.md](subsystems/pump-controller.md)). En parallèle :

- Une alerte MQTT retain est publiée sur `pool/alerts/calibration_required`
- Une chip ambrée apparaît sur les cartes Régulation des pages pH et ORP
- Logger `critical` au boot et à chaque transition

#### Préparation

| Solution tampon requise | Quantité |
|-------------------------|----------|
| pH 7.00 (point milieu) | ~50 mL pour rincer + plonger |
| pH 4.00 (point bas) | ~50 mL |
| Solution standard ORP (225 mV ou 470 mV — kit Atlas/Hanna) | ~50 mL |
| Eau distillée pour rinçage entre tampons | au besoin |

#### Workflow pH (2 points)

Pages **pH → carte Calibration**. Les 2 sous-blocs (point milieu + point bas) sont **parallèles** : ordre libre.

1. Rincer la sonde à l'eau distillée, sécher à la lingette douce.
2. Plonger dans le tampon **pH 7.00**, attendre 1 min.
3. Cliquer **« Calibrer le point 7.0 »** → toast info « Calibration en cours ».
4. Attendre que `phCalPoints` passe à `1` (toast succès, ~5 s).
5. Rincer, plonger dans le tampon **pH 4.00**, attendre 1 min.
6. Cliquer **« Calibrer le point 4.0 »** → `phCalPoints` passe à `2` → callout vert « Calibré 2 points ✓ ».

**Attendre 5 minutes** avant qu'un dosage automatique ne reprenne (stabilisation post-calibration `kStabilizationDurationPhMs`).

#### Workflow ORP (1 point)

Page **ORP → carte Calibration**.

1. Rincer la sonde, sécher.
2. Saisir la valeur de référence du tampon dans le champ « Référence (mV) » (par défaut 470).
3. Plonger dans la solution standard, attendre 1 min.
4. Cliquer **« Calibrer »** → `orpCalPoints` passe à `1` → callout vert.

**Attendre 3 minutes** avant qu'un dosage ORP automatique ne reprenne (stabilisation `kStabilizationDurationOrpMs`).

#### Vérifications

- L'alerte MQTT `pool/alerts/calibration_required` est clearée automatiquement (payload vide retain) une fois `phCalPoints >= 2` ET `orpCalPoints >= 1`.
- L'auto-discovery HA expose 2 nouveaux sensors (`Piscine pH Points Calibrés`, `Piscine ORP Points Calibrés`) — les ajouter en card pour visualiser l'état au quotidien.
- Plage de référence ORP acceptée : `0..1000` mV. Hors plage → HTTP 400.

> Si vous voulez **repartir d'une calibration vierge** (changement de sonde, recalibration complète), utiliser `POST /calibrate_clear {sensor:"ph"}` ou `{sensor:"orp"}` (exposé via UI sur les cartes Calibration). Le compteur `phCalPoints` / `orpCalPoints` repasse à `0`.
