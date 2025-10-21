# Quick Start Guide - ESP32 Pool Controller v2.0

## 🚀 Mise en Route Rapide (30 minutes)

### Prérequis

✅ **Matériel:**
- ESP32 DevKit
- Câble USB (data, pas charge seule)
- Ordinateur (Windows/Mac/Linux)

✅ **Logiciels:**
- VS Code installé
- Extension PlatformIO installée

### Étape 1: Installation (5 min)

```bash
# 1. Télécharger le projet
git clone <votre-repo>
cd esp32_pool_controller

# 2. Ouvrir avec VS Code
code .

# 3. PlatformIO détectera automatiquement le projet
# Attendre que les dépendances se téléchargent
```

### Étape 2: Configuration Initiale (5 min)

**⚠️ IMPORTANT**: Désactiver le mode simulation avant utilisation réelle !

Éditer `src/config.h` ligne 64:

```cpp
struct SimulationConfig {
  bool enabled = false;  // ← METTRE À false POUR PRODUCTION
  // ...
};
```

**Autres paramètres à vérifier:**

```cpp
struct MqttConfig {
  // ...
  float phTarget = 7.2f;      // Ajuster selon votre piscine
  float orpTarget = 650.0f;   // 650-750 mV recommandé
};

struct SafetyLimits {
  float maxPhMinusMlPerDay = 500.0f;  // Ajuster selon volume
  float maxChlorineMlPerDay = 300.0f; // Ajuster selon volume
};
```

**Calcul limites journalières:**
```
Volume piscine = 40 m³
pH- pour baisser de 0.1 pH ≈ 0.3L pour 10m³
→ Max raisonnable = 500 ml/jour pour 40m³

Chlore pour remonter ORP de 100mV ≈ 0.2L pour 10m³
→ Max raisonnable = 300 ml/jour pour 40m³
```

### Étape 3: Compilation et Upload (5 min)

```bash
# Dans le terminal PlatformIO:

# 1. Compiler
pio run

# 2. Connecter ESP32 via USB

# 3. Identifier le port
pio device list
# Exemple: /dev/cu.usbserial-0001 ou COM3

# 4. Upload
pio run --target upload

# 5. Moniteur série
pio device monitor -b 115200
```

**Logs attendus:**
```
[INFO] === Démarrage ESP32 Pool Controller v2.0 ===
[INFO] Watchdog activé (30s)
[INFO] LittleFS monté avec succès
[INFO] Configuration chargée avec succès
[INFO] Gestionnaire de capteurs initialisé (mode RÉEL)
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

### Étape 5: Vérification Capteurs (5 min)

**Sans capteurs connectés (test initial):**

```bash
# Interface web
http://poolcontroller.local/data

# Réponse attendue (mode simulation off):
{
  "orp": <valeur aléatoire 0-1000>,
  "ph": <valeur aléatoire 0-14>,
  "temperature": null,
  "filtration_running": false,
  "ph_dosing": false,
  "orp_dosing": false
}
```

**Avec capteurs connectés:**

1. Brancher capteurs (voir [WIRING_DIAGRAM.md](WIRING_DIAGRAM.md))
2. Plonger sondes dans eau piscine
3. Attendre 30s stabilisation
4. Vérifier valeurs réalistes:
   - pH: 6.5 - 8.5 (piscine normale)
   - ORP: 400 - 800 mV
   - Température: 10 - 35°C

**Si valeurs aberrantes:**
- pH = 0 ou 14: Capteur non connecté ou HS
- ORP = 0: Sonde pas étalonnée
- Temp = -127°C: DS18B20 non détecté (pull-up 4.7kΩ manquant)

### Étape 6: Configuration MQTT (5 min - Optionnel)

**Si vous avez Home Assistant ou broker MQTT:**

1. **Interface web**
   ```
   http://poolcontroller.local/config
   ```

2. **Paramètres MQTT**
   ```
   Serveur: 192.168.1.10 (IP de votre broker)
   Port: 1883
   Topic: pool/sensors
   Username: (si nécessaire)
   Password: (si nécessaire)
   Activé: ☑️
   ```

3. **Sauvegarder**
   - L'ESP32 se connecte automatiquement
   - Vérifier logs: `[INFO] MQTT connecté !`

4. **Home Assistant**
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

- [ ] Mode simulation = `false`
- [ ] Capteurs calibrés (voir [CALIBRATION_GUIDE.md](CALIBRATION_GUIDE.md))
- [ ] Valeurs pH/ORP cohérentes avec test manuel
- [ ] Limites de sécurité configurées
- [ ] Pompes testées en mode manuel (tubing dans eau, pas produits!)
- [ ] Relais filtration fonctionne
- [ ] WiFi stable (signal >-70 dBm)
- [ ] MQTT connecté (si utilisé)
- [ ] Watchdog ne déclenche pas (>5min sans reboot)
- [ ] Backup configuration effectué

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

```bash
# Effacer config WiFi sauvegardée
# Dans platformio.ini, ajouter temporairement:
# build_flags = -DWIFI_RESET

# Ou bouton physique sur ESP32 (si board le permet)
```

### Capteurs valeurs fixes

**pH toujours 7.0:**
```cpp
// Vérifier dans sensors.cpp ligne ~104
// Commenter temporairement la calibration
phValue = (rawPh / 4095.0f) * 14.0f;  // Formule basique
```

**ORP toujours 0:**
```
- Vérifier GND commun ESP32 ↔ Capteur
- Tester avec multimètre: tension entre OUT et GND
- Devrait varier 0-3.3V selon solution
```

### Watchdog redémarre en boucle

**Symptôme**: `[CRIT] Watchdog timeout!` répété

```cpp
// Désactiver temporairement dans main.cpp setup():
// esp_task_wdt_init(WATCHDOG_TIMEOUT, true);  // Commenter cette ligne

// Identifier le blocage via logs
// Chercher dernière ligne avant reboot
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
Interface web → Configuration
- Essayer sans username/password d'abord
- Vérifier pas d'espace avant/après
- Vérifier broker accepte connexions anonymes
```

## 📱 Interface Web - Guide Rapide

### Pages Disponibles

| URL | Description |
|-----|-------------|
| `/` | Page d'accueil (index.html) |
| `/config` | Configuration système |
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
    },
    ...
  ]
}
```

## 🎓 Prochaines Étapes

1. **Calibration capteurs** → [CALIBRATION_GUIDE.md](CALIBRATION_GUIDE.md)
2. **Câblage complet** → [WIRING_DIAGRAM.md](WIRING_DIAGRAM.md)
3. **Migration v1→v2** → [MIGRATION_GUIDE.md](MIGRATION_GUIDE.md)
4. **Documentation complète** → [README.md](README.md)

## 📞 Support

**Problème non résolu ?**

1. Vérifier les logs: `/get-logs` ou moniteur série
2. Consulter [WIRING_DIAGRAM.md](WIRING_DIAGRAM.md) pour câblage
3. Lire [README.md](README.md) section Dépannage
4. Ouvrir Issue GitHub avec:
   - Version firmware
   - Logs complets
   - Configuration (masquer mots de passe)

---

**Bon démarrage ! 🏊‍♂️**

En cas de doute, toujours commencer en mode monitoring passif (dosage désactivé) pour valider les lectures capteurs avant d'activer l'automatisation.
