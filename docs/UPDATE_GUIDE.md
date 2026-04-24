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
