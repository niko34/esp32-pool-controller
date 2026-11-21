## Métadonnées
- **ID** : bug-179
- **Slug** : journal-rendu-en-innerhtml-sous-csp-permissive
- **Type** : bug — 🔴 **sécurité applicative**
- **Statut** : doing
- **Priorité** : haute
- **Version cible** : ~~2.89.0~~ → **2.97.0** (la fiche datait de la v2.89.0 ; le dépôt était en 2.96.2 quand elle a été reprise)
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
- [x] A0 — ⭐ **Soupçon ÉTABLI, et CONFIRMÉ sur l'appareil réel** (2026-11-21) — voir la section dédiée
- [x] B1 — Rendu du journal en DOM, plus aucun `innerHTML` — **et étendu à 5 puits** (voir ci-dessous)
- [x] B2 — Lisibilité préservée, vecteur dédié qui tue aussi un **sur-échappement**
- [x] B3 — Performance **mesurée**, un défaut trouvé et corrigé (voir « B3 »)
- [x] A1 — ⛔ **retrait NON fait** : inventaire écrit ci-dessous + fiche de suivi
- [x] A2 — assainissement **à la source**, 31 sites
- [x] C1 — `logger.md`, `auth.md`, `web-server.md`, `page-settings.md`, `API.md`, `CHANGELOG.md`, ADR-0136
- [x] D1 — suite native + vecteurs JS par **extraction** (ADR-0099)
- [x] D2 — **6 mutants sur 6** tuent un test nommé ⭐ dont un qui a révélé une garde décorative
- [x] D3 — Essai de terrain **fait** sur l'appareil de production, charge **inerte** — ⭐ et il a trouvé un défaut

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

---

## 🔴 A0 — TRANCHÉ : LE SOUPÇON EST VRAI, ET MESURÉ SUR L'APPAREIL DE PRODUCTION

**C'est la branche grave de la fiche.** ⇒ N'importe qui sur le réseau local, **sans aucun
identifiant**, dépose le texte de son choix dans le journal ; ce texte est ensuite rendu par
`innerHTML` **dans la session de l'administrateur** quand il ouvre le journal.

### La preuve, relevée sur l'appareil (192.168.1.48), pas déduite

```
[2026-11-21 14:58:52] WARN : Accès non autorisé depuis 192.168.1.55 vers /get-config/<b>BUG179</b> ; et 3 refus non journalisés (autres routes)
```

⚠️ **Aucun jeton n'a été présenté.** La requête était un simple `curl` anonyme.
⛔ Charge utile volontairement **inerte** (`<b>`) : l'essai devait prouver le dépôt, pas exécuter
quoi que ce soit sur la machine de l'utilisateur.

### La chaîne, maillon par maillon — chacun vérifié dans le code courant

| # | Fait | Emplacement vérifié |
|---|---|---|
| 1 | Le message de refus **concatène `req->url()` sans échappement** | `src/auth.cpp:51` |
| 2 | ⭐ Un chemin **arbitraire** atteint la garde d'une route protégée : `/get-config/<...>` rend **401**, ⛔ **pas 404** — la bibliothèque fait une correspondance par **préfixe** | mesuré : 3 sondes, toutes 401 |
| 3 | Le 404, lui, **ne journalise rien** (`onNotFound` répond et sort) | `src/web_server.cpp:283-285` |
| 4 | Le journal est rendu par **`innerHTML`**, sans échappement | `data/app.js:223-225` (`renderLogs`) |
| 5 | La CSP autorise **`script-src 'unsafe-inline'`** | `src/web_server.cpp:104-105` |

⇒ **Dépôt anonyme + rendu non échappé + CSP permissive = exécution de code dans la session admin.**
Et l'administrateur de cet appareil pilote des **pompes à acide**.

### ⚠️ Ce qui atténue, et qui ⛔ NE CORRIGE PAS

Le site « 401 » est **borné** (ADR-0127, recul exponentiel : fenêtre 60 s << shift, plafond
64 min). Mes quatre sondes l'ont montré en direct : la première a été journalisée, **trois ont été
occultées**, puis la cinquième est passée après l'expiration de la fenêtre.

