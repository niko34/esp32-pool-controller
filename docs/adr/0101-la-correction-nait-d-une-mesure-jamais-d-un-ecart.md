# ADR-0101 — La correction naît d'une mesure, jamais d'un écart

- **Statut** : Accepté
- **Date** : 2026-10-11
- **Décideurs** : `architect` (feature-155, critères 1, 3 et 4 — contrainte sur d'autres parties du système, alternatives crédibles, verrouille une orientation), gate `pool-chemistry` (deux passages)
- **Spec(s) liée(s)** : [`feature-155`](../../specs/features/done/feature-155-accorder-l-appareil-sur-un-test-manuel.md)

## Contexte

Jusqu'en v2.70.0, l'utilisateur corrigeait la mesure pH en réglant directement un
**écart abstrait** (« offset pH », `−0,50` à `+0,50`) via un pas-à-pas. Ce nombre
ne correspond à rien qu'il puisse observer : il doit lui-même calculer la
différence entre ce que son test (bandelette, PoolLab, pH-mètre) lui dit et ce
que l'appareil affiche, puis traduire cette différence en un réglage signé.

Demande utilisateur du 2026-10-11, mot pour mot : « *on ne règle plus un offset.
On donne une valeur, qui déterminera l'offset.* » L'utilisateur ajoute ensuite
une seconde contrainte, après avoir vu la maquette initiale : le plan de
l'`architect` prévoyait que le **navigateur** calcule l'écart
(`écart = référence − valeur affichée`) et envoie un `ph_offset` déjà calculé au
serveur. Le gate `pool-chemistry` a bloqué ce point au premier passage (condition
1, bloquante) : un écart calculé côté client, sur une mesure qui a eu le temps de
vieillir pendant que l'utilisateur hésite, ajuste, relit sa bandelette, est
**inauditable** — impossible de reconstituer après coup quelle mesure a
réellement servi de base.

Deux décisions distinctes sont donc en jeu, et elles se renforcent : **ce que
l'utilisateur fournit** (une mesure, jamais un écart) et **où le calcul a lieu**
(le firmware, jamais le navigateur, et sur la valeur **brute** qu'il lit lui-même
à l'instant de la réception).

### Ce que ce lot a découvert en le construisant

Quatre défauts distincts, trouvés **uniquement par confrontation des deux bouts
de la chaîne** (jamais par relecture), partageaient la même racine : confondre la
valeur **affichée** (arrondie, écrêtée) avec la valeur qui **sert au calcul**
(brute, non arrondie) :

1. **Un refus structurellement inatteignable côté firmware.** Le premier jet
   testait la borne ±0,50 sur la **sortie** de `roundPhOffsetToStep()`, qui
   **clampe avant d'arrondir** — sa sortie est donc *toujours* dans la plage
   autorisée. Un écart demandé de +0,80 n'était **jamais refusé** : il était
   silencieusement **écrêté à +0,50 et appliqué**, l'exact inverse du critère de
   la spec.
2. **Un aperçu côté navigateur qui divergeait du verdict du firmware.** L'aperçu
   comparait des **nombres de pas entiers** (`Math.round(écart/pas)` contre un
   nombre de pas max) là où le firmware compare l'**écart brut non arrondi** aux
   bornes. Pour tout écart demandé dans `]0,50 ; 0,55]`, l'aperçu arrondissait à
   5 pas, concluait « dans la plage » et activait le bouton — le serveur, lui,
   refusait en 400. Un bouton actif qui mène à un refus est un bouton qui ment.
3. **Un journal qui traçait l'écart écrêté**, masquant l'ampleur réelle de la
   demande : une tentative à +0,80 était journalisée « écart 0,50 hors plage »
   au lieu de « écart 0,80 hors plage » — rendant le diagnostic impossible
   précisément dans le cas où il sert.
4. **Zéro appui pouvait déplacer la correction d'un cran.** La position de
   départ du pas-à-pas est la valeur **affichée** (`ph`, arrondie au dixième) ;
   le calcul d'écart se fait contre `phValueRaw` (la valeur **brute**, non
   arrondie). Si la valeur affichée tombe sur un demi-dixième par arrondi
   (ex. brut 7,150 + offset 0,30 = 7,450 → affiché 7,5), l'écart implicite entre
   le départ affiché et le brut réel vaut 0,35 pH → 0,40 appliqué, **sans aucune
   action de l'utilisateur**.

Un cinquième écart, de nature différente (divergence d'arrondi JS/C sur les
valeurs négatives — voir plus bas), a été trouvé par la même méthode de
confrontation et fermé par le même principe.

## Décision

### 1. L'utilisateur indique une mesure, jamais un écart

L'interface ne propose **aucun réglage d'écart**. Le pas-à-pas existant est
**réaffecté** (pas supprimé) : il affiche désormais la **valeur que l'utilisateur
amène** (sa lecture de test), jamais un nombre signé abstrait. Le mot « offset »
disparaît du produit. Le vocabulaire retenu — **ajuster**, **amener à**,
**indiquer**, **corriger** — exclut explicitement tout champ libre ou clavier
(« il n'y a pas de saisie », décision utilisateur du 2026-10-11).

### 2. Le calcul part de la valeur officielle en échelle BRUTE servie par le firmware, jamais d'un écart envoyé par le client

Le corps de `POST /tune-ph-offset` porte `{"ph_reference": <valeur amenée>}` —
jamais un offset pré-calculé. Le firmware calcule :

```
offset = roundPhOffsetToStep(ph_reference − sensors.getPhValueRaw())
```

à l'**instant de la réception** de la requête, avec **sa propre mesure**. La
formule naïve « cible − valeur affichée » est **fausse dès que la correction
n'est pas nulle** : `ph`, `phFiltered` et `phMedian` sont **déjà corrigés** de
l'offset courant (`sensors.cpp`) — partir de l'un d'eux ajouterait l'écart
demandé à un écart déjà appliqué. Ce défaut est **invisible à offset 0,00**
(celui de l'utilisateur au moment d'écrire cette spec) et **indétectable à la
recette tant que la correction vaut 0,00** : il n'apparaît qu'au **second**
accord. C'est pourquoi le calcul part exclusivement de `getPhValueRaw()` — la
valeur officielle (même lissage que `ph`), mais **avant** l'offset — exposée au
client sous le nom `phValueRaw` pour que l'aperçu utilise la **même** référence
que le calcul serveur.

## Alternatives considérées

- **Calcul côté navigateur, le client envoie `ph_offset` déjà calculé**
  (proposition initiale de l'`architect`) — **rejetée par le gate
  `pool-chemistry`** (condition 1, bloquante) : inauditable, la référence a
  vieilli pendant le geste de l'utilisateur.
- **Garder un réglage d'écart direct, changer seulement le libellé** (« ajustement »
  au lieu d'« offset ») — **rejetée** : ne répond pas à la demande utilisateur, qui
  porte explicitement sur le geste (« on donne une valeur »), pas sur le
  vocabulaire seul.
- **Référence figée au moment où l'utilisateur ouvre l'écran, avec
  reconfirmation si l'écart dépasse 0,05 pH avant validation** — **rejetée** :
  sur ce projet, la valeur filtrée fluctue de **±0,08 à 10 minutes** (dispersion
  pH mesurée, courants de fuite de la pompe de filtration) — **au-dessus** du
  seuil de reconfirmation envisagé. Elle redemanderait confirmation à presque
  chaque geste.
- **Demander l'instrument utilisé** (bandelette / PoolLab / pH-mètre) pour
  pondérer la confiance — **écartée** par le gate : remplacée par un texte
  explicite disant que la valeur indiquée sert au dosage, pas seulement à
  l'affichage (condition B4).

## Conséquences

### Positives
- Le calcul est **reproductible et auditable** : une même paire (valeur amenée,
  mesure brute à l'instant T) produit toujours le même verdict, quel que soit le
  délai de réflexion de l'utilisateur.
- L'utilisateur ne fait plus de soustraction mentale : il compare deux nombres
  qu'il peut observer (ce qu'il lit sur son test, ce que l'appareil affiche).
- Le piège de la double correction (partir d'une valeur déjà corrigée) est fermé
  **structurellement** : `evaluatePhTune()` (`src/ph_tune_logic.h`) ne reçoit
  jamais `ph`/`phFiltered`/`phMedian`, seulement `phValueRaw`.

### Négatives / dette assumée
- Un champ supplémentaire (`phValueRaw`) sur `/data` et la trame WebSocket, déjà
  serrée en budget — marge résiduelle réduite de 228 à 208 o (voir
  [docs/subsystems/ws-manager.md](../subsystems/ws-manager.md)).
- La route dédiée (`POST /tune-ph-offset`) duplique partiellement la sémantique
  de `ph_offset` sur `POST /save-config` (qui reste le contrat de **stockage**,
  déjà calculé) — deux chemins d'écriture pour le même champ NVS, documentés
  séparément pour ne pas laisser le sens du champ dériver selon l'appelant.
- Le résidu d'arrondi au dixième du pas-à-pas n'est pas supprimé par cette
  décision (cf. feature-155, « Piège 1 ») : amener le nombre pile sur la cible
  peut laisser un écart résiduel jusqu'à 0,05 pH — accepté car dix fois plus fin
  que la bandelette de l'utilisateur (±0,5 pH).

### Ce que ça verrouille
- **Tout futur geste de correction de mesure** (pH ou un autre capteur qui
  suivrait le même patron) part d'une **mesure de référence fournie par
  l'utilisateur**, jamais d'un écart qu'on lui demande de calculer lui-même.
  Revenir à un réglage d'écart direct demanderait de rouvrir cette décision
  explicitement, pas de l'ignorer en silence.
- **Tout calcul qui entre dans une décision de dosage et dépend d'une mesure
  fournie par l'utilisateur doit se faire côté firmware**, avec une mesure lue
  à l'instant de la réception — jamais sur une valeur pré-calculée transmise par
  un client. Un aperçu client reste toujours un aperçu, jamais l'autorité.
- **Un seuil ou une borne de sécurité testée côté serveur doit porter sur la
  grandeur AVANT toute fonction qui l'arrondit, l'écrête ou la clampe** — jamais
  sur sa sortie. C'est la cause du défaut n°1 ci-dessus, et une classe d'erreur
  qui peut réapparaître sur n'importe quel futur test de borne du projet.

## Une leçon transférable : confondre l'affiché et le calculé

Les quatre défauts listés en contexte ne sont pas quatre bugs indépendants :
c'est le **même** défaut, sous quatre formes, trouvé **uniquement par
confrontation des deux bouts de la chaîne** (aperçu client vs décision serveur ;
texte de refus vs valeur réellement comparée ; position affichée du pas-à-pas vs
valeur servant au calcul) — jamais par relecture du code pris isolément. Les deux
moitiés d'un même calcul étaient, à chaque fois, **justes séparément**.

D'où le job CI `ph-tune-vectors` ([`tools/check_ph_tune_vectors.sh`](../../tools/check_ph_tune_vectors.sh),
workflow `hygiene-specs.yml`) : il extrait le texte réel d'`evaluatePhTunePreview()`
(`data/app.js`, patron ADR-0099) et l'évalue contre des vecteurs qui comparent
explicitement son verdict à celui du firmware (`evaluatePhTune()`,
`src/ph_tune_logic.h`) sur les mêmes entrées — borne exacte, borne +0,01 au-delà,
offset déjà non nul, mesure indisponible. C'est un verrou de **cohérence entre
deux chemins de code qui ne se touchent jamais en production**, pas un simple
test unitaire de plus : c'est la seule façon de refermer mécaniquement cette
classe de défaut.

### Deux fermetures connexes, trouvées par la même méthode

- **Divergence d'arrondi JavaScript / C sur les valeurs négatives.**
  `Math.round()` arrondit les demis **vers +∞** (`Math.round(-3.5) === -3`), le
  `roundf()` du firmware arrondit **à l'opposé de zéro**
  (`roundf(-3.5f) === -4`). Un écart négatif tombant pile à mi-pas (ex. −0,35)
  aurait donné −0,30 à l'écran et −0,40 dans l'appareil — encore l'aperçu et le
  verdict qui divergent. L'aperçu reproduit désormais explicitement la règle du
  C (`enPas < 0 ? -Math.round(-enPas) : Math.round(enPas)`), commentée comme
  telle dans `data/app.js`.
- **Verrou à la compilation sur `kPhTuneMinFiltrationMs`.** Les messages de
  refus imposés mot pour mot par le gate `pool-chemistry` écrivent « 10 minutes »
  **en toutes lettres**, des deux côtés (`src/web_routes_config.cpp` et
  `data/app.js`). Rendre le texte dynamique aurait cassé une formulation
  validée ; le projet verrouille donc la **constante** à la place, par un
  `static_assert` (`constants.h`) qui casse la compilation si
  `kPhTuneMinFiltrationMs` change sans que les deux textes soient relus et
  repassés par `pool-chemistry`.

## Références

- Code : [`src/ph_tune_logic.h`](../../src/ph_tune_logic.h) (`evaluatePhTune`, logique pure),
  [`src/web_routes_config.cpp`](../../src/web_routes_config.cpp) (`handleTunePhOffset`),
  [`src/constants.h`](../../src/constants.h) (`kPhTuneMinFiltrationMs`, `kPhTuneCalPointsMin`),
  [`src/sensors.cpp`](../../src/sensors.cpp) (`getPhValueRaw`),
  [`data/app.js`](../../data/app.js) (`evaluatePhTunePreview`, `renderPhTuneCard`)
- Test : [`test/test_native_ph_tune/`](../../test/test_native_ph_tune/) (logique serveur, natif),
  [`test/ph_tune_vectors.js`](../../test/ph_tune_vectors.js) + [`tools/check_ph_tune_vectors.sh`](../../tools/check_ph_tune_vectors.sh)
  (job CI `ph-tune-vectors`, cohérence aperçu ↔ serveur)
- Spec : [`specs/features/done/feature-155-accorder-l-appareil-sur-un-test-manuel.md`](../../specs/features/done/feature-155-accorder-l-appareil-sur-un-test-manuel.md)
- Doc externe : [`docs/API.md`](../API.md#post-tune-ph-offset--critical),
  [`docs/features/page-ph.md`](../features/page-ph.md#carte--correction-manuelle-de-la-mesure--ph-card-tune-feature-155-v2710),
  [`docs/subsystems/ws-manager.md`](../subsystems/ws-manager.md)
- Précédents amendés/réemployés : [ADR-0039](0039-offset-ph-applique-en-sortie-des-accesseurs.md)
  (point d'application de l'offset, inchangé par cette décision),
  [ADR-0099](0099-tester-par-extraction-de-texte-jamais-par-duplication.md) (patron
  d'extraction réutilisé pour `evaluatePhTunePreview`)
