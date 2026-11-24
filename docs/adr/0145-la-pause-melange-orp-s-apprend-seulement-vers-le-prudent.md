# ADR-0145 — La pause mélange ORP s'apprend, seulement dans le sens prudent

- **Statut** : Accepté
- **Date** : 2026-11-23
- **Décideurs** : Nicolas + Claude (`architect`, `pool-chemistry` — P1 sur le design en trois tours, P2 sur le diff)
- **Spec(s) liée(s)** : feature-202 ; patron d'apprentissage : feature-085 ; pause créée par feature-025 ; contrainte pH : [ADR-0017](0017-logique-metier-pure-humble-object-testabilite.md) (logique pure)

## Contexte

Après une injection de chlore, l'appareil s'interdit de réinjecter pendant une pause de 20 min (`kOrpMixingDelayMs`), identique pour tout bassin. Sur le bassin de référence, la mesure met ~88 min à finir de monter (relevé du 18/11) : la pause autorise donc une seconde dose par-dessus une première qui n'a exprimé qu'une partie de son effet.

C'est la première fois qu'un **paramètre de sécurité de dosage** deviendrait une grandeur **apprise** et non fixée. Contraintes posées par l'utilisateur : l'appareil est **vendu** pour une piscine quelconque (rien ne dépend d'un bassin), et le système doit rester simple (« pas de systèmes à 5 seuils »).

## Décision

### 1. Apprendre, mais seulement vers le prudent

- **Plancher tenu par construction** : la pause ne descend jamais sous `kOrpMixingDelayMs` (écrêtage bas conservé en filet, testé).
- **Estimateur = MAXIMUM des 3 dernières observations propres**. ⛔ Ni médiane ni moyenne : la détection se déclenche au **premier creux du bruit**, donc chaque observation est biaisée **vers le court**. Un maximum corrige ce biais ; une médiane mélangerait un biais systématique à une dispersion aléatoire.
- **Plafond en MULTIPLE du plancher** : `kOrpMixingDelayLearnedMaxMs = 6 * kOrpMixingDelayMs`. ⛔ Jamais en minutes absolues : la valeur numérique (120 min) est déduite d'une constante du code, pas d'un bassin. Il est un garde-fou contre l'aberrant, pas une protection saisonnière.
- Sous 3 observations propres : comportement rigoureusement identique à l'existant (20 min).
- L'observation est une **comparaison de la mesure à elle-même**, par blocs de 20 min : zéro seuil en mV. La durée ne peut valoir que 20, 40, 60, 80, 100 ou 120 min, arrondie vers le haut.

**La pause pH reste figée** (`kPhMixingDelayMs`, 15 min). Son terme intégral est verrouillé sur cette constante par des `static_assert` (`src/constants.h`) ; la rendre variable casserait ces preuves. Les deux pauses sont pourtant **jumelles dans le code** (armées à quatre lignes d'écart, transmises dans le même tableau) : l'exclusion du pH est une barrière contre un dégât collatéral, pas une position chimique.

### 2. La fenêtre de comparaison est la CONSTANTE, pas la durée en vigueur

Si la fenêtre suivait la durée apprise, toute observation future serait mécaniquement **≥ la durée courante** : cliquet irréversible, la pause ne pourrait plus jamais redescendre. Avec la constante, le « maximum des trois dernières » reste un vrai maximum **glissant**. Conséquence assumée : résolution de 20 min, arrondi vers le haut.

### 3. Le repli fail-closed arme la pause, uniformément sur les deux voies

`dosingWindowResolveChannel()` armait `mixingEndMs = 0` dans son repli fail-closed, en comptant sur le verrou horaire (60 min). Pour l'ORP, une pause apprise peut dépasser 60 min : **trou réel, indépendant de l'apprentissage** (le verrou ne couvrait déjà pas une pause > 60 min). Le repli arme désormais la durée en vigueur sur les **deux** voies. Pour le pH (15 min < 60 min), aucune autorisation ne change : seul le libellé de cause peut différer — argument **exécuté** par `test_21`.

### 4. Une pause en cours au redémarrage repart à la durée EN VIGUEUR

À la restauration, la pause repart à la durée **en vigueur**, pas à celle qui l'a armée : elle ne peut qu'allonger, et on évite de persister une seconde grandeur. Le format persisté de la pause est un **reste en millisecondes** : **aucun changement de schéma NVS/RTC** pour la pause. Le seul schéma neuf est l'instantané d'apprentissage : 20 octets, CRC FNV-1a, clé `orpmix` (namespace `pool-dose`), ⛔ sans empreinte de contexte ni péremption (grandeur **hydraulique**, ni consigne ni produit ne la changent). L'ordre de démarrage est contraint : `beginOrpMixingLearn()` **avant** `dosingWindow.begin()`.

## Alternatives considérées

- **Franchissement de la consigne** (rejetée) — mesure le surdosage, pas le mélange : plus la dose est excessive, plus la pause raccourcit. Inerte sur le cas du 18/11.
- **Seuil en mV / médiane 7 jours / empreinte de contexte** (rejetée) — seuil calé sur un bassin, mécanisme à cinq pièces.
- **Médiane ou moyenne des observations** (rejetée) — voir §1.
- **Fenêtre variable = durée en vigueur** (rejetée) — cliquet, voir §2.
- **Plafond en minutes absolues** (rejetée) — voir §1.
- **Rendre aussi la pause pH apprise** (rejetée) — casse les `static_assert` du terme intégral.
- **Comparaison continue à la minute** (rejetée) — tampon, index et cadence : trois pièces de plus pour une précision dont une garde n'a pas besoin.

## Conséquences

### Positives
- L'appareil devient plus patient là où le mélange est lent, jamais moins qu'avant.
- Trou du repli fail-closed fermé (indépendant de l'apprentissage).
- Visible : durée, origine et nombre d'observations servis sur `/data` et WS.

### Négatives / dette assumée
- ⚠️ **L'apprentissage peut rester inerte** sur un bassin très demandeur : toute injection — **y compris d'acide** — salit l'observation et la fait jeter. *(Amendé par bug-216, 2026-11-24 : seule la **marche** de la pompe pH salit désormais l'observation, plus la séance de pH ouverte — elle durait jusqu'à 3 h pour quelques minutes de pompe. Voir la fiche bug-216.)* Le rejet est chimiquement justifié (acidifier augmente la part active du chlore et fait monter la mesure sans ajout de chlore), mais ralentit la convergence. L'inertie reste **constatable** : le compte d'observations est affiché, les motifs de rejet sont journalisés.
- ⚠️ **Au plafond, l'observation est jetée en silence** : la pause reste au plancher. L'asymétrie est **défavorable** : l'appareil est le moins patient là où le mélange est le plus lent. Renvoyé à une fiche de suivi, **non traité ici**.
- Ce lot **ne supprime pas** les dépassements : celui du 18/11 vient de la taille de la dose, pas de la pause.

### Ce que ça verrouille
- Tout futur paramètre de sécurité appris suit la même règle : borne prudente tenue par construction, estimateur du côté prudent, plafond exprimé relativement à une constante.
- `kOrpMixingDelayMs` fait trois métiers (plancher, fenêtre, borne basse des observations) : le changer change les trois.
- Le seul `/1000` est dans `getOrpMixingDelayS()` (parité `/data` ↔ WS).

## Références

- Code : `src/orp_mixing_learn_logic.h`, `src/pump_controller.cpp` (`beginOrpMixingLearn`, `tickOrpMixingLearn`, `notifyOrpDose`), `src/dosing_window_logic.h` (repli fail-closed), `src/main.cpp` (ordre de démarrage), `src/constants.h` (`kOrpMixingDelayLearnedMaxMs`)
- Tests : `test/test_native_orp_mixing_learn/`
- Spec : `specs/features/doing/feature-202-la-pause-melange-orp-s-apprend-et-s-affiche.md`
- Doc : [`docs/subsystems/pump-controller.md`](../subsystems/pump-controller.md)
