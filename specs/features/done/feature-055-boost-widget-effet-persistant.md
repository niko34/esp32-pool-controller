# Feature-055 — Widget Boost : indication d'effet persistante

- **Type** : feature (finition feature-053/054) — UI/observabilité, aucune logique de dosage
- **Statut** : done — tests verts, revue OK (build validé, cf. notes)
- **Version cible** : 2.18.2
- **Créée** : 2026-07-07 (retour utilisateur : le signal des leviers inertes ne doit pas être qu'un toast fugace)
- **Domaine** : ws_manager.cpp (2 champs WS calculés), data/index.html + app.js (ligne « Effet ») — **pas de pool-chemistry**

## Contexte

Le toast feature-054 est fugace (uniquement au clic UI, absent si activation via HA, perdu au rechargement). Le widget Boost du dashboard affiche « Actif/Inactif » + expiration mais **pas** quels leviers s'appliquent réellement. Rendre l'info **persistante et visible à tout moment** sur le widget.

## Comportement attendu

1. **ws_manager.cpp `_buildSensorJson`** : ajouter `boost_filtration_extended` (= `filtrationCfg.enabled`) et `boost_chlorine_boosted` (= `orpRegulationMode == "automatic"`) — champs booléens calculés au vol, publiés à chaque cycle sensor_data (donc valides après activation HA ET après reboot/rechargement).
2. **data/index.html** : nouvelle ligne « Effet » dans le widget Boost (`#detail-boost-effect-row` + `#detail-boost-effect`), masquée par défaut.
3. **data/app.js `updateBoostCard()`** : quand le Boost est actif, afficher la ligne Effet selon les 2 booléens :
   - filtration + chlore → « Filtration prolongée + surchloration » (badge Actif normal `--ok`).
   - chlore seul → « Surchloration seule » (`--ok`).
   - filtration seule → « Filtration prolongée seule » (`--ok`).
   - aucun → « Sans effet (filtration non gérée, ORP non automatique) » + **badge `--warn`** (attire l'œil).
   Quand inactif : masquer la ligne Effet, badge `--off`.
4. **Affichage immédiat** : le handler du bouton Activer mappe `r.filtration_extended`/`r.chlorine_boosted` vers `latestSensorData.boost_filtration_extended`/`boost_chlorine_boosted` avant `updateBoostCard()` (sinon délai jusqu'au prochain WS ~5 s).

## Hors périmètre
- Aucune modification du comportement Boost (dosage/filtration/limites) — purement affichage.
- Le toast feature-054 reste (complémentaire).

## Critères d'acceptation
- [ ] Boost actif, filtration non gérée → widget affiche « Surchloration seule » (on-target).
- [ ] Boost actif, ORP non automatique → « Filtration prolongée seule ». (on-target)
- [ ] Boost actif, les deux inertes → « Sans effet » + badge orange (`--warn`). (on-target)
- [ ] Boost actif nominal → « Filtration prolongée + surchloration ». (on-target)
- [ ] Info visible après activation via HA et après rechargement de page (champs WS, pas seulement réponse HTTP). (on-target)
- [x] Boost inactif → ligne Effet masquée. (logique `updateBoostCard`)
- [x] Aucun chemin de dosage touché.
- [x] `pio run` 0 warning, 199 tests natifs verts, `node --check` + `build_fs.sh` OK.

## Notes techniques
- **2 champs WS** ajoutés dans `_buildSensorJson()` : `boost_filtration_extended` (= `filtrationCfg.enabled`), `boost_chlorine_boosted` (= `orpRegulationMode == "automatic"`), calculés au vol → valides après activation HA et après reload.
- **UI** : ligne « Effet » (`#detail-boost-effect-row` / `#detail-boost-effect`) dans `data/index.html` ; `updateBoostCard()` couvre les 4 cas + badge `--warn` pour « sans effet », masquée si Boost inactif.
- **Affichage immédiat** : handler Activer mappe `filtration_extended`/`chlorine_boosted` de la réponse `/boost/start` vers `latestSensorData` avant `updateBoostCard()`.
- **Buffer** WS bumpé 1600 → 1664 octets.
- Aucune logique de dosage touchée. `version.h` → 2.18.2.
- Cases restantes non cochées = validation **on-target** (matériel/HA/reload réel).
