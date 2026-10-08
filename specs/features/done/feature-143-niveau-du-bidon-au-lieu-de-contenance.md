# feature-143 — Le suivi de produit parle de NIVEAU, pas de contenance

## Métadonnées
- **ID** : feature-143
- **Slug** : niveau-du-bidon-au-lieu-de-contenance
- **Type** : feature — **correction d'un modèle mental faux**, pas un ajout de fonction
- **Statut** : livré — tests verts, revue OK (doc-writer, 2026-10-08)
- **Priorité** : normale — aucun risque chimique, mais le défaut a **réellement** produit un état incohérent en production
- **Version cible** : 2.59.0
- **Dernière mise à jour** : 2026-10-08
- **Spec(s) liée(s)** : `feature-110` (compteurs V et N, remise à zéro simultanée) · `specs/features/done/feature-078-…` (campagnes de concordance)

## Domaines touchés
- [ ] Capteurs (pH / ORP / température)
- [ ] Régulation (pump_controller, PID)
- [ ] Filtration / Éclairage
- [x] **Web — routes HTTP** (`/save-config`, champs `*_container_ml` / `*_reset_container`)
- [ ] MQTT / Home Assistant
- [ ] UART / écran LVGL
- [ ] OTA
- [ ] Authentification
- [ ] Historique / persistance
- [x] **Frontend (`data/`)** — c'est le cœur du lot

## Agents requis
- [x] spec-reader
- [x] architect
- [ ] pool-chemistry — **non requis** : aucun chemin de dosage touché, aucune limite modifiée
- [x] **ux-ui-designer — OBLIGATOIRE** : c'est une refonte de vocabulaire et d'interaction
- [ ] embedded-developer
- [x] web-ui-developer
- [x] test-engineer
- [x] code-reviewer
- [x] doc-writer

## Description

Le champ **« Volume du bidon (L) »** annonce une **contenance** — la taille du récipient. Le
logiciel, lui, s'en sert comme d'un **niveau de départ** : la quantité présente au moment où les
compteurs sont remis à zéro. Deux notions différentes, un seul champ, et c'est le mauvais nom
qui s'affiche.

Conséquence : le geste le plus courant — **faire un appoint sans changer de bidon** — n'existe
nulle part, ni dans le vocabulaire ni dans les actions.

## Contexte — l'incident réel du 2026-10-08

L'utilisateur avait **2,85 L** restants mesurés au doseur dans son jerrican de 20 L. Il y a versé
**2,15 L**, soit **5 L** au total. **Il n'a pas changé de bidon.**

Il a saisi `5` dans « Volume du bidon » puis cliqué **« Enregistrer »** — le geste que le libellé
appelle. Résultat relevé sur l'appareil :

```
orp_container_ml       = 5000       ← sa saisie
orp_total_injected_ml  = 18221,21   ← PAS remis à zéro
orp_remaining_ml       = 0          ← écrêté
```

**L'appareil s'est cru à sec alors que le bidon venait d'être rempli**, sous le seuil d'alerte de
2 000 mL, avec l'escalade que cela entraîne — le projet traite ce cas comme « un bidon vide, un
tube désamorcé, une fuite ou une sonde » (`constants.h`).

Ses mots, qui énoncent le défaut mieux que n'importe quelle analyse :

> *« Mais mon bidon fait toujours 20 l. C'est la contenance qui a bougé. J'avais 2,85 l, j'ai
> ajouté 2,15 l et je n'ai pas changé de bidon. »*

Et, sur la solution qu'on lui proposait :

> *« je n'ai rien compris. […] si je ne comprends pas, je ne vois pas comment un utilisateur
> comprendra. »*

**Il a raison, et le logiciel a tort.** Son jerrican fait bien 20 L et ne changera jamais. Lui
demander d'y écrire `5` contredit frontalement le libellé du champ.

## Le défaut, en une phrase

Pour le logiciel, « nouveau bidon » et « appoint » sont **exactement la même opération** —
*il y a maintenant N litres dedans, recompte à partir de zéro*. La distinction qu'il impose à
l'utilisateur **n'a aucune réalité de son côté**.

