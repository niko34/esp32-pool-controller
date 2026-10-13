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

## ✅ VERDICT `pool-chemistry` — GO SOUS 17 CONDITIONS (2026-10-13)

### ⚠️ DEUX PRÉMISSES DE CETTE SPEC SONT FAUSSES — corrigées ici

**A — « l'écran annonce une fenêtre qu'il n'utilise pas » est DÉJÀ CORRIGÉ.** Les trois `Δ60 s`
ont disparu avec feature-159 (v2.75.0) : `index.html:1098/1126/1637` portent le texte T1. ⛔ **Ne
pas re-corriger.**

**B — le chiffrage est un PLANCHER, pas un pire cas.** Avec l'étendue actuelle sur 20 s, le
résiduel réel au moment du vert atteint **0,95 pH à τ = 30 s** et **1,3 pH à τ = 60 s**.

### P1 — Mes constantes étaient auto-contradictoires. Tranché.

⛔ **Seuil 0,03 REFUSÉ** : il n'a **aucune source** ADR-0098, c'est un nombre choisi parce qu'il
fait passer le vecteur — *« exactement le mouvement qu'ADR-0098 interdit, et la même faute que
celle qu'on corrige »*.

⚠️ **Le garde-fou n'est pas une fenêtre, c'est une INÉGALITÉ.** La fenêtre ne garantit rien : la
séparation réelle des médianes dépend de la **cadence effective**. Sur les temps **réellement
observés** :
- `D = |médiane(valeurs récentes) − médiane(valeurs anciennes)|`
- `S` = séparation **réalisée** des temps médians · `a` = âge **réalisé** de la médiane récente
- `G = e^(a/τmax) · (e^(S/τmax) − 1)` ⇒ **résiduel `R = D/G`**

> **CRITÈRE NORMATIF** — verdict rendu seulement si `n ≥ 6` **et** `G ≥ G_MIN` ; sinon **muet**.
> `stable` ⟺ `D ≤ SEUIL`. **`G_MIN = SEUIL / RESIDUEL_MAX`** — aucun degré de liberté.

⇒ Le résiduel est borné **par construction, quelle que soit la cadence**. ⚠️ Et *« la tentation est
structurellement fermée »* : si un vecteur échoue, ce n'est plus un seuil qu'on peut assouplir,
c'est `G ≥ G_MIN` qui est violée — la seule issue est d'allonger la fenêtre ou la cadence, **toutes
deux plus strictes**.

| constante | valeur | source ADR-0098 |
|---|---|---|
| `RESIDUAL_MAX_PH` | **0,10 pH** | produit — `kPhHighMargin` |
| `THRESHOLD.ph` | **0,05 pH** | produit — `kPhGuardReleaseHysteresis` |
| `THRESHOLD.orp` | **0,05 × 59,16 mV** | physique — Nernst. ⚠️ **écrire l'expression, jamais `3.0`** |
| `RESIDUAL_MAX_ORP` | **0,10 × 59,16 mV** | physique |
| `TAU_MAX` | **120 s** (pH) / **180 s** (ORP) | matériel — ⚠️ **à confirmer sur la datasheet** |
| `G_MIN` | **0,5** | dérivé |
| `WINDOW` | **90 s** / **150 s** | **dérivé**, pas choisi |
| `DIAG_MS` | **300 s** | produit — `kStabilizationDurationPhMs`. ⛔ Pas `kStabilizationDurationOrpMs` (3 min), **plus courte que la pose normale d'une sonde redox** |

Résiduels obtenus : pH **0,0911** à τ = 120 s ✅ · ORP **4,65 mV** à τ = 180 s ✅.

### P2 — Prémisse corrigée : ce n'est pas la coupure, c'est l'absence de garde-fou

