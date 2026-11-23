## Métadonnées
- **ID** : feature-213
- **Slug** : photographier-l-etat-connexion-en-cours
- **Type** : feature
- **Statut** : todo
- **Priorité** : normale
- **Version cible** : `N/A` — outil de développement, ⛔ ne change rien dans l'appareil
- **Dernière mise à jour** : 2026-11-22
- **Spec(s) liée(s)** : [bug-196](../done/bug-196-le-bandeau-hors-ligne-passe-sous-le-bouton-menu.md) (origine), [bug-212](../done/bug-212-les-pastilles-disent-hors-ligne-pendant-la-connexion.md) (premier client)

## Domaines touchés
- [ ] Frontend (`data/`) — ⛔ **aucun fichier livré n'est modifié**
- [x] Outillage (`tools/ui_shot.sh`)

## Agents requis
- [x] spec-reader
- [ ] architect
- [ ] ux-ui-designer — ⛔ aucune surface utilisateur n'est créée
- [x] web-ui-developer — il connaît le cycle de vie du WebSocket et de `/data`
- [x] test-engineer
- [x] code-reviewer
- [x] doc-writer

## Description
`./tools/ui_shot.sh` sait photographier l'application **en ligne** (`-n`) et **hors ligne**
(sans `-n`). ⛔ Il ne sait **pas** photographier le troisième état, « **Connexion…** » — celui
qui s'affiche à chaque ouverture de page. Tant qu'il n'est pas capturable, tout lot qui y
touche se vérifie « par lecture de code », ⛔ jamais à l'image.

## Contexte
⭐ **Constat du 2026-11-22**, pendant la revue de [bug-196](../done/bug-196-le-bandeau-hors-ligne-passe-sous-le-bouton-menu.md).

⚠️ **Ce n'est pas théorique.** Un faux vivait dans cet état **sans que personne ne l'ait vu** :
l'en-tête affirmait « Enregistrement impossible » alors que l'appareil venait de répondre, et
les pastilles du tableau de bord annoncent **encore** « Hors ligne » au même moment
([bug-212](../done/bug-212-les-pastilles-disent-hors-ligne-pendant-la-connexion.md)).

⛔ Les deux corrections de bug-196 ont dû être validées **par raisonnement sur la cascade CSS et
le cycle de vie du WebSocket**, pas par une capture. C'est exactement ce que
`CLAUDE.md` reproche : *« l'agent doit VOIR ce qu'il conçoit »*, et un brief d'ergonomie sans
image est un brief incomplet. **Il manque donc une image qu'on ne sait pas produire.**

## Comportement actuel
| option | WebSocket simulé | état atteint | capturable |
|---|---|---|---|
| `-n` | s'ouvre, puis se tait | `ok` (« En ligne ») | ✅ |
| *(aucune)* | aucun socket, échec | `bad` (« Déconnecté ») | ✅ |
| — | **reste en cours d'ouverture** | `mid` (« Connexion… ») | ⛔ **non** |

L'état `mid` est posé au démarrage **juste après un `loadConfig()` réussi**, et il n'en sort que
lorsque le WebSocket s'ouvre **ou** que `/data` répond. Il est donc **transitoire** : une
capture tombe presque toujours après.

## Comportement attendu
Une option — `-m` / `--mid`, nom à arrêter — qui **fige** l'application dans « Connexion… » de
façon **déterministe**, en combinant deux conditions que l'outil contrôle déjà :

1. un faux `WebSocket` qui **reste en `CONNECTING`** : ⛔ ni `onopen`, ni `onerror`, ni `onclose` ;
2. `/get-config` qui **répond normalement** (c'est lui qui prouve que l'appareil est joignable) ;
3. `/data` qui **ne résout jamais** — sinon l'application repasse en `ok`.

⇒ La capture doit montrer l'écran **exactement** tel qu'il est pendant la première seconde
d'une ouverture de page réelle.