## Comportement actuel

- `src/web_routes_config.cpp:1183-1200` : `orp_container_ml` écrit la contenance ;
  `orp_reset_container` remet à zéro **quatre compteurs d'un coup** (volume injecté, démarrages,
  et les deux compteurs d'anomalie) — invariant de `feature-110`, à **préserver absolument**.
- `data/index.html:1627` : libellé « Volume du bidon (L) ».
- `data/index.html:1637-1638` : deux boutons, **« Enregistrer »** et **« Nouveau bidon »**.
- `data/app.js:3511-3517` : « Nouveau bidon » envoie **déjà** contenance **et** remise à zéro
  dans la même requête — l'action correcte existe, elle est juste mal nommée et mal découverte.
- La contenance sert de **pleine échelle à la jauge** (`data/app.js:3433-3436`) et de référence
  au restant (`ws_manager.cpp:991-996`, `web_routes_data.cpp:670-673`).

## Comportement attendu

**Un seul geste, nommé par ce qu'il fait :**

> **Mettre à jour le niveau** — *« Il y a maintenant ____ litres dans le bidon. »*

Il pose la quantité **et** remet les compteurs à zéro. Il couvre les trois cas sans que
l'utilisateur ait à choisir : bidon neuf · appoint · simple correction après mesure au doseur.

**Le champ « Volume du bidon » disparaît des réglages.** Il n'y a plus de contenance à saisir,
puisque le logiciel ne s'en sert jamais comme telle. La valeur continue d'exister en
configuration — elle est posée par l'action ci-dessus, pas tapée comme un réglage autonome.

**« Enregistrer » ne garde que de vrais réglages** : le seuil d'alerte et le produit déclaré.

**La jauge reste juste** : elle mesure la progression dans le **remplissage courant** (4 988 sur
5 000), pas dans la capacité du jerrican — ce qui est précisément ce que l'utilisateur veut voir.

### Cas limites
- **Niveau saisi à 0** : refus, ou confirmation explicite ? Un bidon à 0 L n'a pas de sens comme
  point de départ.
- **Action annulée** dans la boîte de dialogue : rien ne doit être écrit, ni niveau ni compteurs.
  Le comportement actuel (`app.js:3613`, `if (ok !== true) return;`) est correct — **à préserver**.
- **Appareil hors ligne** au moment de l'action : message d'erreur explicite, aucun état partiel.
- **Suivi produit désactivé** (`*_tracking_enabled`) : l'action doit-elle rester offerte ?

## Contraintes techniques
- ⚠️ **L'invariant de `feature-110` est INTOUCHABLE** : volume injecté, démarrages et **les deux
  compteurs d'anomalie** repartent de zéro **dans le même bloc**. Le commentaire de
  `web_routes_config.cpp:1172-1177` en donne le motif : *« le rapport V/N n'a de sens que si les
  deux grandeurs couvrent la même campagne […] sinon l'indicateur qui doit qualifier la campagne
  à venir survivrait à celle qu'il vient de qualifier — il disqualifierait un relevé propre. »*
- ⚠️ **Le relevé AVANT remise à zéro doit rester possible et visible.** `app.js:3595-3605`
  recharge la configuration **avant** d'afficher le récapitulatif, précisément parce que certains
  compteurs sont HTTP-seuls et seraient *« incomplets à l'instant même où ils sont détruits »*.
  Ce récapitulatif est la seule chance de noter V, N et les anomalies avant qu'ils disparaissent —
  **le conserver, et plutôt le renforcer**.
- L'action reste **irréversible** et confirmée explicitement (`danger: true`).
- Contrats publics : `*_container_ml` et `*_remaining_ml` sont publiés en `/data`, WebSocket et
  MQTT. **Ne pas renommer les champs** — c'est le libellé d'interface qui change, pas l'API.
- Parité `/data` ↔ WebSocket à préserver.
- Vanilla JS, français, pas de framework.

