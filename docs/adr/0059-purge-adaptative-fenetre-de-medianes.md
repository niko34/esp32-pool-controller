# ADR-0059 — La purge des mesures se termine sur la STABILITÉ, jugée par une fenêtre de 10 médianes : le rinçage est un fond en V, pas une rampe — et le réglage utilisateur disparaît

- **Statut** : Superseded by [ADR-0092](0092-retrait-analyse-stabilite-porte-mesures.md) — l'analyse de stabilité (critère, re-fermeture C1, plafond C2) est retirée en v2.62.0 ; ce qui survit : le retrait du réglage utilisateur `stabilization_delay_min` (§5 ci-dessous)
- **Date** : 2026-09-04
- **Décideurs** : gate `pool-chemistry` (**GO SOUS CONDITIONS C1-C6** le 04/09 17:15, **deux ré-arbitrages** de la fenêtre les 18:40 et 19:10, **GO final option (a)** à 20:50 avec les conditions 6-11), `architect`, `test-engineer` (rejeux du CSV terrain — c'est lui qui a invalidé les deux premières fenêtres), `code-reviewer` (Approved 21:15), utilisateur (retrait du réglage)
- **Spec(s) liée(s)** : [feature-091](../../specs/features/done/feature-091-purge-adaptative-stabilisation.md) ; suite ouverte : [feature-097](../../specs/features/done/feature-097-seuil-de-stabilite-sur-donnees-terrain.md) ; journal d'arbitrage : `specs/plan-2026-09-regulation-et-robustesse.md` §6

## Contexte

### Une minuterie ne peut pas décrire un rinçage

Les sondes pH/ORP sont montées dans le circuit de filtration. Filtration arrêtée, l'eau
immobilisée devant elles dérive chimiquement (CO₂ accumulé → pH local réellement bas,
chlore consommé → ORP bas). À la reprise de la circulation, ce **bouchon d'eau morte**
défile devant les sondes avant que l'eau du bassin n'arrive : c'est la **purge**, et
jusqu'à la v2.33 sa durée était un **réglage utilisateur** (`stabilization_delay_min`).

Une durée fixe est structurellement fausse dans les deux sens — trop courte après une
longue stagnation, trop longue après une micro-coupure. Le terrain l'a démontré deux fois :

- **02/09/2026**, réglage à 5 min : la porte s'ouvre **au moment exact** où le bouchon
  atteint la sonde (bruts 6,85 → 5,60) → cascade d'alertes pH à tort. Mitigation :
  réglage porté à **15 min**.
- **04/09/2026**, réglage à 15 min : **incident identique**. 06:56:59 reprise (bruts
  pH 6,78 / ORP 583 mV) ; 07:12:00 fin de purge à pH **5,969** (delta −0,810) ; 07:13:05
  `ph_low_warning`, deux `ph_guard` (butée brute 5,78 < 6,20 ; plancher 6,28 < 6,70),
  feedforward retenu. Les gardes ont fait leur travail — **aucune injection** — mais trois
  alertes à tort, et surtout l'**intégrale pH** (440 pH·s gelés la nuit) remise à zéro puis
  chargée **négativement** (−191 pH·s) par l'erreur du bouchon. Rinçage réel observé :
  **~45-50 min**.

Deux valeurs de réglage tâtonnées, deux échecs : le paramètre demandé à l'utilisateur n'a
pas de valeur juste.

### Le piège qui a coûté deux ré-arbitrages : le rinçage est un V, pas une rampe

À la reprise, la lecture de l'eau stagnante est **déjà stable** (6,78 immobile toute la
nuit) ; le transitoire ne commence que quelques minutes plus tard. Un critère naïf
« N échantillons stables » ouvre donc la porte **avant** le plongeon.

Pire, le transitoire n'est pas monotone : c'est un **fond en V** — descente ≈ **−0,047
pH/min**, remontée ≈ **+0,043 pH/min** (relevé du 04/09, reprise 06:57, plateau vers
t+39). Sur une fenêtre de `W` médianes, une **rampe** est détectée dès que sa pente
dépasse `seuil / (W − 1)` ; mais un **V** occupe **une demi-fenêtre de chaque côté** du
fond, et la pente détectable devient :

