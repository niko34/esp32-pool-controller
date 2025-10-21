# Guide de Migration v1.0 → v2.0

## 📖 Vue d'Ensemble

La version 2.0 apporte une refonte complète de l'architecture avec:
- ✅ Code modulaire (fichiers séparés)
- ✅ Sécurité renforcée (limites journalières, watchdog)
- ✅ Contrôle PID pour dosage progressif
- ✅ Système de logs centralisé
- ✅ Lecture capteurs non-bloquante
- ✅ Validation des entrées utilisateur
- ✅ Alertes MQTT automatiques

## ⚠️ Compatibilité

**Configuration MQTT**: ✅ Compatible (fichier `mqtt.json` inchangé)
**Interface Web**: ⚠️ Nécessite mise à jour HTML
**Code personnalisé**: ❌ Refactorisation requise

## 🚀 Procédure de Migration

### Étape 1: Sauvegarde

**IMPORTANT**: Sauvegarder avant toute modification !

```bash
# Sauvegarder configuration actuelle
# Connecter l'ESP32 et télécharger mqtt.json
pio device monitor
# Dans le moniteur, aller sur http://IP_ESP32/get-config
# Copier la réponse JSON dans un fichier backup_config.json

# Sauvegarder ancien code
cd esp32_pool_controller
git init  # si pas déjà fait
git add .
git commit -m "Backup v1.0 avant migration"
git tag v1.0-backup
```

### Étape 2: Mise à Jour du Code

1. **Renommer l'ancien main.cpp**
   ```bash
   mv src/main.cpp src/main_v1_backup.cpp
   mv src/main_new.cpp src/main.cpp
   ```

2. **Vérifier les nouveaux fichiers**
   ```
   src/
   ├── config.h              ✓ Nouveau
   ├── config.cpp            ✓ Nouveau
   ├── logger.h              ✓ Nouveau
   ├── logger.cpp            ✓ Nouveau
   ├── sensors.h             ✓ Nouveau
   ├── sensors.cpp           ✓ Nouveau
   ├── pump_controller.h     ✓ Nouveau
   ├── pump_controller.cpp   ✓ Nouveau
   ├── filtration.h          ✓ Nouveau
   ├── filtration.cpp        ✓ Nouveau
   ├── mqtt_manager.h        ✓ Nouveau
   ├── mqtt_manager.cpp      ✓ Nouveau
   ├── web_server.h          ✓ Nouveau
   ├── web_server.cpp        ✓ Nouveau
   ├── main.cpp              ✓ Nouveau (ex main_new.cpp)
   └── main_v1_backup.cpp    ✓ Ancien code
   ```

3. **Compiler et vérifier**
   ```bash
   pio run
   # Si erreurs, voir section "Résolution de Problèmes"
   ```

### Étape 3: Configuration Personnalisée

Si vous aviez modifié des valeurs dans l'ancien `main.cpp`, les reporter dans les nouveaux fichiers:

#### Consignes pH/ORP

**V1** (main.cpp ligne ~41-42):
```cpp
float phTarget = 7.2f;
float orpTarget = 650.0f;
```

**V2** (config.h ligne ~42-43):
```cpp
struct MqttConfig {
  // ...
  float phTarget = 7.2f;    // ← Modifier ici
  float orpTarget = 650.0f; // ← Modifier ici
```

#### Limites d'Injection

**V1** (main.cpp ligne ~47-48):
```cpp
int phInjectionLimitSeconds = 60;
int orpInjectionLimitSeconds = 60;
```

**V2** (config.h ligne ~49-50 + nouvelles limites journalières):
```cpp
struct MqttConfig {
  // ...
  int phInjectionLimitSeconds = 60;
  int orpInjectionLimitSeconds = 60;
};

struct SafetyLimits {
  float maxPhMinusMlPerDay = 500.0f;      // ← NOUVEAU !
  float maxChlorineMlPerDay = 300.0f;     // ← NOUVEAU !
  // ...
};
```

#### Paramètres Pompes

**V1** (main.cpp ligne ~129-130):
```cpp
PumpControlParams phPumpControl = {5.2f, 90.0f, 1.0f};
PumpControlParams orpPumpControl = {5.2f, 90.0f, 200.0f};
```

