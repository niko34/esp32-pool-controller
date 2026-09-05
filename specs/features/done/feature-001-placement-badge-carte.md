## Métadonnées
- **ID** : feature-001
- **Slug** : placement-badge-carte
- **Type** : refactor (UI)
- **Statut** : done
- **Priorité** : normale
- **Dernière mise à jour** : 2026-04-29

## Type
feature

## Description
Dans les cartes, placer les badges de fonctionnement (type Marche / Arrêt) alignés horizontalement à droite du titre. C'est déjà le cas sur plusieurs écrans. Par exemple, l'écran de paramétrage MQTT a le badge correctement placé. Faire une revue de tous les badges d'état sur toutes les pages pour ne pas en oublier.

## Contexte
Cela permettra de gagner de la place verticalement dans la carte.

## Fichiers potentiellement concernés
- data/app.js
- data/app.css
- data/index.html

## Critères d'acceptation
- [x] Les badges affichés dans les cartes s'affichent à droite des titres, alignés horizontalement et proche du libellé à gauche de la ligne.
- [x] Aucune régression visuelle sur les cards déjà conformes (Wi-Fi, Heure, Sécurité, Régulation, Calibrations, Produits, Historique, Système).
- [x] Aucune régression sur le dashboard (`card--status`, structure différente, badges `state-badge--*` intacts).
- [x] Build LittleFS OK (`./build_fs.sh`), partition 1088 KB respectée.

## Travail réalisé

**Périmètre final :** 3 cards transformées (refactor visuel pur, aucun changement de comportement firmware).

1. **`data/index.html`** — pour chacune des 3 cards (MQTT « Paramètres », Filtration « Contrôle manuel », Lighting « Contrôle manuel ») :
   - Le badge `pill` est sorti de l'intérieur du `<h2>` pour devenir **frère direct** du `<h2>` à l'intérieur du `card__head`.
   - Pour Filtration et Lighting : les `.detail-row` « État actuel » du body ont été supprimées (l'information est désormais lue dans le badge en en-tête, redondance levée).
   - MQTT : suppression du wrapping uniquement (pas de ligne body à retirer).

2. **`data/app.js`** — split du routing des badges :
   - `updateFiltrationBadges()` : page détail → applique `pill ok` / `pill bad` / `pill mid` ; dashboard → conserve `state-badge--*`.
   - `updateLightingStatus()` : même split (détail = `pill ok/bad`, dashboard = `state-badge--*` inchangé).
   - `getFiltrationState()` : nouvelle propriété `pillClass` exposée dans le retour (mapping `warn → mid`).

3. **`data/app.css`** — règle de garde dans le `card__head` :
   ```css
   .card__head .pill { flex-shrink: 0; white-space: nowrap; }
   ```
   Empêche la compression du badge ou le retour à la ligne quand le titre est long.

## Fichiers modifiés
- `data/index.html` (3 cards transformées, 2 lignes « État actuel » supprimées)
- `data/app.js` (`updateFiltrationBadges()`, `updateLightingStatus()`, `getFiltrationState()`)
- `data/app.css` (règle `.card__head .pill`)

## Build
- `./build_fs.sh` : SUCCESS
- Partition LittleFS : 1088 KB respectée
- `pio run` : non requis (aucun fichier `src/` modifié)

## Revue
- `code-reviewer` : APPROVE (refactor visuel sans risque, périmètre strictement contenu, dashboard intact)
- UX validée par l'utilisateur sur les 3 cards concernées

## Notes
- Aucun ADR requis : les 4 critères d'ADR ne sont pas remplis (refactor visuel pur, aucune contrainte structurante, aucune alternative crédible à documenter, aucun verrou architectural).
- Cohérence inter-pages obtenue : tous les badges d'état des cards de configuration suivent désormais le même placement DOM.
