## Métadonnées
- **ID** : bug-177
- **Slug** : rechargement-aveugle-apres-redemarrage-ota
- **Type** : bug
- **Statut** : doing
- **Priorité** : haute
- **Version cible** : 2.88.0
- **Spec(s) liée(s)** : `feature-151` (§6 de l'essai de terrain du 2026-11-15, où le défaut a été
  observé), `bug-176` (même cycle, autre cause)

## Domaines touchés
- [ ] Capteurs (pH / ORP / température)
- [ ] Régulation (pump_controller, PID)
- [ ] Filtration / Éclairage
- [x] Web — routes HTTP / WebSocket
- [ ] MQTT / Home Assistant
- [ ] UART / écran LVGL
- [x] OTA
- [ ] Authentification
- [ ] Historique / persistance
- [x] Frontend (`data/`)

## Agents requis
- [x] spec-reader
- [x] architect
- [ ] pool-chemistry
- [x] ux-ui-designer
- [ ] embedded-developer
- [x] web-ui-developer
- [x] test-engineer
- [x] code-reviewer
- [x] doc-writer

## Description
Après une mise à jour, la page attend **quinze secondes en aveugle** puis recharge **une seule
fois**, sans jamais vérifier que l'appareil est revenu. Si le redémarrage prend un peu plus
longtemps, l'utilisateur tombe sur la page d'erreur de son navigateur, sans explication et sans
recours — alors que l'appareil fonctionne parfaitement.

## Contexte

**Observé en conditions réelles le 2026-11-15**, à l'issue de la première mise à jour en ligne
signée (2.87.0 → 2.87.1). Mots de l'utilisateur :

> *« par contre, je n'ai rien touché dans Safari et je ne vois pas l'appli »*
> *« un rechargement de la page fonctionne avec le dns local. Mais ce n'est pas un comportement
> normal. Un utilisateur serait perdu »*

### Le code en cause
`data/app.js` (séquence d'installation en ligne, fin du lot firmware) :
```js
let countdown = 15;
status.textContent = `✓ Mise à jour terminée. Rechargement dans ${countdown}s…`;
// …décrément…
window.location.reload();   // UNE fois, sans filet
```

### La marge mesurée : quatre secondes
Journal persistant de l'appareil, même séquence :
```
12:33:00 CRIT Redémarrage après mise à jour OTA
12:33:11 INFO mDNS: poolcontroller.local disponible
```
⇒ Le rechargement tombe à **12:33:15** pour un appareil prêt à **12:33:11**. ⚠️ **Quatre secondes
de marge**, et aucune seconde tentative. Un démarrage légèrement plus lent — ou un navigateur qui
a mis en cache l'échec pendant la fenêtre d'indisponibilité — et l'application disparaît.

⭐ **L'appareil n'est pour rien dans cet échec** : vérifié le jour même, il répond correctement en
mDNS (`poolcontroller.local` → `192.168.1.48`, TTL 120 s) et sert ses pages. Le défaut est
**entièrement côté page**.

### Découverte connexe, faite pendant les mesures de bug-176
L'appareil applique un quota de **120 requêtes/minute par IP** (`src/constants.h:219`,
`kMaxRequestsPerMinute`). ⚠️ Au-delà, `/check-update` est refusé **en quelques millisecondes**,
avec un journal `WARN : Rate limit dépassé` — mais **rien ne distingue ce refus** d'un autre
échec côté page. ⇒ Un utilisateur qui appuie plusieurs fois sur « Vérifier », ou qui laisse deux
onglets ouverts, peut déclencher ce quota et ne recevoir aucune explication utilisable.

## Comportement attendu

1. **Attente ACTIVE, pas un compte à rebours.** Après l'installation du firmware, la page affiche
   « L'appareil redémarre… », **interroge l'appareil** à intervalle régulier, et ne recharge
   **que lorsqu'il répond**.
2. **Une borne généreuse et un échec qui dit quoi faire.** Si l'appareil n'est pas revenu au bout
   d'un délai à arbitrer (ordre de grandeur : 90 s), la page affiche un message explicite — et
   notamment **l'adresse IP de secours**, puisque c'est le `.local` qui échoue le plus souvent.
   ⛔ Ne jamais laisser le navigateur afficher sa propre page d'erreur à la place.
3. **Le quota est nommé.** Un refus pour dépassement de quota se distingue d'une panne : message
   dédié, invitant à patienter, ⛔ jamais « erreur de mise à jour ».
