# bug-214 — Le refus de vérification en ligne accuse GitHub au lieu des notes de release

## Métadonnées
- **ID** : bug-214
- **Slug** : le-refus-de-verification-en-ligne-accuse-github-au-lieu-des-notes
- **Type** : bug
- **Statut** : todo
- **Priorité** : normale — ⚠️ **défaut OBSERVÉ en production le 2026-11-23**, contourné le
  jour même en raccourcissant les notes. Rien n'empêche la récidive à la prochaine release.
- **Version cible** : à définir (`src/version.h` = 3.3.0 au moment de l'écriture)
- **Dernière mise à jour** : 2026-11-23
- **Spec(s) liée(s)** : feature-151 (signature OTA), bug-173 (filtre ArduinoJson), bug-169

## Domaines touchés
- [x] Web — routes HTTP / WebSocket
- [x] OTA
- [ ] Capteurs · Régulation · Filtration · MQTT · UART · Authentification · Historique · Frontend

## Agents requis
- [x] spec-reader
- [x] architect
- [ ] pool-chemistry — sans objet
- [ ] ux-ui-designer — ⚠️ **à trancher par l'architecte** : si le libellé affiché à
      l'utilisateur change, il relève de l'interface. Le reste est du texte de journal.
- [x] embedded-developer
- [x] web-ui-developer — seulement si le message affiché change
- [x] test-engineer
- [x] code-reviewer
- [x] doc-writer

## Description

Quand le corps des notes d'une release GitHub dépasse le budget de parse de l'appareil, la
vérification de mise à jour échoue avec **« GitHub response too large »**. Le message accuse
GitHub, alors que la cause est **le texte que nous écrivons nous-mêmes** au moment de publier.
Et rien, dans la procédure de release, ne mesure ce texte avant publication.

## Contexte — ce qui s'est réellement passé

**2026-11-23, release v3.3.0.** Les notes de release faisaient **6 243 octets**. L'utilisateur
a lancé « Vérifier » depuis la page de mise à jour en ligne et a reçu l'erreur. La mise à jour
était impossible **alors que la release était parfaitement formée** : les quatre assets aux noms
exacts étaient présents, les deux signatures valides, l'API répondait HTTP 200 sans jeton.

### Le mécanisme, mesuré

`/check-update` parse la réponse GitHub en flux, avec un filtre ArduinoJson qui ne retient que
7 champs (`src/web_routes_ota.cpp:1956-1962`). Ce parse dispose d'un budget d'allocation
**plafonné** :

```
src/constants.h:1473   constexpr size_t kOtaCheckJsonBudgetBytes = 8192;
```

Le commentaire qui le précède dit comment il a été dimensionné : *« corps ≤ ~2,5 Ko +
doublement transitoire du buffer de chaîne + pool de variants ~1 Ko »*. ⚠️ **Ce « ≤ 2,5 Ko »
n'est écrit nulle part ailleurs** — ni dans la commande de release, ni dans un contrôle, ni
dans la documentation. C'est une hypothèse de dimensionnement, pas une garde.

Mesures faites le jour même sur les champs **réellement retenus par le filtre** :

| Release | Contenu retenu | dont corps | dont assets | Résultat |
|---|---|---|---|---|
| v2.88.1 | 3 782 o | 1 671 o | 1 910 o (8 assets) | fonctionnait |
| v2.88.2 | 4 064 o | 1 959 o | 1 910 o (8 assets) | fonctionnait |
| **v3.3.0 publiée** | **≈ 8 321 o** (reconstitué) | **6 243 o** | 1 894 o (8 assets) | ⛔ **refus** |
| v3.3.0 après correction | 3 607 o | 1 529 o | 1 894 o (8 assets) | ✅ |

⭐ **La part « assets » est quasi constante** (~1 900 o pour 8 assets, dont les URL et les
empreintes `digest`). ⇒ **Le seul levier réel est la longueur du corps**, et c'est précisément
la seule chose que personne ne mesure.

ℹ️ La ligne « v3.3.0 publiée » est une **reconstitution arithmétique** (3 607 − 1 529 + 6 243),
pas une mesure directe : la release avait déjà été corrigée quand le calcul a été fait.

## Comportement actuel

1. **Le message affiché accuse la mauvaise partie.** `sendErrorResponse(request, 500, "GitHub
   response too large")` (`src/web_routes_ota.cpp:1981`) désigne GitHub. Un utilisateur — ou un
   client — en conclut une panne distante et attend. ⛔ Rien ne reviendra : l'état est stable
   jusqu'à ce que quelqu'un réécrive les notes.
2. **Le journal ne dit pas quoi faire.** « Réponse GitHub hors budget JSON /check-update
   (8192 o) » donne le plafond, ⛔ jamais la taille constatée, ⛔ jamais la cause actionnable.
3. **Aucune garde avant publication.** `/release` écrit les notes et publie. `gh release
   create` accepte n'importe quelle longueur.
4. **Le message est en anglais**, à rebours de la règle de langue du projet.

## Comportement attendu

### Volet A — dire la vraie cause (firmware)

- Le refus nomme **ce qui est trop gros** et **de combien**, en français, avec la taille
  constatée **et** le plafond.
- ⚠️ **À trancher par l'architecte** : la taille constatée est-elle disponible au point de
  refus ? `NoMemory` remonte de l'allocateur, pas du contenu. ⛔ Ne pas inventer un chiffre
  qu'on n'a pas — il vaut mieux un message qui dit « les notes de la release sont trop
  longues » sans chiffre qu'un chiffre faux.
- ⛔ **Ne PAS relever le plafond en réflexe.** Il protège d'une consommation non bornée sur un
  appareil dont le plus gros bloc contigu nominal tourne autour de 25 Ko. Si l'étude conclut
  qu'il faut le bouger, elle dit **pourquoi** et **sur quelle mesure**.
- ⭐ **Piste à instruire : TRONQUER le corps, ⛔ pas le supprimer.** ⚠️ Vérifié dans le code
  le 2026-11-23 : `body` est bien consommé (`src/web_routes_ota.cpp:2088` →
  `doc["body"].as<String>()`, servi comme `release_notes`) et **affiché à l'utilisateur**
  (`data/app.js:12353`, `#release_notes`). ⛔ Le retirer du filtre ferait disparaître les
  notes de la page de mise à jour — ce n'est donc pas une option gratuite. En revanche, les
  tronquer à la lecture bornerait le poids retenu quelle que soit la longueur publiée. ⚠️ À
  instruire : ArduinoJson permet-il de borner la longueur d'une chaîne **pendant** le parse
  filtré, ou faut-il la couper en amont ? Si la troncature n'est possible qu'après coup, elle
  n'économise rien — c'est le pic d'allocation qui refuse, pas la chaîne finale.

### Volet B — empêcher la récidive (outillage)

- Un contrôle **à code de sortie** (famille `tools/check_*.sh`) qui mesure les champs retenus
  par le filtre — pas la réponse entière — et refuse au-delà du seuil.
- Appelé par `/release` **avant** `gh release create`, pas après : une release publiée puis
  corrigée laisse une fenêtre où les appareils refusent.
- ⚠️ Le seuil se dérive de `kOtaCheckJsonBudgetBytes`, il ne se recopie pas à la main — sinon
  les deux divergent le jour où le budget bouge.

### Cas limites
- Release **sans** notes (corps vide) : doit passer.
- Release avec **plus de 8 assets** : la part incompressible grandit. Le contrôle mesure, il ne
  suppose pas 8.
- Appareil **hors ligne** ou API en erreur : les messages existants ne changent pas.
- Release de **pré-version** ou brouillon : hors périmètre de `/check-update`.

## Fichiers potentiellement concernés
- `src/web_routes_ota.cpp` — point de refus (`:1981`), filtre (`:1956-1962`), journal (`:1979`)
- `src/constants.h` — `kOtaCheckJsonBudgetBytes` (`:1473`) et son commentaire de dimensionnement
- `data/app.js` / `data/index.html` — [nouveau] seulement si le texte affiché change
- `tools/check_release_notes_size.sh` — [nouveau] contrôle de taille
- `.claude/commands/release.md` — étape appelant le contrôle
- `.github/workflows/hygiene-specs.yml` — job si le contrôle est CI-able
- `docs/API.md`, `docs/UPDATE_GUIDE.md` — le refus et sa cause

## Livrables — check-list
- [ ] A1 — Le refus de `/check-update` nomme la cause réelle, en français
- [ ] A2 — Le journal porte la taille constatée **si elle est disponible** (sinon : le dire)
- [ ] A3 — Décision tracée sur le champ `body` : retiré du filtre, ou conservé avec sa raison
- [ ] B1 — `tools/check_release_notes_size.sh`, seuil **dérivé** de `kOtaCheckJsonBudgetBytes`
- [ ] B2 — `/release` appelle ce contrôle **avant** `gh release create`
- [ ] C1 — `docs/UPDATE_GUIDE.md` : ce que signifie ce refus et qui le corrige
- [ ] D1 — Test natif sur le calcul de taille (vecteurs : corps vide, 8 assets, dépassement)

## Sécurité chimique
- Dosage impliqué : **non**. Aucun chemin vers `pump_controller`.

## ADR attendu ?
- **Probable** : oui, **si** le volet A conclut à retirer `body` du filtre ou à bouger le
  plafond — les deux verrouillent un contrat OTA.
- **Critère(s) candidat(s)** : 2 (décision invisible dans le code), 4 (compat OTA)
- **Sujet pressenti** : ce que l'appareil accepte de lire d'une release, et pourquoi

## Éléments visibles (pour doc-writer)

### Endpoints HTTP concernés
- `GET /check-update` — texte du refus 500

### Champs WebSocket
- aucun

### Topics MQTT
- aucun

### Documentation à produire / mettre à jour
- `docs/subsystems/ota-manager.md` — budget de parse et sa cause de dépassement
- `docs/API.md` — libellé du refus
- `docs/UPDATE_GUIDE.md` — que faire quand ce refus apparaît
- `CHANGELOG.md` — entrée sous la version cible

## Critères d'acceptation
- [ ] Avec des notes de 6 243 o, le refus **nomme les notes de release**, ⛔ jamais « GitHub »
- [ ] Avec des notes de 1 500 o et 8 assets, la vérification aboutit
- [ ] Le contrôle de release **refuse** (code non nul) des notes hors budget, **avant** publication
- [ ] Le contrôle passe sur une release sans notes
- [ ] Tous les libellés utilisateur sont en français
- [ ] `pio run` compile sans nouveau warning

## Plan de test manuel
1. Publier une release d'essai aux notes volontairement longues, lancer « Vérifier » : le
   message doit désigner les notes.
2. Raccourcir les notes, relancer : la vérification aboutit, la version proposée est la bonne.
3. Lancer le contrôle de l'étape B sur les deux états : refus puis acceptation.
4. ⚠️ **Le test 1 publie une release réelle** — la supprimer après, ou la marquer brouillon.

## Notes pour agent

- ⚠️ **Ce défaut n'a été vu que parce qu'un humain a cliqué.** Les trois contrôles CI étaient
  verts, la release bien formée, les signatures valides. Même famille que bug-170
  (`littlefs.bin` absent des assets pendant six semaines) et que la leçon de la mémoire
  projet : *cliquer avec de vraies données, pas seulement lire le code*.
- ⛔ **Ne pas conclure que l'appareil est en cause.** Il a refusé **proprement**, sans crash,
  exactement comme son commentaire de code l'annonce. Le défaut est qu'il refuse **en
  désignant un tiers**, et que rien en amont ne l'évite.
- ⚠️ La règle de langue du projet vaut aussi pour les messages d'erreur rendus par les routes :
  plusieurs sont encore en anglais autour de celui-ci. **Ne pas élargir** le lot à tous — le
  signaler, et traiter celui de ce bug.

## Hors périmètre
- Traduire l'ensemble des messages d'erreur HTTP du firmware.
- Revoir le dimensionnement général des budgets d'allocation JSON hors `/check-update`.
- La longueur des notes elle-même en tant que choix éditorial : la fiche borne, elle n'écrit pas.
