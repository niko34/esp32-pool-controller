# feature-075 — Assistant de mesure du débit des pompes (étape par étape)

## Métadonnées
- **ID** : feature-075
- **Slug** : assistant-mesure-debit-pompe
- **Type** : feature (UX produit — préparation flotte)
- **Statut** : **done** — livré (tests verts, revue Approved), **vérifications on-target faites** (28/08) et **P12 clos le 06/09/2026** (concordance bidon/compteur sur baseline propre + limites d'exploitation restaurées)
- **Priorité** : normale
- **Version cible** : 2.24.0 (livrée)
- **Dernière mise à jour** : 2026-09-06
- **Spec(s) liée(s)** : feature-074 (calibration de type, formulaire actuel), ADR-0043/0047, bug-dosage-fantome it.2 (outil ?duty=), **feature-078** (nature du biais de comptage — résidu de P12), **feature-110** (compteur d'impulsions — préalable qui rendra la prochaine concordance auto-suffisante)

## Domaines touchés
- [ ] Capteurs
- [x] Régulation — **l'assistant ACTIONNE les pompes doseuses** (purge + mesure minutée) → **passage obligatoire par `pool-chemistry`**
- [x] Web — routes HTTP (minuterie serveur), Frontend (`data/`)
- [ ] MQTT / OTA / Historique

## Agents requis
- [x] spec-reader
- [x] architect
- [x] pool-chemistry — OBLIGATOIRE ET EN AMONT (nouveau parcours utilisateur qui actionne les pompes ; questions de gardes ci-dessous)
- [x] ux-ui-designer — OBLIGATOIRE (nouvelle surface UI, flux multi-étapes plein écran)
- [x] embedded-developer (selon choix de minuterie serveur)
- [x] web-ui-developer
- [x] test-engineer
- [x] code-reviewer
- [x] doc-writer

## Description
Remplacer le formulaire nu de « pesée » par un **assistant pas à pas** intégré à la page des réglages (le reste est masqué pendant l'assistant, comme l'écran de calibration du pH) qui guide l'utilisateur dans la mesure du débit réel de chaque pompe : purge des tuyaux à l'eau claire, débranchement du tuyau d'injection dans un récipient gradué, marche minutée de la pompe à l'amplitude d'exploitation, saisie du volume lu, conversion et enregistrement. **Terminologie : le mot « pesée » disparaît de l'UI** au profit de « mesure du débit » ; l'indication « pesé » devient « mesuré ».

## Contexte
L'appareil est destiné à une flotte (feature-074). La procédure de mesure vit aujourd'hui dans une spec et des conversations — un tiers ne la trouvera jamais, et le formulaire actuel (« Débit mesuré (ml/min) » + bouton) suppose que l'utilisateur connaît toute la procédure : eau claire, purge, débranchement, chronométrage, conversion. Le pattern d'assistant existe déjà dans l'app (calibration pH). Décisions produit actées par l'utilisateur : assistant intégré à la page (masquage du reste), étapes purge → débrancher → marche minutée → saisie, terme « pesée » banni.

## Comportement attendu

Un bouton « Mesurer le débit » par pompe (remplace « Enregistrer la pesée » comme action principale). Il ouvre l'assistant plein écran (le reste des réglages est masqué). Étapes :

1. **Préparation** : matériel requis (seau d'eau claire, récipient gradué), avertissement « jamais d'acide/chlore pendant la mesure — la pompe est volumétrique, l'eau vaut le produit ». Le dosage automatique est suspendu à l'entrée de l'assistant et rétabli à la sortie (y compris sortie par fermeture de page — c'est la minuterie serveur qui garantit l'arrêt pompe, et la suspension doit avoir un timeout serveur).
2. **Purge** : « Placez le tuyau d'aspiration dans le seau d'eau claire », bouton « Purger » → la pompe tourne ~20-30 s à l'amplitude d'exploitation (durée à fixer par le gate), compte à rebours. Le produit résiduel part par le circuit d'injection normal.
3. **Débranchement** : « Débranchez le tuyau d'injection et placez-le dans le récipient gradué ». (Question gate : statut de la garde de présence d'eau pendant cette phase.)
4. **Mesure** : bouton « Lancer la mesure » → la pompe tourne à l'amplitude EXACTE d'exploitation pendant une durée fixe (60 s proposé par l'utilisateur ; 120 s recommandé pour la précision — ~50 ml lus au lieu de ~25, erreur de lecture halvée ; ux-ui-designer + gate tranchent), compte à rebours affiché, bouton « Arrêter » d'urgence toujours visible.
5. **Saisie** : « Quel volume lisez-vous ? (ml) » → l'assistant convertit en ml/min, affiche la valeur pour confirmation, envoie (chemin /save-config existant de la 074 : le serveur déduit le duty, pose le drapeau). L'indication passe à « mesuré ».
6. **Remontage** : « Rebranchez le tuyau d'injection ». Note : les premières doses injecteront l'eau restée dans le tuyau (négligeable).

### Exigences de sécurité (non négociables)
- **Minuterie CÔTÉ SERVEUR** pour la purge ET la mesure : si l'onglet se ferme, si le WiFi tombe, la pompe s'arrête à l'échéance. Jamais de marche pompe dont l'arrêt dépend du navigateur. (Le firmware a déjà l'injection manuelle minutée — l'architect choisit entre la réutiliser et ajouter une durée au test manuel `?duty=`.)
- **Suspension du dosage automatique** pendant l'assistant, avec timeout serveur (un assistant abandonné ne doit pas laisser la régulation suspendue indéfiniment) et rétablissement explicite à la sortie.
- Toutes les gardes existantes du chemin d'actionnement restent (auth WRITE, inhibition maintenance…) ; le gate statue sur la présence d'eau en mode tuyau débranché.
- Une seule pompe en assistant à la fois.

### Cas limites
- Fermeture de l'onglet en pleine marche → la minuterie serveur arrête ; à la reconnexion, l'assistant ne reprend pas (état perdu côté client, assumé), la suspension expire par timeout.
- Saisie improbable (ex. > 200 ml/min après conversion, ou < 2 ml/min) → refus avec message (validation serveur existante ]0..200], le client peut resserrer avec un avertissement de vraisemblance : « attendu ~20-35 ml/min »).
- WiFi coupé pendant le compte à rebours → l'UI le signale, la pompe s'arrête seule à l'échéance.
- Le champ de saisie directe actuel reste accessible (« saisie avancée », discret) pour l'utilisateur expert.

## Comportement actuel
Formulaire nu par pompe (feature-074) : un champ « Débit mesuré (ml/min) » pré-rempli + bouton « Enregistrer la pesée ». Aucun guidage. Le test manuel `?duty=` existe (API) mais sans minuterie exposée à l'UI. L'injection manuelle minutée existe (volumétrique, arrêt serveur).

## Contraintes techniques
- Vanilla JS, pattern de l'écran de calibration pH réutilisé (masquage, étapes), français.
- Aucun nouveau rythme réseau ; compte à rebours client resynchronisé (pattern 2.21.1) si l'état de marche est exposé, sinon minuterie affichée localement + vérification d'état à l'échéance.
- Budget FS ; pas de framework.

## Fichiers potentiellement concernés
- `data/index.html`, `data/app.js`, `data/app.css` — l'assistant
- `src/web_routes_control.cpp` — marche minutée serveur (durée sur le test manuel ou réutilisation de l'injection minutée) + suspension/rétablissement du dosage auto avec timeout
- `src/pump_controller.{h,cpp}` — si la suspension « mode assistant » nécessite un état dédié (à minimiser)
- `src/constants.h` — durées (purge, mesure, timeout suspension) en k*
- `src/version.h` (2.24.0), docs (page-settings, API si nouvelle route/param, pump-controller si état), CHANGELOG

## Livrables — check-list
- [x] A1 — Marche pompe minutée côté serveur (purge + mesure), durées en constantes, arrêt garanti sans client — `/flow-assist/start`, duty/durée 100 % serveur (purge 255/60 s max 3 ; mesure amplitude/120 s), `kFlowAssist*`
- [x] A2 — Suspension du dosage auto pendant l'assistant, timeout serveur, rétablissement à la sortie — `setFlowAssistSuspend` (deux chimies), timeout 15 min réarmé à chaque requête, réconciliation fail-closed, invariant exit session-avant-levée
- [x] B1 — Assistant plein écran par pompe, 6 étapes, pattern calibration pH, compte à rebours, bouton Arrêter — carte `#pump-flow-wizard`, classe `is-flow-wizard`, Arrêter jamais désactivé
- [x] B2 — Conversion volume lu → ml/min + confirmation + envoi par le chemin 074 (duty déduit, drapeau posé) — `saveMeasuredFlow()` → `/save-config` `pumpN_pulse_flow_ml_per_min`
- [x] B3 — Terminologie : « pesée » éliminée de toute l'UI (« mesure du débit », « mesuré ») ; saisie directe conservée en avancé — grep `data/` vide, dépliant « Saisie avancée »
- [x] B4 — Garde-fous de vraisemblance à la saisie (avertissement hors ~20-35 ml/min, refus hors bornes serveur) — `#flow-wizard-plausibility` non bloquant, refus client ]0..200], serveur autorité
- [x] C1 — Docs (page-settings avec le déroulé, API si contrat modifié, CHANGELOG) ; ADR si l'architect identifie un arbitrage structurant — page-settings + API + pump-controller + logger + CHANGELOG [2.24.0] ; **aucun ADR** (verdict architect confirmé : assemblage d'existants, véhicule imposé par le gate)
- [x] D1 — pio run 0 warning, natifs sans régression, build_fs OK — **674/674** (base 649 + 25 `test_native_flow_assist`)

