# Bug — UI : valeurs pH/ORP/température figées quand un capteur devient indisponible

- **Type** : bug (format minimal `specs/modele.md`) — frontend uniquement
- **Statut** : done
- **Version cible** : 2.10.5
- **Créée** : 2026-07-27 (rapport utilisateur on-target v2.10.4 : sonde de température débranchée → logs corrects, mais tableau de bord et pages dédiées affichent toujours les dernières valeurs)
- **Domaine** : Frontend (`data/app.js`) — aucun fichier firmware touché, aucun chemin de dosage

## Symptôme observé

Sonde débranchée → le firmware invalide la lecture (NaN) et publie `null` (`/data` + WebSocket) — vérifié dans web_routes_data.cpp:51-73 et ws_manager.cpp:196-214. Mais l'UI continue d'afficher la dernière valeur connue, sans aucune indication d'indisponibilité, sur le tableau de bord et les widgets compacts.

## Cause racine

Pattern « skip si null » répété sur les sites d'affichage numérique : `if (el && data.x != null) { el.textContent = ... }` — quand la valeur passe à `null`, la mise à jour est sautée et l'ancienne valeur reste affichée indéfiniment. Sites identifiés :

- `updateDashboardMetrics` (app.js:3014-3022) : `#m-temp`, `#m-ph`, `#m-orp` ;
- widgets compacts : `#compact-ph` (≈2375), `#compact-orp` (≈2388), `#compact-temperature` (≈2405) ;
- à balayer : `updateDetailSections` et tout autre site affichant `data.ph` / `data.orp` / `data.temperature` (le développeur fait le tour exhaustif).

Incohérence avec les pages Réglages/Calibration qui font déjà le bon geste (`--` quand null, app.js:3450-3475). Par ailleurs le badge « Capteur indisponible » (feature-022) existe pour pH et ORP mais **pas pour la température**.

## Correctif attendu

1. **Convention unique** : toute valeur capteur `null`/NaN s'affiche « `--` » (convention déjà utilisée par les pages Réglages) sur TOUS les sites d'affichage numérique — dashboard, widgets compacts, sections détaillées. La branche `else` doit écrire explicitement `--` (jamais de skip).
2. **Badge température** : ajouter le segment « Capteur indisponible » (danger) pour la température dans `updateSensorBadges`, calqué sur les blocs pH/ORP existants (avec le même garde anti-faux-positif au boot : `latestSensorData !== null`).
3. **Graphiques** : ne PAS changer (sauter les null y est correct — trous dans les courbes).
4. Unité affichée avec `--` : conserver la structure existante (ex. `--<span class="compact-unit">°C</span>`) pour ne pas casser la mise en page.

## Critères d'acceptation

- [ ] Sonde température débranchée → « -- » (+ badge « Capteur indisponible ») partout où la température s'affiche, en ≤ 1 cycle de rafraîchissement.
- [ ] Même comportement pour pH et ORP quand leurs valeurs sont `null` (EZO absent/invalidé).
- [ ] Capteur rebranché → les valeurs réapparaissent sans recharger la page.
- [ ] Aucun changement sur les graphiques ni sur les pages Réglages (déjà correctes).
- [ ] Aucun fichier firmware modifié ; `./build_fs.sh` passe.

## Notes

- Piste distincte NON couverte ici (en attente de confirmation utilisateur) : sonde ORP débranchée du BNC → lecture flottante bruitée qui désarme le détecteur de gel (variance non nulle) → `orp_sensor_problem → OFF` à tort. Si confirmé → spec firmware séparée (critère de plausibilité/bruit, validation pool-chemistry obligatoire).

## Notes d'implémentation (v2.10.5, 2026-07-27)

- **Cycle** : diagnostic (firmware vérifié correct : NaN → null sur /data + WS) → web-ui-developer → code-reviewer (**Approved**, exhaustivité re-vérifiée par grep indépendant) → doc-writer. Frontend pur, une itération.
- **Fichiers modifiés** : `data/app.js` (updateStatusCards : #compact-ph/#compact-orp/#compact-temperature avec else `--` et unité conservée ; updateDashboardMetrics : #m-temp/#m-ph/#m-orp ; updateSensorBadges : segment température priority 0 danger « Capteur indisponible », garde anti-faux-positif boot identique à pH/ORP), `data/index.html` (élément #temperature-sensor-badge, même emplacement structurel que les badges pH/ORP), `src/version.h` (2.10.5).
- **Non touchés (voulu)** : graphiques (les trous sur null y sont corrects), pages Réglages/Calibration (déjà `--`), updateDetailSections (n'affiche pas de valeur capteur), tout src/ hors version.h.
- **Point vérifié en revue** : `temperature_enabled` ne gate pas la lecture DS18B20 côté firmware → désactiver la mesure dans la config ne publie pas null → pas de faux badge.
- **Validation** : syntaxe Node OK, `./build_fs.sh` OK (161 716 o minifiés, image 320 K).
- **Docs** : `CHANGELOG.md` [2.10.5], `docs/features/page-dashboard.md` (convention `--` + badge température), `docs/subsystems/ws-manager.md` (contrat null + représentation UI).
- **Reste on-target** (après `./deploy.sh ota-all`) : débrancher la sonde température → `--` + badge sur toutes les cartes en ≤ 1 rafraîchissement ; rebrancher → valeurs de retour sans recharger.
- **Piste connexe NON couverte, en attente de confirmation utilisateur** : sonde ORP débranchée du BNC → lecture flottante bruitée qui désarme le détecteur de gel (`orp_sensor_problem → OFF` observé le 24/07 à 14:13:30, contexte à confirmer) → si confirmé, spec firmware « détection sonde flottante » (critère plausibilité/bruit, validation pool-chemistry obligatoire).
