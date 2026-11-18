# bug-184 — L'authentification HTTP Basic accepte un mot de passe qui n'est pas le bon, et ignore le blocage du premier démarrage

## Métadonnées
| | |
|---|---|
| Type | bug — 🔴 **sécurité, authentification** |
| Statut | **done** — ⭐ **déployé en 2.92.0 et vérifié sur l'appareil réel le 2026-11-17** |
| Priorité | **CRITIQUE** — ⚠️ deux défauts **mesurés**, sur la seule barrière d'accès d'un appareil **vendu à des clients** |
| Version cible | **2.92.0** |
| Relevé par | `security-reviewer` (passe P1 de [`bug-183`](../done/bug-183-auth-login-echecs-muets-et-comparaison-non-constante.md)), **vérifié et mesuré sur l'appareil** le 2026-11-17 |
| Specs liées | [`bug-183`](../done/bug-183-auth-login-echecs-muets-et-comparaison-non-constante.md) (⚠️ **mise en pause**, ce lot passe avant) · `feature-028` (comparaison à temps constant, v2.11.2) · [`bug-112`](../done/bug-112-journal-non-borne-sur-route-protegee.md) |

## Domaines touchés
- [ ] Capteurs — [ ] Régulation — [ ] Filtration / Éclairage — [ ] UART / écran
- [x] **Web — routes HTTP** (toutes les routes protégées)
- [x] **Authentification** (`src/auth.{h,cpp}`)
- [x] **Outillage** (`ota_update.sh`)
- [ ] MQTT — [ ] Historique — [ ] Frontend (`data/`) — ⚠️ à confirmer : l'interface n'utilise que le jeton

## Agents requis
- [x] spec-reader · [x] architect · [x] embedded-developer · [x] test-engineer · [x] code-reviewer · [x] doc-writer
- [x] **`security-reviewer`** — ⛔ **OBLIGATOIRE, P1 sur le design ET P2 sur le diff.** C'est lui qui a trouvé le défaut.
- [ ] `pool-chemistry` — ⚠️ **à reconsidérer** : la conséquence d'une authentification franchie est l'écriture des **consignes de dosage**. Son avis n'est pas requis sur le mécanisme, mais le périmètre d'impact est chimique.
- [ ] `ux-ui-designer` — sans objet si `data/` n'est pas touché.

## Description

`AuthManager::checkAuth()` accepte **deux** voies : le jeton `X-Auth-Token`, **ou** l'authentification HTTP Basic (`src/auth.cpp:288`, `checkTokenAuth(req) || checkBasicAuth(req)`). La seconde porte **deux défauts indépendants**, tous deux **vérifiés**.

## 🔴 DÉFAUT 1 — des identifiants qui ne sont pas les bons sont acceptés

`checkBasicAuth` (`src/auth.cpp:169-174`) délègue à `req->authenticate()`, qui aboutit dans la bibliothèque à :

```cpp
// .pio/libdeps/esp32dev_core3/ESPAsyncWebServer/src/WebAuthentication.cpp:24
return generateBasicHash(username, password).equalsIgnoreCase(hash);
```

⇒ La chaîne d'identifiants encodée (base64) est comparée **sans tenir compte de la casse**. Or le base64 **distingue** majuscules et minuscules : une variante de casse est un base64 **valide et différent**, qui décode en **octets différents**.

### ⭐ MESURÉ SUR L'APPAREIL DE L'UTILISATEUR — 2026-11-17, v2.91.0

| Requête `GET /get-system-info` | Réponse |
|---|---|
| `Authorization: Basic <base64 correct>` | `200` ✅ attendu |
| `Authorization: Basic <même base64, casse inversée>` | 🔴 **`200` — ACCEPTÉ** |
| `Authorization: Basic <identifiants faux>` | `401` ✅ attendu |

⛔ **Ce n'est pas une déduction : l'appareil a répondu `200` à une chaîne d'identifiants qui ne contient pas le mot de passe.**

