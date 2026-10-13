## Métadonnées
- **ID** : bug-160
- **Slug** : l-indicateur-de-stabilite-ment-et-le-dosage-ne-se-suspend-pas
- **Type** : bug
- **Statut** : todo
- **Priorité** : **haute** — deux défauts de sécurité chimique **indépendants l'un de l'autre**,
  tous deux trouvés en marge d'une demande d'ergonomie (feature-159)
- **Version cible** : à définir
- **Dernière mise à jour** : 2026-10-12
- **Spec(s) liée(s)** : feature-159 (le lot d'ergonomie d'où ces constats sont issus),
  feature-034 (origine des seuils), feature-154 (bande de santé de la pente)

## Domaines touchés
- [x] Capteurs (pH / ORP / température) — calibration
- [x] Régulation (pump_controller, PID) → **passage obligatoire par `pool-chemistry`**
- [x] Web — routes HTTP (journalisation optionnelle)
- [x] Frontend (`data/`)

## Agents requis
- [x] spec-reader · [x] architect · [x] **pool-chemistry** · [x] embedded-developer
- [x] web-ui-developer · [x] test-engineer · [x] code-reviewer · [x] doc-writer
- [ ] ux-ui-designer — **à confirmer** : le volet 1 change un affichage, le volet 2 un texte.

## Description

Deux défauts trouvés par le gate `pool-chemistry` le 2026-10-12, **en marge** de feature-159.
Aucun des deux n'a été demandé par l'utilisateur ; les traiter dans son lot d'ergonomie les
aurait noyés.

## Contexte

### ⚠️ DÉFAUT 1 — « Stable — vous pouvez calibrer » est annoncé à 0,8-1,1 pH de l'équilibre

`CAL_STAB_THRESHOLD.ph` = **0,9 pH** sur une fenêtre `CAL_STAB_WINDOW_MS` de **20 s**. Le
commentaire du code justifie ce seuil par « *un bruit de lecture ~0,07 pH* » : **il vaut 13 fois
ce bruit**. Chiffré par le gate (premier ordre, `résiduel = étendue_W / (e^(W/τ) − 1)`), passage
du tampon 7,00 au 4,00 :

| τ | « Stable » annoncé à | écart RÉEL |
|---|---|---|
| 30 s (sonde saine) | t ≈ 40 s | **0,79 pH** |
| 60 s (vieillissante) | t ≈ 60 s | **1,10 pH** |

⚠️ **L'indicateur ne se contente pas de ne pas freiner : il AUTORISE un étalonnage faux de près
d'une unité pH.** La décision « non bloquant » de feature-034 n'est pas en cause — **la valeur
est fausse au regard de sa propre justification écrite**.

**Et l'écran annonce une fenêtre qu'il n'utilise pas** : le HTML affiche `Δ60 s` à l'état initial
(3 emplacements), le JS affiche ensuite `Δ20s`.

**Chaîne de conséquences, point MILIEU étalonné trop tôt** : zéro décalé de −0,79 ⇒ la boucle
acide **s'arrête** ⇒ le pH vrai monte ⇒ `kPhOrpInhibit` (7,60) se compare à la mesure
**falsifiée** ⇒ **l'inhibition ORP ne se déclenche jamais** ⇒ à pH vrai 8,0 la part active du
chlore tombe à ~24 % ⇒ la boucle ORP conclut au manque et **surdose**. ⚠️ **C'est l'aggravation
exacte de l'épisode du 10/10, fabriquée par un clic trop rapide.** Point BAS trop tôt ⇒ pente
≈ **74 %**, hors de la bande 90-110 % que l'écran « État sonde pH » affiche **déjà** sans que
personne ne fasse le lien. Et un biais d'étalonnage constant est **indiscernable du droop** d'un
P pur dans les journaux — la confusion du 28/08.

### ⚠️ DÉFAUT 2 — rien ne suspend le dosage pendant une calibration

`armStabilizationTimer()` n'est armé qu'**APRÈS** un `Cal` réussi (`sensors.cpp:847`, `:870`,
`:896`). Pendant que les sondes trempent dans les béchers, **l'appareil continue de réguler sur ce
qu'elles lisent**. Côté ORP : étalon à 470 mV contre consigne 530 ⇒ erreur de 60 mV sur 200 de
pleine échelle ⇒ **il peut injecter du chlore dans le bassin en se fondant sur la lecture d'un
bécher**.

⚠️ **Et corriger le défaut 1 AGGRAVE celui-ci** : allonger la fenêtre de stabilité allonge
d'autant la durée pendant laquelle les sondes sont hors de l'eau (20 s → ≥ 60 s par point, 2-3 min
côté ORP). Les deux volets sont donc **liés par le risque**, même s'ils sont indépendants par le
code.

## Comportement attendu
- L'indicateur n'annonce « stable » que lorsque l'écart résiduel est **du même ordre que la
  précision que le projet sait exploiter**.
- Le dosage **ne décide plus** sur une sonde qui n'est pas dans l'eau du bassin.

