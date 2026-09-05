## Métadonnées
- **ID** : feature-091
- **Slug** : purge-adaptative-stabilisation
- **Type** : feature
- **Statut** : done (implémentée, tests verts, revue Approved 04/09/2026 — déploiement 2.34.0 en attente du go utilisateur)
- **Priorité** : haute (incident quotidien ; mitigation 5 → 15 min du 02/09 insuffisante, cf. note du 04/09)
- **Version cible** : 2.34.0 (cycle commun avec feature-093, plan §5 point 3)
- **Dernière mise à jour** : 2026-09-04
- **Origine** : incident du 02/09/2026 au matin (diagnostic utilisateur confirmé)

## Domaines touchés
- [x] Capteurs (pH / ORP) — `measure_validity`, filtres
- [x] Régulation (pump_controller — porte de stabilisation dans `canDose`) → **pool-chemistry obligatoire**
- [x] Web / MQTT / UI — retrait du réglage `stabilization_delay_min`, nouveaux champs « durée mesurée / motif de fin » dans `/data` + WS (parité)
- [x] Frontend : page Réglages (retrait du champ), carte pH/ORP (compte à rebours → « depuis X min »), wizard si concerné → **ux-ui-designer** à consulter pour la présentation de la durée mesurée

## Le problème (constaté le 02/09/2026)

Après une nuit de stagnation, l'eau immobilisée dans les canalisations dérive
chimiquement (CO₂ accumulé → pH local réellement bas ; chlore consommé → ORP
bas). Au redémarrage de la filtration, la purge à durée FIXE (5 min à l'époque)
s'est terminée **au moment exact où ce bouchon d'eau morte arrivait à la
sonde** : bruts mesurés 6,85 → 5,60 pH (delta −1,25) pendant la purge, puis
alertes en cascade au premier tick représentatif (eau agressive, butée brute
6,20, plancher 6,70, latch feedforward). Rinçage réel observé : **~25-30 min**
(retour à 7,14 corrigé en une demi-heure, zéro dosage).

La durée fixe est structurellement fausse dans les deux sens : trop courte
après une longue stagnation (lectures mensongères, alertes à tort), trop
longue après une micro-coupure (régulation retardée pour rien).

## Le principe demandé

**La purge se termine sur la STABILITÉ de la mesure, pas sur un chronomètre.**
La porte des mesures représentatives (et donc du dosage) reste fermée tant que
la lecture brute évolue ; elle s'ouvre quand la mesure s'est stabilisée.

L'instrumentation de base existe déjà : le journal `[STAB]` mesure les bruts
en début et fin de purge avec le delta — c'est lui qui a permis le diagnostic.

## Esquisse (à valider par l'architecte)

- Pendant la purge : suivre le brut pH (et ORP) par fenêtre glissante ; la
  purge se termine quand |delta| sur la fenêtre < seuil pendant N échantillons.
- **Plancher = constante firmware, le réglage utilisateur disparaît** (décision
  du 04/09/2026, remplace l'esquisse initiale « l'utilisateur garde la main sur
  un minimum »). Raisons : (a) le plancher a un sens physique — le temps de
  renouvellement de l'eau dans la tuyauterie (1-2 min) plus l'amorçage du filtre
  (fenêtre médiane 65 s) —, pas un sens de préférence ; (b) un réglage que
  l'utilisateur doit tâtonner (5 → 15 min le 02/09, encore insuffisant le
  04/09) est exactement ce que la fin sur stabilité rend inutile ; (c) une
  valeur trop courte saisie par erreur recrée l'incident, une valeur trop longue
  recrée l'attente. Ordre de grandeur : `kStabilizationFloorMs` ≈ 3 min, à
  fixer par l'architecte/pool-chemistry. **En remplacement**, l'UI affiche la
  *durée mesurée* de la dernière purge et son motif de fin (stabilité /
  plafond), information plus utile que le réglage qu'elle remplace.
  Migration : le champ NVS `stabilization_delay_min` est conservé mais INERTE
  (précédent `screen_enabled`, feature-065) ; retiré de la page Réglages
  (`data/index.html` ~1824, `app.js` ~338/394/2664/8892) et des défauts par
  taille de bassin (`regulation_defaults.h` : `stabilizationDelayMin` devient
  sans effet ou est supprimé, test natif `test_native_regulation_defaults` à
  ajuster) ; le compte à rebours existant (`*_stab_remaining_s`) devient
  indicatif (« stabilisation en cours depuis X min »), puisqu'il n'y a plus de
  durée connue à l'avance.
