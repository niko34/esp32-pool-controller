# ADR-0098 — Toute constante de la chaîne de mesure déclare sa source

- **Statut** : Accepté
- **Date** : 2026-10-11
- **Décideurs** : utilisateur (exigence posée explicitement, applicable à toutes les piscines),
  `architect`, `pool-chemistry` (vérification des sources, P1 et P2 de feature-152)
- **Spec(s) liée(s)** : [`feature-152`](../../specs/features/done/feature-152-refonte-de-la-chaine-de-mesure.md) (déclencheur), [ADR-0097](0097-le-filtre-lisse-le-dosage-juge.md) (application immédiate)

## Contexte

La bande de cohérence de l'ancienne sortie de secours du filtre pH valait **0,20 pH**. Le bruit
réellement mesuré sur le bassin de référence valait **0,36 pH**. Un réglage choisi pour un
signal qui n'était pas celui qu'on mesurait, donc **inopérant** — et personne ne pouvait le
voir sans comparer deux nombres situés à des milliers de lignes l'un de l'autre, dans deux
fichiers différents, écrits à des mois d'écart.

Ce défaut n'est pas un hasard isolé : c'est la conséquence mécanique d'une pratique de
conception jamais interdite jusqu'ici — calibrer une constante sur une **observation de
terrain** (une capture de bruit, une campagne de mesure sur *ce* bassin). Une telle constante
fonctionne tant que l'installation qui a servi à la calibrer reste représentative, et cesse de
fonctionner — silencieusement — dès qu'elle ne l'est plus. L'utilisateur l'a formulé comme une
exigence générale, pas seulement pour ce lot : « Aucun des principes ne doit être spécifique à
ma piscine. Ça doit être générique pour fonctionner dans toutes les piscines. »

Cette décision est délibérément **séparée** de l'[ADR-0097](0097-le-filtre-lisse-le-dosage-juge.md)
(qui documente le retrait des verrous collants) parce qu'elle **survit** au lot qui l'a fait
émerger : c'est une règle de conception opposable à **toute spec future** touchant une
constante de mesure, pas une décision contenue dans une seule refonte.

## Décision

**Toute constante de la chaîne de mesure (capteurs, filtrage, seuils physiques) doit être
justifiable par l'une de ces trois sources, et la spec ou le commentaire de code qui
l'introduit doit dire laquelle :**

