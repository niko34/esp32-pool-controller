# feature-072 — Carte « Signaux capteurs » : courbe de l'évolution de la dispersion

## Métadonnées
- **ID** : feature-072
- **Slug** : courbe-dispersion
- **Type** : feature
- **Statut** : doing — implémenté (tests verts, revue Approved, docs à jour) ; reste la vérification manuelle on-target
- **Priorité** : normale
- **Version cible** : 2.20.8
- **Dernière mise à jour** : 2026-08-23
- **Spec(s) liée(s)** : feature-071 (lignes dispersion, reconstruction fenêtre 13 — prérequis livré en 2.20.7), feature-057 (trace capteur)

## Domaines touchés
- [x] Frontend (`data/`)
(aucun autre — feature 100 % frontend, comme la 071)

## Agents requis
- [x] spec-reader
- [x] architect
- [ ] pool-chemistry — non : affichage pur, aucun chemin vers le dosage
- [x] ux-ui-designer — oui : nouveaux graphes dans une carte déjà dense, arbitrage réel (graphes dédiés vs surcouche à double axe sur les graphes existants, placement, hauteur)
- [x] web-ui-developer
- [x] test-engineer
- [x] code-reviewer
- [x] doc-writer

## Description
Ajouter à la carte « Signaux capteurs » un graphique de l'évolution de la dispersion (fenêtre glissante de 13, la grandeur du drapeau `unstable`) sur la durée de la trace, **en valeur absolue** (décision utilisateur explicite : pas de normalisation en % du seuil), avec le seuil visible, pour le pH et pour l'ORP.

## Contexte
Depuis la 2.20.7, la carte affiche la dispersion instantanée (route) et son résumé sur la trace (médiane · max reconstruites). Il manque la **trajectoire** entre les deux : une médiane à 0,28 peut cacher un plateau tranquille comme des excursions répétées à 0,45 qui frôlent le drapeau ; seule la courbe les distingue, et elle rend lisibles les corrélations aux événements (démarrage de filtration, dosage) déjà visibles sur les graphes brut/lissé au-dessus. La série est déjà calculée par `_traceDispersionStats()` (data/app.js), qui n'en publie que la médiane et le max.

**Décision utilisateur actée** : échelle en **valeur** (pH d'un côté, mV de l'autre), PAS en pourcentage du seuil. Conséquence : deux tracés à échelles distinctes — l'arbitrage restant (graphes dédiés vs double axe sur les graphes existants) revient à ux-ui-designer.

## Comportement attendu
- Au chargement de la trace (mêmes déclencheurs qu'aujourd'hui : activation onglet Avancé, bouton « Recharger »), la ou les courbes de dispersion s'affichent : un point par fenêtre de 13 valide, positionné au temps du dernier échantillon de la fenêtre.
- **Ligne de seuil visible** sur chaque tracé (0,5 pH / 100 mV) : la marge se lit graphiquement. Source du seuil : `dispersion_max` de la route si disponible, sinon constante JS commentée (`kPhFilterMaxDispersion` / `kOrpFilterMaxDispersion`) — le tracé ne doit pas dépendre du succès du fetch route (indépendance B5 de la 071 conservée).
- L'échelle Y inclut toujours le seuil (une courbe à 0,05 sous un seuil à 0,5 doit montrer la marge, pas zoomer sur le bruit).
- **Fenêtres écartées** (null/NaN ou saut ≥ 1,0 pH / 300 mV, logique 071 inchangée) → trous dans la courbe (points null), PAS d'interpolation par-dessus : le trou est une information (transitoire rejeté).
- Moins de 13 points de trace → pas de courbe (comportement cohérent avec le tiret des stats).
- Les lignes texte existantes (dispersion instantanée route, médiane · max reconstruites) sont conservées telles quelles.
- Statut « indicatif » hérité de la reconstruction : le libellé/la légende du graphe doit le porter (cohérence avec la note UI 071 déjà en place).

### Cas limites
- Trace avec transitoire (saut > 1,0 pH) : la courbe montre un trou sur les 13 fenêtres couvrant le saut, pas un pic à 5.
- Buffer partiellement rempli après reboot : courbe courte, pas d'erreur.
- Échec du fetch route : le seuil tracé vient du repli JS, la courbe s'affiche quand même.

## Comportement actuel
`_traceDispersionStats(values, spikeReject)` (data/app.js, ~l.5972) calcule les dispersions par fenêtre et ne retourne que `{median, max, windows}` — la série est jetée. Aucun graphe de dispersion. Deux graphes uPlot existent (pH brut/lissé, ORP brut/lissé) via `createTraceChart()`.

## Contraintes techniques
- ESP32, offline first ; vanilla JS ; français ; pas de polling ajouté
- uPlot déjà embarqué — aucune nouvelle dépendance
- Budget FS : vérifier `./build_fs.sh` (layout v4, 320 KB)
- Réutiliser les patterns 071 : la série sort de `_traceDispersionStats` (refactor : retourner aussi la série `[{t, valeur|null}]` sans dupliquer le calcul)
- Alignement temporel : même axe X (secondes relatives) que les graphes existants

