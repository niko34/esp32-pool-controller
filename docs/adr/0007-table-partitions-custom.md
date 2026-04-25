# ADR-0007 — Table de partitions custom (SPIFFS 1088 KB, history séparée 128 KB)

- **Statut** : Accepté
- **Date** : 2025 (refonte des partitions pour permettre OTA + historique préservé)
- **Spec(s) liée(s)** : aucune (décision infrastructure)

## Contexte

L'ESP32 utilisé embarque **4 MB de flash**. La table de partitions par défaut d'Arduino-ESP32 (`default.csv`) réserve :
- 2 slots OTA (`app0`, `app1`)
- un SPIFFS unique pour les données

Le projet a trois contraintes qui interdisent la table par défaut :

1. **OTA firmware + OTA filesystem** : il faut assez de place dans chaque slot `app0` / `app1` pour le binaire actuel (~1 MB et plus si on ajoute une lib), plus un SPIFFS dimensionné pour tenir toute l'UI web (HTML + CSS + JS + Chart.js + images).
2. **Historique préservé lors des mises à jour** : effacer l'historique des mesures à chaque upload filesystem OTA est inacceptable côté utilisateur (potentiellement plusieurs mois de données pH/ORP).
3. **Un factory reset doit effacer la NVS mais pas la config ni l'historique.**

## Décision

La table de partitions est décrite dans [`partitions.csv`](../../partitions.csv) :

| Name     | Type | SubType | Offset    | Size      | Taille |
|----------|------|---------|-----------|-----------|--------|
| nvs      | data | nvs     | 0x9000    | 0x5000    | 20 KB  |
| otadata  | data | ota     | 0xE000    | 0x2000    | 8 KB   |
| app0     | app  | ota_0   | 0x10000   | 0x160000  | 1408 KB |
| app1     | app  | ota_1   | 0x170000  | 0x160000  | 1408 KB |
| spiffs   | data | spiffs  | 0x2D0000  | 0x110000  | **1088 KB** |
| history  | data | spiffs  | 0x3E0000  | 0x20000   | 128 KB |

Points clés :

- **Deux partitions SPIFFS** (`spiffs` pour l'UI web, `history` pour l'historique des mesures) : l'OTA filesystem n'écrase que `spiffs`, jamais `history`.
- **Budget SPIFFS = 1088 KB** pour toute l'UI. Un garde-fou dans [`build_fs.sh`](../../build_fs.sh) vérifie que le LittleFS généré tient dans cette taille.
- Le fichier `history.json` sert de fallback sur `spiffs` si `history` n'est pas montée (ex. partition corrompue).

Le firmware et l'UI utilisent LittleFS plutôt que SPIFFS historique (malgré le nom de la partition SubType), via `<LittleFS.h>`.

## Alternatives considérées

- **Table par défaut Arduino-ESP32** (rejeté) — écrase l'historique à chaque upload filesystem.
- **Stocker l'historique en NVS** (rejeté) — NVS limitée à 20 KB et conçue pour des clés courtes, pas pour un JSON croissant.
- **SPIFFS au lieu de LittleFS** (rejeté) — fragmente vite, écriture plus lente, moins robuste en cas de coupure électrique.

## Conséquences

### Positives
- OTA filesystem sans perte d'historique → les utilisateurs mettent à jour en confiance.
- Le factory reset physique efface la NVS (calibrations, mot de passe, WiFi) sans toucher aux fichiers LittleFS (config) ni à l'historique.
- Budget SPIFFS connu et monitoré : les specs d'UI rappellent « Budget FS `spiffs` 1088 KB » (voir `_template.md`).

### Négatives / dette assumée
- La table de partitions est **propre au projet** : flasher un firmware tiers sur ce contrôleur écraserait la structure.
- Aucune sauvegarde externe de l'historique : une corruption flash le perd.

### Ce que ça verrouille
- Changer les offsets = *breaking change* OTA : un firmware avec une nouvelle table ne peut pas mettre à jour un appareil avec l'ancienne table de façon transparente. Prévoir un path de migration explicite.
- Le budget 1088 KB plafonne l'UI : pas possible d'y embarquer un gros framework sans sacrifier `app0` / `app1`.

## Références

- Code : [`partitions.csv`](../../partitions.csv)
- Build : [`build_fs.sh`](../../build_fs.sh)
- Doc : [`docs/BUILD.md`](../BUILD.md) — guide opérationnel
- Code : [`src/history.h`](../../src/history.h), [`src/history.cpp`](../../src/history.cpp) fallback logic