**Portée réelle, à ne pas surestimer** : ⛔ ce n'est **pas** un contournement direct — l'attaquant doit toujours trouver une préimage dont le base64 est une variante de casse du bon. ⇒ L'effet est un **affaiblissement** : chaque lettre du base64 perd ~1 bit. Pour un mot de passe courant, de l'ordre de **15 à 20 bits**. ⚠️ Combiné à l'absence totale de trace sur les échecs ([`bug-183`](../done/bug-183-auth-login-echecs-muets-et-comparaison-non-constante.md)) et à un quota de 120 req/min, cela rend un essai en ligne **plus rapide et toujours invisible**.

ℹ️ Accessoirement, cette comparaison est aussi **en temps variable** et **révèle la longueur** — c'est la **troisième** comparaison de mot de passe du fichier, celle que `bug-183` n'avait pas vue.

## 🔴 DÉFAUT 2 — le blocage du premier démarrage est contourné

Au premier démarrage et **après remise à zéro par le bouton physique** (`src/auth.cpp:120-122` et `:325-327`), `adminPassword = "admin"` et `isFirstBoot = true`.

- `POST /auth/login` fait son travail : il rend **`403` « Changement de mot de passe obligatoire au premier démarrage »** (`src/web_routes_auth.cpp:64-67`).
- ⛔ **`checkAuth()` ne teste `isFirstBoot` nulle part** pour la voie Basic (`src/auth.cpp:275-296` — le seul test de `isFirstBoot` est une **autorisation** pour `/save-config`).

⇒ **`admin` / `admin` ouvre TOUTES les routes protégées**, y compris `CRITICAL` — c'est-à-dire l'écriture des **consignes de dosage** — pendant que l'interface affirme que c'est interdit.

