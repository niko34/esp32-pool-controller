# ESP32 Pool Controller v2.0

Contrôleur automatique de piscine basé sur ESP32 avec gestion pH, ORP (chlore), température et filtration automatique. Intégration complète avec Home Assistant via MQTT.

## 🎯 Fonctionnalités

### Mesures et Contrôle
- **pH** : Mesure et régulation automatique avec dosage pH- proportionnel
- **ORP (Redox)** : Mesure et dosage automatique de chlore
- **Température** : Sonde Dallas DS18B20
- **Filtration** : Contrôle automatique basé sur la température de l'eau

### Sécurité
- ⚠️ **Limites journalières** : Protection contre le surdosage (500ml pH- / 300ml chlore par défaut)
- ⚠️ **Limites horaires** : Temps maximum d'injection par heure configurable
- ⚠️ **Watchdog** : Redémarrage automatique en cas de blocage (30s)
- ⚠️ **Alertes MQTT** : Notifications en cas d'anomalie
- ⚠️ **Validation entrées** : Toutes les entrées utilisateur sont validées

### Automatisation
- **Mode Auto** : Calcul automatique du temps de filtration (température ÷ 2)
- **Mode Manuel** : Plages horaires personnalisées
- **Contrôle PID** : Dosage progressif pour éviter les oscillations
- **Intégration Home Assistant** : Auto-discovery MQTT

### Monitoring
- **Interface Web** : Configuration et visualisation temps réel
- **Logs système** : Buffer circulaire de 100 entrées
- **Historique** : Suivi des injections et alertes
- **mDNS** : Accessible via `poolcontroller.local`

## 📋 Matériel Requis

### Composants Principaux
- **ESP32 DevKit** (ou équivalent)
- **Capteur pH** analogique (0-14 pH)
- **Capteur ORP** analogique (0-1000 mV)
- **Sonde température DS18B20** (étanche)
- **2x Pompes doseuses péristaltiques** (12V DC)
- **2x Drivers moteur L298N** (ou équivalent)
- **Relais 5V/230V** pour pompe de filtration
- **Alimentation 5V/2A** pour ESP32
- **Alimentation 12V/2A** pour pompes

### Optionnel
- Boîtier étanche IP65
- Convertisseur DC-DC 12V→5V
- Protection surtension

## 🔌 Schéma de Câblage

```
ESP32 GPIO Layout:
├─ GPIO 34 (ADC1_6)  → Capteur ORP (signal analogique)
├─ GPIO 35 (ADC1_7)  → Capteur pH (signal analogique)
├─ GPIO 4            → Sonde température DS18B20 (OneWire + pull-up 4.7kΩ)
├─ GPIO 27           → Relais filtration
│
├─ Pompe 1 (pH-):
│  ├─ GPIO 25 → PWM (vitesse)
│  ├─ GPIO 32 → IN1 (direction)
│  └─ GPIO 33 → IN2 (direction)
│
└─ Pompe 2 (Chlore):
   ├─ GPIO 26 → PWM (vitesse)
   ├─ GPIO 18 → IN1 (direction)
   └─ GPIO 19 → IN2 (direction)
```

### Branchement Capteurs

**Capteur pH:**
```
pH Sensor → ESP32
  VCC     → 5V
  GND     → GND
  OUT     → GPIO 35 (via diviseur si >3.3V)
```

**Capteur ORP:**
```
ORP Sensor → ESP32
  VCC      → 5V
  GND      → GND
  OUT      → GPIO 34 (via diviseur si >3.3V)
```

**Sonde Température:**
```
DS18B20 → ESP32
  VCC   → 3.3V
  GND   → GND
  DATA  → GPIO 4 + Pull-up 4.7kΩ vers 3.3V
```

## 🚀 Installation

### PlatformIO (Recommandé)

1. **Cloner le projet**
   ```bash
   git clone <votre-repo>
   cd esp32_pool_controller
   ```

2. **Ouvrir avec VS Code + PlatformIO**
   ```bash
   code .
   ```

3. **Renommer le fichier principal**
   ```bash
   mv src/main.cpp src/main_old.cpp
   mv src/main_new.cpp src/main.cpp
   ```