## Fichiers potentiellement concernés
- `data/index.html` — libellés et boutons des deux cartes (pH et chlore)
- `data/app.js` — `saveProduct()`, `resetProduct()`, `_buildProductResetSummary()`, `updateCard()`
- `src/web_routes_config.cpp` — **a priori inchangé** : l'API accepte déjà les deux champs
  ensemble ; à confirmer par l'architecte
- `docs/features/page-dosages.md`, `docs/user/src/guide-utilisation.html`

## Livrables — check-list
- [x] B1 — Action unique **« Mettre à jour le niveau »**, formulée *« Il y a maintenant ___ litres
      dans le bidon »*, posant le niveau ET remettant les compteurs à zéro
- [x] B2 — Champ « Volume du bidon » **retiré** des réglages ; la saisie du niveau vit dans
      l'action
- [x] B3 — « Enregistrer » ne porte plus que le **seuil d'alerte** et le **produit déclaré**
- [x] B4 — Récapitulatif avant remise à zéro **conservé et renforcé** (V, N, anomalies —
      la dernière chance de les noter)
- [x] B5 — Les deux cartes (pH **et** chlore) traitées à l'identique
- [x] B6 — Cas limites tranchés : niveau 0, annulation, hors ligne, suivi désactivé
- [x] D1 — Tests natifs de la logique pure extraite s'il y en a une ; sinon, **le dire**
- [x] D2 — Vérifier qu'aucune combinaison d'interface ne peut produire
      **niveau < volume déjà injecté** — l'état incohérent du 08/10 doit devenir
      **impossible par construction**, pas détecté après coup
- [x] C1 — `docs/features/page-dosages.md`
- [x] C2 — `docs/user/src/guide-utilisation.html` (source HTML seule, pas de build PDF)
- [x] C3 — `CHANGELOG.md`
- [x] C4 — Signaler **LA capture d'écran** à refaire (`screenshots/`)

## Sécurité chimique
- Dosage impliqué : **non**. Aucune limite, aucun plafond, aucune garde touchés.
- ⚠️ Effet **indirect** à ne pas perdre de vue : un suivi de produit faux déclenche des alertes
  « bidon vide » injustifiées, et le projet traite ce cas comme un symptôme sérieux. Rendre le
  suivi fiable **sert** la sécurité, même sans toucher au dosage.

## ADR attendu ?
- **Probable** : non
- **Critère(s) candidat(s)** : aucun — correction d'un libellé et d'un flux d'interaction, pas
  d'arbitrage entre options structurantes.

## Éléments visibles (pour doc-writer)
### Endpoints HTTP concernés
- `/save-config` — **aucun changement de contrat**, seuls les appels du frontend changent.
### Champs WebSocket
- Aucun nouveau. `*_container_ml` et `*_remaining_ml` **conservent leur nom** (API publique).
### Topics MQTT
- Aucun.
### Documentation à produire / mettre à jour
- `docs/features/page-dosages.md` · `docs/user/src/guide-utilisation.html` · `CHANGELOG.md`

## Critères d'acceptation
- [x] Un appoint **sans changement de bidon** se saisit en un geste, sans contradiction de libellé
- [x] L'état « niveau < volume déjà injecté » est **impossible à créer** depuis l'interface
- [x] Le récapitulatif avant remise à zéro affiche V, N et les anomalies
- [x] L'invariant `feature-110` tient : les quatre compteurs repartent **ensemble**
- [x] Les deux cartes se comportent à l'identique
- [x] Libellés en français, compréhensibles **sans connaître le fonctionnement interne**
- [x] `./build_fs.sh` passe sans dépasser la partition

## Plan de test manuel
1. Rejouer **le scénario exact du 08/10** : 2,85 L mesurés, 2,15 L ajoutés, saisir 5 L.
   Vérifier que le restant affiché est bien ~5 L et que les compteurs sont repartis de zéro.
2. Annuler la boîte de dialogue : **rien** ne doit changer.
3. Tenter de produire l'état incohérent par tous les chemins offerts : il doit être inatteignable.
4. Vérifier le récapitulatif avant remise à zéro : V, N et anomalies lisibles et notables.