1. **La physique** — une loi ou une plage qui ne dépend d'aucune installation particulière
   (plage pH 0-14, plage de sortie électrique d'une électrode ORP, loi de diffusion thermique).
2. **Une décision produit déjà prise** — une tolérance de régulation, une plage de consigne,
   un arbitrage de sécurité déjà acté ailleurs et simplement repris (ex. plage de consigne pH
   utilisée pour dériver une butée, [ADR-0048](0048-seuils-ph-fixes-derives-de-la-plage-de-consigne.md)).
3. **Le matériel** — une caractéristique du capteur ou de l'électronique qui ne varie pas
   d'une piscine à l'autre (cadence de lecture possible d'un module EZO, résolution/LSB d'une
   sonde, délai de réponse documenté par le fabricant).

**Est interdite** : toute constante dont la valeur provient d'une mesure de bruit, d'une
capture de terrain, ou d'un « ça marche bien chez nous ». **Si un mécanisme ne peut pas être
réglé sans mesurer une installation particulière, c'est le mécanisme qu'il faut retirer, pas
la constante qu'il faut ajuster** — c'est exactement le choix qui a produit l'ADR-0097 : la
sortie de secours du filtre n'avait pas de source légitime pour sa bande de cohérence ; elle a
été retirée plutôt que recalibrée.

**Corollaire pour les tests** : une fixture de signal réel (ex.
`test/fixtures/f097_purge_noise_20260920.csv`) sert exclusivement à **vérifier** qu'une
conception tient sur du signal réel. Elle ne doit **jamais** servir à **choisir** une valeur.
Un chiffre dont la justification serait « mesuré sur la fixture » est à rejeter en revue.

**Application immédiate (feature-152)** — les 12 constantes restantes du domaine sont sourcées
explicitement dans [`src/constants.h`](../../src/constants.h) :

| Constante | Source déclarée |
|---|---|
| `kSensorFilterMedianWindow`/`Max` (= 9) | décision produit (dynamique du bassin, validée par le gate) |
| `kSensorFilterEmaTauMs` (= 45 000 ms) | décision produit (dynamique du bassin) |
| `kPhFilterMin`/`Max` (0–14) | physique (échelle pH) |
| `kOrpFilterMin`/`Max` (±1019,9 mV) | matériel (plage de sortie EZO-ORP) |
| `kSensorFilterMaxAgeMs` (= 20 000 ms) | matériel (couvre un échec I²C isolé) |
| `kSensorFrozenSamples` (= 30) | décision produit (borne haute de N) |
| `kSensorFrozenEpsilonPh`/`Orp` | matériel (½ LSB du capteur) |
| `kDailyLimitStreakAlertDays` (= 3) | décision produit |

⚠️ **Contre-exemple identifié par le gate, non corrigé dans ce lot** : [ADR-0062](0062-porte-de-session-ph-dimensionnee-par-le-bruit-de-mesure.md)
(porte de session pH) dimensionne sa fenêtre de décision par le bruit de mesure **de
l'installation de référence** (σ = 0,043, τc ≈ 50 s) — exactement le type de source que cette
règle interdit. L'ADR-0062 **n'est pas superseded** par cette décision (ses chiffres restent
valides pour l'installation qui les a produits et aucun incident ne l'a remis en cause), mais
il est **signalé ici** comme un contre-exemple à traiter par une **spec de suivi distincte**,
hors périmètre de feature-152.

## Alternatives considérées

- **Ne poser aucune règle, corriger au cas par cas** — **rejetée** : c'est l'absence de règle
  qui a produit le défaut initial (bande de cohérence 0,20 contre bruit réel 0,36) ; sans
  obligation de déclarer la source, rien n'empêche la prochaine spec de recommettre la même
  erreur sous une autre forme.
- **Autoriser une quatrième source, « mesure de référence documentée et datée »** — **rejetée**,
  explicitement : c'est précisément le mécanisme qui a échoué. Une mesure documentée et datée
  reste une mesure d'**une** installation à **un** instant ; la documenter ne la rend pas
  générique.
- **Limiter la règle au seul module `sensor_filter`** — **rejetée** : le défaut qui l'a motivée
  n'est pas propre à un module, c'est une pratique de conception. La règle est posée comme
  opposable à toute constante de la chaîne de mesure, présente ou future.

## Conséquences

### Positives
- Une revue de code peut désormais rejeter une constante sur un critère objectif (« quelle est
  sa source ? ») plutôt que sur une impression de familiarité avec le bassin de test.
- Le lien entre une constante et sa justification devient **local** (commentaire au site de la
  déclaration), là où il fallait auparavant croiser deux fichiers distants pour le vérifier.

### Négatives / dette assumée
- La règle ne corrige pas rétroactivement les constantes existantes hors du périmètre de
  feature-152 — [ADR-0062](0062-porte-de-session-ph-dimensionnee-par-le-bruit-de-mesure.md) en
  est le contre-exemple identifié, non traité ici.
- Une constante légitimement dérivée du matériel peut encore se révéler fausse si la
  documentation du fabricant est elle-même imprécise (exemple relevé pendant ce lot : le temps
  de rafraîchissement réel d'un module EZO doit être vérifié, pas supposé) — la règle impose de
  déclarer la source, elle ne dispense pas de la vérifier.

### Ce que ça verrouille
- Toute spec future qui introduit ou modifie une constante de la chaîne de mesure doit
  remplir le tableau de justification (source : physique / décision produit / matériel) —
  c'est un livrable désormais attendu par construction, pas une option.
- Une fixture de signal réel ne peut plus servir de justification de valeur dans aucune revue
  de code de ce domaine — seulement de vérification.

## Références

- Spec : [`specs/features/done/feature-152-refonte-de-la-chaine-de-mesure.md`](../../specs/features/done/feature-152-refonte-de-la-chaine-de-mesure.md)
- Code : [`src/constants.h`](../../src/constants.h) (bloc `kSensorFilter*`/`kPhFilter*`/`kOrpFilter*`/`kSensorFrozen*`/`kDailyLimitStreakAlertDays`, chacune commentée de sa source)
- ADR lié : [ADR-0097](0097-le-filtre-lisse-le-dosage-juge.md) (application immédiate), [ADR-0062](0062-porte-de-session-ph-dimensionnee-par-le-bruit-de-mesure.md) (contre-exemple signalé, non corrigé)
