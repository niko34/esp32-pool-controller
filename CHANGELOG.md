# Changelog - ESP32 Pool Controller

## [1.1.0] - 2026-03-29

### Firmware
- **MQTT** : ajout des topics publiés `ph_dosing`, `orp_dosing`, `ph_limit`, `orp_limit`, `ph_target`, `orp_target`
- **MQTT** : ajout des topics de commande `ph_target/set` et `orp_target/set` (modification des consignes pH et ORP depuis HA ou MQTT)
- **MQTT** : correction du switch "Filtration Marche/Arrêt" — la commande `OFF` forçait l'arrêt de la filtration mais elle redémarrait immédiatement selon le planning
- **Home Assistant Auto-Discovery** : ajout de 6 nouvelles entités (Dosage pH Actif, Dosage Chlore Actif, Limite Journalière pH, Limite Journalière Chlore, Consigne pH, Consigne ORP)

### Documentation
- `docs/MQTT.md` : documentation complète des topics publiés, commandes et entités Home Assistant avec les noms tels qu'ils apparaissent dans l'interface HA
- `docs/API.md` : réécriture complète — tous les endpoints documentés (30+)
- `docs/UPDATE_GUIDE.md` : mise à jour avec les modes OTA de `deploy.sh`
- `deploy.sh` : ajout des modes `ota-firmware`, `ota-fs`, `ota-all` (compile + envoi OTA en une commande)
- Renommage `quick_update.sh` → `ota_update.sh`

---

## [1.0.3] - 2026-03-27

### Firmware
- **Factory reset** : détection par appui long 10s pendant le fonctionnement normal — plus besoin de couper l'alimentation
- Suppression des constantes `PH_SENSOR_PIN` / `ORP_SENSOR_PIN` (vestiges ADC interne non utilisés depuis le passage à l'ADS1115 I2C)

### Documentation
- Procédure factory reset mise à jour (fonctionnement runtime)
- Section Matériel Requis : schéma électronique et PCB illustrés, liens vers fichiers Gerber et STL
- `build_all.sh` documenté dans BUILD.md et UPDATE_GUIDE.md

---

## [1.0.1] - 2026-03-26

### Firmware
- **Bouton factory reset (GPIO32)** : appui de 10 secondes au démarrage pour réinitialisation usine complète
  - LED intégrée clignote pendant l'appui pour indiquer la progression
  - Efface la partition NVS (mot de passe, WiFi, MQTT, calibrations)
  - Préserve les consignes, limites et l'historique des mesures
  - L'ESP32 redémarre en mode AP avec l'assistant de configuration

### Hardware
- Ajout des fichiers Gerber (fabrication PCB) dans le dossier `hardware/`
- Ajout des fichiers STL du boîtier v3 (corps + couvercle) dans le dossier `hardware/`

---

## [1.0.0] - 2026-03-24 — Première release publique

### Fonctionnalités
- Régulation automatique pH et ORP (chlore) via algorithme PID
- Gestion filtration (auto / manuel / off) avec programmation horaire
- Contrôle éclairage avec programmation horaire
- Interface web avec tableau de bord temps réel (graphiques pH, ORP, température)
- Intégration Home Assistant via MQTT Auto-Discovery
- Mises à jour OTA via interface web (firmware et filesystem)
- Assistant de configuration au premier démarrage (mot de passe, WiFi, heure)
- Protocole UART pour écran LVGL externe
- Historique des mesures sur partition dédiée (préservé lors des mises à jour)
- Alertes MQTT en cas d'anomalie (valeurs aberrantes, limites atteintes, mémoire faible)
- Factory reset via bouton physique GPIO32
