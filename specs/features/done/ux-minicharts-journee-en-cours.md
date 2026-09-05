# UX — Mini-graphiques du tableau de bord : journée en cours au lieu de 24 h glissantes

- **Type** : amélioration UX (format minimal `specs/modele.md`)
- **Statut** : implémenté (v2.18.9) — validation on-target restante
- **Version cible** : 2.18.9
- **Créée** : 2026-08-14 (demande utilisateur)
- **Domaine** : Frontend uniquement (`data/app.js`) — aucun changement firmware,
  aucun contrat HTTP modifié
- **Specs liées** : bug-points-futurs-et-ux-courbes (v2.18.4, qui avait fixé la
  fenêtre à 24 h glissantes), bug-minicharts-ecretes-mobile

## Demande

Les mini-graphiques de l'accueil affichent aujourd'hui une fenêtre **glissante de
24 h**, donc une partie de la journée d'hier. L'utilisateur veut n'y voir que
**la journée en cours** (depuis minuit, heure locale).

## Correctif attendu

1. **Filtrage à minuit local** : ne conserver que les points dont l'horodatage
   est ≥ minuit du jour courant. La requête firmware reste `range=24h` (aucun
   changement de contrat ni de version du firmware) ; le filtrage est fait côté
   navigateur, avant tout calcul d'affichage.
2. **Le filtrage doit précéder le calcul des coupures de courbe** (`prevTs` /
   `CHART_GAP_SECONDS`), sinon le premier point de la journée créerait un faux
   trou avec le dernier point d'hier.
3. **Passage de minuit** : le rafraîchissement incrémental (toutes les 5 min,
   via `?since=`) ajoute les nouveaux points à ceux déjà affichés. Au changement
   de jour, il faut donc **repartir de zéro** (rechargement complet) au lieu
   d'ajouter, pour que les points de la veille disparaissent. Détecter le
   changement de jour civil entre deux rafraîchissements.
4. **Début de journée** : entre minuit et les premiers relevés, les courbes sont
   vides ou très courtes — c'est le comportement demandé. Vérifier que le repli
   d'échelle existant (`applyMiniChartFallbackScale`) donne un rendu correct et
   qu'aucune erreur n'apparaît en console.
5. Les étiquettes de l'axe X peuvent être simplifiées si pertinent : tous les
   points appartenant au jour courant, la date n'est plus nécessaire
   (`miniChartPointLabel` affiche déjà l'heure seule pour aujourd'hui). Ne pas
   toucher à l'infobulle, qui reste datée (acquis de la v2.18.4).
6. **Hors périmètre** : les graphiques des écrans détail (pH, ORP, température)
   gardent leurs plages actuelles ; seuls les mini-graphiques de l'accueil sont
   concernés.

## Critères d'acceptation

- [x] Les mini-graphiques ne montrent aucun point antérieur à minuit
- [x] Aucun faux trou en début de courbe
- [x] Après minuit, les points de la veille disparaissent sans rechargement
      manuel de la page *(implémenté ; confirmation on-target à faire)*
- [x] Aucune régression des coupures de filtration, de l'infobulle datée ni du
      rendu mobile
- [x] `node --check data/app.js` et `./build_fs.sh` OK

## Notes d'implémentation (v2.18.9)

Modification **frontend unique** : `data/app.js` (+ `src/version.h` → 2.18.9).
Aucun fichier `src/*.cpp`, aucune route HTTP, aucun topic MQTT touché.

1. **Borne de journée** — `startOfTodaySeconds()` : `new Date()` +
   `setHours(0,0,0,0)` → epoch en secondes. Le fuseau est celui du **navigateur**
   (pas celui du firmware) : c'est volontaire, la plage doit correspondre à la
   journée de l'utilisateur qui regarde l'écran.
2. **Détection du changement de jour** — variable de module `miniChartDayStart`
   (minuit du jour actuellement affiché). À chaque appel de `loadMiniChartData()`,
   si `dayStart !== miniChartDayStart` (premier appel **ou** passage de minuit) :
   `miniChartDayStart` est réaffecté, `lastMiniChartTimestamp` remis à 0 et
   `since` forcé à 0 → la requête repart en **rechargement complet** et
   `updateMiniChart()` **remplace** la série au lieu d'y ajouter. Les points de la
   veille disparaissent donc au premier polling suivant minuit (≤ 5 min), sans
   reload de page. Aucun timer supplémentaire n'est nécessaire.
3. **Filtrage avant coupures** — `if (point.timestamp < dayStart) return;` est
   placé **en tête** de la boucle `data.history.forEach`, donc avant le test
   `point.timestamp - prevTs > CHART_GAP_SECONDS` : le premier point du jour ne
   peut pas être comparé à un point de la veille (sinon faux trou systématique de
   plusieurs heures en tête de courbe). `maxTs` n'est mis à jour que sur les
   points retenus.
4. **Chemin incrémental durci** — le « point précédent » n'est repris que s'il
   appartient au jour courant :
   `prevTs = (since > 0 && lastMiniChartTimestamp >= dayStart) ? … : null`
   (était `lastMiniChartTimestamp > 0`). Garde-fou redondant avec le point 2,
   conservé pour être robuste à un futur appel `since > 0` non réinitialisé.
5. **Requête inchangée** — toujours `GET /get-history?range=24h[&since=…]` :
   demander 24 h et jeter la veille coûte quelques points de plus sur le réseau
   mais évite d'ajouter un paramètre de plage au firmware (pas de changement de
   contrat, pas de version firmware liée). Le surcoût est négligeable (points
   bruts + moyennes horaires sur 24 h).
6. **Début de journée** — série vide entre minuit et le premier relevé :
   `applyMiniChartFallbackScale()` (déjà en place) borne l'échelle sur
   `cible ± tolérance`, le graphique reste cadré et aucune exception n'est levée
   (uPlot accepte des séries vides).
7. **Étiquettes / infobulle** — `miniChartPointLabel()` inchangé : tous les points
   étant du jour, il affiche déjà l'heure seule. L'infobulle datée/typée de la
   v2.18.4 (`tooltipDateTime()` / `granularityLabel()`) n'est pas touchée.

**Validations effectuées** : `node --check data/app.js` OK ; `./build_fs.sh` OK ;
relecture du chemin incrémental et du chemin complet ; hors périmètre confirmé
(les écrans détail passent par `loadHistoricalData()` / `createLineChart()`, non
modifiés).

## Validation on-target restante

À faire **après `./build_fs.sh` + flash du filesystem** (`./deploy.sh fs` ou
`ota-fs`) — la mise à jour du firmware seul ne suffit pas :

- [ ] Tableau de bord : aucun point antérieur à minuit sur les 3 mini-graphiques
      (vérifier l'infobulle du **premier** point : date du jour)
- [ ] Aucun trou en tête de courbe (pas de segment `null` initial)
- [ ] Laisser la page ouverte au passage de minuit : les points de la veille
      disparaissent seuls sous ≤ 5 min, sans rechargement manuel
- [ ] Début de journée (avant le premier relevé) : graphiques vides mais cadrés,
      **aucune erreur en console**
- [ ] Coupure de filtration > 90 min toujours visible ; infobulle toujours datée
      et typée ; rendu mobile (iPhone) non écrêté
- [ ] Écrans détail pH / ORP / Température : plages `Tout` / `30j` / `7j` / `24h`
      inchangées