## Notes pour agent

⚠️ **Le défaut n'est pas un manque d'avertissement, c'est un modèle mental faux.** La tentation
sera d'ajouter un garde-fou (« attention, cette valeur est inférieure au volume déjà injecté »).
Ce serait soigner le symptôme : l'utilisateur a saisi la **bonne valeur selon le libellé qu'on
lui affichait**. C'est le libellé qu'il faut corriger, pas l'utilisateur qu'il faut rattraper.

⚠️ **L'action correcte existe déjà** (`app.js:3511-3517`, « Nouveau bidon » envoie contenance +
remise à zéro ensemble). Ce lot ne crée presque aucune mécanique : il **renomme**, **retire un
champ trompeur** et **supprime un choix qui n'a pas de sens côté logiciel**. Résister à l'envie
d'en faire davantage.

⚠️ **Ne pas renommer les champs d'API.** `*_container_ml` reste `*_container_ml` en `/data`, WS
et MQTT. Seule l'interface change de vocabulaire.

## Hors périmètre
- Le **calcul** du restant et la jauge elle-même (justes, et inchangés).
- Le biais de comptage de ~6 % : instruit et **clos** dans `feature-078`.
- Toute modification du dosage, des limites ou des seuils d'alerte.

## Notes d'implémentation (doc-writer, 2026-10-08, v2.59.0)

**Mécanisme retenu (`ux-ui-designer` + `web-ui-developer`)** : fusion stricte plutôt
qu'ajout — `data/app.js` renomme `resetProduct()` en `updateLevel()` et retire tout
usage du champ `*_container_l` par `saveProduct()`. Un seul chemin d'écriture du
niveau (`POST /save-config` avec `*_container_ml` **et** `*_reset_container: true`
dans la même requête) rend l'invariant D2 (« niveau < volume déjà injecté
impossible ») vrai **par construction** : il n'existe tout simplement plus de
chemin qui pose le niveau sans réinitialiser le cumul injecté en même temps.

**Champ jamais pré-rempli** : `loadProductConfig()` ne touche plus aux champs
`*_level_l` (contrairement à l'ancien `*_container_l`, pré-rempli depuis
`cfg.*_container_ml`). Décision explicite : pré-remplir aurait reconduit le
malentendu de l'incident (« il y a déjà un nombre, il doit être bon »).

**Cas limites tranchés** :
- Niveau ≤ 0 → refus côté client (`isNaN(levelL) || levelL <= 0`), toast d'erreur,
  aucune requête envoyée.
- Annulation de la confirmation (`ok !== true`) → comportement préexistant
  (feature-110) conservé tel quel, rien n'est écrit.
- Appareil hors ligne au moment du `GET /get-config` préalable → toast d'erreur,
  aucune remise à zéro (le code existant gérait déjà ce cas, repris sans
  modification de fond).
- Suivi désactivé (`*_tracking_enabled = false`) → tout le corps de la carte,
  action de mise à jour du niveau comprise, est masqué par
  `applyProductTracking()` : l'action n'est simplement pas offerte quand le
  suivi est coupé. Pas de nouveau code : réutilisation du masquage existant.

**Retrait de l'exception de parité `/data` ↔ WebSocket (fait en premier dans ce
lot)** : `ph_container_ml` / `orp_container_ml` étaient déclarées dans
`kWsPlusOtherRoute` (`test/test_native_ws_data_parity/`) comme exception
dual-sourcée légitime (« RÉGLAGE, servi par `/get-config` »). Ce raisonnement
ignorait que la carte Produits se rafraîchit à **chaque** charge `/data` (30 s),
pas seulement au chargement de la configuration — 4ᵉ récidive du motif de parité
sur ce projet. Correction : `src/web_routes_data.cpp` sert désormais ces deux
clés directement, l'exception a été **retirée** (pas contournée) de la sentinelle,
qui passe au vert via la vérification générique (a)-(d). `kWsPlusOtherRoute` passe
de 8 à 6 entrées, le total de clés WS-seules de 24 à 22.

