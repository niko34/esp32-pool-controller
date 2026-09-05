## Métadonnées
- **ID** : feature-043
- **Slug** : migration-uplot
- **Type** : feature (remplacement de bibliothèque frontend — réduction taille FS)
- **Statut** : done
- **Priorité** : normale (prérequis réel du repartitionnement spiffs→app)
- **Version cible** : N/A côté logique ; changement frontend visible (bump MINEUR à la release)
- **Dernière mise à jour** : 2026-07-04
- **Spec(s) liée(s)** : feature-042 (slim Chart.js — abandonnée, gain insuffisant), (à venir) repartitionnement spiffs→app0/app1

## Domaines touchés
- [x] Frontend (`data/`)
- [ ] Régulation / Capteurs / Web / MQTT / Tests natifs

## Agents requis
- [x] spec-reader
- [x] architect
- [ ] pool-chemistry — NON applicable (frontend, zéro impact chimie)
- [x] ux-ui-designer — **parité visuelle** (aucune refonte : reproduire l'existant à l'identique)
- [x] web-ui-developer
- [x] code-reviewer
- [x] doc-writer
- [ ] test-engineer — pas de test natif (frontend) ; **vérification navigateur = critère d'acceptation**

## Description
Remplacer **Chart.js v4.5.1** (`data/chart.umd.min.js`, ~208 KB) par **uPlot** (~47 KB JS + ~5 KB CSS) pour les 3 usages de graphiques, **sans changement visuel ni fonctionnel**. Objectif : libérer **~150 KB** sur `spiffs`, ce qui débloquera un vrai palier de repartitionnement (donner de la place à `app0`/`app1`). Gain secondaire : rendu plus rapide (uPlot est optimisé time-series).

## Contexte
La flash `app0` est à 90,8 % (marge ~145 KB). Le seul donneur pour agrandir `app0`/`app1` est `spiffs`, et `chart.umd.min.js` (208 KB) y est le plus gros poste. Le slim Chart.js (feature-042) n'a libéré que ~38 KB (cœur v4 incompressible) → **abandonné**. **uPlot** est la seule voie qui réduit vraiment (÷4 environ) : ~150 KB libérés sur spiffs.

Avantage bonus pour la contrainte projet : **uPlot ships un IIFE prêt à l'emploi + un CSS** → intégration **100 % vanilla, sans bundler** (juste `<script>` + `<link>`). Récupérer les 2 fichiers **une fois** depuis npm (paquet `uplot`) et les committer dans `data/`.

## Graphiques à migrer — 3 fonctions-usines → **7 instances**
> ⚠️ Il n'y a que 3 `new Chart` dans le code, mais ce sont des **usines** appelées plusieurs fois. Migrer les 3 usines couvre les **7 graphiques**, **température incluse**.

1. **Mini-charts dashboard** (usine `createMiniLineChart`, l.424) → **3 instances** : `ph-mini-chart`, `orp-mini-chart`, **`temp-mini-chart`**. Courbe + remplissage, et **coloration conditionnelle par segment** (vert dans la tolérance / rouge hors limite) — pour les **3** (pH cible 7.2±0.2, ORP 700±50, **température 26±8 °C**).
2. **Graphiques historique détail** (usine `createLineChart`, l.662) → **3 instances** :
   - `phChart` et `orpChart` : courbe + **bandes de référence min/max** (`ensurePhReferenceDatasets`/`ensureOrpReferenceDatasets`, `fill: '+1'/'-1'`) + bornes Y + boutons de plage.
   - **`tempChart`** (écran Température) : cas **le plus simple** — courbe seule, **sans bandes** (`createLineChart(canvas, "#4f8fff", "Température")`).
3. **Debug oscillation** (panel Avancé, l.5294) → **1 instance** `debug_ph_chart` : **multi-axes** (Y pH gauche, Y1 ORP droite, Y2 masqué), axe X linéaire, **légende** en haut, **titres d'axes**.

(La clé `annotation:` de la vue détail est **inerte** — plugin non chargé — rien à reporter.)

## Inventaire exhaustif des effets Chart.js à reproduire (PARITÉ OBLIGATOIRE)
Chaque effet ci-dessous existe aujourd'hui et doit être reproduit à l'identique. **Ne rien oublier** (source du contrôle utilisateur) :

