## Métadonnées
- **ID** : feature-034
- **Slug** : calibration-guidee-tous-modes
- **Type** : feature
- **Statut** : doing — livré (tests verts, pool-chemistry GO, code-reviewer Approved) ; en attente de déplacement vers `done/`
- **Priorité** : normale
- **Version cible** : 2.3.0
- **Dernière mise à jour** : 2026-06-23
- **Spec(s) liée(s)** : `specs/features/done/feature-021-migration-atlas-ezo.md` (calibration EZO), `specs/features/done/feature-024-pente-sonde-ph.md` (diagnostic sonde), `specs/features/done/feature-025-lissage-mesures-ph-orp-pid.md` (reset filtre après calibration, readout brut)

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
- [x] pool-chemistry — la calibration des sondes pH/ORP est touchée (déverrouillage dans tous les modes) ; valider qu'aucun chemin de dosage n'est rendu dangereux
- [x] ux-ui-designer — **cœur de la demande** : refonte de l'accompagnement utilisateur de l'écran de calibration (steppers, états, minuteries). Invocation et livrable de design **obligatoires et approfondis** avant tout codage UI.
- [ ] embedded-developer
- [x] web-ui-developer
- [x] test-engineer
- [x] code-reviewer
- [x] doc-writer

## Description
Améliorer l'expérience de calibration des sondes pH et ORP sur deux axes :
1. **Rendre la calibration accessible quel que soit le mode de régulation** (automatique, programmée, manuelle), et non plus uniquement en mode automatique.
2. **Accompagner réellement l'utilisateur** pendant la calibration : étapes symbolisées (faites / en cours / restantes), minuteries d'attente pour respecter les temps de stabilisation, repères visuels clairs.

## Contexte
- **Verrou mode auto** : aujourd'hui le bloc d'accès à la calibration (`#ph-calibration-info` / équivalent ORP, contenant le bouton « Calibrer ») n'est affiché **que si le mode = automatique** ([`data/app.js:1225,1242`](../../data/app.js)). En mode **programmée** ou **manuelle**, l'utilisateur ne peut pas calibrer — alors que calibrer est une opération de maintenance utile **indépendamment** du mode de régulation choisi.
- **Accompagnement insuffisant** : les premières versions du projet utilisaient des **steppers** donnant des étapes claires. L'écran pH/ORP actuel repose sur un `.cal-point-block` avec une simple liste `<ol>` de micro-étapes, sans progression visuelle, sans état « étape faite/en cours/restante », et sans minuterie pour aider à attendre la stabilisation de l'électrode. L'utilisateur doit deviner combien de temps attendre.
- Un pattern de steppers existe encore pour la **calibration température** (`.calibration-steps` / `.step` / `.step__number`, [`data/index.html`](../../data/index.html)) — base d'inspiration possible, à moderniser.

## Comportement attendu

### 1. Calibration accessible dans tous les modes
- Le point d'entrée « Calibrer » (pH et ORP) est **visible et fonctionnel** en mode automatique, programmée **et** manuelle.
- Entrer en session de calibration depuis n'importe quel mode ouvre la carte de calibration comme aujourd'hui (elle remplace Régulation + Historique le temps de la session).
- À la sortie de la calibration, on **revient au mode précédemment sélectionné** (pas de bascule forcée en automatique).
- Les **garde-fous existants** restent en place : bouton « Calibrer » désactivé si une **injection est en cours**, et le reste des sécurités de calibration EZO (queue I²C, reset filtre après succès — feature-025) inchangé.

