# Quick Start Guide - ESP32 Pool Controller

## 🚀 Mise en Route Rapide (30 minutes)

### Prérequis

✅ **Matériel:**
- ESP32 DevKit
- Câble USB (data, pas charge seule)
- Ordinateur (Windows/Mac/Linux)

✅ **Logiciels:**
- VS Code installé
- Extension PlatformIO installée
- Python 3 (pour minification)

### Étape 1: Installation (5 min)

```bash
# 1. Télécharger le projet
git clone https://github.com/niko34/esp32-pool-controller.git
cd esp32-pool-controller

# 2. Ouvrir avec VS Code
code .

# 3. PlatformIO détectera automatiquement le projet
# Attendre que les dépendances se téléchargent
```

### Étape 2: Configuration Port Série

**Éditer `platformio.ini` lignes 9-10:**

```ini
upload_port = /dev/cu.usbserial-210  # À adapter selon votre système
monitor_port = /dev/cu.usbserial-210
```

**Identifier votre port:**
```bash
pio device list
```

**Ports selon OS:**
- macOS: `/dev/cu.usbserial-*` ou `/dev/cu.SLAB_USBtoUART`
- Linux: `/dev/ttyUSB0` ou `/dev/ttyACM0`
- Windows: `COM3`, `COM4`, etc.

### Étape 3: Compilation et Upload (5 min)

**Option A - Automatique (recommandé):**
```bash
# Compile et upload firmware + filesystem en une commande
./deploy.sh all
```

**Option B - Étape par étape:**
```bash
# 1. Compiler le firmware
pio run

# 2. Construire le filesystem (avec minification auto)
./build_fs.sh

# 3. Upload firmware
pio run -t upload

# 4. Upload filesystem
python3 ~/.platformio/packages/tool-esptoolpy/esptool.py \
  --chip esp32 --port /dev/cu.usbserial-210 --baud 115200 \
  write_flash 0x2B0000 .pio/build/esp32dev/littlefs.bin

# 5. Moniteur série
pio device monitor -b 115200
```

**⚠️ IMPORTANT:**
- Ne PAS utiliser `pio run -t buildfs` ou `pio run -t uploadfs`
- Ces commandes utilisent une mauvaise taille (128KB au lieu de 1344KB)
- Toujours utiliser `./build_fs.sh` pour construire le filesystem

**Logs attendus:**
```
[INFO] === Démarrage ESP32 Pool Controller v2.9.4 ===
[INFO] Watchdog activé (30s)
[INFO] LittleFS monté avec succès
[INFO] Configuration chargée avec succès
[INFO] WiFi connecté: PoolControllerAP
[INFO] IP: 192.168.4.1
[INFO] Initialisation terminée
```

### Étape 4: Configuration WiFi (5 min)

**Première connexion:**

1. **Point d'accès automatique**
   - L'ESP32 crée un réseau: `PoolControllerAP`
   - Mot de passe: `12345678`

2. **Connexion au réseau**
   - Smartphone/PC → WiFi → PoolControllerAP
   - Page config s'ouvre automatiquement
   - Sinon: `http://192.168.4.1`

3. **Configurer votre WiFi**
   - Sélectionner votre réseau
   - Entrer mot de passe
   - Sauvegarder

4. **Redémarrage**
   - ESP32 redémarre et se connecte à votre réseau
   - Noter l'IP affichée dans les logs série
   - Ou utiliser: `http://poolcontroller.local`

### Étape 5: Interface Web (5 min)

**Accès interface:**
```
http://poolcontroller.local
```