⚠️ *« C'est `G` qui porte la garantie, pas le mode de coupure. »* Une coupure au comptage **avec**
`G` est sûre (rafale : `G = 0,081 < 0,5` ⇒ muet). Une coupure par le temps **sans** `G` laisse
passer la cadence mixte. ⇒ **Les deux imposés**, ils tuent des mutants différents.
⚠️ **Honnêteté sur la reachabilité** : aucun chemin ne l'atteint **aujourd'hui** — mais la
protection est **accidentelle** (ordre de priorité d'affichage, pas statistique). Elle meurt avec
C9, avec tout chemin effaçant l'échec sans vider le tampon, et avec toute réorganisation des cinq
états. ⇒ Exigence de conception valide, **à écrire comme telle, pas comme un faux vert avéré**.
⚠️ Et `pushStabilitySample()` borne sur `Date.now()`, horloge **non monotone** : un recalage NTP
fait exploser ou vider le tampon ⇒ `performance.now()` (C4).

### P3 — Ma règle était muette sur une sonde DÉGRADÉE. Complétée.

> Vert inatteignable sur une sonde **saine** ⇒ retirer le mécanisme (ADR-0098). Vert inatteignable
> sur une sonde **dégradée** ⇒ le mécanisme a **raison**, on le garde : **on ne gonfle jamais la
> constante pour faire verdir une sonde qui dérive vraiment.** Ce qui change, c'est **la nature du
> message** — d'une consigne d'attente vers un **diagnostic** — jamais le critère.

Délai **5 min**, compté depuis le premier échantillon du tampon courant (se remet à zéro avec lui,
**aucun état nouveau**). Textes **T-D1/T-D2 imposés**, le diagnostic **remplace** T2.
⚠️ **Pas de renvoi vers une pente côté ORP** : un étalonnage ORP est à **un seul point**, il n'y a
pas de pente. **Ne pas symétriser.** Bouton toujours cliquable.

### ⚠️ LE GATE M'AMENDE : NE PAS RÉUTILISER `armStabilizationTimer()`

J'avais vérifié qu'il est borné et auto-libérant — exact, **mais insuffisant**, et deux raisons
sont dirimantes :
1. ⛔ **`clearStabilizationTimer()` est appelé à CHAQUE ARRÊT DE FILTRATION** (`filtration.cpp:539`).
   Une suspension d'étalonnage logée là serait **effacée** par une fin de plage horaire, et le
   dosage **reprendrait au redémarrage de la pompe, sondes toujours dans les béchers**. *« La
   réutilisation transformerait la correction en piège. »*
2. Un battement de cœur **raccourcirait** la stabilisation post-étalonnage existante — régression
   silencieuse.
⇒ **Minuteur SÉPARÉ** `_calHoldEndMs[2]`, OR'é dans la même garde sur le patron `flowAssistSuspend`.
**Réutiliser le PATRON, pas la VARIABLE.**

### ⚠️ T8 ET T15 DE feature-159 SONT ANNULÉS PAR CE LOT (C16)

⛔ Ne **plus** demander à l'utilisateur de mettre la régulation sur Arrêt avant d'étalonner, ni de
la remettre en Auto. *« Demander une coupure manuelle FABRIQUE L'OUBLI, et produit un état NON
BORNÉ — l'exact opposé d'une suspension auto-libérante. »* À écrire dans la spec pour que personne
ne les réintroduise de bonne foi.

### Les 17 conditions — l'essentiel

**C1** critère normatif (`D`,`S`,`a`,`G ≥ G_MIN`, `n ≥ 6`) — rend **caduques** mes trois règles de
feature-159 (« couverture ≥ 45 s / n ≥ 6 / ≥ 3 de chaque côté »), `G` les subsume · **C2** seuil
0,05 conservé, fenêtres **dérivées** · **C3** rétention + plafond d'âge dur · **C4** tampon borné
en nombre (256) + horloge **monotone** + vidage sur chemin d'ÉCHEC · **C5** ⚠️ **le nombre affiché
EST celui qui décide, DURÉE COMPRISE** — ⛔ interdit d'afficher une durée **nominale** : ce serait
« exactement le défaut `Δ60 s`/`Δ20s` reconstitué sous une autre forme » · **C6** tableau de
sources **avec l'arithmétique**, pour qu'une session future **recontrôle** au lieu de croire ·
**C7** diagnostic 5 min · **C8** ⚠️ **vérifier que le vert est ATTEIGNABLE** — si le bruit
l'empêche, seuls **allonger la fenêtre ou la cadence** sont autorisés (tous deux plus stricts) ;
⛔ **interdit de relever le seuil** · **C9** cadence garantie (ex-recommandation, **promue
bloquante** : le rejet du bruit est en `1/√n`) · **C10** journalisation au **point d'exécution
réel** ; si trop coûteux, à la route **à condition de dire « demandé »** · **C11** minuteur séparé
· **C12** ⚠️ **wrap-safe `(int32_t)(end - now) > 0`**, jamais `millis() < end` · **C13** armement à
**l'entrée**, battement **60 s**, réarmement sur `visibilitychange` et sur sortie, ⛔ **aucune
relâche explicite** (le filtre porte encore les valeurs du bécher, ~135 s de mémoire) · **C14**
⚠️ **étalonnage pH ⇒ suspendre pH ET ORP** — la boucle ORP **décide à partir de la mesure pH**
(`kPhOrpInhibit`), sonde dans un tampon ⇒ l'inhibition ne se déclenche **jamais** ; bloque aussi
l'**injection manuelle** · **C15** **plafond 30 min**, sinon un écran laissé ouvert suspend
**indéfiniment** — l'impasse reconstituée côté firmware ; ⛔ **interdit de détecter le bécher** ·
**C16** bandeaux imposés + annulation de T8/T15 · **C17** parité `/data` ↔ WS **sur les
expressions**, + ADR.

### P5 — La distinction avec ADR-0092, à ÉCRIRE (clause anti-retrait)

1. **Ce n'est pas une porte, c'est un indicateur** — il ne bloque rien, donc aucune impasse.
2. **L'objet mesuré est inversé** : ADR-0092 demandait « cette mesure est-elle représentative ? »
   (état permanent, où un détecteur d'étendue est aveugle) ; ici « l'électrode a-t-elle fini son
   transitoire dans une solution **dont je connais déjà la valeur vraie** ? » — **le transitoire
   EST le mesurande**.
3. **La statistique a changé de nature** : estimateur de **vitesse de dérive**, insensible au bruit
   et au nombre d'échantillons.
4. **Les constantes ont une source opposable**, et le résiduel garanti est **calculé et tenu par
   vecteurs**.

> **Clause anti-retrait** : *« Un futur lot qui voudrait retirer ce mécanisme au motif d'ADR-0092
> doit d'abord constater lequel des quatre points ci-dessus a cessé d'être vrai. S'il bloque
> quelque chose, c'est le blocage qu'il faut retirer, pas l'indicateur. »*

### Recommandations

**R1 — défaut latent trouvé en marge** : `isStabilizationTimerActive()` compare `millis() < end`
sans arithmétique signée ⇒ au repliement (49,7 j), une calibration réussie dans les 5 min
précédentes **n'obtient aucune suspension**. Correctif d'une ligne, mais c'est `src/` ⇒ **sa propre
ligne de spec et son mutant**. · **R2** `_flowAssistSuspend` : suspension **sans** minuteur, reste
collée — même famille, autre sens. · **R3** afficher la date du dernier étalonnage près du
diagnostic. · **R5** ⚠️ **invoquer `ux-ui-designer`** : deux ajouts de plus sur un écran qui en a
reçu trois en feature-156/159 ; mandat **non découpé**, couleur du diagnostic comprise (**ni vert
ni rouge** : ni succès ni échec).

### Portée de ce que le gate a vérifié (et pas)

**Pas vérifié** : la cadence réelle du push WebSocket (supposée 5 s) · le bruit réel en bécher
(ADR-0098 **interdit** de s'en servir comme source, mais **C8 impose de le vérifier**) · **la
datasheet Atlas elle-même** — les deux sources « matériel » (`τmax`) sont **à confirmer** ·
`web_routes_calibration.cpp` dans le détail.

## ✅ GATE 2 `pool-chemistry` — GO SOUS 3 RÉSERVES (2026-10-13)

⚠️ **C14 est RÉVOQUÉE. C13 (« 5 min ») est REMPLACÉE.** Les deux amendements de l'architecte sont
acceptés sur le principe ; une seule valeur chiffrée est rejetée.

### Amendement 1 — sur-suspendre les DEUX pompes dans les deux cas : **ACCEPTÉ**

> *« Mon C14 raisonnait chimie ; son plan raisonne chemin de fermeture. Son argument est le plus
> fort, et je révoque ma condition. »*

Dupliquer `closePhDosingSession` + pause mélange + `settlePumpAccounting` hors du bloc global pour
obtenir une granularité par pompe **achèterait une précision chimique au prix d'un risque
d'exécution** — et un second chemin de fermeture subtilement différent est un risque **déjà réalisé
dans ce dépôt** (défaut de la perte I²C).

⚠️ **Et le chiffre d'ADR-0092 l'oblige à changer d'avis** : 0,07 pH pour **une heure entière** de
dosage perdu dans 40 m³ ⇒ au plafond de 30 min, **≤ 0,035 pH**, sous le bruit de mesure de cette
installation. *« Je ne peux pas l'invoquer dans un sens et l'ignorer dans l'autre. J'aurais dû
trancher ainsi dès le gate 1. »*

**R-a (bloquante)** — C16 devient faux : le bandeau ne peut plus être conditionnel au produit.
Texte imposé, **identique dans les deux cas** :
> `La régulation pH et ORP est suspendue pendant l'étalonnage. Elle reprend d'elle-même quelques minutes après votre sortie.`

⛔ Supprimer la variante « La régulation ORP est suspendue… ». Le journal d'armement cite le produit
**étalonné** (traçabilité), **pas** le périmètre suspendu :
> `[Dosage] Étalonnage <pH|ORP> : régulation pH+ORP suspendue (bail N s, plafond 30 min)`

### Amendement 2 — garde manuelle au rang 4 : **POSITION VALIDÉE**

Vérifié dans `src/dosing_logic.cpp:386-430` (ordre gelé par `pool-chemistry` feature-006). Rang 4,
immédiatement après `StabilizationActive`, devant `AlreadyInjecting`.

⚠️ **DEUX refus doivent IMPÉRATIVEMENT rester devant — c'est LOAD-BEARING** : `WatchdogInactive`
(1) et `FiltrationOff` (2). *« Annoncer "étalonnage en cours" alors que le watchdog est mort ou que
l'eau est absente masquerait une cause PLUS GRAVE derrière une cause bénigne et auto-libérante.
L'utilisateur attendrait 9 minutes au lieu de voir le vrai défaut. »* C'est exactement la logique
du `isnan` LOAD-BEARING d'`evaluatePhTune`.
⚠️ **Et il doit rester DEVANT les quotas** (5 journalier, 6 horaire, 7 anti-cycling) : sinon le
message citerait une limite journalière là où la vraie cause est « une sonde est dans un bécher »
(ADR-0095).

**R-b (bloquante)** — commentaire LOAD-BEARING à écrire au site d'insertion + **vecteur qui épingle
l'ordre** (watchdog inactif **et** suspension active ⇒ le refus rendu est `WatchdogInactive`) +
**mutation** : permuter les rangs 1 et 4 doit **tuer** la suite.

### ⚠️ R-c (bloquante) — le bail de 195 s est REJETÉ : il ne couvre qu'UNE cinétique sur deux

Le principe de **dériver** la durée est adopté — *« meilleur que mon "5 min en dur" »* — mais les
termes sont **incomplets**. Le bail ne court pas pendant l'étalonnage : il court **après le dernier
battement**, c'est-à-dire pendant que la sonde **revient dans l'eau**. Il doit donc couvrir :
1. la **ré-équilibration de l'électrode** elle-même — `3 · τmax_ph` = **360 s** ;
2. **puis** l'oubli du bécher par le filtre — `3 · kSensorFilterEmaTauMs` = **135 s** ;
3. plus une période de battement de marge — **60 s**.

⚠️ **Pourquoi la comparaison avec les 5 min post-étalonnage ne sauve pas 195 s** : le chemin
post-`Cal` bénéficie de `resetPhFilter()` **et** d'une sonde **déjà remise en eau**. Le chemin
d'**abandon** n'a **ni l'un ni l'autre** — et c'est le cas que ce lot existe pour couvrir.

```
kCalHoldLeaseMs = 3·CAL_STAB_TAU_MAX_MS.ph + 3·kSensorFilterEmaTauMs + kCalHoldBeatMs ≈ 9 min
```
Les **trois termes et leur rôle** s'écrivent au site de déclaration : la dérivation doit être
**recontrôlable, pas crue** (ADR-0098). Bien sous le plafond de 30 min.

### τmax non confirmé sur datasheet — **ACCEPTABLE, ne bloque pas la clôture**

⚠️ **Parce que la direction est fail-safe** : `WINDOW` dérive de `τmax`, et **surestimer** `τmax`
**allonge** la fenêtre, donc **durcit** le critère. L'erreur ne peut pas produire un faux vert.
**C'est la BAISSE qui serait dangereuse.**

Deux mentions **obligatoires** dans le commentaire :
1. source déclarée = **« majoration de conception, non relevée sur datasheet »** — ⛔ surtout pas
   « matériel ». *« Une source étiquetée matériel sans l'être est pire qu'une source assumée comme
   un choix : c'est une garantie écrite plus forte que ce qu'on sait »* — le travers de
   feature-159.
2. **« Toute BAISSE de cette valeur exige un relevé fabricant. Une hausse est toujours sûre. »**

### Reste du plan : conforme, aucun autre écart

Noté favorablement : minuteur séparé sur le **patron** et non la variable (piège `filtration.cpp:539`
évité) · wrap-safe **partout, R1 compris**, « au même endroit que la nouvelle garde, là où il est
vérifiable » · **aucune route de relâche** (invariant central du volet 2) · parité garantie
**structurellement** (accesseur unique + `kSharedExpr`, axe (q)) et non par grep · expressions
jamais littéraux · deux ADR · cadence 5 s **seulement** pendant une étape active.

## ✅ GATE 3 `pool-chemistry` — le plafond ne bornait que l'ÉPISODE, pas la SÉQUENCE (2026-10-13)

**Défaut signalé par l'`embedded-developer` lui-même** après livraison du lot A, et vérifié : quand
le plafond de 30 min expire, le battement suivant ouvre simplement une **nouvelle** session avec un
**nouveau** plafond. Une tablette laissée ouverte obtient donc des suspensions **enchaînées**,
séparées par une fenêtre de dosage de **moins d'une minute**. Sur une heure, la régulation tourne
~1 min.

⛔ **Le gate REJETTE « accepter en l'état »**, qui était mon inclination :
> *« Mon C15 visait l'impasse, pas sa périodicité. 48 suspensions enchaînées sur une journée, avec
> ~1 min de régulation par heure, EST l'impasse — et le chiffre d'ADR-0092 ne me couvre pas ici :
> 0,07 pH vaut pour UNE heure perdue, pas pour une saison. Un `warning` toutes les 30 min n'est pas
> un garde-fou, c'est un bruit qu'on apprend à ignorer — exactement le travers que j'ai moi-même
> invoqué contre l'orange perpétuel. »*

**(b) RÈGLE PRINCIPALE — deux intentions sur la route.** `action:"open"` émis **uniquement** par
`onEnter` (geste humain) : seul à pouvoir ouvrir une session. `action:"beat"` ne peut que
**prolonger**. ⚠️ Un `beat` sans session active est **IGNORÉ** (200, aucun armement) — jamais un
refus dur, jamais de route de relâche. ⛔ **Le battement ne doit JAMAIS se replier sur `open`.**

**(c) GARDE-FOU — période réfractaire**, *« parce que (b) seul fait confiance au client »*. Après
une expiration **par plafond** (⚠️ pas après un bail ordinaire — la distinction compte), tout
`open` est ignoré pendant `kCalHoldRefractoryMs = kCalHoldLeaseMs` (≈ 9 min) : *« le dosage doit
disposer d'au moins l'équivalent d'un bail pour agir »*. Bandeau :
> `La suspension a atteint sa durée maximale. Un nouvel étalonnage sera possible dans N min.`

## ✅ GATE 4 — textes exacts, et la détection paresseuse refusée (2026-10-13)

**[1] Le texte du 1ᵉʳ avertissement n'avait jamais été donné.** Le développeur a écrit le sien et
l'a signalé ; ⛔ **refusé** : *« "rétablie automatiquement" décrit une normalité, alors que
l'événement est une REPRISE DE DOSAGE SANS SAVOIR OÙ SONT LES SONDES. C'est cette clause qui porte
le danger. »* Textes **exacts** :

```
[Dosage] Plafond d'étalonnage atteint (30 min) — régulation rétablie SANS confirmation que les sondes sont dans l'eau
[Dosage] 2e plafond d'étalonnage consécutif — écran probablement laissé ouvert, régulation quasi continûment suspendue
[Dosage] <N>e plafond d'étalonnage consécutif — écran probablement laissé ouvert, régulation quasi continûment suspendue
```
Doublements seulement (2, 4, 8, 16…) : *« un journal qui se répète à l'identique n'informe plus »*.

**[2] ⚠️ DÉTECTION PARESSEUSE REFUSÉE.** L'escalade était évaluée **au prochain POST**. Or si
l'écran atteint le plafond **puis s'arrête** (onglet fermé, téléphone verrouillé), **aucun POST ne
suit et l'expiration n'est JAMAIS tracée** — alors que c'est précisément le cas que C15 voulait
rendre visible, le seul où plus personne ne regarde le bandeau.
> *« Une alerte qui ne se déclenche que si quelqu'un redemande quelque chose n'est pas une
> alerte. »*

⛔ Le compromis « warning dans la boucle, escalade au POST » est **explicitement rejeté** :
*« scinder l'émission entre deux horloges donnerait un journal dont les horodatages ne se lisent
plus ensemble »*.
⚠️ **Et l'objection de coût ne tient pas** — vérifié : le patron est **déjà dans le fichier, dix
lignes au-dessus de la garde** (`_flowAssistSuspendPrev`, `pump_controller.cpp:3566-3569`). C'est
**une comparaison par tour de boucle**. ⛔ ADR-0082 : chaînes statiques, `snprintf` sur pile,
aucune allocation.

⚠️ **Dette de consignation assumée** : les gates 3 et 4 ont été rendus par messages et n'étaient
consignés **nulle part** — relevé en revue de code, corrigé ici. Les textes imposés doivent être
**recontrôlables dans la spec**, pas seulement dans un fil de conversation.

## ⚠️ TEXTES T-D1 / T-D2 — VERBATIM (dette de consignation, corrigée le 2026-10-13)

**Ces textes étaient RÉSUMÉS et non recopiés** dans la consignation du gate 1 ci-dessus. Le
`web-ui-developer` a donc dû en **rédiger de plausibles**, et l'a signalé. C'est la **deuxième
fois** dans ce lot qu'un texte imposé manque à la spec (les gates 3 et 4 l'ont été aussi).
⇒ **Un texte imposé se RECOPIE, il ne se résume jamais** : résumé, il cesse d'être opposable.

**T-D1 (pH)** — deux lignes dans le même bloc :
```
Toujours instable après 5 min (écart 0,12 pH sur 45 s).
Vous pouvez calibrer quand même. Une dérive qui dure est aussi un signe d'usure de la sonde : vérifiez la pente dans « État sonde pH » après l'étalonnage.
```

**T-D2 (ORP)** — deux lignes dans le même bloc :
```
Toujours instable après 5 min (écart 4 mV sur 75 s).
Vous pouvez calibrer quand même. Une dérive qui dure est aussi un signe d'usure de la sonde redox.
```

⚠️ **Le marqueur typographique n'est PAS dans ces textes** : le gate l'a explicitement laissé à
`ux-ui-designer`, qui a tranché **⚠** pour le diagnostic (ni vert ni rouge : *« l'ambre est la
seule teinte du jeu existant qui ne porte ni succès ni échec ni action-en-cours »*). ⛔ Ne pas
reprendre le `○` qui figurait dans la rédaction du gate — il est **réservé à l'état instable**.

⚠️ `0,12 pH`, `45 s`, `4 mV`, `75 s` sont des **exemples de format** : ce sont `D` et `S`
**réalisés** (condition C5), **jamais des littéraux**.
⚠️ **Pas de renvoi vers une pente côté ORP** — un étalonnage ORP est à **un seul point**, il n'y a
pas de pente. ⛔ **Ne pas symétriser les deux textes.**

## C8 — ATTEIGNABILITÉ DU VERT : mesure RÉELLE rejouée (2026-10-13)

Le gate avait rendu cette vérification **bloquante** : *« constater qu'une sonde saine atteint le
vert en ~2 min »*, et surtout — *« si le vert n'arrive pas à CAUSE DU BRUIT, les seules
corrections autorisées sont allonger la fenêtre ou augmenter la cadence. ⛔ Interdit de relever le
seuil. »* La crainte était que la statistique de **dérive**, avec un seuil 18 fois plus serré
qu'avant (0,05 contre 0,9), ne passe **jamais** au vert.

### Protocole

Capture **réelle** sur l'appareil de production (`GET /data`, champ `phValueRaw`, cadence 5 s,
212 s, **37 points valides** sur 40 tentatives). Signal **rejoué à travers la fonction EXTRAITE**
de `data/app.js` (ADR-0099 : le code livré, jamais une recopie), avec les ancres exactes du
harnais officiel.

Conditions relevées : filtration **en marche depuis 13 455 s**, `measures_valid = true`, pH brut
autour de **7,08**, **étendue 0,064** sur toute la capture.

### Résultat

| n | t | état | D | G |
|---|---|---|---|---|
| 6 | 25 s | muet | 0,0030 | 0,140 |
| 12 | 59 s | muet | 0,0145 | 0,356 |
| 18 | 98 s | **stable** | 0,0240 | 0,728 |
| 24 | 128 s | stable | 0,0030 | 1,049 |
| 37 | 212 s | stable | 0,0130 | 2,441 |

> ✅ **VERT ATTEINT À t = 83 s**, avec `D = 0,024` — moins de la moitié du seuil de 0,05.

⇒ **La crainte du « vert inatteignable à cause du bruit » est écartée.** Le silence des
83 premières secondes ne vient pas du bruit mais de `G < G_MIN` : **la fenêtre se remplit**, ce
qui est exactement le comportement voulu — refuser de conclure avant d'avoir de quoi conclure.
Et le résultat est cohérent avec la cible du gate (« ~2 min »).

### ⚠️ PORTÉE DE CETTE MESURE — ce qu'elle prouve, et ce qu'elle NE prouve PAS

**Elle prouve** que le bruit de mesure de cette installation, **pompe en marche**, ne suffit pas à
empêcher le verdict : c'était la question ouverte de C8, et elle est tranchée.

⛔ **Elle ne prouve PAS** l'atteignabilité en conditions d'étalonnage :
- la sonde est **dans le bassin**, pas dans un tampon ⇒ **aucun transitoire** n'est rejoué. On
  mesure un régime **établi**, pas la convergence après changement de milieu ;
- la sonde de cette installation est **dégradée** (zéro EZO à 128 mV) ⇒ elle ne peut pas servir de
  preuve pour une sonde saine, le gate l'avait écrit ;
- ⚠️ **et inversement, elle n'infirme pas P3** : la crainte de l'« orange perpétuel » portait sur
  une **dérive en solution tampon**, régime différent. Le fait que cette sonde se stabilise **dans
  l'eau du bassin** ne dit rien de son comportement **dans un tampon**.

**Reste donc à faire, et cela exige un geste humain** : une capture pendant une **immersion réelle
en tampon**, rejouée de la même façon. C'est le seul protocole qui tranche à la fois
l'atteignabilité en conditions d'étalonnage et le cas P3 de cette sonde.
Script de rejeu conservé : `scratchpad/replay.js` (ancres du harnais officiel, extraction ADR-0099).

## 🔄 REFONTE DU VOLET 2 — décision utilisateur du 2026-10-13

> « *Encore des recettes complexes. Il suffirait de passer le dosage en manuel quand on clique
> sur Calibrer et l'indiquer comme un message d'alerte en haut de l'écran de calibration […].
> On peut aussi modifier le bouton "Fermer" pour mettre "Fermer et remettre la régulation en mode
> Auto". C'est simple et efficace. Pas d'histoire de débit ou de timer, etc…* »

**Décision : le volet 2 est REFAIT sur cette base. Tout le mécanisme de bail est ABANDONNÉ.**

### ⚠️ Le mécanisme existe DÉJÀ — ce n'est pas une invention, c'est une réutilisation

`config.h:136-148` : depuis **feature-067**, l'appareil sait **basculer la régulation en `manual`
de lui-même** et consigner la cause, **en français**, avec sa date (`phAutoManualEpoch` /
`phAutoManualReason`, miroirs ORP). Il le fait déjà sur escalade de refus de plancher acide.
Et le commentaire dit pourquoi ce mécanisme a été créé : lors de l'incident du **2026-08-16**, le
mode était sur `manual` et **rien ne permettait de dire si c'était l'appareil ou un humain**.

⇒ Les traces `*AutoManualEpoch` / `*AutoManualReason` sont **exactement** ce qu'il faut ici, et
elles sont déjà persistées, déjà effacées quand l'utilisateur reprend la main.

### Ce qui est ABANDONNÉ (et pourquoi c'est un gain, pas une perte)

⛔ `cal_hold_logic.{h,cpp}`, `_calHold*`, `_calHoldMux`, `POST /calibration/hold`, le battement de
cœur, le plafond de 30 min, la période réfractaire, les trois clés de parité, les trois textes de
journal du gate 4, `DoseRefusal::CalibrationHold`, le rang 4 d'`evaluateManualInject`.

⚠️ **Les DEUX défauts bloquants de la revue n'existaient QUE à cause de ce mécanisme** :
le repliement d'horloge sur la période réfractaire (fail-open de 24,8 jours) et les deux écrivains
concurrents (un étalonnage entier sans suspension). **Un mécanisme invisible et temporisé fabrique
ses propres défauts.**

### Le compromis, assumé et explicite

| | bail auto-expirant (abandonné) | bascule en Manuel (retenu) |
|---|---|---|
| reprise | automatique | **demande une action humaine** |
| si l'écran meurt | reprend seul | reste en Manuel |
| **visibilité d'un échec** | ⚠️ **AUCUNE** — minuterie coincée, invisible | ✅ **le mode Manuel s'affiche au tableau de bord** |
| sens de l'erreur | dosage reprend sans savoir où sont les sondes | dosage arrêté |

> **On échange un oubli qu'on VOIT contre une panne qu'on ne voit PAS.** C'est le bon sens de
> l'échange : un défaut visible se corrige, un défaut silencieux dure.

### Ce qu'il reste à faire

1. **À l'entrée** dans le mode réglage : basculer **pH ET ORP** en `manual` (le périmètre global
   reste celui du gate 2 — la boucle ORP décide à partir de la mesure pH), en posant
   `*AutoManualEpoch` / `*AutoManualReason` avec une cause en clair.
