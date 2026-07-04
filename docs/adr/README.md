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
| [0001](0001-capteurs-analogiques-ads1115.md) | Capteurs pH/ORP analogiques via ADS1115 + DFRobot_PH | Superseded by ADR-0014 |
| [0002](0002-mode-programmee-volume-quotidien.md) | Mode Programmée exprimé en volume quotidien (mL), pas en cadence | Accepté |
| [0003](0003-calibration-orp-cote-client.md) | Calibration ORP calculée côté client, pas par le firmware | Superseded by ADR-0014 |
| [0004](0004-mode-regulation-enum-3-valeurs.md) | Sélecteur de mode à 3 valeurs au lieu de booléens `ph_enabled` / `orp_enabled` | Accepté |
| [0005](0005-websocket-push-sans-polling.md) | WebSocket push pour le temps réel, pas de polling périodique | Accepté |
| [0006](0006-frontend-vanilla-js.md) | Frontend en vanilla JS sans framework ni bundler | Accepté |
| [0007](0007-table-partitions-custom.md) | Table de partitions custom (SPIFFS 1088 KB, history séparée) | Superseded by ADR-0009 puis ADR-0015 |
| [0008](0008-persistance-cumuls-journaliers-nvs.md) | Persistance NVS des cumuls journaliers + reset aligné minuit local | Accepté |
| [0009](0009-partition-coredump.md) | Partition coredump dédiée + redimensionnement history (128 KB → 64 KB) | Accepté |
| [0010](0010-stabilite-mqtt-reseau.md) | Stabilité MQTT et réseau : WiFi sans power save, pré-résolution DNS, backoff non réinitialisé | Accepté |
| [0011](0011-mqtt-task-dediee.md) | MQTT déplacé dans une tâche FreeRTOS dédiée | Accepté |
| [0012](0012-mapping-gpio-pcb-v2.md) | Mapping GPIO PCB v2 (Atlas EZO + 2ᵉ DS18B20 + CTN_AUX, plus d'ADS1115) | Accepté |
| [0013](0013-identification-sondes-onewire.md) | Identification des sondes DS18B20 par adresse ROM persistée NVS + workflow utilisateur | Accepté |
| [0014](0014-migration-atlas-ezo.md) | Migration Atlas EZO pH/ORP (PCB v2) — supersedes ADR-0001 | Accepté |
| [0015](0015-partition-app-1.5mb.md) | Partition application 1.5 MB (layout v2) | Superseded by ADR-0019 |
| [0016](0016-regulation-p-temporisee-vs-pid.md) | Régulation P temporisée par défaut (Kp seul, Ki=0, Kd=0) sur mesure filtrée | Accepté |
| [0017](0017-logique-metier-pure-humble-object-testabilite.md) | Logique métier pure (Humble Object) séparée de la couche hardware pour testabilité native | Accepté |
| [0018](0018-migration-uplot.md) | uPlot au lieu de Chart.js pour les graphiques de l'UI (contrainte Flash/FS) | Accepté |
| [0019](0019-partition-app-1664k.md) | Partitions app à 1664 KB (layout v3, spiffs 576 KB) | Accepté |
| [0020](0020-budget-horaire-dosage-unique.md) | Budget horaire de dosage unique, partagé auto + manuel | Accepté |

## Template

Copier `_template.md` pour créer un nouvel ADR.