## Contraintes techniques
- ⚠️ **ADR-0098** : chaque constante déclare sa source (physique / décision produit / matériel).
  ⛔ **Interdit** de dériver un seuil d'une mesure de bruit sur cette installation.
  Valeurs proposées par le gate : fenêtre **60 000 ms** (matériel : Atlas, 1-2 min avant `Cal`),
  seuil pH **0,05** (décision produit : `kPhGuardReleaseHysteresis`), seuil ORP **3 mV**
  (physique : 0,05 × 59,16 mV/pH ≈ 2,96).
- ⚠️ **LA STATISTIQUE DOIT CHANGER, PAS SEULEMENT LE NOMBRE.** `max − min` est une **étendue** :
  elle croît avec le nombre d'échantillons et mélange bruit et dérive. Avec 12 points, 0,05 ne
  serait **jamais atteint** ⇒ indicateur éternellement orange ⇒ **on apprend à l'ignorer**, le
  pire des résultats. ⇒ **dérive médiane moitié/moitié** :
  `D = |médiane(moitié récente) − médiane(moitié ancienne)|`.
- **Rétention robuste à la cadence** (5 s avec WebSocket, **30 s sans**) : écarter > 60 s **sauf**
  s'il reste moins de 6 échantillons ; verdict seulement si couverture ≥ 45 s **et** n ≥ 6 **et**
  ≥ 3 de chaque côté ; sinon **muet**. Sans WS : plus tard et plus conservateur, **jamais
  bloquant**.
- ⚠️ **Si le vert s'avérait inatteignable sur une sonde saine : NE PAS gonfler la constante** —
  ADR-0098, « c'est le mécanisme qu'il faut retirer, pas la constante qu'il faut ajuster ».
- ⛔ **Le bouton n'est JAMAIS `disabled` pour cause d'instabilité** (verdict du gate, feature-159).
  Une sonde dégradée dérive en permanence et ne satisfera jamais un critère serré : un bouton mort
  la condamnerait. C'est l'impasse vécue les 10 et 11/10.
- ⚠️ **Volet 2 — la suspension doit être BORNÉE PAR UN MINUTEUR FIRMWARE** qui la lève tout seul.
  ⛔ Jamais dépendante de la survie du navigateur : *« une suspension sans minuteur de relâche
  serait une régression de sécurité, pas une amélioration »* — elle resterait collée si l'onglet
  meurt.
- Journalisation (volet 1) : `POST /calibrate_ph|_orp` acceptent deux champs **optionnels**
  (`stable`, `drift`) ; absents ⇒ comportement **strictement inchangé** ; `drift` hors bornes ⇒
  **ignoré, jamais un refus** — une sécurité de journalisation ne doit pas pouvoir empêcher un
  étalonnage.

## Sécurité chimique
- **Dosage impliqué : OUI, dans les deux volets**, et dans la direction dangereuse.
- **Questions au gate** : (a) confirmer les trois valeurs et la statistique ; (b) la suspension
  doit-elle couvrir l'entrée en mode réglage ou seulement une étape de lecture active ? (c) quelle
  durée pour le minuteur de relâche ? (d) faut-il signaler à l'utilisateur que la suspension est
  active, et que devient-elle s'il quitte l'écran ?

## ADR attendu ?
- **Possible** : la règle « une commande ne se désactive pas pour un état que l'appareil ne mesure
  que de façon indicative — elle informe » tranche un arbitrage **récurrent** (feature-155, le
  correctif du 11/10, feature-159).

## Éléments visibles (pour doc-writer)
- `docs/features/page-ph.md`, `CHANGELOG.md`, `docs/subsystems/sensors.md`,
  `docs/user/src/guide-utilisation.html` si le texte visible change.

## Critères d'acceptation
- [ ] Sur une rampe synthétique `pH(t) = 4 + 3·e^(−t/τ)` pour τ ∈ {10, 30, 60, 120} s, le verdict
      « stable » n'arrive **jamais** tant que le résiduel dépasse 0,10 pH.
      ⚠️ **À exécuter D'ABORD sur le code existant** pour matérialiser le défaut (il échoue
      aujourd'hui à τ = 30 s et τ = 60 s).
- [ ] Cadence dégradée (30 s, WebSocket coupé) : le verdict finit par être rendu — **jamais**
      d'état muet permanent.
- [ ] Signal purement bruité sans dérive ⇒ « stable » (là où `max − min` disait « instable »).
- [ ] **Mutation** : remettre 0,9 / 20 s doit **tuer** la suite. ⚠️ C'est le mutant qui survit
      depuis feature-034 ; s'il survit encore, la suite ne protège rien.
- [ ] Volet 2 : le dosage ne décide plus sur une sonde hors de l'eau, et la suspension **se lève
      toute seule** même si le navigateur est tué.

## Hors périmètre
- L'ergonomie de l'écran de calibration (c'est feature-159).
- Les **textes imposés** par les gates précédents.
