# ADR-0106 — Toute suspension demandée par un client est bornée, auto-libérante, plafonnée, et journalise sa levée

- **Statut** : **Superseded by [ADR-0107](0107-etat-visible-et-persistant-plutot-que-suspension-invisible.md)**
  (2026-10-13, décision utilisateur, même journée) — le mécanisme de bail décrit ci-dessous
  (`cal_hold_logic.{h,cpp}`) a été **ABANDONNÉ avant tout déploiement**, au profit d'une
  bascule explicite de régulation en Manuel, restaurée par un geste humain (gate
  `pool-chemistry` GATE 5, GO sous 8 conditions). Code de remplacement :
  [`src/cal_manual_switch_logic.h`](../../src/cal_manual_switch_logic.h) /
  [`.cpp`](../../src/cal_manual_switch_logic.cpp). **La doctrine du patron général (les
  quatre propriétés ci-dessous) reste la référence pour toute FUTURE suspension côté
  client qui n'a PAS de surface de contrôle visible existante à rejoindre** (voir
  ADR-0107 pour l'arbitrage) — `_flowAssistSuspend` reste le contre-exemple non corrigé
  qu'elle nommait. Le reste de ce document est conservé **tel quel**, comme l'exige la
  règle d'immuabilité des ADR acceptés — il décrit fidèlement l'analyse qui a mené au
  mécanisme depuis remplacé.
- **Décideurs** : gate `pool-chemistry` (GO sous 17 conditions, GATE 2, GATE 3, GATE 4),
  `architect`, `embedded-developer`, `code-reviewer`
- **Spec(s) liée(s)** : [`bug-160`](../../specs/features/done/bug-160-l-indicateur-de-stabilite-ment-et-le-dosage-ne-se-suspend-pas.md)
  (volet 2 — déclencheur, aucun lien avec le volet 1, voir [ADR-0105](0105-un-indicateur-de-transitoire-d-etalonnage-n-est-pas-une-porte-de-mesure.md))

## Contexte

`bug-160` (volet 2) corrige un défaut de sécurité chimique : pendant qu'une sonde pH ou ORP
trempe dans un bécher d'étalonnage, l'appareil continuait de réguler sur ce qu'elle lisait — un
étalon ORP à 470 mV contre une consigne de 530 pouvait injecter du chlore dans le **bassin** sur
la foi de la lecture d'un **bécher**.

Le mécanisme correctif — un bail de suspension (`cal_hold_logic.{h,cpp}`) armé à l'entrée dans
l'écran de calibration, prolongé par battement de cœur, et qui se lève tout seul — n'est pas
propre à la calibration. C'est un **patron général** : un client web (navigateur) demande une
suspension temporaire d'un comportement de sécurité, et le firmware doit décider jusqu'où lui
faire confiance. Deux défauts, trouvés **pendant ce même lot**, montrent que le patron naïf
(« le client dit quand ça commence et quand ça finit ») est dangereux des deux côtés :

1. **Sans minuteur de relâche**, un onglet fermé ou un navigateur tué laisse la suspension
   collée indéfiniment (impasse n°1).
2. **Avec seulement un plafond par épisode mais sans distinction entre une ouverture et un simple
   battement**, un client qui ne cesse jamais de battre peut enchaîner des sessions plafonnées
   dos à dos — observé par le gate : « 48 suspensions enchaînées sur une journée, ~1 min de
   régulation par heure », soit l'impasse n°1 redécoupée en tranches (impasse n°2, GATE 3).