2. **Bandeau en haut de l'écran** (texte proposé par l'utilisateur, à faire viser par le gate) :
   > « La régulation a été configurée en Manuel pour vous permettre de calibrer la sonde en toute
   > sécurité. Pensez à remettre la régulation en Auto si cela n'a pas été fait automatiquement à
   > la fin de la calibration. »
3. **Le bouton « Fermer » devient « Fermer et remettre la régulation en mode Auto »** et le fait.
   ⚠️ **Note** : `ux-ui-designer` proposait de SUPPRIMER ce bouton comme redondant avec les liens
   de retour. Cette décision le **rend au contraire porteur d'une action** — la suppression est
   caduque, et les liens de retour doivent faire la même chose.
4. ⚠️ **Fermer le risque d'oubli** : alerte si la régulation traîne en `manual` (recommandation
   R2 du gate 1, qui prend ici tout son sens puisqu'on fabrique délibérément des bascules).

⚠️ **À FAIRE VISER PAR `pool-chemistry`** : son gate 1 avait **interdit** de *demander à
l'utilisateur* de couper la régulation (« demander une coupure manuelle FABRIQUE L'OUBLI »). Ici
c'est **l'appareil** qui coupe et le bouton qui restaure — ce n'est pas la même chose, mais c'est
son périmètre et la décision lui revient.
⚠️ **Le volet 1 (l'indicateur de stabilité) n'est PAS touché** par cette refonte : il est livré,
revu, et sa preuve d'atteignabilité est mesurée.

## ✅ GATE 5 `pool-chemistry` — REFONTE : GO SOUS 8 CONDITIONS (2026-10-13)

**C16 du gate 1 est LEVÉE.** *« Mon objection visait l'acte DEMANDÉ à l'humain AVANT : il peut
être oublié, et l'oubli ouvre le dosage pendant que les sondes sont dans un tampon. Ici l'appareil
coupe lui-même : l'état dangereux n'est plus atteignable par oubli. L'oubli restant porte sur la
REPRISE, dont le sens d'erreur est dosage ARRÊTÉ — et il est VISIBLE. L'échange est bon. Le bail
n'est pas regretté : il échouait EN SILENCE DU MAUVAIS CÔTÉ. »*

### ⚠️ UNE AFFIRMATION DE CETTE SPEC ÉTAIT FAUSSE (C1)

J'avais écrit « `phAutoManualEpoch` / `phAutoManualReason`, **miroirs ORP** ». ⛔ **Il n'y a AUCUN
miroir ORP** — `grep -r "orp_am_\|orpAutoManual" src/` ⇒ zéro résultat. L'ORP exige donc des champs
neufs. **Vérifier avant d'affirmer, même sur un détail d'infrastructure.**

### ⚠️ C2 — NE JAMAIS RÉUTILISER `phAutoManual*` POUR L'ÉTALONNAGE

C'est le piège que la refonte allait créer, et il est grave. `web_routes_config.cpp:518-566` est un
**gate fail-closed** (incident du 2026-08-25) : tant qu'une trace `phAutoManual*` existe, sortir de
`manual` **exige** `ph_auto_manual_ack: true`. Donc :
- **(a)** le bouton « rétablir » prendrait un **409** ;
- **(b)** s'il envoyait l'acquittement, il **effacerait une escalade de plancher acide RÉELLE**
  (feature-067) — ⛔ **un nouveau trou de sécurité**, créé par la simplification.

⇒ **Trace d'étalonnage DISTINCTE** (champs et clés NVS propres). Le chemin de fermeture n'acquitte
**jamais** `phAutoManual*`.
⇒ **C3** : si `phAutoManual*` est **actif** à l'entrée, ⛔ **ne pas toucher au mode pH** (il est
déjà `manual` sous verrou de sécurité) et la fermeture **ne le rétablit pas**.