### 2. Écran de calibration guidé (cœur UX)
Sur les pages pH **et** ORP, l'écran de calibration doit :
- **Symboliser les étapes** sous forme de stepper : chaque étape a un état visuel distinct — **faite** (✓), **en cours** (mise en avant), **restante** (atténuée). La progression doit être lisible d'un coup d'œil.
- Étapes typiques pH (2 points) : rincer → plonger en pH 7.0 → attendre stabilisation → calibrer point milieu → rincer → plonger en pH 4.0 → attendre stabilisation → calibrer point bas → terminé. ORP (1 point) : rincer → plonger en solution étalon → attendre stabilisation → calibrer → terminé.
- **Minuteries de stabilisation** : pour chaque étape « attendre la stabilisation », proposer un **compte à rebours** (durée par défaut configurable, ex. 60 s) que l'utilisateur peut démarrer, avec indication visuelle de fin (« stabilisation atteinte, vous pouvez calibrer »). Optionnel : aider à juger la stabilité en affichant la **valeur brute** live (déjà disponible, feature-025) et/ou sa variation récente.
- **Readout brut conservé** : la valeur affichée pendant la calibration reste la mesure **brute** (décision feature-025), pas la lissée.
- Messages clairs, en français, accessibles (rôles ARIA, focus), cohérents avec le système de composants existant.
- Le flux doit rester **non bloquant** : on peut annuler à tout moment et revenir à l'écran normal.

### Cas limites
- **Injection en cours** : bouton « Calibrer » désactivé, message explicite (inchangé).
- **Mode programmée / manuelle** : calibration accessible ; au retour, le mode est conservé.
- **EZO injoignable** pendant la calibration : message d'erreur clair, pas de blocage de l'UI, étape non validée.
- **Calibration réussie** : étape marquée faite, filtre réinitialisé (warmup), retour propre au mode précédent.
- **Minuterie** : annulation possible ; l'utilisateur peut calibrer avant la fin du minuteur s'il le souhaite (la minuterie est une aide, pas une contrainte bloquante — à arbitrer par ux-ui-designer / pool-chemistry).
- **WebSocket déconnecté** : la mesure brute live se fige/`--`, le stepper reste utilisable, la minuterie (client) continue.

## Comportement actuel
- Accès calibration pH/ORP **uniquement en mode automatique** (`mode === "automatic"` gate dans `updatePhModeControls` / équivalent ORP).
- Écran de calibration = `.cal-point-block` avec liste `<ol>` statique de micro-étapes, **sans** progression visuelle, **sans** état d'étape, **sans** minuterie.
- Au retour de calibration, comportement de mode à vérifier (risque de retour forcé en automatique).

## Contraintes techniques
- **Vanilla JS / HTML / CSS** dans `data/` (pas de framework, pas de bundler) ; modules `<dialog>`/composants existants réutilisables.
- Offline first ; français ; accessibilité (focus, ARIA, contraste, cible tactile ≥ 44 px).
- Budget FS `spiffs` 832 KB — vérifier `./build_fs.sh` après ajouts.
- Ne pas modifier le protocole de calibration EZO ni les endpoints firmware sauf nécessité (le déverrouillage est surtout côté UI). Si un champ WS/route est nécessaire (ex. durée de stabilisation par défaut), le justifier.
- Minuteries gérées **côté client** par timestamps (pas de dépendance firmware), cohérentes avec `stabilization_remaining_s` déjà exposé si pertinent.
- Ne pas régresser les sécurités : injection en cours → calibration désactivée ; reset filtre après succès (feature-025).

