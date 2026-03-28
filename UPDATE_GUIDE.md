# Guide de Mise à Jour

## Génération des Fichiers

### Compilation complète (recommandé)

Pour préparer les deux fichiers en une seule commande :

```bash
./build_all.sh
```

Ce script compile le firmware, construit le filesystem, puis copie `firmware.bin` et `littlefs.bin` à la racine du projet — prêts à être uploadés via l'interface web ou `quick_update.sh`.

### Compilation séparée

#### Firmware (.bin)
```bash
pio run
```
Fichier généré : `.pio/build/esp32dev/firmware.bin`

#### Système de Fichiers (littlefs.bin)
```bash
./build_fs.sh
```
Fichier généré : `.pio/build/esp32dev/littlefs.bin`

> **Important** : Ne pas utiliser `pio run --target buildfs` (mauvaise taille de partition).

---

## Méthodes de mise à jour

| Méthode | USB requis | Réseau WiFi requis |
|---------|:----------:|:------------------:|
| Interface web (OTA) | ❌ | ✅ |
| `quick_update.sh` | ❌ | ✅ |
| Mise à jour automatique GitHub | ❌ | ✅ |
| `deploy.sh` | ✅ | ❌ |

---

## Mise à Jour via Interface Web (OTA) — WiFi uniquement

1. Compiler les fichiers avec `./build_all.sh`
2. Accéder à http://poolcontroller.local
3. Aller dans **Paramètres > Système**
4. Sélectionner le fichier (`firmware.bin` ou `littlefs.bin`)
5. Cliquer sur **Mettre à jour**
6. Attendre le redémarrage automatique

> Si vous modifiez le code ET les fichiers web, faire 2 mises à jour séparées.

---

## Mise à Jour via `quick_update.sh` — WiFi uniquement

Script automatisant l'envoi des fichiers `.bin` via HTTP (endpoint `/update`), sans USB.

```bash
# Compiler d'abord les fichiers
./build_all.sh

# Envoyer firmware uniquement
./quick_update.sh firmware

# Envoyer filesystem uniquement
./quick_update.sh filesystem

# Envoyer les deux (firmware, attend 30s, puis filesystem)
./quick_update.sh both

# Avec un hostname personnalisé
./quick_update.sh both 192.168.1.42
```

Par défaut, le script cible `poolcontroller.local`.

> Cette méthode est l'équivalent de l'interface web mais en ligne de commande — pratique pour les cycles de développement rapides.

---

## Mise à Jour Automatique depuis GitHub — WiFi uniquement

L'interface web peut vérifier et télécharger automatiquement la dernière version depuis GitHub :

1. Accéder à **Paramètres > Système**
2. Cliquer sur **Vérifier les mises à jour** — consulte la dernière release GitHub via `/check-update`
3. Si une nouvelle version est disponible, cliquer sur **Mettre à jour** — télécharge et installe via `/download-update`

> Le contrôleur se connecte directement à GitHub en HTTPS. Aucun USB, aucune manipulation de fichiers.

---

## Mise à Jour via USB (`deploy.sh`) — USB requis

Nécessite une connexion USB au PC. Utile pour la première installation ou si l'ESP32 n'est pas accessible sur le réseau.

> Contrairement à `quick_update.sh`, ce script **compile** le firmware et le filesystem avant de les uploader — pas besoin de lancer `build_all.sh` au préalable.

```bash
# Firmware + filesystem en une commande
./deploy.sh all

# Firmware uniquement
./deploy.sh firmware

# Filesystem uniquement
./deploy.sh fs
```

> **Note** : Ne pas utiliser `pio run --target uploadfs` car il reconstruit le filesystem avec une mauvaise taille.

Le port série est configuré dans `platformio.ini` (`upload_port`). Adapter selon le système :
- macOS : `/dev/cu.usbserial-*`
- Linux : `/dev/ttyUSB0` ou `/dev/ttyACM0`
- Windows : `COM3`, `COM4`, etc.

---

## Dépannage

| Problème | Solution |
|----------|----------|
| Update Failed | Vérifier la connexion WiFi et réessayer |
| ESP32 ne redémarre pas | Utiliser le bouton RESET ou débrancher |
| Interface inaccessible | Mettre à jour le filesystem via USB (`./deploy.sh fs`) |
| `quick_update.sh` : fichier introuvable | Lancer `./build_all.sh` d'abord |
