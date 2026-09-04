# ADR-0060 — La fraîcheur d'une grandeur qui ne vit qu'en filtration se mesure en TEMPS DE FILTRATION — et tant qu'une candidate attend sa confirmation, la NVS a raison, pas la RAM

- **Statut** : Accepté — **amende R3 (péremption) et R5 (cadence d'écriture) de l'[ADR-0053](0053-persistance-conditionnelle-integrale-ph.md)**. R1 (plafond chimique), R2 (scellement et contexte), R4 (confirmation par la mesure), R6 (journalisation) et R7 (anti-boucle) sont **inchangés**, ainsi que tout l'argument de sûreté de l'ADR-0053 : *ce qui est restauré n'est pas une autorisation de doser, c'est la vitesse d'une correction déjà décidée par la mesure*.
- **Date** : 2026-09-04
- **Décideurs** : gate `pool-chemistry` (double critère filtration **ET** plafond mural — tranché sur la question laissée ouverte à la clôture de feature-084), `architect` (piste P1 retenue contre P2+P3), `code-reviewer` (**Majeur 1** : garde `candidatePending`), `test-engineer` (mutation), utilisateur (« un reboot ne devrait pas perturber le fonctionnement »)
- **Spec(s) liée(s)** : [feature-093](../../specs/features/done/feature-093-regulation-transparente-au-reboot.md) ; origine : [feature-084](../../specs/features/doing/feature-084-integrale-ph-cadence-et-memoire.md) (question R3 laissée ouverte), `bug-080` (source des redémarrages)

## Contexte

### L'ADR-0053 a persisté l'intégrale, puis l'a jetée chaque matin

L'[ADR-0053](0053-persistance-conditionnelle-integrale-ph.md) a autorisé la persistance NVS
de l'intégrale pH sous sept garanties. Deux d'entre elles se contredisaient sans qu'on le
voie :

- **R3** : péremption à `kPhIntegralPersistMaxAgeS` = **6 h d'horloge murale** (= un `Ti` à
  vitesse `normale`) ;
- **R5** : écriture au plus toutes les 30 min **et** seulement si la valeur a bougé de
  ≥ 5 % du plafond (810 pH·s en vitesse `normale`).

Or **l'intégrale est gelée hors filtration**. La nuit, elle ne bouge pas → R5 n'écrit
rien ; au matin, la dernière écriture date de la veille après-midi → R3 la déclare périmée.

**Le défaut est structurel, pas accidentel : tout redémarrage survenant avant ~10-11 h du
matin perdait l'intégrale, par construction.** Et plus tard dans la journée, un reboot 6 h
après la dernière écriture la perdait aussi si la valeur avait peu varié — c'est-à-dire
précisément en **régime établi**, le cas où elle est la plus précieuse.

### Le cas réel du 03/09/2026, 08:59:47 (PANIC, 7 s de coupure, filtration active)

| Heure | Journal | Coût |
|---|---|---|
| 08:59:54 | `Intégrale pH : instantané NVS REJETÉ (instantané périmé) — la régulation repart de zéro` | intégrale perdue |
| 08:59:54 | purge de stabilisation de 15 min (durée fixe de l'époque) | pas de mesure valide |
| 09:15:54 | `Démarrage dosage pH (auto)` | reprise **16 min après le boot, intégrale à 0** |

L'intégrale s'est reconstruite de 0 à 1 343 pH·s à 12:00 et n'a retrouvé son régime
(1 968 pH·s) qu'à **14:30**. Au départ la perte était masquée (P + feedforward saturaient
déjà le débit soutenable), mais dès que l'erreur est passée sous ~0,2, il manquait un tiers
de la demande. Ordre de grandeur : **une heure de régulation perdue**, hors purge.

### Le constat à formuler

L'intégrale pH est une grandeur **qui n'existe qu'en filtration** : elle n'est chargée que
quand l'eau circule, et gelée sinon. Lui appliquer une péremption en **heure murale**
revient à faire vieillir une grandeur pendant qu'elle est arrêtée. *Le critère d'âge en
heures murales est inadapté à une grandeur qui ne vit qu'en temps de filtration.*

## Décision

### 1. R3 amendée — double critère, filtration d'abord

Un instantané est **frais** si, et seulement si, **les trois** conditions tiennent :

