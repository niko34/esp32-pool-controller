## Métadonnées
- **ID** : bug-178
- **Slug** : mise-a-jour-proposee-vers-une-version-anterieure
- **Type** : bug
- **Statut** : done
- **Priorité** : haute
- **Version cible** : 2.88.1
- **Spec(s) liée(s)** : `bug-176` (découvert pendant son essai de terrain), `bug-169`

## Domaines touchés
- [ ] Capteurs (pH / ORP / température)
- [ ] Régulation (pump_controller, PID)
- [ ] Filtration / Éclairage
- [x] Web — routes HTTP / WebSocket
- [x] OTA
- [ ] MQTT / Home Assistant
- [ ] UART / écran LVGL
- [ ] Authentification
- [ ] Historique / persistance
- [x] Frontend (`data/`)

## Agents requis
- [x] spec-reader
- [x] architect
- [ ] pool-chemistry
- [x] ux-ui-designer
- [x] embedded-developer
- [x] web-ui-developer
- [x] test-engineer
- [x] code-reviewer
- [x] doc-writer

⚠️ `security-reviewer` : **obligatoire** (`web_routes_ota.cpp`, chaîne OTA).

## Description
L'appareil annonce « mise à jour disponible » dès que la version de la release **diffère** de la
sienne — y compris quand elle est **plus ANCIENNE**. Il propose alors un **retour en arrière**
présenté comme une mise à jour.

## Contexte

**Constaté le 2026-11-15**, pendant l'essai de terrain de `bug-176`. L'appareil venait de passer
en **2.88.0** par déploiement local ; la dernière release GitHub était la **2.87.1**.

```
GET /check-update  →  current_version 2.88.0 · latest_version 2.87.1 · update_available TRUE
```

### La cause, en une ligne
```cpp
// src/web_routes_ota.cpp:1866
bool updateAvailable = (latestVersion != currentVersion);
```
⇒ **Inégalité de CHAÎNES**, pas comparaison de versions. `"2.87.1" != "2.88.0"` est vrai, donc
l'appareil propose d'installer la 2.87.1 **par-dessus** la 2.88.0.

### Pourquoi ce n'est pas anodin
⚠️ L'utilisateur qui accepte **réinstalle une version antérieure** : il perd les correctifs de la
version installée, **y compris ceux qui corrigeaient la mise à jour elle-même**. Dans le cas
observé, accepter aurait réintroduit le défaut que `bug-176` venait de corriger, **et** remplacé la
page par celle qui ne sait pas appeler `/ota-sig`.

