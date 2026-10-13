# ADR-0105 — Un indicateur de transitoire d'étalonnage n'est pas une porte de mesure

- **Statut** : Accepté
- **Date** : 2026-10-13
- **Décideurs** : gate `pool-chemistry` (GO sous 17 conditions, puis GATE 2 et GATE 3), `architect`,
  `embedded-developer`, `web-ui-developer`
- **Spec(s) liée(s)** : [`bug-160`](../../specs/features/done/bug-160-l-indicateur-de-stabilite-ment-et-le-dosage-ne-se-suspend-pas.md)

## Contexte

`bug-160` (volet 1) remplace le critère « Stable — vous pouvez calibrer » de l'écran de
calibration pH/ORP (feature-034) : l'ancien seuil (`max − min` sur 20 s, 0,9 pH) annonçait
« stable » à 0,8-1,1 pH de l'équilibre réel de l'électrode — treize fois le bruit de lecture
invoqué pour le justifier.

Dix jours plus tôt, [ADR-0092](0092-retrait-analyse-stabilite-porte-mesures.md) a **retiré** un
mécanisme de nature très proche en apparence : un détecteur d'étendue qui gardait la porte des
mesures représentatives du bassin après un arrêt d'eau. Le verdict de ce retrait était que
**tout** détecteur d'étendue est structurellement aveugle à une mesure durablement fausse mais
stable, et que son coût (régulation bloquée sur une simple fluctuation) dépassait le risque qu'il
couvrait.

Sans cette clarification, un futur lot pourrait lire `bug-160` comme une régression de la même
doctrine — réintroduire un détecteur d'étendue après l'avoir retiré dix jours plus tôt — et le
retirer au motif d'ADR-0092, sans examiner si les deux mécanismes sont réellement comparables.

## Décision

**L'indicateur de stabilité de calibration n'est pas un détecteur de stabilité au sens
d'ADR-0092, et la clause de ce dernier ne s'y applique pas**, pour quatre raisons cumulatives,
chacune suffisante à elle seule à distinguer les deux cas :

1. **Ce n'est pas une porte, c'est un indicateur.** Le bouton « Calibrer » n'est **jamais**
   désactivé pour cause d'instabilité (condition posée dès la spec, confirmée par le gate) — une
   sonde dégradée qui ne redescend jamais sous le seuil ne produit aucune impasse, contrairement
   au détecteur retiré par ADR-0092, qui bloquait le dosage automatique.