- **Piège du critère de stabilité (constaté le 04/09)** : à la reprise, la
  lecture de l'eau stagnante est *déjà stable* (6,78 brut, immobile toute la
  nuit) et le transitoire ne commence que quelques minutes plus tard (brut à
  5,60 après 5 min le 02/09, 5,97 après 15 min le 04/09, retour vers 33 min).
  Un critère « N échantillons stables » ouvrirait la porte à t+1 min, juste
  avant le plongeon. Le critère doit donc porter sur toute la fenêtre depuis
  la reprise (aucune variation > seuil depuis l'ouverture de la purge, ou
  « changement détecté puis re-stabilisation ») **et** être conjugué au
  plancher. La courbe brute à 5 s des 20 premières minutes (`/debug/sensor_trace`,
  anneau de 25 min) est à acquérir dans le protocole N1 du plan pour caler le
  seuil et la fenêtre.
- **Plafond fail-safe obligatoire** : une purge qui ne converge jamais (sonde
  réellement en dérive) doit se terminer par un TIMEOUT borné (ex. 45-60 min)
  avec alerte explicite — sinon une sonde malade bloquerait la régulation en
  silence indéfiniment. Le sens du timeout est à trancher par pool-chemistry :
  ouvrir en signalant, ou rester fermé en alertant (fail-closed).
- Logique pure testable (ADR-0017) : le critère de stabilité en module .h natif.

## Cas limites
- Micro-coupure de filtration (< 1 min) : purge courte (le plancher).
- Nuit complète : ~25-30 min observés — la purge doit suivre sans réglage.
- Sonde en dérive lente réelle : ne jamais confondre avec un rinçage —
  d'où le plafond + alerte.