Le projet possède déjà un contre-exemple non traité de l'impasse n°1 : `_flowAssistSuspend`
(l'assistant de mesure du débit, feature-075) est une suspension **sans minuteur de relâche
firmware** — elle reste collée si l'onglet meurt, exactement le défaut que ce lot corrige côté
calibration. Ce n'est **pas** corrigé ici (hors périmètre de `bug-160`), mais il doit être nommé
pour qu'un futur lot le retrouve.

## Décision

**Toute suspension d'un comportement de sécurité (dosage, régulation, garde quelconque) demandée
par un client HTTP doit respecter les quatre propriétés suivantes, structurellement — pas par
convention de développeur :**

1. **Bornée par un minuteur firmware qui la lève tout seul.** Le client ne peut qu'armer ou
   prolonger ; il ne peut **jamais** maintenir la suspension active par sa seule présence (pas de
   WebSocket de session, pas de simple flag sans échéance).
2. **Auto-libérante par construction, jamais par une route de relâche.** Aucun endpoint
   « release »/« stop » n'existe pour cette suspension (`cal_hold_logic` : `⛔ aucune route de
   relâche`) — une suspension qui dépend d'un client pour se **lever** est tout autant une
   régression de sécurité qu'une suspension qui dépend d'un client pour se **maintenir** : les
   deux sens de dépendance au navigateur sont dangereux, pas seulement celui qu'on corrige
   aujourd'hui.
3. **Plafonnée à une durée absolue, et la distinction OUVERTURE / PROLONGATION est obligatoire
   dès qu'un plafond existe.** Un plafond par épisode ne suffit pas : sans distinguer un geste
   d'entrée humain (seul habilité à ouvrir une nouvelle session et fixer un nouveau plafond) d'un
   battement automatique (qui ne peut que prolonger, jamais ouvrir), le plafond se contourne par
   enchaînement. Une période réfractaire après une expiration **par plafond** (distincte d'une
   expiration par bail ordinaire) referme cette brèche : après un plafond, toute nouvelle
   ouverture est ignorée pendant une durée du même ordre que le bail normal.
4. **La levée se journalise, y compris si elle n'est provoquée par aucune requête.** Une
   expiration qui ne serait tracée qu'au prochain appel client manquerait précisément le cas où
   plus personne ne regarde (onglet fermé pile au plafond) — c'est le cas que le mécanisme existe
   pour rendre visible. La détection doit donc s'exécuter **à chaque tour de la boucle
   principale**, par détection de front, indépendamment de tout battement HTTP ultérieur (gate 4 :
   « une alerte qui ne se déclenche que si quelqu'un redemande quelque chose n'est pas une
   alerte »).

**Patron de référence (à réutiliser, pas à redupliquer)** : `_flowAssistSuspendPrev`
(`pump_controller.cpp`) pour la détection de front one-shot par transition ; `cal_hold_logic.h`
pour l'arithmétique wrap-safe (`(int32_t)(end - now) > 0`, jamais `now < end`) et pour la
séparation Open/Beat avec période réfractaire.

**Minuteur toujours dédié, jamais réutilisé depuis un mécanisme existant pour une finalité
différente** : ce lot a d'abord envisagé de réutiliser `armStabilizationTimer()` (le minuteur de
stabilisation post-calibration), rejeté par le gate parce que `clearStabilizationTimer()` est
appelé à **chaque arrêt de filtration** — une suspension de calibration logée là serait effacée
par une simple fin de plage horaire, et le dosage reprendrait au redémarrage de la pompe, sondes
toujours dans les béchers. Le **patron** (arithmétique, plafond dur) se réutilise ; la
**variable d'état**, jamais, si son cycle de vie répond à un autre événement.

## Alternatives considérées

- **Suspension maintenue par la présence d'une connexion WebSocket active** (écartée, jamais
  proposée formellement mais implicite dans l'intuition initiale) — fait dépendre un invariant de
  sécurité de la couche transport ; une coupure réseau transitoire lèverait la suspension au
  mauvais moment, et il n'existe pas de signal fiable de fermeture propre côté WebSocket (une
  coupure et une fermeture volontaire sont indiscernables).
- **Accepter « 48 suspensions enchaînées par jour » comme un risque mineur** (écartée, GATE 3) —
  le chiffrage qui couvre une heure de suspension (ADR-0092, ≤ 0,07 pH) ne couvre pas une saison
  de suspensions quasi continues ; un `warning` répété toutes les 30 min n'est pas un garde-fou,
  c'est un bruit qu'on apprend à ignorer.
- **Détecter la présence réelle de la sonde dans le bécher (capteur de niveau, etc.)** (écartée,
  interdite explicitement par le gate, condition C15) — ajoute une dépendance matérielle pour
  remplacer un plafond temporel qui suffit et reste universel.
- **Corriger `_flowAssistSuspend` dans ce même lot** (écartée, hors périmètre) — le même défaut
  existe mais sa correction n'est pas demandée par `bug-160` ; le nommer ici évite qu'il se
  reperde.

## Conséquences

### Positives
- Un unique patron documenté (`cal_hold_logic.h` + ce ADR) sert de référence à toute future
  suspension demandée par un client, au lieu de redériver les mêmes pièges à chaque fois.
- Le défaut « plafond contournable par enchaînement » (impasse n°2), découvert **après** la
  première livraison de ce lot, a donné lieu à un correctif avant la clôture de la spec plutôt
  qu'à un incident de production.

### Négatives / dette assumée
- **`_flowAssistSuspend` reste un contre-exemple non corrigé** de la propriété n°1 (pas de
  minuteur de relâche firmware) — nommé ici, traité par aucun lot à ce jour. Une suite distincte
  devra l'aligner sur ce patron.
- La période réfractaire (propriété n°3) ajoute un état (« suspension refusée temporairement »)
  qu'il faut communiquer à l'utilisateur sous peine de confusion (« pourquoi mon étalonnage
  n'arme-t-il plus la suspension ? ») — traité côté UI par un bandeau dédié
  ([`docs/features/page-ph.md`](../features/page-ph.md#bandeau-de-suspension-ph-cal-hold-banner--orp-cal-hold-banner-bug-160)),
  mais c'est un coût de compréhension réel, pas gratuit.

### Ce que ça verrouille
1. **Aucune suspension de sécurité future ne peut dépendre d'une route de relâche explicite** —
   si un futur mécanisme en réclame une « pour la commodité du debug » ou « pour que l'utilisateur
   puisse annuler proprement », c'est un signal qu'il viole cette décision et doit repasser par
   `pool-chemistry`.
2. **Tout plafond doit distinguer ouverture et prolongation dès qu'il existe** — un plafond seul,
   sans cette distinction, est un faux sentiment de sécurité (démontré par l'impasse n°2).
3. **La détection d'expiration doit être indépendante de tout appel client** — un correctif futur
   qui réintroduirait une détection paresseuse (évaluée seulement au prochain POST) viole cette
   décision, quel que soit l'argument de coût (le gate 4 a vérifié que le coût réel est une
   comparaison entière par tour de boucle, pas une allocation).

## Références

- ⚠️ **Code ABANDONNÉ (mécanisme décrit par cet ADR, supprimé le 2026-10-13)** —
  `src/cal_hold_logic.{h,cpp}` et le bloc `PumpControllerClass::_calHold*` de
  `src/pump_controller.{h,cpp}` n'existent plus. Remplacés par
  [`src/cal_manual_switch_logic.h`](../../src/cal_manual_switch_logic.h) /
  [`.cpp`](../../src/cal_manual_switch_logic.cpp) (machine pure de la bascule explicite) et
  `POST /calibration/enter` / `POST /calibration/exit` dans
  [`src/web_routes_calibration.cpp`](../../src/web_routes_calibration.cpp) (remplacent
  `POST /calibration/hold`, retiré). Le bloc `kCalHold*` de
  [`src/constants.h`](../../src/constants.h) est retiré (remplacé par
  `kCalSwitchAlertFirstDelayS`/`kCalSwitchAlertRepeatS`).
- Doc : [`docs/subsystems/pump-controller.md`](../subsystems/pump-controller.md#suspension-de-régulation-pendant-une-calibration-bug-160-volet-2),
  [`docs/API.md`](../API.md#post-calibrationhold--write), [`docs/features/page-ph.md`](../features/page-ph.md)
- Spec : [`bug-160`](../../specs/features/done/bug-160-l-indicateur-de-stabilite-ment-et-le-dosage-ne-se-suspend-pas.md)
- ADR liés : [ADR-0105](0105-un-indicateur-de-transitoire-d-etalonnage-n-est-pas-une-porte-de-mesure.md)
  (volet 1 du même bug, doctrine distincte et volontairement **non fusionnée** ici — fusionner
  aurait enterré cette doctrine générale dans un ADR d'étalonnage, où un futur mécanisme de
  suspension ne l'aurait jamais retrouvée), [ADR-0092](0092-retrait-analyse-stabilite-porte-mesures.md)
  (chiffrage ≤ 0,07 pH/h réutilisé pour dimensionner le bail et juger l'impasse n°2),
  [ADR-0098](0098-toute-constante-de-mesure-declare-sa-source.md) (source de `kCalHoldLeaseMs`)
