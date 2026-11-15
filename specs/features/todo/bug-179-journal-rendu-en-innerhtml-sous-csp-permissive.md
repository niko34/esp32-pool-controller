## Métadonnées
- **ID** : bug-179
- **Slug** : journal-rendu-en-innerhtml-sous-csp-permissive
- **Type** : bug — 🔴 **sécurité applicative**
- **Statut** : todo
- **Priorité** : haute
- **Version cible** : 2.89.0
- **Spec(s) liée(s)** : `bug-178` (a fermé UN chemin vers ce défaut, pas le défaut), `bug-112`

## Domaines touchés
- [ ] Capteurs · [ ] Régulation · [ ] Filtration / Éclairage
- [x] Web — routes HTTP / WebSocket
- [ ] MQTT / Home Assistant · [ ] UART / écran LVGL
- [x] Authentification (portée de l'exposition)
- [x] Historique / persistance (le journal)
- [x] Frontend (`data/`)

## Agents requis
- [x] spec-reader · [x] architect · [ ] pool-chemistry · [ ] ux-ui-designer
- [ ] embedded-developer · [x] web-ui-developer · [x] test-engineer · [x] code-reviewer · [x] doc-writer

⚠️ `security-reviewer` : **OBLIGATOIRE**, c'est l'objet même de la spec.

## Description
Le journal système est rendu dans la page par **`innerHTML` sans échappement**, sous une CSP qui
autorise `script-src 'unsafe-inline'`. ⇒ Toute chaîne non maîtrisée qui atteint une ligne de
journal **s'exécute dans la session de l'utilisateur** à l'ouverture du journal.

## Contexte

**Relevé par la revue de sécurité de `bug-178`, le 2026-11-15.**

```
data/app.js:223-225     → rendu des lignes de journal par innerHTML, sans échappement
src/web_server.cpp:86   → CSP : script-src 'unsafe-inline'
```
Aucun échappement côté firmware (recherche vide).

### Ce que `bug-178` a fermé — et ce qu'il n'a PAS fermé
`bug-178` journalisait `tag_name`, chaîne venue de **GitHub**. ⇒ Corrigé **sur ce chemin
précis** : la version n'est journalisée que si l'analyseur l'accepte (chiffres et points
uniquement), sinon un libellé fixe.
⛔ **Le mécanisme, lui, reste ouvert** : n'importe quelle autre donnée non maîtrisée atteignant le
journal produit le même effet. Fermer un chemin n'est pas fermer un défaut.

### 🔴 Soupçon à vérifier EN PREMIER — il décide de la gravité
La revue signale, **sans l'avoir vérifié**, que d'autres lignes de journal porteraient des
**entrées non authentifiées** (par exemple des refus d'authentification, qui journalisent des
données fournies par l'appelant).
- **Si c'est faux** : l'exploitation exige un **dépôt GitHub compromis** ⇒ gravité modérée.
- ⚠️ **Si c'est vrai** : n'importe qui **sur le réseau local**, **sans authentification**, peut
  déposer du contenu dans le journal, qui s'exécutera **dans la session admin** à sa prochaine
  ouverture. ⇒ **Élévation de privilège par le réseau local.**
⇒ **C'est la première chose à établir, avant toute conception.**

## Comportement attendu
1. ⛔ **Aucune ligne de journal n'est rendue par `innerHTML`.** Construction DOM (`createElement`
   + `textContent`) pour **tout** contenu de journal.
2. Le rendu reste lisible et aussi rapide qu'aujourd'hui (⚠️ le journal peut porter des centaines
   de lignes — mesurer, ne pas supposer).
3. **Défense en profondeur** : retirer `'unsafe-inline'` de `script-src`, ou dire précisément ce
   qui l'exige encore et ouvrir le retrait comme lot suivant.
4. Si le soupçon ci-dessus est confirmé : **borner et assainir** ce qui entre dans le journal
   depuis une source non authentifiée (⚠️ rejoint `bug-112`).

### Cas limites
- Journal volumineux (8 Ko volatil, persistant plus gros) ⇒ performance du rendu DOM.
- Lignes contenant des chevrons **légitimes** (`->`, `<=`) : ⛔ elles doivent rester **lisibles**,
  pas échappées en entités visibles.
- Caractères de contrôle, bidi, chaînes très longues.
- ⚠️ Le journal est aussi téléchargé (`/download-logs`) : le fichier brut n'est pas concerné, seul
  l'affichage l'est.

## Comportement actuel
`data/app.js:223-225` concatène les lignes dans `innerHTML`. Le firmware n'échappe rien. La CSP
autorise l'inline.

## Contraintes techniques
- Vanilla JS, pas de framework. Français dans l'UI.
- ⚠️ **RESPONSIVE** : 390 px d'abord.
- Budget FS `spiffs` 512 KB.
- ⛔ Ne pas dégrader la lisibilité du journal : c'est l'outil de diagnostic principal du projet.

## Fichiers potentiellement concernés
- `data/app.js` — rendu du journal (`:223-225`) et tout autre point d'affichage de lignes
- `src/web_server.cpp` — en-tête CSP (`:86`)
- `src/logger.*`, `src/web_routes_*` — si un assainissement à la source est retenu
- `docs/subsystems/logger.md`, `docs/API.md`

## Livrables — check-list
- [ ] A0 — ⭐ **Établir le soupçon** : des entrées **non authentifiées** atteignent-elles le journal ?
- [ ] B1 — Rendu du journal en DOM, plus aucun `innerHTML`
- [ ] B2 — Lisibilité préservée (chevrons légitimes affichés tels quels)
- [ ] B3 — Performance mesurée sur un journal réaliste
- [ ] A1 — CSP : retrait d'`'unsafe-inline'`, ou inventaire écrit de ce qui l'exige
- [ ] A2 — Si A0 confirmé : bornage/assainissement des entrées non authentifiées
- [ ] C1 — `docs/subsystems/logger.md`, `CHANGELOG.md`
- [ ] D1 — Test : une ligne de journal contenant du balisage s'affiche en **texte inerte**
- [ ] D2 — Validation par MUTATION
- [ ] D3 — Essai de terrain

## ADR attendu ?
- **Probable** : non (sauf si le retrait d'`'unsafe-inline'` impose une refonte du chargement des scripts)

## Critères d'acceptation
- [ ] Une ligne de journal contenant `<img src=x onerror=...>` s'affiche **en texte**, rien ne s'exécute
- [ ] Les chevrons légitimes restent lisibles
- [ ] Le rendu d'un journal réaliste n'est pas plus lent qu'avant (mesuré)
- [ ] Le soupçon A0 est **tranché par écrit**, dans un sens ou dans l'autre
- [ ] `./build_fs.sh` passe · suite native verte

## Plan de test manuel
1. Injecter une ligne de journal porteuse de balisage (par le chemin établi en A0) puis ouvrir le journal.
2. Vérifier l'affichage de lignes contenant `->` et `<=`.
3. Journal de plusieurs centaines de lignes : mesurer le rendu.

## Notes pour agent
- ⚠️ **Défaut PRÉEXISTANT**, relevé en auditant autre chose. Il exige aujourd'hui un dépôt
  compromis **ou** le chemin non authentifié de A0 — c'est A0 qui décide de la gravité réelle.
- ⚠️ L'appareil a des **clients** et pilote des pompes à **acide**.