### ⚠️ C6 — LE SEUL TROU QUE LE BAIL COUVRAIT ET QUE LA BASCULE NE COUVRE PAS

**Changer de mode n'arrête pas une injection DÉJÀ LANCÉE.** ⇒ à l'entrée, **interrompre** tout
dosage pH/ORP actif (log `info` + cause). *« Le rang 4 d'`evaluateManualInject` disparaissant,
c'est le seul filet. »*

### Les autres conditions

**C4 — R2 devient BLOQUANTE** (on fabrique délibérément des bascules) : alerte **2 h** après une
bascule d'étalonnage non rétablie, répétée **toutes les 6 h**, `warning` + bandeau + MQTT :
> `Régulation pH et chlore en Manuel depuis <durée> (étalonnage du <date>) — aucun dosage automatique. Rétablissez-la.`

**C5 — mémoriser le mode ANTÉRIEUR de chaque boucle** et restaurer **celui-là**, ⛔ jamais
`automatic` en dur (`scheduled` doit revenir en `scheduled`). Libellé **dynamique** :
« **Fermer et rétablir la régulation** », ou simplement « **Fermer** » si aucune bascule n'a eu
lieu. Même action sur les **liens de retour**.

**C7** bascule et restauration sous `configMutex`, persistées, journalisées `info` avec
l'utilisateur authentifié · **C8** état exposé en **parité `/data` ↔ WS** sur les **expressions**.