## Fichiers potentiellement concernés
- `data/index.html` — blocs graphes dispersion dans `#card-sensor-trace`
- `data/app.js` — refactor `_traceDispersionStats` (série exposée), création/alimentation des tracés, ligne de seuil
- `data/app.css` — si nécessaire (hauteur des nouveaux blocs)
- `src/version.h` — bump 2.20.8
- `docs/features/page-settings.md`, `CHANGELOG.md`

## Livrables — check-list
- [x] A1 — `src/version.h` → 2.20.8
- [x] B1 — Courbe dispersion pH en valeur absolue, ligne de seuil incluse, échelle Y englobant le seuil
- [x] B2 — Courbe dispersion ORP idem (mV)
- [x] B3 — Fenêtres écartées → trous (null), pas d'interpolation ; < 13 points → pas de courbe, pas d'erreur
- [x] B4 — Seuil tracé : `dispersion_max` route si chargée, repli constante JS commentée ; indépendance des deux fetchs conservée
- [x] B5 — Refactor `_traceDispersionStats` sans duplication du calcul ; lignes texte 071 inchangées
- [x] C1 — `docs/features/page-settings.md` mis à jour
- [x] C2 — `CHANGELOG.md` entrée 2.20.8
- [x] D1 — `pio run` sans nouveau warning + `pio test -e native` 609/609
- [x] D2 — `./build_fs.sh` ≤ 320 KB

## Nouvelles signatures / API
Aucune côté firmware. JS interne uniquement.

## Sécurité chimique
- Dosage impliqué : non — affichage en lecture seule.

## ADR attendu ?
- **Probable** : non
- **Critère(s) candidat(s)** : aucun
- **Sujet pressenti** : aucun

## Éléments visibles (pour doc-writer)
### Endpoints HTTP concernés
- Aucun nouveau
### Champs WebSocket / Topics MQTT
- Aucun
### Documentation à produire / mettre à jour
#### Docs vivantes
- `docs/features/page-settings.md` — carte « Signaux capteurs » : les courbes de dispersion, la sémantique des trous, l'échelle incluant le seuil
#### Docs transverses
- `CHANGELOG.md` — entrée 2.20.8
#### Docs immuables
- Aucune

## Critères d'acceptation
- [ ] Trace de régime établi (filtration en marche) : la courbe pH oscille autour de la valeur instantanée affichée (~0,28 au moment de la spec) et la ligne de seuil 0,5 est visible au-dessus
- [ ] L'échelle Y montre le seuil même quand la courbe est très basse (filtration arrêtée, ~0,06)
- [ ] Un transitoire > 1,0 pH dans la trace produit un trou, pas un pic
- [ ] Fetch route coupé : la courbe s'affiche avec le seuil de repli
- [x] Tous les libellés utilisateur sont en français
- [x] `pio run` sans nouveau warning ; `./build_fs.sh` passe
- [ ] Comportement nominal testé manuellement selon le plan ci-dessous

## Plan de test manuel
1. Filtration en marche, onglet Avancé : courbes visibles, seuils à 0,5/100 tracés, cohérence avec la ligne « Dispersion pH » instantanée.
2. Vider le buffer, attendre < 13 points : pas de courbe, pas d'erreur console.
3. Redémarrer la filtration (transitoire) puis recharger : trou dans la courbe sur le transitoire.
4. Simuler l'échec route (couper le WiFi après chargement puis Recharger) : courbe présente avec seuil de repli.

## Notes pour agent
- La décision « valeur absolue, pas de % » est de l'utilisateur — ne pas la rouvrir.
- ux-ui-designer tranche : graphes dédiés sous les graphes existants vs double axe superposé, hauteur, légendes. Contrainte : la carte est déjà dense, rester sobre.
- Le calcul des fenêtres écartées (saut ≥ rejet) est celui de la 071 — ne pas le réimplémenter, exposer la série depuis la fonction existante.

## Suivi / itérations

### Itération 1 — 2026-08-23 — Statut : livré — tests verts, revue Approved (v2.20.8)
**Fait** : sous-graphes dispersion 120 px sous les graphes pH/ORP (`data/index.html`, `data/app.css`) ; série exposée par `_traceDispersionStats` (alignée, trous = fenêtres écartées) ; courbe ambre `#d97706` linéaire à points, seuil pointillé rouge `#dc2626` toujours inclus dans l'échelle Y (`yInclude` de `createTraceChart`), seuil route + repli JS, fetchs indépendants (`data/app.js`) ; note muted enrichie ; version 2.20.8 ; docs (`page-settings.md`, `CHANGELOG.md`). Aucun ADR (aucun critère rempli).
**Reste à faire** : plan de test manuel sur appareil (4 scénarios §Plan de test manuel) + vérification visuelle — critères d'acceptation on-target non cochés en attendant. Ne pas déplacer vers `done/` avant.

## Hors périmètre
- Toute modification firmware
- Historisation de la dispersion au-delà de la trace (~25 min)
- Normalisation en % du seuil (explicitement refusée)
- Affichage de latch_cycles / clean_streak
