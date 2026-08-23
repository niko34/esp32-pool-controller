# ADR-0044 — Budget horaire et temps minimum d'injection comptés en temps POMPE-ON

- **Statut** : Accepté
- **Date** : 2026-08-23
- **Décideurs** : gate `pool-chemistry` (contre-visite du 2026-08-23, GO), architect
- **Spec(s) liée(s)** : [bug-dosage-fantome-duty-non-calibre](../../specs/features/todo/bug-dosage-fantome-duty-non-calibre.md) (itération 2, v2.21.0)

## Contexte

Le passage au proportionnel de temps ([ADR-0043](0043-dosage-proportionnel-de-temps-amplitude-calibree.md))
fait qu'une **session** de dosage n'est plus un intervalle où la pompe tourne en
continu : elle alterne pouls (pompe ON) et repos (pompe OFF). Deux invariants de
sécurité préexistants raisonnaient en **temps de session** (temps mural depuis le
démarrage) :

1. la **limite horaire** `ph/orp_limit_minutes` (fenêtre glissante 1 h, `usedMs` —
   budget partagé auto + manuel, [ADR-0020](0020-budget-horaire-dosage-unique.md)) ;
2. le **temps minimum d'injection** `minInjectionTimeMs` (30 s — une injection
   démarrée ne s'arrête pas avant, anti short-cycling).

Conserver le comptage en temps de session aurait changé leur **sens chimique** : une
session d'une heure à 10 % de rapport d'impulsion (6 min de produit réel) aurait épuisé
tout le budget horaire, et le « minimum 30 s » aurait pu être satisfait par 5 s de
produit + 25 s de repos.

## Décision

`usedMs` (limite horaire) **et** la garde de temps minimum d'injection accumulent
désormais sur le **temps POMPE-ON** : les millisecondes où la pompe est physiquement
alimentée (pouls auto/scheduled, injections manuelles, pompes test), jamais les repos
entre pouls. Sémantique validée par le gate : **« minutes où le produit coule »**.

- `refreshDosingState(state, now, pumpOn)` : le prédicat d'accumulation devient
  `pumpOn` (un seul point d'accumulation, pas de double comptage — inchangé sur ce
  point depuis feature-006).
- `shouldContinueDosing(error, stopThreshold, sessionOnMs)` : la garde des 30 s
  raisonne sur le temps pompe-ON **cumulé de la session** (`_sessionOnMs`).
- Le **cycle** (`cyclesToday`, ring anti-rafale) reste compté **par session** : les
  impulsions internes n'incrémentent rien. La pause mélange s'arme en **fin de
  session** (y compris fin par seuil C), jamais entre deux pouls.

## Alternatives considérées

- **Conserver le temps de session** (rejetée) — surestime la consommation du budget
  d'un facteur 1/rapport d'impulsion (jusqu'à ×12 au pouls minimal) : la limite
  horaire, dimensionnée en minutes de produit, aurait suspendu des régulations saines ;
  et le temps minimum aurait perdu sa fonction de dose plancher.
- **Compter les deux (session pour la limite, ON pour le minimum)** (rejetée) — deux
  horloges pour deux gardes voisines, incohérence garantie à la maintenance ; le gate a
  tranché une sémantique **unique**.
- **Convertir la limite horaire en mL/h** (écartée) — plus juste conceptuellement mais
  change le contrat utilisateur (`ph_limit_minutes` existe dans l'UI, MQTT et NVS) ;
  hors périmètre du correctif.

## Conséquences

### Positives
- La limite horaire mesure ce qu'elle prétend limiter : du produit injecté, pas du
  temps d'attente. Cohérente entre auto (pouls), scheduled (continu) et manuel.
- Le minimum de 30 s redevient une **dose plancher réelle** (30 s de produit).

### Négatives / dette assumée
- **Changement de comportement observable** : à réglage identique, une régulation en
  impulsions consomme son budget horaire **plus lentement** qu'avant (elle peut donc
  doser plus longtemps en temps mural). C'est voulu — le volume reste borné par les
  limites journalières et le débit calibré — mais un utilisateur qui avait réglé
  `ph_limit_minutes` en compensant l'ancien sur-comptage doit revalider sa valeur
  (procédure P3 de la spec : limites abaissées pendant 3 jours de validation).
- Une session peut durer bien plus que `ph_limit_minutes` en temps mural (repos non
  comptés) — l'affichage « injection en cours » prolongé est traité à l'itération 3
  (UI commandé/effectif).

### Ce que ça verrouille
- Le prédicat d'accumulation de `usedMs` est **pompe physiquement alimentée** — toute
  future source d'activation (nouveau mode, nouvelle route) est comptée par
  construction si elle passe par `applyPumpDuty`, et doit l'être.
- [ADR-0020](0020-budget-horaire-dosage-unique.md) (budget **unique** auto + manuel)
  reste pleinement en vigueur : cet ADR ne change que l'horloge, pas le partage.

## Références

- Code : [`src/pump_controller.cpp`](../../src/pump_controller.cpp)
  (`refreshDosingState`, `shouldContinueDosing`, `_sessionOnMs`),
  [`src/pump_controller.h`](../../src/pump_controller.h)
- Spec : [bug-dosage-fantome-duty-non-calibre](../../specs/features/todo/bug-dosage-fantome-duty-non-calibre.md)
- ADR liés : [ADR-0020](0020-budget-horaire-dosage-unique.md) (budget partagé — horloge
  modifiée ici), [ADR-0043](0043-dosage-proportionnel-de-temps-amplitude-calibree.md)
- Doc : [pump-controller.md](../subsystems/pump-controller.md#dosage-en-proportionnel-de-temps-itération-2-v2210)