### Texte du bandeau — AMENDÉ (le vôtre affirmait un automatisme dont l'appareil ne sait rien)

> « La régulation pH et chlore a été mise en **Manuel** : aucun dosage pendant l'étalonnage. Elle
> **ne repartira pas toute seule**. Utilisez le bouton ci-dessous pour la rétablir. »

**Périmètre** : pH **et** ORP, cible `manual` pour les deux. ⛔ Pas `scheduled` — *« il dose au
volume, sans sonde »*.

## Notes de clôture (2026-10-13)

### ⚠️ LE VOLET 2 A ÉTÉ CONSTRUIT DEUX FOIS — et la seconde version est de l'utilisateur

J'ai d'abord bâti un **bail temporisé** : minuterie firmware, battement de cœur toutes les 60 s,
plafond de 30 min, période réfractaire, route dédiée, trois clés de parité, cinq gates successifs.
⚠️ **Les DEUX défauts bloquants trouvés en revue n'existaient QUE dans cette mécanique** — un
repliement d'horloge ouvrant un fail-open de 24,8 jours, et deux écrivains concurrents pouvant
laisser passer **un étalonnage entier sans suspension**.

L'utilisateur a tranché : *« Encore des recettes complexes. Il suffirait de passer le dosage en
manuel quand on clique sur Calibrer […]. C'est simple et efficace. Pas d'histoire de débit ou de
timer, etc… »*

