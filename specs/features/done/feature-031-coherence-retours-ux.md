## Métadonnées
- **ID** : feature-031
- **Slug** : coherence-retours-ux
- **Type** : feature
- **Statut** : done
- **Priorité** : basse
- **Version cible** : **2.12.0**
- **Dernière mise à jour** : 2026-07-06 (livrée en v2.12.0)
- **Spec(s) liée(s)** : ~~feature-030~~ (n'existe plus ni en todo/ ni en done/ — contrainte de coordination caduque)

> **Annotations d'audit (2026-07-06)** :
> 1. Le problème a grossi : **43 occurrences** `alert()`/`confirm()` (vs ~30 en juin), dont ~13 `confirm()` destructifs (clear historique, logs, coredump, reset usine à DOUBLE confirmation l.5587+5591, redémarrage, déconnexion, mot de passe, reset sondes).
> 2. Les briques existent : `showToast` rodé (toasts 409 feature-006), pattern `<dialog>` maison (modales sonde/filtre).
> 3. **Risque technique principal** : `confirm()` est synchrone, `confirmDialog()` sera `Promise<boolean>` → conversion `async/await` des ~13 sites destructifs avec flux préservé (double confirmation enchaînée, countdown de redémarrage, redirection logout). Un `return` manquant après un refus = action destructive exécutée quand même → revue centrée sur ces flux (AC3).
> 4. Version cible réalignée : 2.12.0.

## Domaines touchés
- [ ] Capteurs (pH / ORP / température)
- [ ] Régulation (pump_controller, PID)
- [ ] Filtration / Éclairage
- [ ] Web — routes HTTP / WebSocket
- [ ] MQTT / Home Assistant
- [ ] UART / écran LVGL
- [ ] OTA
- [ ] Authentification
- [ ] Historique / persistance
- [x] Frontend (`data/`)

## Agents requis
- [x] spec-reader
- [x] architect
- [ ] pool-chemistry
- [x] ux-ui-designer — cohérence des composants de feedback (toast / modale de confirmation)
- [ ] embedded-developer
- [x] web-ui-developer
- [x] test-engineer
- [x] code-reviewer
- [x] doc-writer

## Description
Remplacer les **30 appels natifs `alert()` / `confirm()`** par le système de **toasts** existant (`showToast`) et une **modale de confirmation maison**, pour une UX cohérente, stylée et accessible.

## Contexte
La revue de code (2026-06-20) a relevé ~30 occurrences de `alert()`/`confirm()` natifs (`app.js:2121, 2133, 2160, 2184…`, notamment dans import/export/clear historique) alors qu'un système `showToast` stylé existe déjà. Les dialogues natifs sont bloquants, non stylés, non traduisibles de façon cohérente et incohérents avec le reste de l'UI (modales `<dialog>` déjà utilisées ailleurs, ex. modal sonde/filtre). C'est un point de finition, d'où la priorité basse, mais il améliore nettement la cohérence perçue.

## Comportement attendu
- Tous les `alert()` informatifs → `showToast(message, type)` (success/warning/error/info selon le contexte).
- Tous les `confirm()` (actions destructives : clear historique, reset, etc.) → **modale de confirmation maison** réutilisable (`<dialog>` stylée, boutons Confirmer/Annuler, focus management, fermeture `Escape`/backdrop), cohérente avec les modales existantes.
- Les actions destructives conservent une confirmation explicite (pas de suppression silencieuse).
- Messages en français, accessibles (ARIA, focus piégé dans la modale).

### Cas limites
- Action destructive annulée → aucun effet.
- Toast empilés → gestion de file existante préservée.
- Navigateur sans `<dialog>` → fallback documenté (déjà géré pour les modales existantes).

## Comportement actuel
- ~30 `alert()`/`confirm()` natifs dispersés dans `app.js`.
- `showToast` existe et est utilisé ailleurs ; aucune modale de confirmation générique réutilisable.

## Contraintes techniques
- Vanilla JS, pas de framework.
- Réutiliser le pattern `<dialog>` + styles `probe-modal` existants pour la modale de confirmation.
- Offline first ; français ; accessibilité (focus, ARIA).
- Budget FS — impact minime, vérifier `./build_fs.sh`.

## Fichiers potentiellement concernés
- `data/app.js` — remplacement des `alert`/`confirm`, helper `confirmDialog()` [modif]
- `data/index.html` — markup d'une modale de confirmation générique [modif]
- `data/app.css` — styles si nécessaire (réutiliser l'existant) [modif]
- `docs/features/page-settings.md` (et autres pages touchées) — note UX [doc]

## Nouvelles signatures / API (optionnelle)
- `Promise<boolean> confirmDialog({title, message, confirmLabel, danger})` — helper réutilisable

## Sécurité chimique (optionnelle)
- Dosage impliqué : non (UI uniquement). Veiller à ce que la confirmation des actions sensibles ne soit pas supprimée.

## ADR attendu ?
- **Probable** : non
- **Critère(s) candidat(s)** : aucun
- **Sujet pressenti** : aucun

## Éléments visibles (pour doc-writer)

### Endpoints HTTP concernés
- aucun

### Champs WebSocket
- aucun

### Topics MQTT
- aucun

### Documentation à produire / mettre à jour
#### Docs vivantes
- `docs/features/page-settings.md` — confirmations maison sur actions destructives (et autres pages concernées)

#### Docs transverses
- `CHANGELOG.md` — entrée UX

## Critères d'acceptation
- [x] AC1 — Aucun `alert()` ni `confirm()` natif ne subsiste dans `data/app.js` (audit grep à blanc — seules occurrences restantes : le fallback documenté `window.confirm` dans `confirmDialog()` et ses commentaires).
- [x] AC2 — Les messages informatifs passent par `showToast` ; les actions destructives par une modale de confirmation maison.
- [x] AC3 — Les actions destructives restent confirmées explicitement (annulation possible) — refus = défaut sûr sur les 12 flux async, vérifié site par site en revue.
- [x] AC4 — Modale accessible (focus piégé, `Escape`, backdrop) et en français.
- [x] `./build_fs.sh` passe sous la limite de partition.

## Plan de test manuel
1. Clear historique → modale de confirmation maison → Annuler (aucun effet) puis Confirmer (effet).
2. Import/export config → toasts cohérents.
3. Navigation clavier dans la modale → focus correct, `Escape` ferme.

## Notes pour agent
- Coordonner avec feature-030 (mêmes fichiers) : si feature-030 est faite d'abord, implémenter directement dans les modules ; sinon prévoir un rebase.
- Ne pas affaiblir les confirmations d'actions sensibles.

## Hors périmètre
- Refonte globale du design system.
- Notifications push / hors-app.

## Notes d'implémentation (2026-07-06, v2.12.0)

### Remplacements
- **43 dialogues natifs remplacés** : **31 `alert()`** → `showToast` (mapping `error`/`success`/`info` selon le contexte) et **12 `confirm()`** → nouvelle modale maison `confirmDialog()`.
- Reset usine : **double confirmation conservée** (deux modales enchaînées « Réinitialiser » puis « Tout effacer »).
- Emojis ⚠ retirés des messages : la variante danger (rouge) porte la sévérité.

### Design retenu (ux-ui-designer — verrouillé)
- `confirmDialog({title, message, confirmLabel, cancelLabel, danger}) → Promise<boolean>` (`data/app.js:242`).
- `<dialog id="confirm-modal">` **statique** dans `index.html`, pattern `probe-modal` existant.
- Focus initial sur **Annuler** (action sûre) ; `Escape` / backdrop = annuler ; réentrance refusée (`Promise.resolve(false)` si déjà ouverte) ; fallback `window.confirm` si `<dialog>` indisponible (jamais de suppression d'une confirmation de sécurité).
- Variante `danger: true` (bouton rouge `btn--danger`) pour les 7 actions irréversibles.

### Vérifications
- **code-reviewer : Approved** — les 12 flux async convertis vérifiés **site par site** : refus = défaut sûr partout (aucun `return` manquant après refus), injection du contenu en `textContent` uniquement (anti-XSS).
- `node --check`, `./build_fs.sh` et `pio run` verts.

### Mineures relevées (acceptées)
1. Fallback `window.confirm` sans titre (navigateurs anciens sans `<dialog>`) — message seul affiché.
2. `loadConfig` fire-and-forget dans un flux — **préexistant**, hors périmètre.

### Reste à faire — validation on-target (plan de test manuel de la spec)
- Clear historique : Annuler (aucun effet) puis Confirmer (effet).
- Navigation clavier dans la modale (focus correct).
- `Escape` ferme la modale.