| Condition | Constante | Valeur | Rôle |
|---|---|---|---|
| filtration accumulée depuis l'écriture ≤ | `kPhIntegralPersistMaxFiltMin` | **360 min** | **le critère nominal** — 6 h de filtration = un `Ti` à vitesse `normale` |
| âge mural ≤ | `kPhIntegralPersistMaxWallAgeS` | **259 200 s (72 h)** | **filet**, jamais critère nominal — après un hivernage ou une longue coupure, la chimie a pu changer sans circulation |
| compteur de filtration courant ≥ celui écrit | — | — | monotonie : un âge **négatif** n'est pas « très frais », il est **incalculable** |

L'âge de filtration est fourni par un **compteur monotone** de minutes d'eau présente,
`SafetyLimits::filtTotalMin` (clé NVS `pool-daily/filt_total`), jamais remis à zéro à
minuit. Il est **passé en paramètre** au module pur, jamais lu par lui
([ADR-0017](0017-logique-metier-pure-humble-object-testabilite.md)).

Fail-closed inchangé : horloge inexploitable, horodatage nul ou dans le futur ⇒ rejet
(`kEpoch`) ; compteur courant inférieur à celui écrit — NVS `pool-daily` effacée,
restauration d'usine — ⇒ rejet (`kFiltCounter`). La fraîcheur est **réévaluée à chaque
tick**, pas figée à la lecture : une candidate peut mourir de vieillesse pendant son attente.

### 2. R5 amendée — trois motifs d'écriture, dont une échéance

`phPersistWriteReason()` rend un **motif** et non plus un booléen, par priorité :

| Motif | Déclencheur | Pourquoi |
|---|---|---|
| `kRewrite` | aucun instantané valide en NVS **et** intégrale > 0 | l'enregistrement relu au boot a été rejeté : tant qu'il n'est pas remplacé, un second redémarrage repartirait de zéro (condition C5 du gate) |
| `kDelta` | ≥ 5 % du plafond **et** ≥ 30 min | le critère historique de l'ADR-0053, inchangé |
| `kDue` | **180 min de filtration** (`kPhIntegralPersistDueFiltMin`) depuis la dernière écriture, à valeur stable | **c'est lui qui rend le critère de fraîcheur atteignable** : sans échéance, un régime stable n'est jamais réécrit et finit par périmer — exactement le cas du 03/09 |

`kDue` n'écrit que s'il y a quelque chose à préserver (`valeur > 0`) ; la retombée à zéro
est déjà couverte par `kDelta`. Sans heure valide, on n'écrit pas (l'enregistrement serait
de toute façon rejeté au prochain boot). Coût mesuré : **≤ 6 écritures par jour de
filtration** (test natif sur 12 h simulées), à comparer aux 100 000 cycles garantis d'un
secteur NVS.

### 3. La garde `candidatePending` — la NVS a raison, pas la RAM

**Tant qu'une restauration attend sa confirmation R4, aucune écriture n'est émise**, quel
que soit le motif.

Sans cette garde, la feature s'annulait elle-même : pendant les 5 minutes de confirmation,
l'intégrale **en RAM vaut 0** alors que la NVS porte la valeur à préserver. Si le pH est
proche de sa consigne, la confirmation n'arrive jamais, la candidate vit — et 30 min plus
tard `kDelta` constate `|0 − savedValue| > seuil` et **écrase l'instantané valide par un
zéro**. Le redémarrage suivant repartait de zéro : précisément ce que feature-093 devait
empêcher. Défaut trouvé en revue (Majeur 1), corrigé, **et figé par un test tueur de
mutant** (garde retirée ⇒ exactement 1 échec).

Quand la candidate meurt (péremption R3, changement de contexte R2), la coquille baisse le
drapeau et la cadence normale reprend — un zéro écrit **à ce moment-là** est légitime :
l'ancien instantané n'était plus restaurable.

### 4. Schéma 2 : la migration est un rejet, jamais une réinterprétation

`PhIntegralSnapshot` passe de **20 à 24 octets** (nouveau champ `filtTotalMin`) et
`kPhIntegralPersistSchema` passe à **2**. Un enregistrement de schéma 1 est **rejeté**
(taille inexacte) au premier boot en v2.34.0, avec un log `info` explicite — **l'intégrale
repart de zéro une seule fois**, sans action utilisateur. C'est la règle de l'ADR-0053
appliquée telle quelle : un enregistrement d'un autre schéma est rejeté, **jamais**
réinterprété.