⛔ **Le bornage ne protège de rien ici.** Il limite le *débit* de purge du journal (son objet,
bug-112) ; il ne limite pas l'*attaquant*, à qui **une seule ligne suffit**. Et la toute première
tentative sur un appareil au repos est **toujours** journalisée. ⇒ Le recul exponentiel joue même
**contre** la défense : il rend l'attaque plus discrète en effaçant les tentatives voisines.

### Les autres entrées non maîtrisées trouvées au passage

| Source | Emplacement | Authentification requise ? |
|---|---|---|
| ⭐ `req->url()` sur refus 401 | `src/auth.cpp:51` | ⛔ **AUCUNE** |
| Charge utile MQTT rejetée (`payloadStr`) | `src/mqtt_manager.cpp:1731`, `:1855`, `:1910` | identifiants du courtier |
| SSID (connexion Wi-Fi) | `src/main.cpp:881`, `src/web_routes_config.cpp:2574`, `:2884`, `:2894`, `:2933` | session admin, sauf `WiFi.SSID()` |
| `tag_name` GitHub | fermé par `bug-178` (filtre chiffres et points) | — |

⇒ **A2 n'est pas optionnel** : il reste au moins deux familles d'entrées non maîtrisées après
correction du rendu, et le dépôt anonyme est la seule qui soit exploitable par le réseau seul.

### Conséquence sur la conception

⭐ **Les deux corrections sont indépendantes et aucune ne dispense de l'autre** :
B1 (rendu DOM) ferme l'**exécution** ; A2 (bornage à la source) ferme le **dépôt**. Un seul des
deux laisserait un défaut : le journal est aussi lu ailleurs (téléchargement, copier-coller vers
un autre outil), et une ligne de journal vérolée reste un piège hors de la page.

---

## ✅ IMPLÉMENTATION — 2026-11-21, version 2.97.0

### Les cinq puits : 3 convertis, ⭐ 2 SUPPRIMÉS

| Puits | Sort |
|---|---|
| `data/app.js` — journal | **converti** : `formatLogLine()` pure + `buildLogLine()` en `createElement`/`textContent`, partagée avec le temps réel |
| `data/wifi.html` — liste de scan | **converti** |
| `data/wizard.html` — liste de scan | **converti** |
| `data/app.js` — notification Wi-Fi ×2 | ⭐ **SUPPRIMÉE** |

⭐ **La notification Wi-Fi était du CODE MORT.** `data/wifi.html:572/578` redirigeaient vers
`/#/settings/wifi?wifi=…&ssid=…` — paramètres **après le `#`**, donc dans `location.hash` — alors
que le lecteur lisait `location.search`. ⇒ Elle ne s'est **jamais** affichée en usage normal : sa
seule façon de s'allumer était un **lien fabriqué**. ⛔ Elle n'existait que comme puits.
Le besoin est couvert ailleurs (`showSuccess()` avant la redirection, `updateWiFiDisplay()`).
**Bonus** : 2 attributs `onclick=` en ligne partent avec.

### A2 — assainissement à la source, 31 sites

`src/log_text_sanitize_logic.h` — module **pur**, zéro allocation, tampon de pile.
Liste blanche **positive** ; tout le reste en `%XX` majuscule ; `%` → `%25` ⇒ ⭐ **réversible** ;
troncature à `kLogExternalTextMax = 96` sur **frontière d'échappement**, suffixe `~<longueur>o`.

| Famille | Sites | Identifiants requis ? |
|---|---|---|
| Adresse d'une requête refusée (`src/auth.cpp`) | 1 | ⛔ **AUCUN** |
| Charge MQTT (`src/mqtt_manager.cpp`) | 21 + 3 multi-lignes | courtier |
| SSID (`src/main.cpp`, `src/web_routes_config.cpp`) | 5 | session admin |
| Nom de fichier d'envoi manuel (`src/web_routes_ota.cpp`) | 1 | session admin |

⚠️ **Régression de lisibilité assumée** : « Livebox-Café » s'écrit `Livebox-Caf%C3%A9` **dans le
journal**. ⛔ Pas à l'écran de la liste des réseaux. Laisser passer l'UTF-8 ≥ 0x80 rouvrirait le
texte bidirectionnel et les séquences sur-longues.

