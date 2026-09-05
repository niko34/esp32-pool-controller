# Bug/ajustement — Mode éclairage HA : switch → select (cohérence filtration)

- **Type** : ajustement (finition feature-052)
- **Statut** : done
- **Version cible** : 2.17.2
- **Créée** : 2026-07-06 (retour utilisateur)
- **Domaine** : MQTT/HA discovery (mqtt_manager.cpp) — aucun backend, aucun handler

## Demande

Le mode de programmation éclairage est exposé en `switch` ON/OFF dans HA. L'utilisateur veut une **liste déroulante** « Programmation » / « Désactivé », comme le select « Mode Filtration ».

## Correctif

Basculer le bloc discovery `_lighting_schedule` de `switch/` vers `select/`, avec la même technique de templates que le mode filtration (feature bug-ha-filtration-mode-labels) — protocole sur le fil INCHANGÉ (« ON »/« OFF »), backend booléen `scheduleEnabled` INCHANGÉ, handler `LightingSchedule` INCHANGÉ (reçoit toujours ON/OFF via command_template) :
- Composant `select/`, name « Mode Éclairage » (aligné sur « Mode Filtration »).
- `options` = [« Programmation », « Désactivé »].
- `state_topic` = `lighting_schedule` (publie ON/OFF), `command_topic` = `.../set`.
- `value_template` : `{'ON':'Programmation','OFF':'Désactivé'}` → libellé affiché.
- `command_template` : `{'Programmation':'ON','Désactivé':'OFF'}` → valeur brute envoyée au /set (le handler la reçoit inchangée).
- unique_id conservé `{HA_DEVICE_ID}_lighting_schedule`.

**Migration** : publier un payload retain VIDE sur l'ancien topic `homeassistant/switch/{HA_DEVICE_ID}_lighting_schedule/config` pour supprimer le switch orphelin de HA (feature-052 non publiée : n'existe que sur le device de test). Ligne de nettoyage commentée comme transitoire.

## Critères d'acceptation

- [ ] HA affiche « Mode Éclairage » en liste déroulante (Programmation / Désactivé) ; plus de switch orphelin. *(on-target — à valider dans HA)*
- [ ] Sélectionner « Programmation » active le planning (scheduleEnabled=true) ; « Désactivé » le coupe ; l'UI web reflète. *(on-target — à valider dans HA)*
- [x] Backend booléen + handler LightingSchedule inchangés (protocole ON/OFF sur le fil).
- [x] `pio run` sans warning, 185 tests natifs verts.

## Notes d'implémentation

- **Statut** : livré — build OK, revue OK. Reste on-target : voir le `select` « Mode Éclairage » apparaître dans HA + disparition du switch orphelin.
- **Changement** : bloc discovery `_lighting_schedule` basculé de `switch/` vers `select/` (name « Mode Éclairage », options Programmation/Désactivé) dans `publishDiscovery()`.
- **Templates** : `value_template` `{'ON':'Programmation','OFF':'Désactivé'}.get(value, value)` et `command_template` `{'Programmation':'ON','Désactivé':'OFF'}` — traduction purement côté HA, `unique_id` conservé (`poolcontroller_lighting_schedule`).
- **Inchangés** : protocole sur le fil (`ON`/`OFF`), booléen `lightingCfg.scheduleEnabled`, handler `LightingSchedule` (reçoit toujours ON/OFF via command_template).
- **Nettoyage orphelin** : `safePublish("homeassistant/switch/{id}_lighting_schedule/config", "", true)` (payload retain vide) publié avant le nouveau `select`. Ligne transitoire.
- **Fichiers** : `src/mqtt_manager.cpp`, `src/version.h` (2.17.2). Docs : `docs/MQTT.md`, `docs/subsystems/mqtt-manager.md`, `CHANGELOG.md`.