```
pente détectable = seuil / ((W − 1) / 2)
```

Ce facteur 2 est exactement ce qui a été raté au premier arbitrage. Les rejeux du CSV réel
par le `test-engineer` l'ont établi expérimentalement, fenêtre après fenêtre :

| Fenêtre | Pente détectable | Rejeu de la courbe du 04/09 | Verdict |
|---|---|---|---|
| **W = 4** (plancher 5 min) | 0,10 pH/min | ouverture à **t+7-8**, pH ≈ **5,6** — *le fond du V* | aveugle : c'est l'incident à supprimer |
| **W = 8** (plancher 8 min) | 0,043 pH/min | ouverture à t+13 à pH **5,71** (étendue 0,145, **5 millièmes** sous le seuil), puis refermeture C1 à t+17 (0,303, 3 millièmes au-dessus) | pas une marge — invalidé |
| **W = 10** (plancher 10 min) | **0,033 pH/min** | ouverture **sur le plateau** (pH **6,88**), t+47 sans bruit, t+46-62 avec bruit | **retenu** |

## Décision

**La purge ne se termine plus sur un chronomètre mais sur la stabilité des mesures
brutes**, jugée par une **fenêtre glissante de 10 médianes par minute**, encadrée d'un
plancher et d'un plafond. **Le réglage utilisateur `stabilization_delay_min` est retiré**
et remplacé par l'affichage de la durée **mesurée** et du **motif de fin**.

### 1. Le critère de stabilité

Par voie (**pH brut** et **ORP brut**, sans offset — [ADR-0039](0039-offset-ph-applique-en-sortie-des-accesseurs.md)) :

1. les lectures EZO (cadence 5 s) sont accumulées ; une « minute » vaut
   `kPurgeSamplesPerMin` = **12 lectures valides** (NaN ignorés) — pas une minute murale ;
2. la **médiane** de chaque minute pleine entre dans un anneau de `kPurgeWindowMin` = **10** ;
3. **stable** = anneau **plein** ET étendue (max − min) ≤ `kPurgeStablePhRange` **0,15 pH**
   ET ≤ `kPurgeStableOrpRange` **15 mV** ;
4. **refermeture (C1)** = étendue > `kPurgeRecloseRangePh` **0,30 pH** ou
   `kPurgeRecloseRangeOrp` **30 mV**, soit **2 × le seuil d'ouverture** — hystérésis
   verrouillée par `static_assert` : un simple retour au bruit d'ouverture ne referme pas.

L'ORP est une voie **secondaire** : au rinçage il ne bouge que de ~1,5 mV/min, à comparer à
13,5 mV de bruit pour un seuil de 15 — il ne détecte pas seul une remontée lente, mais il
ne coûte rien et ferme les cas où le pH est plat et l'ORP non.

### 2. Pourquoi W = 10, et pourquoi ce n'est pas un réglage

10 est à la fois la valeur qui **couvre le V observé** (4,5 × 0,045 = 0,20 > 0,15) et la
**capacité de l'anneau** (`kPurgeStabilityMaxWindowMin`) : aller au-delà exigerait
d'agrandir le module pur. `kPurgeWindowMin` est borné par `static_assert` à [2 ; 10], et la
mutation 10 → 8 est tuée par **6 tests** de rejeu.

### 3. Le plancher et le plafond