4. **Compiler et uploader**
   - Connecter l'ESP32 via USB
   - Cliquer sur "Upload" dans PlatformIO
   - Ou via CLI: `pio run --target upload`

5. **Moniteur série**
   ```bash
   pio device monitor -b 115200
   ```

### Configuration Initiale

1. **Première connexion WiFi**
   - Au démarrage, l'ESP32 crée un point d'accès `PoolControllerAP`
   - Mot de passe: `12345678`
   - Se connecter et configurer votre réseau WiFi

2. **Accès interface web**
   - `http://poolcontroller.local` (ou IP affichée dans les logs)
   - Aller dans "Configuration" pour régler les paramètres

3. **Configuration MQTT (optionnel)**
   - Serveur: IP de votre broker MQTT
   - Port: 1883 (par défaut)
   - Topic de base: `pool/sensors`
   - Username/Password si nécessaire

## ⚙️ Configuration

### Paramètres Essentiels

**Consignes:**
- pH cible: 7.2 (recommandé: 7.0 - 7.4)
- ORP cible: 650 mV (recommandé: 650 - 750 mV)

**Limites de Sécurité:**
- pH- max/jour: 500 ml (ajuster selon volume piscine)
- Chlore max/jour: 300 ml (ajuster selon volume piscine)
- Temps injection max/heure: 60 secondes

**Filtration:**
- Mode Auto: Durée = Température ÷ 2 (ex: 24°C → 12h filtration)
- Mode Manuel: Définir plages horaires
- Mode Off: Filtration désactivée

### Calibration Capteurs

#### Calibration pH

1. **Solution pH 7.0** (neutre)
   ```
   - Rincer la sonde
   - Plonger dans solution pH 7.0
   - Noter la valeur brute analogique
   - Calculer: offset = 7.0 - valeur_mesurée
   ```

2. **Solution pH 4.0** (acide)
   ```
   - Rincer la sonde
   - Plonger dans solution pH 4.0
   - Noter la valeur
   - Calculer: slope = (7.0 - 4.0) / (valeur_pH7 - valeur_pH4)
   ```

3. **Appliquer dans le code** (sensors.cpp, ligne ~104):
   ```cpp
   float rawPh = analogRead(PH_PIN);
   float voltage = (rawPh / 4095.0f) * 3.3f;
   phValue = (voltage * slope) + offset;
   ```

#### Calibration ORP

1. **Solution de référence ORP** (généralement 470 mV à 25°C)
   ```
   - Rincer la sonde
   - Plonger dans solution
   - Noter valeur analogique
   - Calculer: factor = 470.0 / valeur_analogique
   ```

2. **Appliquer** (sensors.cpp):
   ```cpp
   float rawOrp = analogRead(ORP_PIN);
   orpValue = (rawOrp / 4095.0f) * 1000.0f * factor;
   ```

### Tuning PID (Avancé)

Les paramètres PID contrôlent la réactivité du dosage:
- **Kp** (Proportionnel): Réaction à l'erreur actuelle (défaut: 2.0)
- **Ki** (Intégral): Correction erreur accumulée (défaut: 0.5)
- **Kd** (Dérivé): Anticipation tendance (défaut: 1.0)

