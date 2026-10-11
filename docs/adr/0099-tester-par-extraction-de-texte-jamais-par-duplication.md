# ADR-0099 — Tester la logique pure du navigateur par extraction de texte, jamais par duplication

- **Statut** : Accepté
- **Date** : 2026-10-11
- **Décideurs** : `architect` (feature-153, critère 3 — alternatives crédibles à documenter)
- **Spec(s) liée(s)** : [`feature-153`](../../specs/features/done/feature-153-signaux-capteurs-medianne-et-deux-cadences.md), précédent [`feature-118`](../../specs/features/done/feature-118-authenticite-de-l-image-ota.md) (`sha256_vectors.js`)

## Contexte

La carte « Signaux capteurs » (feature-153) introduit trois fonctions logiques pures côté
navigateur — `groupHistoryPoints()` (regroupement par fenêtre de 10 min, avec `null` au
milieu), `seriesStats()` (min/max/moyenne) et `traceWindowDurationMs()` (durée de couverture
dérivée de la cadence réelle, jamais figée). Ce sont elles qui décident si un trou de mesure
reste un trou, si une fenêtre vide produit « rien » plutôt qu'un zéro inventé, et si la durée
affichée suit une cadence de lecture qui a déjà changé une fois (feature-152).

`pio test -e native` (ADR-0017) ne voit que du C++ : il ne peut ni exécuter ni vérifier un
fichier `.js`. Deux voies existaient pour donner à ce code une protection par mutation :
dupliquer la logique en C++ pur et la tester là, ou tester le JavaScript lui-même par un
moyen qui ne dépende ni du DOM ni d'un navigateur headless.

Un précédent existe déjà dans le dépôt, mais **isolé** : `test/sha256_vectors.js` +
`tools/check_sha256_vectors.sh` (feature-118) extraient et exécutent le SHA-256 pur JS de
`data/app.js` pour l'empreinte d'upload OTA. Jusqu'à ce lot, ce patron n'avait jamais été
généralisé, et sa sentinelle de câblage CI a elle-même été un point de défaillance : une
suite qui existe mais que personne n'appelle ne protège rien.

## Décision

**La logique pure qui vit dans `data/app.js` se teste en extrayant et en exécutant le texte
réel de ses fonctions, jamais en le recopiant en C++.**

`test/trace_logic_vectors.js` repère chaque fonction visée par une ancre de nom, puis compte
la profondeur des accolades (en ignorant chaînes et commentaires, même garde que
`check_fopen_guard.sh` et `sha256_vectors.js`) pour extraire le corps **exact** tel qu'il est
livré — jamais une paraphrase maintenue à la main. Le texte extrait est ensuite évalué sous
Node et comparé à des vecteurs couvrant les cas limites (fenêtres partielles, fenêtre
entièrement vide — aucun point produit, pas un point à zéro —, `null` au milieu d'une série
ignoré sans être compté, dépendance réelle de `traceWindowDurationMs()` à `intervalMs`).

`tools/check_trace_logic_vectors.sh` lance ce fichier et est **câblé** comme job `trace-logic-
vectors` de `hygiene-specs.yml` (onzième job du workflow) — la leçon du précédent isolé de
feature-118 : une sentinelle non appelée en CI ne tient que par la discipline de qui s'en
souvient.

**Contrainte induite sur l'implémentation (`B` de la spec)** : les trois fonctions visées
doivent rester **pures, nommées, sans dépendance au DOM ni à `authFetch`** — c'est la condition
pour qu'elles restent extractibles par ancre et profondeur d'accolades. Un refactor qui les
rendrait anonymes, ou qui les ferait lire `document`/`window` en cours de corps, casserait
l'extraction et ferait échouer le job CI au lieu de faire échouer silencieusement la carte.

## Alternatives considérées

- **Dupliquer la logique en C++ pur, testée nativement (ADR-0017)** — **rejetée** : crée **deux
  sources de vérité** pour un code qui ne tourne jamais ailleurs que dans le navigateur. La
  version C++ pourrait diverger de celle réellement livrée sans qu'aucun test ne le détecte —
  c'est précisément le défaut qu'une sentinelle est censée empêcher. Mentionné explicitement
  comme écarté dans la spec feature-153 (item D3).
- **Exécuter `data/app.js` entier dans un navigateur headless (Puppeteer/Playwright) et piloter
  le DOM** — **rejetée (non retenue pour ce lot)** : ajoute une dépendance lourde (binaire
  navigateur) à une CI qui tient aujourd'hui en « ni Python, ni PlatformIO, ni cache » (~1 s par
  job, voir [BUILD.md](../BUILD.md#intégration-continue-github-actions)) pour trois fonctions
  qui n'ont besoin d'aucun DOM.
- **Ne rien tester côté navigateur, se fier à la revue de code et au plan de test manuel** —
  **rejetée** : la spec impose explicitement des mutations qui doivent tuer un test (D2), chose
  impossible sans exécution automatisée du code réellement livré.
- **Généraliser le patron `sha256_vectors.js` à un harnais unique réutilisable pour toute future
  fonction pure du frontend** — **non retenue pour ce lot** (chaque extraction reste spécifique
  à ses ancres), mais c'est la direction naturelle si un troisième cas apparaît.

## Conséquences

### Positives
- Toute divergence future entre le code livré et ce qui est testé est **structurellement
  impossible** : le texte évalué est le texte extrait de `data/app.js`, pas une copie
  maintenue à la main.
- Le patron devient réutilisable : un troisième besoin de tester du JS pur suit la même
  recette (ancre + profondeur d'accolades + vecteurs + script `check_*` + job CI).

### Négatives / dette assumée
- L'extraction par ancre + comptage de profondeur est plus fragile qu'un import de module : un
  renommage de fonction, ou un remaniement qui change sa signature, casse l'extraction — par
  construction (le job doit échouer bruyamment plutôt que tester silencieusement autre chose).
- N'importe quelle fonction qui a besoin du DOM ou d'un état partagé ne peut pas suivre ce
  patron ; elle reste hors de portée de `pio test -e native` comme de ce harnais.

### Ce que ça verrouille
- Les trois fonctions `groupHistoryPoints`, `seriesStats`, `traceWindowDurationMs` doivent
  rester pures et nommées **sous ces noms exacts** dans `data/app.js` ; les renommer exige de
  mettre à jour les ancres de `test/trace_logic_vectors.js` dans le même geste.
- Toute future logique pure non triviale ajoutée à `data/app.js` et touchant à une donnée
  sensible (dosage, sécurité, affichage de mesure) est désormais attendue sous ce patron plutôt
  que laissée sans protection automatisée.

## Références

- Code : [`test/trace_logic_vectors.js`](../../test/trace_logic_vectors.js), [`tools/check_trace_logic_vectors.sh`](../../tools/check_trace_logic_vectors.sh), [`data/app.js`](../../data/app.js) (`groupHistoryPoints`, `seriesStats`, `traceWindowDurationMs`)
- Précédent généralisé : [`test/sha256_vectors.js`](../../test/sha256_vectors.js), [`tools/check_sha256_vectors.sh`](../../tools/check_sha256_vectors.sh) (feature-118)
- CI : job `trace-logic-vectors` de [`.github/workflows/hygiene-specs.yml`](../../.github/workflows/hygiene-specs.yml), documenté dans [BUILD.md](../BUILD.md#intégration-continue-github-actions)
- Spec : [`specs/features/done/feature-153-signaux-capteurs-medianne-et-deux-cadences.md`](../../specs/features/done/feature-153-signaux-capteurs-medianne-et-deux-cadences.md)
