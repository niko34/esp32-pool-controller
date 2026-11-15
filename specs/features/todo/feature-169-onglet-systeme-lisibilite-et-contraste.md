## Métadonnées
- **ID** : feature-169
- **Slug** : onglet-systeme-lisibilite-et-contraste
- **Type** : feature (dette d'interface — ⚠️ un point d'**accessibilité** dedans)
- **Statut** : todo
- **Priorité** : normale — ⚠️ **sauf B1 (contraste), qui est de l'accessibilité et vaut haute**
- **Version cible** : à définir
- **Dernière mise à jour** : 2026-11-14
- **Spec(s) liée(s)** : `doing/feature-151-…` (c'est en la testant que l'onglet a été regardé
  **rendu** pour la première fois) · `done/feature-143-…` (page Produits — **même travers**) ·
  mémoire « Chantier de simplification de l'interface » (direction du 2026-10-09)

## Domaines touchés
- [ ] Capteurs · [ ] Régulation · [ ] Filtration / Éclairage
- [ ] Web — routes HTTP / WebSocket · [ ] MQTT · [ ] UART · [ ] OTA · [ ] Authentification
- [ ] Historique / persistance
- [x] **Frontend (`data/`)**

## Agents requis
- [x] spec-reader
- [x] architect
- [ ] pool-chemistry — aucun chemin de dosage
- [x] **ux-ui-designer — sur l'ÉCRAN ENTIER**, ⛔ pas carte par carte. ⚠️ **B1 touche un
      composant PARTAGÉ** (`.btn:disabled`) : son mandat impose alors les trois obligations
      ensemble — inventaire **exhaustif** des emplois en comptes réels, cohérence de
      l'ensemble, et **tous** les impacts réalisés dans le MÊME lot.
      ⛔ L'application partielle est interdite.
- [ ] embedded-developer
- [x] web-ui-developer
- [x] test-engineer
- [x] code-reviewer
- [x] doc-writer

## Description
L'onglet **Système** des Réglages a accumulé cinq cartes sans que personne ne les compare
**rendues**. Cette spec corrige ce qui gêne vraiment : le **contraste des boutons désactivés**
(accessibilité, composant partagé), la **hauteur** de la carte « Infos système », et deux
débordements à 390 px.

## Contexte

⭐ **Origine : la campagne de preuve terrain de feature-151 (2026-11-14).** En photographiant
l'onglet pour une question de libellés, on a regardé l'écran **entier** — et on y a trouvé plus
que ce qu'on cherchait.

⚠️ **C'est exactement le motif de la page Produits** (2026-10-09) : aucun ajout n'était mauvais,
c'est leur **somme** que personne ne regardait. La règle d'accumulation de `CLAUDE.md` est née
de là. ⇒ Cette spec est le constat qu'elle s'applique aussi ici.

