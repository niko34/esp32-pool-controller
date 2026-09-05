# feature-076 — Détection d'un produit de correction mal déclaré

## Métadonnées

| Champ | Valeur |
|---|---|
| Statut | todo |
| Origine | Contre-visite `pool-chemistry` du cycle « refonte des seuils pH » (défaut C2), 27/08/2026 |
| Priorité | Haute — sécurité chimique, aggravée par la mise en flotte |
| Version cible | à définir |
| Décision utilisateur | Reportée hors du cycle de refonte (périmètre volontairement contenu) |

## Domaines touchés

- `src/pump_controller.{h,cpp}` — surveillance d'excursion, message d'escalade
- `src/constants.h` — seuils de la détection d'incohérence
- Nouveau module pur candidat (ADR-0017) — logique de cohérence effet/produit
- `docs/subsystems/pump-controller.md`, `CHANGELOG.md`

## Sécurité chimique

**Oui — invocation `pool-chemistry` obligatoire.** Cette spec existe parce que le
gate a refusé de considérer le sujet clos.

## Problème

Depuis la refonte des seuils (v2.25.0), les gardes pH sont **asymétriques par
type de correction déclaré**, conformément à la demande utilisateur : seul le
seuil bas (6,70 corrigé) est armé quand le produit est `ph_minus`, seul le
plafond (7,70 corrigé) quand il est `ph_plus`.

Le type de correction est une **déclaration**, pas une mesure. Si l'utilisateur
raccorde de l'acide en déclarant `ph_plus` — inversion de bidon, erreur de
première mise en service, ou reprise d'un appareil par un tiers — alors :

1. Aucune garde basse n'est armée : ni le seuil 6,70, ni la butée brute 6,20.
2. La régulation demande d'alcaliniser ; la pompe injecte de l'acide ; le pH
   **chute**.
3. L'erreur PID **croît** au lieu de décroître : la demande augmente à chaque
   cycle.
4. L'emballement n'est borné que par la limite journalière — et il **recommence
   le lendemain**, jour après jour, sans qu'aucun seuil ne s'y oppose.

Le contexte flotte aggrave le scénario : les destinataires ne liront aucun
journal et n'ont pas construit l'appareil.

## Défaut annexe à corriger dans la même spec

Le message d'escalade de `pump_controller.cpp` annonce « produit mal déclaré »
parmi les causes possibles du refus répété. **L'appareil ne détecte pas cette
cause aujourd'hui.** Le message affirme une capacité qu'il n'a pas — il oriente
l'utilisateur vers un diagnostic que le firmware n'a pas posé. Soit la détection
existe (objet de cette spec), soit la mention disparaît.

## Piste — détection d'incohérence effet/produit

Le principe : le pH doit bouger **dans le sens du produit injecté**. Un volume
significatif injecté suivi d'une dérive persistante à contresens est la signature
d'un produit inversé, et cette signature ne dépend d'aucune déclaration.

Points à instruire avec `pool-chemistry` :

- Volume minimal et fenêtre temporelle avant de conclure (le pouvoir tampon du
  bassin retarde l'effet ; l'inertie varie de plusieurs heures selon le TAC).
- Distinguer l'inversion réelle d'une perturbation externe : pluie, appoint
  d'eau, baignade, chloration choc manuelle.
- Que faire à la détection ? Bascule en manuel (comme l'escalade actuelle) ou
  simple alerte ? La bascule est cohérente avec le précédent, mais un faux
  positif désactive la régulation d'un utilisateur qui n'a rien fait de mal.
- Une **surveillance d'excursion basse armée quel que soit le type déclaré**
  est la variante minimale : elle ne diagnostique pas l'inversion mais borne sa
  conséquence. Coût nettement moindre, à arbitrer contre la détection complète.

## Éléments visibles

- Nouvelle alerte MQTT et entrée de journal en cas d'incohérence détectée
- Message d'escalade corrigé (retrait ou justification de « produit mal déclaré »)
- Éventuel bandeau UI

## Critères de clôture

- Scénario « pH+ déclaré, acide raccordé » simulé sur 3 jours : l'emballement est
  borné et signalé.
- Aucun faux positif sur les séries réelles de perturbation (pluie, appoint).
- Le message d'escalade ne mentionne plus aucune cause que l'appareil ne sait pas
  établir.