⇒ **Tout le bail a été supprimé.** Le gate l'a résumé mieux que moi : *« il échouait EN SILENCE DU
MAUVAIS CÔTÉ »*.

**LEÇON, et elle dépasse ce lot** : j'ai inventé une minuterie **invisible** là où l'appareil
possédait déjà un état **visible et persisté** — le mode de régulation, affiché au tableau de bord.
⚠️ **Un mécanisme qu'on ne voit pas fabrique ses propres pannes.** Avant d'ajouter un état caché,
chercher si une surface de contrôle existante porte déjà l'information.

### Ce que les gates ont trouvé et que je n'avais pas vu

- ⛔ **C2 — le piège que la simplification allait créer** : réutiliser `phAutoManual*` aurait fait
  prendre un **409** au bouton de rétablissement, ou pire — s'il envoyait l'acquittement, il aurait
  **effacé une escalade de plancher acide RÉELLE** (feature-067). Trace **distincte** imposée.
- ⚠️ **C1 — j'avais écrit une FAUSSETÉ** : « `phAutoManual*`, miroirs ORP ». Il n'y a **aucun**
  miroir ORP (`grep` ⇒ zéro). Énoncé comme un fait d'infrastructure, jamais vérifié.
- **C6 — le seul trou que le bail couvrait et pas la bascule** : changer de mode **n'arrête pas
  une injection déjà lancée**. L'entrée interrompt désormais le dosage en cours.