### Cas limites
- ⚠️ **La capture ne doit pas expirer** : l'outil attend un rendu, et ici rien ne « finit ».
  Prévoir un délai borné et un rendu réputé stable.
- ⛔ **Aucune temporisation arbitraire** (« attendre 300 ms et espérer ») : la fiche existe
  précisément pour remplacer une vérification au jugé par une vérification **reproductible**.
- L'option doit être **exclusive** de `-n` — les deux posent un faux WebSocket contradictoire.

## Contraintes techniques
- ⛔ **Aucun fichier de `data/` ni de `src/` ne doit être modifié.** L'outil photographie
  l'application telle qu'elle est livrée ; ⛔ il ne doit jamais exister de code « spécial
  capture » dans le produit.
- ⚠️ `ui_shot.sh` lance un navigateur : il **se sérialise** avec `pio run`, `pio test` et
  `build_fs.sh` — la compilation est une ressource **exclusive et globale** (CLAUDE.md).
- ⚠️ **`window.innerWidth` ment sous 500 px** dans ce conteneur (piège déjà documenté dans
  l'aide de l'outil) — ⛔ ne pas en conclure qu'une capture est fausse.
- Les données recopiées contiennent le **SSID Wi-Fi** et l'**identifiant MQTT** : ⛔ elles
  restent dans un dossier temporaire, **jamais** dans le dépôt.

## Fichiers potentiellement concernés
- `tools/ui_shot.sh` — nouvelle option, aide en ligne, exclusion mutuelle avec `-n`
- `docs/DESIGN_SYSTEM.md` §9 — l'outil y est décrit
- `CLAUDE.md` — ⛔ **seulement si l'utilisateur le demande explicitement**

## Livrables — check-list
- [ ] A1 — L'option fige l'application dans « Connexion… », de façon déterministe
- [ ] A2 — Exclusion mutuelle avec `-n`, message d'erreur clair
- [ ] A3 — Aide en ligne de l'outil mise à jour, avec **pourquoi** cet état mérite une option
- [ ] C1 — `docs/DESIGN_SYSTEM.md` §9
- [ ] D1 — Capture de démonstration : en-tête **et** tableau de bord à 390 px

## Critères d'acceptation
- [ ] L'option produit une capture montrant « Connexion… » dans l'en-tête
- [ ] Deux exécutions successives donnent **le même** écran (déterminisme)
- [ ] ⛔ Aucun fichier de `data/` ni de `src/` n'est modifié par ce lot
- [ ] `-n` et la nouvelle option ensemble → refus explicite
- [ ] L'aide en ligne décrit les **trois** états et comment atteindre chacun

## Plan de test manuel
1. Lancer l'outil avec la nouvelle option à 390 px, **regarder l'image** : l'en-tête doit dire
   « Connexion… ».
2. Relancer : l'image doit être identique.
3. Combiner avec `-n` : refus.
4. Vérifier que les captures `ok` et `bad` existantes sont **inchangées**.

## Notes pour agent
- ⭐ **Le premier client de cette option est [bug-212](../done/bug-212-les-pastilles-disent-hors-ligne-pendant-la-connexion.md)** : son plan de test manuel dit
  explicitement que l'état n'est pas photographiable aujourd'hui. ⚠️ Si bug-212 est traité
  **avant**, sa vérification restera « par lecture » — c'est acceptable, mais à **écrire**.
- ⚠️ **Vérifier la fiche avant de la traiter** : elle décrit `ui_shot.sh` au 2026-11-22.

- ⭐ **Preuve que le besoin est réel (2026-11-22)** : bug-212, premier client, a été corrigé et vérifié **sans image** — par lecture de code et vecteurs de test — faute de pouvoir photographier « Connexion… ». La fiche reste ouverte.

## Hors périmètre
- ⛔ Tout changement de comportement de l'application elle-même.
- ⛔ L'ajout d'autres états simulés (capteur en panne, limite atteinte) : `-f` et `-c` existent
  déjà pour forcer des champs.