- Redémarrage en cours de purge : reprise propre.
- **Redémarrage du firmware pendant une filtration active** (cas réel du
  03/09/2026 08:59:47, PANIC bug-080, coupure de 7 s) : l'eau n'a pas stagné,
  rien à rincer, et pourtant la purge fixe de 15 min a retardé la régulation
  d'un quart d'heure (journal : `Stabilisation des mesures en cours` 08:59:54 →
  `Stabilisation terminée` 09:14:51). Avec la purge adaptative, la stabilité
  est atteinte immédiatement → la purge doit se réduire au plancher, et le
  plancher (`stabilizationDelayMin`, porté à 15 min le 02/09 en mitigation)
  doit pouvoir redescendre à quelques minutes. Mieux : **une coupure ≤ 10 s
  dispense de purge** (seuil `kShortOutageS = 10`, décision utilisateur du
  04/09 : la coupure observée est de 7 s, la marge couvre le temps de
  démarrage). Cela suppose de connaître la durée de coupure : `time()` au boot
  − dernier horodatage « vivant ». La NVS ne convient pas (il faudrait écrire
  toutes les secondes) ; la piste est la **mémoire RTC lente** (`RTC_NOINIT_ATTR`,
  rafraîchie chaque seconde à coût nul), qui survit à un reset logiciel, un
  PANIC ou un watchdog mais pas à une coupure secteur — et c'est le bon
  comportement : après une coupure secteur la durée est inconnue, on purge
  (fail-closed). Condition supplémentaire : la filtration était ON avant la
  coupure (l'état est aussi à garder en mémoire RTC), sinon on est dans le cas
  « reprise » normal. L'architecte valide le mécanisme ; feature-093 (A4) est
  déchargée de ce point si 091 le porte. Exigence utilisateur : « un reboot ne
  devrait pas perturber le fonctionnement et ralentir l'atteinte de la cible ».
- Interaction avec le sas post-calibration EZO (3-5 min) : les deux portes
  sont des OR indépendants dans canDose — ne pas fusionner.

## Sécurité chimique
- La porte de stabilisation conditionne le DOSAGE : tout changement = visa
  pool-chemistry (amont sur le critère de stabilité et le sens du timeout).
- Sens sûr à préserver : en cas de doute, la porte reste FERMÉE (pas de dosage
  sur mesure suspecte) — mais jamais silencieusement au-delà du plafond.

## Critères d'acceptation
- [x] Rejouer le scénario du 02/09 (stagnation longue) : aucune alerte
      critique au démarrage, dosage autorisé seulement une fois la mesure
      stabilisée
- [x] Micro-coupure : purge ≈ plancher, pas de sur-attente
- [x] Sonde en dérive : timeout au plafond + alerte explicite, comportement
      visé par pool-chemistry
- [x] Logique pure testée en natif, mutations tuées (critère de stabilité,
      plancher, plafond)
- [x] Reboot logiciel ≤ 10 s pendant une filtration active : aucune purge,
      dosage possible dès que les filtres sont amorcés (~65 s) ; coupure
      secteur ou durée inconnue : purge normale (fail-closed)
- [x] Le réglage `stabilization_delay_min` n'apparaît plus dans l'UI ; le champ
      NVS existant est ignoré sans erreur (config ancienne relue proprement) ;
      les défauts par taille de bassin n'en dépendent plus
- [x] L'UI et le journal indiquent la durée mesurée de la dernière purge et son
      motif de fin (stabilité / plafond) ; le compte à rebours devient un
      « depuis X min » (parité `/data` ↔ WebSocket)
- [x] La lecture stable de l'eau stagnante à t+0 n'ouvre pas la porte (test
      natif rejouant la courbe du 04/09 : stable, plongeon à t+5, retour t+33)
- [x] Aucun terme de régulation (P, I, FF) ni aucune alerte pH/ORP n'est
      alimenté par des mesures antérieures à l'ouverture de la porte
- [ ] Docs (doc-writer en cours le 04/09 21:15) : `docs/features/page-settings.md`, `page-wizard.md` si le réglage y
      figurait, guide utilisateur PDF (capture d'écran Réglages à refaire),
      `CHANGELOG.md`, ADR (retrait d'un réglage utilisateur au profit d'un
      automatisme = décision à tracer)

> Critères cochés le 04/09/2026 sur la base des tests natifs (rejeux 0209/0409,
> dérive, plafond, dispense) et de la revue ; la **validation terrain** (matin
> du 05/09 : durée de purge réelle, aucune alerte à tort, intégrale intacte)
> reste à consigner ici après déploiement.

## Note de mitigation
Le 02/09, `stabilization_delay_min` a été porté de 5 (défaut Medium) à 15 min
sur l'appareil de référence — compromis en attendant cette feature. Les
défauts d'usine par taille (regulation_defaults.h) restent à 5/5/10 : à
réévaluer AU MÊME CYCLE que cette feature (un plancher de 5 min + purge
adaptative est probablement le bon couple).

**04/09/2026 — la mitigation à 15 min est insuffisante, l'incident du 02/09 s'est
reproduit à l'identique** (journal `builds/2.33.0/esp32dev_core3/system_log_20260904_0900.txt`,
enregistrement `/data` 60 s en parallèle) :
- 06:56:59 reprise filtration, bruts à la reprise pH 6,78 / ORP 583 mV (eau stagnante).
- 07:12:00 fin de purge : `[STAB] Bruts en fin de purge : pH=5.969 (delta -0.810)
  ORP=527.4 mV (delta -56.1)` — le bouchon arrive à la sonde **au moment où la
  porte s'ouvre**, comme le 02/09.
- 07:13:05 cascade : `ph_low_warning` (6,28), `ph_guard` butée brute (5,78 < 6,20),
  `ph_guard` plancher (6,28 < 6,70), feedforward retenu. Les gardes ont fait
  leur travail (aucune injection d'acide), mais trois alertes à tort.
- Rinçage réel : gardes levées 07:24-07:30, filtrée revenue à 7,25 vers 07:40,
  7,33 à 07:50 → **~45-50 min** après la reprise (30-35 min après la fin de purge).
- **Dégât collatéral sur la régulation** : l'intégrale pH, gelée à 440 pH·s
  pendant la nuit, a été **remise à zéro puis chargée négativement** (−191 pH·s
  à 07:50) par l'erreur négative du bouchon (règle de reset `error <
  −(phStartThreshold + kPhIntegralResetMargin)`). La journée démarre donc avec
  un terme intégral de −0,07 mL/min au lieu de +0,16 — même effet qu'un reboot
  (feature-093), par un autre chemin. Première session pH à 07:52 (pH 7,35).
- Côté ORP : session de 69 s à 07:13 sur un ORP de bouchon (515 mV), puis
  dosage légitime à 485-488 mV à partir de 07:34.

Conséquences pour la spec : (1) la priorité passe à **haute** — l'incident est
quotidien, pas exceptionnel ; (2) ajouter aux critères d'acceptation : *l'eau de
rinçage ne remet pas l'intégrale pH à zéro et ne la charge pas* — aujourd'hui
le dégât survient APRÈS la purge trop courte, sur des mesures déclarées
représentatives ; avec la fin de purge sur stabilité, la porte doit rester
fermée pour TOUS les termes (P, I, FF, gardes d'alerte) tant que la mesure
bouge, pas seulement pour l'injection ; (3) le critère de stabilité doit tenir sur ~50 min de rinçage
observé, pas 25-30 comme estimé le 02/09.

---

## Notes d'implémentation (v2.34.0, 04/09/2026)

### Ce qui a été livré
- **Détecteur** (`src/purge_stability_logic.h/.cpp`, module pur ADR-0017 ;
  intégration `src/measure_validity.cpp`) : médiane par minute de
  `kPurgeSamplesPerMin = 12` lectures EZO (5 s) par voie (pH brut, ORP brut),
  fenêtre glissante de `kPurgeWindowMin = 10` médianes ; **stable** = fenêtre
  pleine ET étendue ≤ `kPurgeStablePhRange` 0,15 pH ET ≤ `kPurgeStableOrpRange`
  15 mV ; **refermeture C1** si l'étendue redépasse 0,30 pH / 30 mV (= 2 × seuil,
  verrouillé par `static_assert`) ; **plancher** `kPurgeFloorMs` = 10 min ;
  **plafond** `kPurgeCapMs` = 90 min → la porte S'OUVRE quand même, alerte
  `measures_purge_timeout` (MQTT + bannières UI), refermeture désarmée jusqu'à
  la prochaine purge.
- **Dispense de purge** après coupure ≤ 10 s : enregistrement `OutageRtcRecord`
  en `RTC_NOINIT` (empreinte FNV-1a, liste blanche de raisons de reset
  SW/PANIC/INT_WDT/TASK_WDT, au plus 1 dispense par heure
  `kOutageWaiverMinIntervalS`). Une coupure secteur (brownout/power-on) ne
  dispense jamais.
- **Porte étendue à tous les termes** (critère ajouté le 04/09) : le dosage
  programmé attend `isCirculationSettled()` (C4) ; `SensorFilter::ready()` est
  faux porte fermée (C6) → P, I, feedforward et gardes d'alerte ne voient
  aucune mesure de rinçage.
- **Réglage `stabilization_delay_min` retiré** de l'UI (Réglages, wizard) au
  profit de l'automatisme ; textes « plancher 10 min, plafond 90 min ».
- **Télémétrie** en parité `/data` ↔ WebSocket (vérifiée par grep des deux
  côtés, piège mémorisé) : `measures_valid`, `measures_stab_elapsed_s`,
  `measures_purge_last_duration_s`, `measures_purge_last_end` (0-4),
  `measures_purge_count`. C'est la matière de feature-097.

### Itérations (deux ré-arbitrages pool-chemistry sur la fenêtre)
| # | Fenêtre / plancher | Résultat | Décision |
|---|---|---|---|
| 1 | W = 4, plancher 5 min | Rejeu du CSV réel du 04/09 : ouverture t+7-8 à pH **5,6** (fond du V) | Invalidé par le test-engineer (`test_E_rejeu_0409_*`) |
| 2 | W = 8, plancher 8 min | Sans bruit : ouverture t+13 à pH 5,71 (étendue 0,145, 5 millièmes sous le seuil), refermeture C1 à t+17 | Trop près du bord — invalidé |
| 3 | **W = 10** (capacité de l'anneau `kPurgeStabilityMaxWindowMin`), **plancher 10 min** | Ouverture t+47 sans bruit / t+46-62 avec bruit, sur le plateau (pH 6,88) ; refermeture C1 dès 0,033 pH/min | **GO** pool-chemistry, conditions 6-11 |

**Physique retenue** : le rinçage est un **V** (descente −0,047 pH/min, remontée
+0,043 pH/min le 04/09). Le fond occupe une demi-fenêtre de chaque côté, donc la
pente détectable vaut **seuil / ((W − 1) / 2)** : 0,15/1,5 = 0,10 pH/min à W = 4
(aveugle au V), 0,033 à W = 10. Coût : ≈ 10 min + 65 s après longue coupure,
contre 15 min de minuterie fixe avant — et une ouverture **sur le plateau** au
lieu du fond du V.

### Limites connues et assumées (GO option (a), fail-closed)
- Une dérive ≤ 0,017 pH/min est jugée stable ; l'ORP seul ne détecte pas une
  pente ≤ 1,6 mV/min (ORP est une voie secondaire : seuil 15 mV ≈ 13,5 mV de
  bruit).
- Sur eau plate avec bruit ±0,18 pH, l'étendue de 10 médianes ≈ 0,16 ≈ seuil :
  ouverture au plancher dans 12 graines sur 21, queue possible jusqu'à t+29
  (`test_E_rejeu_0409_5_graines_ouvrent_ou_plafond`). Option (b) seuil 0,20
  **refusée** ; option (c) médianes sur 2 min = piste préférée →
  **feature-097** (décision sur ≥ 5 matins de `measures_purge_last_*`, au plus
  tôt le 10/09).
- Recommandation 5 de pool-chemistry (révision des défauts d'usine par taille)
  différée : le réglage n'existe plus côté utilisateur.

### Tests
- `test/test_native_purge_stability/` (25 tests : rejeux 0209/0409 synthétiques
  et bruités, dérives lentes, ORP en pente, démonstration W = 8 à tort) et
  `test/test_native_measure_validity/` (plancher 10 min, deux refermetures puis
  plafond). Mutation : W 10 → 8 tué par 6 tests ; plancher < fenêtre tué par
  `static_assert`. Total natif : 29 suites, 1089 cas, 0 échec.
- Compilation core3 : Flash 98,6 % (1 808 697 / 1 835 008), aucun warning
  nouveau.

### Revue
- Request changes puis correctifs : branche UI fausse « Stabilisation non
  atteinte (90 min) » sur `measures_purge_last_end === 2` retirée (le plafond
  OUVRE la porte — les bannières portent l'info) ; `-Os` restauré dans
  `[env:esp32dev]`. Voir aussi feature-093 (garde `candidatePending`).

### Documentation
ADR-0059 (fenêtre de médianes, retrait du réglage), `docs/subsystems/measure-validity.md`,
API.md, MQTT.md, ws-manager.md, pages UI, guides utilisateur PDF, CHANGELOG 2.34.0.