⚠️ **`isFirstBoot` reste vrai jusqu'à l'appel de `clearFirstBootFlag()`** (fin de l'assistant de configuration, `src/auth.cpp:316-322`). Un appareil livré, alimenté, mais dont l'assistant n'a pas été terminé **reste dans cet état indéfiniment**.

🔴 **ET C'EST L'ÉTAT DANS LEQUEL LES CLIENTS REÇOIVENT L'APPAREIL.**

## ⚠️ Défaut 3, relevé au passage — ⛔ à ARBITRER, pas à corriger en silence

`src/auth.cpp:277-279` : pendant le premier démarrage, **toute** requête commençant par `/save-config` est autorisée **sans aucune authentification** (`return true` avant même le contrôle de quota). ⇒ À confirmer et à trancher dans ce lot : est-ce voulu (l'assistant doit pouvoir écrire avant qu'un mot de passe existe) ou est-ce un trou ?

## Comportement attendu

⭐ **La direction recommandée : SUPPRIMER la voie Basic.**

**Vérifié avant de le proposer** :
- l'interface web n'utilise **que** `X-Auth-Token` ;
- ⭐ **la page de secours OTA aussi** (`src/ota_recovery_page.html.in:273`) ⇒ **le chemin de réparation n'est pas menacé** ;
- seul `ota_update.sh` emploie Basic (`:360`, `:362`, `:368`), et il sait **déjà** parler jeton (`:364`, via `POOL_TOKEN`).

⇒ Le coût du retrait est **une évolution de `ota_update.sh`** : obtenir un jeton par `POST /auth/login` puis l'employer, au lieu d'envoyer les identifiants à chaque requête.

⚠️ **Si la voie Basic est CONSERVÉE** (décision à écrire), alors : comparaison **sensible à la casse**, **à temps constant**, **sans fuite de longueur**, **et** test de `isFirstBoot`. ⛔ Les quatre, ou aucun — un correctif partiel laisserait croire le sujet traité.

### Cas limites
- Appareil neuf / après remise à zéro : ⛔ aucune route protégée ne doit s'ouvrir avec le mot de passe d'usine.
- Assistant de configuration jamais terminé : ⇒ même exigence, indéfiniment.
- ⚠️ **Mise à jour d'un appareil en service** : si `ota_update.sh` change de méthode, la version qui l'accompagne doit encore pouvoir être **envoyée** à un appareil qui tourne l'**ancien** firmware. ⛔ Ne pas se couper la branche.
- Page de secours OTA : ⛔ **ne doit pas régresser** — c'est le dernier filet.

## Contraintes techniques
- ⛔ **Aucune solution exigeant un équipement chez le client** (proxy, VPN, certificat) : décision utilisateur du 2026-11-17, l'appareil est **vendu**.
- Le serveur reste en **HTTP clair** (même décision). ⇒ Ce lot ne protège pas de l'écoute, il protège de **l'émission** : l'attaquant qui peut envoyer des requêtes sans pouvoir lire le trafic.
- ⛔ Aucune allocation sur un chemin de journalisation (ADR-0082) ; ⛔ handler AsyncWebServer < 50 ms ; ⛔ pas de temporisation artificielle pour « égaliser » les temps.
- ⚠️ La bibliothèque `ESPAsyncWebServer` est un paquet **tiers** : ⛔ ne pas la patcher sur place (`.pio/libdeps/` est régénérable). Contourner par le code du projet.

## ✅ DÉCISIONS PRISES ET APPLIQUÉES — 2026-11-17

### A1 — La voie HTTP Basic est **SUPPRIMÉE**, pas durcie

⭐ **Décision : retrait.** `AuthManager::checkBasicAuth()` n'existe plus (`src/auth.h`, `src/auth.cpp`).
Il reste **une seule** preuve d'identité : l'en-tête `X-Auth-Token`.

**Pourquoi le retrait plutôt que le durcissement** : durcir exigeait de réimplémenter dans le
projet ce que fait la bibliothèque tierce (décodage base64, comparaison sensible à la casse, à
temps constant, sans fuite de longueur) — ⛔ les **quatre** points, sous peine de laisser croire
le sujet traité. Pour une voie que **personne n'utilise** : ni l'interface web, ni la page de
secours OTA, et le seul script qui l'employait sait déjà parler jeton. ⇒ Le coût du durcissement
était supérieur au coût du retrait, pour un bénéfice nul.

### A4 — L'exemption du premier démarrage est **CONSERVÉE**, mais **BORNÉE**

⭐ **Décision : conserver, et resserrer.** L'assistant de premier démarrage **doit** pouvoir écrire
la configuration avant qu'un mot de passe existe — c'est le seul moyen d'amener un appareil neuf
dans un état utilisable. Supprimer l'exemption rendrait l'appareil inconfigurable à la sortie du
carton.

**Ce qui a changé** — l'exemption portait sur `url().startsWith("/save-config")`, elle porte
maintenant sur l'URL **EXACTE** `/save-config`, **aux deux endroits** :
1. `AuthManager::checkAuth()` (`src/auth.cpp`) ;
2. `saveConfigBodyAuthorized()` (`src/web_routes_config.cpp`).

⭐ **Et c'est le second qui comptait.** Ce prédicat est **partagé** avec `/tune-ph-offset`
(`web_routes_config.cpp`, handler de corps) : un `isFirstBootDetected()` sans test d'URL ouvrait
donc **aussi** l'écriture de l'offset pH à n'importe qui sur le réseau, indéfiniment tant que
l'assistant n'était pas terminé — alors que l'assistant n'appelle **jamais** cette route (elle
exige 10 min de filtration et une calibration). ⇒ **Exemption involontaire, fermée.**

⚠️ **Ce que cette borne NE corrige PAS** : la fenêtre de premier démarrage elle-même ne se referme
que sur `/auth/complete-wizard`, et le jeton **n'est pas régénéré** au changement de mot de passe.
⇒ [`bug-185`](../done/bug-185-fenetre-de-premier-demarrage-ouverte-indefiniment.md), ouverte le
même jour. ⛔ **Ce lot ne rend pas le premier démarrage sûr.**

⚠️ **Et le quota n'a pas bougé pour le corps de `/save-config`** : il est admis par
`saveConfigBodyAuthorized()` **avant** tout décompte. ⛔ Écrire « `/save-config` est désormais sous
quota au premier démarrage » serait **faux** — le commentaire de `checkAuth()` le dit maintenant
explicitement.

### Conception — la décision d'admission devient un module PUR

`checkAuth()` ne décide plus : elle **collecte les faits** et délègue à `src/auth_admission_logic.h`
(`decideAdmission(AuthAdmissionInput) -> AuthVerdict{kAllow, kDenyUnauth, kDenyRateLimited}`),
testable en natif sans matériel (ADR-0017) — suite `test/test_native_auth_admission/`.

### `ota_update.sh` — un jeton, jamais d'identifiants

Un mot de passe est **échangé** contre un jeton par `POST /auth/login`, puis seul `X-Auth-Token`
est employé. ⚠️ `POST /auth/login` existe **aussi sur les anciens firmwares** ⇒ le script marche
sur un appareil ancien **et** neuf : la branche n'est pas coupée.

⛔ **Ni le mot de passe ni le jeton ne passent par la ligne de commande de `curl`** :
`/proc/<pid>/cmdline` est lisible par **tout** utilisateur de la machine, alors que
`/proc/<pid>/environ` ne l'est que par son propriétaire. ⇒ corps de connexion dans un fichier
`chmod 600`, jeton dans un fichier de configuration curl (`--config`) `chmod 600`, mot de passe
passé à `python3` par l'**environnement**.
⚠️ **Correction d'une affirmation antérieure** : il avait été dit que « le jeton n'est jamais en
argument de commande ». C'était **faux** avant ce correctif — `-H "X-Auth-Token: …"` **est** un
argument. Ça l'est maintenant.

### Garde CI mise à jour

`tools/check_unauth_log_guard.sh`, règle (5) : la liste ne contient plus `checkBasicAuth`
(fonction disparue). ⭐ **La garde a échoué d'elle-même** sur le retrait (`fonction
AuthManager::checkBasicAuth introuvable`) — elle a fait son travail.

## Livrables — check-list
- [x] A1 — Décision écrite : **retrait** de la voie Basic (section « Décisions » ci-dessus, [ADR-0130](../../../docs/adr/0130-l-appareil-n-accepte-que-le-jeton-pas-http-basic.md))
- [x] A2 — Aucune route protégée ne s'ouvre avec le mot de passe d'usine : ⭐ **par construction** — il n'existe plus aucun chemin qui compare un mot de passe dans `checkAuth()`. ⚠️ **Preuve sur l'appareil = D3, non faite** (exige un déploiement)
- [x] A3 — `ota_update.sh` fonctionne encore, **prouvé par un envoi réel** : firmware **ET** interface envoyés le 2026-11-17 à 15:2x vers un appareil qui tournait encore la **2.91.0**, signatures relues (`--use-sig`), aucune re-signature. ⭐ **Le script ne sait plus parler qu'en jeton et il a quand même déployé** — la branche n'était pas coupée
- [x] A4 — Arbitrage écrit sur `/save-config` anonyme au premier démarrage : **exemption conservée, bornée à l'URL exacte**, aux deux endroits (section « Décisions »)
- [x] A5 — Page de secours OTA : `src/ota_recovery_page.html.in` n'emploie que `X-Auth-Token`, **aucune ligne touchée par ce lot**
- [x] D1 — Tests natifs de la logique d'admission (`test/test_native_auth_admission/`) — **2599 cas verts**, validation par mutation : voir le compte rendu de `code-reviewer`
- [x] D2 — ⭐ **MESURÉ SUR L'APPAREIL, 2026-11-17, v2.92.0** : voir le tableau ci-dessous
- [x] D3 — `admin`/`admin` n'ouvre **aucune** route protégée : `/data`, `/get-system-info`, `/auth/token`, `/get-logs` rendent tous **`401`**. ⚠️ **Limite assumée** : l'appareil n'est **pas** en premier démarrage et ⛔ **je n'ai pas fait de remise à zéro** (appareil en service sur un bassin réel). La partie « premier démarrage » reste prouvée **par construction et par les tests natifs**, pas sur le terrain
- [x] C1 — `docs/API.md` (≈ 90 exemples `curl -u` convertis), `docs/subsystems/auth.md`, `sensors.md`, `ota-manager.md`, `ws-manager.md`, `UPDATE_GUIDE.md`, `page-settings.md`, `CHANGELOG.md` (2.92.0), **ADR-0130**, note datée en fin d'ADR-0127

### ⭐ D2 — LES TROIS REQUÊTES REJOUÉES SUR L'APPAREIL (2026-11-17, v2.92.0)

| Requête `GET /get-system-info` | Avant (2.91.0) | **Après (2.92.0)** |
|---|---|---|
| `Authorization: Basic <base64 correct>` | `200` | 🔒 **`401`** |
| `Authorization: Basic <même base64, CASSE INVERSÉE>` | 🔴 **`200`** | 🔒 **`401`** |
| `Authorization: Basic <identifiants faux>` | `401` | `401` |
| `X-Auth-Token: <jeton>` (témoin) | `200` | ✅ **`200`** |

⭐ **Le défaut d'origine est fermé, mesuré, pas déduit** : la chaîne à casse inversée — celle qui
ouvrait l'appareil sans contenir le mot de passe — est maintenant refusée. ⚠️ Et la chaîne
**correcte** l'est aussi : c'est voulu, la voie entière a disparu.

✅ **Aucun défi `WWW-Authenticate` sur le `401`** ⇒ pas de fenêtre de mot de passe du navigateur.

### Autres observations de terrain, même session

- ✅ **Journal de démarrage** : `INFO | Authentification activée (API Token X-Auth-Token uniquement)`.
- ✅ **Interface servie par l'appareil** : la nouvelle astuce est en place (3 occurrences de
  `tips__code--wrap`), ⛔ **plus un seul `curl -u`** dans la page.
- 🔴 **Trouvé par hasard pendant ces mesures** : `GET /get-config` répond **`200` sans aucun
  identifiant** — aucune garde dans le code (`web_routes_config.cpp:2411`). ⛔ **Sans rapport avec
  ce lot**, défaut préexistant. Les mots de passe et le jeton y sont masqués, mais toute la
  configuration sort. ⇒ Versé à [`bug-183`](../done/bug-183-auth-login-echecs-muets-et-comparaison-non-constante.md).

## 🔎 VERDICTS DES AGENTS — 2026-11-17

| Agent | Verdict | État |
|---|---|---|
| `security-reviewer` (P2, sur le diff) | **GO SOUS CONDITIONS** (2) | ✅ **les deux levées** |
| `code-reviewer` | **Request changes** (1 bloquant, 8 mineurs) | ✅ **tous traités** |
| `ux-ui-designer` | recommandations, 1 question bloquante | ✅ **tranchée et appliquée** |
| `doc-writer` | livré | ✅ + 4 faux qu'il a trouvés seul |

### ⭐ La condition qui comptait : le correctif avait créé un trou

🔴 `security-reviewer` a relevé que **mon propre correctif** de `ota_update.sh` introduisait une
faille **plus grave que celle qu'il fermait**. Pour sortir le jeton de la ligne de commande, je
l'écrivais dans un fichier de configuration `curl`. ⛔ **Un tel fichier porte UNE OPTION PAR
LIGNE** : un saut de ligne ou un guillemet dans le jeton referme l'en-tête et ouvre des lignes
d'options arbitraires (destination, fichier à lire, fichier à écrire).

⚠️ **Et le jeton vient du RÉSEAU, en clair** (réponse de `POST /auth/login`) : un faux appareil sur
le réseau local le choisit. ⇒ **Prise de contrôle de la machine de développement** — celle qui
détient la clé de signature OTA et dont la phrase de passe est saisie à chaque envoi.

⇒ **Correctif** : `auth_conf_from_token()` **refuse** tout jeton hors de `[A-Za-z0-9._-]`, **avant**
d'écrire quoi que ce soit, et sort en code 1. **Vérifié** : guillemet, antislash, saut de ligne,
`/` et chaîne vide sont tous refusés sans qu'aucun fichier ne soit créé ; un jeton hexadécimal de
32 caractères passe et produit le bon en-tête (mesuré contre un serveur local).

⚠️ **La leçon** : `code-reviewer` a mesuré qu'avec un guillemet, `curl` envoyait l'en-tête
**tronqué, en silence, sans erreur**. Un correctif de sécurité se vérifie comme n'importe quel
autre code.

### Les autres points levés

- **`data/index.html`** — l'interface enseignait encore `curl -u` sur la route qui signale la
  présence d'eau. ⚠️ Pas cosmétique : sans ce signal pendant 3 min, **le dosage se suspend**. Un
  intégrateur qui suivait l'astuce n'aurait jamais eu de régulation, et rien ne le lui aurait dit.
  ⇒ Astuce réécrite en **deux étapes** (obtenir un jeton, puis signaler).
- ⭐ **Découverte de `ux-ui-designer`, qui contredit ce que j'avais écrit** : il n'existe **aucun
  écran affichant le jeton** dans l'interface. ⛔ J'avais fait écrire « Paramètres → Système →
  Token API » dans quatre documents — **cet écran n'existe pas**. Vérifié par recherche dans tout
  `data/` : le seul chemin d'obtention est `POST /auth/login`. Les quatre mentions sont corrigées.
- **Défilement latéral** : à 390 px, ~34 à 36 caractères sont visibles dans un bloc de code — et
  c'est **l'URL** qui disparaissait. Nouveau modificateur `.tips__code--wrap` (repli **visuel**,
  le copier-coller reste exact), documenté dans `DESIGN_SYSTEM.md`. ⛔ Pas appliqué au bloc YAML,
  dont l'indentation porte du sens.
- **Mineurs appliqués** : commentaire faux de `src/config.h:313` ; message `401` d'envoi (le mot de
  passe a déjà été validé — c'est le **jeton** qui est refusé) ; dernier `curl -u` de
  `docs/subsystems/system-health.md` ; garde `nullptr` fail-closed sur les deux prédicats de corps
  qui ne l'avaient pas ; initialiseurs `= false` sur `AuthAdmissionInput` (fail-closed **par
  construction** si un champ est ajouté plus tard) ; dépendance `python3` **déclarée** comme
  l'est déjà `openssl` ; extraction du jeton qui ne tue plus le script sur un corps non-JSON ;
  message `403` qui oriente vers la **page de secours** et non vers une interface peut-être hors
  service.
- **Commentaire corrigé, pas supprimé** : `src/web_routes_ota.cpp` justifiait un court-circuit par
  un journal `debug` que `bug-112` avait **supprimé**. Le court-circuit est conservé (il évite
  ≈1000 appels par flash), son motif est réécrit.

### Mutation — 7 mutants posés, 7 tués

`code-reviewer` a muté `decideAdmission()` sept fois et la suite est morte à chaque fois.
⭐ Le mutant le plus utile : **permuter le quota et l'exemption de l'assistant** (retour à l'ordre
d'avant le lot) ⇒ 2 échecs. L'« ORDRE IMPOSÉ » annoncé dans l'en-tête du module est donc
**réellement tenu par un test**, pas seulement documenté.

⚠️ **Réserve honnête, relevée par l'agent lui-même** : l'oracle `expected()` de la suite est
structurellement le **miroir** du code. Ce qui ancre vraiment, c'est le **53** en dur (nombre
d'admissions sur 64 combinaisons, recalculé à la main : 32 + 16 + 5) et les propriétés C3/C4/C5.

### ⛔ Une erreur de ma part, relevée par `code-reviewer`

J'ai écrit dans son brief que `set -u` était actif dans `ota_update.sh`. **C'est faux** : seul
`set -e` l'est. ⚠️ Et c'est heureux — `${3}`, `${POOL_TOKEN}` et `${POOL_PASSWORD}` planteraient
tous les trois sous `set -u`. ⇒ Si quelqu'un l'ajoute un jour, il faudra passer ces trois-là en
`${…:-}`.

## Critères d'acceptation
- [x] Une variante de casse des identifiants rend **401** — ⭐ **MESURÉ SUR L'APPAREIL** (tableau D2)
- [x] `admin`/`admin` n'ouvre **aucune** route protégée — **mesuré** sur quatre routes. ⚠️ **ET CE N'EST PAS LA MÊME CHOSE QUE « le premier démarrage est sûr »** : `POST /auth/change-password` reste anonyme pendant cette fenêtre **et rend le jeton** ⇒ [`bug-185`](../done/bug-185-fenetre-de-premier-demarrage-ouverte-indefiniment.md)
- [x] `ota_update.sh` envoie firmware et interface **sans** Basic — ⭐ **ENVOI RÉEL RÉUSSI**, vers un appareil qui tournait l'**ancien** firmware
- [x] La page de secours OTA fonctionne toujours — `src/ota_recovery_page.html.in` **n'a pas une ligne touchée** par ce lot et n'emploie que `X-Auth-Token`. ⚠️ ⛔ **non rejouée sur l'appareil** : la déclencher exige de mettre l'appareil en panne
- [x] `pio run` sans nouveau warning ; **toutes** les gardes CI vertes (`check_release_artefacts.sh` excepté : il exige une signature de release)

## 📊 État mesuré à la clôture — 2026-11-17

| | |
|---|---|
| `pio run` | ✅ SUCCESS, **aucun warning** |
| `pio test -e native` | ✅ **2599 cas**, tous verts |
| Gardes `tools/check_*.sh` | ✅ toutes vertes (hors release) |
| `./tools/check_doc_links.sh` | ✅ **6829 liens**, relancé **APRÈS** le déplacement vers `done/` |
| `./build_fs.sh` | ✅ reconstruit (`data/` modifié) |
| Version | **2.92.0** |

✅ **DÉPLOYÉ ET VÉRIFIÉ SUR LE TERRAIN le 2026-11-17** : l'appareil tourne la **2.92.0**
(`build Nov 17 2026 14:56:38`), firmware et interface. ⛔ **Rien n'est commité ni poussé.**

⚠️ **Ce que la clôture de cette fiche ne dit PAS** : que l'authentification de l'appareil est
saine. Deux défauts **vérifiés** restent ouverts — la fenêtre de premier démarrage
([`bug-185`](../done/bug-185-fenetre-de-premier-demarrage-ouverte-indefiniment.md), qualifiée
« bloquante pour la vente ») et `GET /get-config` sans aucune garde
([`bug-183`](../done/bug-183-auth-login-echecs-muets-et-comparaison-non-constante.md)).

## Notes pour agent
- ⚠️ **Les trois défauts ont été VÉRIFIÉS, deux d'entre eux MESURÉS sur l'appareil réel.** ⛔ Ne pas les re-chercher ; les re-mesurer **après** correctif, oui.
- ⚠️ **Ce lot passe AVANT [`bug-183`](../done/bug-183-auth-login-echecs-muets-et-comparaison-non-constante.md)** (décision utilisateur, 2026-11-17). ⇒ `bug-183` traitera **ensuite** les deux autres comparaisons et la journalisation des échecs. ⛔ Ne pas les traiter ici sans arbitrage : les deux lots se recouvriraient.
- ⚠️ `bug-183` porte déjà un relevé à jour du code (section « ✅ RELEVÉ DANS LE CODE COURANT — 2026-11-17 ») : **le lire**, il évite de refaire le travail.

## Hors périmètre
- Les comparaisons de `/auth/login` (`:58`) et de `/auth/change-password` (`:117`) → [`bug-183`](../done/bug-183-auth-login-echecs-muets-et-comparaison-non-constante.md).
- La journalisation des échecs de connexion → [`bug-183`](../done/bug-183-auth-login-echecs-muets-et-comparaison-non-constante.md).
- Le chiffrement des échanges → ⛔ **tranché le 2026-11-17 : on reste en clair.**
- `rateLimitMap` → `feature-131`. Routes `/debug/*` → [`feature-070`](../todo/feature-070-durcissement-routes-debug-et-cooldown-reset.md).