⛔ **Deux points ont été ÉCARTÉS après mesure** — ne pas les réinstruire :
1. **Aligner la typographie des libellés** (`IMAGE (.BIN)` en capitales face à `Firmware` en
   casse normale). **Mesuré** : `.field__label` = **59** emplois (un champ qu'on **remplit**),
   `.kv__k` = **27** (une valeur qu'on **lit**), et les deux ne cohabitent que dans **1 carte
   sur 28**. ⇒ La distinction est **systématique**, pas accidentelle. Les aligner toucherait
   59 emplois pour effacer une différence qui a du sens.
2. **Les libellés de type `(.bin)`** — déjà corrigés par feature-151.

## Comportement attendu

### B1 — 🔴 Contraste des boutons désactivés (composant PARTAGÉ)
`data/app.css` : `.btn:disabled { opacity: 0.4 }`. Les 40 % s'appliquent au **texte ET au
fond** ⇒ sur un bouton déjà pâle, le libellé devient illisible. Constaté sur « Vérifier »,
« Mettre à jour », « Redémarrer », « Réinitialiser ».
⚠️ **Et le libellé est la SEULE indication de ce que fait le bouton.** WCAG n'impose rien aux
contrôles désactivés, ⛔ mais un bouton dont on ne peut pas lire le nom n'informe plus.
⇒ Découpler l'opacité du texte de celle du fond, ou poser une couleur de texte dédiée.
⇒ ⛔ **Inventaire exhaustif de TOUS les boutons désactivés de `data/`**, et tous traités dans
le même lot.

**Et dire POURQUOI il est désactivé.** Dans l'état vide, « Mettre à jour » est grisé **sans
explication**. Un texte d'aide (« Choisissez un fichier ») lève l'ambiguïté.

### B2 — La carte « Infos système » est la plus haute de l'onglet
**11 lignes**, ~60 px chacune, ≈ **660 px** de diagnostic avant d'atteindre quoi que ce soit
d'actionnable.
- **Disposition** : libellé **au-dessus** de la valeur. Un libellé à gauche / valeur à droite
  diviserait la hauteur par ~2 — les textes sont courts.
- **Ce qui a servi à concevoir et n'a plus à rester affiché** : `Build`, `Chip`, `CPU`, `Flash`,
  `MAC`. ⛔ **Repliés (`<details>`), pas supprimés** : le support en a besoin.
  ⚠️ `Build` est en plus **redondant** avec la version du firmware.
- **À garder visible** : version, uptime, mémoire utilisable, Wi-Fi RSSI, occupation FS et flash.
- ⚠️ **La phrase sous « Mémoire utilisable »** — *« Mesure brute : 78 KB, dont une zone technique
  inutilisable pour les allocations courantes »* — est du **texte de conception** (cause racine
  de la métrique heap faussée, 15/08/2026). ⇒ En infobulle, ⛔ pas en permanence à l'écran.

### B3 — L'encadré de réinitialisation domine la page
**9 lignes en gros corps (~16 px)**, alors que l'aide de « Redémarrage » est en petit.
⭐ **La différence de GRAVITÉ justifie une différence de TON, ⛔ pas de structure** : Redémarrage
est réversible et sans perte (texte simple), Réinitialisation est destructive (encadré ambre
justifié). C'est la **taille** qui ne l'est pas.
⇒ Raccourcir d'environ 40 %, proposition :
> **Action irréversible.** Toute la configuration, les calibrations, les identifiants Wi-Fi et
> l'historique des produits seront supprimés. Après redémarrage : réseau Wi-Fi
> **PoolControllerAP**, puis **http://192.168.4.1**.

⚠️ **À vérifier au passage** : le bouton de réinitialisation a-t-il une étape de confirmation ?
⛔ Non constaté sur capture.

### B4 — « Mise à jour en ligne » : deux boutons de même poids
« Vérifier » et « Mettre à jour » sont empilés à poids égal, alors que **le second dépend du
premier**. ⇒ Masquer « Mettre à jour » tant qu'aucune version n'a été trouvée : la carte perd
la moitié de sa hauteur au repos.

### B5 — Débordements à 390 px
- **En-tête** : « Déconnecté — Modifications non sauvegardées » **touche le bouton menu**.
- 🔴 ⚠️ **ET C'EST PEUT-ÊTRE UN BUG, PAS UNE MISE EN PAGE** : ce message s'affiche sur un onglet
  où **rien n'a été modifié**. ⛔ **Diagnostiquer AVANT de toucher à la forme** — raccourcir un
  texte qui n'aurait pas dû apparaître masquerait le vrai défaut.
  *(« Déconnecté » est, lui, un artefact de `ui_shot.sh`, qui n'ouvre pas de WebSocket.)*
- **Barre d'onglets** : 8 onglets sur **4 lignes**, soit **215 px** avant tout contenu, avec
  « À propos » seul et centré. ⇒ Piste : défilement horizontal ou regroupement. ⛔ C'est un
  choix de **navigation globale** — hors de cette spec si le lot devient trop gros.

### Cas limites
- ⚠️ Le contraste doit tenir en **mode sombre** comme en clair.
- ⚠️ Le repli de B2 doit être **refermé par défaut** et son état **non persisté** (c'est du
  diagnostic ponctuel, pas une préférence).
- ⛔ Aucun champ d'« Infos système » ne doit **disparaître** : tout reste atteignable.

## Comportement actuel
L'onglet affiche cinq cartes à la suite. Tout ce qui est décrit ci-dessus est l'état livré en
**2.86.0**, constaté sur capture à **390 px** (`./tools/ui_shot.sh -r '#/settings/system'`).

## Contraintes techniques
- ⚠️ **RESPONSIVE** : conçu **à 390 px d'abord**. Compter les caractères des libellés placés sur
  une ligne partagée. ⛔ Vérifier, ne pas affirmer.
- ⚠️ **`ui_shot.sh` ne sait pas photographier un `<input type="file">` rempli.** Les états « avec
  fichier choisi » restent **non photographiables** ⇒ à juger à l'œil sur l'appareil.
- ⚠️ `window.innerWidth` est plafonné à 500 px dans le conteneur ; ⛔ la **mise en page**, elle,
  suit bien `-w` (prouvé par sonde). Voir l'en-tête de `tools/ui_shot.sh`.
- Vanilla JS, français dans toute l'UI, offline first.
- Budget FS : vérifier avec `./build_fs.sh`.

## Fichiers potentiellement concernés
- `data/app.css` — `.btn:disabled` (**partagé**), `.kv__*`, `.callout`
- `data/index.html` — carte « Infos système », encadré de réinitialisation, en-tête
- `data/app.js` — repli de B2, masquage conditionnel de B4, texte d'aide de B1
- `docs/DESIGN_SYSTEM.md` — état `:disabled` des `.btn` avec son **contraste cible**
- `docs/features/page-settings.md` — l'onglet change d'aspect

## Livrables — check-list
- [ ] B1 — contraste des boutons désactivés + inventaire exhaustif + texte d'aide « pourquoi »
- [ ] B2 — « Infos système » : disposition compacte, repli du diagnostic, phrase en infobulle
- [ ] B3 — encadré de réinitialisation raccourci ; vérifier l'étape de confirmation
- [ ] B4 — « Mettre à jour » masqué tant qu'aucune version n'est trouvée
- [ ] B5a — 🔴 **DIAGNOSTIQUER** « Modifications non sauvegardées » affiché à tort (bug ?)
- [ ] B5b — débordement de l'en-tête à 390 px, **après** B5a
- [ ] C1 — `docs/DESIGN_SYSTEM.md` : `.btn:disabled` et son contraste cible
- [ ] C2 — `docs/features/page-settings.md`
- [ ] D1 — captures **avant / après** à 390 px, jointes à la clôture

## Ordre conseillé
**B1 d'abord** (accessibilité, composant partagé), puis **B2** (le gain de hauteur le plus net),
puis B5a → B5b, B3, B4.

## Hors périmètre
- La barre d'onglets (choix de navigation globale) — spec séparée si l'envie vient.
- La typographie des libellés de champ — **écartée après mesure**, voir « Contexte ».
- Tout ce qui touche la signature OTA — c'est feature-151.

---

## 📥 APPORT DE L'ÉTUDE ERGONOMIE DU 2026-11-15 (versé depuis `bug-176`)

L'étude `ux-ui-designer` menée pour `bug-176`, **sur captures de l'écran réel** (390 px et
1024 px, route `#/settings/system`), a porté sur la **page entière** au titre de la règle
d'accumulation. Ses propositions de désencombrement **n'avaient aucun rapport avec `bug-176`** et
sont versées ici, où elles ont leur place.

⚠️ **Constat de départ** : l'écran mesure **2 600 px de haut à 390 px de large** et porte cinq
cartes (Infos système, Mise à jour en ligne, Mise à jour manuelle, Redémarrage, Réinitialisation).

### Propositions (≈ 700 px gagnés)
1. **Infos système — 12 lignes → 9** (~210 px) : fusionner `Build` dans `Firmware`
   (« 2.87.1 · 15 nov. 2026 »), `CPU` dans `Chip` (« ESP32-D0WD-V3 · 240 MHz »), et **supprimer
   `Flash`** (8 MB est constant et redondant avec `Firmware (flash)`). ⇒ HTML et IDs seuls.
2. **« Mise à jour manuelle » dans un `<details>` replié** (~330 px) : usage rare. ⚠️ Ouvert
   **automatiquement** quand `#online_update_unavailable` est visible. Cible tactile du `summary`
   ≥ 44 px, avec `:focus-visible`.
3. **Fusionner « Redémarrage » et « Réinitialisation » en une carte « Maintenance »** : économise
   un en-tête et un cadre. Le texte d'aide du redémarrage devient un `.field__hint` (plus
   d'encadré). ⛔ Les deux boutons restent **distincts** : « Redémarrer » en `btn--danger` atténué,
   « Réinitialiser » en `btn--danger`, **≥ 16 px d'écart**.
4. **Avertissement de réinitialisation : 9 lignes → 2** à 390 px — « Action irréversible : toutes
   les données sont supprimées. L'appareil redémarre en point d'accès. » ⚠️ La procédure Wi-Fi
   (`PoolControllerAP`, `192.168.4.1`) **ne disparaît pas** : elle passe dans la **confirmation
   avant exécution**, c'est-à-dire là où l'utilisateur en a besoin.

### ⚠️ À savoir avant de reprendre
- L'étude **n'a pas lu `app.css`** : aucun comptage d'emplois de composant partagé n'a été fait.
  ⇒ Si l'un de ces points touche un composant **partagé**, l'inventaire exhaustif de ses emplois
  reste dû (règle du design system — ⛔ l'application partielle est interdite).
- `bug-176` ajoute une ligne `#update_sig_note` (`.field__hint`) au-dessus des boutons de la carte
  « Mise à jour en ligne ». ⇒ En tenir compte : cette carte ne doit **pas** récupérer les trois
  lignes retirées en 2.87.0.
- `bug-177` touchera la même carte (attente active après redémarrage). ⇒ **Coordonner**, ou traiter
  les trois dans un même lot d'interface si l'ordre le permet.
