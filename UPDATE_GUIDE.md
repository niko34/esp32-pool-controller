# Guide de Mise à Jour OTA

## Gestion des Versions

Le firmware utilise un système de versionnage à 3 chiffres : `ANNÉE.MOIS.RÉVISION`

- **ANNÉE** : Année de la version (ex: 2025)
- **MOIS** : Mois de la version (1-12)
- **RÉVISION** : Numéro de révision du mois (commence à 1)

**Exemples :**
- `2025.1.1` : Première version de janvier 2025
- `2025.1.2` : Deuxième version de janvier 2025
- `2025.2.1` : Première version de février 2025

### Comment changer la version

Modifiez le fichier `src/version.h` :

```cpp
#define FIRMWARE_VERSION "2025.1.2"  // Mettre à jour cette ligne
```

## Types de Mises à Jour

L'ESP32 nécessite **2 fichiers distincts** pour une mise à jour complète :

### 1. Firmware (.bin)
Contient le code de l'application (C++/Arduino).

**Quand mettre à jour :**
- Modifications du code source (.cpp, .h)
- Corrections de bugs
- Nouvelles fonctionnalités

**Comment générer :**
```bash
pio run
```
Le fichier sera généré dans : `.pio/build/esp32dev/firmware.bin`

### 2. Système de Fichiers (.littlefs.bin)
Contient les fichiers web (HTML, CSS, JS, images).

**Quand mettre à jour :**
- Modifications de l'interface web (config.html, index.html)
- Changements de CSS/JavaScript
- Nouveaux fichiers statiques

**Comment générer :**
```bash
pio run --target buildfs
```
Le fichier sera généré dans : `.pio/build/esp32dev/littlefs.bin`

## Procédure de Mise à Jour via l'Interface Web

1. **Accéder à l'interface** : http://poolcontroller.local/config
2. **Aller dans l'onglet "Système"**
3. **Choisir le type de mise à jour** :
   - Firmware (.bin) : pour le code
   - Système de fichiers (.littlefs.bin) : pour les fichiers web