### ⭐ Pourquoi PAS d'assainissement dans le journal lui-même (ADR-0136)

`src/logger.cpp:111-114` capture les **pointeurs de l'appelant AVANT le mutex** (condition C3 de
feature-129). ⇒ Nettoyer au `memcpy` de l'arène aurait donné un `/system.log` propre et une
**trame WebSocket vérolée** : **5ᵉ récidive du piège de parité**, fabriquée volontairement.
⛔ Nettoyer sur place le tampon de l'appelant est **impossible** : beaucoup d'appels passent un
littéral `.rodata` ⇒ écriture en flash = plantage.

### B3 — le défaut que la mesure a trouvé

Le rendu incrémental se dégradait en **reconstruction complète** dès que l'anneau atteint ses 500
entrées : `push` + `shift` change la première entrée, que le code comparait. ⇒ **≈1000
`createElement` toutes les 5 s**, et ⭐ **c'est le régime NORMAL** (l'anneau est plein en
permanence). Corrigé : on retrouve l'ancre (`lastIndexOf` de la dernière ligne rendue) et on ne
touche **qu'aux deux bouts**, avec compensation du défilement pour que la vue ne saute pas.
Le vecteur est **strict** : remettre la comparaison naïve le tue.
Médiane de 20 reconstructions de 500 lignes : **10,35-10,80 ms** contre **8,70 ms** avant,
soit **1,19-1,24×** (seuil 1,5×).

### Gardes posées

| Garde | Refuse |
|---|---|
| `tools/check_no_html_interpolation.sh` (neuve) | toute écriture HTML dont le membre droit n'est pas un **littéral pur**, identifiant simple compris. 1 seule exemption dans tout `data/`, motivée en ligne |
| `tools/check_unauth_log_guard.sh` règle (6) | texte externe brut en argument d'un `systemLogger` |
| `tools/check_unauth_log_guard.sh` règle (6b) | le contournement **par variable intermédiaire** dans `auth.cpp` |
| `tools/check_log_line_vectors.sh` + `test/log_line_vectors.js` | 43 assertions, par **extraction** (ADR-0099) |
| `test/test_native_log_text_sanitize/` | 14 cas ; le vecteur des **80 routes réelles** exige `sanitize(r) == r` octet pour octet |

⭐ **La mutation M6 a révélé une garde DÉCORATIVE** : la règle (6) ne regardait que l'intérieur de
l'appel, donc ranger l'adresse dans une variable suffisait à passer. ⇒ Règle (6b) ajoutée.
⛔ **Limite écrite dans le script** : 6b ne couvre que `auth.cpp`. Généraliser demanderait de
suivre le **flot de données**, hors de portée d'une sentinelle textuelle, et « toute ligne citant
`payloadStr` doit être assainie » noierait la garde sous les usages d'**analyse**, légitimes et
majoritaires. ⇒ `auth.cpp` est couvert parce que c'est le **seul chemin sans identifiants**.

⚠️ **Deux faux verts de la sentinelle neuve, trouvés par l'audit P2 et corrigés** : un littéral
suivi d'un `+` **à la ligne suivante**, et un `//` **à l'intérieur d'une chaîne** plus tôt sur la
ligne, qui faisait passer la ligne entière pour un commentaire. Vérifiés sur trois fichiers-pièges.

### A1 — inventaire de ce qui exige encore `script-src 'unsafe-inline'`

⛔ **Le retrait N'EST PAS fait dans ce lot.** Motif : `data/login.html` et `data/wizard.html` sont
la page de connexion et l'assistant ; ⚠️ une externalisation fautive rend l'appareil
**inatteignable** — il n'y a **aucun port série** dans le conteneur, l'OTA passe par l'interface.
⇒ Fiche de suivi ouverte.

| Fichier | Bloc `<script>` en ligne | Taille |
|---|---|---|
| `data/index.html:15` | 1 | 552 o |
| `data/login.html:337` | 1 | 9 550 o |
| `data/wifi.html:415` | 1 | 7 633 o |
| `data/wizard.html:988` | 1 | 31 032 o |