⚠️ **Le défaut est PRÉEXISTANT** (il n'est pas introduit par `bug-176`) mais il était **invisible** :
l'appareil n'avait jamais une version plus récente que la dernière release. Un déploiement local,
une pré-version, ou une release retirée de GitHub suffisent à le révéler.

⚠️ **Cas aggravant à considérer** : une release **supprimée** ou **remplacée** côté GitHub ferait
basculer tout le parc vers la version précédente, sans que personne n'ait rien demandé.

## Comportement attendu
1. ⛔ **Aucune mise à jour proposée vers une version strictement antérieure.** La comparaison doit
   être **sémantique** (majeur, mineur, correctif), pas textuelle.
2. **Versions égales** ⇒ « à jour », comportement actuel conservé.
3. **Version de l'appareil plus récente que la release** ⇒ état explicite et **non alarmant** :
   l'appareil est en avance, ce qui est normal pendant un développement. ⛔ Ne pas afficher
   « à jour » (ce serait faux), ⛔ ni « mise à jour disponible » (dangereux).
4. ⚠️ **Le retour arrière délibéré doit rester possible** — mais **explicitement**, jamais sous
   l'étiquette « mise à jour ». ⇒ Forme à arbitrer : la **mise à jour manuelle** y pourvoit déjà,
   ce qui peut suffire à ne rien ajouter à l'interface.

### Cas limites
- Format de version inattendu (suffixe, pré-version, chaîne vide, non numérique) ⇒ ⛔ **fail-safe :
  ne rien proposer**, et le dire, plutôt que de deviner.
- `tag_name` avec ou sans « v » initial (⚠️ le code retire déjà le préfixe).
- Nombres à plusieurs chiffres : ⚠️ `"2.9.0"` vs `"2.10.0"` — ⛔ une comparaison lexicographique
  les inverse. C'est le piège classique, il doit être couvert par un test.
- Composants manquants (`"2.88"` contre `"2.88.0"`).

## Comportement actuel
`web_routes_ota.cpp:1866` compare deux `String` par inégalité. Toute différence, dans un sens
comme dans l'autre, vaut « mise à jour disponible ».

## Contraintes techniques
- ESP32, offline first. Logique **pure** et testable en natif (ADR-0017).
- ⛔ Aucune dépendance neuve pour parser une version : quelques lignes suffisent.
- Champs de réponse **ADDITIFS** si un état neuf est exposé.
- ⚠️ **RESPONSIVE** si `data/` est touché : 390 px d'abord, libellés comptés.

## Fichiers potentiellement concernés
- `src/web_routes_ota.cpp` — `:1866` et la construction de la réponse `/check-update`
- `src/` — module de comparaison de versions **à créer**, en logique pure (ADR-0017)
- `data/app.js` — libellé et état de la carte « Mise à jour en ligne »
- `docs/API.md`, `docs/subsystems/ota-manager.md`, `docs/UPDATE_GUIDE.md`, `CHANGELOG.md`

## Livrables — check-list
- [x] A1 — Comparaison **sémantique** de versions, en module pur testable en natif
- [x] A2 — `/check-update` ne propose **jamais** une version antérieure
- [x] A3 — État « appareil en avance » distinct de « à jour » et de « mise à jour disponible »
- [x] A4 — Format inattendu ⇒ fail-safe, rien de proposé, état explicite
- [x] B1 — La page rend le nouvel état, libellés français comptés à 390 px
- [x] C1 — `docs/API.md`, `ota-manager.md`, `UPDATE_GUIDE.md`, `CHANGELOG.md`
- [x] D1 — Suite native : ⚠️ **`2.9.0` vs `2.10.0`**, égalité, antérieure, postérieure, formats
      invalides, préfixe « v », composants manquants
- [x] D2 — Validation par MUTATION (⚠️ dont le retour à une comparaison lexicographique)
- [ ] D3 — Essai de terrain : appareil **en avance** sur la release ⇒ rien n'est proposé

## Notes de clôture doc (2026-11-15)
- Livré (code, tests natifs, `pio run`, `build_fs.sh`) ; docs : `API.md`, `ota-manager.md`, `UPDATE_GUIDE.md`, `CHANGELOG.md` [2.88.1], `page-settings.md`, `DESIGN_SYSTEM.md`, `guide-utilisation.html`.
- ⚠️ **Aucun ADR**. Seule décision qui en mériterait un : le **refus des pré-versions** (tag suffixé = `unknown_version`). Si le projet distribue un jour des pré-versions, c'est ce choix-là qu'il faudra formaliser.
- ⛔ **D3 reste ouvert** : essai de terrain non fait. Spec laissée en `doing/`.

## ADR attendu ?
- **Probable** : non
- **Critère(s) candidat(s)** : aucun
- **Sujet pressenti** : aucun — correction d'un défaut, sans alternative structurante

## Éléments visibles (pour doc-writer)
### Endpoints HTTP concernés
- `GET /check-update` — sémantique de `update_available`, et éventuel état additif
### Champs WebSocket
- aucun
### Topics MQTT
- aucun
### Documentation à produire / mettre à jour
- `docs/API.md`, `docs/subsystems/ota-manager.md`, `docs/UPDATE_GUIDE.md`, `CHANGELOG.md`
- `docs/features/page-settings.md` si un libellé change

## Critères d'acceptation
- [x] Appareil **plus récent** que la release ⇒ ⛔ **rien n'est proposé**, et l'état le dit
- [x] Appareil **plus ancien** ⇒ mise à jour proposée, comportement inchangé
- [x] Versions **égales** ⇒ « à jour »
- [x] ⚠️ **`2.10.0` est reconnue postérieure à `2.9.0`**
- [x] Version illisible ⇒ rien n'est proposé, aucun plantage
- [x] Libellés en français · `pio run` sans nouveau warning · `build_fs.sh` passe
- [x] Suite native verte, mutations tuées

## Plan de test manuel
1. Appareil **en avance** sur la dernière release (⚠️ **situation réelle au 2026-11-15**) ⇒ rien proposé.
2. Appareil **en retard** ⇒ mise à jour proposée et installable.
3. Versions égales ⇒ « à jour ».
4. Release au `tag_name` malformé ⇒ rien proposé, message explicite.

## Notes pour agent
- ⚠️ **Découvert par un essai de terrain, pas par relecture.** Le défaut vivait depuis longtemps et
  restait invisible faute d'appareil en avance sur la release.
- ⚠️ **Ne pas « corriger » en bloquant le retour arrière manuel** : il doit rester possible, mais
  jamais présenté comme une mise à jour.
- ⚠️ L'appareil régule un bassin réel et **doit continuer à doser**.

## Hors périmètre
- Le relais des signatures : `bug-176`.
- Le rechargement aveugle après redémarrage : `bug-177`.
- Le refus sur signature invalide : D3-c de `feature-151`.

---

## 🏗️ CONCEPTION ARRÊTÉE (2026-11-15)

### Logique : dans le header pur EXISTANT, pas un module neuf
`src/ota_check_logic.h` (header-only, sans dépendance Arduino, déjà couvert par
`test/test_native_ota_check/`). ⛔ Créer un `ota_version_logic.*` ajouterait un fichier et une
suite pour trois fonctions relevant du mandat déclaré de ce header.

```
enum class OtaUpdateState : uint8_t { kUnknownVersion, kUpToDate, kUpdateAvailable, kDeviceAhead };
parseOtaVersion() · otaUpdateStateFor() · otaUpdateStateCode()
kOtaVersionMaxParts = 3 · kOtaVersionMaxDigits = 9   (999999999 < 2^32-1 ⇒ pas de débordement)
```

⛔ **Invariant central, à rendre testable** : `kUpdateAvailable` n'est renvoyé que si **les DEUX**
versions sont lisibles **ET** `latest > current`. Aucun autre chemin.
⭐ **Règle de confort sûre** : deux chaînes **identiques octet pour octet** (après retrait du `v`)
⇒ `kUpToDate`, **même illisibles** — deux chaînes identiques ne peuvent pas être un retour arrière.
⛔ **Suffixes refusés** (`-rc1`, `+b3`) ⇒ `kUnknownVersion`. ⚠️ Conséquence **à documenter** :
publier un jour un tag `v2.89.0-rc1` comme release normale ferait dire « version illisible » à tout
le parc. `api.github.com/.../releases/latest` exclut drafts et pré-versions par construction, donc
le cas ne peut venir que d'un tag malformé.

### Contrat : UN seul champ additif, `update_state`
| État | `update_available` | `update_state` |
|---|---|---|
| Appareil en retard | `true` | `update_available` |
| Égalité | `false` | `up_to_date` |
| **Appareil en avance** | `false` | `device_ahead` |
| Version illisible | `false` | `unknown_version` |
| Aucune release (404) | `false` | `no_release` |

⭐ **Pourquoi un champ plutôt que zéro** — l'argument qui a renversé mon analyse initiale : sans lui,
la page devrait **refaire la comparaison sémantique en JavaScript**, un langage où
`"2.10.0" < "2.9.0"` est **vrai**. Ce serait **le défaut qu'on corrige, recréé côté client**, et la
famille « deux expressions pour une même clé » que ce projet a déjà payée trois fois.
⇒ **L'appareil seul décide.** `update_available` reste et se **restreint** (sous-ensemble des cas
d'avant) : un client ancien fonctionne toujours et ne peut plus être induit en erreur.
⚠️ Le champ est **TOTAL**, branche 404 comprise : un client doit pouvoir brancher sur `update_state`
**seul**.

### Retour arrière : RIEN à ajouter à l'interface — vérifié dans le code
`POST /update` (envoi manuel) vérifie intégrité, authenticité et genre, et **ne compare aucune
version** (`web_routes_ota.cpp:671-1040`). `POST /download-update` ne connaît qu'une URL.
⇒ Ce lot garantit « rien n'est **proposé** », **pas** « le retour arrière est **bloqué** » — c'est
exactement ce que demande la spec. Réinstaller la **même** version n'était déjà pas proposé :
**aucune régression**.
⭐ **Nuance retenue** : ne rien ajouter ne veut pas dire ne rien **dire**. Le libellé de
`device_ahead` **nomme la carte « Mise à jour manuelle »** — zéro contrôle neuf, zéro champ neuf,
le manque de découvrabilité traité par un mot.

### Release supprimée ou remplacée
1. Release supprimée, une plus ancienne devient « latest » ⇒ `device_ahead` ⇒ rien proposé.
   **Le basculement du parc est neutralisé par la correction elle-même.**
2. Toutes supprimées ⇒ 404 ⇒ branche `no_release` existante.
3. ⚠️ **Release REMPLACÉE sous le même tag** ⇒ versions égales ⇒ `up_to_date`. ⛔ **Aucune
   comparaison de versions ne verra jamais ce cas** : le tag ne bouge pas.
   🔴 **CORRECTION (revue de sécurité, 2026-11-15)** — j'avais écrit que la chaîne empreinte +
   signature le couvrait. **C'EST FAUX EN L'ÉTAT** : l'empreinte vient de la **même réponse GitHub**
   que l'image, donc elle prouve le **transport**, pas l'**authenticité** ; et la signature
   **ne refuse encore rien** (`web_routes_ota.cpp:2090`, « aucun refus appliqué (D3-a) »), avec un
   plancher de version figé à `0.0.0` (`ota_manifest_logic.h:87-95`).
   ⇒ Formulation juste : **ce cas sera couvert QUAND D3-c de `feature-151` sera livré**, pas avant.
   ⚠️ C'est exactement le travers que ce projet documente : affirmer une couverture au lieu de la
   vérifier. **Limite assumée, à documenter comme telle.**
⛔ **Écarté : mémoriser en NVS la plus haute version jamais vue.** Ajoute un état persistant, des
écritures NVS, et un **mode d'échec neuf** — une release légitimement republiée plus bas après un
retrait d'urgence serait refusée **définitivement**. Le remède coûte plus que le mal.

### ADR : NON
Aucune alternative structurante (« comparer en JS » est un défaut, pas une option), contrat additif,
rien de verrouillé — **sauf un point** : le refus des suffixes interdit de proposer une pré-version.
⇒ À consigner ici et dans `docs/API.md`. ⚠️ **Si un jour le projet distribue des pré-versions,
c'est CE choix-là qui méritera un ADR**, pas celui-ci.

---

## 🎨 ÉTUDE ERGONOMIE — RENDUE (2026-11-15)

⚠️ **Limite déclarée par l'agent** : l'état `device_ahead` **ne peut pas être photographié** (branche
JS ; `ui_shot.sh` ne force que `/data` et `/get-config`). Conçu **par analogie** avec
`#no_update_msg`. ✅ L'avoir dit plutôt que d'affirmer un rendu jamais vu est le bon comportement.

### Libellés arrêtés, caractères comptés (gras 15 px ≈ 38 car./ligne à 390 px)
| État | Ligne en gras | Complément `.field__hint` |
|---|---|---|
| `device_ahead` | `ℹ La version installée est plus récente` (41) | `Aucune mise à jour proposée. Pour revenir en arrière : « Mise à jour manuelle », ci-dessous.` (91, 3 lignes) |
| `unknown_version` | `⚠ Version de la release illisible` (33) | `Le numéro publié n'a pas le format attendu. Aucune mise à jour proposée.` (72) |
| `up_to_date`, `no_release` | inchangés | — |

⛔ **Ma proposition « Version installée plus récente » (30 car.) est REJETÉE** : ambiguë (plus
récente que **quoi** ?) et un propriétaire peut se demander ce qu'on lui reproche. ⇒ Le terme de
comparaison doit être **nommé**.

### Choix visuels
- ⛔ **Aucune couleur neuve.** `device_ahead` : couleur de texte héritée, **sans teinte** — être en
  avance est **normal**. `unknown_version` : `var(--warn)` en **texte seul**, sans fond.
- ⭐ **Le GLYPHE porte le signal, jamais la couleur** ⇒ sûr en daltonisme.
- ⚠️ **Glyphes TEXTE `ℹ` et `⚠`, pas les variantes emoji `ℹ️` `⚠️`** : celles-ci se rendent **en
  couleur selon la plateforme**, hors du contrôle du design system. ⇒ Aligne au passage le `ℹ️`
  existant de `no_release` (une ligne, même lot).
- `.field__hint` dans un **contexte d'emploi nouveau** (attaché à un **constat**, non à un contrôle)
  ⇒ à consigner dans `docs/DESIGN_SYSTEM.md`. Le composant **n'évolue pas**, CSS inchangé ⇒ aucun
  inventaire d'emplois dû.

### Implémentation imposée
- ⛔ **Construction DOM (`createElement` + `textContent`), jamais `innerHTML`** sur `#no_update_msg`
  — ⭐ **ce qui lève au passage la réserve portée depuis `feature-151`** sur ce même élément.
- `role="status"` sur `#no_update_msg` (contenu dynamique). ⛔ Aucune ID ni classe neuve, **aucun CSS**.
- ⚠️ **Vider l'élément AU DÉBUT de chaque vérification** : sinon le complément de `device_ahead`
  survit sous un `up_to_date` ultérieur, ou sous un échec réseau.
- ⚠️ `#check_update_status` est **masqué dans le `finally`** ⇒ ⛔ inutilisable pour un message
  persistant.
- `#update_sig_note` (bug-176) ne parle que si `update_available` est vrai ⇒ **les deux lignes ne
  peuvent jamais coexister**. Rien à arbitrer.
- Firmware ancien sans `update_state` ⇒ **repli sur `update_available`**.

### 🔗 Conflit avec `feature-169`, à inscrire LÀ-BAS
Le renvoi « **ci-dessous** » doit **rester vrai**. ⚠️ Si `feature-169` replie « Mise à jour
manuelle » dans un `<details>`, le **titre du résumé doit rester visible** et conserver son **nom
exact**, sinon le renvoi casse. Idem si la carte « Maintenance » unifiée la déplace ou la renomme :
les libellés de ce lot sont à reprendre **dans le même geste**.

---

## 🧪 TESTS, REVUES ET CORRECTIONS (2026-11-15)

### Tests
Suite **existante** `test/test_native_ota_check/` étendue de **15 cas** (⛔ aucun env PlatformIO neuf).
**81 suites, 2 433 cas, 0 échec** (mesuré). `pio run` après `clean` : **SUCCESS**, Flash **51,3 %**,
RAM **29,8 %**, **2 warnings, tous deux OneWire, préexistants** — « zéro nouveau warning » **vérifié**,
non supposé. `build_fs.sh` : **213 158 o, 40,7 %** de la partition.

⭐ **L'invariant central est prouvé par BALAYAGE, pas par exemples** : 17×17 combinaisons de
versions lisibles et illisibles, confrontées à un **oracle indépendant en entiers 64 bits**.

### Mutations — 8 sur 8 tuées
| # | Mutation | Résultat |
|---|---|---|
| 1 | `strcmp` lexicographique | ⚠️ tué sur `2.10.0` / `2.9.0` |
| 2 | sens inversé | tué sur le cas de terrain |
| 3 | `kUnknownVersion` ⇒ « mise à jour » | tué |
| 4 | défaut à 0 des composants manquants | tué (⚠️ **approximation déclarée** : le défaut est implicite, mutation d'un effet équivalent) |
| 5 | garde `kOtaVersionMaxDigits` supprimée | tué |
| 6 | comparaison limitée à majeur.mineur | tué sur `2.87.0` / `2.87.1` |
| 7 | retrait du `v` supprimé | tué |
| 8 | raccourci « chaînes identiques » | tué sur `"abc"` / `"abc"` |

### ✅ Le point qui justifie le lot, vérifié
⛔ **Aucune comparaison de versions n'a été recréée dans `data/app.js`** : la page ne lit que
`update_state`. ⚠️ En JavaScript, `"2.10.0" < "2.9.0"` est **vrai** — refaire la comparaison côté
client aurait **déplacé** le défaut au lieu de le corriger, et personne ne s'en serait aperçu avant
la version 2.10.

### 👀 Revue de code — « le code est bon », deux corrections appliquées
Verdict initial : *Request changes* — ⚠️ **uniquement** pour la documentation manquante (depuis
écrite). Aucune réserve technique sur `src/` ni `data/`.

🔴 **Correction 1 — le test affirmait une propriété que le système n'avait pas.**
La route retirait un `v` **avant** `otaUpdateStateFor`, qui en retire un second ⇒ en production
`vv2.0.0` aurait été **lisible**, alors que `test_version_double_v_prefix_unreadable` affirmait le
contraire. ⚠️ **Le danger n'est pas le tag malformé, c'est le test qui ment** : une suite verte qui
atteste une propriété absente décourage de vérifier.
⇒ La route passe désormais le **tag BRUT** (`releaseTag`) au comparateur ; son propre retrait du `v`
est marqué **cosmétique** (affichage et journal seulement). ⭐ **Une seule autorité sur la règle du
préfixe** — au lieu de deux expressions pour une même règle, motif déjà payé par ce projet.

### 🔒 Revue de sécurité — GO SOUS CONDITIONS, condition levée
🔴 **La réserve d'`innerHTML` portée depuis `feature-151` n'était pas levée : elle était DÉPLACÉE.**
`#no_update_msg` est bien passé en construction DOM — mais `tag_name` (chaîne **GitHub**, non
maîtrisée) partait dans le **journal**, rendu par `innerHTML` côté page sous une CSP autorisant
l'inline. ⇒ Un tag porteur de balisage se serait exécuté **dans la session admin** à l'ouverture
du journal.

🔴 **Correction 2** : la version n'est journalisée **que si l'analyseur l'a acceptée** — auquel cas
elle ne contient que des chiffres et des points, inertes en HTML. Sinon, libellé **fixe**.
⚠️ **Mon premier libellé était `<non analysable>`** — avec des chevrons, donc du balisage dans la
ligne même que j'assainissais. Corrigé en `(non analysable)`. ⇒ **Un assainissement ne vaut que si
la valeur de remplacement est elle-même inerte.**

### Points écartés, avec leur raison
- `kOtaVersionMaxParts/MaxDigits` en `static const` plutôt que `constexpr` : ⚠️ `constexpr` serait
  plus conforme, mais le header est consommé tel quel par les suites natives. **Non traité**, sans
  conséquence.
- Retour silencieux de la garde d'installation (`app.js`), sans toast : c'est une garde de
  **défense en profondeur** sur un chemin déjà fermé en amont ; un toast y serait du bruit.

### Autres constats de la revue de sécurité — PRÉEXISTANTS, hors de ce lot
⇒ Ouverts séparément : `bug-179` (journal rendu par `innerHTML` + CSP `'unsafe-inline'`),
`bug-180` (liste blanche OTA acceptant **tout** dépôt `github.com`). Le **plancher de version figé
à `0.0.0`** est noté dans `feature-151`, dont le lot D3-c le concerne.

### ⛔ RESTE OUVERT — D3, essai de terrain
Le rendu des quatre états **n'a jamais été vu sur l'appareil**. ⚠️ `ui_shot.sh` ne peut pas les
photographier (il force `/data` et `/get-config`, pas `/check-update`). ⇒ Vérifiable **gratuitement
après déploiement** : l'appareil en **2.88.1** face à la release **2.88.0** produira exactement
`device_ahead` — la situation qui a motivé cette spec.

### 🔴 LIBELLÉ CORRIGÉ PAR L'UTILISATEUR (2026-11-15, après observation sur l'appareil)
Vu en vrai sur son téléphone, l'utilisateur a tranché :
> *« Ce serait plus parlant de dire "La version installée est plus récente. Aucune mise à jour…" »*

⇒ `ℹ Installée : plus récente que la release` (41) devient
**`ℹ La version installée est plus récente`** (39).

**Pourquoi c'est mieux** : l'ancien libellé était **télégraphique** — une étiquette, pas une phrase.
Le nouveau **s'enchaîne** avec le complément qui existe déjà (« Aucune mise à jour proposée… ») et
se lit d'une traite, exactement comme l'utilisateur l'a formulé.

⚠️ **Réserve assumée** : l'étude d'ergonomie avait rejeté ma version courte (« Version installée
plus récente ») comme ambiguë — *plus récente que quoi ?* — et imposé de **nommer le terme de
comparaison**. Le libellé retenu l'omet lui aussi. ⇒ Accepté parce que la carte affiche
**« Dernière version »** juste au-dessus : le terme de comparaison est **à l'écran**, pas dans la
phrase. ⭐ Arbitrage rendu par l'utilisateur **sur le rendu réel**, pas sur une maquette — c'est la
seule façon de trancher ce genre de question.

⛔ **Non embarqué dans la release v2.88.2** (le libellé y est encore l'ancien) : ce changement
partira avec le prochain lot. Rien ne justifie une release pour un libellé.

---

## ✅ CLÔTURE — essai de terrain (2026-11-15, soir)

Firmware local **2.88.3** déployé (non publié), page **2.88.2** conservée, release **2.88.2**.
⇒ `current_version 2.88.3 · latest_version 2.88.2 · update_available **False** ·
update_state **device_ahead**`, et le message affiché sur le téléphone de l'utilisateur.

⭐ **C'est exactement la situation qui, le matin même, proposait d'installer la 2.87.1 par-dessus la
2.88.0** — c'est-à-dire d'effacer un correctif en croyant mettre à jour.

**Libellé corrigé par l'utilisateur sur le rendu réel** (voir section précédente) :
`ℹ La version installée est plus récente` (39).

### ⭐ Le retour arrière, démontré en acte
L'appareil a été ramené de **2.88.3 à 2.88.2** par envoi manuel signé, pendant cette même séance.
⇒ Le lot garantit que le retour arrière n'est **plus PROPOSÉ** ; il reste **parfaitement possible**.
C'était la question de l'utilisateur, et la réponse est une démonstration, pas une affirmation.

**Statut : done.**