- **Plancher** `kPurgeFloorMs` = **10 min** : la porte ne s'ouvre jamais avant, même si la
  fenêtre est déjà stable (une canalisation courte peut paraître stable alors que l'eau
  n'a pas été renouvelée). `static_assert` : plancher ≥ fenêtre — sinon la porte
  s'ouvrirait sur un anneau incomplet.
- **Plafond** `kPurgeCapMs` = **90 min** (condition C2 du gate) : si la stabilité n'arrive
  jamais, **la porte S'OUVRE quand même** (fail-open assumé), avec un WARNING `[STAB]`,
  l'alerte MQTT `measures_purge_timeout` et deux bannières UI ; la surveillance de
  refermeture est alors **désarmée** jusqu'à la prochaine reprise d'eau (sinon boucle
  ouvrir/fermer).

**Le sens du plafond a été tranché explicitement** : rester fermé bloquerait la régulation
**en silence** sur une sonde bruyante ; ouvrir en **signalant** laisse les gardes chimiques
habituelles (butées brutes, planchers, limites) faire leur travail. La disponibilité de la
régulation vaut mieux qu'un blocage muet, à condition que le blocage soit **dit**.

### 4. La dispense après coupure courte (C3)

Un redémarrage **logiciel** de quelques secondes pendant une filtration active ne renouvelle
pas l'eau de la conduite : re-purger 10 min serait du temps de régulation perdu (cas réel
du 03/09, PANIC, 7 s de coupure, 15 min de purge inutile). Un enregistrement en **mémoire
RTC** (`RTC_NOINIT`, empreinte FNV-1a) porte `{heure vive, eau présente, dernière dispense}`.
La dispense est accordée si — et seulement si — la mémoire est intacte, le motif de reset
est dans la liste blanche **SW / PANIC / INT_WDT / TASK_WDT**, l'heure est valide et
cohérente, la coupure vaut ≤ `kShortOutageS` **10 s**, l'eau était présente avant, et
aucune dispense n'a été accordée depuis `kOutageWaiverMinIntervalS` **3 600 s**.

**Une coupure secteur ne dispense jamais** : la mémoire RTC ne lui survit pas, la durée est
donc inconnue — et une durée inconnue vaut une longue stagnation (fail-closed).

### 5. Le réglage utilisateur disparaît — c'est la décision structurante

`stabilization_delay_min` **n'existe plus côté utilisateur** :

- retiré de `/get-config` (**7 réglages de régulation au lieu de 8**) et de la page
  Réglages ;
- la clé POST est **acceptée silencieusement** et sans effet
  ([`src/web_routes_config.cpp:850`](../../src/web_routes_config.cpp)) — un client ancien
  ou l'assistant ne provoquent pas d'erreur ;
- le champ NVS `stab_delay` reste **lu et écrit mais inerte** (même traitement que
  `screen_enabled` après feature-065) ;
- `regulation_defaults.h` n'a plus de `stabilizationDelayMin`.

Trois raisons, dans cet ordre :

1. **Le plancher a un sens physique, pas un sens de préférence.** Il mesure le
   renouvellement de l'eau dans la tuyauterie plus l'amorçage du filtre de mesure (~65 s).
   Ce n'est pas un curseur de confort : il n'y a pas d'utilisateur qui « préfère » une eau
   à moitié renouvelée.
2. **Un réglage que l'utilisateur doit tâtonner est un aveu.** 5 min → incident ;
   15 min → même incident. La bonne valeur dépend de la longueur de la conduite, du débit
   et de la durée de stagnation de la nuit : l'appareil la **mesure**, l'utilisateur ne
   peut que la deviner.
3. **Les deux erreurs de saisie sont nocives** : trop court recrée l'incident du 02/09,
   trop long retarde la régulation chaque matin.

**En remplacement, l'appareil rend compte** : la ligne « Dernière purge » (Réglages →
Régulation) affiche la durée mesurée et le motif de fin (« stabilité atteinte », « plafond
atteint, mesures non stabilisées », « dispensée — coupure brève (≤ 10 s) », « relancée, la
mesure a bougé »), et le badge des cartes pH/ORP passe du compte à rebours à
« Stabilisation depuis N min » — il n'y a plus de fin prévisible à décompter.

## Alternatives considérées

- **Garder une minuterie réglable, même mieux dimensionnée** (rejetée) — deux valeurs
  essayées sur le terrain, deux incidents. Aucune durée fixe ne décrit à la fois une
  micro-coupure de 7 s et une nuit de stagnation de 12 h.
- **W = 4** (rejetée par rejeu) — pente détectable 0,10 pH/min : aveugle au V, ouverture à
  t+7-8 à pH 5,6. C'est **exactement l'incident** que la feature devait supprimer. Retenue
  au premier arbitrage, invalidée le soir même par le test natif : c'est la démonstration
  qu'un seuil de stabilité ne se choisit pas au jugé, il se **rejoue sur la courbe réelle**.