4. **Rien ne change pour le cas nominal** : quand l'appareil revient vite, l'expérience doit être
   au moins aussi fluide qu'aujourd'hui, et de préférence plus rapide (⇒ recharger **dès** qu'il
   répond, au lieu d'attendre quinze secondes de principe).

### Cas limites
- Appareil qui ne revient **jamais** (flash raté, coupure secteur) : message terminal, pas de
  sondage infini.
- Navigateur ayant mis l'échec en cache (⚠️ cas **constaté** avec Safari sur `.local`) : le sondage
  doit contourner le cache (paramètre anti-cache) et, en dernier recours, proposer l'IP.
- `.local` qui ne se résout plus alors que l'appareil répond en IP : **c'est le cas observé** —
  la page doit pouvoir basculer ou au moins l'indiquer.
- Mise à jour du **filesystem seul** (`restart=false` puis `restart=true` pour le firmware) :
  ⚠️ un déploiement FS redémarre **aussi** l'appareil — ne pas supposer le contraire.
- Le sondage lui-même ne doit pas déclencher le quota de 120 req/min.
- ⚠️ **La page qui sonde est le trafic qui charge l'appareil** (mécanisme établi par `bug-176`) :
  l'intervalle de sondage doit rester modeste.

## Comportement actuel
Compte à rebours fixe de 15 s, puis `window.location.reload()` unique. Aucune vérification de
disponibilité, aucune seconde tentative, aucun message si l'appareil n'est pas revenu. Le refus
pour quota est indiscernable d'une panne.

## Contraintes techniques
- ⚠️ **RESPONSIVE** : téléphone **ET** tablette **ET** ordinateur, conçu **à 390 px d'abord**.
  Compter les caractères de tout libellé sur une ligne partagée.
- Vanilla JS, pas de framework.
- Français dans toute l'UI.
- Budget FS `spiffs` 512 KB — vérifier `./build_fs.sh`.
- ⛔ Le sondage doit rester **sous** `kMaxRequestsPerMinute` = 120 (`src/constants.h:219`).
- ⚠️ Ne pas aggraver la charge concurrente pendant une opération OTA (cf. `bug-176`).

## Fichiers potentiellement concernés
- `data/app.js` — séquence d'installation en ligne et son compte à rebours ; messages d'erreur
- `data/index.html` — si un élément d'état est ajouté
- `data/app.css` — si un état visuel est ajouté
- `docs/features/page-settings.md` — flux de mise à jour
- `docs/UPDATE_GUIDE.md` — ce que voit l'opérateur pendant un redémarrage
- `CHANGELOG.md`

## Livrables — check-list
- [x] B1 — Attente active : sondage jusqu'au retour de l'appareil, puis rechargement
- [x] B2 — Échec borné avec message explicite **et adresse IP de secours**
- [x] B3 — Refus pour quota distingué d'une panne, message dédié
- [x] B4 — Sondage anti-cache, et sous le quota
- [x] C1 — `docs/features/page-settings.md`, `docs/UPDATE_GUIDE.md`, `CHANGELOG.md`
- [x] D1 — Suite native sur la logique pure de décision du sondage (état / borne / abandon)
- [x] D2 — Validation par MUTATION
- [ ] D3 — Essai de terrain : mise à jour complète observée depuis un téléphone, **sans** que le
      navigateur affiche sa page d'erreur

## ADR attendu ?
- **Probable** : non
- **Critère(s) candidat(s)** : aucun
- **Sujet pressenti** : aucun — correctif d'ergonomie local, sans contrainte imposée au reste du système

## Éléments visibles (pour doc-writer)

### Endpoints HTTP concernés
- aucun endpoint neuf ; le sondage réutilise une route existante légère (à choisir par `architect`)

### Champs WebSocket
- aucun

### Topics MQTT
- aucun

### Documentation à produire / mettre à jour
#### Docs vivantes
- `docs/features/page-settings.md`

#### Docs transverses
- `docs/UPDATE_GUIDE.md`
- `CHANGELOG.md` — entrée 2.88.0

## Critères d'acceptation
- [ ] Un redémarrage plus lent que 15 s ne fait **jamais** apparaître la page d'erreur du navigateur
- [x] L'appareil revenu, la page se recharge **dès** qu'il répond
- [x] L'appareil absent au bout de la borne, un message explicite s'affiche **avec l'IP de secours**
- [x] Un refus pour quota affiche un message distinct d'une panne
- [ ] Tous les libellés utilisateur sont en français, longueur comptée, tenant à 390 px
- [x] `./build_fs.sh` passe sans dépasser la partition
- [x] Suite native verte, zéro régression

## Plan de test manuel
1. **Nominal** — mise à jour complète depuis un téléphone : la page se recharge seule, sans erreur.
2. **Retour lent** — simuler un appareil qui met 60 s à revenir : la page patiente et aboutit.
3. **Absent** — appareil qui ne revient pas : message explicite avec IP, pas de sondage infini.
4. **Quota** — dépasser 120 req/min et vérifier que le message est spécifique.
5. **Cache navigateur** — reproduire le cas Safari / `.local` constaté le 2026-11-15.

## Notes pour agent
- ⚠️ **Le défaut a été vécu par l'utilisateur**, pas déduit. Son verdict : *« Un utilisateur serait
  perdu »*. L'objectif n'est pas de réduire une probabilité, c'est de **supprimer la page d'erreur
  du navigateur** de ce parcours.
- ⚠️ `ux-ui-designer` passe **avant** `web-ui-developer`, avec un mandat sur l'**écran entier**
  (règle d'accumulation) et sur **tout** choix visuel, y compris ceux qu'on n'a pas pensé à lui confier.
- ⚠️ L'appareil régule un bassin réel et **doit continuer à doser** pendant toute l'opération.

## Hors périmètre
- La cause du saut de relais `.sig` : c'est `bug-176`.
- Le refus sur signature invalide : c'est D3-c de `feature-151`.
- Toute modification du firmware : ce lot est **frontend seul** (sauf si `architect` démontre le
  contraire pour le sondage).

## Notes de livraison (2026-11-15, lot web-ui + doc-writer)
- Livré : `data/app.js` (`awaitDeviceBack`, `otaRebootWaitStep`…), `data/index.html` (`#ota_recovery`), `data/app.css`, commentaire de contrat dans `src/web_routes_auth.cpp`, `test/ota_wait_vectors.js`, `tools/check_ota_wait_vectors.sh`, job CI `ota-wait-vectors`. Mesures : 81 suites / 2 433 cas natifs verts, 11 mutations tuées, FS ≈ 215 Ko gzippés (41,0 %).
- Docs : `docs/API.md` (contrat de sondabilité de `/auth/status`), `page-settings.md`, `UPDATE_GUIDE.md`, `BUILD.md`, `CHANGELOG.md` [2.88.2], `guide-utilisation.html`.
- Reste ouvert : **D3** (essai de terrain depuis un téléphone) ; critères « jamais la page d'erreur » et « libellés tenant à 390 px » non cochés (preuve = terrain). `src/version.h` encore à 2.88.1.
- Limites assumées : pas de bascule automatique vers l'IP (CSP + changement d'origine) ; re-sondage sur `visibilitychange` ; grâce 8 s liée à `kRestartAfterOtaDelayMs = 3000`.

---

## 🏗️ CONCEPTION, TESTS ET REVUES (2026-11-15)

### Principe : on ne recharge que sur une page PROUVÉE servable
8 s de grâce → sondage `GET /auth/status` toutes les 2 s (90 s au plus) → **confirmation
`GET /index.html`** → rechargement. En cas d'échec, bloc de secours `#ota_recovery`.

⭐ **La confirmation est ce qui ferme réellement le chemin vers la page d'erreur**, pas l'attente :
un appareil peut être **vivant** avec un système de fichiers non remonté, donc **incapable de
servir la page**. Un non-2xx renvoie dans la boucle.

### 🔴 CE QUI AURAIT RENDU LE CORRECTIF PIRE QUE LE DÉFAUT
`kRestartAfterOtaDelayMs = 3000` : l'appareil répond `200` **puis redémarre 3 s plus tard**.
⚠️ Une attente active naïve aurait sondé immédiatement, obtenu ce `200` d'un appareil **pas encore
redémarré**, rechargé, et le rechargement aurait été **tué en vol** — ⛔ le défaut d'origine, **en
plus reproductible**. ⇒ **Grâce de 8 s. ⛔ NE JAMAIS LA RACCOURCIR.**

### `/auth/status` : quatre propriétés, et elles étaient des ACCIDENTS
(1) publique · (2) **hors quota** (⛔ pas de `checkRateLimit`) · (3) **sans système de fichiers** ·
(4) **enregistrée en premier** (`web_server.cpp:96`), donc **hors de la garde 503** (`:111`).
⚠️ **Vérifié dans le code**, pas repris d'un rapport. Idem `Retry-After: 60` (`auth.cpp:247`).
⇒ Contrat inscrit **en commentaire au-dessus du handler** (`src/web_routes_auth.cpp`) **et** dans
`docs/API.md`. ⚠️ Les casser ne produirait **aucune erreur** — seulement une page qui ne constate
plus le retour de l'appareil, **en silence**. C'est la famille de défauts de `bug-169`.

### ⭐ UN SECOND EXEMPLAIRE DU MÊME DÉFAUT
La carte **« Mise à jour manuelle »** avait son **propre** compte à rebours aveugle (30 s). ⚠️ Ni la
spec ni moi ne l'avions vu — trouvé par l'`architect` en lisant le code. ⛔ N'en corriger qu'un
aurait laissé intact un chemin vers la page d'erreur, **sur la carte de RÉPARATION** — celle qu'on
utilise quand le reste a échoué.

### Autres décisions
- ⛔ **Pas de bascule automatique vers l'IP** : la CSP (`default-src 'self'`) interdit de sonder
  l'IP depuis `.local`, et changer d'origine **déconnecte** l'utilisateur. ⇒ On **affiche** un lien
  et on **prévient** de la reconnexion.
- ⭐ **Re-sondage sur `visibilitychange`** : un téléphone qui verrouille l'écran **gèle les
  minuteurs** — sans cela, les 90 s pourraient s'écouler **sans un seul sondage réel**.
- ⭐ **Masquer la barre de progression** pendant l'attente : à 100 %, elle dit « fini » alors qu'on
  attend. ⚠️ C'est probablement une partie de ce qui a induit l'utilisateur en erreur le 15/11.
- Décompte **arrondi à 5 s**, nombre dans un `<span aria-hidden="true">` ⇒ ⛔ pas de ré-annonce
  chaque seconde par un lecteur d'écran.

### 🧪 TESTS — vecteurs JS (ADR-0099), pas de suite C++
`test/ota_wait_vectors.js` + `tools/check_ota_wait_vectors.sh` + job CI `ota-wait-vectors`.
**18 vecteurs**, extraction du **code vivant** par `test/js_extract.js` — ⛔ aucune recopie.
⚠️ **Le drapeau `OTA_WAIT_STRICT` a été SUPPRIMÉ** une fois les défauts corrigés : un mécanisme qui
désarme des vecteurs est un endroit où un défaut se rendort.

**11 mutations, 11 tuées** — dont `graceMs` 8000→0 (23 échecs), budget, borne `>=`, 429 reclassé,
arrondi, suppression de `confirm`, `?t=` retiré, motif d'octet, garde de grâce déplacée, borne sur
le seul premier octet, et `0.0.0.0` accepté.

### 🔴 TROIS DÉFAUTS TROUVÉS PAR LES TESTS, QUE TROIS RELECTURES AVAIENT LAISSÉS PASSER
1. ⭐ **La garde de grâce n'était pas dans la fonction pure** : `servable`/`confirmed` donnaient
   `confirm`/`reload` **avant 8 s**. Non atteignable alors (l'orchestrateur ne sondait pas pendant
   la grâce) — ⛔ **mais l'invariant reposait sur la DISCIPLINE DE L'APPELANT**, donc il aurait
   disparu à la première refonte, sans que rien ne le signale. ⇒ Garde déplacée **en tête**.
2. **Octets d'adresse non bornés** : `999.1.1.1` était accepté (`\d{1,3}`). ⇒ Bornés 0-255 **sur
   les quatre** (mutation dédiée pour le prouver), zéro de tête refusé.
3. **`0.0.0.0` accepté** ⇒ aurait produit un lien mort. Refusé ; le repli « cherchez-le dans votre
   box » vaut mieux.

### 👀 REVUE DE CODE — Approved, et une remarque « mineure » reprise
✅ Sept invariants vérifiés dans le code. ⭐ Elle confirme que **l'URL du lien est CONSTRUITE après
validation IPv4** ⇒ `javascript:` et `//evil` sont inatteignables : la validation n'est pas
cosmétique, c'est la garde du `href`.

🔴 **Remarque reprise, car elle n'était pas mineure** : sur un **503** de famine mémoire, le bloc
affichait « ne répond pas encore » **et conseillait d'ouvrir l'appareil par son IP**.
⚠️ Le libellé était faux (l'appareil **répond**), mais surtout **le conseil était un FAUX REMÈDE** :
l'appareil est joignable, c'est **servir la page** qu'il ne peut pas. ⇒ Troisième cas ajouté :
`L'appareil est là, mais pas prêt` (31) + « Il redémarre ou finit de se préparer. Patientez une
minute, puis rechargez la page. », **sans lien IP**.
⚠️ **Deux messages et non un seul** pour 429 et 503 : le **geste diffère** (patienter une minute
contre attendre qu'il se prépare).
Également repris : le `<p>` conteneur du lien est masqué (plus de marge résiduelle) et le corps de
la requête de confirmation est libéré avant le rechargement.

### Mesures
`pio run` après `clean` **SUCCESS** · Flash **51,3 %** · RAM **29,8 %** · **0 nouveau warning**
(2 OneWire préexistants, relus) · **81 suites, 2 433 cas natifs, 0 échec** · `build_fs.sh`
**215 196 o, 41,0 %** · `check_doc_links` 6 125 liens · version **2.88.2**.

### ⛔ RESTE OUVERT — D3, essai de terrain
Le parcours réel (mise à jour, 429 ou 503 pendant le redémarrage, rechargement) et le rendu des
trois cas du bloc **n'ont pas été vus sur l'appareil**. ⚠️ `ui_shot.sh` ne sait pas forcer ces
états. ⇒ Vérifiable au prochain déploiement.
