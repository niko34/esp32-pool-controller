## Métadonnées
- **ID** : feature-035
- **Slug** : uniformisation-ecran-temperature
- **Type** : feature
- **Statut** : todo
- **Priorité** : normale
- **Version cible** : 2.3.0 (à confirmer)
- **Dernière mise à jour** : 2026-06-23
- **Spec(s) liée(s)** : `specs/features/done/feature-034-calibration-guidee-tous-modes.md` (patterns réutilisés : bloc stats, chip d'état, bouton « Calibrer la sonde », carte de calibration masquée/affichée)

## Domaines touchés
- [x] Capteurs (pH / ORP / température)
- [ ] Régulation (pump_controller, PID)
- [x] Web — routes HTTP / WebSocket
- [ ] Filtration / Éclairage
- [ ] MQTT / Home Assistant
- [ ] UART / écran LVGL
- [ ] OTA
- [ ] Authentification
- [ ] Historique / persistance
- [x] Frontend (`data/`)

## Agents requis
- [x] spec-reader
- [x] architect
- [ ] pool-chemistry — non requis : la température ne pilote aucun dosage (sert à la compensation de mesure), aucun risque chimique direct
- [x] ux-ui-designer — refonte de l'écran température pour l'aligner sur pH/ORP (bloc stats + chip + carte de calibration) ; arbitrage des états du chip et de la mise en page
- [ ] embedded-developer
- [x] web-ui-developer
- [x] test-engineer
- [x] code-reviewer
- [x] doc-writer

## Description
Uniformiser l'écran **Température** avec les écrans pH et ORP : un **bloc « stats » en haut** affichant la valeur courante + un **chip d'état de calibration** + un **bouton « Calibrer la sonde »**, et une **carte de calibration masquée** qui s'affiche au clic sur le bouton (comme pH/ORP). Réutiliser au maximum les patterns de feature-034.

## Contexte
- Les écrans pH/ORP ont (feature-034) : bloc Statistiques en haut (valeur + chips d'état + bouton « Calibrer la sonde » sous les badges) et une **carte de calibration** affichée seulement pendant une session (déclenchée par le bouton, masquant Régulation + Historique).
- L'écran **Température** (`#view-temperature`) est aujourd'hui **différent** : la valeur courante (`#temp_current_value`) est **à l'intérieur** de la section calibration, qui est **toujours visible** (pas de bouton déclencheur ni de bloc stats dédié). Le stepper de calibration (`temp_step1/2/3`, saisie d'une référence, `#temp_cal_start_btn`/`#temp_cal_cancel_btn`) est affiché en permanence.
- La calibration température est **par offset** (1 point) : l'utilisateur saisit une température de référence connue, le firmware calcule/stocke `tempCalibrationOffset` + `tempCalibrationDate` (via `/save-config`). Pas d'EZO, pas de multi-points, pas de pente.

## Comportement attendu

### Bloc « stats » en haut (nouveau)
- Affiche la **valeur de température courante** (avec offset appliqué), en grand, format cohérent avec les autres écrans (ex. `25.4 °C`).
- Un **chip d'état de calibration** dans une rangée de chips (cohérent visuellement avec pH/ORP), états :
  - 🟢 **Calibré** (offset appliqué / date de calibration présente) — afficher éventuellement l'ancienneté (« calibré le … » ou « il y a N j »).
  - 🟠 / ⚪ **Non calibré** (aucune calibration enregistrée).
  - (Pas de chip « filtre/mesure » : la température n'a pas de filtre médian/EMA comme pH/ORP.)
- Un **bouton « Calibrer la sonde »** sous la rangée, **toujours accessible**, qui ouvre la carte de calibration.

### Carte de calibration (masquée par défaut)
- Affichée **au clic** sur « Calibrer la sonde » (masque les autres cartes de l'écran, comme pH/ORP), avec un bouton **Fermer** qui revient à l'état nominal.
- Réutilise le **stepper température existant** (saisie de la référence → calibrer) ; le moderniser visuellement pour s'aligner sur le stepper pH/ORP si pertinent (états d'étape), sans changer la logique offset.
- La lecture live de la température reste visible pendant la calibration (pour juger la stabilité), cohérente avec pH/ORP.
- À la calibration réussie : chip passe à « Calibré », date mise à jour, retour possible à l'écran nominal.

### Cas limites
- **Sonde absente / NaN** : valeur « -- », chip neutre/non calibré, calibration accessible.
- **WebSocket déconnecté** : valeur figée/`--`, pas de crash.
- **Jamais calibré** : chip « Non calibré », bouton « Calibrer la sonde » bien visible.
- **Annulation de calibration** : retour à l'état nominal sans modifier l'offset.

## Comportement actuel
- Écran température : calibration toujours affichée, valeur courante imbriquée dedans, pas de bloc stats ni de chip ni de bouton déclencheur. Stepper `temp_step1/2/3` permanent.

## Contraintes techniques
- **Vanilla JS / HTML / CSS** dans `data/` (pas de framework). Réutiliser les composants feature-034 (`.chip`/`.chip--probe`/`.chip--cal`, show/hide de carte, bouton sous badges, `.cal-stepper`/`.calibration-steps`).
- Offline first ; français ; accessibilité (focus, ARIA, cible ≥ 44 px) ; responsive ≤ 500 px.
- Budget FS `spiffs` 832 KB — vérifier `./build_fs.sh` (réutilisation maximale pour limiter l'empreinte).
- **Ne pas modifier la logique de calibration offset** ni le contrat `/save-config` (`temp_calibration_offset` / `temp_calibration_date`) sauf nécessité justifiée.
- Pas de nouvelle route firmware a priori (la calibration température passe déjà par `/save-config` ; les données `temperature` / `temp_calibration_date` sont déjà exposées).

## Fichiers potentiellement concernés
- `data/index.html` — restructurer `#view-temperature` : bloc stats (valeur + chip + bouton), carte de calibration masquée [modif]
- `data/app.js` — chip d'état calibration température, show/hide carte, binding bouton « Calibrer la sonde », adaptation du stepper température existant [modif]
- `data/app.css` — styles (réutilisation, ajustements mineurs) [modif]
- `docs/features/page-temperature.md` — créer/mettre à jour la doc de la page [doc]
- `CHANGELOG.md` — entrée feature-035 [doc]

## Nouvelles signatures / API (optionnelle)
- Aucune nouvelle route/topic/WS a priori (réutilise `/save-config`, champs `temperature` + `temp_calibration_date` déjà exposés).

## Sécurité chimique (optionnelle)
- Dosage impliqué : **non**. La température sert à la compensation des mesures pH/ORP, elle ne déclenche aucune injection. pool-chemistry non requis.

## ADR attendu ?
- **Probable** : non
- **Critère(s) candidat(s)** : aucun (uniformisation UI, décisions réversibles, pas de contrainte structurante).
- **Sujet pressenti** : aucun

## Éléments visibles (pour doc-writer)

### Endpoints HTTP concernés
- Réutilisation de `/save-config` (calibration offset) et `/get-config` (lecture). Aucun nouveau.

### Champs WebSocket
- Réutilisation de `temperature` (et `temp_calibration_date` via `/get-config`). Aucun nouveau a priori.

### Topics MQTT
- aucun

### Documentation à produire / mettre à jour
#### Docs vivantes
- `docs/features/page-temperature.md` — bloc stats (valeur + chip calibration), bouton « Calibrer la sonde », carte de calibration

#### Docs transverses
- `CHANGELOG.md` — entrée feature-035 (changement visible UX)

## Critères d'acceptation
- [x] AC1 — L'écran température affiche un bloc « stats » en haut : valeur courante + chip d'état de calibration + bouton « Calibrer la sonde » (mise en page cohérente avec pH/ORP).
- [x] AC2 — Le chip reflète l'état réel : « Calibré » (offset/date présents, + ancienneté) / « Non calibré ».
- [x] AC3 — La carte de calibration est masquée par défaut et s'affiche au clic sur « Calibrer la sonde » ; un bouton Fermer revient à l'état nominal.
- [x] AC4 — La calibration offset (saisie de référence) fonctionne comme avant (logique inchangée) ; après succès, le chip passe à « Calibré » et la date est mise à jour.
- [x] AC5 — UI FR, accessible (focus/ARIA, ≥ 44 px), responsive ≤ 500 px, pas de crash si sonde absente / WS déconnecté.
- [x] `./build_fs.sh` sous la limite de partition.
- [x] `pio run` SUCCESS si firmware touché (a priori non).

## Plan de test manuel
1. Ouvrir l'écran Température → vérifier bloc stats (valeur + chip + bouton) en haut, cohérent avec pH/ORP.
2. Sonde non calibrée → chip « Non calibré ». Cliquer « Calibrer la sonde » → carte de calibration s'affiche.
3. Saisir une référence et calibrer → succès, chip passe à « Calibré » + date, retour nominal.
4. Annuler une calibration → retour nominal sans modifier l'offset.
5. Sonde absente / WS coupé → valeur « -- », pas de crash.
6. Responsive ≤ 500 px : bloc stats + bouton lisibles.

## Notes pour agent
- **ux-ui-designer d'abord** : arbitrer le bloc stats température (pas de chip filtre, calibration par offset), les états du chip (Calibré/Non calibré + ancienneté), et la modernisation éventuelle du stepper.
- Réutiliser au maximum les composants feature-034 (chip, carte masquée, bouton sous badges) pour cohérence ET empreinte FS.
- Ne pas changer la logique de calibration offset ni le contrat `/save-config`.
- Pas de pool-chemistry (température hors chemin de dosage).

## Hors périmètre
- Changement de la méthode de calibration température (reste offset 1 point).
- Filtre médian/EMA sur la température (hors périmètre).
- Toute modification de la régulation ou du dosage.

## Suivi / itérations

### Itération 1 — uniformisation écran température
- **Statut** : livré — tests verts (`pio run` SUCCESS, `./build_fs.sh` sous la limite de partition), revue OK.
- **Date** : 2026-06-23
- **Périmètre livré** : bloc stats en haut (valeur courante + chip de calibration `#temp-cal-chip` non cliquable + bouton « Calibrer la sonde »), carte de calibration masquée (`#temperature-card-calibration`) affichée au clic et masquant Activation + Historique, bouton Fermer. Stepper offset 3 étapes inchangé sur le fond. Chip : « Calibré · aujourd'hui / il y a N j / le JJ/MM » (vert), « Calibré · ancien (il y a N mois) » > 6 mois (ambré), « Non calibré » (gris). Lecture live conservée pendant la calibration, pas d'indicateur de stabilité. Anciens callouts `#temp_cal_date_header` / `#temp_calibrated_status` supprimés.
- **Logique inchangée** : calibration par offset 1 point côté client (`offset = ref − temperature_raw`), persistée via `POST /save-config` (`temp_calibration_offset` / `temp_calibration_date`). Aucun endpoint / topic MQTT / message WS nouveau.
- **AC couverts** : AC1, AC2, AC3, AC4, AC5 + build FS + `pio run`.
- **Fichiers modifiés** :
  - `data/index.html` — restructuration `#view-temperature`
  - `data/app.js` — `updateTempCalChip()`, `showTempCalibrationCard()` / `hideTempCalibrationCard()`, `updateTempCalibrationSteps()`, `bindTempCalibration()`
  - `data/app.css` — réutilisation `.ph-stats--single`, `.chip--cal`, `.cal-trigger-row`
  - `docs/features/page-temperature.md` — réécriture (doc)
  - `CHANGELOG.md` — entrée feature-035 dans `[2.2.3]` (doc)
- **ADR** : aucun (uniformisation UI, décisions réversibles, aucun critère de déclenchement rempli).