**Garde contre les chiffres inventés** : `updateCard()` dans `data/app.js`
n'utilise plus de repli `?? 20000` — `*_container_ml` absent produit `--` et une
barre à 0 %, jamais une valeur par défaut masquant un défaut de parité.

**`src/web_routes_config.cpp` confirmé inchangé** pour ce lot (vérifié par
`git diff` — seule modification de ce fichier dans l'arbre de travail : le
correctif bug-081 sur `revokeAllAuthenticated()`, sans rapport avec feature-143).
Le contrat `/save-config` existait déjà depuis feature-110 ; seul l'appel
frontend change.

**Vérification `./build_fs.sh`** : rejoué le 2026-10-08, LittleFS construit à
512 Ko exactement, dans le gabarit de la partition `spiffs` (ADR-0068).

**ADR** : aucun créé. Aucun des 4 critères de déclenchement n'est rempli — c'est
un renommage de libellé et une fusion d'un flux d'interaction déjà existant
(`resetProduct()` envoyait déjà les deux champs ensemble), pas un arbitrage entre
alternatives structurantes.

### Correction de l'agent principal après revue

Le `code-reviewer` a rendu **Approved**, avec un point mineur que j'ai traité : les nouveaux
champs `#ph_level_l` / `#orp_level_l` avaient **perdu la borne haute** `max="1000"` que portait
l'ancien champ. Une saisie à 5 000 L serait passée sans broncher. Restaurée sur les deux cartes,
`./build_fs.sh` rejoué.

### Deux points tranchés en revue

- **B6 — suivi produit désactivé : non-sujet.** `applyProductTracking` masque
  `#product-{prefix}-body`, et le nouveau bloc (champ + bouton) est **à l'intérieur** de ce body.
  L'action est donc inatteignable. Le développeur avait un doute, l'`ux-ui-designer` avait raison.
- **Les six autres exceptions de la sentinelle de parité ont été auditées** — c'était une demande
  explicite, puisque l'enseignement du lot est que *ce sont les exceptions qui méritent la
  vigilance, pas la règle*. Aucune ne souffre du même vice : toutes leurs lectures sont gardées
  ou repliées sur `window._config`, aucune ne peut être écrasée par une valeur muette. La
  contenance était bien le seul cas.

### L'enseignement, à ne pas perdre

Le mécanisme de détection **existait** : la sentinelle `test_native_ws_data_parity` vérifie que
chaque clé du WebSocket est servie par `/data`. `ph_container_ml` et `orp_container_ml` y
figuraient comme **exception déclarée**, au motif :

> *« Volume du bidon pH : RÉGLAGE, servi par /get-config vers `window._config` ; la trame le
> pousse pour la jauge de niveau qui, elle, bouge à l'usage. »*

Le raisonnement était **juste sur le papier et faux en pratique** : l'interface rafraîchit la
carte à **chaque charge `/data`** (`app.js:7322`), pas seulement au chargement de la
configuration. La clé était donc bien attendue dans `/data`.

Ce n'est donc **pas** un mécanisme qui manquait — c'est une exception, écrite de bonne foi avec
un motif plausible, qui a cessé d'être vraie sans que rien ne le signale. Le défaut est resté
invisible des mois parce que trois replis muets vers 20 L **tombaient juste par hasard** tant que
le bidon faisait effectivement 20 L.

### Résultats mesurés
- `pio run -e esp32dev_core3` : SUCCESS, aucun warning neuf (50,2 % flash)
- `pio test -e native` : **2 082 cas / 2 082**, dont la sentinelle de parité (16 cas) qui
  **prouve le retrait** via son test « aucune exception morte ni contredite »
- `./build_fs.sh` : SUCCESS, partition `spiffs` (512 Ko) non dépassée
- `code-reviewer` : **Approved**

### ⚠️ Reste à faire — étape humaine
**`screenshots/09_Produits.jpg`** est à refaire : la page Produits a changé visuellement.
À faire avant la prochaine génération de PDF.
