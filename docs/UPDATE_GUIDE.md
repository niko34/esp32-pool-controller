# Guide de Mise à Jour

> ⚠️ **Première installation obligatoire via USB** : le bootloader et la table de partitions personnalisée (`partitions.csv`) ne peuvent être flashés que via USB. Une fois cette installation initiale effectuée, toutes les mises à jour suivantes peuvent se faire en OTA (WiFi).

---

## Méthodes de mise à jour

| Méthode | Compile | USB requis | WiFi requis |
|---------|:-------:|:----------:|:-----------:|
| `deploy.sh [firmware\|fs\|all]` | ✅ | ✅ | ❌ |
| `deploy.sh [ota-firmware\|ota-fs\|ota-all]` | ✅ | ❌ | ✅ |
| Interface web (OTA) | ❌ | ❌ | ✅ |
| Mise à jour automatique GitHub | ❌ | ❌ | ✅ |

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

Le port série est configuré dans `platformio.ini` (`upload_port`). Adapter selon le système :
- macOS : `/dev/cu.usbserial-*`
- Linux : `/dev/ttyUSB0` ou `/dev/ttyACM0`
- Windows : `COM3`, `COM4`, etc.

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
3. Aller dans **Paramètres > Système**
4. Sélectionner le fichier (`firmware.bin` ou `littlefs.bin`)
5. Cliquer sur **Mettre à jour**
6. Attendre le redémarrage automatique

> Si vous modifiez le code ET les fichiers web, faire 2 mises à jour séparées.

---

## Mise à Jour Automatique depuis GitHub — WiFi uniquement

L'interface web peut vérifier et télécharger automatiquement la dernière version depuis GitHub :

1. Accéder à **Paramètres > Système**
2. Cliquer sur **Vérifier les mises à jour** — consulte la dernière release GitHub
3. Si une nouvelle version est disponible, cliquer sur **Mettre à jour** — télécharge et installe automatiquement

> Le contrôleur se connecte directement à GitHub en HTTPS. Aucun USB, aucune manipulation de fichiers.

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

> ⚠️ **Recommandation :** configurer `stabilizationDelayMin ≥ 5 min` (Paramètres → Régulation) pour bénéficier d'une protection anti-double-quota au passage de minuit. Si ce paramètre est à `0`, le dosage peut reprendre immédiatement après le reset de minuit alors que la journée précédente venait d'atteindre son quota.

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