- **C5** — restauration du **mode antérieur mémorisé**, jamais `automatic` en dur.

### ⚠️ Un dépassement de trame WebSocket, trouvé par hasard

Le décompte de budget de `ws_manager.cpp` — tenu commentaire par commentaire depuis des mois —
**avait décroché pendant QUATRE lots** (feature-156 à bug-160). Sept champs ajoutés, **213 o**,
jamais comptés, pour une marge de 208. Pire cas **3 589 o contre un plafond de 3 584**.
⚠️ **Un dépassement ne tronque pas : il fait SAUTER le push.** Le temps réel aurait cessé **en
silence**. `kWsFrameBufBytes` porté à **4096** (+512 o de `.bss`, RAM 29,0 → 29,1 %).
**Ce décompte ne tient que par discipline, et la discipline a lâché.**

### Un vecteur de test qui confondait deux horloges

Le vecteur de priorité du diagnostic échouait. Diagnostic : il vieillissait la **session** pour
déclencher le diagnostic, mais vieillissait **aussi les échantillons** — 900 s, au-delà du plafond
d'âge dur de 360 s. Tous purgés ⇒ aucun écart calculable ⇒ la garde supprimait le diagnostic,
**à juste titre**. ⇒ **Le code avait raison, le vecteur était faux.** Corrigé côté vecteur.