Attributs `onclick=` restants : **3**, tous dans `data/wizard.html` (`:1583`, `:1586`, `:1637`).
⭐ Il y en avait **5** : deux sont partis avec la notification supprimée.
Aucun `eval(`, aucun `new Function(`, aucun `javascript:`.
Marge FS mesurée : **~302 800 o sur 524 288** ⇒ ⛔ le budget n'est **pas** un obstacle.
⚠️ Les empreintes CSP sont à proscrire : l'en-tête vit dans le **firmware** et les pages dans le
**système de fichiers**, déployés séparément — une empreinte casserait les pages à chaque envoi
isolé du système de fichiers.
⛔ `style-src 'unsafe-inline'` reste de toute façon (222 attributs `style=`, 3 blocs `<style>`).

### Ce qui reste OUVERT, nommé et porté par une fiche

**15 sites journalisent encore du texte brut**, tous **derrière des identifiants** — effet maximal :
une ligne d'apparence authentique dans le journal, ⛔ **jamais d'exécution**.
`src/web_routes_ota.cpp:113`, `:133`, `:2137`, `:2155` · `src/web_routes_config.cpp:1366`, `:1368`,
`:1393`, `:1492` · `src/main.cpp:1052` · `src/mqtt_manager.cpp:346`, `:489`, `:669`, `:2477` ·
`src/atlas_ezo.cpp:168`, `:231`.

⭐ **Et un défaut d'une autre famille, trouvé par l'audit P1** : `/save-config` écrit
`auth_password` et `auth_enabled` **sans demander le mot de passe courant**
(`src/web_routes_config.cpp:1659-1665` — le commentaire sur place le signale déjà), et
`POST /ota-signature-switch/arm` n'exige que le jeton. ⇒ Tout vol de jeton devient une **perte
définitive** de l'appareil. Fiche de durcissement ouverte.

### Agents — rectification

