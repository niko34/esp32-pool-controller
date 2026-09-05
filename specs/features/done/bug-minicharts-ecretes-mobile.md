# Bug — Mini-graphiques du dashboard écrêtés en bas sur mobile (iPhone)

- **Type** : bug (format minimal `specs/modele.md`)
- **Statut** : doing — implémenté, en validation
- **Version cible** : 2.18.3
- **Créée** : 2026-08-09 (constaté sur iPhone/Safari, OK sur ordinateur)
- **Domaine** : Frontend uniquement (`data/app.js`, éventuellement `data/app.css`) —
  aucun changement firmware
- **Spec liée** : aucune (les données/échelles sont correctes — bug d'affichage pur)

## Symptôme

Sur iPhone, les mini-graphiques ORP/température (et probablement pH) du
dashboard débordent/sont coupés en bas de leur carte ; l'axe Y paraît non
respecté (étiquettes « 500 », « 28 » collées au bord inférieur). Parfait sur
ordinateur. Capture fournie le 2026-08-09.

## Cause RÉELLE (établie à l'implémentation — la présomption initiale était fausse)

Un `ResizeObserver` (`observeChart`, app.js l.494) couvrait DÉJÀ les 3 sites de
création uPlot. Le vrai coupable : règle CSS morte héritée de Chart.js dans
`data/app.css` (~l.1872) — `@media (max-width: 980px) { canvas { height:
240px !important; } }` (les classes voisines chartWrapper/chartAreaWrapper
n'existent plus nulle part). Le `!important` écrasait la hauteur inline posée
par uPlot : canvas de 88 px étiré à 240 px, écrêté par l'overflow:hidden de la
carte. Explique exactement le « parfait sur desktop » (> 980 px). Les
graphiques des écrans détail (220 px) étaient aussi étirés sur mobile.

## Correctif livré

1. `data/app.css` : suppression des 4 lignes CSS mortes (canvas 240px
   !important + classes Chart.js orphelines), commentaire de garde posé.
2. `data/app.js` : observer existant durci — débounce 100 ms, setSize
   uniquement si dimensions > 0 et réellement changées, re-synchronisation
   initiale (corrige le repli Math.max(…, 50) si conteneur provisoire),
   `unobserveChart()` prêt pour un futur destroy (aucune fuite actuelle).
3. Rotation portrait/paysage couverte par l'observer.

## Critères d'acceptation

- [ ] iPhone Safari : courbes entièrement contenues dans la carte, axe Y lisible
- [ ] Rotation : le graphique se réadapte
- [ ] Aucun changement de contrat/API ; `./build_fs.sh` OK ; revue avant commit