1. **Coloration conditionnelle par segment** ⚠️ *(risque n°1)* : sur les mini-charts dashboard, la couleur de la courbe change **segment par segment** selon la moyenne des 2 points (`segment.borderColor`, l.448-452). ⚠️ **CE N'EST PAS UN BINAIRE vert/rouge** — c'est `getMiniChartRGB` (l.402-412) :
   - dans la tolérance (`|valeur−cible| ≤ tol`) → **vert** `[34,197,94]` ;
   - au-delà → **interpolation CONTINUE vert→rouge** avec `t = min(1, dépassement/tolérance)` → `[34+(239−34)t, 197−(197−68)t, 94−(94−68)t]` ;
   - valeur `null`/`NaN` → **gris** `[156,163,175]`.
   Reproduire cette formule exacte (pas un seuillage binaire). uPlot : `series.stroke` = fonction renvoyant un `CanvasGradient` par valeur, ou hook de dessin.
2. **Remplissage en dégradé vertical** dont la couleur est celle de la **DERNIÈRE valeur non nulle de la série** (parcours arrière l.438-441), via la même `getMiniChartRGB`, en dégradé alpha 0.22 (haut) → 0.02 (bas) (`createLinearGradient`, l.443-445). uPlot : `series.fill` = fonction renvoyant un `CanvasGradient`.
3. **Bandes de référence min/max** (datasets avec `fill: '+1'/'-1'`) sur les vues détail pH/ORP. uPlot : option `bands`.
4. **Multi-axes** : Y pH (gauche), Y1 ORP (droite), Y2 masqué (debug oscillation). uPlot : plusieurs `scales`/`axes`.
5. **Tooltip au survol** avec **callbacks de formatage** (titre = label, valeur via `formatValue`) et style custom (fond sombre, arrondi). uPlot : plugin tooltip léger (custom) + `cursor`.
6. **Lissage de courbe** (`tension`) → `uPlot.paths.spline()`.
7. **spanGaps** (ignorer les `null`) sur le debug → gestion native des trous.
8. **pointRadius**, **légende** (position/affichage), **titres d'axes**, **`animation: false`**, `responsive`/`maintainAspectRatio`.
9. **Bornes Y dynamiques** (`min`/`max`/`beginAtZero`) + **boutons de plage temporelle** (vues détail).
10. **Ordre de dessin + affectation d'axe** (`order`, `yAxisID`) : bandes min/max dessinées **derrière** la courbe, séries rattachées au bon axe Y. uPlot : ordre du tableau `series` + `scale` par série.
11. **Épaisseur de trait** (`borderWidth`) → `series.width`. **Mode de survol** (`interaction`/`mode: 'index'`/`intersect`) → cursor uPlot + tooltip « à x sur toutes les séries ».
12. **Séries masquées par défaut + toggle via légende** (debug chart) ⚠️ : « ORP (mV) » et « T° envoyée (°C) » ont **`hidden: true`** (l.5301-5302) — masquées au chargement, **affichables par clic sur l'entrée de légende** (comportement natif Chart.js). uPlot : `series.show: false` + toggle natif de la légende uPlot. Si non reproduit → impossible d'afficher ORP/T° dans le debug (régression fonctionnelle).
13. **Labels d'axe X catégoriels custom — À CONSERVER TELS QUELS** ⚠️ : **aucun graphique n'utilise d'axe temps**. Détail : agrégation **par jour calendaire** (dernier point connu du jour), mois français en dur (`MOIS_FR`, workaround Safari l.827), label spécial **« Aujourd'hui »**, injection du point du jour depuis `latestSensorData` si absent (l.825-859). Mini : labels `HH:MM` / `J/M HH:MM` (l.498-505). Debug : labels relatifs `-Ns`. → **Réutiliser la logique d'agrégation/labellisation existante à l'identique** ; uPlot : x = index + formatter de ticks depuis les labels (voir « Points d'implémentation »).
14. **Ticks entiers ORP** (`integerOnly`, l.617-621) : l'axe Y de `orpChart` n'affiche que les valeurs entières.
15. **Échelle Y de repli des mini-charts vides** : sans aucune donnée valide, l'axe Y est forcé à **cible ± 4×tolérance** (l.578-586) ; dès qu'il y a des données → bornes auto.
16. **Gel des courbes pendant calibration** : `updateTodayOnCharts` **ne met pas à jour** `phChart`/`orpChart` quand `phCalibrationActive`/`orpCalibrationActive` est vrai (l.919-924) — la sonde est en solution étalon. À préserver.
17. **Synchronisation des sélecteurs de plage** : un clic sur un bouton (`all/30d/7d/24h`) active la même plage sur **les trois écrans** (l.671-673) + recharge l'historique.
18. **Cycle de vie du debug chart** : `destroy()` + recréation complète à chaque refresh (l.5293) ; bouton « vider » → destroy + masquage du conteneur (l.5365-5368). uPlot : `u.destroy()`.
19. **Mécanisme réel de refresh des mini-charts** : chargement initial `GET /get-history?range=3d`, puis **polling HTTP incrémental toutes les 5 min** avec `since=` → **append** des nouveaux points (l.5877-5881 + `appendMiniChart`). Ce n'est PAS du WebSocket (contrairement à ce que dit la doc — cf. note doc-writer).

> Tout est faisable en uPlot ; les points **1 et 2** (coloration/dégradé pilotés par la valeur) sont ceux qui demandent du code custom et concentrent le risque de régression visuelle.

### Portée de la réécriture (au-delà de la création des charts)
Confirmé par relecture : **~30 sites de mise à jour** (`chart.data.datasets[...].data = …` + `chart.update()`) répartis dashboard / détail pH-ORP / debug (l.559-927 et 2198-2212). **Tous** doivent migrer vers le modèle **columnar** d'uPlot (`u.setData([...])`). C'est la part la plus volumineuse — l'effort n'est PAS que dans les 3 `new Chart`.
- Surface bornée confirmée : 3 `new Chart` (l.424/662/5294), méthodes `.data`/`.update()`/`.destroy()` uniquement. **Aucune** dépendance à `Chart.register`/`Chart.defaults`/`Chart.helpers`, ni adaptateur date, ni plugin externe actif.

## Comportement attendu
**Parité stricte** (characterization au niveau UI) : reproduire **tous** les effets de l'inventaire ci-dessus, plus la **mise à jour temps réel** (WebSocket) sans rechargement.

### Points d'implémentation uPlot (≠ Chart.js)
- **Données columnar** : uPlot attend `[ xValues[], serie1[], serie2[], … ]` (et non des objets `{x,y}`). Les chemins d'alimentation (données `/data`, historique, WS) doivent produire ce format ; `u.setData(...)` pour les mises à jour temps réel.
- **Remplissage / bandes** : `series[i].fill` + option `bands` (remplissage entre deux séries) pour les bandes min/max.
- **Multi-axes** : plusieurs `scales` + `axes` (axe droit pour ORP) pour le debug chart.
- **Tooltip** : uPlot n'a pas de tooltip riche natif → utiliser le **cursor + legend** intégré (valeurs live) et/ou un **petit plugin tooltip** (léger, committé avec uPlot). À cadrer par ux-ui-designer/architect pour rester proche de l'actuel.
- **Responsive** : uPlot ne se redimensionne pas seul → `ResizeObserver` sur le conteneur → `u.setSize({width,height})`.
- **Axe X : NE PAS utiliser l'axe temps natif d'uPlot** ⚠️ (correction d'une version antérieure de cette spec). Les graphiques actuels sont **catégoriels** avec labels custom (« Aujourd'hui », mois FR, `HH:MM`, `-Ns`) — passer à un axe temps natif changerait le rendu (locale, densité de ticks, perte du label « Aujourd'hui »). Faire : `scale x` non-temporelle (x = indices 0..n-1), tableau de labels conservé à part, et `axes[0].values = (u, ticks) => ticks.map(i => labels[i])`. La logique d'agrégation par jour (`loadHistoricalData`) et de labellisation (`miniChartPointLabel`) est **réutilisée telle quelle**.
- **Options mortes à NE PAS porter** : `createLineChart` expose `hideYAxis`, `showYAxisGrid`, `extraPlugins`, `annotation`, `backgroundColor` — **inutilisées** aux call-sites actuels. La nouvelle usine uPlot ne les reprend pas (réduction de périmètre assumée ; à noter dans la doc).

### Cas limites
- Données vides / NaN (capteur indisponible) → pas de crash, courbe absente proprement ; mini-charts : **échelle Y de repli = cible ± 4×tolérance** (inventaire #15) ; couleur **grise** (inventaire #1).
- **Calibration en cours** → `phChart`/`orpChart` gelés (inventaire #16), mini-charts non affectés.
- Mise à jour temps réel haute fréquence → `setData` performant (pas de fuite).
- Changement de plage temporelle (boutons) → re-scale correct + **synchronisation des 3 écrans** (inventaire #17).
- Fenêtre redimensionnée / rotation mobile → chart re-dimensionné.
- Mode hors-ligne (WS coupée) → dernier état figé, pas d'erreur console.

## Comportement actuel
3 graphiques via Chart.js v4 (auto-enregistré, UMD). `data/index.html` charge `chart.umd.min.js`. `data/app.js` : création + `update()` des charts (objets datasets).

## Contraintes techniques
- **Offline-first** : uPlot committé dans `data/` (pas de CDN au runtime).
- **Vanilla, pas de bundler** : uPlot IIFE + CSS chargés par `<script>`/`<link>` ; **aucune** étape de build ajoutée au deploy.
- Budget `spiffs` : viser un payload nettement réduit (mesurer avant/après).
- Ne pas régresser les autres écrans ni la mise à jour temps réel.
- Version uPlot **figée** (récupérée une fois de npm, committée).

## Fichiers potentiellement concernés
- `data/uPlot.iife.min.js` + `data/uPlot.min.css` — [nouveaux] (remplacent `chart.umd.min.js`, à supprimer).
- `data/index.html` — remplacer `<script chart.umd.min.js>` par `<script uPlot>` + `<link uPlot.css>`.
- `data/app.js` — réécriture des 3 créations de graphiques + logique `update()` (data columnar, bands, multi-axes, tooltip, resize).
- `data/app.css` — styles éventuels (tooltip, conteneurs).
- `docs/features/` (pages concernées), `docs/BUILD.md` (provenance uPlot), `CHANGELOG.md`.

## Nouvelles signatures / API
- Aucune côté firmware (endpoints/WS/MQTT inchangés). Le contrat `/data`, `/history`, WS reste identique ; seule la **consommation** côté JS change.

## ADR attendu ?
- **Probable : oui** — changement de bibliothèque de visualisation (dépendance frontend structurante, alternative Chart.js écartée pour raison de taille). ADR court « uPlot au lieu de Chart.js pour la contrainte Flash/FS » (critères : décision structurante + alternative crédible tracée). À confirmer par l'architect.

## Critères d'acceptation
- [x] AC1 — `chart.umd.min.js` supprimé ; uPlot (JS+CSS) intégré ; **payload FS réduit d'au moins ~120 KB** (mesuré avant/après).
- [x] AC2 — **Parité visuelle** des 3 graphiques (courbes, couleurs, remplissages, bandes min/max, légende, titres, axe droit ORP) — vérif navigateur.
- [x] AC2a — **Coloration conditionnelle par segment** reproduite avec la **formule exacte** de `getMiniChartRGB` : vert dans la tolérance, **interpolation continue** vert→rouge au-delà (pas un binaire), **gris** si donnée absente (inventaire #1).
- [x] AC2b — **Remplissage en dégradé** piloté par la **dernière valeur** de la série reproduit (mini-charts, inventaire #2).
- [x] AC2c — **Lissage** (tension), **spanGaps**, **bornes Y dynamiques** (+ zones ombrées qui suivent), **ticks entiers ORP** et **boutons de plage synchronisés** reproduits (inventaire #9, #14, #17).
- [x] AC2d — **Labels X catégoriels préservés** : « Aujourd'hui », mois français, agrégation par jour, `HH:MM` sur les mini-charts, `-Ns` sur le debug (inventaire #13) — PAS d'axe temps natif.
- [x] AC2e — **Debug chart** : séries ORP/T° **masquées par défaut et activables via la légende** (inventaire #12) ; cycle destroy/recreate au refresh + bouton « vider » (inventaire #18).
- [x] AC2f — **Gel pendant calibration** : `phChart`/`orpChart` non mis à jour quand une calibration est active (inventaire #16).
- [x] AC2g — Mini-charts vides : **échelle Y de repli cible ± 4×tolérance** (inventaire #15) ; mécanisme de refresh conservé (chargement `3d` + polling incrémental 5 min, inventaire #19).
- [x] AC3 — **Tooltip** au survol fonctionnel (titre + valeur formatée), proche de l'actuel.
- [x] AC4 — **Mise à jour en direct** sans rechargement ni fuite : point « Aujourd'hui » des graphiques détail via le rafraîchissement `/data` (`updateTodayOnCharts`, l.2853), mini-charts via le polling incrémental 5 min ; plages temporelles OK.
- [x] AC5 — **Responsive** : re-dimensionnement fenêtre/mobile correct.
- [x] AC6 — Cas limites : données vides/NaN, WS coupée → aucune erreur console.
- [x] AC7 — `./build_fs.sh` OK, sous partition ; `node --check data/app.js` OK ; backend/firmware non affectés.
- [x] AC8 — Aucune régression des autres écrans.

## Plan de test manuel (CRITIQUE — aucun test auto pour le frontend)
1. `./build_fs.sh` → reporter payload FS avant/après (référence ~601 KB).
2. Déployer / servir `data/` ; ouvrir : **dashboard** (3 mini-charts + fill), **pH & ORP détail** (courbe + bandes + plages + tooltip), **panel Avancé** (debug multi-axes + légende + titres).
3. Vérifier **console navigateur sans erreur**, mise à jour temps réel, redimensionnement, cas capteur indisponible.
4. Comparer côte à côte avec l'ancien rendu (captures avant/après) pour la parité.

## Notes pour agent
- **uPlot ships prêt à l'emploi** (IIFE + CSS) → pas de bundler, contrairement à Chart.js slim. C'est ce qui rend la migration compatible « vanilla ».
- Le gros du travail est dans `app.js` : passer d'un modèle « datasets d'objets » à un modèle **columnar** + gérer bands/multi-axes/tooltip/resize.
- **La référence de parité est le CODE, pas la documentation** (divergences doc↔code connues, cf. section doc-writer ci-dessous).
- **Risque principal** : régression visuelle/fonctionnelle silencieuse (pas de test auto). La vérif navigateur (AC2-AC6) est le vrai filet — prévoir des captures avant/après.
- Garder Chart.js récupérable via git (réversibilité) tant que la parité n'est pas validée.
- ux-ui-designer : cadrer la parité (couleurs, épaisseurs, remplissages, tooltip, formule `getMiniChartRGB`) AVANT le codage.

## Corrections documentation à faire au passage (doc-writer)
> Les 3 divergences doc↔code préexistantes détectées lors de la revue de cette spec (zone ORP 600–800 mV, mini-charts `range=3d` + polling 5 min, reformulation « historique horaire ») ont été **corrigées en amont** (commit docs du 2026-07-04, hors feature).

Reste pour cette feature : remplacer les mentions « Chart.js » par « uPlot » dans les pages concernées (`page-dashboard.md`, `page-ph.md`, `page-orp.md`, `page-temperature.md`) une fois la migration faite.

## Hors périmètre
- Toute refonte/amélioration visuelle des graphiques (parité stricte uniquement).
- Le repartitionnement (feature suivante, qui consommera l'espace libéré).
- Migration d'autres composants frontend.

---

## Suivi / itérations

**2026-07-04 — Implémentation livrée, EN ATTENTE DE VALIDATION NAVIGATEUR (H1).**

### Livré
- uPlot **1.6.32** (npm, figé) : `data/uPlot.iife.min.js` (51 KB) + `data/uPlot.min.css` (1,8 KB) committés ; `chart.umd.min.js` (208 KB) supprimé ; crédit page À propos mis à jour.
- 7 graphiques réécrits à parité (inventaire #1-#19) : adaptateur columnar `makeChartState`, stroke = CanvasGradient horizontal à hard stops (formule `getMiniChartRGB` exacte, gris NaN), fill dégradé vertical 0.22→0.02 (dernière valeur), bands min/max, multi-axes debug + séries `show:false` togglables, labels catégoriels conservés (« Aujourd'hui », MOIS_FR, HH:MM, -Ns), tooltip custom, ResizeObserver unique, gel calibration, plages synchronisées, polling 5 min inchangé, options mortes non portées.
- **AC1 mesuré : payload FS 601 054 → 449 177 o (−151 877 o ≈ −148 KB)** — objectif ≥120 KB dépassé.
- Vérifs : `node --check` OK, `./build_fs.sh` OK (832 KB), 0 résidu Chart.js, `src/` intouché (hors version.h → 2.3.0).
- Revue : **Approved** (19/19 parité ; 2 nits cosmétiques non bloquants notés). Docs : 5 pages + BUILD.md + CHANGELOG [2.3.0] + **ADR-0018**.

### Reste (bloquant pour done/)
- **H1 — validation navigateur par l'utilisateur** (plan de test manuel de la spec + 11 points de comparaison du référentiel ux-ui). Déploiement : `./deploy.sh fs` (ou ota-fs) — firmware inchangé (version.h seul).

### Clôture
- **H1 validée par l'utilisateur le 2026-07-04** (« tout est conforme ») : AC2/AC2a-2g/AC3-AC6/AC8 confirmés en navigateur. Statut : **done**, version 2.3.0.
