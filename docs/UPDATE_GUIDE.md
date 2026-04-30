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

## Notes de migration

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
| ESP32 ne redémarre pas | Utiliser le bouton RESET ou débrancher |
| Interface inaccessible | `./deploy.sh fs` (USB) |
| Authentification refusée | Vérifier le mot de passe ou utiliser `POOL_PASSWORD=...` |
| `poolcontroller.local` inaccessible | mDNS non supporté par le réseau — utiliser l'IP directe : `./ota_update.sh both 192.168.1.x motdepasse` |
