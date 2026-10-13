# ADR-0107 — Un état visible et persistant prime sur une suspension auto-libérante invisible, quand une surface de contrôle existe déjà

- **Statut** : Accepté
- **Date** : 2026-10-13
- **Décideurs** : utilisateur (décision produit), gate `pool-chemistry` (GATE 5, GO sous
  8 conditions), `architect`, `embedded-developer`, `web-ui-developer`, `code-reviewer`
- **Spec(s) liée(s)** : [`bug-160`](../../specs/features/done/bug-160-l-indicateur-de-stabilite-ment-et-le-dosage-ne-se-suspend-pas.md)
  (volet 2 — REFONTE du 2026-10-13, aucun lien avec le volet 1, voir
  [ADR-0105](0105-un-indicateur-de-transitoire-d-etalonnage-n-est-pas-une-porte-de-mesure.md))

## Contexte

`bug-160` (volet 2) corrige un défaut de sécurité chimique : pendant qu'une sonde pH ou ORP
trempe dans un bécher d'étalonnage, l'appareil continuait de réguler sur ce qu'elle lisait — un
étalon ORP à 470 mV contre une consigne de 530 pouvait injecter du chlore dans le **bassin** sur
la foi de la lecture d'un **bécher**.

Un premier mécanisme a été conçu, implémenté, et revu par **trois gates** `pool-chemistry`
successifs (GO initial sous 17 conditions, puis GATE 2 et GATE 3 sur deux défauts trouvés après
coup) : un **bail de suspension** firmware (`cal_hold_logic.{h,cpp}`), armé à l'entrée de l'écran
de calibration, prolongé par un battement de cœur HTTP toutes les 60 s, plafonné à 30 minutes
avec une période réfractaire pour empêcher l'enchaînement de sessions. Ce mécanisme est décrit en
détail par [ADR-0106](0106-toute-suspension-demandee-par-un-client-est-bornee.md), qui en tire une
doctrine générale : toute suspension demandée par un client HTTP doit être bornée, auto-libérante,
plafonnée, et journalisée indépendamment de tout appel client.

**Avant tout déploiement**, l'utilisateur a proposé une approche radicalement plus simple :

> « Encore des recettes complexes. Il suffirait de passer le dosage en manuel quand on clique sur
> Calibrer et l'indiquer comme un message d'alerte en haut de l'écran de calibration […]. On peut
> aussi modifier le bouton "Fermer" pour mettre "Fermer et remettre la régulation en mode Auto".
> C'est simple et efficace. Pas d'histoire de débit ou de timer, etc… »

Le gate `pool-chemistry`, ressaisi (GATE 5), a confirmé que cette approche n'est pas une
simplification qui sacrifie la sécurité, mais une approche **structurellement différente**, avec
un sens d'erreur opposé :

| | Bail auto-expirant (ADR-0106, abandonné) | Bascule en Manuel (retenu ici) |
|---|---|---|
| Reprise de la régulation | automatique, sans geste humain | **demande une action humaine** |
| Si l'écran meurt (onglet fermé, navigateur tué) | la suspension expire d'elle-même, invisible pendant qu'elle court | reste en Manuel, **visible au tableau de bord** |
| Visibilité d'un échec de reprise | **aucune** — une minuterie coincée ne se voit nulle part | **visible** — le mode Manuel s'affiche, et une alerte d'oubli le signale après 2 h |
| Sens de l'erreur en cas d'oubli | le dosage **reprend** sans savoir où sont les sondes | le dosage **reste arrêté** |

> « On échange un oubli qu'on VOIT contre une panne qu'on ne voit PAS. » (gate `pool-chemistry`,
> GATE 5)

Les deux défauts bloquants qui avaient nécessité GATE 2 et GATE 3 (repliement d'horloge sur la
période réfractaire — fail-open de 24,8 jours — et deux écrivains concurrents sur un même
étalonnage) **n'existaient que parce que le mécanisme de bail existait**. Un mécanisme invisible
et temporisé fabrique sa propre complexité, et cette complexité fabrique ses propres défauts.

## Décision

**Quand le comportement à suspendre est déjà une surface de contrôle VISIBLE pour
l'utilisateur** — ici, le mode de régulation `automatic` / `scheduled` / `manual`, déjà affiché au
tableau de bord et déjà pourvu d'une trace de sécurité équivalente ailleurs dans le projet
(`phAutoManualEpoch`/`Reason`, feature-067) — **la bascule EXPLICITE et PERSISTANTE vers cet état
existant, restaurée par un GESTE HUMAIN et surveillée par une alerte d'oubli, prime sur une
suspension auto-libérante mais invisible.**

