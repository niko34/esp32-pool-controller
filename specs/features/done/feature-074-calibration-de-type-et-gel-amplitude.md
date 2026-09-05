# feature-074 — Calibration de type par défaut + gel de l'amplitude à 50 % (préparation flotte)

## Métadonnées
- **ID** : feature-074
- **Slug** : calibration-de-type-et-gel-amplitude
- **Type** : feature (produit — préparation multi-appareils)
- **Statut** : doing — **livré (v2.23.0), tests verts (649/649), revue Approved, docs à jour** ; reste la vérification on-target post-OTA (NVS de l'appareil provisionné intact) avant clôture
- **Priorité** : normale
- **Version cible** : 2.23.0
- **Dernière mise à jour** : 2026-08-24
- **Spec(s) liée(s)** : bug-dosage-fantome (itération 2, ADR-0043), feature-073 (ADR-0046)

## Contexte produit (NOUVEAU — change le cadre des décisions passées)
L'appareil n'est plus destiné à un seul bassin : **l'utilisateur va en fournir à des tiers**, avec **le même modèle de pompe péristaltique** sur tous les exemplaires. Deux conséquences :
1. L'état « non caractérisé » fail-closed (calibration vide → dosage refusé jusqu'à pesée) est adapté à UN appareil en cours de mise au point, pas à une flotte : aucun utilisateur tiers ne pèsera jamais. Il faut une **calibration de type** (valeur du modèle de pompe, mesurée sur unité de référence) embarquée par défaut — le pattern des doseuses du commerce (débit nominal).
2. Le réglage d'amplitude (`pumpN_max_duty_pct`) est devenu un piège depuis le verrou de calibration (tout déplacement = dosage arrêté jusqu'à re-pesée) et a déjà été réécrit deux fois par des sauvegardes d'onglets périmés. Pour une flotte : **gel à 50 %**, retrait de l'UI, champ serveur conservé.

## Domaines touchés
- [ ] Capteurs
- [x] Régulation (défauts de calibration du dosage) → **passage obligatoire par `pool-chemistry`**
- [x] Web — routes HTTP (validation), Frontend (`data/`)
- [ ] MQTT / OTA / Historique

