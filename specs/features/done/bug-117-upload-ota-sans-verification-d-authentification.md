# bug-117 — Le handler d'upload OTA ne vérifie aucune authentification

## Métadonnées
| | |
|---|---|
| Type | bug de **sécurité — critique** |
| Statut | **terminé** (2026-09-07) — tests verts, revue `code-reviewer` **Approved**, code figé |
| Priorité | **CRITIQUE** — à traiter avant tout autre bug de cette file |
| Version livrée | **2.46.2** (`src/version.h`) |
| Relevé par | `doc-writer`, 2026-09-07, en refusant de reprendre le verdict « sain » de l'audit ; confirmé par lecture directe |
| Specs liées | `bug-save-config-applique-avant-authentification` (**même topologie**, corrigée en v2.46.1), `feature-026` (intégrité OTA), [`feature-118`](../doing/feature-118-authenticite-de-l-image-ota.md) (**suite** : authenticité de l'image), [`bug-112`](../todo/bug-112-journal-non-borne-sur-route-protegee.md) (journal non borné) |

## Domaines touchés
- [x] OTA (`src/web_routes_ota.cpp`, `handleOtaUpdate()`)
- [x] Web — Authentification
- [ ] Régulation — [ ] Capteurs — [ ] MQTT — [ ] Frontend

## Agents requis
- [x] architect · [x] embedded-developer · [x] code-reviewer · [x] doc-writer
- [ ] `pool-chemistry` — sans objet (aucun chemin de dosage), mais l'**impact** est total :
      un firmware arbitraire contrôle les pompes.

## Description
`handleOtaUpdate()` (`src/web_routes_ota.cpp:234`) est le **handler d'upload** de
`POST /update`. Il **ne contient aucune vérification d'authentification** — vérifié par
recherche exhaustive sur les ~286 lignes de la fonction : ni `authManager`, ni
`checkAuth`, ni `REQUIRE_AUTH`, ni `checkTokenAuth`, ni `checkBasicAuth`.

`REQUIRE_AUTH(req, RouteProtection::CRITICAL)` n'existe que dans le **lambda de
réponse** (`:1198`), exécuté **après** le handler d'upload. Or ce handler exécute
`Update.begin()` (`~:400`) puis `Update.end(true)` (`~:497`).

**C'est exactement la topologie du défaut corrigé en v2.46.1 sur les trois routes de
calibration** — authentification présente mais évaluée trop tard — avec un effet
incomparablement plus lourd : l'écriture d'une partition applicative.

## Pourquoi l'empreinte SHA-256 ne rattrape pas
La comparaison est conditionnée à `g_uploadHasExpected` : elle n'a lieu **que si le
client a fourni** un paramètre `sha256`. Sans ce paramètre, la finalisation s'exécute.

Et même fournie, une empreinte **transmise par l'appelant** vérifie l'intégrité du
**transport**, jamais l'**authenticité** de l'image. `CLAUDE.md` (règle de sécurité
absolue n° 6) demande de « vérifier la signature/CRC avant flash » : l'implémentation
actuelle vérifie un condensat que l'appelant choisit lui-même.

## Pourquoi le défaut a survécu à un audit conduit exprès
L'audit du 07/09 avait classé cette route **saine**, en lisant le pré-contrôle de
l'octet magique (`:260-315`, « refus AVANT `setOtaInProgress`, `LittleFS.end()` et
`Update.begin()` »). Ce pré-contrôle refuse une image **invalide** — c'est un contrôle
de **validité**, pas d'**autorisation**. Les deux ont été confondus.

### La leçon de méthode — le vrai livrable de ce cycle

Ce défaut n'a été trouvé **qu'en refusant de reprendre le verdict « sain » de l'audit**
et en relisant la fonction ligne à ligne. Sans ce refus, il partait en production. Ce
n'est pas anecdotique : c'est reproductible, et c'est la partie de ce cycle qui se
réutilise.

**Trois défauts de sécurité en une seule journée (2026-09-07), tous survivants à des
revues pour la MÊME raison : un garde-fou VISIBLE fait passer le relecteur.**

