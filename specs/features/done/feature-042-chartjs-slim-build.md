## Métadonnées
- **ID** : feature-042
- **Slug** : chartjs-slim-build
- **Type** : feature (optimisation taille FS / frontend)
- **Statut** : todo
- **Priorité** : normale (prérequis du repartitionnement — feature B à venir)
- **Version cible** : N/A — optimisation frontend, aucun changement fonctionnel (bump CORRECTIF optionnel)
- **Dernière mise à jour** : 2026-06-28
- **Spec(s) liée(s)** : (à venir) repartitionnement `spiffs`→`app0/app1`

## Domaines touchés
- [x] Frontend (`data/`)
- [ ] Régulation / Capteurs / Web / MQTT / Tests natifs

## Agents requis
- [x] spec-reader
- [x] architect
- [ ] pool-chemistry — NON applicable (frontend, zéro impact chimie)
- [ ] ux-ui-designer — NON (pas de changement visuel)
- [x] web-ui-developer
- [x] code-reviewer
- [x] doc-writer
- [ ] test-engineer — pas de test natif (frontend) ; **vérification manuelle navigateur obligatoire**

## Description
Remplacer le bundle complet `data/chart.umd.min.js` (Chart.js v4.5.1, ~208 KB, tous types) par un **build custom réduit** ne contenant que les composants réellement utilisés (graphiques `line`). Objectif : libérer ~120–150 KB sur la partition `spiffs`, **sans changement visuel ni fonctionnel**. Prépare le repartitionnement ultérieur (donner la place à `app0`/`app1`).

## Contexte
Le frontend n'utilise que des graphiques **en ligne** (3 instances). Le bundle UMD embarque tous les types/contrôleurs/scales/plugins inutiles. Chart.js v4 est tree-shakeable : un build sur-mesure (réalisé **une fois, hors ligne**) garde le projet « vanilla » (un simple `<script>`) tout en divisant la taille par ~3.

> ⚠️ La règle projet « pas de bundler » concerne le **flux de deploy** : le bundler (esbuild) est utilisé comme **outil ponctuel de fabrication** sur PC ; l'artefact réduit est committé comme fichier statique. Le runtime reste 100 % vanilla.