⚠️ La revue de code a relevé « `ux-ui-designer` non invoqué ». ⛔ **C'est inexact** : il a été
invoqué le 2026-11-21 sur l'**écran entier**, captures 390 px et 1024 px jointes au brief.
Ses décisions **retenues** : césure forcée, couleurs par niveau **avec redondance non colorée**
(barre 2/3/4 px + gras + le mot du niveau dans le texte, car ambre et rouge sont confondables en
deutéranopie), quatre états de texte, `role="log"` + `aria-live="off"` (le `polite` faisait
réannoncer **500 lignes toutes les 5 s** à un lecteur d'écran), `<bdi>` et remplacement des
caractères invisibles dans les noms de réseau, et le **vocabulaire**.
Ses décisions **refusées, avec motif** : supprimer la case « Scroll auto », la case **DEBUG** et le
bouton « Effacer (écran) », et remplacer les 5 cases de niveaux par un seuil.
⛔ La case DEBUG ne fait **pas** double emploi avec l'interrupteur firmware — vérifié :
`data/app.js:212` décide de l'**affichage**, `:4283` de l'**émission**. ⛔ « Effacer (écran) » est
documenté (`docs/API.md:1101`, `docs/features/page-settings.md:461`) comme le pendant **inoffensif**
du bouton destructif. ⚠️ Et un **seuil** retirerait une capacité de diagnostic : masquer INFO en
gardant DEBUG deviendrait impossible. ⇒ Fiche séparée.

### Outillage réparé en chemin

⭐ `tools/ui_shot.sh` ne recopiait **pas** `/get-logs` : le panneau Journal s'affichait **toujours
vide** en capture. ⇒ **Personne ne pouvait voir le rendu du journal avant de déployer** — très
exactement l'angle mort qui a laissé un `innerHTML` passer des mois. Recopie ajoutée, et rendue
**facultative** pour que l'outil continue de marcher contre un firmware antérieur.

### Contrôles

`pio run` **SUCCESS, 0 warning** (flash 1 970 293 o / 52,7 % · RAM 98 552 o / 30,1 %) ·
`pio test -e native` **95 suites / 2 710 cas / 0 échec** · `./build_fs.sh` SUCCESS,
**~302 800 o de marge** · **35 gardes sur 36 vertes** (`check_release_artefacts.sh` exige une
signature de release — ⛔ normal hors release) ·
`logSanitizeExternalText` **fuzzé 600 000 cas sous ASan/UBSan** : aucun dépassement, aucune
collision, jamais de `%XX` coupé.

---

## ✅ D3 — ESSAI DE TERRAIN, 2026-11-21, SUR L'APPAREIL DE PRODUCTION

Déploiement de la **2.97.0** par `./ota_update.sh --use-sig`, **un binaire à la fois**.
Les deux acceptés, verdicts relevés au journal persistant (authenticité valide, liaison
manifeste↔image liée deux fois, empreinte conforme). ⛔ Charge **inerte** (`<b>`) à toutes les
étapes — aucune charge exécutable n'a été déposée sur l'appareil de l'utilisateur.

### Le dépôt est fermé — mesuré, pas déduit

| | Ligne réellement écrite dans le journal |
|---|---|
| **avant** (2026-11-21 14:58) | `… vers /get-config/<b>BUG179</b>` |
| **après** (19:23 et 19:36) | `… vers /get-config/%3Cb%3EBUG179-POST%3C/b%3E` · `… %3Cb%3ED3-FINAL%3C/b%3E` |

Requêtes **anonymes** (aucun jeton), réponse **401** inchangée. ⭐ La route `/get-config/` reste
lisible (la barre oblique est en liste blanche) et la charge est **reconstructible à l'identique**.

### ⭐ CE QUE SEUL L'ESSAI RÉEL POUVAIT TROUVER

En décochant « CRITICAL » devant de vraies lignes, elles sont **restées affichées**.
⇒ **Le firmware écrit `CRIT`** (`src/log_arena_logic.cpp:127`), **l'interface attendait `CRITICAL`**.

| Défaut | Ancienneté | Effet |
|---|---|---|
| La case CRITICAL ne filtrait **RIEN** | ⚠️ **préexistant**, jamais vu | les lignes tombaient dans « niveau inconnu, affiché par défaut » |
| La **couleur par niveau** ne s'appliquait pas aux lignes critiques | ⛔ **introduit par ce lot** | le niveau qui en a le plus besoin était le seul non signalé |

Les deux corrigés. ⭐ **Et le vecteur qui manquait a été écrit** : il énumère les **cinq** noms de
niveau réellement émis par le firmware (`DEBUG`, `INFO`, `WARN`, `ERROR`, `CRIT`) et vérifie pour
chacun que **la classe de couleur s'applique** *et* que **sa case le masque**. Remis à l'ancien
comportement, il meurt. 43 → **53 assertions**.

⚠️ **La leçon vaut d'être écrite** : aucun test, aucune revue, aucun audit n'avait vu ce défaut —
quatre agents ont lu ce code. Il a fallu **décocher une case devant de vraies données**.
⭐ *Un test vert ne prouve rien tant qu'il n'a pas été attaqué*, et ici l'attaque était un clic.

### Rendu observé à 390 px, sur les données de l'appareil
Lignes critiques : fond teinté, barre gauche, texte clair — la couleur n'est jamais seule.
Décocher le niveau les masque, et l'état vide s'affiche : « Aucun message à ce niveau. 60 masqués. »
Césure forcée, retrait suspendu, horodatage atténué : conformes.

### ⛔ Ce qui N'A PAS été observé, et pourquoi
La ligne de refus elle-même n'a **pas** été vue rendue à l'écran : `/get-logs` sert l'anneau **par
pages** (~30 entrées, puis `?since=`), et le serveur local de `ui_shot.sh` rend **toujours la même
page** — donc le panneau capturé s'arrête aux lignes de démarrage. ⇒ L'affichage en texte littéral
est établi par le **chemin de rendu unique** (`buildLogLine`, `textContent`, partagé par toutes les
lignes), par les **53 vecteurs** et par la **mutation M3**, ⛔ pas par une capture de cette ligne-là.
Limite d'outillage, à lever si `ui_shot.sh` apprend un jour à pagineriser son stub.

### Version
⚠️ La 2.97.0 a été renvoyée **une seconde fois** (système de fichiers seul) après la correction
`CRIT`. ⛔ **Pas de bump** : la 2.97.0 n'a jamais été publiée, elle n'a vécu que treize minutes sur
l'appareil, et c'est bien ce contenu-là qui doit porter ce nom. Le firmware n'ayant pas changé, sa
signature restait valable : **une seule signature** a été demandée à l'utilisateur.