Concrètement, pour `bug-160` (volet 2) :

1. À l'entrée dans l'écran de calibration, l'appareil bascule **pH et ORP** en `manual`, mémorise
   le mode antérieur de chaque boucle, et interrompt tout dosage en cours.
2. **Aucun minuteur de relâche** ne lève cette bascule — elle reste active jusqu'à un geste humain
   explicite (`POST /calibration/exit`, bouton « Fermer et rétablir la régulation »).
3. **Une alerte d'oubli** (2 h puis toutes les 6 h) compense le risque nouveau introduit par
   l'absence d'auto-libération — gate `pool-chemistry`, condition C4, **bloquante** : « on
   fabrique délibérément des bascules, il faut donc fermer le risque d'oubli ».
4. La trace de cette bascule est **distincte** de la trace de sécurité `phAutoManual*`
   (feature-067) — les confondre aurait ouvert un nouveau trou de sécurité (voir
   [pump-controller.md](../subsystems/pump-controller.md#c2--le-piège-que-la-refonte-a-failli-créer)).

**Cette décision ne contredit pas [ADR-0106](0106-toute-suspension-demandee-par-un-client-est-bornee.md) en général, elle en restreint le domaine d'application.** La doctrine du bail
borné, auto-libérant et plafonné reste la bonne réponse **quand aucune surface de contrôle
visible n'existe déjà** pour porter l'état suspendu — c'est le cas de `_flowAssistSuspend`
(l'assistant de mesure de débit, feature-075), qui n'a pas d'équivalent « mode » affiché à
l'utilisateur, et pour lequel une reprise automatique rapide (minutes) reste désirable et sans
risque de confusion. Dans ce cas, imposer une bascule visible et un geste humain pour une
suspension de quelques minutes serait une charge cognitive sans bénéfice de sécurité
correspondant.

## Alternatives considérées

- **Conserver et achever le mécanisme de bail** (ADR-0106, écartée) — fonctionnellement livrable
  (GO sous 8 conditions avant ressaisie), mais **plus complexe à documenter, tester et maintenir
  que le défaut qu'il corrige** : trois clés de parité, un battement HTTP périodique côté client,
  une arithmétique Open/Beat avec période réfractaire, deux défauts de revue trouvés après la
  première livraison (fail-open par repliement d'horloge, double écrivain). La complexité du
  mécanisme de sécurité est elle-même devenue une source de risque.
- **Bascule en Manuel SANS alerte d'oubli** (écartée, gate `pool-chemistry` GATE 5, condition C4
  rendue bloquante) — « on fabrique délibérément des bascules » : sans backstop, un écran de
  calibration oublié après un succès laisserait la régulation éteinte indéfiniment, sans aucun
  signal. L'alerte à 2 h puis toutes les 6 h ferme ce risque sans réintroduire d'automatisme de
  reprise.
- **Réutiliser `phAutoManualEpoch`/`phAutoManualReason` pour porter la bascule de calibration**
  (écartée, gate `pool-chemistry` GATE 5, condition C2 — **piège central**) — ce champ porte une
  **autre** trace de sécurité (escalade de plancher acide, feature-067), protégée par un gate
  fail-closed (`web_routes_config.cpp`, incident du 2026-08-25) qui exige un acquittement explicite
  pour en sortir. Le réutiliser aurait soit fait échouer le bouton de fermeture (409), soit — pire
  — permis d'acquitter silencieusement une escalade de sécurité réelle en fermant un simple écran
  de calibration. **Trace distincte obligatoire.**
- **Réutiliser `armStabilizationTimer()`** (écartée, gate `pool-chemistry`, héritée de la
  conception du bail) — `clearStabilizationTimer()` est appelé à chaque arrêt de filtration ; un
  mécanisme logé là serait effacé par une simple fin de plage horaire programmée.

## Conséquences

### Positives
- **Suppression nette de complexité** : pas de minuteur de relâche, pas de battement HTTP
  périodique, pas de période réfractaire, pas de distinction Open/Beat, pas de parité à trois
  champs. Le code retiré (`cal_hold_logic.{h,cpp}`, bloc `_calHold*` de `PumpControllerClass`)
  n'a jamais été déployé en production.
- **L'état est visible par construction** : `phRegulationMode`/`orpRegulationMode` sont déjà des
  champs `/data` et WebSocket existants, déjà affichés au tableau de bord — aucune nouvelle
  surface d'affichage n'était nécessaire pour rendre la bascule visible, seulement un bandeau
  explicatif et deux champs de datation (`cal_switch_active`, `cal_switch_epoch`).
- **Le sens d'erreur favorise la sécurité chimique** : un oubli produit un dosage **arrêté**, pas
  un dosage repris sans confirmation que les sondes sont de retour dans l'eau.

### Négatives / dette assumée
- **Un geste humain est désormais requis pour rétablir la régulation.** Un utilisateur qui
  calibre puis oublie de fermer l'écram correctement (par exemple en tuant l'onglet avant que
  `onExit` ne s'exécute) laisse la régulation en Manuel jusqu'à la prochaine alerte (2 h) ou sa
  propre vigilance — ce coût est **assumé explicitement** par l'utilisateur-décideur comme
  préférable à l'alternative (reprise automatique sans certitude sur l'état des sondes).
- **`_flowAssistSuspend` reste un contre-exemple non corrigé** de la propriété « bornée et
  auto-libérante » d'ADR-0106 — nommé ici et dans ADR-0106, traité par aucun lot à ce jour. Cette
  décision ne le corrige pas : son cas d'usage (pas de surface visible existante, reprise rapide
  désirable) reste couvert par la doctrine d'ADR-0106, pas par celle-ci.

### Ce que ça verrouille
1. **Une suspension de sécurité qui porte sur une surface de contrôle déjà visible et déjà
   persistée (un mode de régulation, par exemple) ne doit pas réinventer un mécanisme temporaire
   parallèle** — elle doit basculer cette surface elle-même, et la restaurer par geste humain.
   Un futur lot qui voudrait ajouter un minuteur de relâche à un tel mécanisme doit d'abord
   justifier pourquoi la visibilité existante ne suffit pas.
2. **Toute bascule automatique du firmware vers un mode « sous contrôle de sécurité » doit avoir
   sa propre trace, jamais réutiliser une trace de sécurité existante à une autre fin** — le piège
   C2 (ci-dessus) est générique, pas spécifique à la calibration.
3. **Toute bascule sans reprise automatique doit être accompagnée d'une alerte d'oubli** — sans
   quoi l'absence de minuteur de relâche n'économise de la complexité qu'en déplaçant le risque
   vers un oubli silencieux.

## Références

- Code : [`src/cal_manual_switch_logic.h`](../../src/cal_manual_switch_logic.h) /
  [`.cpp`](../../src/cal_manual_switch_logic.cpp) (décision pure, ADR-0017),
  [`src/web_routes_calibration.cpp`](../../src/web_routes_calibration.cpp) (`POST /calibration/enter`
  / `POST /calibration/exit`), `PumpControllerClass::tickCalSwitchAlert()`
  (`src/pump_controller.cpp`), champs `calSwitch*` de `src/config.h`.
- Doc : [`docs/subsystems/pump-controller.md`](../subsystems/pump-controller.md#bascule-de-régulation-en-manuel-pendant-une-calibration-bug-160-volet-2),
  [`docs/API.md`](../API.md#post-calibrationenter--write), [`docs/features/page-ph.md`](../features/page-ph.md#bandeau-de-bascule-en-manuel-ph-cal-switch-banner--orp-cal-switch-banner-bug-160)
- Spec : [`bug-160`](../../specs/features/done/bug-160-l-indicateur-de-stabilite-ment-et-le-dosage-ne-se-suspend-pas.md)
  (section « 🔄 REFONTE DU VOLET 2 » et « ✅ GATE 5 »)
- ADR liés : [ADR-0106](0106-toute-suspension-demandee-par-un-client-est-bornee.md) (Superseded
  par cet ADR pour le cas de la calibration — sa doctrine reste la référence pour une suspension
  sans surface de contrôle visible existante, cf. `_flowAssistSuspend`),
  [ADR-0105](0105-un-indicateur-de-transitoire-d-etalonnage-n-est-pas-une-porte-de-mesure.md)
  (volet 1 du même bug, doctrine distincte et non affectée par cette refonte),
  [ADR-0004](0004-mode-regulation-enum-3-valeurs.md) (le mode de régulation à 3 valeurs, surface
  de contrôle réutilisée ici)