4. **Sélectionner le fichier** correspondant
5. **Cliquer sur "Mettre à jour"**
6. **Attendre la fin** (ne PAS couper l'alimentation !)
7. **L'ESP32 redémarre automatiquement**

⚠️ **IMPORTANT** : Si vous modifiez à la fois le code ET les fichiers web, vous devez faire **2 mises à jour séparées** :
1. D'abord le firmware (.bin)
2. Puis le système de fichiers (.littlefs.bin)

## Mise à Jour via Ligne de Commande (API)

L'API `/update` accepte les mises à jour via HTTP POST. Le système détecte automatiquement le type de fichier selon son extension.

### Mettre à jour le Firmware
```bash
curl -X POST -F "update=@.pio/build/esp32dev/firmware.bin" http://poolcontroller.local/update
```

### Mettre à jour le Système de Fichiers
```bash
curl -X POST -F "update=@.pio/build/esp32dev/littlefs.bin" http://poolcontroller.local/update
```

### Script de Déploiement Complet
```bash
#!/bin/bash
# deploy.sh - Script de déploiement automatique

ESP32_HOST="poolcontroller.local"

echo "🔨 Compilation du firmware..."
pio run || exit 1

echo "🔨 Construction du système de fichiers..."
pio run --target buildfs || exit 1

echo "📤 Mise à jour du firmware..."
curl -X POST -F "update=@.pio/build/esp32dev/firmware.bin" http://$ESP32_HOST/update
if [ $? -eq 0 ]; then
    echo "✅ Firmware mis à jour"
    echo "⏳ Attente du redémarrage (30s)..."
    sleep 30
else
    echo "❌ Erreur lors de la mise à jour du firmware"
    exit 1
fi

echo "📤 Mise à jour du système de fichiers..."
curl -X POST -F "update=@.pio/build/esp32dev/littlefs.bin" http://$ESP32_HOST/update
if [ $? -eq 0 ]; then
    echo "✅ Système de fichiers mis à jour"
    echo "✅ Déploiement terminé!"
else
    echo "❌ Erreur lors de la mise à jour du système de fichiers"
    exit 1
fi
```

**Utilisation :**
```bash
chmod +x deploy.sh
./deploy.sh
```

### Avec Adresse IP
Si mDNS ne fonctionne pas, utilisez l'adresse IP :
```bash
curl -X POST -F "update=@.pio/build/esp32dev/firmware.bin" http://192.168.1.100/update
```

### Vérifier la Version Actuelle
```bash
curl http://poolcontroller.local/get-system-info | jq '.firmware_version'
```

### Scripts de Déploiement Fournis

#### 1. Script Complet (deploy.sh)
Compile, construit et déploie automatiquement firmware + filesystem avec vérifications.
```bash
./deploy.sh                          # Utilise poolcontroller.local
./deploy.sh 192.168.1.100           # Avec adresse IP
```

**Fonctionnalités :**
- ✅ Vérification de connectivité
- ✅ Affichage de la version actuelle
- ✅ Compilation automatique
- ✅ Confirmation avant mise à jour
- ✅ Attente et vérification du redémarrage
- ✅ Affichage de la nouvelle version

#### 2. Mise à Jour Rapide (quick_update.sh)
Envoie les fichiers déjà compilés sans recompiler.
```bash
./quick_update.sh firmware          # Firmware seulement
./quick_update.sh filesystem        # Filesystem seulement
./quick_update.sh both              # Les deux
./quick_update.sh firmware 192.168.1.100  # Avec IP
```

### Intégration Continue (CI/CD)

Exemple avec GitHub Actions :
```yaml
name: Deploy to ESP32

on:
  push:
    branches: [ main ]

jobs:
  deploy:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v3

      - name: Setup PlatformIO
        run: |
          pip install platformio

      - name: Build Firmware
        run: pio run

      - name: Build Filesystem
        run: pio run --target buildfs

      - name: Deploy to ESP32
        env:
          ESP32_HOST: ${{ secrets.ESP32_HOST }}
        run: |
          curl -X POST -F "update=@.pio/build/esp32dev/firmware.bin" http://$ESP32_HOST/update
          sleep 30
          curl -X POST -F "update=@.pio/build/esp32dev/littlefs.bin" http://$ESP32_HOST/update
```

## Mise à Jour via USB (PlatformIO)

### Téléverser le Firmware
```bash
pio run --target upload
```

### Téléverser le Système de Fichiers
```bash
pio run --target uploadfs
```

### Téléverser les Deux (complet)
```bash
pio run --target upload && pio run --target uploadfs
```

## Checklist Avant Mise à Jour

- [ ] Version mise à jour dans `src/version.h`
- [ ] Code compilé sans erreur
- [ ] Fichiers testés localement
- [ ] Sauvegarde de la configuration actuelle
- [ ] ESP32 connecté au WiFi
- [ ] Alimentation stable

## Dépannage

**Erreur "Update Failed"**
- Vérifier que le bon type de fichier est sélectionné
- S'assurer que le fichier n'est pas corrompu
- Réessayer avec une connexion WiFi stable

**ESP32 ne redémarre pas**
- Débrancher/rebrancher l'alimentation
- Utiliser le bouton RESET physique

**Interface web inaccessible après mise à jour**
- Le système de fichiers n'a peut-être pas été mis à jour
- Téléverser le .littlefs.bin via USB : `pio run --target uploadfs`

## Génération des Fichiers de Mise à Jour

Pour distribuer une mise à jour :

```bash
# 1. Mettre à jour la version dans src/version.h
# 2. Compiler le firmware
pio run

# 3. Construire le système de fichiers
pio run --target buildfs

# 4. Les fichiers seront dans :
# - Firmware : .pio/build/esp32dev/firmware.bin
# - Filesystem : .pio/build/esp32dev/littlefs.bin
```

Vous pouvez ensuite distribuer ces fichiers aux utilisateurs pour une mise à jour OTA via l'interface web.
