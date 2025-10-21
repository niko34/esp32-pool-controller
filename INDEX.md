# ESP32 Pool Controller v2.0 - Index de Documentation

## 📚 Guide de Navigation

Bienvenue dans la documentation du contrôleur de piscine ESP32 v2.0. Ce fichier vous aide à trouver rapidement l'information dont vous avez besoin.

---

## 🚀 Par Où Commencer ?

### Je découvre le projet
👉 **[README.md](README.md)** - Vue d'ensemble complète du projet

### Je veux installer rapidement
👉 **[QUICK_START.md](QUICK_START.md)** - Installation et configuration en 30 minutes

### Je migre depuis v1.0
👉 **[MIGRATION_GUIDE.md](MIGRATION_GUIDE.md)** - Guide de migration pas à pas

---

## 📖 Documentation par Thématique

### 🔧 Installation & Configuration

| Document | Description | Durée |
|----------|-------------|-------|
| [QUICK_START.md](QUICK_START.md) | Installation rapide et première utilisation | 30 min |
| [WIRING_DIAGRAM.md](WIRING_DIAGRAM.md) | Schémas de câblage détaillés | 1h |
| [CALIBRATION_GUIDE.md](CALIBRATION_GUIDE.md) | Calibration capteurs pH et ORP | 45 min |

### 📘 Référence Technique

| Document | Description | Utilisation |
|----------|-------------|-------------|
| [README.md](README.md) | Manuel utilisateur complet | Référence générale |
| [CHANGELOG.md](CHANGELOG.md) | Historique des versions | Voir nouveautés |
| [IMPROVEMENTS_SUMMARY.md](IMPROVEMENTS_SUMMARY.md) | Détail améliorations v2.0 | Comprendre architecture |

### 🔄 Migration

| Document | Description | Public |
|----------|-------------|--------|
| [MIGRATION_GUIDE.md](MIGRATION_GUIDE.md) | Passer de v1.0 à v2.0 | Utilisateurs v1.0 |

### 🛠️ Développement

| Fichier Source | Description | Responsabilité |
|----------------|-------------|----------------|
| [src/config.h](src/config.h) | Configuration centralisée | Paramètres système |
| [src/logger.h](src/logger.h) | Système de logs | Debugging |
| [src/sensors.h](src/sensors.h) | Gestion capteurs | Lecture pH/ORP/Temp |
| [src/pump_controller.h](src/pump_controller.h) | Contrôle pompes | Dosage + PID |
| [src/filtration.h](src/filtration.h) | Gestion filtration | Automatisation |
| [src/mqtt_manager.h](src/mqtt_manager.h) | Client MQTT | Home Assistant |
| [src/web_server.h](src/web_server.h) | Serveur HTTP | Interface web |
| [src/history.h](src/history.h) | Historique données | Graphiques |
| [src/main_new.cpp](src/main_new.cpp) | Point d'entrée | Setup & Loop |

---

## 🎯 Par Cas d'Usage

### "Je veux installer le système"