- **W = 8** (rejetée par rejeu) — ouverture à t+13 à pH 5,71 avec **5 millièmes** de marge
  sous le seuil, suivie d'une refermeture C1 à t+17 avec 3 millièmes au-dessus. Une
  décision de sûreté qui tient à 5 millièmes sur **une** courbe n'est pas une décision.
- **Élargir le seuil à 0,20 pH** (option (b), **refusée par `pool-chemistry`**) — cela
  aurait supprimé la queue d'ouvertures tardives sur eau bruitée, mais laisse **une marge
  nulle** face au V (0,20 sur une demi-fenêtre couvre une pente de 0,044 pH/min ≈ la pente
  du rinçage). On ne desserre pas le critère qui vient d'être calé sur un incident.
- **Plafond fail-closed** (rejetée) — une sonde bruyante bloquerait la régulation
  indéfiniment **en silence** ; la disponibilité l'emporte, sous réserve d'alerte.
- **Médianes sur 2 min, ou statistique robuste** (option (c), **différée**) — c'est la piste
  préférée pour la queue d'ouvertures tardives, mais elle se tranche sur des **données
  réelles**, pas sur des rejeux bruités synthétiques : → [feature-097](../../specs/features/done/feature-097-seuil-de-stabilite-sur-donnees-terrain.md),
  décidable sur **≥ 5 matins** de télémétrie `measures_purge_last_*`, au plus tôt le
  10/09/2026.
- **Fonder la fin de purge sur un volume d'eau renouvelé calculé** (rejetée) — exigerait la
  longueur de conduite et le débit de filtration, deux grandeurs que l'utilisateur ne
  connaît pas et que l'appareil ne mesure pas. Même raison de fond qu'à
  l'[ADR-0057](0057-taille-bassin-preselection-jamais-entree-de-calcul.md) : on n'entre pas
  une géométrie dans un calcul de sûreté.

## Conséquences

### Positives

- L'ouverture se fait **sur le plateau** du rinçage (pH 6,88 au rejeu) au lieu du **fond du
  V** (pH 5,6) : plus d'alertes pH à tort au matin, et l'intégrale pH n'est plus détruite
  par l'eau de rinçage.
- La purge **s'adapte** : ~10 min après une coupure ordinaire, ~47 min un matin de rinçage
  réel, **0** après une coupure logicielle ≤ 10 s.
- Un réglage de moins à comprendre, et à la place une **information mesurée** (durée +
  motif) — plus utile que le curseur qu'elle remplace.
- Le critère vit dans un module **pur** (`purge_stability_logic`,
  [ADR-0017](0017-logique-metier-pure-humble-object-testabilite.md)) : les rejeux terrain
  sont des tests natifs, et une régression de fenêtre est tuée par mutation.