| Garde-fou visible | Ce qu'il contrôlait réellement | Défaut |
|---|---|---|
| `REQUIRE_AUTH` présent **trois lignes plus bas** | la **réponse**, pas le corps | routes de calibration (v2.46.1) |
| une **sentinelle de parité** `/data` ↔ WebSocket | elle déclarait **légitime** le bug qu'elle devait signaler | `bug-082` |
| le pré-contrôle de l'**octet magique** | la **validité** de l'image, prise pour une **autorisation** | `bug-117`, cette spec |

> **Règle : chercher la PLACE de la garde dans le cycle de vie de la requête, jamais sa
> seule présence.** Corollaire opératoire : un verdict « route saine » n'est recevable
> que s'il nomme *quelle* garde s'exécute *à quel moment* du cycle de vie — « il y a un
> contrôle » n'est pas un verdict.

## Comportement attendu
Vérification d'authentification en **première instruction** du handler d'upload, avant
tout effet de bord — sur le patron de `calibrationBodyAuthorized()` (v2.46.1) et de
`saveConfigBodyAuthorized()` (v2.25.0), avec le niveau `CRITICAL`.

`REQUIRE_AUTH` reste dans le lambda de réponse : il produit le 401 propre. La garde
n'est pas redondante — elle empêche l'effet de bord.

## Contraintes techniques
- ⚠️ **Ne pas casser l'OTA légitime.** C'est le seul canal de déploiement de cet
  appareil (conteneur LXC sans port série depuis le 05/09 : OTA uniquement). Un
  correctif trop strict rendrait l'appareil non déployable — donc non corrigeable.
- Vérifier le chemin `/download-update` (mise à jour depuis GitHub), qui pilote aussi
  `Update` : même examen, même exigence.
- Le prédicat doit accepter tout ce que `REQUIRE_AUTH(CRITICAL)` accepte, sans le rate
  limiting (une seule réponse, un seul décompte).

## Critères d'acceptation
- [x] Un upload non authentifié n'atteint **ni** `Update.begin()`, **ni** aucune
      écriture de partition