### Une suggestion de revue REFUSÉE, avec son motif

La revue demandait d'encadrer une écriture de configuration par `configMutex`, jugée « gratuite ».
⛔ Refusé : **ce fichier ne prend jamais ce verrou directement** (convention explicite), la
persistance le prend **elle-même**, et la course résiduelle produit au pire une **alerte fantôme
— jamais une alerte manquée**. Le remède coûtait plus cher que le défaut. Motif écrit dans le code.

### C8 — atteignabilité du vert : MESURÉE

Capture **réelle** sur l'appareil (37 points, 212 s), rejouée à travers la fonction **extraite** :
**vert à t = 83 s**, `D = 0,024` — moins de la moitié du seuil. La crainte du « vert inatteignable
à cause du bruit » est écartée.
⚠️ **Portée** : sonde **dans le bassin**, pas dans un tampon ⇒ aucun transitoire rejoué ; et la
sonde est **dégradée**. Cela ne prouve **pas** l'atteignabilité en conditions d'étalonnage, et
**n'infirme pas P3**. Reste à faire, et cela exige un geste humain : une capture pendant une
**immersion réelle en tampon**.

### Validation finale (mesurée, ADR-0073)

`pio run` SUCCESS zéro warning neuf · **70 suites / 2 152 cas** · **17** contrôles d'hygiène ·
`build_fs.sh` dans les 512 Ko · harnais de stabilité vert · aucune référence résiduelle au bail
dans `data/`.

⚠️ **CAPTURES À REFAIRE (seule étape humaine)** : `screenshots/07_pH_Calibration.jpg` et
`08_ORP_Calibration.jpg` — bandeau **ambre** et bouton « Fermer et rétablir la régulation ».