## Alternatives considérées

- **Garder la péremption murale, quelle que soit sa valeur** (rejetée) — 6 h jettent
  l'intégrale chaque matin ; 24 h laissent le bassin changer d'état sans que la circulation
  ait tourné ; aucune valeur murale ne distingue « 12 h d'arrêt » de « 12 h de filtration ».
  Le problème n'est pas le nombre, c'est l'**unité**.
- **P2 — re-timbrer l'instantané à chaque passage OFF→ON de la filtration** (rejetée
  seule) — sans changement de schéma, mais ne couvre que le cas du matin : un reboot en
  régime établi 6 h après la dernière écriture perd toujours l'intégrale (valeur stable ⇒
  pas de réécriture).
- **P3 seul — échéance d'écriture, sans changer le critère d'âge** (rejetée seule) —
  garantit qu'un instantané récent existe, mais fait payer des écritures NVS **pour
  compenser une unité de mesure fausse**. Retenue en **complément** de P1, pas à sa place :
  c'est elle qui garantit qu'un instantané frais au sens filtration existe à tout instant.
- **Critère de filtration SEUL, sans plafond mural** (rejetée par `pool-chemistry`) — après
  trois jours d'arrêt (hivernage, coupure secteur longue), le temps de filtration accumulé
  est **nul** : l'instantané serait éternellement « frais ». R4 protège (un pH à sa consigne
  au retour ⇒ rien n'est restauré), mais on ne fonde pas une garantie de sûreté sur le seul
  fait qu'une autre garantie rattrapera. D'où les 72 h murales, **filet et non critère**.
- **Réutiliser les minutes de filtration journalières de l'anneau de charge** (rejetée) —
  elles sont remises à zéro au changement de date : inutilisables comme base d'un âge qui
  traverse minuit. D'où un compteur **monotone** dédié.
- **Réinterpréter un instantané de schéma 1 en supposant `filtTotalMin = 0`** (rejetée) —
  supposer un compteur nul rendrait l'instantané artificiellement « très frais ». Une perte
  unique et journalisée vaut mieux qu'une restauration fondée sur une supposition.
- **Modifier l'ADR-0053 en place** (rejetée) — un ADR accepté est immuable ; l'ADR-0053
  amendait lui-même l'ADR-0052 par un **nouveau** document. On garde la trace du
  raisonnement d'origine **et** de ce qui l'a corrigé.

## Conséquences

### Positives

- Un redémarrage pendant la filtration **ne coûte plus l'intégrale** : la régulation
  retrouve ce qu'elle savait une seconde avant (à R1 et R4 près). Le coût d'un reboot passe
  d'**une heure de régulation** à la seule durée de purge.
- Un reboot **le matin** restaure la valeur de la veille au soir — c'est-à-dire exactement
  ce que la régulation aurait en RAM si elle n'avait pas redémarré.
- Le mécanisme est **auto-cohérent** : la grandeur, sa fraîcheur et sa cadence d'écriture
  se mesurent toutes dans la **même unité de temps**.
- La garde `candidatePending` ferme une classe de défaut entière : « la RAM transitoire
  écrase la mémoire durable pendant une phase d'attente ».
- Tout est dans un module **pur** : les quatre motifs de rejet et les trois motifs
  d'écriture sont testés en natif et validés par mutation.

### Négatives / dette assumée

- **Perte unique au premier boot 2.34.0** : le changement de schéma rejette l'instantané
  v1. L'intégrale repart de zéro une fois, avec un log `info`. Aucune action utilisateur.
- **Écritures NVS en hausse** : de ~2 à **≤ 6 par jour de filtration** (l'échéance `kDue`
  en ajoute ~3). Négligeable devant 100 000 cycles, mais c'est une hausse **délibérée** —
  elle achète la garantie qu'un instantané frais existe toujours.