## Fichiers potentiellement concernés
- `data/app.js` — retrait du gate `mode === "automatic"` sur l'accès calibration (pH + ORP) ; logique de stepper (état des étapes, progression) ; minuteries ; retour au mode précédent [modif]
- `data/index.html` — markup du stepper de calibration pH/ORP (états d'étape, zone minuterie) [modif]
- `data/app.css` — styles du stepper (états faite/en cours/restante), minuterie [modif]
- `src/web_routes_*.cpp` / `src/ws_manager.cpp` — uniquement si un champ/réglage s'avère nécessaire (à justifier) [modif éventuel]
- `docs/features/page-ph.md`, `docs/features/page-orp.md` — accès calibration tous modes + écran guidé [doc]
- `docs/subsystems/sensors.md` — si comportement de calibration documenté impacté [doc]

## Nouvelles signatures / API (optionnelle)
- A priori **aucune nouvelle route firmware** : le déverrouillage est côté UI, la calibration réutilise les endpoints existants (`/calibrate_ph*`, `/calibrate_orp`).
- Éventuelle constante de durée de stabilisation par défaut côté client (ex. 60 s) — à confirmer par ux-ui-designer.

## Sécurité chimique (obligatoire — calibration sondes touchée)
- Dosage impliqué : indirect — la **calibration** modifie l'étalonnage des sondes qui pilotent la régulation. Pas de modification de `pump_controller`.
- Limites journalières / horaires : non modifiées.
- Filtration requise avant action : la calibration n'est pas une injection ; à confirmer.
- Calibration capteur modifiée : **oui** (accès élargi à tous les modes).
- Algorithme PID touché : non.
- Risque identifié : permettre la calibration en mode **programmée**/**manuelle** pourrait coïncider avec un dosage actif. **pool-chemistry doit valider** : (a) qu'une session de calibration met le dosage en pause / le bloque proprement (ou que les gardes existantes suffisent), (b) que le reset du filtre après calibration (warmup → dosage bloqué, feature-025) se comporte correctement quel que soit le mode, (c) le bouton « Calibrer » reste désactivé pendant une injection en cours.

## ADR attendu ?
- **Probable** : non
- **Critère(s) candidat(s)** : aucun a priori (amélioration UX + déverrouillage d'un accès ; pas de décision structurante verrouillante). À reconsidérer si pool-chemistry impose une règle de pause de dosage pendant calibration (alors ADR possible).
- **Sujet pressenti** : éventuellement « dosage en pause pendant session de calibration » si retenu.

## Éléments visibles (pour doc-writer)

### Endpoints HTTP concernés
- Réutilisation des endpoints de calibration existants (aucun nouveau a priori).

### Champs WebSocket
- Réutilisation de `phRaw`/`orpRaw` (mesure brute live), `stabilization_remaining_s` si pertinent. Nouveaux champs à éviter sauf justification.

### Topics MQTT
- aucun

### Documentation à produire / mettre à jour
#### Docs vivantes
- `docs/features/page-ph.md` — calibration accessible tous modes + écran guidé (stepper, minuterie)
- `docs/features/page-orp.md` — idem ORP

#### Docs transverses
- `CHANGELOG.md` — entrée (changement visible UX)

## Critères d'acceptation
- [x] AC1 — Le bouton « Calibrer » (pH et ORP) est accessible et fonctionnel dans les modes **automatique, programmée et manuelle**. (`updatePhModeControls`/`updateOrpModeControls` : `calInfo.style.display = ""` inconditionnel)
- [x] AC2 — À la sortie de la calibration, le mode de régulation **précédent est conservé** (pas de bascule forcée). (la session ne touche pas `ph_regulation_mode`/`orp_regulation_mode`)
- [x] AC3 — L'écran de calibration affiche un **stepper** avec états distincts : étape **faite** (`is-completed`), **en cours** (`is-active` + `aria-current="step"`), **restante** (`is-upcoming`).
- [x] AC4 — Chaque étape d'attente propose une **minuterie / compte à rebours** avec indication de fin (`startCalTimer`, `CAL_STAB_DURATION_S = 60`, message « Stabilisation atteinte »).
- [x] AC5 — La valeur affichée pendant la calibration reste la mesure **brute** (non lissée). (readout feature-025 non régressé)
- [x] AC6 — Le bouton « Calibrer » reste **désactivé pendant une injection en cours** (sécurité inchangée) — garde ajouté côté pH (`ph_inject_remaining_s`/`ph_dosing`), existait déjà côté ORP.
- [x] AC7 — Après calibration réussie, le filtre est réinitialisé (warmup) quel que soit le mode (feature-025), et le dosage reste bloqué pendant le warmup. (comportement firmware mode-indépendant, non régressé)
- [x] AC8 — UI en français, accessible (focus, ARIA `aria-current`/`role="timer"`/`aria-live`), sans crash si EZO injoignable / WS déconnecté.
- [x] `./build_fs.sh` passe sous la limite de partition.
- [x] `pio run` compile sans nouveau warning — non concerné (frontend uniquement, aucun fichier `src/` modifié).

## Plan de test manuel
1. **Tous modes** : passer en mode programmée puis manuelle, vérifier que « Calibrer » est accessible et ouvre l'écran de calibration.
2. **Retour de mode** : calibrer en mode manuel → après calibration, vérifier qu'on est toujours en mode manuel.
3. **Stepper** : dérouler une calibration pH 2 points → vérifier que les étapes passent visuellement de restante → en cours → faite.
4. **Minuterie** : démarrer le compte à rebours d'une étape d'attente → vérifier l'indication de fin ; vérifier qu'on peut calibrer avant la fin si voulu.
5. **Sécurité** : lancer une injection manuelle → vérifier que « Calibrer » est désactivé.
6. **Calibration réussie** : vérifier reset filtre + warmup + dosage bloqué pendant warmup, retour au mode précédent.
7. **EZO débranché** pendant calibration → message d'erreur, pas de crash.
8. **Responsive / tactile** : vérifier le stepper et la minuterie sur petit écran (≤ 500 px).

## Notes pour agent
- **ux-ui-designer d'abord, en profondeur** : c'est explicitement la demande de l'utilisateur. Produire une vraie étude (hiérarchie, états du stepper, micro-interactions, gestion des minuteries, accessibilité, responsive, cohérence avec l'existant) AVANT toute implémentation. Étudier le pattern stepper température existant comme point de départ, le moderniser.
- Le déverrouillage tous modes est techniquement simple (retrait d'un gate UI) — la valeur de la feature est surtout dans l'**accompagnement**.
- Ne pas réintroduire un retour forcé en mode automatique.
- Réutiliser au maximum les composants existants (cards, chips, `<dialog>`, readout brut feature-025) pour limiter l'empreinte FS.
- Minuteries = aide, pas contrainte : à arbitrer si elles bloquent ou non le bouton « Calibrer » (avis pool-chemistry + ux-ui-designer).

## Hors périmètre
- Modification du protocole de calibration Atlas EZO.
- Changement des consignes/limites chimiques.
- Calibration de la température (hors périmètre sauf si le stepper générique est réutilisé sans surcoût).
- Refonte globale du design system.

## Suivi / itérations

### Itération 1 — calibration guidée tous modes (frontend)
- **Statut** : livré — tests verts, revue OK (pool-chemistry GO, code-reviewer Approved)
- **Date** : 2026-06-23
- **Périmètre réel** : frontend uniquement, aucun fichier `src/` touché.
- **Fichiers modifiés** :
  - `data/app.js` — retrait du gate `mode === "automatic"` sur l'accès calibration pH/ORP (`updatePhModeControls`/`updateOrpModeControls`) ; moteur stepper (`setCalStepState`, `advanceCalStepper`, `completeCalStep`, `resetCalStepper`, focus) ; minuterie de stabilisation client (`startCalTimer`/`cancelCalTimer`, `CAL_STAB_DURATION_S = 60`) ; indicateur de stabilité Δ60 s (`pushStabilitySample`/`renderStability`, fenêtre `CAL_STAB_WINDOW_MS`, seuils indicatifs `CAL_STAB_THRESHOLD`) ; garde injection pH ajouté sur `#ph_cal_trigger_btn`.
  - `data/index.html` — markup stepper `<ol class="calibration-steps" id="ph-cal-steps">` / `id="orp-cal-steps">` (états d'étape, boutons « C'est fait → », boutons de calibration), zones minuterie `#ph-cal-timer` / `#orp-cal-timer` (`role="timer" aria-live="polite"`), indicateurs de stabilité.
  - `data/app.css` — styles des états de stepper (`is-completed`/`is-active`/`is-upcoming`), minuterie (`.cal-timer`, barre de progression), indicateur de stabilité (`.is-stable`).
- **Notes techniques** :
  - Le maintien du mode est obtenu **par construction** : la session de calibration ne réécrit jamais `*_regulation_mode`. La carte Régulation est seulement masquée/réaffichée.
  - Minuterie + indicateur Δ60 s sont des **aides non bloquantes** : ils n'inhibent jamais le bouton « Calibrer ». Décision UX réversible, visible dans le code → **aucun ADR** requis.
  - Le reset filtre/warmup post-calibration (feature-025) reste mode-indépendant côté firmware (déclenché par le succès EZO) — non régressé.
- **Validation** : `pool-chemistry` GO (aucun chemin de dosage rendu dangereux ; gardes injection préservées et étendues au pH) ; `code-reviewer` Approved ; `./build_fs.sh` sous la limite de partition.

### Correctif post-itération 1 — accès calibration réellement déverrouillé (bug de structure)
- **Date** : 2026-06-23
- **Bug** : malgré le retrait du gate `mode === "automatic"` côté JS, la calibration restait inaccessible hors mode automatique. Cause : le bloc `#ph-calibration-info` / `#orp-calibration-info` (callouts + bouton Calibrer) était **imbriqué dans `#ph-regulation-params` / `#orp-regulation-params`** (conteneur du mode automatique), masqué avec lui par `updatePhModeControls`/`updateOrpModeControls` dans les autres modes.
- **Correctif** (`data/index.html`) : déplacement des blocs `#ph-calibration-info` / `#orp-calibration-info` HORS des conteneurs de mode automatique, en bas de la carte de régulation, comme frères des blocs de mode → visibles dans tous les modes. IDs préservés, structure équilibrée, `./build_fs.sh` SUCCESS.

### Itération 2 — badge d'état de calibration + bouton Calibrer toujours accessible
- **Statut** : livré — tests verts, revue OK (code-reviewer Approved, `./build_fs.sh` SUCCESS ; frontend uniquement, pool-chemistry non requis)
- **Date** : 2026-06-23
- **Fichiers modifiés** :
  - `data/index.html` — chips d'état de calibration `#ph-cal-chip` / `#orp-cal-chip` (`<span role="status" aria-live="polite">`, non cliquables) ajoutés dans la rangée de chips ; suppression des 3 anciens callouts (`#ph_calibrated_status`/`#ph_inhibition_chip`/`#ph_ezo_unreachable` + équivalents ORP) ; bouton « Calibrer » + hint `#ph_cal_hint`/`#orp_cal_hint` déplacés sous la rangée de chips.
  - `data/app.js` — `setCalChip(prefix, …)` (classe, libellé chip, libellé/disabled du bouton, hint) et `renderCalibrationStatus()` réécrits : mapping `phCalPoints`/`orpCalPoints` → chip + libellé de bouton adaptatif (« Calibrer » / « Continuer la calibration » / « Recalibrer »). Garde injection conservé (prioritaire, appliqué dans `_updateInjectButtons`).
  - `data/app.css` — variante `.chip--cal` du chip de calibration.
- **Demande utilisateur** : remplacer le callout « Calibré 2 points » + le bouton Calibrer noyés dans la carte de régulation par un **chip d'état de calibration** placé dans la rangée de chips existante des pages pH/ORP (à côté du chip sonde « Sonde excellente » et du chip mesure « Mesure stable »). Le chip indique l'état de calibration (OK / recommandée / requise / EZO indisponible). Le **bouton Calibrer** est sur la ligne en dessous, **toujours accessible** (y compris quand la calibration est OK, pour recalibrer).
- **Périmètre** : frontend uniquement (`data/index.html`, `data/app.js`, `data/app.css`). Aucun firmware, aucune nouvelle route/topic/champ WS (réutilise `phCalPoints`/`orpCalPoints` + état EZO déjà exposés). Pas de modif du protocole EZO ni des limites chimiques → **pool-chemistry non requis** (présentation de statut, pas de chemin de dosage touché).
- **États du chip (à arrêter par ux-ui-designer)** :
  - 🟢 Calibré (pH : ≥ 2 points ; ORP : ≥ 1 point)
  - 🟠 Calibration recommandée (pH : 1/2 points)
  - 🔴 Calibration requise (0 point → régulation auto inhibée)
  - ⚪ EZO indisponible
- **Articulation** : le chip remplace/condense les callouts actuels (`#ph_calibrated_status`, `#ph_inhibition_chip`, `#ph_ezo_unreachable` + équivalents ORP). Le bouton Calibrer (`#ph_cal_trigger_btn`/`#orp_cal_trigger_btn`) reste accessible en dessous, avec son garde « injection en cours ». Cliquabilité du chip (ouvrir l'écran de calibration ou un détail) à arbitrer par ux-ui-designer.
- **Critères d'acceptation itération 2** :
  - [x] AC9 — Un chip d'état de calibration apparaît dans la rangée de chips des pages pH et ORP, à côté des chips sonde/mesure (`#ph-cal-chip` / `#orp-cal-chip`).
  - [x] AC10 — Le chip reflète l'état réel : calibré (pH « Calibré 2/2 » / ORP « Calibré ») / 1/2 (pH partiel) / requise / EZO indisponible. ORP : pas d'état « 1/2 » (1 seul point).
  - [x] AC11 — Le bouton Calibrer reste accessible quel que soit l'état (libellé adaptatif « Calibrer » / « Continuer la calibration » / « Recalibrer »), sous les chips, dans tous les modes.
  - [x] AC12 — Le garde « injection en cours » reste actif sur le bouton (pH + ORP), prioritaire sur l'état de calibration.
  - [x] AC13 — UI FR, accessible (chip non cliquable `role="status" aria-live="polite"`), robuste si EZO injoignable (chip gris « EZO indisponible », bouton désactivé) / WS déconnecté (« Calibration — », bouton actif).
  - [x] `./build_fs.sh` sous la limite de partition.
- **Agents** : ux-ui-designer (arbitrage placement/états/cliquabilité), web-ui-developer, test-engineer, code-reviewer, doc-writer.
- **Notes techniques** :
  - Le chip est volontairement **non cliquable** (`<span role="status" aria-live="polite">`) : c'est une annonce d'état, l'action (calibrer) est portée par le bouton voisin. Décision UI réversible, visible dans le code → **aucun ADR**.
  - **Correctif de libellé neutre** : tant qu'aucune donnée n'est reçue (`phCalPoints`/`orpCalPoints == null`) et hors EZO réellement injoignable, le chip affiche « Calibration — » (gris neutre) et le bouton reste **actif** — on ne désactive le bouton que sur preuve d'EZO injoignable (`pts < 0`), évitant un faux « Calibration requise » alarmant au chargement.
- **Reste à faire (it. 2)** : aucune — itération 2 livrée.

### Itération 3 — bouton « Calibrer la sonde » sous les badges
- **Statut** : livré (volet A) — tests verts, revue OK (`./build_fs.sh` SUCCESS ; frontend uniquement, pool-chemistry non requis). **Volet B ABANDONNÉ** (décision utilisateur 2026-06-23 : pas de réintroduction d'horodatage de calibration côté firmware ; l'alerte « calibration périmée » n'est pas implémentée).
- **Fichiers modifiés (volet A)** : `data/index.html`, `data/app.js`, `data/app.css`.
- **Date** : 2026-06-23
- **Demande utilisateur** : (A) le bouton de calibration s'affiche **sous la ligne des badges** (sous la `.chip-row`, près du chip de calibration), avec un **libellé unifié « Calibrer la sonde »** (au lieu des libellés adaptatifs « Calibrer / Continuer la calibration / Recalibrer »), pour pH **et** ORP. (B) le chip d'état de calibration affiche un **message clair lorsque la calibration doit être refaite** parce qu'elle dépasse le **délai prévu** (calibration périmée).

#### Volet A — placement + libellé du bouton (frontend simple)
- Déplacer le bouton de calibration (`#ph_cal_trigger_btn` / `#orp_cal_trigger_btn`) pour qu'il soit rendu **directement sous la `.chip-row`** (rangée des badges sonde/filtre/calibration), et non plus en bas de la carte de régulation dans `#ph-calibration-info` / `#orp-calibration-info`.
- **Libellé fixe « Calibrer la sonde »** quel que soit l'état (remplace le libellé adaptatif de l'itération 2).
- Conserver : garde « injection en cours » (prioritaire), accessibilité (≥44px), responsive, le hint contextuel sous le bouton.
- Périmètre : frontend uniquement (`data/index.html`, `data/app.js`, `data/app.css`). pool-chemistry non requis.

#### Volet B — alerte « calibration périmée » (NÉCESSITE DU FIRMWARE — décision requise)
- **Blocage constaté** : il n'existe **aucune date de calibration pH/ORP** côté firmware. La migration Atlas EZO (feature-021) a **supprimé** `phCalibrationDate` / `orpCalibrationDate` (cf. `src/config.h:60-62`) ; le module EZO stocke sa calibration en interne mais **n'expose pas de date**. Seule la **température** garde une date (`tempCalibrationDate`).
- **Conséquence** : afficher « calibration à refaire après le délai prévu » impose de **réintroduire un suivi de date de calibration** :
  1. Firmware : persister en NVS un **timestamp de calibration pH et ORP** au **succès** d'une commande `Cal` EZO (dans `_executeEzoCmd` / le hook de reset filtre feature-025). 1 horodatage par sonde.
  2. Firmware : exposer l'**âge** (ou la date) de calibration via WS (ex. `phCalAgeDays` / `orpCalAgeDays`) — réutilise l'horloge RTC/NTP existante.
  3. Constantes : **délai de recalibration recommandé** (ex. `kPhCalIntervalDays`, `kOrpCalIntervalDays`) — valeur à définir (proposition : pH ~30 j, ORP ~30 j — **à valider par pool-chemistry**, l'ORP dérive plus vite).
  4. Frontend : nouvel état du chip 🟠 « Calibration à refaire » (ou « Calibration ancienne — N j ») quand `âge > intervalle`, distinct de « Calibration recommandée » (1/2 points). Message clair dans le hint.
- **Décision requise utilisateur** avant implémentation du volet B : (1) confirmer qu'on réintroduit un horodatage de calibration côté firmware ; (2) fixer le **délai de recalibration** pH et ORP (jours). pool-chemistry validera les valeurs.
- **Agents volet B** : architect, pool-chemistry (intervalle), embedded-developer (NVS + WS), web-ui-developer, test-engineer, code-reviewer, doc-writer.

#### Critères d'acceptation itération 3
- [x] AC14 — Le bouton « Calibrer la sonde » (libellé fixe) est affiché sous la rangée des badges, pour pH et ORP, accessible dans tous les modes.
- [x] AC15 — Le garde « injection en cours » reste prioritaire sur ce bouton.
- ~~AC16 (volet B) — message « calibration à refaire » au-delà du délai~~ — **ABANDONNÉ** (pas d'horodatage de calibration firmware).
- ~~AC17 (volet B) — horodatage de calibration persisté + âge exposé WS~~ — **ABANDONNÉ**.
- [x] `./build_fs.sh` sous la limite (volet A, frontend uniquement).
- **Note** : seul le volet A est implémenté. Volet B abandonné par décision utilisateur.