- [x] L'OTA authentifié fonctionne — équivalence des trois clients légitimes
      **prouvée par lecture du code d'ESPAsyncWebServer 3.11.2**, et
      **déploiement réel à exécuter** : voir *Plan de vérification post-déploiement*
      (l'appareil tourne encore la **2.45.0**, l'étape n'est **pas** exécutée)
- [x] `/download-update` audité, résultat consigné (**SAIN**, raison structurelle)
- [ ] Décision tracée sur l'empreinte : rendue **obligatoire**, ou remplacée par une
      vraie signature, ou explicitement assumée comme contrôle d'intégrité seul (→ ADR)
      — **NON couvert par ce cycle**, renvoyé à
      [`feature-118`](../doing/feature-118-authenticite-de-l-image-ota.md) : ce
      correctif apporte l'**autorisation** de l'appelant, pas l'**authenticité** de
      l'image. Les deux sont indépendants et le second ne devait pas retarder le premier.
- [x] `pio run` et `pio test -e native` sans régression

## Correctif livré (2.46.2)

`otaUploadAuthorized()` ([`src/web_routes_ota.cpp:290`](../../../src/web_routes_ota.cpp:290)),
appelée en **première instruction** de `handleOtaUpdate()`
([`:360`](../../../src/web_routes_ota.cpp:360)) :

- `fail-closed` sur pointeur nul ; `isEnabled()` faux → autorisé ; sinon
  `checkBasicAuth() || checkTokenAuth()`.
- **Avant le verrou `g_uploadOwner`**, avant le pré-contrôle du type, avant tout
  effet de bord. Le chemin de refus **ne touche rien** : aucune statique, pas de
  `g_uploadOwner`, pas de `markUploadRejected()`, rien sur `Update`, ni empreinte,
  ni rollback.
- **Réévaluée à CHAQUE fragment**, jamais mise en cache par pointeur de requête :
  un pointeur **réutilisé après `free`** accorderait l'authentification à une
  autre requête. Un client ne doit pas franchir la garde une fois puis pousser la
  suite du flux.
- `REQUIRE_AUTH` **reste** dans le lambda de réponse et produit le `401` : une
  seule réponse, un seul décompte de quota. La garde n'est **pas** redondante —
  elle empêche l'effet de bord.
- WARN throttlé (≤ 1/60 s, une seule fois par upload) réutilisant
  `kCalibrationAuthWarnThrottleMs` ; chemin muet **sans aucune allocation**.

### Bénéfice second, relevé en revue et non anticipé

`PumpController::setOtaInProgress(true)` **coupe les pompes doseuses**. Il était
atteignable depuis le handler d'upload, donc **sans authentification** : un
appareil du réseau local pouvait **suspendre la régulation pH et ORP** en
déclarant un OTA fantôme, sans jamais fournir d'image valide et sans laisser
autre chose qu'un « Type de mise à jour » au journal. La garde ferme ce vecteur
en même temps que le flash. Ce n'était pas l'objet de la spec — il a été trouvé
en revue, et il vaut d'être écrit : le périmètre réel d'un défaut d'ordonnancement
n'est pas celui qu'on lui prête au moment de l'ouvrir.

### Équivalence vérifiée — ESPAsyncWebServer 3.11.2

`_parseReqHeader()` renseigne `_authorization` / `_authMethod` **et** empile
chaque en-tête dans `_headers` pendant l'état `PARSE_REQ_HEADERS` ;
`_parseMultipartPostByte()` → `_handleUploadByte()` ne tourne qu'**ensuite**, et
**rien ne purge `_headers`** avant la fin de la requête. Les deux méthodes sont
donc lisibles dès `index == 0` et le restent jusqu'au fragment final. Le
commentaire « check if authenticated before calling the upload » de
`WebRequest.cpp:738` n'est suivi d'**aucun** contrôle en amont : la responsabilité
est bien à notre charge.

Les **trois** clients légitimes passent :

| Client | Méthode | Chemin |
|---|---|---|
| `ota_update.sh` (défaut) | `curl -u admin:<mdp>` | `checkBasicAuth` → `request->authenticate()`, lit `_authorization` |
| `ota_update.sh` (`POOL_TOKEN`) | `-H 'X-Auth-Token: …'` | `checkTokenAuth` → `getHeader()`, lit `_headers` |
| Page web d'upload | XHR + `X-Auth-Token` ([`data/app.js:7715`](../../../data/app.js:7715)) | idem |

Exemptions écartées à dessein : `checkRateLimit()` (ne peut que **refuser** —
l'omettre élargit, ne restreint pas) et l'exemption « premier démarrage » de
`checkAuth()` (bornée aux URL commençant par `/save-config`, elle n'a **jamais**
couvert `/update` — l'ajouter serait un trou).

## `/download-update` : audité SAIN — raison structurelle

Enregistrée en `server->on("/download-update", HTTP_POST, lambda)` à **trois**
arguments ([`src/web_routes_ota.cpp:1329`](../../../src/web_routes_ota.cpp:1329)) :
**aucun handler de corps, aucun handler d'upload**. Il n'existe donc aucun point
d'entrée exécuté avant le lambda de requête — `REQUIRE_AUTH(CRITICAL)` s'exécute
**nécessairement** avant tout accès à `Update`. Le défaut de `POST /update` ne peut
pas s'y produire.

Consigné pour **ne pas re-auditer la même route** : la question ne se rouvre que si
un 4ᵉ ou 5ᵉ argument (corps / upload) est ajouté à cet enregistrement.

## Deux justifications corrigées APRÈS revue

Elles sont consignées parce qu'elles instruisent : dans les deux cas le correctif
était bon et la **raison** invoquée était fausse. Une justification fausse survit
plus longtemps qu'un bug — elle est réutilisée.

1. **Le motif du throttle était surévalué.** `sendAuthRequired()`
   ([`auth.cpp:234`](../../../src/auth.cpp:234)) émet déjà **un `WARNING` non
   throttlé par requête** sur cette route. Le throttle **réduit la pression, il ne
   ferme pas le vecteur** : la fenêtre forensique reste purgeable sans
   authentification. Le sujet est l'émetteur **générique** →
   [`bug-112`](../todo/bug-112-journal-non-borne-sur-route-protegee.md). Le
   commentaire du code porte désormais l'avertissement de ne pas conclure le contraire.
2. **La justification de l'ordre des prédicats était inversée.**
   `authenticate()` court-circuite déjà sur `_authorization` vide : le chemin
   **jeton** n'allouait rien. C'est le chemin **basique** — celui d'`ota_update.sh`
   par défaut — qui paie `generateBasicHash()` (2 `new char[]` + une `String`) à
   **chaque** fragment, soit **≈ 1 000 fois pour un flash de 1,5 Mo**, dans la tâche
   réseau et en famine mémoire potentielle. **Aucun ordre n'évite ce coût**, la garde
   étant réévaluée par fragment à dessein : c'est un **coût assumé, pas un coût
   évité**. Allocations transitoires et LIFO, jugées non bloquantes en revue. Seul le
   court-circuit `hasHeader("X-Auth-Token")` reste justifié (strictement équivalent,
   et il évite ~1 000 lignes de `debug` pendant le flash).

## Verdicts

| Agent | Verdict |
|---|---|
| `code-reviewer` | **Approved** — code figé ; deux justifications à corriger (faites, ci-dessus) ; bénéfice `setOtaInProgress` relevé par lui |
| `test-engineer` | `pio run` OK, `pio test -e native` **sans régression** (aucune suite native possible : la garde dépend de `AsyncWebServerRequest`, hors périmètre ADR-0017) |
| `pool-chemistry` | **sans objet** — aucun chemin de dosage nouveau (l'impact est en revanche total : un firmware arbitraire contrôle les pompes) |

## Plan de vérification post-déploiement — ⚠️ NON EXÉCUTÉ

**L'appareil tourne encore la 2.45.0** : ce plan est écrit, pas joué. À exécuter au
prochain déploiement, dans cet ordre.

1. **Upload non authentifié** (`curl -X POST -F "update_type=firmware"
   -F "update=@firmware.bin" http://<IP>/update`, **sans** identifiants).
   Attendu : **`401`**. Puis dans `/get-logs` : **aucune** ligne « Type de mise à
   jour », **aucun** `Update.begin`, et **un seul** WARN
   `[OTA] POST /update NON AUTHENTIFIÉ`.
2. **Dans la minute qui suit** : `./ota_update.sh both <IP>` en authentification
   basique → **200** puis redémarrage. Puis **répéter en jeton** (`POOL_TOKEN`).

**Pourquoi l'étape 2 est immédiate, et pas remise à plus tard** : l'OTA est le
**seul** canal de déploiement de cet appareil (aucun port série depuis le passage
en conteneur LXC le 05/09). Une garde trop stricte le rendrait **non déployable —
donc non corrigeable — sans repli**. Enchaîner l'étape 2 fait découvrir un tel
blocage **à la seconde**, tant qu'un firmware sain est encore en place et que la
cause est évidente ; la reporter à la livraison suivante ferait découvrir le
blocage sans savoir quel changement l'a causé.

## Hors périmètre
- Le journal non borné (`bug-112`), le tamponnage JSON (`bug-113`).
- La refonte du mécanisme d'intégrité en signature cryptographique : instruite
  séparément, spec [`feature-118`](../doing/feature-118-authenticite-de-l-image-ota.md)
  (créée le 2026-09-07).

## Documentation produite

- [`docs/API.md`](../../../docs/API.md) — `POST /update` : le `401` dès l'upload,
  note structurelle sur `/download-update`.
- [`docs/subsystems/ota-manager.md`](../../../docs/subsystems/ota-manager.md) —
  chapitre « Authentification du handler d'UPLOAD ».
- [`docs/subsystems/logger.md`](../../../docs/subsystems/logger.md) — troisième site
  du throttle et sa limite.
- [`CHANGELOG.md`](../../../CHANGELOG.md) — section `[2.46.2]`.

**Aucun ADR** : le correctif **applique** un patron déjà décidé et déjà appliqué deux
fois (`saveConfigBodyAuthorized`, v2.25.0 ; `calibrationBodyAuthorized`, v2.46.1). Il
ne contraint rien de nouveau, n'a pas d'alternative crédible (l'alternative « laisser
flasher sans authentification » n'en est pas une) et ne verrouille aucune orientation.
La décision structurante de ce domaine — intégrité **ou** authenticité de l'image — est
justement celle qui reste ouverte, et c'est `feature-118` qui la portera.

**Aucun PDF, aucune capture d'écran** : vérifié, rien ne change pour un utilisateur
**légitime**. La page Mise à jour est inchangée (même formulaire, même XHR, même
jeton), et un utilisateur connecté n'atteint jamais le nouveau `401`. Seul un appelant
non authentifié voit une différence — il n'a pas d'écran dans cette documentation.
