# ADR-0050 — Le débit à pleine puissance est une constante UNIQUE de flotte, majorée de la dispersion mesurée (amende les valeurs d'usine de l'ADR-0047)

- **Statut** : Accepté
- **Date** : 2026-08-28
- **Décideurs** : gate `pool-chemistry` (arbitrage des deux valeurs, 2026-08-28),
  utilisateur (propositions 70,0 et 26,0, écartées), architect
- **Spec(s) liée(s)** : [feature-075-assistant-mesure-debit-pompe](../../specs/features/done/feature-075-assistant-mesure-debit-pompe.md)
  (les mesures qui déclenchent cet ADR viennent de l'assistant qu'elle livre) — v2.25.2

## Contexte

L'[ADR-0043](0043-dosage-proportionnel-de-temps-amplitude-calibree.md) a supprimé le
modèle linéaire duty↔débit et ne conserve que **deux points connus** : le débit **pesé
à l'amplitude** et le débit **à duty 255**. L'[ADR-0047](0047-calibration-de-type-par-defaut-et-gel-amplitude.md)
a ensuite posé une **calibration de type** (valeur d'usine du modèle de pompe) pour que
tout appareil de flotte dose dès son premier flash, en assumant deux dettes explicites :
la valeur d'usine à 127 était une **interpolation**, et les débits à 255 restaient deux
constantes **par index physique de pompe**.

Deux faits nouveaux referment ces dettes en sens contraire de ce qui était écrit.

**1. Les débits à 255 étaient des mesures d'exemplaire figées sur un index physique.**
`kPump1Flow255MlPerMin = 61,0` et `kPump2Flow255MlPerMin = 67,0` n'étaient **pas
configurables**, provenaient d'**un seul exemplaire chacune**, et servaient de
**majorant de sécurité** dans trois usages : crédit du kick de chaque impulsion,
majorant du mode manuel `?duty=N`, durée d'une injection volumée sur le **chemin
dégradé** (pompe dont l'amplitude ne correspond pas au duty calibré). La constante de
la pompe 1 était calée sur l'exemplaire **le plus lent** du lot. Un appareil de flotte
dont la pompe pH ressemblerait à l'exemplaire « 67 » se serait donc **sous-crédité de
11 %** : il aurait injecté plus que ses limites ne le croyaient. Attribuer la mesure
d'un exemplaire à un **index physique** n'a par ailleurs aucun sens dès qu'on parle de
flotte — rien ne fait qu'une pompe soit plus rapide parce qu'elle est câblée en n°2.

**2. Les mesures réelles à duty 127 existent enfin.** L'assistant de mesure du débit
(feature-075) a été exécuté on-target le 2026-08-28, 120 s par pompe : **26,0 mL/min
(pompe 1) et 25,5 mL/min (pompe 2)**. Les interpolations d'origine (24,7 et ~27,2)
étaient bonnes à **4-7 %** près — le modèle par calibration ponctuelle tient. La
justification de `kPumpTypePulseFlowMlPerMin` peut donc quitter l'interpolation pour la
mesure, ce qui était la dette explicite de l'ADR-0047.

**3. Une valeur de sécurité était recopiée dans le navigateur.** `data/app.js` portait
`FLOW_255_ML_PER_MIN = {1: 61.0, 2: 67.0}`, miroir en dur des constantes firmware
utilisé pour annoncer la durée d'une injection manuelle. C'est la **troisième
occurrence** du même motif dans le projet (après les durées de l'assistant et les
libellés de refus de dosage) : une copie cliente d'une valeur de sécurité dérive
silencieusement — celle-ci était devenue fausse de ~21 %.

## Décision

### 1. Une seule constante de débit à 255, majorant de flotte

`kPump1Flow255MlPerMin` / `kPump2Flow255MlPerMin` sont **supprimées** et remplacées par
`kPumpFlow255MlPerMin = 74.0f` ([`constants.h`](../../src/constants.h)). Les helpers
`pumpFlow255()` et `flowAssistFlow255()`, qui **dispatchaient par index physique**, sont
supprimés eux aussi : ce dispatch **était le défaut**, pas un service rendu. 11 sites
d'appel sont ramenés à la constante unique.

Une constante de sécurité non configurable est un **majorant de flotte**, pas la fiche
technique d'un exemplaire.

### 2. La marge doit dépasser la dispersion observée : 74,0 et non 70,0

74,0 = **maximum réellement mesuré 68,0 + 8 % de dispersion inter-unités**. La valeur
70,0 proposée a été **refusée par le gate** : elle n'offre que **+3 %** de marge alors
que l'écart **déjà constaté entre deux exemplaires du même lot** est de **11 %**. Une
marge inférieure à la dispersion observée est **statistiquement nulle** — elle donne
l'apparence d'une précaution sans en être une.

Le même arbitrage est rendu le même jour sur `kPumpTypePulseFlowMlPerMin` : la valeur
**reste 28,0**, mais sa justification est refondée — ce n'est plus « interpolation
24,7 / 27,2 », c'est **max mesuré 26,0 + 8 % de dispersion**. La valeur 26,0 proposée a
été refusée pour la même raison : c'est le maximum de **n = 2 exemplaires du même lot**,
donc une marge nulle, pas une statistique de flotte.

### 3. Les deux maxima mesurés deviennent des verrous de compilation

`kPumpFlow255MeasuredMaxMlPerMin = 68.0f` et `kPumpTypeFlowMeasuredMaxMlPerMin = 26.0f`
n'interviennent dans **aucun calcul**. Elles n'existent que pour porter deux
`static_assert` interdisant qu'une future « simplification » ramène le défaut **sous le
maximum réellement mesuré**. Elles ne changent que sur une **nouvelle mesure
d'exemplaire**.

### 4. Aucun miroir client d'une valeur de sécurité

Le frontend lit `flow_assist.flow255` servi par `GET /data`. Le **repli à 61 / 67** est
conservé **délibérément** : un firmware antérieur à la v2.25.2 ne publie pas ce champ et
dose réellement à ces débits-là. Aligner le repli sur 74 reviendrait à **mentir sur un
firmware ancien**. Le tableau `flow255` reste de taille 2 (même valeur répétée) : casser
la forme ferait tomber les clients existants sur leur repli générique.

## Alternatives considérées

- **Deux valeurs : un majorant pour créditer, un estimateur central pour la durée**
  (proposée par doc/architect, **rejetée**) — les **trois** usages poussent dans le même
  sens : surestimer le débit ne sur-injecte **jamais** (pompe ON moins longtemps,
  compteurs plus généreux, limites qui mordent plus tôt). Un « estimateur central »
  destiné à la durée serait, par construction, une valeur choisie pour **allonger la
  marche de la pompe** — exactement le risque interdit. La précision de l'injection
  volumée dispose déjà d'un **chemin exact** (`ampDuty == calDuty` ⇒ débit pesé) ; le
  chemin à 255 est un **repli dégradé**, et la précision d'un repli ne s'achète pas
  contre une garde.
- **70,0 mL/min** (proposée par l'utilisateur, **rejetée par le gate**) — +3 % de marge
  face à 11 % de dispersion constatée : marge de façade.
- **26,0 mL/min comme valeur d'usine à 127** (proposée par l'utilisateur, **rejetée par
  le gate**) — max de n = 2 exemplaires d'un même lot, marge nulle. À duty 127 la pompe
  travaille en **zone non linéaire** (frottement balais, tension d'alimentation, couple
  de démarrage) et un tube **neuf** débite plus qu'un tube rodé : la queue de
  distribution est inconnue. Le fait que les deux pompes diffèrent de 10 % à 255 mais de
  2 % à 127 ne rassure pas — il **prouve** que le rapport de débit n'est pas transposable
  d'un point de fonctionnement à l'autre.
- **Rendre le débit à 255 configurable** (rejetée) — ajouterait un réglage de sécurité
  modifiable par l'utilisateur là où le firmware a besoin d'un majorant qu'il contrôle ;
  le raffinement individuel existe déjà, à l'amplitude d'exploitation, par l'assistant de
  mesure.
- **Conserver deux constantes en les recalant** (rejetée) — corrigerait les valeurs sans
  corriger le défaut : une mesure d'exemplaire resterait attachée à un index physique.

## Conséquences

### Positives
- Un appareil de flotte ne peut plus **sous-créditer** son dosage parce que sa pompe est
  plus rapide que l'exemplaire qui a servi de référence.
- Les deux valeurs d'usine sont désormais adossées à des **mesures**, plus à des
  interpolations — la dette explicite de l'ADR-0047 est soldée.
- Deux `static_assert` rendent impossible, à la compilation, un défaut sous le maximum
  mesuré.
- Une valeur de sécurité de moins recopiée dans le navigateur.

### Négatives / dette assumée
- **Les compteurs journaliers se remplissent un peu plus vite.** Le crédit du kick passe
  de **0,407 à 0,494 mL par impulsion**. Sur une journée au pouls minimal
  (~45 impulsions/h × 12 h) : **+46,8 mL/jour de sur-crédit sur la pompe pH (+3,6 %)** et
  **+25,2 mL sur la pompe chlore (+1,9 %)**, sur ~1 300 mL comptés. Effet **borné** et
  de direction **fail-safe** : les limites journalières mordent plus tôt. Visible par
  l'utilisateur, annoncé au CHANGELOG.
- Un appareil neuf **sous-dose de ~8 %** jusqu'à sa propre mesure. En boucle fermée cela
  ne crée aucun biais permanent, seulement une convergence plus lente : on n'affaiblit
  pas un filet de sécurité pour compenser un défaut de réglage.
- Le repli client 61 / 67 subsiste par exactitude historique — il devra être retiré
  quand plus aucun firmware antérieur à la v2.25.2 ne sera en service.
- **Dette repérée, non traitée** : `phPumpControl` / `orpPumpControl`
  ([`config.cpp`](../../src/config.cpp)) sont **entièrement mortes** — déclarées et
  initialisées (le second membre portait le débit à 255), lues **nulle part** dans
  `src/` ni `test/`. Elles ont été mises à jour mécaniquement pour ne pas laisser
  traîner une valeur périmée, mais elles doivent disparaître.
- **Dette repérée, non traitée** : `MAX_INJECT_DURATION_S = 600` dans
  [`data/app.js`](../../data/app.js) est un **quatrième miroir de valeur** (copie de
  `kManualInjectMaxDurationS`). Candidat naturel à être servi par `/get-config`.

### Ce que ça verrouille
- **Une constante de sécurité non configurable ne peut plus être indexée sur une pompe
  physique.** Toute réintroduction d'un dispatch par index sur ce type de valeur exige un
  nouvel ADR.
- **Toute valeur d'usine reste ≥ au maximum réellement mesuré**, verrouillé par
  `static_assert` : `kPumpFlow255MlPerMin ≥ kPumpFlow255MeasuredMaxMlPerMin` et
  `kPumpTypePulseFlowMlPerMin ≥ kPumpTypeFlowMeasuredMaxMlPerMin`.
- **La marge de flotte doit rester ≥ la dispersion observée.** Toute réduction en dessous
  de l'écart inter-unités constaté (11 % à 255) est une régression de sécurité, pas une
  amélioration de précision.
- **Un seul débit majorant, trois usages.** Dédoubler la constante pour gagner en
  précision sur un chemin dégradé est explicitement rejeté ; le chemin exact reste la
  calibration à l'amplitude.
- Le champ `flow_assist.flow255` reste un **tableau de 2 éléments** (contrat client).
- Le reste de l'[ADR-0047](0047-calibration-de-type-par-defaut-et-gel-amplitude.md) —
  sémantique NVS « présent-à-0 = fail-closed », drapeaux `weighed` informatifs, gel de
  l'amplitude, `kPumpTypeCalDuty` dérivé par `static_assert` — est **inchangé**. Seules
  les **valeurs d'usine et leur justification** sont amendées.

## Références

- Code : [`src/constants.h`](../../src/constants.h) (`kPumpFlow255MlPerMin`,
  `kPumpFlow255MeasuredMaxMlPerMin`, `kPumpTypePulseFlowMlPerMin`,
  `kPumpTypeFlowMeasuredMaxMlPerMin` + `static_assert`),
  [`src/pump_controller.cpp`](../../src/pump_controller.cpp) (helpers de dispatch
  supprimés, crédit du kick, plans d'impulsion),
  [`src/web_routes_control.cpp`](../../src/web_routes_control.cpp) (majorant manuel,
  marches d'assistant), [`src/web_routes_data.cpp`](../../src/web_routes_data.cpp)
  (`flow_assist.flow255`), [`data/app.js`](../../data/app.js) (miroir supprimé, repli
  historique conservé)
- Spec : [feature-075-assistant-mesure-debit-pompe](../../specs/features/done/feature-075-assistant-mesure-debit-pompe.md)
  (mesures on-target du 2026-08-28)
- ADR liés : [ADR-0047](0047-calibration-de-type-par-defaut-et-gel-amplitude.md)
  (valeurs d'usine amendées — le reste inchangé),
  [ADR-0043](0043-dosage-proportionnel-de-temps-amplitude-calibree.md) (les « deux points
  connus » restent la doctrine ; seule la forme du second point change)
- Doc : [pump-controller.md](../subsystems/pump-controller.md), [API.md](../API.md),
  [page-settings.md](../features/page-settings.md)