- L'appareil dépend maintenant d'un **compteur persisté supplémentaire**
  (`pool-daily/filt_total`) : son effacement (restauration d'usine) fait rejeter
  l'instantané par `kFiltCounter` — fail-closed voulu, mais un mode de rejet de plus à
  connaître au diagnostic.
- Le filet mural de 72 h est un **choix, pas une mesure** : aucune donnée terrain ne dit
  qu'une intégrale est encore valide après 71 h d'arrêt et invalide après 73.
- ⚠️ **Non vérifié sur le terrain** à la rédaction : validation attendue au matin du
  05/09/2026 (instantané écrit la veille au soir, restauré puis confirmé après la purge
  adaptative — [ADR-0059](0059-purge-adaptative-fenetre-de-medianes.md)).

### Ce que ça verrouille

- **Toute grandeur qui ne vit qu'en filtration mesure son âge en temps de filtration.**
  Réintroduire un critère de fraîcheur en heure murale sur une telle grandeur reproduit le
  défaut du 03/09.
- **Le plafond mural reste un filet, jamais un critère nominal.** Le descendre à une valeur
  atteignable en fonctionnement normal (< 72 h) rétablirait la péremption structurelle.
- **Aucune écriture pendant qu'une candidate attend sa confirmation.** Tout nouveau chemin
  d'écriture de l'instantané doit consulter `candidatePending` — la RAM ne fait pas foi
  tant que la NVS porte une valeur non encore jugée.
- **Le compteur de filtration doit rester monotone et persisté.** Le remettre à zéro
  (minuit, changement de config, reset de compteurs journaliers) invaliderait tous les
  instantanés en cours.
- **R1, R2, R4, R6, R7 restent cumulatives et inchangées** : cet ADR n'élargit pas *ce qui*
  peut être restauré, seulement *quand* l'instantané est jugé frais. Retirer l'une d'elles
  rétablirait l'objection de l'[ADR-0052](0052-terme-integral-ph-dimensionne-par-autorite.md).
- **Toute retouche de `PhIntegralSnapshot` incrémente son schéma** (règle de l'ADR-0053,
  appliquée ici pour la première fois : 1 → 2).

## Références

- Code : [`src/ph_persist_logic.h`](../../src/ph_persist_logic.h) (module pur —
  `PhIntegralSnapshot` schéma 2 / 24 o, `PhPersistAgeInputs`, `phPersistAge`,
  `PhPersistWriteReason`, `phPersistWriteReason` avec la garde `candidatePending`),
  [`src/constants.h`](../../src/constants.h) (`kPhIntegralPersistMaxFiltMin`,
  `kPhIntegralPersistMaxWallAgeS`, `kPhIntegralPersistDueFiltMin`,
  `kPhIntegralPersistSchema` — constants.h:1289-1342),
  [`src/config.cpp`](../../src/config.cpp) / [`src/config.h`](../../src/config.h)
  (transport NVS, `SafetyLimits::filtTotalMin`, clé `pool-daily/filt_total`),
  [`src/pump_controller.cpp`](../../src/pump_controller.cpp) (câblage, drapeau de candidate,
  journaux)
- Tests : [`test/test_native_ph_persist/`](../../test/test_native_ph_persist/) (89 cas —
  nuit sans filtration puis reboot accepté, 361 min de filtration rejeté, 72 h + 1 s
  rejeté, compteur non monotone rejeté, schéma 1 rejeté avant tout autre contrôle,
  ≤ 6 écritures sur 12 h, `test_J_candidate_en_attente_interdit_toute_ecriture`).
  Mutation : garde `candidatePending` retirée ⇒ exactement 1 échec.
- Spec : [feature-093](../../specs/features/done/feature-093-regulation-transparente-au-reboot.md)
- Doc : [subsystems/pump-controller.md](../subsystems/pump-controller.md),
  [UPDATE_GUIDE.md](../UPDATE_GUIDE.md)
- ADR liés : [ADR-0053](0053-persistance-conditionnelle-integrale-ph.md) (**amendé sur les
  seuls points R3 et R5**), [ADR-0052](0052-terme-integral-ph-dimensionne-par-autorite.md)
  (dimensionnement de l'intégrale, inchangé),
  [ADR-0008](0008-persistance-cumuls-journaliers-nvs.md) (compteurs journaliers NVS, qui
  hébergent désormais le compteur monotone de filtration),
  [ADR-0017](0017-logique-metier-pure-humble-object-testabilite.md) (module pur),
  [ADR-0059](0059-purge-adaptative-fenetre-de-medianes.md) (même cycle v2.34.0 : la porte
  des mesures protège la valeur restaurée du transitoire de rinçage)