Modifier dans [pump_controller.h](src/pump_controller.h#L25-L28).

## 🏠 Intégration Home Assistant

### Auto-Discovery

Le contrôleur publie automatiquement sa configuration MQTT:
- Sensor: Température
- Sensor: pH
- Sensor: ORP
- Binary Sensor: État filtration
- Select: Mode filtration (auto/manual/off)

### Exemple Automation

```yaml
automation:
  - alias: "Alerte pH Anormal"
    trigger:
      - platform: numeric_state
        entity_id: sensor.piscine_ph
        above: 7.6
        for: "00:15:00"
    action:
      - service: notify.mobile_app
        data:
          title: "Piscine - pH Élevé"
          message: "pH: {{ states('sensor.piscine_ph') }}"

  - alias: "Notification Limite Injection"
    trigger:
      - platform: mqtt
        topic: "pool/sensors/alerts"
    condition:
      - condition: template
        value_template: "{{ 'limit' in trigger.payload_json.type }}"
    action:
      - service: notify.mobile_app
        data:
          title: "Piscine - Alerte Sécurité"
          message: "{{ trigger.payload_json.message }}"
```

## 🐛 Dépannage

### ESP32 ne démarre pas
- Vérifier alimentation 5V/2A minimum
- Vérifier câble USB (data, pas charge seule)
- Appuyer sur bouton BOOT pendant upload

### Capteurs valeurs aberrantes
- **pH toujours 0 ou 14**: Vérifier connexion capteur, diviseur tension
- **ORP fixe à 0**: Sonde pas étalonnée ou HS, vérifier GND commun
- **Température -127°C**: Sonde DS18B20 non détectée, pull-up manquant

### Pompes ne démarrent pas
- Vérifier alimentation 12V pompes
- Vérifier connexions drivers moteur
- Logs: chercher "LIMITE" (sécurité déclenchée)
- Vérifier mode simulation désactivé pour usage réel

### WiFi/MQTT déconnecté
- Vérifier portée WiFi (signal faible)
- MQTT: vérifier broker accessible (ping IP)
- Voir logs dans interface web `/get-logs`

### Watchdog Redémarrage
- Mémoire insuffisante: vérifier heap (doit être >10KB)
- Boucle infinie détectée: consulter logs avant reboot

## 📊 Mode Simulation

Pour tester sans matériel réel, modifier [config.h](src/config.h):

```cpp
struct SimulationConfig {
  bool enabled = true;  // Activer simulation
  float poolVolumeM3 = 50.0f;
  float initialPh = 8.5f;
  float initialOrp = 650.0f;
  float initialTemp = 24.0f;
  float timeAcceleration = 360.0f;  // 1h réelle = 10s simulation
  // ...
};
```

**Attention**: Désactiver (`enabled = false`) avant utilisation réelle !

## 🔐 Sécurité

### Bonnes Pratiques

1. **Produits chimiques**
   - Utiliser pH- et chlore liquides adaptés piscines
   - Stockage bidons dans local ventilé, hors gel
   - Ajuster limites journalières selon volume piscine

2. **Électricité**
   - Boîtier étanche IP65 minimum
   - Relais filtration avec protection 16A
   - Disjoncteur différentiel 30mA obligatoire

3. **Maintenance**
   - Calibrer sondes pH/ORP tous les 3 mois
   - Nettoyer électrodes mensuellement (solution acide pH)
   - Vérifier tubing pompes (usure, fuites)

4. **Monitoring**
   - Activer alertes MQTT
   - Vérifier logs quotidiennement (premiers jours)
   - Tester sécurités (déconnecter sonde → alerte?)

## 📈 Améliorations Futures

- [ ] Stockage historique LittleFS (graphiques 7 jours)
- [ ] Mode maintenance (purge manuelle pompes)
- [ ] Support multi-langues interface web
- [ ] OTA (mise à jour sans câble)
- [ ] Graphiques temps réel (Chart.js)
- [ ] Export CSV données

## 🤝 Contribution

Les Pull Requests sont bienvenues ! Pour changements majeurs:
1. Ouvrir une Issue pour discussion
2. Fork le projet
3. Créer branche feature (`git checkout -b feature/AmazingFeature`)
4. Commit (`git commit -m 'Add AmazingFeature'`)
5. Push (`git push origin feature/AmazingFeature`)
6. Ouvrir Pull Request

## 📄 Licence

MIT License - Voir fichier LICENSE

## ⚠️ Avertissement

Ce projet est fourni "tel quel" sans garantie. L'utilisation de produits chimiques et d'équipements électriques près de l'eau présente des risques. L'utilisateur est seul responsable de:
- La conformité aux réglementations locales
- La sécurité de l'installation
- Le bon dosage des produits chimiques
- La surveillance du système

**En cas de doute, consulter un professionnel.**

## 📞 Support

- **Issues GitHub**: Pour bugs et demandes de fonctionnalités
- **Discussions**: Pour questions générales
- **Wiki**: Documentation détaillée (à venir)

---

**Auteur**: Nicolas
**Version**: 2.0
**Dernière mise à jour**: 2024