## Sécurité chimique (obligatoire)
- Dosage impliqué : OUI — l'assistant actionne les pompes (purge + mesure) et suspend la régulation
- Questions au gate : durée/duty de purge ; durée de mesure (60 vs 120 s) ; garde de présence d'eau pendant les phases tuyau débranché ; volumes de purge partant au bassin (acide résiduel du tuyau — quantifier, borner) ; timeout de suspension ; interaction avec les limites journalières (la marche assistant compte-t-elle dans usedMs/daily ? recommandation : compter dans usedMs, créditer le volume au majorant comme le test manuel — cohérent P4)
- Risque principal : une marche pompe non arrêtée → minuterie serveur exigée ; second risque : suspension de régulation oubliée → timeout exigé

## ADR attendu ?
- **Probable** : non a priori (assemblage de mécanismes existants) — l'architect confirme

## Critères d'acceptation
- [x] **P12 — concordance bidon/compteur** *(transféré depuis
  `bug-dosage-fantome-duty-non-calibre.md` le 2026-08-28, avec l'accord de
  l'utilisateur)*. Comparer le compteur au niveau réel des bidons gradués, relevé
  daté ; puis **restaurer les limites d'exploitation** abaissées pendant la
  validation (P3).
  **Motif du transfert** : ce critère ne valide pas la correction du dosage
  fantôme mais la **justesse des débits**, aujourd'hui **extrapolés** (25,0 et
  27,5 mL/min à l'amplitude d'exploitation, valeurs d'usine provisoires de la
  v2.23.0). Or c'est cette spec qui porte leur remplacement par une mesure
  réelle. Faire la concordance avant la mesure reviendrait à valider un compteur
  contre un débit supposé : l'ordre naturel est **mesurer d'abord** (assistant),
  **concorder ensuite**.

  **MESURÉ le 2026-09-06 — première concordance sur baseline propre** (posée le
  31/08 : bidon chlore mesuré 8 750 mL, compteur remis à 0).

  | Grandeur | Valeur |
  |---|---:|
  | Compteur appareil (`orp_total_injected_ml`) | **5 290,44 mL** |
  | Restant **estimé** (8 750 − 5 290,44) | 3 459,56 mL |
  | Restant **mesuré** | **3 700 mL** |
  | Injecté **réel** (8 750 − 3 700) | **5 050 mL** |
  | **Biais** | **+240,44 mL = +4,76 %** — l'appareil **sur-compte** |

  **Méthode** : récipient doseur de 3 L rempli **à ras**, puis complément de
  0,7 L. Incertitude ~±30-50 mL ⇒ biais réel entre **+3,8 % et +5,8 %** :
  **distinguable de zéro**. **Fenêtre** : 6 jours, 5 050 mL réellement injectés,
  amplitude `duty = 127`.

  **Le critère demandait « 3 jours » — il est dépassé par l'esprit** : 6 jours,
  5 L injectés, baseline contrôlée et compteur remis à zéro au même instant que
  la mesure physique. C'est un **meilleur signal** que trois relevés quotidiens,
  chacun noyé dans l'incertitude de lecture d'un bidon à peine entamé. Critère
  **satisfait**.

  **Second volet — restauration des limites d'exploitation : satisfait.**
  `ph_limit_minutes` et `orp_limit_minutes` sont à **10 min/h** (défauts d'usine),
  restaurés le 31/08 par l'utilisateur (cf. mémoire `parametres-bassin-utilisateur`).

  #### Verdict `pool-chemistry` du 2026-09-06

  **Ni la branche (a) ni la branche (b)** annoncées en itération 2bis. Le point
  est **concluant sur l'EXISTENCE** du biais, **non concluant sur sa NATURE**.
  **NO GO ferme sur tout facteur de comptabilité de flotte.**

  1. **Le biais était déjà documenté.** [`src/constants.h:2083-2089`](../../../src/constants.h)
     annonce que « le compteur reste FAUX », décrit une **perte fixe par
     impulsion**, déclare la mesure due et renvoie à `feature-078`. Cette mesure
     ne découvre rien : elle **chiffre pour la première fois une dette connue**,
     et **dans le sens annoncé**.
  2. **Le sens observé est l'INVERSE de celui que postulait la branche (b)**,
     laquelle supposait des kick/rampes délivrant *plus* que `débit × temps`.
  3. **Quantification** ([`src/pulse_dosing_logic.cpp:136-163`](../../../src/pulse_dosing_logic.cpp)) :
     un pouls minimal est compté **4,573 mL** (kick 0,493 + plateau 4,08) ⇒
     ~**1 160 impulsions** sur la fenêtre ⇒ **0,21 mL de surcoût par impulsion**,
     soit ~**0,5 s de débit perdu au démarrage** — l'ordre de grandeur exact d'un
     transitoire péristaltique.
  4. **~19 % de ce surcoût est VOLONTAIRE** : le kick est crédité à
     `kPumpFlow255MlPerMin = 74,0` contre **68,0 mesurés**
     ([`constants.h:2114-2136`](../../../src/constants.h)). C'est une marge de
     sécurité de flotte, **à ne jamais « corriger »** en croyant gagner en
     précision.
  5. **Pourquoi pas de facteur de flotte** : un facteur **multiplicatif** calé sur
     un point unique corrigerait **mal** une perte **par impulsion** — la fraction
     de biais varie avec la durée moyenne de pouls, donc avec la demande. Sur
     pouls longs il **sur-corrigerait et relâcherait les quotas**.
  6. **Sens conservateur, aucun effet de sécurité chimique** : sur-compter
     **resserre** les quotas journaliers et **avance** l'alerte de bidon vide.
     Rien à corriger en urgence.
  7. **Réserve** : `kPulseMinMs` porté de 5 à 10 s ([ADR-0051](../../../docs/adr/0051-pouls-minimal-double-rendement-avant-comptage.md))
     a **divisé par deux** le nombre d'impulsions — donc **réduit** ce coussin
     involontaire d'autant.
  8. **Incertitude à signaler** : une purge expulse du liquide **sans le
     compter**, elle **masquerait** donc une partie du sur-comptage.
     `measures_purge_count` valait 0 au relevé, mais l'appareil avait redémarré
     20 min plus tôt : ce compteur **ne couvre pas la fenêtre**.
     ⇒ **+4,76 % est un PLANCHER, pas une valeur.**

  **Résidu routé** : la **nature** du biais et son éventuel correctif appartiennent
  à [feature-078](../todo/feature-078-rendement-des-impulsions-et-lenteur-regulation-ph.md),
  dont le **préalable** est [feature-110](../todo/feature-110-compteur-d-impulsions-de-dosage.md)
  (compteur d'impulsions : deux inconnues, deux équations). Sans ce compteur,
  **chaque concordance future sera aussi peu concluante que celle-ci**.
- [x] Fermer l'onglet pendant purge ou mesure → la pompe s'arrête à l'échéance
  **VÉRIFIÉ on-target le 2026-08-28**, par une variante assumée (voir nuance ci-dessous).
  Trace enregistrée depuis un poste tiers, échantillonnage 1,5 s de `/data` :
  ```
  11:43:50  active=True  phase=purge  restant=59 s  aborted=False  purges_left=2
            ← l'utilisateur QUITTE l'écran de l'assistant (clic sur un autre menu)
  11:44:48  active=True  phase=purge  restant= 1 s  aborted=False
  11:44:50  active=True  phase=""     restant= 0 s  aborted=False   ← arrêt SEUL
  ```
  **60 s exactement** entre démarrage et arrêt : la purge est allée à son terme
  sans interruption ni prolongation, écran quitté. `aborted` est resté `false`
  (correct : ce drapeau signale un abandon décidé par l'appareil, typiquement une
  perte de filtration — pas une action de navigation). Une purge consommée
  (`purges_left` 3 → 2). Session testée sur la **pompe 2**.

  **Nuance honnête sur la portée** : le geste réalisé est une sortie d'écran dans
  la SPA, pas une fermeture d'onglet. La page restant chargée, le navigateur
  continue de dialoguer avec l'appareil — la variante stricte (fenêtre fermée,
  tout dialogue coupé) n'a pas été exécutée. Ce qui est néanmoins **démontré** :
  la commande d'arrêt ne vient pas de l'écran de l'assistant, sans quoi le quitter
  l'aurait perdue. La minuterie est bien côté serveur.

  *Constat annexe relevé pendant ce test, non traité* : `_flowAssistSuspend`
  (`pump_controller.cpp:1827`) journalise la suspension du dosage automatique
  mais **ne pose aucune cause de refus exposée** — pendant une session,
  `ph|orpDoseBlockedCode` reste nul alors que le dosage est empêché. Angle mort
  d'observabilité de même nature que ceux corrigés en v2.25.1, sur un chemin que
  ce cycle n'a pas couvert. Correctif attendu : une valeur d'énumération
  supplémentaire + son libellé, à faire lors d'une prochaine intervention sur ce
  code. Impact modéré (l'UI dispose de `flow_assist.active`), sauf session
  laissée ouverte par oubli.
- [x] L'assistant complet aboutit à une valeur enregistrée avec drapeau « mesuré » et duty déduit correct
  **VÉRIFIÉ on-target le 2026-08-28** — parcours de bout en bout exécuté par
  l'utilisateur sur les **deux** pompes, sans incident.

  | Pompe | Mesuré à duty 127 | Estimation antérieure | Écart | `flow_weighed` | `calibrated_duty` |
  |---|---|---|---|---|---|
  | 1 (pH) | **26,0** mL/min | 25,0 extrapolée | +4 % | `true` | 127 |
  | 2 (ORP) | **25,5** mL/min | 27,5 extrapolée | −7 % | `true` | 127 |

  Le duty déduit est correct (127 = amplitude d'exploitation 50 %) et les deux
  drapeaux sont passés à « mesuré ». L'assistant remplit donc son office : il
  transforme une procédure d'expert en manipulation exécutable sans nous — ce qui
  était son objet pour la flotte.

  **Deux enseignements dépassant le critère :**
  1. **L'extrapolation d'usine était bonne** (écarts de 4 et 7 %), très loin des
     facteurs 1,9 / 1,5 dont le modèle linéaire duty↔débit était coupable. Le
     modèle par calibration ponctuelle tient.
  2. **La cause n°3 de la lenteur pH est ÉCARTÉE** (cf. `feature-078`) : le débit
     réel (26,0) est *supérieur* à celui utilisé pour le calcul (25,0), donc le
     firmware sous-comptait — il injectait un peu plus qu'il ne croyait. La
     lenteur ne vient pas de là.
  3. Les deux pompes diffèrent de **10 %** à duty 255 (61 / 67) mais de **2 %**
     seulement à duty 127 (26,0 / 25,5) : l'écart de tube ne se transpose pas
     linéairement d'un point de fonctionnement à l'autre.
- [x] Le mot « pesée » n'apparaît plus dans l'UI (grep data/) — grep vide vérifié
- [x] Dosage auto suspendu pendant l'assistant, rétabli après (et par timeout si abandon) — garde structurelle + timeout 15 min + réconciliation fail-closed (tests natifs)
- [x] `pio run` 0 warning, natifs sans régression, build_fs OK — 674/674 (+25 flow_assist)

## Hors périmètre
- Modification du modèle de dosage, des limites, de la calibration de type (074)
- Assistant pour autre chose que le débit des pompes
- Reprise d'un assistant interrompu (état non persisté, assumé)

## Notes d'implémentation (2026-08-24, v2.24.0)

### Verdicts

- **Gate `pool-chemistry`** : GO amont **sous conditions** + **GO en contre-visite** (condition résiduelle fermée : les injections manuelles classiques `/ph|orp/inject/start` sont **refusées `409 assist_active`** pendant une session — sous suspension, l'intégration par tick est sautée et une injection classique interrompue serait créditée 0, violation P4). Arbitrages du gate : purge à duty **255 impérativement** (seul débit connu → volume résiduel au bassin bornable), mesure **120 s** (erreur de lecture halvée vs 60 s), marches comptées dans usedMs/cycles et **créditées au majorant** `flow255 × durée`, timeout suspension 15 min, gardes manualInject **complètes** (présence d'eau incluse — pas d'exemption tuyau débranché : l'aspiration est dans le seau, l'injection éventuelle reste gardée).
- **Revue de code** : **Approved après 2 rebouclages**. Anomalies trouvées et corrigées :
  1. **P4 abandon-filtration** (test-engineer) : une marche d'assistant interrompue par la perte de filtration passait par le chemin d'abandon historique « pas de crédit » alors que la comptabilité par tick est sautée sous suspension → volume réel jamais inscrit. Correctif : `flowAssistCreditAbortedRun()` — crédit majorant prorata en loopTask, comportement hors assistant strictement inchangé.
  2. **Drapeau `aborted`** : sans signalement serveur, l'échéance du compte à rebours client routait une mesure PARTIELLE vers la saisie du volume → débit sous-estimé enregistré → sur-injection systématique. Correctif : `flow_assist.aborted` dans `/data`, consommé par `verifyRunEnd()` + filet à l'enregistrement (`saveMeasuredFlow`), réarmé au start suivant.
  3. **Course exit/réconciliation** : la réconciliation fail-closed du tick (session active sans suspension → re-pose) pouvait s'intercaler entre la levée de suspension et le reset de session dans `/exit` → suspension collée définitivement. Correctif : invariant **session fermée AVANT la levée** (+ réconciliation idempotente pour la course timeout/start symétrique).
- **Tests** : **674/674 natifs** (+25 `test/test_native_flow_assist/` sur `src/flow_assist_logic.h` — expiration wrap-safe, quota de purges, refus PumpBusy/SessionInactive), `pio run` 0 warning, `build_fs` OK.

### Architecture livrée

- **Serveur** ([`src/web_routes_control.cpp`](../../../src/web_routes_control.cpp)) : session `sFlowAssist` (une pompe à la fois), 3 routes `POST /flow-assist/start|stop|exit` (WRITE, params `pump`+`phase` SEULS — duty/durée 100 % serveur), 409 `assist_busy`/`purge_limit`/`already_injecting`/`invalid_amplitude` + codes des gardes manuelles. **Véhicule d'exécution : le chemin `manualInject*` existant** (minuterie serveur + crédit plancher) — aucune nouvelle mécanique de pompe. Timeout + réconciliation + libération `runOwned` dans `updateManualInject()` (tick loopTask). `/data` : objet `flow_assist` {active, pump, phase, remaining_s, purges_left, purge_s, measure_s, aborted, flow255[2]} (buffer 1920 → 2304 o).
- **PumpController** : `setFlowAssistSuspend()`/`isFlowAssistSuspended()` (`volatile`, patron `maintenanceInhibit`) — fermeture du dosage auto+scheduled (deux chimies) par la **garde structurelle existante** (chemin perte-filtration), mode manuel préservé, log one-shot par transition.
- **Constantes** : `kFlowAssistPurgeDurationS` = 60 s, `kFlowAssistMeasureDurationS` = 120 s, `kFlowAssistMaxPurgesPerSession` = 3, `kFlowAssistSuspendTimeoutMs` = 900 000 ms.
- **UI** ([`data/index.html`](../../../data/index.html), [`data/app.js`](../../../data/app.js) `bindFlowWizard()`) : carte unique `#pump-flow-wizard`, 6 étapes, compte à rebours local resynchronisé à l'échéance (1 GET /data — aucun nouveau rythme), callouts hors-ligne/expiration, budget annoncé sur `flow255` servi, `beforeunload` pendant marche seulement, dépliant « Saisie avancée » (formulaire 074).
- **ADR : aucun** — verdict architect confirmé par doc-writer : véhicule d'exécution imposé par le gate, assemblage de mécanismes existants (manualInject, garde structurelle, /save-config 074), aucun des 4 critères de déclenchement rempli.

### Docs mises à jour

`docs/API.md` (3 routes, objet `flow_assist`, 409 `assist_active`), `docs/features/page-settings.md` (section assistant + amendement formulaire 074), `docs/subsystems/pump-controller.md` (section suspension), `docs/subsystems/logger.md` (émetteurs événementiels), `CHANGELOG.md` [2.24.0].

### Suivi / itérations

| Itération | Périmètre | Statut |
|---|---|---|
| 1 | Assistant complet (routes + suspension + UI 6 étapes + terminologie) | **livré — tests verts (674/674), revue OK (Approved après 2 rebouclages)**. Fichiers : `src/web_routes_control.{h,cpp}`, `src/web_routes_data.cpp`, `src/pump_controller.{h,cpp}`, `src/constants.h`, `src/flow_assist_logic.h` (nouveau), `src/version.h`, `data/index.html`, `data/app.js`, `data/app.css`, `test/test_native_flow_assist/` (nouveau), docs (API, page-settings, pump-controller, logger, CHANGELOG) |
| 2 | Vérifications on-target post-OTA : fermeture d'onglet en pleine marche, parcours complet avec enregistrement « mesuré » | **vérifié on-target le 2026-08-28** (traces et tableau de mesures dans les critères d'acceptation ci-dessus) ; reste **P12** (3 jours de concordance bidon/compteur), qui ne peut se faire qu'après la mesure |
| 2bis (P12) | **Premier point de concordance bidon/compteur — chlore, 31/08/2026.** Mesure physique du bidon : **8 750 mL** (saisi tel quel, l'affichage 8,8 L est un arrondi). État du compteur au même instant : `orp_total_injected_ml = 10 101,91` sur un bidon déclaré 20 000 ⇒ restant ESTIMÉ 9 898 mL vs 8 750 MESURÉ = **écart −1 148 mL (~11,4 % du volume compté), l'appareil SOUS-compte la consommation** (ou le remplissage initial était < 20,0 L — les bidons nominaux 20 L tolèrent ±3-5 %, ce qui absorbe une partie de l'écart ; purges et appoints manuels non comptés aussi). Si l'écart était 100 % débit : débit réel pompe 2 ≈ 25,5 × 1,114 ≈ **28,4 mL/min** vs 25,5 pesé. NON CONCLUANT seul — la baseline initiale n'était pas contrôlée. **Baseline PROPRE posée ce jour** : bidon 8 750 mesuré + compteur remis à 0 ⇒ la prochaine mesure physique donnera une concordance exploitable sans hypothèse. **OBJECTIF CADRÉ PAR L'UTILISATEUR (31/08)** : il ne s'agit PAS de mesurer les pompes de CET appareil (les appareils futurs auront d'autres pompes, jamais pesées une à une) mais de valider la CHAÎNE D'ESTIMATION de flotte. Le point propre à J+2 départage : (a) écart ≈ 0 ⇒ la comptabilité (débit pesé × temps de marche, phases de pouls comprises) est juste, l'erreur de flotte se réduit à la dispersion pompe-à-pompe autour de la calibration de type 28,0 — rien à changer ; (b) écart ≈ +10 % confirmé SUR POMPE PESÉE ⇒ biais dans la chaîne de comptage elle-même (kick/rampes qui délivrent plus que débit×temps, ou protocole de pesée) ⇒ correctif de flotte (facteur de comptabilité unique) à instruire avec pool-chemistry — ATTENTION au sens : sous-compter la consommation relâche de fait les quotas journaliers (~+11 %) et retarde l'alerte bidon vide. Mesure acide : possible mais délicate à manipuler, à la discrétion de l'utilisateur — non requise si le point chlore est net. | **clos le 06/09/2026 — ni (a) ni (b)** : mesure faite (compteur 5 290,44 mL, restant mesuré 3 700, injecté réel 5 050 ⇒ **+240,44 mL = +4,76 %, l'appareil SUR-compte**, plancher). Verdict `pool-chemistry` : **concluant sur l'existence** du biais, **non concluant sur sa nature**, **NO GO ferme sur tout facteur de comptabilité de flotte**. Détail complet dans le critère P12 ci-dessus. Résidu routé vers `feature-078` (nature + correctif) avec `feature-110` en préalable. Mesure acide **non faite et non requise** — le point chlore est net. |
| 3 (hors périmètre, conséquence) | Report des mesures dans les **valeurs d'usine** — v2.25.2, [ADR-0050](../../../docs/adr/0050-debit-pleine-puissance-constante-unique-de-flotte.md) : débit à 255 = constante **unique de flotte** `kPumpFlow255MlPerMin = 74,0`, justification de `kPumpTypePulseFlowMlPerMin = 28,0` refondée sur la mesure, miroir client supprimé | **livré** — traité comme un cycle distinct (cette spec exclut explicitement la modification de la calibration de type) |

## Notes de clôture (2026-09-06)

**Ce que cette spec a livré** : l'assistant de mesure du débit, vérifié on-target
sur les deux pompes le 28/08, et — par le critère P12 qui lui avait été transféré —
**le premier chiffrage du biais du compteur de volume** sur une baseline contrôlée.

**Ce qu'elle ne livre pas, et qui n'est pas de son ressort** : la **nature** de ce
biais (multiplicatif ou perte fixe par impulsion) et son éventuel correctif. Le
volume seul ne peut pas trancher — une inconnue, une équation. C'est un résultat
en soi, et il est routé :

| Suite | Objet | Pourquoi elle passe avant / après |
|---|---|---|
| [feature-110](../todo/feature-110-compteur-d-impulsions-de-dosage.md) | Compteur d'impulsions par pompe, persistant, remis à zéro **avec** le volume | **Préalable.** Deux inconnues, deux équations : sans lui, **chaque concordance future sera aussi peu concluante que celle du 06/09**. Il se relève sans effort, au geste déjà fait du changement de bidon. |
| [feature-078](../todo/feature-078-rendement-des-impulsions-et-lenteur-regulation-ph.md) | Nature du biais (M2) puis correction du modèle de comptage | **Après.** Toute correction du comptage est une **hausse de dose réelle** : elle exige la nature établie et un visa `pool-chemistry`. |

**Aucune modification de code n'a été faite par cette clôture** — le sens du biais
est **conservateur** (quotas resserrés, alerte de bidon vide en avance) : rien
n'appelait d'action corrective immédiate.

**Documentation touchée** : [`docs/subsystems/pump-controller.md`](../../../docs/subsystems/pump-controller.md)
(section « Biais du compteur de volume — mesuré »), mémoire projet
`.claude/memory/parametres-bassin-utilisateur.md` (relevé du 06/09).
**`CHANGELOG.md` : aucune entrée** — rien n'est livré, aucun comportement ne change.