**Onglets disponibles:**
- **Tableau de bord**: Graphiques temps réel pH/ORP/Température
  - Échelle dynamique (s'adapte si valeurs hors plage)
  - Zones rouges pour valeurs hors consignes
- **Configuration**: Réglages MQTT, consignes, limites
- **Historique**: Suivi des événements et injections
- **Logs**: Journal système avec filtrage par niveau
- **Système**: Test pompes, OTA, informations

### Étape 6: Assistant de Configuration Initiale (3 étapes)

Au **premier démarrage**, l'application affiche automatiquement un assistant de configuration en 3 étapes :

#### Étape 1: Mot de passe administrateur
- Définir un mot de passe sécurisé (minimum 8 caractères)
- Confirmer le mot de passe
- ⚠️ **Important** : Ce mot de passe protège l'accès à votre installation

#### Étape 2: Configuration Wi-Fi (optionnelle)
**Avantages avec Wi-Fi :**
- ✅ Mises à jour automatiques du firmware
- ✅ Synchronisation automatique de l'heure via NTP
- ✅ Accès à distance à l'interface web
- ✅ Publication des données via MQTT (optionnel)

**Sans Wi-Fi :**
- ⚠️ Connexion uniquement via le réseau AP du PoolController
- ⚠️ Réglage manuel de l'heure
- ⚠️ Mises à jour manuelles par upload

**Configuration :**
1. Cliquer sur "Scanner les réseaux"
2. Sélectionner votre réseau WiFi
3. Saisir le mot de passe WiFi
4. Ou cocher "Ignorer la configuration Wi-Fi" pour configurer plus tard

#### Étape 3: Configuration de l'heure
L'heure est **essentielle** pour :
- ⏰ Programmation de la filtration (horaires auto)
- ⏰ Programmation de l'éclairage (allumage/extinction)
- 📊 Historique des mesures (horodatage)
- 📝 Logs système (traçabilité)

**Deux modes disponibles :**

**Mode Automatique (NTP)** - Si WiFi configuré :
- Synchronisation automatique via serveur NTP
- Fuseau horaire : Europe/Paris (CET/CEST)
- Pas de maintenance nécessaire

**Mode Manuel** - Sans WiFi ou par choix :
- Réglage manuel de la date et l'heure
- À mettre à jour manuellement en cas de changement d'heure

**Validation :**
- Cliquer sur "Terminer la configuration"
- Redirection automatique vers le tableau de bord
- L'assistant ne s'affichera plus au prochain démarrage

### Étape 7: Configuration Avancée (5 min)

**Paramètres essentiels (onglet Configuration):**

```
Consignes:
- pH cible: 7.2 (recommandé: 7.0 - 7.4)
- ORP cible: 650 mV (recommandé: 650 - 750 mV)

Limites de Sécurité:
- pH- max/jour: 500 ml (ajuster selon volume piscine)
- Chlore max/jour: 300 ml (ajuster selon volume piscine)
- Temps injection max/heure: 60 secondes
```

**Calcul limites journalières:**
```
Volume piscine = 40 m³
pH- pour baisser de 0.1 pH ≈ 0.3L pour 10m³
→ Max raisonnable = 500 ml/jour pour 40m³

Chlore pour remonter ORP de 100mV ≈ 0.2L pour 10m³
→ Max raisonnable = 300 ml/jour pour 40m³
```

### Étape 8: Vérification Capteurs (5 min)

**Avec capteurs connectés:**

1. Brancher capteurs (ADS1115 en I2C @ 0x48, DS18B20 sur GPIO5)
2. Plonger sondes dans eau piscine
3. Attendre 30s stabilisation
4. Vérifier valeurs réalistes:
   - pH: 6.5 - 8.5 (piscine normale)
   - ORP: 400 - 800 mV
   - Température: 10 - 35°C

**Si valeurs aberrantes:**
- pH = 0 ou 14: Capteur non connecté, vérifier I2C (ADS1115 @ 0x48)
- ORP = 0: Sonde pas étalonnée ou HS, vérifier ADS1115
- Temp = -127°C: DS18B20 non détecté (pull-up 4.7kΩ manquant)

### Étape 9: Configuration MQTT (Optionnel)

**Si vous avez Home Assistant ou broker MQTT:**

1. **Interface web → Configuration → MQTT**
   ```
   Serveur: 192.168.1.10 (IP de votre broker)
   Port: 1883
   Topic: pool/sensors
   Username: (si nécessaire)
   Password: (si nécessaire)
   Activé: ☑️
   ```

2. **Sauvegarder et vérifier logs:**
   ```
   [INFO] MQTT connecté !
   ```

3. **Home Assistant Auto-Discovery**
   - Aller dans Paramètres → Appareils et Services → MQTT
   - Nouveaux appareils détectés automatiquement:
     * Pool Controller (appareil)
     * Piscine Température (capteur)
     * Piscine pH (capteur)
     * Piscine ORP (capteur)
     * Filtration Active (binary sensor)
     * Mode Filtration (select)

## 📋 Checklist Première Utilisation

### Avant de laisser tourner seul:

- [ ] Capteurs calibrés (pH 2 points, ORP 1 point)
- [ ] Valeurs pH/ORP cohérentes avec test manuel
- [ ] Limites de sécurité configurées
- [ ] Pompes testées en mode manuel (tubing dans eau, pas produits!)
- [ ] Relais filtration fonctionne
- [ ] WiFi stable (signal >-70 dBm)
- [ ] MQTT connecté (si utilisé)
- [ ] Watchdog ne déclenche pas (>5min sans reboot)
- [ ] Mot de passe admin changé

### Test Dosage (IMPORTANT)

**Ne jamais tester directement dans la piscine !**

1. **Préparation**
   ```
   - Remplir seau 10L eau du robinet
   - Ajouter quelques gouttes vinaigre (augmenter pH)
   - Plonger sonde pH
   - Attendre pH stable > 8.0
   ```

2. **Configuration test**
   ```
   Interface web → Configuration:
   - pH Target: 7.5
   - pH Enabled: ☑️
   - pH Pump: 1
   - Limit seconds/hour: 10 (sécurité!)
   ```

3. **Lancement**
   ```
   - Tubing pompe pH dans seau (pas dans bidon pH-!)
   - Observer logs série
   - Vérifier pompe démarre
   - Vérifier pompe s'arrête après 10s ou quand pH < 7.6
   ```

4. **Validation**
   ```
   ✅ Pompe démarre quand pH > target + 0.05
   ✅ Pompe s'arrête quand pH ≤ target
   ✅ Pompe s'arrête après limite horaire
   ✅ Pas de fuite tubing
   ✅ Sens rotation correct (aspire bidon)
   ```

## 🔧 Dépannage Rapide

### ESP32 ne démarre pas

**Symptôme**: Rien dans moniteur série

```bash
# Vérifier port
pio device list

# Essayer vitesse différente
pio device monitor -b 9600

# Maintenir bouton BOOT pendant upload
```

### WiFi ne se connecte pas

**Symptôme**: Reste en mode AP

**Solution**: Triple reset WiFi
```
Interface web → Système → Reset WiFi
OU
Bouton reset mot de passe (GPIO4) pendant 10s au démarrage
```

### Capteurs valeurs aberrantes

**pH toujours 0 ou 14:**
```
- Vérifier connexion I2C (SDA/SCL)
- Vérifier adresse ADS1115 (0x48)
- Vérifier alimentation ADS1115 (3.3V)
```

**ORP fixe à 0:**
```
- Sonde pas étalonnée ou HS
- Vérifier ADS1115 canal A1
- Tester avec multimètre: tension entre OUT et GND
```

**Température -127°C:**
```
- DS18B20 non détecté
- Vérifier pull-up 4.7kΩ sur GPIO5
- Vérifier alimentation 3.3V
```

### Watchdog redémarre en boucle

**Symptôme**: `[CRIT] Watchdog timeout!` répété

```
- Vérifier heap disponible (doit être >10KB)
- Consulter logs avant reboot
- Vérifier pas de boucle infinie dans le code
```

### MQTT ne se connecte pas

**Vérifier connexion broker:**
```bash
# Depuis PC sur même réseau
ping 192.168.1.10  # IP du broker

# Tester avec client
mosquitto_sub -h 192.168.1.10 -t test -v
```

**Vérifier credentials:**
```
Interface web → Configuration → MQTT
- Essayer sans username/password d'abord
- Vérifier pas d'espace avant/après
- Vérifier broker accepte connexions anonymes
```

### Mot de passe admin oublié

**Solution**: Factory Reset via bouton sur GPIO4

#### Procédure de reset (Factory Reset complet)

1. **Débrancher** l'alimentation ESP32
2. **Maintenir** le bouton reset enfoncé (GPIO4 → GND)
3. **Rebrancher** l'alimentation (continuer à maintenir le bouton)
4. **Maintenir 10 secondes** (la LED clignote pour indiquer la progression)
5. **Confirmation** : LED clignote rapidement 5× quand le reset est validé
6. **Redémarrage automatique** de l'ESP32

#### Effets du Factory Reset

- ✅ **Mot de passe réinitialisé** à `admin`
- ✅ **Token API régénéré**
- ✅ **Partition NVS complètement effacée** (toutes les données stockées)
- ✅ **Credentials WiFi supprimés** (retour en mode AP uniquement)
- ✅ **Redémarrage automatique** de l'ESP32
- ✅ **Assistant de configuration affiché** au prochain accès

#### Après le Factory Reset

1. **L'ESP32 redémarre** automatiquement en **mode AP uniquement**
2. **Connectez-vous au réseau WiFi** : `PoolControllerAP`
3. **Accédez à l'interface** via `http://192.168.4.1`
4. **L'assistant de configuration s'affiche automatiquement** :
   - Étape 1 : Définir un nouveau mot de passe administrateur
   - Étape 2 : Reconfigurer le WiFi (optionnel)
   - Étape 3 : Configurer l'heure (NTP ou manuel)
5. **Terminer la configuration** et accéder au tableau de bord

**Matériel requis**: Bouton externe NO (Normally Open) connecté entre **GPIO4** et **GND**. La résistance pull-up interne de l'ESP32 est activée automatiquement.

**⚠️ Attention** : Le Factory Reset efface **TOUTES** les données stockées dans la partition NVS (WiFi, préférences, etc.). Seule la configuration sauvegardée dans les fichiers JSON du système de fichiers LittleFS est conservée (consignes, limites, MQTT, etc.).

## 📱 Interface Web - Guide Rapide

### API Endpoints

| URL | Description |
|-----|-------------|
| `/` | Interface web principale |
| `/data` | API JSON données temps réel |
| `/get-config` | API JSON configuration |
| `/get-logs` | API JSON logs système |
| `/time-now` | API JSON heure actuelle |

### API Examples

**Données temps réel:**
```bash
curl http://poolcontroller.local/data

{
  "orp": 680.5,
  "ph": 7.32,
  "temperature": 24.1,
  "filtration_running": true,
  "ph_dosing": false,
  "orp_dosing": true,
  "ph_daily_ml": 120,
  "orp_daily_ml": 85,
  "ph_limit_reached": false,
  "orp_limit_reached": false
}
```

**Logs système:**
```bash
curl http://poolcontroller.local/get-logs

{
  "logs": [
    {
      "timestamp": 123456,
      "level": "INFO",
      "message": "Démarrage filtration"
    }
  ]
}
```

## 🎓 Prochaines Étapes

1. **Build et déploiement** → [BUILD.md](BUILD.md)
2. **Minification des fichiers web** → [MINIFICATION.md](MINIFICATION.md)
3. **Mises à jour OTA** → [UPDATE_GUIDE.md](UPDATE_GUIDE.md)
4. **Protocole UART (écran)** → [docs/uart_protocol.md](docs/uart_protocol.md)

## 📞 Support

**Problème non résolu ?**

1. Vérifier les logs: Interface web → Logs ou moniteur série
2. Vérifier BUILD.md pour problèmes de compilation
3. Ouvrir Issue GitHub avec:
   - Version firmware (voir Interface web → Système)
   - Logs complets
   - Configuration (masquer mots de passe)

---

**Bon démarrage ! 🏊‍♂️**

En cas de doute, toujours commencer en mode monitoring passif (dosage désactivé) pour valider les lectures capteurs avant d'activer l'automatisation.