## Agents requis
- [x] spec-reader
- [x] architect
- [x] pool-chemistry — OBLIGATOIRE ET EN AMONT : son verdict feature-073/2.21.0 refusait toute « valeur provisoire » ; le contexte flotte + pompe standardisée + valeur de type mesurée est un dossier NOUVEAU qu'il doit ré-arbitrer
- [ ] ux-ui-designer (retrait d'un curseur + simplification d'un formulaire — pas de refonte ; à réévaluer si l'architect le juge utile)
- [x] embedded-developer
- [x] web-ui-developer
- [x] test-engineer
- [x] code-reviewer
- [x] doc-writer

## Description
1. **Calibration de type par défaut** : les défauts de config passent de « non caractérisé » (cal_duty=0, fail-closed) à une calibration de type complète : `pumpN_calibrated_duty = 127`, `pumpN_pulse_flow_ml_per_min = kPumpTypePulseFlowMlPerMin` (nouvelle constante, **28.0 ml/min** — voir justification). Un appareil neuf dose dès le premier flash, sur une valeur mesurée du MODÈLE de pompe.
2. **Distinction « valeur usine » / « pesée »** : nouveau drapeau persisté par pompe (`pumpN_flow_weighed`, défaut false). La pesée individuelle reste possible (raffinement) et pose le drapeau. L'UI affiche discrètement « Débit : X ml/min (valeur usine) » ou « (pesé) » — le badge d'alerte « non caractérisé » ne subsiste que pour l'état cal_duty=0 (effacement explicite / NVS hérité), qui reste fail-closed.
3. **Gel de l'amplitude** : curseur « puissance max » retiré des paramètres avancés, remplacé par une ligne lecture seule (« Amplitude d'impulsion : 50 % — débit X ml/min (usine|pesé) »). Champ serveur + validation [40..100] conservés (modifiable par API pour cas exceptionnel — précédent `screen_enabled` : champ vivant, surface UI retirée). Retirer le champ du save global du frontend (immunise contre les sauvegardes d'onglets périmés).
4. **Formulaire de pesée simplifié** : un seul champ « débit mesuré (ml/min) », pré-rempli avec la valeur courante, **vide refusé** (validation client + serveur existante) ; le duty calibré est implicite (= amplitude courante). Soumission → drapeau `weighed` posé.

## Justification de la valeur 28.0 (à ré-arbitrer par le gate)
Mesures unité de référence (2026-08-23) : pompe 1 = 12,0 ml/min @82 et 61,0 @255 → interpolation 24,7 @127 ; pompe 2 = 67,0 @255 → ~27,2 @127 (échelle 67/61). Modèle proportionnel pur (réfuté par le point à 82 mais borne haute plausible si la courbe est convexe) : 30,4-33,4. Dispersion d'unité constatée : ±10 % (61 vs 67 entre deux pompes identiques). **28.0** = au-dessus des deux interpolations (sens SÛR : surestimer le débit ⇒ sous-doser + sur-compter), sous la borne proportionnelle. Sera remplacée par la pesée de type à duty 127 quand l'utilisateur pourra la faire (l'appareil actuel garde ses valeurs NVS provisionnées 25,0/27,5 — les défauts ne s'appliquent qu'aux NVS vierges).

## Comportement attendu
- Flash sur appareil VIERGE : dosage fonctionnel immédiatement, calibration de type 127/28.0, drapeaux weighed=false, indication « valeur usine » dans les réglages.
- Appareil provisionné (le vôtre) : AUCUN changement de valeurs au flash (NVS prime) ; le drapeau weighed des pompes existantes : à trancher par l'architect (migration : considérer les valeurs NVS existantes comme « pesées » ou « usine » ? — recommandation : usine, honnête puisque 25/27,5 sont des interpolations).
- Curseur amplitude absent de l'UI ; ligne lecture seule à la place ; `pumpN_max_duty_pct` absent du payload de save global du frontend.
- Formulaire : champ unique débit, pré-rempli, vide/0 refusés, soumission pose weighed=true.
- L'effacement de calibration (API cal_duty=0) reste possible et fail-closed (badge + refus) — la machinerie 2.21.0 est conservée, seuls les DÉFAUTS changent.

### Cas limites
- NVS hérité avec cal_duty=0 (appareil flashé avant 2.23.0 jamais calibré) : reste fail-closed (pas de conversion silencieuse vers le défaut de type — l'utilisateur de CET appareil avait un état explicite).
- Amplitude NVS ≠ 50 % (héritée) : inchangée par le flash, mais plus modifiable par l'UI ; la ligne lecture seule affiche la valeur réelle.
- Sauvegarde d'un ancien onglet contenant pump1_max_duty_pct : le serveur l'accepte toujours (rétrocompat API) — le retrait du champ du frontend NEUF suffit à l'objectif ; documenter ce résidu.

## Contraintes techniques
- Aucune modification de la logique de dosage (pulse_dosing_logic, planPulse/WithCarry, gate exact-match, comptabilité) : SEULS les défauts, un drapeau, et la surface UI changent.
- `kPumpTypePulseFlowMlPerMin` dans constants.h, commentée avec les mesures de référence et le biais sûr.
- Drapeau NVS : clés courtes (≤15 car.), défaut false.

## Fichiers potentiellement concernés
- `src/constants.h`, `src/config.{h,cpp}` (défauts + drapeau + NVS), `src/web_routes_config.cpp` (weighed posé à la soumission, exposition get-config), `src/web_routes_data.cpp` (weighed exposé si l'UI en a besoin au /data — sinon get-config suffit)
- `data/index.html`, `data/app.js`, `data/app.css` (curseur retiré, ligne lecture seule, formulaire simplifié, indication usine/pesé, retrait du champ du save global)
- `src/version.h` (2.23.0), tests natifs si logique pure ajoutée (a priori non), docs (ADR successeur/amendement 0043, page-settings, pump-controller, API, CHANGELOG)

## Livrables — check-list
- [x] A1 — kPumpTypePulseFlowMlPerMin = 28.0 (constants.h, justifiée) ; défauts config cal_duty 0→127, pulse_flow 0→28.0 ; drapeau pumpN_flow_weighed (NVS, défaut false)
- [x] A2 — Migration : appareils provisionnés inchangés ; weighed=false pour l'existant ; cal_duty=0 hérité reste fail-closed
- [x] B1 — Curseur amplitude retiré, ligne lecture seule, champ hors du save global frontend (purgé des DEUX payloads : save global ET save pompes)
- [x] B2 — Formulaire pesée : champ unique débit pré-rempli, vide/0/NaN refusés (client+serveur), soumission pose weighed (duty déduit de l'amplitude courante côté serveur)
- [x] B3 — Indication « valeur usine » / « pesé » dans les réglages (ligne lecture seule `#pumpN_cal_info`, pas un badge d'alerte ; badge restreint à cal_duty=0)
- [x] C1 — Docs + ADR-0047 (le fail-closed de 2.21.0 amendé pour la flotte, note croisée dans ADR-0043) + CHANGELOG [2.23.0]
- [x] D1 — pio run 0 warning, natifs sans régression (649/649), build_fs OK

## Sécurité chimique (obligatoire)
- Dosage impliqué : oui (défauts de calibration du modèle volumétrique)
- Risque : valeur de type fausse pour une unité donnée (±10 % constaté) → sens d'erreur contrôlé par le biais haut (sous-dosage) ; limites journalières/horaires inchangées bornent l'exposition réelle
- Le gate tranche : la valeur 28.0, le biais, le sort du drapeau/badge, la migration

## ADR attendu ?
- **Probable** : oui — amende le « pesée obligatoire, pas de valeur provisoire » de 2.21.0 (critères 1 et 3 : contexte produit nouveau, alternatives crédibles : fail-closed flotte / type-default / type-default + limites réduites)

## Critères d'acceptation
- [x] Config par défaut (NVS vierge simulée en natif si testable, sinon lecture de code) : cal_duty=127, flow=28.0, weighed=false, dosage non bloqué
- [x] Appareil provisionné : valeurs NVS intactes après flash — VÉRIFIÉ on-target le 2026-08-24 post-OTA (calibrations 127/25,0 et 127/27,5, amplitudes 50 %, cible et limites conservées, weighed=false) — **seul critère restant : à vérifier après le déploiement OTA de la 2.23.0** (attendu : loadMqttConfig lit les clés existantes p1/p2_cal_duty et pulse_flow telles quelles, les défauts de type ne s'appliquent qu'aux clés absentes)
- [x] Curseur absent, ligne lecture seule présente, save global sans pumpN_max_duty_pct
- [x] Formulaire : vide refusé, soumission valide pose weighed et l'indication passe à « pesé »
- [x] `pio run` 0 warning, natifs sans régression (649/649), build_fs OK

## Notes d'implémentation (2026-08-24, v2.23.0)

**Verdicts** : gate `pool-chemistry` GO amont sous conditions C1-C5 + GO en contre-visite ; revue `code-reviewer` : **Approved** ; tests : **649/649** natifs, `pio run` 0 warning. Formalisé dans [ADR-0047](../../../docs/adr/0047-calibration-de-type-par-defaut-et-gel-amplitude.md) (amende le fail-closed des défauts de l'ADR-0043, note croisée posée).

**Conditions du gate (réalisation)** :
- **C1** — sémantique NVS : clé absente = vierge → défaut de type ; clé présente à 0 = fail-closed conservé, jamais de conversion silencieuse (`config.cpp`, `loadMqttConfig`).
- **C2** — `kPumpTypeCalDuty = (kPumpAmpDefaultPct × MAX_PWM_DUTY) / 100` = **même expression entière** que `getPumpAmplitudeDuty` : égalité exacte du gate d'impulsion garantie par construction ; `static_assert` (== 127, ≥ MIN_ACTIVE_DUTY).
- **C3** — `kPumpTypePulseFlowMlPerMin = 28.0`, AU-DESSUS des interpolations mesurées 23/08 (24,7 / ~27,2) : biais SÛR (sous-dosage + compteurs majorants). À remplacer par la pesée de type à 127.
- **C4** — la garde de plausibilité NVS rabat aussi `weighed = false`.
- **C5** — drapeaux `pumpN_flow_weighed` purement informatifs, aucun gate ne s'y fie.

**Décisions architect** : weighed=false pour l'existant (honnête : 25/27,5 sont des interpolations) ; amplitude gelée via retrait UI + purge du champ des DEUX payloads de save (immunité onglets périmés), champ API + validation [40..100] conservés (précédent `screen_enabled`) ; pesée sans duty → duty déduit de l'amplitude courante (∈ [102..255] par construction) + weighed=true ; effacement (duty=0) → flow=0 + weighed=false ; badge d'alerte restreint à cal_duty=0.

**Résidus statués (assumés, documentés dans l'ADR-0047 et API.md)** :
- (a) un duty explicite non nul envoyé par un vieux client pose aussi `weighed = true` — acceptable, drapeau informatif sans gate ;
- (b) un appareil 2.21/2.22 n'ayant JAMAIS sauvegardé sa config (clés absentes) recevrait la calibration de type — cas étroit, équivalent vierge ;
- (c) le serveur accepte toujours `pumpN_max_duty_pct` [40..100] d'un client périmé (rétrocompat API).

**Fichiers** : `src/constants.h`, `src/config.{h,cpp}`, `src/web_routes_config.cpp` (buffer get-config 2752→2816), `src/version.h` (2.23.0), `data/index.html`, `data/app.js`, `data/app.css`. Docs : ADR-0047, ADR-0043 (note), `docs/API.md`, `docs/features/page-settings.md`, `docs/subsystems/pump-controller.md`, `CHANGELOG.md`, index ADR.

**Correctif post-livraison (on-target, 2026-08-24, même version 2.23.0)** — bug trouvé par l'utilisateur à la vérification sur cible : la trame `config` WebSocket ne portait **pas** les champs de calibration (ils n'existaient que dans `/get-config`) — au premier push (≤ 5 s), `updatePumpCalibrationDisplay` traitait les clés absentes comme « zéro » et **écrasait le pré-remplissage** avec « débit non caractérisé ». Correctif **double** : (1) `src/ws_manager.cpp` `_buildConfigJson` — parité STRICTE avec `/get-config`, +6 champs `pump1/2_calibrated_duty`, `pump1/2_pulse_flow_ml_per_min`, `pump1/2_flow_weighed` (buffer 2368 → 2560 o, trame ≈ 2,56 Ko / 3584) ; (2) `data/app.js` — clé absente = **pas d'information**, early-return (même leçon que la parité `/data` des pentes en 2.21.2 : le client reste inerte face à un payload d'un firmware plus ancien). Docs amendées : `docs/subsystems/ws-manager.md` (section contractuelle trame `config`), `docs/API.md` (note de parité), `CHANGELOG.md` (ligne ajoutée à [2.23.0]).

## Hors périmètre
- Toute modification de la logique de dosage/accumulateur
- La valeur définitive de type (pesée à 127 par l'utilisateur, ajustera kPumpTypePulseFlowMlPerMin plus tard)
- Provisioning multi-appareils au-delà des défauts (pas de gestion de flotte)
