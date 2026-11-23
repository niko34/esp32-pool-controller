## Métadonnées
- **ID** : feature-213
- **Slug** : photographier-l-etat-connexion-en-cours
- **Type** : feature
- **Statut** : ✅ **done** — 2026-11-23.
- **Priorité** : normale
- **Version cible** : `N/A` — outil de développement, ⛔ ne change rien dans l'appareil
- **Dernière mise à jour** : 2026-11-23
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
- [x] A1 — `-m` fige l'application dans « Connexion… », **par construction**
- [x] A2 — Exclusion mutuelle avec `-n`, code de sortie `2`, **avant** toute lecture de l'appareil
- [x] A3 — Aide en ligne : tableau des **trois** états + pourquoi celui-ci mérite une option
- [x] C1 — `docs/DESIGN_SYSTEM.md` §9 : sous-section « Les TROIS états de liaison »
- [x] D1 — Capture de démonstration prise à 390 px (en-tête **et** tableau de bord)

## Critères d'acceptation
- [x] L'option produit une capture montrant « Connexion… » dans l'en-tête — **vérifié à l'image**
- [x] Deux exécutions donnent le même écran **à 8×8 px près** — voir la note ci-dessous
- [x] ⛔ Aucun fichier de `data/` ni de `src/` modifié — vérifié sur `git status`
- [x] `-n` et `-m` ensemble → refus explicite, **couvert en CI**
- [x] L'aide en ligne décrit les **trois** états et comment atteindre chacun

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

---

## Notes techniques de clôture (2026-11-23)

### Ce qui a été livré
| Fichier | Rôle |
|---|---|
| `tools/ui_shot.sh` | option `-m`, exclusion mutuelle avec `-n`, aide en ligne des trois états |
| `tools/ui_shot_server.py` | `_mid_script()` — la pose ; module rendu **importable** (le serveur ne démarre plus que sous `__main__`) |
| `test/mid_stub_vectors.js` | 20 assertions : la pose **exécutée** sous node |
| `test/mid_stub_plumbing.py` | câblage de bout en bout : le vrai `ui_shot.sh`, faux appareil + faux navigateur |
| `tools/check_mid_stub_vectors.sh` | la garde, + job CI `mid-stub-vectors` |
| `.gitignore` | `ui-shot-*.png` — voir plus bas |

⛔ **Aucun fichier de `data/` ni de `src/`.** `src/version.h` n'est **pas** bumpé : rien ne change
dans l'appareil, et il n'y a donc rien à déployer.

### Le choix de conception, et pourquoi il n'y a aucune temporisation
Les **quatre** sorties de `mid` dans le code livré :

| sortie | condition | pose |
|---|---|---|
| `_ws.onopen` | le socket s'ouvre | `ok` |
| `_onDisconnect` | `onerror` / `onclose` | `bad` |
| battement de cœur WS | **armé par `onopen` seul** | `bad` |
| issue de `/data` | succès **ou** échec | `ok` / `mid` / `bad` |

Le faux socket reste en `CONNECTING` et n'émet **jamais** rien ⇒ les trois premières sont closes.
`/data` rend une promesse **qui ne se résout jamais** ⇒ ni le `try` ni le `catch` de
`loadSensorData` ne tournent.
⚠️ **Un échec aurait été un mauvais choix** : il appellerait `setNetStatus('mid')` au premier, puis
`('bad')` au second — l'écran basculerait pendant la capture.
⚠️ Le signal d'abandon à 10 s de l'application est **ignoré** : il annule une requête réseau, pas
cette promesse. C'est ce qui rend l'état insensible à la durée de la capture.
⇒ `/get-config` passe **normalement** : c'est lui qui prouve que l'appareil est joignable, et c'est
tout le sens de cet état.

### ⚠️ Déterminisme : la réponse exacte, mesurée
Deux exécutions ne donnent **pas** un PNG identique à l'octet. L'unique zone qui diffère est de
**8×8 px, en (58,38)-(66,46)** : la pastille d'état de l'en-tête. Elle porte
`animation: pulse … infinite` (`app.css`), et la phase diffère d'un lancement à l'autre.
⭐ **Ce n'est pas propre à `-m`** : `-n` présente **exactement** la même zone de différence, au même
endroit — mesuré le 2026-11-23. ⇒ Le critère « même écran » est tenu ; « même fichier » ne l'est
pour **aucun** des trois états, et ne pouvait pas l'être.

### Revue de code — deux constats, tous deux dans mon propre travail de vérification
1. 🔴 **Un mutant a survécu : `export UI_SHOT_MID="$MID_STUB"` → `export UI_SHOT_MID=0`.** Les
   vecteurs interrogeaient `ui_shot_server.py` **directement** : l'option pouvait cesser d'arriver
   jusqu'à la page sans qu'aucun contrôle ne bouge. ⚠️ L'option serait morte **en silence**, comme
   `littlefs.bin` pendant six semaines (bug-170). ⇒ `test/mid_stub_plumbing.py` lance le **vrai**
   `ui_shot.sh` contre un faux appareil et un faux navigateur.
2. 🔴 **Un second mutant a survécu pour une raison structurelle** : détourner **aussi**
   `/get-config` laissait la garde verte — non parce qu'un vecteur le ratait, mais parce qu'un
   `await` sur une promesse qui ne se résout jamais fait **sortir node en 0**. La file d'événements
   se vide, le harnais s'arrête **au milieu**, et l'absence de sortie passe pour un succès.
   ⚠️ **Même famille que la sentinelle de bug-212 : un contrôle qui ne PEUT PAS échouer.**
   ⇒ Deux fermetures : les URL ne sont plus attendues une à une mais **comptées** après vidage, et
   une garde `process.on("exit")` rend 1 tant que la dernière ligne n'a pas tourné.

**16 mutations posées, 16 tuées** (après ces deux corrections). Sources restaurées à l'identique
(`cmp`).

### Trouvé en chemin, hors périmètre de la fiche mais traité
⚠️ La sortie **par défaut** de `ui_shot.sh` est `ui-shot-<largeur>px.png` **à la racine du dépôt**,
et aucune règle ne l'ignorait. L'image montre l'interface avec les **données réelles du bassin** :
un `git add -A` l'emportait. Règle ajoutée à `.gitignore`.

### Limites écrites, non traitées
- ⛔ Les vecteurs prouvent que l'état ne **peut plus** être réécrit. Ils ne prouvent pas que l'état
  **posé** est le bon — c'est la **capture** qui le constate. Les deux se complètent.
- Un cinquième chemin d'écriture de l'état ajouté un jour à `app.js` ne serait pas vu : seul
  `loadSensorData` est exécuté par le vecteur central.
- ⚠️ **Constat fait en regardant l'image, et laissé ouvert** : dans cet état, la carte **Éclairage**
  affirme « Éteinte » alors que `/data` n'est jamais arrivé, pendant que Filtration et Température
  disent « Chargement… ». C'est l'asymétrie déclarée **délibérée** dans
  [bug-212](bug-212-les-pastilles-disent-hors-ligne-pendant-la-connexion.md) (l'éclairage est
  **déduit** de la configuration, pas observé) — mais elle n'avait jamais été **vue** sur un même
  écran. ⇒ À verser à [feature-195](feature-195-langage-visuel-du-tableau-de-bord.md) si la question
  est rouverte. ⛔ Rien n'a été changé ici : ce lot ne touche pas à l'application.
