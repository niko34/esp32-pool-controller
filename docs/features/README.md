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
| [Assistant de premier démarrage](page-wizard.md) | `/wizard.html` | Onboarding : mot de passe, Wi-Fi, installation et bassin, heure |

## Composants transverses

### `showToast(message, type)` — notification éphémère

Helper unique pour tous les messages informatifs ([`data/app.js`](../../data/app.js), style `.toast` dans [`data/app.css`](../../data/app.css)).

- **Types** : `info` (bleu, défaut), `success` (vert), `error` (rouge), `warning` (orange). Le style `.toast--warning` **manquait jusqu'à la v2.18.6** : les quatre appels de type `warning` (calibrations pH/ORP sans retour, notamment) produisaient des toasts sans fond, illisibles.
- **Convention `info` — indisponibilités matérielles (v2.18.7)** : quand une fonction n'est pas disponible en raison d'une **caractéristique du matériel** (rien à corriger, aucune action utilisateur possible pour y remédier), le message passe en type `info` et est affiché **sans préfixe** — ni « Erreur … : », ni orange, qui suggéreraient à tort une panne ou une action. Cas de référence : le `503 psram_required` de la mise à jour en ligne, dont le champ `error` est repris tel quel (voir [page-settings.md](page-settings.md#système-panel-system)). `error` reste réservé aux échecs (réseau, refus corrigeable), `warning` aux situations qui appellent une action.
  - **Indisponibilités temporaires (v2.18.7)** : même type `info` et même absence de préfixe pour les refus `psram_low` et `tls_low_memory` de la même carte (PSRAM saturée, heap dégradé) — la différence n'est pas la tonalité mais l'**effet** : eux ne neutralisent rien, l'utilisateur peut réessayer. Le front les distingue par le champ `code`, **jamais** par le texte du message.
- **Durée adaptative (v2.18.6)** : `40 ms` par caractère, borné à **[3 s, 9 s]**. Un message court garde donc exactement les 3 s historiques ; un message long (ex. le refus de mise à jour en ligne sans PSRAM, ~220 caractères) reste affiché ~9 s. Disparition par transition d'opacité (300 ms).
- **Messages longs (v2.18.6)** : `max-width: min(92vw, 560px)`, `white-space: normal`, `overflow-wrap: anywhere` — le texte revient à la ligne au lieu de déborder du viewport (la largeur reste en *shrink-to-fit* : les toasts courts gardent leur apparence compacte).
- **Accessibilité** : `role="alert"` + `aria-live="assertive"` pour `error` ; `role="status"` + `aria-live="polite"` sinon ; `aria-atomic="true"`.
- **Anti-XSS** : message injecté via `textContent`, jamais `innerHTML`.

Les erreurs d'API sont affichées telles quelles depuis le champ `error` de la réponse JSON : les messages serveur doivent donc rester **rédigés pour l'utilisateur final** (cf. les 503 de [`/check-update`](../API.md#garde-tls-sortante-psram-libre--heap-v2186)). Le champ `error` sert **uniquement à l'affichage** : toute décision d'UI (neutraliser un bouton, autoriser un réessai) doit se fonder sur le champ `code`, jamais sur le contenu du texte.

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