- La porte couvre désormais **tous les termes** (P, I, feedforward, gardes d'alerte) et non
  la seule injection : `SensorFilter::ready()` est faux porte fermée (C6), la branche
  Programmée attend `isCirculationSettled()` (C4).

### Négatives / dette assumée

- **Une dérive lente ≤ 0,017 pH/min est jugée stable** (0,15 pH sur les 9 intervalles de la
  fenêtre). Une sonde qui dérive très lentement passe donc pour stable — la régulation
  travaille sur la mesure filtrée et les gardes butée/plancher restent actives.
- **L'ORP seul ne détecte pas une pente ≤ 1,6 mV/min.** Voie secondaire, assumée.
- **Sur eau plate bruitée (±0,18 pH), l'étendue de 10 médianes ≈ 0,16 ≈ le seuil** :
  ouverture au plancher dans **12 tirages sur 21**, avec une **queue possible jusqu'à
  t+29**. Sur 5 tirages bruités, **1 n'ouvre pas en 60 min** — le plafond de 90 min et son
  alerte ne sont donc **pas théoriques**.
- **Coût nominal** : ≈ **10 min + 65 s** d'amorçage des filtres après une coupure longue,
  contre 15 min de minuterie fixe auparavant — mais ~47 min un matin de rinçage réel. C'est
  le prix d'une ouverture sur le plateau ; l'objectif O1 du plan passe de « ≤ 4-5 min » à
  **≈ 11 min** en conséquence directe de cet ADR.
- Le champ NVS `stab_delay` survit **inerte** : une valeur y reste lisible sans aucun effet
  (dette identique à `screen_enabled`, à purger dans une spec de ménage).
- Recommandation `pool-chemistry` n°5 (révision des défauts d'usine de délai par taille de
  bassin) **sans objet** : le réglage n'existe plus.
- ⚠️ **Non vérifié sur le terrain** à la rédaction : première validation attendue au matin
  du 05/09/2026.

### Ce que ça verrouille

- **Aucune durée de purge ne redevient un réglage utilisateur.** Si le plancher ou le
  plafond doivent changer, ce sont des constantes de flotte arbitrées par
  `pool-chemistry` — pas un champ de formulaire.
- **La fenêtre, le seuil et l'hystérésis forment un tout** : `plancher ≥ fenêtre`,
  `refermeture = 2 × ouverture`, `fenêtre ≤ capacité de l'anneau` sont verrouillés par
  `static_assert`. Aucun de ces nombres ne se modifie isolément.
- **Tout nouveau critère de fin de purge se justifie par un REJEU de courbe terrain**, pas
  par un raisonnement de pente : deux arbitrages successifs ont été démentis par le rejeu.
  Et la géométrie du transitoire à couvrir est un **V**, pas une rampe — la pente
  détectable est `seuil / ((W − 1) / 2)`.
- **Le plafond ouvre et alerte** ; le passer en fail-closed rouvrirait le gate
  `pool-chemistry`.
- **Une coupure de durée inconnue vaut une longue stagnation.** Toute extension de la
  dispense (micro-coupures de filtration sans reboot, par exemple) est une décision
  chimique, pas une optimisation.

## Amendement 2026-09-20 — l'estimateur de bloc devient la moyenne interquartile (feature-097, v2.54+)

**Ce qui change** : au §1.2, « la **médiane** de chaque minute pleine entre dans un anneau » devient « la **moyenne interquartile** de chaque minute pleine entre dans un anneau ». **Rien d'autre ne bouge** : fenêtre `W = 10`, seuils d'ouverture (0,15 pH / 15 mV), hystérésis de refermeture ×2 (0,30 pH / 30 mV), plancher 10 min, plafond 90 min, retrait du réglage utilisateur — tout est **inchangé**. Cet amendement corrige l'**estimateur qui alimente** ces seuils, pas les seuils eux-mêmes.

**Pourquoi.** Trace réelle capturée le 2026-09-20 (`test/fixtures/f097_purge_noise_20260920.csv`, 300 échantillons à 5 s, 25 min, eau établie) : le pH brut de ce bassin n'est pas du bruit autour d'une moyenne, c'est un signal **anti-corrélé, presque à deux niveaux** — autocorrélation au lag 1 = **−0,440** (0 attendu pour du bruit IID), saut moyen entre deux lectures = 1,47 σ (1,128 σ attendu), 197 changements de signe sur 299 autour de la médiane (~150 attendus). Bloc réel de la minute 22 :

```
7.051 · 6.747 · 7.083 · 6.752 · 7.083 · 6.743 · 7.085 · 6.775 · 7.079 · 6.749 · 7.079 · 7.068
```

— deux niveaux séparés de ~0,21, en alternance quasi stricte. **Sur un tel signal, la médiane n'est pas une moyenne robuste : c'est un interrupteur.** Elle bascule sur le niveau momentanément majoritaire (sept lectures hautes contre cinq → elle saute ; cinq contre sept → elle retombe). C'est cette bascule — pas une excursion réelle de l'eau — qui produisait 3 des 16 fenêtres en échec sur la trace terrain. La corrélation instantanée pH↔ORP (+0,462, l'ORP étant lui aussi anti-corrélé à −0,396) signe une perturbation de **mode commun** sur les deux électrodes, cohérente avec les courants de fuite de la pompe de filtration déjà admis sur ce projet (voir [subsystems/sensors.md](../subsystems/sensors.md#signal-ph-brut--anti-corrélation-et-mode-commun-feature-097)).

**Ce que ça corrige, mesuré à seuils et fenêtre INCHANGÉS** :

| Estimateur | σ du bloc | étendue de fenêtre min / méd / max | ouverture pH ≤ 0,15 (16 fenêtres) |
|---|---|---|---|
| médiane (ADR-0059 d'origine) | 0,0458 | 0,075 / 0,095 / 0,192 | 13/16 |
| moyenne interquartile (retenue) | 0,0263 | 0,068 / 0,074 / 0,089 | 16/16 |

Voie ORP, même chemin de code : max 11,5 → 8,7 mV — gain obtenu sans toucher un seuil.

**Rectification d'une affirmation de l'ADR d'origine.** Le §1 ci-dessus écrivait « seuils = bruit de mesure observé sur le bassin de référence ». C'était une **estimation présentée comme une mesure** : le bruit utilisé au moment de l'arbitrage du 04/09 était **synthétique**, uniforme ±0,18 pH, généré par un LCG à graine fixe (`test/test_native_purge_stability/`, section E) — il n'y avait alors aucune trace réelle de bruit d'eau établie. Le chiffre qui en découlait (« ouverture au plancher une fois sur deux ») était une propriété du bruit synthétique, pas du bassin. La première tentative de mesure réelle du 2026-09-20 était elle-même **invalide** : le script de capture interrogeait `/debug/sensor_trace?page=N` alors que la route attend `?offset=N&count=M` ([`src/web_routes_debug.cpp:33`](../../src/web_routes_debug.cpp)) — le paramètre `page` étant ignoré en silence, les six requêtes renvoyaient toutes `offset=0` : 50 échantillons uniques présentés comme 300, avec des discontinuités artificielles aux raccords. Les chiffres qui en étaient issus (étendue médiane 0,176, ORP 18,5 mV, « critère conjoint 0/16 ») sont **faux et ne doivent pas être recopiés**. La mesure corrigée, sur la trace réellement rejouée le même jour, donne **13/16 (81 %)** avec l'ancien estimateur — le seuil n'était donc pas sous le plancher de bruit, contrairement à ce que la trace invalide laissait croire.

**Planchers de bruit mesurés** (`kPurgeNoiseFloorPh` = 0,10 pH, `kPurgeNoiseFloorOrp` = 10,0 mV, `src/constants.h`) : étendue maximale d'une fenêtre de 10 statistiques de bloc (moyenne interquartile) sur eau établie, arrondie au-dessus, trace du 2026-09-20 ci-dessus. ⚠️ Une seule trace, une seule session (porte ouverte, 1 h 40 après la re-purge n°1 du même matin) : la marge retenue face au seuil d'ouverture (0,15 / 0,10 = 1,5×) est **serrée**, pas généreuse. Un `static_assert` (`kPurgeStablePhRange > kPurgeNoiseFloorPh` et son homologue ORP) rend visible à la compilation tout futur resserrement du seuil sous ce plancher — c'est l'assert qui manquait et qui aurait rendu visible dès le 04/09 que la question méritait d'être posée (même si, avec l'ancien estimateur, la réponse était non).

**Le seul sacrifice** : le point de rupture (fraction de lectures aberrantes d'un bloc tolérée sans dérive significative) recule de **50 %** (médiane) à **25 %** (moyenne interquartile, `trim = max(1, n/4)`). Mesuré sur le bloc réel avec des lectures injectées à 14,0 pH : la moyenne interquartile dérive **moins** que la médiane jusqu'à 3 lectures aberrantes sur 12, et ne décroche qu'à 4 sur 12. Arbitré explicitement par le gate `pool-chemistry` de feature-097 comme un compromis acceptable, en échange de la disparition des bascules d'interrupteur.

**Ce qui ne change pas** : le fond en V du rinçage reste détecté avec la même marge (0,0525 sur 0,03 exigé) — sur une rampe linéaire, médiane et moyenne interquartile renvoient toutes deux la valeur centrale du bloc, seul le bruit change. La re-fermeture C1 garde sa pente de déclenchement (0,0333 pH/min), avec un hystérésis **plus généreux** en pratique (l'écart entre plancher de bruit et seuil de refermeture passe de 0,108 à 0,211 avec le nouvel estimateur, sans qu'aucune constante n'ait bougé).

**Alternatives réexaminées et écartées** — voir [feature-097](../../specs/features/done/feature-097-seuil-de-stabilite-sur-donnees-terrain.md) §Options évaluées le 04/09 pour le détail chiffré : seuil élargi à 0,20 (confirmé refusé, marge sur le V nulle), médianes sur blocs de 2 min (raccourcit l'horizon et casse l'invariant de refermeture), statistique de tendance/pente (aveugle au fond du V), seuil dérivé en direct de la dispersion (boucle auto-réalisatrice), moyenne simple (point de rupture nul, écartée pour la même raison qui avait fait choisir la médiane à l'origine).

**Décideurs de l'amendement** : gate `pool-chemistry` (feature-097, arbitrage du point de rupture 50 %→25 %, révocation du GO SOUS CONDITIONS du 20/09 rendu sur un plancher de bruit surestimé du double), `test-engineer` (rejeu de la fixture terrain), `code-reviewer`.

## Références

- Code : `src/purge_stability_logic.h` / `.cpp` (détecteur pur — **supprimé en v2.62.0**,
  feature-139, [ADR-0092](0092-retrait-analyse-stabilite-porte-mesures.md) ; lien retiré, la
  cible n'existe plus),
  [`src/measure_validity_logic.h`](../../src/measure_validity_logic.h) (machine d'état,
  dispense C3), [`src/measure_validity.cpp`](../../src/measure_validity.cpp) (coquille,
  mémoire RTC, journal `[STAB]`, alerte MQTT),
  [`src/constants.h`](../../src/constants.h) (bloc `kPurge*` et `static_assert`,
  constants.h:1957-2007), [`src/sensor_filter.cpp`](../../src/sensor_filter.cpp) (C6),
  [`src/pump_controller.cpp`](../../src/pump_controller.cpp) (C4),
  [`src/web_routes_config.cpp:850`](../../src/web_routes_config.cpp) (clé POST obsolète
  acceptée sans effet), [`src/regulation_defaults.h`](../../src/regulation_defaults.h)
- Tests : `test/test_native_purge_stability/` (25 cas : rejeux 02/09 et 04/09, bruités et non
  bruités, dérives lentes, ORP en pente, démonstration de W = 8 à tort) — **suite supprimée en
  v2.62.0** (feature-139, [ADR-0092](0092-retrait-analyse-stabilite-porte-mesures.md)) ; lien
  retiré, la cible n'existe plus,
  [`test/test_native_measure_validity/`](../../test/test_native_measure_validity/)
  (plancher, deux refermetures puis plafond). Mutation W 10 → 8 tuée par 6 tests ;
  « plancher < fenêtre » tué par `static_assert`.
- Spec : [feature-091](../../specs/features/done/feature-091-purge-adaptative-stabilisation.md),
  suite [feature-097](../../specs/features/done/feature-097-seuil-de-stabilite-sur-donnees-terrain.md)
- Doc : [subsystems/measure-validity.md](../subsystems/measure-validity.md),
  [subsystems/sensors.md](../subsystems/sensors.md),
  [API.md](../API.md), [MQTT.md](../MQTT.md),
  [features/page-settings.md](../features/page-settings.md)
- ADR liés : [ADR-0041](0041-autorite-mesures-non-representatives-et-sortie-de-secours-filtre.md)
  (autorité des mesures non représentatives — la porte d'alimentation des filtres, durcie
  ici par C6), [ADR-0039](0039-offset-ph-applique-en-sortie-des-accesseurs.md) (le critère
  porte sur le **brut**), [ADR-0026](0026-mode-installation.md) (source unique de la
  présence d'eau), [ADR-0017](0017-logique-metier-pure-humble-object-testabilite.md)
  (module pur), [ADR-0060](0060-fraicheur-en-temps-de-filtration.md) (même cycle v2.34.0 :
  l'intégrale pH survit au redémarrage — c'est la porte de cet ADR qui la protège du
  transitoire de rinçage)
