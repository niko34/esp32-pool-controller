# Architecture Decision Records — ESP32 Pool Controller

Ce dossier contient les décisions d'architecture structurantes du projet, au format Michael Nygard (*Architecture Decision Records*, 2011).

## Pourquoi des ADR ?

Certaines décisions techniques ne sont pas lisibles dans le code : elles expliquent **pourquoi** telle option a été retenue plutôt que telle autre. Un ADR capture le contexte, l'alternative rejetée et la conséquence assumée, pour ne pas avoir à rediscuter la question six mois plus tard.

## Règles du dossier

- **Immuable une fois accepté** : un ADR n'est pas modifié. S'il est remis en cause, on en écrit un nouveau qui le supersède (et on change le statut de l'ancien en `Superseded by ADR-XXXX`).
- **Un ADR = une décision**. Pas de document fourre-tout.
- **Format court** : contexte, décision, conséquences. Pas de tutoriel.
- **Numérotation monotone** : `ADR-0001`, `ADR-0002`, …

## Index

| # | Titre | Statut |
|---|-------|--------|
| [0001](0001-capteurs-analogiques-ads1115.md) | Capteurs pH/ORP analogiques via ADS1115 + DFRobot_PH | Accepté |
| [0002](0002-mode-programmee-volume-quotidien.md) | Mode Programmée exprimé en volume quotidien (mL), pas en cadence | Accepté |
| [0003](0003-calibration-orp-cote-client.md) | Calibration ORP calculée côté client, pas par le firmware | Accepté |
| [0004](0004-mode-regulation-enum-3-valeurs.md) | Sélecteur de mode à 3 valeurs au lieu de booléens `ph_enabled` / `orp_enabled` | Accepté |
| [0005](0005-websocket-push-sans-polling.md) | WebSocket push pour le temps réel, pas de polling périodique | Accepté |
| [0006](0006-frontend-vanilla-js.md) | Frontend en vanilla JS sans framework ni bundler | Accepté |
| [0007](0007-table-partitions-custom.md) | Table de partitions custom (SPIFFS 1088 KB, history séparée) | Superseded by ADR-0009 |
| [0008](0008-persistance-cumuls-journaliers-nvs.md) | Persistance NVS des cumuls journaliers + reset aligné minuit local | Accepté |
| [0009](0009-partition-coredump.md) | Partition coredump dédiée + redimensionnement history (128 KB → 64 KB) | Accepté |
| [0010](0010-stabilite-mqtt-reseau.md) | Stabilité MQTT et réseau : WiFi sans power save, pré-résolution DNS, backoff non réinitialisé | Accepté |
| [0011](0011-mqtt-task-dediee.md) | MQTT déplacé dans une tâche FreeRTOS dédiée | Accepté |
| [0012](0012-mapping-gpio-pcb-v2.md) | Mapping GPIO PCB v2 (Atlas EZO + 2ᵉ DS18B20 + CTN_AUX, plus d'ADS1115) | Accepté |

## Template

Copier `_template.md` pour créer un nouvel ADR.