**V2** (config.h ligne ~129-130 + PID):
```cpp
// Dans config.h
extern PumpControlParams phPumpControl;
extern PumpControlParams orpPumpControl;

// Dans config.cpp
PumpControlParams phPumpControl = {5.2f, 90.0f, 1.0f};
PumpControlParams orpPumpControl = {5.2f, 90.0f, 200.0f};

// NOUVEAU: Tuning PID dans pump_controller.h
struct PIDController {
  float kp = 2.0f;   // Proportionnel
  float ki = 0.5f;   // Intégral
  float kd = 1.0f;   // Dérivé
  // ...
};
```

#### Mode Simulation

**V1** (main.cpp ligne ~64-80):
```cpp
struct SimulationConfig {
  bool enabled = true;  // ← Passer à false pour production !
  // ...
};
```

**V2** (config.h ligne ~64-80):
```cpp
struct SimulationConfig {
  bool enabled = false;  // ← DÉSACTIVER pour production
  // ...
};
```

⚠️ **CRITIQUE**: Vérifier que `enabled = false` avant utilisation réelle !

### Étape 4: Calibration Capteurs

La V2 utilise le même algorithme de lecture, mais la calibration doit être appliquée différemment.

**V1** (inline dans main.cpp):
```cpp
void readSensors() {
  orpValue = (rawOrp / 4095.0f) * 1000.0f;
  phValue = (rawPh / 4095.0f) * 14.0f;
}
```

**V2** (sensors.cpp ligne ~104-108):
```cpp
void SensorManager::readRealSensors() {
  int rawOrp = analogRead(ORP_PIN);
  int rawPh = analogRead(PH_PIN);

  // Appliquer vos valeurs de calibration ici
  orpValue = (rawOrp / 4095.0f) * 1000.0f * YOUR_ORP_FACTOR;
  phValue = (rawPh / 4095.0f) * 14.0f + YOUR_PH_OFFSET;
}
```

Voir [CALIBRATION_GUIDE.md](CALIBRATION_GUIDE.md) pour la procédure complète.

### Étape 5: Upload et Test

1. **Upload du nouveau firmware**
   ```bash
   pio run --target upload
   ```

2. **Moniteur série**
   ```bash
   pio device monitor -b 115200
   ```

   Vérifier les messages de démarrage:
   ```
   [INFO] === Démarrage ESP32 Pool Controller v2.0 ===
   [INFO] Watchdog activé (30s)
   [INFO] LittleFS monté avec succès
   [INFO] Configuration chargée avec succès
   [INFO] Gestionnaire de capteurs initialisé (mode RÉEL)
   [INFO] Contrôleur de pompes initialisé
   [INFO] Gestionnaire de filtration initialisé
   [INFO] WiFi connecté: VotreSSID
   [INFO] IP: 192.168.1.XX
   [INFO] mDNS: poolcontroller.local disponible
   [INFO] Gestionnaire MQTT initialisé
   [INFO] Serveur Web démarré sur port 80
   [INFO] Initialisation terminée
   ```

3. **Vérification fonctionnelle**
   - Ouvrir `http://poolcontroller.local` ou `http://IP`
   - Vérifier lecture capteurs dans `/data`
   - Vérifier logs dans `/get-logs`
   - Tester changement mode filtration

### Étape 6: Configuration MQTT (si nécessaire)

La configuration MQTT est automatiquement conservée (`mqtt.json`).

Vérifier topics dans Home Assistant:
```
pool/sensors/temperature
pool/sensors/ph
pool/sensors/orp
pool/sensors/filtration_state
pool/sensors/filtration_mode
pool/sensors/filtration_mode/set    ← Commande
pool/sensors/alerts                  ← NOUVEAU !
pool/sensors/logs                    ← NOUVEAU !
```

## 🔧 Résolution de Problèmes

### Erreur de Compilation

**Erreur**: `config.h: No such file or directory`
```bash
# Vérifier que tous les fichiers sont présents
ls -la src/*.h src/*.cpp
```

**Erreur**: `multiple definition of mqttCfg`
```bash
# Vérifier qu'il n'y a qu'un seul main.cpp actif
mv src/main_v1_backup.cpp ~/backup/
```

**Erreur**: `WebServerManager does not name a type`
```bash
# Dépendances manquantes, nettoyer et recompiler
pio run --target clean
pio lib install
pio run
```

### ESP32 Bloque au Démarrage

**Symptôme**: Redémarrage watchdog en boucle

```
[CRIT] Watchdog timeout!
[INFO] Redémarrage...
```