## Composants Chart.js requis (analyse du code — à enregistrer dans le bundle)
`LineController, LineElement, PointElement, LinearScale, CategoryScale, Filler, Legend, Tooltip, Title`
- `LineController`/`LineElement`/`PointElement` : graphiques `line` (l.425, 628, 5295).
- `LinearScale` : axes `linear` explicites (debug chart l.5309-5311 + axes Y).
- `CategoryScale` : axe X par défaut des charts l.425/628 (pas de `type` → category).
- `Filler` : `fill: true` (l.433).
- `Legend`/`Tooltip`/`Title` : plugins cœur utilisés.
- **EXCLUS** : tous les autres types (bar/pie/doughnut/radar/bubble/scatter), `TimeScale` (aucun axe temps — axes category/linear), et **`chartjs-plugin-annotation`** (la clé `annotation:` existe l.656 mais **aucun plugin annotation n'est chargé aujourd'hui** → inerte ; comportement préservé en ne l'incluant pas).

## Comportement attendu
- Les **3 graphiques** s'affichent **identiquement** à aujourd'hui (courbes pH/ORP/température, mini-charts dashboard, debug oscillation).
- `data/index.html` continue de charger un simple `<script src="/chart.umd.min.js">` (nom de fichier conservé pour ne pas toucher l'HTML, OU renommé + HTML mis à jour — arbitrage architect).
- `data/app.js` : idéalement **inchangé** — l'enregistrement des composants (`Chart.register(...)`) est fait **dans le bundle** (entry), avant `app.js`. (Aujourd'hui app.js n'appelle pas `Chart.register` : il dépend du bundle auto-enregistré ; le nouveau bundle auto-enregistre le sous-ensemble.)

### Cas limites
- Vérifier que **chaque** graphique rend (axes, légende, remplissage, tooltip) — un composant oublié = graphique cassé silencieusement (erreur console).
- Vérifier qu'aucun autre usage de `Chart.*` dans app.js ne dépend d'un composant exclu.

## Contraintes techniques
- Build **reproductible** : un script (ex. `tools/build-chart.sh`) + un fichier entry (ex. `tools/chart-entry.js`) qui importe + enregistre les composants ci-dessus, bundlé via **esbuild** en **IIFE minifié** exposant `window.Chart`.
- Chart.js installé en **devDependency** (npm) — pas embarqué dans le firmware.
- Pas de dépendance runtime nouvelle ; pas de bundler dans `deploy.sh`/`build_fs.sh`.
- `node --check` sur le bundle généré ; `./build_fs.sh` OK.

## Fichiers potentiellement concernés
- `data/chart.umd.min.js` — remplacé par le build réduit.
- `tools/chart-entry.js` + `tools/build-chart.sh` — [nouveaux] fabrication reproductible.
- `package.json` — [si présent] ajout `chart.js` + `esbuild` en devDependencies ; sinon créer un `package.json` minimal de tooling.
- `data/app.js` — a priori inchangé (à confirmer).
- `docs/` (frontend/build), `CHANGELOG.md`.

## ADR attendu ?
- **Probable** : faible. Éventuellement une note dans la doc build expliquant le « bundler ponctuel hors ligne » (convention). À trancher par l'architect (le « pas de bundler runtime » reste vrai).

## Critères d'acceptation
- [ ] AC1 — Le nouveau `chart.umd.min.js` est sensiblement plus petit (cible < ~100 KB vs 208 KB).
- [ ] AC2 — Les **3 graphiques** s'affichent correctement dans le navigateur (vérif manuelle : courbes, axes, légende, fill, tooltip) — **aucune erreur console** Chart.js.
- [ ] AC3 — `data/app.js` inchangé (ou modification minimale justifiée si un `Chart.register` côté app s'avère nécessaire).
- [ ] AC4 — Build **reproductible** documenté (script + entry) ; `npm`/esbuild en devDeps, hors runtime.
- [ ] AC5 — `./build_fs.sh` OK ; **mesurer et reporter le gain FS réel** (payload avant/après).
- [ ] AC6 — `pio run` non affecté (le firmware ne dépend pas de ce fichier) ; aucune régression backend.
- [ ] Aucun changement visuel.

## Plan de test manuel (CRITIQUE — pas de test auto pour le frontend)
1. `tools/build-chart.sh` → bundle généré, taille mesurée.
2. `./build_fs.sh` → image FS, reporter payload avant/après.
3. Déployer (ou servir `data/` localement) et **ouvrir chaque vue** : dashboard (mini-charts), pH/ORP (courbes + fill), panel Avancé (debug oscillation linéaire). Vérifier rendu + **console navigateur sans erreur**.
4. Vérifier légende, tooltip au survol, remplissage des aires.

## Notes pour agent
- **Ne pas inclure** TimeScale ni annotation (non utilisés). Si la vérif navigateur révèle un composant manquant, l'ajouter à l'entry et rebuild.
- Le nom `chart.umd.min.js` peut être conservé (zéro modif HTML) même si le format interne devient IIFE — c'est un détail d'implémentation. L'architect tranche.
- Garder l'ancien bundle récupérable via git (réversibilité).

## Hors périmètre
- Changer de bibliothèque de graphiques (uPlot, etc.).
- Le repartitionnement (feature suivante, distincte).
- Toute modification visuelle des graphiques.

---

## Statut : ABANDONNÉE (2026-07-04)

Le build slim de Chart.js v4 ne libère que **~38 KB** (208→168 KB) : le cœur « line »
de Chart.js v4 est incompressible à ~168 KB. Insuffisant pour l'objectif (repartitionnement :
+64 KB app ⇒ −128 KB spiffs). Remplacée par **feature-043 (migration uPlot, ~40 KB)** qui,
seule, libère assez de spiffs (~150 KB). Travail de feature-042 reverté (aucun commit).
