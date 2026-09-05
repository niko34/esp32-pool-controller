## Métadonnées
- **ID** : feature-093
- **Slug** : regulation-transparente-au-reboot
- **Type** : feature
- **Statut** : done (implémentée, tests verts, revue Approved 04/09/2026 — déploiement 2.34.0 en attente du go utilisateur)
- **Priorité** : haute (le bug-080 n'est pas résolu : les redémarrages vont continuer, et chacun coûte ~1 h de régulation)
- **Version cible** : 2.34.0 (cycle commun avec feature-091, plan §5 point 3 ; fenêtre d'observation 84/85/86 close le 04/09 sur décision utilisateur)
- **Dernière mise à jour** : 2026-09-04
- **Spec(s) liée(s)** : feature-084 (persistance conditionnelle de l'intégrale, ADR-0053 — question R3 laissée ouverte à la clôture), feature-091 (purge adaptative — porte le volet « purge » de ce problème), bug-080 (source des redémarrages)
- **Origine** : PANIC du 03/09/2026 08:59:47 et journal de reprise (`builds/2.33.0/esp32dev_core3/system_log_20260903_1040.txt`) ; exigence utilisateur : « un reboot ne devrait pas perturber le fonctionnement et ralentir l'atteinte de la cible »

## Domaines touchés
- [ ] Capteurs (pH / ORP / température)
- [x] Régulation (pump_controller, PID) → **passage obligatoire par `pool-chemistry`**
- [ ] Filtration / Éclairage
- [ ] Web — routes HTTP / WebSocket
- [ ] MQTT / Home Assistant
- [ ] UART / écran LVGL
- [ ] OTA
- [ ] Authentification
- [x] Historique / persistance (instantané NVS `pool-phint`)
- [ ] Frontend (`data/`)

## Agents requis
- [x] spec-reader
- [x] architect
- [x] pool-chemistry — la restauration d'une intégrale est une dose différée : toute extension de sa fenêtre de validité est une décision chimique
- [ ] ux-ui-designer
- [x] embedded-developer
- [ ] web-ui-developer
- [x] test-engineer — logique pure `ph_persist_logic.h` déjà testée en natif (`test_native_ph_persist`) : étendre, valider par mutation
- [x] code-reviewer
- [x] doc-writer

## Description
Un redémarrage du firmware pendant que la filtration tourne ne doit pas faire perdre à la régulation pH ce qu'elle savait une seconde avant : l'intégrale doit être retrouvée à la reprise, et la période sans dosage doit se limiter au temps réellement nécessaire pour disposer d'une mesure valide.

## Contexte

### Ce qu'a coûté le reboot du 03/09/2026 à 08:59:47 (7 s de coupure, filtration active)

| Heure | Journal | Coût |
|---|---|---|
| 08:59:54 | `Intégrale pH : instantané NVS REJETÉ (instantané périmé) — la régulation repart de zéro` | intégrale perdue |
| 08:59:54 | `[Dosage ORP] Stabilisation des mesures en cours — dosage chlore automatique suspendu` | purge de 15 min (`stabilization_delay_min` = 15) sans mesure valide |
| 09:14:52 | `Filtres pH/ORP : alimentation REPRISE — filtres ré-amorcés (warmup en cours)` | +1 min |
| 09:15:54 | `Démarrage dosage pH (auto): pH=7.59 cible=7.30 erreur=0.290` + FF 1,41 réarmé | reprise, **16 min après le boot, intégrale à 0** |

L'intégrale s'est reconstruite de 0 à 1 343 pH·s (0,50 mL/min) à 12:00 et n'a atteint son régime (1 968 pH·s, 0,73 mL/min) qu'à 14:30. Au départ (erreur 0,29) la perte était masquée — P (8 × 0,29 = 2,3 mL/min) + FF (1,41) saturaient déjà le débit soutenable (4,33 mL/min) — mais dès que l'erreur est passée sous ~0,2, il manquait un tiers de la demande. Ordre de grandeur : **une heure de régulation perdue** sur la journée, hors purge.

### Pourquoi l'instantané était périmé — c'est structurel, pas accidentel

- **R3** (`kPhIntegralPersistMaxAgeS` = 6 h, `constants.h` ~1295) : l'âge de l'instantané est mesuré en **heure murale** (`time()`), pour traverser le reboot.
- **R5** (`phPersistShouldWrite`, `ph_persist_logic.h` ~528) : on n'écrit qu'au plus toutes les 30 min **et seulement si la valeur a bougé de ≥ 5 % du plafond** (810 pH·s en vitesse `normal`).
- L'intégrale est **gelée toute la nuit** (filtration OFF). Elle ne bouge donc pas → aucune écriture entre l'après-midi de la veille et le matin. Au redémarrage de 08:59, après 2 h de filtration, elle n'avait pas encore varié de 810 pH·s depuis la dernière écriture (la veille) → âge > 6 h → **rejet**.
- Conséquence : **tout reboot survenant avant ~10-11 h du matin perd l'intégrale, par construction.** Et plus tard dans la journée, un reboot 6 h après la dernière écriture la perd aussi si la valeur a peu bougé (régime établi = précisément le cas où elle bouge peu).

C'est la question R3 laissée ouverte à la clôture de feature-084 (« options a/b/c »). Le cas réel du 03/09 la tranche : **le critère d'âge en heures murales est inadapté à une grandeur qui ne vit qu'en temps de filtration.**

### Le volet « purge » est traité par feature-091

La purge de stabilisation (15 min) au boot est un cas particulier de la purge à durée fixe que feature-091 rend adaptative (fin sur stabilité de la mesure, plancher = réglage utilisateur, plafond fail-safe). Le cas « reboot pendant filtration active » y est ajouté explicitement comme cas limite (cf. cette spec). **feature-093 ne touche pas à la purge** ; elle fournit en revanche une information que 091 peut consommer : la **durée de coupure réelle** (voir A4).

## Comportement attendu

1. **Fraîcheur en temps de filtration.** L'instantané d'intégrale reste valide tant que le bassin n'a pas accumulé plus de `kPhIntegralPersistMaxAgeS` (6 h) **de filtration** depuis son écriture. Les heures où la filtration est arrêtée ne comptent pas — exactement comme elles ne comptent pas pour l'intégrale elle-même (gelée hors filtration).
2. **Après un reboot pendant la filtration**, la régulation retrouve la valeur qu'elle avait en RAM une seconde avant la coupure (à l'écrêtage R1 et à la confirmation R4 près, inchangés).
3. **Après une nuit sans reboot** puis un reboot le matin, la valeur retrouvée est celle de la veille au soir — c'est-à-dire exactement celle que la régulation aurait en RAM si elle n'avait pas redémarré (l'intégrale ne bouge pas la nuit).
4. Les garanties R1 (écrêtage chimique), R2 (contexte), R4 (confirmation par la mesure, 5 min), R7 (anti-boucle de boot) **restent intégralement en place**. Cette spec n'élargit pas ce qui peut être restauré, seulement *quand* l'instantané est jugé frais.
5. Le journal dit ce qui s'est passé : valeur restaurée, âge en temps de filtration, ou motif de rejet.

### Cas limites
- Reboot pendant la filtration, en pleine session de dosage (le cas du 03/09) : restauration.
- Reboot à 06:00 après une nuit d'arrêt, dernière écriture la veille 15:00 : filtration accumulée depuis l'écriture = 4 h (15:00 → 19:03) → frais → restauration. Sans cette spec : 15 h murales → rejet.
- Reboot après **3 jours** de filtration arrêtée (hivernage, coupure secteur longue) : le temps de filtration accumulé est nul → l'instantané est « frais » au sens de 1. Est-ce chimiquement acceptable ? **À trancher par pool-chemistry** : soit on l'accepte (l'intégrale décrit une charge qui n'a pas pu évoluer sans circulation), soit on garde un plafond mural de sécurité très large (ex. 72 h) en plus du critère de filtration. La spec penche pour le double critère (filtration 6 h **et** mural ≤ 72 h) — R4 protège de toute façon (un pH déjà à la consigne au retour ⇒ candidate rejetée).
- Heure non synchronisée (NTP/RTC) : comportement inchangé — fail-closed, ni écriture ni restauration.
- Changement de contexte (vitesse, consigne, produit) : R2 rejette, inchangé.
- Compteur de filtration : doit survivre au reboot (l'anneau de charge `PhLoadHistory` dispose déjà d'une minute de filtration journalière persistée — l'architecte dira si elle est réutilisable ou s'il faut un compteur dédié dans l'instantané).

## Comportement actuel
- Instantané 20 octets (`PhIntegralSnapshot`) : `epoch` d'écriture en secondes murales ; `phPersistFresh(epochWritten, epochNow)` compare à `kPhIntegralPersistMaxAgeS`.
- Écriture R5 : intervalle ≥ 30 min **et** |Δ| > 5 % du plafond ; pas d'écriture à l'arrêt ni à la reprise de filtration.
- Restauration : `phPersistEvaluateRestore` (R1+R2+R3+R7) puis `phPersistConfirmTick` (R4, 5 min d'erreur > seuil de démarrage) dans une fenêtre d'armement de 10 min après boot.
- Purge de stabilisation au boot : durée fixe `stabilization_delay_min` (15 min sur l'appareil) — hors périmètre, cf. feature-091.

## Contraintes techniques
- ESP32, offline first
- Logique pure dans `ph_persist_logic.h` (ADR-0017) : le temps de filtration est un **paramètre**, jamais lu depuis un manager dans le module.
- Schéma de l'instantané : tout champ ajouté ⇒ `kPhIntegralPersistSchema` incrémenté (R2 : un ancien enregistrement est rejeté, jamais réinterprété). Conséquence acceptée : **le premier boot après la mise à jour perd l'intégrale une fois.**
- NVS : rester à l'ordre de grandeur de quelques écritures par jour (100 000 cycles garantis).
- Aucun handler bloquant ; l'écriture NVS emprunte le point de sauvegarde différée existant (loopTask).

## Pistes pour l'architecte (non prescriptives)

- **P1 — Âge en temps de filtration** (cœur de la spec) : stocker dans l'instantané un compteur de minutes de filtration à l'écriture ; à la restauration, âge = compteur courant − compteur écrit. Le compteur doit être monotone et persisté (NVS, écrit avec l'instantané ou avec les compteurs journaliers).
- **P2 — Re-timbrage à la reprise de filtration** (plus simple, couvre le cas du matin seulement) : à chaque passage OFF→ON de la filtration, réécrire l'instantané avec la valeur courante (identique à la veille) et l'`epoch` du jour, en contournant le critère de delta de R5. Ne couvre pas le reboot en régime établi 6 h après la dernière écriture (valeur stable ⇒ pas de réécriture) — à compléter par P3.
- **P3 — Écriture par échéance** : en plus du critère de delta, écrire dès que l'âge de la dernière écriture atteint la moitié de la fenêtre de fraîcheur (3 h de filtration), quelle que soit la variation. ≈ 3-4 écritures par jour.
- P1 est la solution propre ; P2+P3 est une approximation à moindre coût de schéma. L'architecte choisit ; pool-chemistry valide la fenêtre et le cas « longue coupure ».
- **A4 (bonus pour 091)** : au boot, calculer et journaliser la **durée de coupure** = `time()` au boot − dernier horodatage « vivant » persisté (la sauvegarde différée des compteurs journaliers passe déjà régulièrement). feature-091 pourra s'en servir pour dimensionner la purge (7 s de coupure ≠ une nuit). Si l'architecte juge que cela alourdit le cycle, reporter A4 dans 091.

## Fichiers potentiellement concernés
- `src/ph_persist_logic.h` — champ(s) de l'instantané, `phPersistFresh` en temps de filtration, R5 étendu (échéance / re-timbrage), schéma incrémenté
- `src/pump_controller.cpp` — câblage : fournir le temps de filtration aux décisions ; hook OFF→ON de la filtration si P2 ; journaux
- `src/config.cpp` — lecture/écriture NVS du blob (taille si schéma étendu) ; horodatage « vivant » si A4
- `src/constants.h` — `kPhIntegralPersistMaxAgeS` re-documentée en temps de filtration ; éventuel plafond mural de sécurité ; échéance d'écriture P3 ; `static_assert` de cohérence à revoir (~1655-1668)
- `test/test_native_ph_persist/test_ph_persist.cpp` — nouveaux cas (nuit, reboot matin, longue coupure, échéance) + mutations
- `docs/subsystems/pump-controller.md`, `docs/adr/0053-persistance-conditionnelle-integrale-ph.md` (amendement ou ADR successeur), `CHANGELOG.md`

## Livrables — check-list
- [ ] A1 — Critère de fraîcheur R3 exprimé en temps de filtration (module pur), avec le traitement de la longue coupure tranché par pool-chemistry
- [ ] A2 — Cadence d'écriture R5 garantissant qu'un instantané valide existe à tout moment de filtration (P1, ou P2+P3)
- [ ] A3 — Schéma de l'instantané incrémenté ; migration = rejet propre de l'ancien enregistrement, journalisé une fois
- [ ] A4 — (optionnel, ou reporté dans 091) Durée de coupure calculée et journalisée au boot
- [ ] A5 — Journaux : restauration avec âge en temps de filtration ; rejet avec motif
- [ ] D1 — Tests natifs : reboot en filtration ; reboot le matin après nuit ; reboot 6 h après écriture en régime établi ; longue coupure ; heure non synchronisée ; validés par mutation (chaque nouveau critère doit tuer au moins un mutant)
- [ ] C1 — `docs/subsystems/pump-controller.md` (section persistance), amendement ADR-0053 ou ADR successeur, `CHANGELOG.md`

## Sécurité chimique
- Dosage impliqué : oui — pH- (acide) ; la restauration d'une intégrale est une dose différée
- Limites journalières / horaires concernées : inchangées (la restauration ne dose pas ; l'intégrale restaurée passe par le débit soutenable, l'écrêtage R1 et la confirmation R4)
- Filtration requise avant action : oui, inchangé
- Calibration capteur modifiée : non
- Algorithme PID touché : non (seule la condition de validité de l'instantané change)
- Risque identifié : restaurer une intégrale qui ne décrit plus le bassin (longue coupure). Mitigations : R1 (plafond ≈ 30 % du plafond, borné par la charge observée), R4 (5 min d'erreur confirmée), critère mural de sécurité éventuel. **pool-chemistry statue sur la longue coupure.**

## ADR attendu ?
- **Probable** : oui
- **Critère(s) candidat(s)** : 2, 3 (le choix « temps de filtration vs temps mural », l'alternative P2+P3 écartée ou retenue, le traitement de la longue coupure)
- **Sujet pressenti** : amendement de l'ADR-0053 (« la fraîcheur d'une grandeur de filtration se mesure en temps de filtration »)

## Éléments visibles (pour doc-writer)

### Endpoints HTTP concernés
- aucun

### Champs WebSocket
- aucun (si l'âge restauré est exposé dans `/data`, le dire ici)

### Topics MQTT
- aucun

### Documentation à produire / mettre à jour
#### Docs vivantes
- `docs/subsystems/pump-controller.md` — section persistance de l'intégrale (R3/R5 révisées, nouveaux journaux)
#### Docs transverses
- `CHANGELOG.md` — « un redémarrage ne fait plus perdre l'intégrale pH »
- `docs/UPDATE_GUIDE.md` — note : le premier boot après mise à jour repart d'une intégrale nulle (changement de schéma)
#### Docs immuables
- amendement ADR-0053 ou ADR successeur

## Critères d'acceptation
- [ ] *(terrain, 05/09)* Quand le firmware redémarre pendant la filtration avec une intégrale de X pH·s, alors, une fois la mesure valide et R4 confirmée, l'intégrale vaut min(X, plafond R1) et le journal indique la restauration avec l'âge en temps de filtration
- [x] Quand le firmware redémarre le matin après une nuit d'arrêt, la dernière écriture datant de la veille (< 6 h de filtration accumulée), alors l'instantané est accepté (aujourd'hui : rejeté « périmé ») *(test_J_nuit_sans_filtration_puis_reboot_accepte)*
- [x] Quand plus de 6 h de filtration se sont écoulées depuis l'écriture, alors l'instantané est rejeté « périmé » — et ce cas ne peut plus se produire en fonctionnement normal grâce à A2 (test : simuler 8 h de filtration sans reboot → au moins une écriture) *(test_J_361_min_…, test_J_huit_heures_de_filtration_au_moins_une_ecriture)*
- [x] Le cas « longue coupure » se comporte comme pool-chemistry l'a tranché, et un test natif le fige *(double critère filtration 360 min ET mural 72 h ; test_J_72h_plus_1s_rejete_…, test_J_compteur_non_monotone_…)*
- [x] Un ancien instantané (schéma précédent) est rejeté avec un journal explicite, sans plantage *(test_J_schema_1_est_rejete_avant_tout_autre_controle + log info ajouté en revue)*
- [x] Le nombre d'écritures NVS reste ≤ 6 par jour de filtration en régime établi (test natif comptant les écritures sur une journée simulée) *(test_J_douze_heures_de_filtration_au_plus_six_ecritures)*
- [x] Tous les libellés utilisateur sont en français
- [x] `pio run` compile sans nouveau warning ; `pio test -e native` : zéro régression ; mutations tuées *(29 suites / 1089 cas ; mutation candidatePending tuée)*
- [ ] *(après déploiement)* Comportement nominal testé sur l'appareil selon le plan ci-dessous

## Plan de test manuel
1. Appareil en régime établi l'après-midi (intégrale > 800 pH·s, lisible dans `/data`) : `POST /debug/reset` (ou reboot) → vérifier au journal la restauration après confirmation R4 et la valeur dans `/data`.
2. Reboot le matin, 1 h après la reprise de filtration (le cas du 03/09) : même vérification ; l'intégrale doit valoir celle de la veille au soir.
3. Changer la vitesse de régulation puis rebooter : rejet R2, journal explicite.
4. Vérifier au bout d'une journée le nombre d'écritures (journal ou compteur de diagnostic).

## Notes pour agent
- **Ne rien déployer avant la fin de la fenêtre d'observation 84/85/86 (~06/09/2026).**
- Lire la config vivante (`/get-config`) avant tout chiffrage, pas les défauts d'usine (piège documenté).
- R7 (anti-boucle de boot, streak 3) est d'autant plus important que le bug-080 n'est pas résolu : ne pas l'affaiblir.
- La purge de stabilisation est le sujet de feature-091 ; ne pas la modifier ici même si c'est tentant.

## Hors périmètre
- Purge de stabilisation adaptative (feature-091)
- Cause des redémarrages (bug-080)
- Persistance de l'état de session (pause de mélange, budgets horaires) : perdue au reboot, coût faible (une pause de 15 min au pire), non traité ici
- Persistance du feedforward : déjà en place et fonctionnelle (1,41 mL/min retrouvé le 03/09)

---

## Notes d'implémentation (v2.34.0, 04/09/2026)

### Ce qui a été livré
- **Schéma 2 de l'instantané** `PhIntegralSnapshot` (24 o, `kPhIntegralPersistSchema = 2`) :
  nouveau champ `filtTotalMin` = valeur à l'écriture du **compteur monotone de
  filtration** `SafetyLimits::filtTotalMin` (clé NVS `pool-daily/filt_total`).
  Un instantané v1 (20 o) est rejeté au premier boot 2.34 → l'intégrale repart de
  zéro **une fois**, avec un log `info` explicite (« aucun instantané NVS
  exploitable … départ de zéro », ajouté en revue).
- **Fraîcheur en temps de FILTRATION** (R3) : restauration si âge de filtration
  ≤ `kPhIntegralPersistMaxFiltMin` 360 min ET âge mural ≤
  `kPhIntegralPersistMaxWallAgeS` 72 h ET compteur monotone cohérent. Une nuit
  sans filtration ne périme plus rien (c'était le défaut structurel du 03/09).
- **Motifs d'écriture** via `phPersistWriteReason(PhPersistWriteInputs)` (module
  pur `src/ph_persist_logic.h`) : `kRewrite` (aucun instantané valide et
  intégrale > 0, C5), `kDelta` (≥ 5 % du plafond et ≥ 30 min), `kDue` (échéance
  `kPhIntegralPersistDueFiltMin` 180 min de filtration même sans variation, P3).
- **Confirmation différée R4** conservée : la valeur relue reste
  `_phIntegralConfirm.candidate` jusqu'à 5 min consécutives d'erreur au-dessus du
  seuil de démarrage, filtre prêt et porte de validité ouverte (donc jamais
  pendant la purge de feature-091).
- **Garde `candidatePending`** (ajoutée en revue, Majeur 1) : tant qu'une
  candidate attend sa confirmation, **aucune écriture** (kRewrite/kDelta/kDue) —
  « la NVS a raison, pas la RAM ». Sans elle, `kDelta` écrivait 0 par-dessus
  l'instantané valide pendant les 5 minutes de confirmation, ce qui annulait la
  feature à chaque reboot suivi d'un second reboot.
- R7 (anti-boucle, streak 3) inchangé.

### Tests
- `test/test_native_ph_persist/` : 89 cas dont le nouveau
  `test_J_candidate_en_attente_interdit_toute_ecriture` (kDelta sans drapeau ;
  kNone avec drapeau même à échéance mûre et avec `rewriteWanted` ; retour à
  kDelta drapeau baissé). **Mutation vérifiée** : garde retirée → exactement 1
  échec (ce test) ; garde restaurée → vert. Total natif : 29 suites, 1089 cas.
- Compilation core3 : Flash 98,6 %, aucun warning nouveau.

### Revue
Request changes (3 majeurs, dont 1 sur cette feature) → correctifs → **Approved**
(04/09 21:15). Mineurs laissés volontairement, à ne pas perdre : commentaire
`_waiverPending`, commentaire de repliement `millis()` dans
`measureValidityCirculationSettled` (bénin), `static_assert` cadence
EZO/échantillonnage, `warn` sur la clé POST `stabilization_delay_min` obsolète.

### Documentation
ADR-0060 (ou amendement de l'ADR-0053) : fraîcheur en temps de filtration +
garde `candidatePending` ; `docs/subsystems/pump-controller.md`,
`docs/UPDATE_GUIDE.md` (perte unique au premier boot), CHANGELOG 2.34.0.

### Validation terrain attendue
Premier matin après déploiement (05/09) : vérifier au journal que l'instantané
écrit la veille au soir est **restauré puis confirmé** à la reprise de
filtration (après la purge adaptative), et non rejeté pour péremption.
