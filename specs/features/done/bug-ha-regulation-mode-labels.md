# Bug — Modes de régulation pH/ORP en anglais dans Home Assistant

- **Type** : bug simple (finition — même famille que bug-ha-filtration-mode-labels)
- **Statut** : done
- **Version cible** : 2.17.3
- **Créée** : 2026-07-06 (retour utilisateur)
- **Domaine** : MQTT/HA discovery (mqtt_manager.cpp) — aucun backend, aucun handler

## Symptôme

Les selects « Mode Régulation pH » et « Mode Régulation ORP » dans HA affichent les valeurs brutes anglaises `automatic`/`scheduled`/`manual` (feature-009), alors que l'UI web affiche « Automatique »/« Programmée »/« Manuelle » et que le mode filtration a déjà été francisé.

## Correctif

Appliquer aux 2 blocs discovery `_ph_regulation_mode` et `_orp_regulation_mode` la même technique de templates que le mode filtration (bug-ha-filtration-mode-labels) — protocole sur le fil INCHANGÉ (`automatic`/`scheduled`/`manual`), handlers feature-009 INCHANGÉS :
- `options` = [« Automatique », « Programmée », « Manuelle »] (libellés UI web exacts).
- `value_template` : `{'automatic':'Automatique','scheduled':'Programmée','manual':'Manuelle'}.get(value, value)`.
- `command_template` : `{'Automatique':'automatic','Programmée':'scheduled','Manuelle':'manual'}[value]`.
- state_topic/command_topic/unique_id inchangés.

## Critères d'acceptation

- [ ] HA affiche « Automatique »/« Programmée »/« Manuelle » pour les 2 modes de régulation (on-target).
- [ ] Sélectionner un libellé applique le bon mode (command_template → valeur brute) ; l'état reflète le mode courant (value_template) (on-target).
- [x] Protocole MQTT (état + commande) inchangé sur le fil ; handlers PhRegulationMode/OrpRegulationMode inchangés.
- [x] `pio run` sans warning, 185/185 tests natifs verts.

## Notes techniques

- Miroir exact du pattern filtration (bug-ha-filtration-mode-labels, v2.15.0) et éclairage (bug-ha-eclairage-select, v2.17.2) : `value_template`/`command_template` ajoutés aux 2 blocs discovery `_ph_regulation_mode` et `_orp_regulation_mode`, `options` francisées.
- Protocole sur le fil INCHANGÉ (`automatic`/`scheduled`/`manual`), handlers `PhRegulationMode`/`OrpRegulationMode` INCHANGÉS — traduction entièrement côté HA.
- Fichiers : `src/mqtt_manager.cpp` (2 blocs discovery), `src/version.h` → 2.17.3.
- Reste on-target : vérifier visuellement les libellés FR dans HA et qu'une sélection applique le bon mode (2 AC on-target laissés ouverts).
