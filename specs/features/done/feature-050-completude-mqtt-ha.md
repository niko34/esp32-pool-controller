# Feature-050 — Complétude MQTT/HA : cumuls journaliers, volume programmé, reboot

- **Type** : feature (MQTT/HA, format minimal — précédent direct : feature-009)
- **Statut** : done
- **Version cible** : 2.14.0
- **Créée** : 2026-07-06 (audit de couverture web/API vs MQTT/HA — périmètre « rentable » choisi par l'utilisateur)
- **Domaines** : mqtt_manager.* uniquement — **pool-chemistry passe légère requise** (P2 = nouveau chemin de commande à distance vers le volume du mode Programmée)

## Périmètre

**P1 — Cumuls journaliers en entités HA** : nouveaux topics retain `{b}/ph_daily_ml` et `{b}/orp_daily_ml` (valeurs `safetyLimits.dailyPhInjectedMl`/`dailyOrpInjectedMl`, publiés au cycle 10 s avec dédup) + 2 blocs discovery `sensor` (unité mL, icône, `state_class: measurement` — la valeur retombe à 0 à minuit). Complément naturel des binary_sensors « Limite journalière » existants.

**P2 — Volume quotidien programmé commandable** : les topics state `{b}/ph_daily_target_ml`/`orp_daily_target_ml` existent déjà — ajouter `{b}/ph_daily_target_ml/set` + symétrique ORP (handlers drainCommandQueue, pattern exact des consignes ph_target : validation bornes, écriture sous configMutex, saveMqttConfig si changement, republish) + 2 blocs discovery `number` (min 0, max = raisonnable, step 10, unité mL). Bornes à valider par pool-chemistry — note : la branche scheduled plafonne déjà `effectiveDailyMl` par `maxPhMlPerDay`/`maxChlorineMlPerDay` (garde vive), la consigne est donc bornée en aval quoi qu'il arrive. C'était le « futur » explicitement noté par la feature-009.

**P3 — Bouton Reboot** : topic `{b}/reboot/set` (payload "PRESS" ou peu importe, non retain) + discovery `button` `device_class: restart`. Handler = même séquence propre que la route POST /reboot (flag de redémarrage différé consommé par la loop, flush MQTT offline — réutiliser le mécanisme existant, PAS de ESP.restart() direct dans drainCommandQueue).

**P4 — Politique documentée** : section « Exclusions volontaires » dans docs/MQTT.md — injections manuelles (sécurité chimique : pas de dosage par automatisation sans supervision), limites journalières/horaires (garde-fous ≠ préférences), calibration/OTA/factory-reset/Wi-Fi/auth (présence physique ou destructif). Transients non exposés (stabilisation, inject_remaining, blocked_reason — écarté 022) : sur besoin constaté.

## Contraintes

- unique_id figés : `{HA_DEVICE_ID}_ph_daily_ml`, `_orp_daily_ml`, `_ph_daily_target`, `_orp_daily_target`, `_reboot`.
- Patterns existants uniquement (dédup, enqueue, drainCommandQueue sous configMutex, validation stricte + warning si invalide).
- Contrats existants inchangés (topics, entités, diagnostic JSON garde ses champs).

## Critères d'acceptation

- [ ] 2 sensors « Dosage pH/Chlore aujourd'hui » (mL) dans HA, valeurs = cumuls temps réel (10 s). *(on-target)*
- [ ] 2 numbers « Volume quotidien pH/Chlore » commandables : changement depuis HA → persisté, reflété dans l'UI web, borné (validation + plafonnement aval). *(on-target)*
- [ ] Bouton « Redémarrer » dans HA → redémarrage propre (flush MQTT offline, même séquence que la route /reboot). *(on-target)*
- [x] Valeur invalide sur les /set → ignorée + warning (pattern 009).
- [x] docs/MQTT.md : section exclusions volontaires.
- [x] `pio run` 0 warning, 185 tests natifs verts, verdict pool-chemistry GO, reviewer Approved.

## Notes d'implémentation

- **Statut** : livré — tests verts (185/185 natifs), `pio run` 0 warning, revue **Approved**, verdict pool-chemistry **GO** (condition « validation contre la limite journalière VIVE » satisfaite).
- **Déroulé** : implémentation reprise en direct après 2 coupures de session du sous-agent ; les trois lots P1/P2/P3 + les 5 entités auto-discovery ont bien été livrés.
- **P1** — topics retain `{base}/ph_daily_ml` / `orp_daily_ml` (cumuls `safetyLimits.dailyPhInjectedMl`/`dailyOrpInjectedMl`, dédup cycle 10 s) + 2 `sensor` HA « Dosage pH/Chlore aujourd'hui » (mL, `state_class: measurement`).
- **P2** — `{base}/ph_daily_target_ml/set` + symétrique ORP + 2 `number` HA (0–2000, step 10 mL). Validation : payload non numérique ignoré + warning ; clamp négatif → 0 ; **REFUS si valeur > limite journalière VIVE** (`maxPhMlPerDay`/`maxChlorineMlPerDay`) + resync HA ; persistance NVS si changement, republish, log « (MQTT) ».
- **P3** — `{base}/reboot/set` + `button` HA `device_class: restart` ; redémarrage différé (flag consommé en tête de `drainCommandQueue`, `shutdownForRestart()` + `ESP.restart()`).
- **P4** — section « Exclusions volontaires » ajoutée à `docs/MQTT.md`.
- **Fichiers modifiés** : `src/mqtt_manager.h`, `src/mqtt_manager.cpp`, `src/version.h` (2.14.0) ; docs : `docs/MQTT.md`, `docs/subsystems/mqtt-manager.md`, `CHANGELOG.md`.
- **Reste on-target** (validation matérielle, hors périmètre logiciel) : voir les 5 entités apparaître dans HA ; tester un changement de volume quotidien depuis HA reflété dans l'UI web ; vérifier le refus si valeur > limite journalière vive ; actionner le bouton « Redémarrer ».
