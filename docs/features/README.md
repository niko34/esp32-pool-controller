# Documentation des fonctionnalités — ESP32 Pool Controller

Ce dossier contient **un fichier par surface UI** de l'application web. Chaque fichier décrit :

- le rôle de la page côté utilisateur,
- la structure (cartes, blocs, sélecteurs),
- les données temps réel consommées (champs WebSocket),
- les actions (endpoints HTTP et topics MQTT associés),
- les cas limites (WS déconnecté, capteur en panne, limite atteinte, etc.),
- les fichiers sources à modifier pour faire évoluer la page.

## Différence avec les autres dossiers

- **`docs/features/`** (ici) : **doc vivante** par page UI, toujours à jour avec le comportement actuel. Mise à jour à chaque modification visible par l'utilisateur.
- **`specs/features/`** : **ordres de travail** pour une évolution donnée. Éphémères (todo → doing → done), peuvent contenir des itérations. Ne décrivent pas l'état actuel mais l'écart à réaliser.
- **`docs/subsystems/`** : doc vivante par composant firmware (pump_controller, sensors, etc.).
- **`docs/adr/`** : décisions d'architecture immuables.

Quand une spec `specs/features/` est terminée et déplacée en `done/`, le fichier correspondant dans `docs/features/` doit refléter le nouvel état de la page.

## Index

| Page | URL | Description |
|------|-----|-------------|
| [Tableau de bord](page-dashboard.md) | `/dashboard` | Vue d'ensemble temps réel (filtration, éclairage, pH, ORP, température) |
| [Filtration](page-filtration.md) | `/filtration` | Configuration et contrôle de la pompe de filtration |
| [Éclairage](page-lighting.md) | `/lighting` | Configuration et contrôle de l'éclairage |
| [Température](page-temperature.md) | `/temperature` | Historique température et calibration DS18B20 |
| [pH](page-ph.md) | `/ph` | Mesure, régulation, historique et calibration pH |
| [ORP](page-orp.md) | `/orp` | Mesure, régulation, historique et calibration ORP (chlore) |
| [Produits](page-dosages.md) | `/dosages` | Suivi des bidons pH et chlore, alertes stock faible |
| [Paramètres](page-settings.md) | `/settings` | WiFi, MQTT, heure, sécurité, régulation avancée, système |

## Composants transverses

### `confirmDialog` — modale de confirmation générique (v2.12.0, feature-031)

Helper réutilisable dans [`data/app.js:242`](../../data/app.js) remplaçant tous les `confirm()` natifs :

```js
const ok = await confirmDialog({
  title: 'Effacer les logs ?',
  message: 'Vide la RAM et supprime le fichier persistant côté ESP32.',
  confirmLabel: 'Effacer',   // défaut : 'Confirmer'
  cancelLabel: 'Annuler',    // défaut : 'Annuler'
  danger: true               // bouton rouge (btn--danger) pour action irréversible
});
if (!ok) return; // refus = défaut sûr, l'action n'est jamais exécutée
```

- **Retour** : `Promise<boolean>` — `true` uniquement sur clic « Confirmer ». Annuler, `Escape`, clic backdrop, réentrance (modale déjà ouverte) ou échec d'ouverture ⇒ `false`.
- **Markup** : `<dialog id="confirm-modal">` statique dans `index.html`, style `probe-modal` (cohérent avec les modales sonde/filtre).
- **Accessibilité** : focus initial sur **Annuler** (action sûre par défaut), `aria-labelledby`, fermeture `Escape`/backdrop.
- **Anti-XSS** : titre et message injectés via `textContent` uniquement (jamais `innerHTML`) ; les `\n\n` sont rendus par `white-space: pre-line`.
- **Fallback** : navigateur sans `<dialog>` → `window.confirm(message)` natif (une confirmation de sécurité n'est **jamais** supprimée).

**Règle** : toute nouvelle action destructive de l'UI **doit** passer par `confirmDialog` (variante `danger: true` si irréversible) — jamais `confirm()` natif. Les messages informatifs passent par `showToast(message, type)`.

## Conventions

- Toutes les pages sont des sections `<section class="view" id="view-XXX">` dans un seul [`data/index.html`](../../data/index.html) (voir [ADR-0006](../adr/0006-frontend-vanilla-js.md)).
- La navigation utilise le fragment `#/dashboard`, `#/filtration`, etc. (SPA vanilla JS).
- Toutes les données temps réel viennent du WebSocket `/ws` (voir [ADR-0005](../adr/0005-websocket-push-sans-polling.md)).
- Toutes les actions passent par des endpoints HTTP authentifiés documentés dans [`docs/API.md`](../API.md).