2. **L'objet mesuré est inversé.** ADR-0092 répondait à la question « cette mesure est-elle
   représentative du bassin ? » — un état **permanent**, où un détecteur d'étendue est aveugle
   par construction (une mesure fausse mais stable passe l'étendue). Ici la question est
   « l'électrode a-t-elle fini son transitoire, dans une solution dont la valeur vraie est **déjà
   connue** (le tampon d'étalonnage) ? ». Le **transitoire lui-même** est le mesurande, pas un
   obstacle à une mesure stable recherchée ailleurs.

3. **La statistique a changé de nature.** L'ancien critère (ADR-0092 comme l'ancien seuil de
   `bug-160`) était une **étendue** (`max − min`), qui croît avec le nombre d'échantillons et
   mélange bruit et dérive. Le nouveau critère est un **estimateur de vitesse de dérive** —
   dérive médiane moitié récente / moitié ancienne (`D`), gardée par une inégalité sur les temps
   **réellement observés** (`G ≥ G_MIN`, voir [`docs/subsystems/sensors.md`](../subsystems/sensors.md#indicateur-de-stabilité-de-calibration-bug-160)) —
   insensible au bruit et au nombre d'échantillons. Un vecteur purement bruité sans dérive donne
   « stable », là où l'ancienne étendue aurait fini par donner « instable » avec assez
   d'échantillons.

4. **Les constantes ont une source opposable, et le résiduel garanti est calculé et tenu par
   vecteurs.** Chaque constante du nouveau critère déclare sa source ([ADR-0098](0098-toute-constante-de-mesure-declare-sa-source.md))
   — produit, physique ou matériel — et `G_MIN = SEUIL / RÉSIDUEL_MAX` ne laisse **aucun degré de
   liberté** : si un vecteur de test échoue, ce n'est structurellement jamais le seuil qu'il faut
   assouplir, c'est la fenêtre ou la cadence qu'il faut durcir.

### Clause anti-retrait

> Un futur lot qui voudrait retirer ce mécanisme au motif d'ADR-0092 doit d'abord constater
> lequel des quatre points ci-dessus a cessé d'être vrai. S'il bloque quelque chose, c'est le
> blocage qu'il faut retirer, pas l'indicateur.

## Alternatives considérées

- **Fusionner cette doctrine dans ADR-0092** (rejetée) — ADR-0092 est déjà **Accepté** et
  immuable ; y ajouter une clause après coup masquerait la distinction au lieu de l'exposer, et un
  lecteur qui ne rouvre pas ADR-0092 pour un sujet de calibration ne la trouverait jamais.
- **Ne pas écrire d'ADR, se fier au commentaire de code** (rejetée) — c'est exactement la pratique
  qu'ADR-0098 cherche à combattre : un arbitrage qui vit uniquement dans `data/app.js` n'est pas
  opposable à une revue future qui ne lirait que la spec ou le gate.
- **Décrire la distinction uniquement dans la spec `bug-160`** (rejetée comme suffisant) — une
  spec migre vers `done/` et n'est plus la source de vérité recherchée en premier pour un
  arbitrage architectural récurrent (feature-155, correctif du 11/10, feature-159 : l'arbitrage
  « commande vs information » revient déjà plusieurs fois, cf. [ADR-0106](0106-toute-suspension-demandee-par-un-client-est-bornee.md)
  pour le volet 2 de ce même bug).

## Conséquences

### Positives
- Une revue future dispose d'un critère objectif (les quatre points) pour trancher si un nouveau
  mécanisme de « stabilité affichée » relève d'ADR-0092 ou de cet ADR, au lieu de trancher sur une
  impression de ressemblance.
- Le résiduel garanti (pH **0,0911** à τ = 120 s, ORP **4,65 mV** à τ = 180 s) est documenté avec
  son arithmétique complète dans [`docs/subsystems/sensors.md`](../subsystems/sensors.md#indicateur-de-stabilité-de-calibration-bug-160),
  recontrôlable sans refaire le calcul du gate.

### Négatives / dette assumée
- Deux mécanismes de nature différente partagent le même vocabulaire de surface (« stabilité »,
  « seuil », « fenêtre ») — le risque de confusion décrit en Contexte reste réel pour qui ne lit
  pas cet ADR ; c'est précisément ce qu'il corrige, pas ce qu'il élimine.

### Ce que ça verrouille
1. **Un indicateur qui n'inhibe aucune action n'est jamais soumis à la doctrine d'ADR-0092**, qui
   ne vaut que pour une porte de mesure bloquant une décision de dosage.
2. **Toute statistique de « stabilité affichée » future doit déclarer explicitement si elle mesure
   un transitoire connu (ce cas) ou une représentativité permanente (cas d'ADR-0092)** — les deux
   exigent des outils statistiques opposés (vitesse de dérive vs détection d'étendue stable).
3. La clause anti-retrait ci-dessus est **opposable mot pour mot** : un lot qui invoquerait
   ADR-0092 pour retirer ce mécanisme sans adresser les quatre points est non conforme.

## Références

- Code : [`data/app.js`](../../data/app.js) (objet `CAL_STAB`, `_calStabilityVerdict()`,
  `pushStabilitySample()`)
- Doc : [`docs/subsystems/sensors.md`](../subsystems/sensors.md#indicateur-de-stabilité-de-calibration-bug-160),
  [`docs/features/page-ph.md`](../features/page-ph.md#les-six-états-de-lindicateur-de-stabilité-renderstability-bug-160)
- Spec : [`bug-160`](../../specs/features/done/bug-160-l-indicateur-de-stabilite-ment-et-le-dosage-ne-se-suspend-pas.md)
- ADR liés : [ADR-0092](0092-retrait-analyse-stabilite-porte-mesures.md) (distingué, non superseded — objet et nature de statistique différents), [ADR-0098](0098-toute-constante-de-mesure-declare-sa-source.md) (source des constantes du nouveau critère), [ADR-0106](0106-toute-suspension-demandee-par-un-client-est-bornee.md) (volet 2 du même bug)