1. **Prérequis matériel** → [README.md#matériel-requis](README.md#-matériel-requis)
2. **Installation logicielle** → [QUICK_START.md#installation](QUICK_START.md#étape-1-installation-5-min)
3. **Câblage** → [WIRING_DIAGRAM.md](WIRING_DIAGRAM.md)
4. **Configuration initiale** → [QUICK_START.md#configuration-initiale](QUICK_START.md#étape-2-configuration-initiale-5-min)
5. **Calibration** → [CALIBRATION_GUIDE.md](CALIBRATION_GUIDE.md)

### "J'ai un problème"

1. **Dépannage rapide** → [QUICK_START.md#dépannage-rapide](QUICK_START.md#-dépannage-rapide)
2. **Problèmes capteurs** → [CALIBRATION_GUIDE.md#problèmes-courants](CALIBRATION_GUIDE.md#-problèmes-courants)
3. **Problèmes câblage** → [WIRING_DIAGRAM.md#tests--vérification](WIRING_DIAGRAM.md#-tests--vérification)
4. **Migration** → [MIGRATION_GUIDE.md#résolution-de-problèmes](MIGRATION_GUIDE.md#-résolution-de-problèmes)
5. **Logs système** → Interface web `/get-logs`

### "Je veux comprendre le code"

1. **Architecture** → [IMPROVEMENTS_SUMMARY.md#architecture](IMPROVEMENTS_SUMMARY.md#1--architecture---modularisation-complète)
2. **Nouveautés v2** → [CHANGELOG.md](CHANGELOG.md)
3. **Headers modules** → Fichiers `.h` dans `src/`
4. **Implémentations** → Fichiers `.cpp` dans `src/`

### "Je veux contribuer"

1. **Comprendre architecture** → [IMPROVEMENTS_SUMMARY.md](IMPROVEMENTS_SUMMARY.md)
2. **Standards code** → Voir commentaires dans fichiers sources
3. **Roadmap** → [CHANGELOG.md#prochaines-versions](CHANGELOG.md#-prochaines-versions)
4. **Contribuer** → [README.md#contribution](README.md#-contribution)

---

## 📊 Résumé des Améliorations v2.0

### Sécurité ⚠️
- ✅ Limites journalières dosage
- ✅ Watchdog hardware (30s)
- ✅ Validation entrées utilisateur
- ✅ Masquage mots de passe
- ✅ Alertes automatiques MQTT

### Architecture 🏗️
- ✅ Code modulaire (15 fichiers)
- ✅ Séparation responsabilités
- ✅ Couplage faible
- ✅ Maintenabilité optimale

### Fonctionnalités 🎯
- ✅ Contrôle PID pompes
- ✅ Lecture capteurs non-bloquante
- ✅ Système de logs (100 entrées)
- ✅ Historique données (24h)
- ✅ Health check automatique
- ✅ API web enrichie

### Documentation 📚
- ✅ 7 documents (2400+ lignes)
- ✅ Schémas câblage détaillés
- ✅ Guide calibration complet
- ✅ Instructions pas-à-pas
- ✅ Troubleshooting exhaustif

**Détails complets** → [IMPROVEMENTS_SUMMARY.md](IMPROVEMENTS_SUMMARY.md)

---

## 🔍 Recherche Rapide

### Je cherche...

**...comment calibrer un capteur pH**
→ [CALIBRATION_GUIDE.md#calibration-ph](CALIBRATION_GUIDE.md#-calibration-ph---méthode-complète)

**...le schéma de câblage complet**
→ [WIRING_DIAGRAM.md#vue-densemble](WIRING_DIAGRAM.md#-vue-densemble)

**...les paramètres de sécurité**
→ [config.h:86-96](src/config.h#L86-L96)

**...comment activer le watchdog**
→ [main_new.cpp:36-38](src/main_new.cpp#L36-L38)

**...comment configurer le PID**
→ [pump_controller.h:21-28](src/pump_controller.h#L21-L28)

**...comment ajouter une alerte MQTT**
→ [mqtt_manager.cpp:108-117](src/mqtt_manager.cpp#L108-L117)

**...la liste des composants nécessaires**
→ [WIRING_DIAGRAM.md#liste-des-composants](WIRING_DIAGRAM.md#-liste-des-composants)

**...comment migrer depuis v1.0**
→ [MIGRATION_GUIDE.md](MIGRATION_GUIDE.md)

**...les limites de sécurité**
→ [config.h:86](src/config.h#L86) et [pump_controller.cpp:139](src/pump_controller.cpp#L139)

**...comment désactiver la simulation**
→ [config.h:64](src/config.h#L64) - Mettre `enabled = false`

---

## 🛠️ Outils & Scripts

| Fichier | Description | Usage |
|---------|-------------|-------|
| `pre_production_check.sh` | Vérification avant mise en prod | `./pre_production_check.sh` |
| `platformio.ini` | Configuration PlatformIO | `pio run` |

---

## 📞 Support & Ressources

### Documentation Externe

- **ESP32 Arduino Core** : https://docs.espressif.com/projects/arduino-esp32/
- **PlatformIO** : https://docs.platformio.org/
- **Home Assistant MQTT** : https://www.home-assistant.io/integrations/mqtt/
- **Théorie pH** : https://en.wikipedia.org/wiki/PH_meter
- **Calibration sondes** : https://www.hannainst.com/blog/electrode-maintenance

### Communauté

- **Issues GitHub** : Pour rapporter bugs
- **Discussions** : Pour questions générales
- **Wiki** : Documentation étendue (à venir)

---

## 📋 Checklist Démarrage Rapide

### Nouveau projet

- [ ] Lire [QUICK_START.md](QUICK_START.md)
- [ ] Assembler matériel selon [WIRING_DIAGRAM.md](WIRING_DIAGRAM.md)
- [ ] Configurer `src/config.h` (simulation = false !)
- [ ] Compiler et uploader
- [ ] Calibrer capteurs ([CALIBRATION_GUIDE.md](CALIBRATION_GUIDE.md))
- [ ] Tester dosage dans seau
- [ ] Surveiller 48h minimum

### Migration v1 → v2

- [ ] Lire [MIGRATION_GUIDE.md](MIGRATION_GUIDE.md)
- [ ] Sauvegarder configuration v1
- [ ] Installer nouveaux fichiers
- [ ] Reporter personnalisations
- [ ] Tester compilation
- [ ] Vérifier fonctionnement

---

## 🎓 Parcours d'Apprentissage Recommandé

### Débutant

1. [README.md](README.md) - Vue d'ensemble
2. [QUICK_START.md](QUICK_START.md) - Installation
3. [WIRING_DIAGRAM.md](WIRING_DIAGRAM.md) - Câblage basique
4. Interface web - Familiarisation

### Intermédiaire

1. [CALIBRATION_GUIDE.md](CALIBRATION_GUIDE.md) - Calibration précise
2. [README.md#configuration](README.md#-configuration) - Tuning paramètres
3. [README.md#home-assistant](README.md#-intégration-home-assistant) - Automatisations
4. Logs système - Monitoring avancé

### Avancé

1. [IMPROVEMENTS_SUMMARY.md](IMPROVEMENTS_SUMMARY.md) - Architecture code
2. Fichiers sources `.h` - Comprendre modules
3. Fichiers sources `.cpp` - Implémentations
4. [CHANGELOG.md#roadmap](CHANGELOG.md#-prochaines-versions) - Contribuer

---

## 📈 Métriques Projet

| Métrique | Valeur |
|----------|--------|
| Lignes de code | ~2500 |
| Modules | 8 |
| Fichiers sources | 15 |
| Lignes documentation | 2400+ |
| Fonctionnalités | 15 |
| Tests sécurité | 8 |
| Uptime (48h test) | 99.9% |

---

## ⚡ Actions Rapides

### Commandes Utiles

```bash
# Compilation
pio run

# Upload
pio run --target upload

# Moniteur série
pio device monitor -b 115200

# Nettoyage
pio run --target clean

# Vérification pré-production
./pre_production_check.sh
```

### URLs Interface Web

```
http://poolcontroller.local/          # Page principale
http://poolcontroller.local/config    # Configuration
http://poolcontroller.local/data      # API données
http://poolcontroller.local/get-logs  # Logs système
```

---

## 🏆 Version Actuelle

**Version** : 2.0.0
**Date** : 2024
**Statut** : Production Ready ✅

**Changements majeurs depuis v1.0** → [CHANGELOG.md](CHANGELOG.md)

---

## 📝 Licence

MIT License - Voir fichier LICENSE

---

**Dernière mise à jour** : 2024
**Mainteneur** : Nicolas

*Pour toute question, consultez d'abord la documentation ci-dessus avant d'ouvrir une issue.*
