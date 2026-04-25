# ADR-0006 — Frontend en vanilla JS sans framework ni bundler

- **Statut** : Accepté
- **Date** : 2024 (choix initial du projet)
- **Doc(s) liée(s)** : [page-ph.md](../features/page-ph.md), [page-orp.md](../features/page-orp.md)

## Contexte

L'UI sert plusieurs pages (dashboard, filtration, éclairage, température, pH, ORP, dosages, paramètres) avec du temps réel, des graphes, des formulaires. La tentation naturelle d'un développeur web est de sortir Vue / React / Svelte + Vite + TypeScript.

Mais :

- le firmware, le filesystem et le web sont servis depuis un ESP32 avec **4 MB de flash** et une partition SPIFFS de **1088 KB** ;
- le site doit être **offline first** (fonctionne sans Internet, directement sur `http://poolcontroller.local`) ;
- les mises à jour passent par OTA : chaque octet économisé dans `data/` accélère l'upload.

Un bundle Vue 3 minimal fait déjà 60-80 KB gzip. React + Router dépasse allègrement 120 KB gzip. Et ça, c'est avant d'ajouter la logique métier.

## Décision

Le frontend est écrit en **vanilla JavaScript** (ES6+), servi par le web server embarqué :

- Un seul fichier `data/index.html` avec **toutes les vues** dans le DOM (SPA), activées via la classe CSS `is-active`.
- Un seul fichier `data/app.js` qui contient la logique (routage léger, WebSocket, fetch, chart updates).
- Un seul fichier `data/app.css`.
- Une seule dépendance tierce : **Chart.js** (chargée depuis CDN ou bundlée localement).
- Aucun framework UI, aucun bundler (pas de Webpack / Vite / Rollup), aucun TypeScript.

La minification est faite au build par [`minify.js`](../../minify.js) via `html-minifier-terser`, `Terser`, `CleanCSS`. Les sources sont versionnées dans `data/`, les fichiers minifiés générés dans `data-build/` (ignoré par git).

## Alternatives considérées

- **Vue 3 + Vite** (rejeté) — ajoute une étape de build Node.js, augmente la taille du bundle, complique le debug (source maps vs. code sur ESP32).
- **htmx** (rejeté) — séduisant pour une UI légère, mais la logique temps réel (push WebSocket, reconnexion, buffering) demande du code JS qui arrive de toute façon.
- **Alpine.js** (considéré, pas retenu) — 15 KB gzip, API sympa, mais une dépendance de plus et ne résout pas le besoin principal (gestion du WebSocket, des graphes).

## Conséquences

### Positives
- Bundle minuscule : HTML + CSS + JS minifiés tiennent largement dans les 1088 KB SPIFFS (voir `build_fs.sh` qui reporte la taille finale).
- Pas de pipeline Node.js à installer pour hacker le code : `data/app.js` se modifie avec n'importe quel éditeur.
- Debug direct dans DevTools sur le code déployé (pas de bundler à reverse-engineer).
- Pas de version lock à maintenir : vanilla JS ne casse pas d'une release à l'autre.

### Négatives / dette assumée
- Pas de système de composants : les vues sont dans le même fichier HTML, la logique d'affichage est à base de `querySelector` + classes CSS.
- Pas de typage : erreurs de typo détectées à l'exécution, pas au build.
- Tout changement d'UI passe par `./build_fs.sh` + upload filesystem (quelques secondes en OTA, plus en USB).
- Le code JS est **gros et monolithique** (`data/app.js` approche plusieurs milliers de lignes) : refactoring manuel uniquement.

### Ce que ça verrouille
- Introduire un framework nécessiterait de tout réécrire + de surveiller la taille bundle.
- La stack reste attrayante pour des contributeurs qui veulent hacker sans toolchain.

## Références

- Code : [`data/index.html`](../../data/index.html), [`data/app.js`](../../data/app.js), [`data/app.css`](../../data/app.css)
- Build : [`minify.js`](../../minify.js), [`build_fs.sh`](../../build_fs.sh)
- Doc : [`docs/BUILD.md`](../BUILD.md)
- Spec template : [`specs/features/_template.md`](../../specs/features/_template.md) ligne « Vanilla JS dans `data/` (pas de framework, pas de bundler) »