**Solution**:
1. Désactiver temporairement watchdog dans `main.cpp`:
   ```cpp
   // esp_task_wdt_init(WATCHDOG_TIMEOUT, true);  // Commenter
   ```

2. Identifier le blocage via logs série
3. Vérifier mémoire disponible (doit être >20KB)

### Capteurs Valeurs Nulles

**Symptôme**: pH=0.00, ORP=0.0 constamment

**Cause**: Mode simulation activé

**Solution**:
```cpp
// Dans config.h
struct SimulationConfig {
  bool enabled = false;  // ← DOIT être false
```

### Configuration Perdue

**Symptôme**: Tous les paramètres revenus aux valeurs par défaut

**Cause**: Fichier `mqtt.json` corrompu ou effacé

**Solution**:
```bash
# Restaurer depuis backup_config.json
# Via interface web: aller dans Configuration
# Copier les valeurs depuis backup_config.json
# Sauvegarder
```

Ou manuellement via série (LittleFS):
```cpp
void loop() {
  // Code temporaire pour restaurer config
  File f = LittleFS.open("/mqtt.json", "r");
  if (!f) {
    // Fichier manquant, créer avec backup
    File fw = LittleFS.open("/mqtt.json", "w");
    fw.print(R"({"server":"192.168.1.10","port":1883,...})");
    fw.close();
  }
}
```

## 📊 Nouveautés v2.0

### Fonctionnalités Ajoutées

1. **Limites de sécurité journalières**
   - Max 500ml pH- par jour (configurable)
   - Max 300ml chlore par jour (configurable)
   - Alerte MQTT si limite atteinte

2. **Système de logs**
   - 100 dernières entrées en mémoire
   - Accessible via `/get-logs`
   - Niveaux: DEBUG, INFO, WARNING, ERROR, CRITICAL

3. **Watchdog matériel**
   - Redémarrage auto si blocage >30s
   - Protection contre plantages

4. **Contrôle PID**
   - Dosage progressif (plus de marche/arrêt brutal)
   - Réduction oscillations pH/ORP
   - Tunable via paramètres Kp, Ki, Kd

5. **Alertes MQTT**
   ```json
   Topic: pool/sensors/alerts
   {
     "type": "ph_limit",
     "message": "Limite journalière pH- atteinte",
     "timestamp": 123456789
   }
   ```

6. **Health Check automatique**
   - Vérification mémoire toutes les 60s
   - Détection valeurs capteurs aberrantes
   - Reconnexion auto WiFi/MQTT

### API Web Étendue

Nouveaux endpoints:

- `GET /get-logs` - Récupérer logs système
- `GET /data` - Données enrichies (dosage actif, limites, etc.)

Exemple réponse `/data`:
```json
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

## 🎯 Checklist Post-Migration

Avant de mettre en production:

- [ ] Code compilé sans erreur
- [ ] Upload ESP32 réussi
- [ ] Logs de démarrage OK (pas d'erreur CRITICAL)
- [ ] Interface web accessible
- [ ] Capteurs affichent valeurs réalistes
- [ ] MQTT connecté (si activé)
- [ ] Home Assistant voit les entités
- [ ] Mode simulation = `false`
- [ ] Limites de sécurité configurées selon volume piscine
- [ ] Calibration capteurs effectuée
- [ ] Test dosage manuel (mode maintenance)
- [ ] Watchdog ne déclenche pas en fonctionnement normal
- [ ] Backup configuration sauvegardé

## 🔄 Retour en Arrière (Rollback)

Si problème majeur, revenir à la v1:

```bash
# Restaurer ancien code
mv src/main.cpp src/main_v2_failed.cpp
mv src/main_v1_backup.cpp src/main.cpp

# Supprimer nouveaux fichiers (optionnel)
rm src/config.* src/logger.* src/sensors.* src/pump_controller.*
rm src/filtration.* src/mqtt_manager.* src/web_server.*

# Recompiler v1
pio run --target clean
pio run --target upload
```

## 📞 Support

Problème lors de la migration ?

1. **Vérifier ce guide** en premier
2. **Consulter les logs** via `/get-logs`
3. **Ouvrir une Issue** sur GitHub avec:
   - Version PlatformIO
   - Modèle ESP32
   - Logs série complets
   - Configuration (masquer mots de passe !)

---

**Bonne migration !** 🚀
