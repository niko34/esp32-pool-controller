# ADR-0018 — uPlot au lieu de Chart.js pour les graphiques de l'UI

- **Statut** : Accepté
- **Date** : 2026-07-04
- **Décideurs** : architect, web-ui-developer (feature-043)
- **Spec(s) liée(s)** : feature-043 (migration-uplot) ; feature-042 (slim Chart.js — abandonnée)

## Contexte

La partition `app0` est à **90,8 %** (marge ~145 KB) et bloque les évolutions du firmware. Le seul donneur d'espace pour agrandir `app0`/`app1` est la partition `spiffs`, dont le plus gros poste est la bibliothèque de graphiques **`chart.umd.min.js` (208 KB)**. Il faut réduire drastiquement ce poste pour débloquer un vrai palier de repartitionnement, tout en respectant deux contraintes projet : **offline-first** (pas de CDN) et **vanilla JS sans bundler** ([ADR-0006](0006-frontend-vanilla-js.md)).

## Décision

Remplacer Chart.js v4.5.1 par **uPlot 1.6.32** pour les 7 graphiques de l'UI (3 mini-charts dashboard, 3 historiques détail, 1 debug oscillation) :

- fichiers `dist/uPlot.iife.min.js` + `dist/uPlot.min.css` du paquet npm `uplot@1.6.32` **committés dans `data/`** (version figée, chargement `<script>`/`<link>`, aucune étape de build ajoutée) ;
- **parité stricte** de rendu (aucune refonte visuelle : coloration conditionnelle par segment, dégradés, bandes min/max, multi-axes, tooltip, labels reproduits à l'identique) ;
- **axe X non temporel** : x = indices `0..n-1` + labels catégoriels custom conservés (« Aujourd'hui », mois français, `HH:MM`, `-Ns`) — l'axe temps natif d'uPlot changerait le rendu (locale, densité de ticks).

## Alternatives considérées

- **Chart.js slim (feature-042)** (rejetée) — build custom réduit de Chart.js : gain mesuré ~38 KB seulement, le cœur v4 est incompressible ; imposait en plus une étape de bundling contraire à ADR-0006.
- **Garder Chart.js v4 tel quel** (rejetée) — bloque le repartitionnement `spiffs` → `app0`/`app1` : aucun autre poste FS ne permet de libérer ~150 KB.
- **uPlot 1.6.32** (retenue) — ~50 KB JS + ~2 KB CSS (÷4 vs Chart.js), IIFE + CSS prêts à l'emploi (100 % vanilla, sans bundler), optimisé time-series (rendu plus rapide).

## Conséquences

### Positives
- **−148,3 KB** de payload FS (601 054 → 449 177 octets, ≈ 449 KB pour une partition `spiffs` de 832 KB) — débloque le repartitionnement à venir.
- Rendu plus rapide (uPlot est conçu pour les séries temporelles denses).
- Zéro dépendance de build ajoutée ; mise à jour = copie manuelle de 2 fichiers (procédure dans [BUILD.md](../BUILD.md#bibliothèque-de-graphiques-uplot-frontend)).

### Négatives / dette assumée
- Le **tooltip**, le **redimensionnement** (`ResizeObserver` → `setSize`), la **coloration conditionnelle par segment** et les **dégradés de remplissage** ne sont pas fournis par uPlot : c'est du **code custom dans `app.js`**, à maintenir (c'était du déclaratif Chart.js auparavant).
- Points et légende sont gérés différemment en interne (rendu équivalent, mais le portage d'un futur effet « à la Chart.js » n'est pas direct).
- Les options mortes de l'ancienne usine `createLineChart` (`hideYAxis`, `showYAxisGrid`, `extraPlugins`, `annotation`, `backgroundColor`) ne sont **pas reprises** (réduction de périmètre assumée).

### Ce que ça verrouille
- `data/` embarque uPlot **1.6.32 figé** : toute montée de version est manuelle et exige une revalidation de parité en navigateur (aucun test auto frontend).
- Les graphiques restent sur un **axe X catégoriel** (indices + labels custom) — pas d'axe temps natif.
- Le futur repartitionnement peut compter sur un payload FS ≈ 449 KB.

## Références

- Code : `data/uPlot.iife.min.js`, `data/uPlot.min.css`, `data/index.html`, `data/app.js` (`createMiniLineChart` l.549, `createLineChart` l.772, debug chart l.5395)
- Spec : `specs/features/doing/feature-043-migration-uplot.md` (→ `done/` à la clôture)
- Doc : [BUILD.md](../BUILD.md#bibliothèque-de-graphiques-uplot-frontend), [ADR-0006](0006-frontend-vanilla-js.md) (vanilla sans bundler), [ADR-0015](0015-partition-app-1.5mb.md) (layout partitions v2)
