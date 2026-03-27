# Guide de Mise à Jour

## Génération des Fichiers

### Compilation complète (recommandé)

Pour préparer les deux fichiers en une seule commande :

```bash
./build_all.sh
```

Ce script compile le firmware, construit le filesystem, puis copie `firmware.bin` et `littlefs.bin` à la racine du projet — prêts à être uploadés via l'interface web.

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

## Mise à Jour via Interface Web (OTA)

1. Accéder à http://poolcontroller.local
2. Aller dans **Paramètres > Système**
3. Sélectionner le fichier (firmware.bin ou littlefs.bin)
4. Cliquer sur **Mettre à jour**
5. Attendre le redémarrage automatique

> Si vous modifiez le code ET les fichiers web, faire 2 mises à jour séparées.

## Mise à Jour via USB

```bash
# Firmware
pio run --target upload

# Système de fichiers (2 étapes)
./build_fs.sh
~/.platformio/penv/bin/python ~/.platformio/packages/tool-esptoolpy/esptool.py --chip esp32 --port /dev/cu.usbserial-0001 write_flash 0x2B0000 .pio/build/esp32dev/littlefs.bin
```

> **Note** : Ne pas utiliser `pio run --target uploadfs` car il reconstruit le filesystem avec une mauvaise taille.

## Dépannage

| Problème | Solution |
|----------|----------|
| Update Failed | Vérifier la connexion WiFi et réessayer |
| ESP32 ne redémarre pas | Utiliser le bouton RESET ou débrancher |
| Interface inaccessible | Mettre à jour le filesystem via USB |
