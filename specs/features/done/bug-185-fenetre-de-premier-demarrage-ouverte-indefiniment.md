# bug-185 — La fenêtre de premier démarrage reste ouverte indéfiniment, le jeton qu'on y prend ne s'invalide jamais, et un inconnu peut annuler l'obligation de changer le mot de passe

## Métadonnées
| | |
|---|---|
| Type | bug — 🔴 **sécurité, authentification** |
| Statut | **done** — déployé en **2.93.0** le 2026-11-17 ; ⚠️ 5 essais de terrain **repoussés** (voir `bug-186`) |
| Priorité | **CRITIQUE** — ⚠️ qualifiée **« bloquante pour la vente »** par `security-reviewer` (P2 de [`bug-184`](../done/bug-184-authentification-basic-accepte-un-mot-de-passe-qui-n-est-pas-le-bon.md)) |
| Version cible | **2.93.0** (déployée le 2026-11-17) |
| Relevé par | `security-reviewer`, 2026-11-17, passe P2 de `bug-184` — **vérifié dans le code** le même jour |
| Specs liées | [`bug-184`](../done/bug-184-authentification-basic-accepte-un-mot-de-passe-qui-n-est-pas-le-bon.md) (⚠️ ce lot ne le corrige PAS) · [`bug-183`](../todo/bug-183-auth-login-echecs-muets-et-comparaison-non-constante.md) |

## Domaines touchés
- [x] **Web — routes HTTP** · [x] **Authentification** (`src/auth.{h,cpp}`, `src/web_routes_auth.cpp`)
- [ ] Capteurs — [ ] Régulation — [ ] Filtration — [ ] MQTT — [ ] OTA — [ ] Frontend
- ⚠️ **Conséquence** : `/save-config` écrit les **consignes de dosage**.

## Agents requis
- [x] spec-reader · [x] architect · [x] embedded-developer · [x] test-engineer · [x] code-reviewer · [x] doc-writer
- [x] **`security-reviewer`** — ⛔ OBLIGATOIRE, P1 sur le design **et** P2 sur le diff.
- [ ] `ux-ui-designer` — ⚠️ **à cocher** si le correctif impose un geste à l'utilisateur (preuve de présence physique).
- [ ] `pool-chemistry` — pas sur le mécanisme ; ⚠️ le périmètre d'impact est chimique.

## Description

Tant que `isFirstBoot` est vrai, **n'importe qui sur le réseau local** peut prendre le contrôle complet de l'appareil. Et cet état **ne se termine pas tout seul**.

## Les quatre faits, vérifiés dans le code

1. **`POST /auth/change-password` est anonyme et rend le jeton.** `src/web_routes_auth.cpp:99-104` : si `isFirstBoot`, ⛔ **aucune authentification n'est demandée**. La réponse contient `response["token"] = authManager.getApiToken()` (`:158`).
2. **`POST /save-config` est anonyme.** `saveConfigBodyAuthorized` (`src/web_routes_config.cpp:545-549`) rend `true` dès `isFirstBootDetected()`, ⛔ **sans contrôle de quota**. ⇒ Les consignes de dosage sont écrites sans identité.
3. 🔴 **Le jeton n'est JAMAIS régénéré.** Ni au changement de mot de passe (`web_routes_auth.cpp:144` appelle `setPassword()`, ⛔ **pas** `regenerateApiToken()` — qui **existe pourtant**, `src/auth.h:56`), ni à la clôture de l'assistant (`AuthManager::clearFirstBootFlag()`, `src/auth.cpp:316-322`).
   ⇒ ⛔ **Un jeton pris pendant la fenêtre reste valide APRÈS que le propriétaire a choisi un mot de passe fort.** Changer le mot de passe ne ferme rien.
4. **Seul `/auth/complete-wizard` referme la fenêtre.** ⇒ Un appareil **livré, alimenté, mais dont l'assistant n'a pas été terminé** reste ouvert **indéfiniment**.

## 🔴 Pourquoi c'est bloquant pour la vente

⭐ **C'est l'état dans lequel les clients reçoivent l'appareil.** Et il y revient après toute **remise à zéro par le bouton physique** (`src/auth.cpp:325-327`).

⚠️ **`bug-184` ne le corrige pas.** Son critère A2 (« aucune route protégée ne s'ouvre avec le mot de passe d'usine ») est tenu **à la lettre** — il n'y a plus de mot de passe d'usine exploitable — ⛔ **mais pas dans l'esprit** : la fenêtre de premier démarrage ouvre tout sans mot de passe du tout.

## ✅ RELEVÉ DANS LE CODE COURANT — 2026-11-17, APRÈS la livraison de `bug-184` (v2.92.0)

⚠️ **Relevé refait depuis zéro** : la fiche a été écrite avant `bug-184`, et `bug-184` a modifié
`auth.cpp` et `web_routes_config.cpp`. Consigne utilisateur : *« étudie quand même chaque feature
ou bug avant de les traiter parce que l'application a évolué »*.

**Les quatre faits sont CONFIRMÉS.** ⚠️ Mais **trois numéros de ligne sur quatre avaient bougé** —
voici les vrais :

| Fait | Annoncé | **Réel au 2026-11-17** |
|---|---|---|
| 1. `/auth/change-password` anonyme en premier démarrage | `web_routes_auth.cpp:99-104` | **`:80`** (route), **`:103`** (test `isFirstBoot`), **`:158`** (jeton rendu) |
| 2. `/save-config` anonyme, sans quota | `web_routes_config.cpp:545-549` | **`:563-570`** ⚠️ **modifié par `bug-184`** : désormais borné à l'URL **exacte**, mais **toujours anonyme et toujours sans quota** |
| 3. Le jeton n'est jamais régénéré | `:144` / `auth.h:56` | **confirmé** : `setPassword()` à `:144`, et `regenerateApiToken()` n'est appelée **que** depuis `/auth/regenerate-token` (`:169`). ⛔ Ni dans `change-password`, ni dans `clearFirstBootFlag()` |
| 4. Seul `/auth/complete-wizard` referme | `auth.cpp:316-322` | **confirmé**, route à **`:210`** |

## 🔴 FAIT 5 — TROUVÉ AUJOURD'HUI, et il est PIRE que les quatre autres

⛔ **`POST /auth/complete-wizard` n'a AUCUNE garde** (`src/web_routes_auth.cpp:209-220` : ni
`REQUIRE_AUTH`, ni `checkAuth`, ni jeton — **vérifié, zéro occurrence**). Le commentaire au-dessus
l'assume : *« PUBLIC : doit rester accessible sans token pour couvrir le cas où l'utilisateur
revient sur le wizard après interruption »*.

⇒ **N'importe qui sur le réseau local peut refermer la fenêtre de premier démarrage** — **sans
avoir changé le mot de passe**.

🔴 **Et voici ce que ça produit**, en lisant `/auth/login` (`:57-70`) :
1. `complete-wizard` met `isFirstBoot = false`. ⛔ Le mot de passe, lui, **vaut toujours `admin`**.
2. Or le refus `403 « Changement de mot de passe obligatoire »` ne s'applique **que** si
   `isFirstBoot` est vrai.
3. ⇒ `POST /auth/login` avec **`admin` / `admin`** rend désormais **un jeton valide**. Pour
   toujours.

⭐ **L'obligation de changer le mot de passe peut donc être ANNULÉE par un inconnu**, et l'appareil
reste **définitivement sur son mot de passe d'usine** — pendant que son propriétaire, lui, ne voit
plus l'assistant mais un écran de connexion, et n'a aucune raison de soupçonner quoi que ce soit.

⚠️ **Ce fait change la conception** : régénérer le jeton (axe 1) ne suffit plus. Tant que
`complete-wizard` est ouverte, fermer la fenêtre ne ferme rien — ⛔ **ça empire** même, puisque ça
débloque `/auth/login` sur le mot de passe d'usine.

## ➕ AJOUT AU PÉRIMÈTRE — `GET /get-config` sans aucune authentification

⛔ **Mesuré sur l'appareil le 2026-11-17** (v2.92.0) : `GET /get-config` répond **`200` sans le
moindre identifiant**. Code : `src/web_routes_config.cpp:2411` —
`server->on("/get-config", HTTP_GET, handleGetConfig);`, ⛔ **ni `REQUIRE_AUTH`, ni
`RouteProtection`**. Ce n'est pas une route déclarée publique : c'est une route **oubliée**.

⚠️ **Comparaison qui situe l'écart** : `GET /data` (les mesures) rend `401`. La **configuration
entière** est donc plus exposée que les **mesures**.

**Ce qui sort** : nom du réseau Wi-Fi, identifiant MQTT, nom mDNS, et toute la configuration de
régulation (consignes, limites journalières et horaires, débits, horaires de filtration).
✅ Mot de passe, mot de passe MQTT et jeton sont **masqués** (`******`).
⇒ Pas une prise de contrôle : une **divulgation**, sans la moindre trace (aucune garde ⇒ aucun
journal, aucun quota).

⭐ **ET LA QUESTION QUI BLOQUAIT EST TRANCHÉE** : fermer cette route **ne casse rien**. Vérifié —
l'interface ne l'appelle qu'à **deux** endroits (`data/app.js:4210` et `:15428`), **tous deux via
`authFetch()`**, qui joint déjà le jeton (`data/app.js:606-612`). ⛔ Et `login.html`,
`wizard.html` et `wifi.html` **ne l'appellent jamais** — c'était le vrai risque, il n'existe pas.
⇒ **Une ligne de code.**

ℹ️ ⛔ **Sans rapport avec `bug-184`** : défaut préexistant, relevé en vérifiant ce lot-là sur
l'appareil. Versé ici plutôt que dans `bug-183` parce qu'il décrit le **même scénario** — un
appareil qui arrive chez un client — et qu'il partage le **même déploiement**.

## 🔴 FAIT 6 — TROUVÉ PAR `security-reviewer` (P1), VÉRIFIÉ LIGNE PAR LIGNE : le pire de tous

⛔ **Le corps anonyme de `/save-config` écrit aussi les réglages d'AUTHENTIFICATION et le courtier
MQTT.** Vérifié dans le code courant :

| Ce qu'une seule requête anonyme écrit | Ligne |
|---|---|
| 🔴 `auth_enabled` → `authManager.setEnabled(false)` — **l'authentification s'éteint**, durablement | `web_routes_config.cpp:1591-1594` |
| `auth_password` → `setPassword()` (⛔ sans longueur minimale sur ce chemin) | `:1605-1611` |
| **Le courtier MQTT** : `server`, `port`, `topic`, `username`, `password` | `:1045-1058` |
| Persistance de l'ensemble | `:1707` (`saveMqttConfig()`) |

⇒ Et `decideAdmission()` (`src/auth_admission_logic.h`) rend `kAllow` **pour tout** dès que
`authEnabled` est faux. ⛔ `otaUploadAuthorized()` aussi.

⚠️ **Et le courtier MQTT pilote la RÉGULATION** : `ph_target/set`, `orp_target/set`,
`ph_regulation_mode/set`, `orp_regulation_mode/set`, filtration, éclairage
(`src/mqtt_manager.cpp:234-269`).

🔴 **Le scénario le plus discret n'a AUCUN symptôme** : authentification coupée + courtier
substitué, en **deux** requêtes. Le propriétaire ouvre l'appareil et voit un tableau de bord
normal — ni assistant, ni écran de connexion. ⛔ **Il ne saura jamais qu'il n'a pas de mot de
passe.** ⚠️ Et changer le mot de passe **ne reprend pas la main** : le courtier reste branché.

✅ **Seule borne réelle** : la signature OTA reste exigée, et l'armement du mode non signé est
refusé quand l'authentification est coupée (`src/web_routes_ota_switch.cpp:59`).

## ✅ DÉCISIONS — 2026-11-17

### Axe 3 (preuve de présence physique) — ⭐ DÉCISION UTILISATEUR : **AUCUNE**

> *« Rien de plus »* — utilisateur, 2026-11-17.

⇒ **Aucun geste nouveau n'est imposé au client au déballage.** L'appareil reste amorçable tel
quel ; le premier mot de passe choisi ferme l'accès anonyme.

⚠️ **Risque résiduel ASSUMÉ, écrit noir sur blanc** : entre la mise sous tension d'un appareil
neuf et le choix du mot de passe, **le premier arrivé sur le réseau gagne**. ⛔ Ce lot ne ferme
PAS « quiconque est sur le réseau local ». Ce qu'il garantit, c'est que l'attaque devient
**visible** (l'assistant du propriétaire est refusé) et **réversible** (appui long sur le bouton).

ℹ️ ⭐ **Porte laissée ouverte pour plus tard, sans coût aujourd'hui** : l'utilisateur confirme que
**chaque appareil vendu porte déjà une étiquette avec son code** — le mot de passe du point
d'accès, tiré au hasard au premier démarrage et que le firmware demande déjà de noter
(`src/auth.cpp:104-115`). ⇒ Si le risque résiduel devenait inacceptable, faire de ce code le mot
de passe de départ (à la place de `admin`) coûterait ~15 lignes et **aucun matériel**. Fiche à
ouvrir le jour où.

### Conception retenue — l'état se déduit du MOT DE PASSE, pas d'un drapeau

⭐ **Invariant unique** : *un droit anonyme n'existe que si aucun mot de passe n'a jamais été
choisi, et il se limite à en choisir un.*

⛔ **Pourquoi pas le drapeau NVS `wizardCompleted`** : c'est précisément ce qu'un inconnu peut
poser (fait 5). Un état de sécurité ne peut pas dépendre d'un bit que l'attaquant contrôle.

| État | Droits anonymes | On en sort par |
|---|---|---|
| **Vierge** (mot de passe d'usine) | `POST /auth/change-password` **seulement** | le premier changement réussi |
| **Possédé** | ⛔ aucun | — |

⭐ **Effet de bord précieux : la migration RÉPARE les appareils déjà piégés.** Un appareil livré
dont un tiers a refermé la fenêtre (`wizardCompleted` vrai **et** mot de passe `admin`) rouvre son
assistant à la mise à jour, au lieu de rester sur `admin`/`admin` à vie.

### A4 — `/save-config` anonyme : **FERMÉ**, exemption SUPPRIMÉE

⚠️ **Arbitrage entre deux agents.** `architect` proposait de la conserver (« la fenêtre se referme
désormais bien plus tôt ») ; `security-reviewer` exigeait sa suppression.

⇒ **Tranché pour la suppression**, et voici le fait qui décide : le fait 6 se joue en **UNE
requête**. Une fenêtre « plus courte » reste une fenêtre de plusieurs **semaines** pour un appareil
alimenté en stock chez un revendeur.

✅ **Et la fermer ne casse RIEN — vérifié moi-même, pas repris d'un rapport** : le seul appel
anonyme de l'assistant est `/auth/change-password` (`data/wizard.html:1182`), qui lui rend le jeton
(`:1202`) ; **toutes** ses écritures passent ensuite par `authFetch` (`:1281`, `:1317`). La page de
secours OTA ne contient **aucune** occurrence de `/save-config`. Tous les autres appelants
(`data/app.js:2007`, `:2024`) sont en `authFetch`.

⇒ Les champs `firstBoot` et `isSaveConfigUrl` de `AuthAdmissionInput` (bug-184) **disparaissent** —
⛔ ne pas les laisser morts, ce serait un piège. La suite `test_native_auth_admission` est mise à
jour en conséquence.

### A6 — `GET /get-config` passe en `RouteProtection::WRITE`

⚠️ **Elle n'était pas « oubliée »** : son commentaire la déclarait publique « pour premier
démarrage » (`web_routes_config.cpp:115-119`). ⇒ **La justification est périmée** : ni
`wizard.html`, ni `login.html`, ni la page de secours ne l'appellent.
ℹ️ ⭐ **Et `docs/API.md` la déclarait DÉJÀ `WRITE`** : c'est le code qui rejoint la documentation.
⛔ Pas `CRITICAL` : ce serait incohérent avec `/data`, `/get-logs` et `/get-system-info`.

### Rotation unique du jeton à la mise à jour

⚠️ **Sans elle, ce lot ne tient pas sa propre promesse** sur les appareils **déjà livrés** : un
jeton pris pendant leur fenêtre de premier démarrage resterait valide après la mise à jour.
⇒ **Coût : chacun se reconnecte une fois.** ⛔ Ne touche ni MQTT, ni Home Assistant.

### ⚠️ Deux corrections à CETTE fiche

1. ⛔ Le mot de passe du point d'accès **n'est pas dérivé du chipID** : il est **tiré au hasard**
   (8 caractères, `src/auth.cpp:104-115`), stocké en NVS, et **journalisé en clair à dessein** —
   « Notez-le sur une etiquette ! ». ⚠️ Il **survit** à la remise à zéro.
2. ⛔ `resetPasswordToDefault()` (`src/auth.cpp:320`) est **du code mort** : aucun appelant. Le
   bouton physique passe par l'effacement complet de la NVS puis un redémarrage. ⇒ La référence
   `auth.cpp:325-327` de cette fiche était **fausse**. (⛔ Ne pas supprimer cette fonction dans ce
   lot : hors périmètre.)

### ⭐ CORRECTIF G1 — l'impasse que ma première correction avait créée

`security-reviewer` a rendu **NO GO** en passe P2 : ma correction était juste **côté appareil**,
incomplète **côté pages**. Un appareil « possédé, assistant interrompu » (mot de passe réel,
assistant jamais fini) répondait toujours `firstBoot: true` ⇒ les pages proposaient le formulaire
**anonyme**, que l'appareil refuse désormais. ⛔ **La page de secours OTA devenait une impasse** —
le dernier filet de réparation. ⚠️ Et c'était une **régression** : en 2.92.0 cet état fonctionnait,
par le trou qu'on venait justement de fermer.

⛔ **Je n'ai PAS pris le correctif proposé** (collapser `firstBoot` sur `factoryPassword`) : vérifié,
il rendait `wizardCompleted` **mort** et la branche de fermeture de `/auth/complete-wizard`
**inatteignable**. ⇒ ⭐ **J'ai séparé les deux notions au lieu de les fusionner** :
`GET /auth/status` publie une clé **additive** `virgin`, et les **deux** points de branchement
clients (il n'y en avait que deux) passent dessus.

| Page | Repli sur `firstBoot` ? | Pourquoi |
|---|---|---|
| `src/ota_recovery_page.html.in` | ⛔ **non** | embarquée dans le firmware qui sert la clé : un repli serait du code mort, dans le dernier filet |
| `data/login.html` | ✅ **oui** | le système de fichiers se déploie séparément, il peut tourner sur un firmware antérieur |

### ⚠️ Ce que `security-reviewer` classe « acceptable » et qu'il faut quand même savoir

L'état « possédé, assistant interrompu » **n'a plus de sortie par l'interface** : `firstBoot` y
reste vrai indéfiniment ⇒ le point d'accès `PoolControllerAP` **se rallume à chaque démarrage** et
**son mot de passe est journalisé en clair à chaque fois**.
ℹ️ **Portée réelle** : en 2.93.0 cet état **ne naît plus** (le changement de mot de passe ferme la
fenêtre dans la même écriture) — il ne subsiste que par **migration** d'un appareil antérieur.
⇒ Fiche de suivi à ouvrir, ⛔ pas à traiter ici.

### ⚠️ Questions laissées OUVERTES — ⛔ ne pas les traiter ici sans arbitrage

1. **`auth_enabled` n'a aucun réglage dans l'interface** (zéro occurrence dans `data/`) : option
   réservée à l'API, ou à retirer ? ⇒ à décider plus tard.
2. **Appareil déjà livré dont l'authentification serait coupée** : la mise à jour doit-elle la
   réactiver d'office ? ⛔ **Non traité ici** — réactiver de force est un changement surprenant et
   difficile à annuler.
3. **Plafonds journaliers sans borne visible** (`web_routes_config.cpp:1476-1477`) ⇒ relevé par
   `security-reviewer`, à passer par `pool-chemistry`, fiche à ouvrir.
4. **Qualité de l'aléa au démarrage** (jeton et code du point d'accès tirés très tôt) ⇒ fiche à
   ouvrir.

## Comportement attendu — ⚠️ à CONCEVOIR, rien n'est tranché

Trois axes, ⛔ à arbitrer, **pas** à présumer :
1. ⭐ **Régénérer le jeton** à la clôture de l'assistant **et** à tout changement de mot de passe. La méthode existe (`regenerateApiToken()`), elle n'est pas appelée. ⇒ C'est le correctif le moins cher et le plus évident ; ⚠️ il invalide les sessions ouvertes — **dire ce que voit l'utilisateur**.
2. **Borner la fenêtre** : dans le temps ? au premier changement de mot de passe réussi ? ⚠️ Attention à ne pas créer d'appareil inadministrable — c'est le piège déjà rencontré sur `bug-184`.
3. **Exiger une preuve de présence physique** pour l'amorçage (appui sur le bouton, code affiché, point d'accès dédié). ⭐ C'est la seule réponse qui ferme vraiment « quiconque sur le réseau local », ⛔ mais c'est une **décision produit** : elle change l'expérience d'installation du client.

### Cas limites
- Assistant jamais terminé (appareil alimenté en stock, chez le revendeur, chez le client avant installation).
- Remise à zéro par le bouton physique.
- ⛔ **Ne pas créer d'impasse** : un appareil doit rester administrable à la sortie du carton.
- ⚠️ Appareils **déjà livrés** : un correctif ne les atteint que par mise à jour — et leur jeton actuel pourrait déjà être compromis.

## Contraintes techniques
- ⛔ Aucune solution exigeant un équipement chez le client (décision du 2026-11-17).
- Serveur en **HTTP clair** (même décision) ⇒ ce lot protège de **l'émission**, pas de l'écoute.
- ⛔ Aucune allocation sur le chemin de journalisation (ADR-0082) ; handler AsyncWebServer < 50 ms.
- ⛔ `/auth/status` : contrat de sondabilité (bug-177), ne pas toucher.

## Livrables — check-list
- [x] A1 — Décision écrite sur les trois axes : **axe 3 = AUCUNE preuve de présence** (décision utilisateur) ; axes 1 et 2 absorbés par l'invariant « l'état se déduit du mot de passe »
- [x] A0 — `/auth/complete-wizard` : `REQUIRE_AUTH(WRITE)` en **première instruction** + `409` si mot de passe d'usine
- [x] A6 — `GET /get-config` : `REQUIRE_AUTH(WRITE)` en première instruction de `handleGetConfig` ⭐ le code rejoint `docs/API.md`, qui la déclarait déjà `WRITE`
- [x] A7 — `/auth/login` refuse le mot de passe d'usine **quel que soit** l'état de l'assistant (ferme le fait 5)
- [x] A8 — ⛔ **`/save-config` : exemption anonyme SUPPRIMÉE**, aux deux endroits (ferme le fait 6). Les champs `firstBoot`/`isSaveConfigUrl` de `AuthAdmissionInput` ont disparu
- [x] A9 — Rotation **unique** du jeton à la mise à jour : `kAuthEpoch` (`src/constants.h`) contre le champ persisté `authEpoch`
- [x] A2 — Le jeton est régénéré à **tout** changement de mot de passe (`planAfterPasswordChange` ⇒ `rotateToken` toujours vrai), **et** recopié dans `authCfg` ⛔ sans quoi l'ancien ressusciterait au redémarrage
- [ ] A3 — Un jeton obtenu pendant la fenêtre cesse de fonctionner ⇒ **couvert par le code** (rotation à tout changement de mot de passe), ⛔ **non prouvé sur l'appareil** (= D2)
- [x] A4 — **FERMÉ**, décision écrite et arbitrée entre deux agents (voir « A4 » ci-dessus)
- [x] A5 — L'appareil reste administrable : état vierge ⇒ `/auth/change-password` anonyme ; état possédé ⇒ `/auth/login` avec le mot de passe choisi, **assistant terminé ou non**. ⚠️ Démontré par **lecture et tests natifs** ; ⛔ pas encore par un essai réel
- [x] D1 — `test/test_native_auth_bootstrap/` — **2610 cas verts** au total. Mutations : 7 par l'implémenteur + **2 posées par moi** (remettre `firstBoot` dans `decideLogin` ⇒ 2 échecs ; droit anonyme inconditionnel ⇒ 3 échecs), fichier restauré et empreinte vérifiée
- [~] D2 — ⭐ Essai réel — **2.93.0 déployée le 2026-11-17** (firmware + système de fichiers, signatures relues par `--use-sig`). **4 points mesurés, 5 en attente d'un appareil en état vierge** (voir « Relevé du 2026-11-17 » plus bas). Rappel de la contrainte : ⛔ **ENVOYER FIRMWARE *ET* SYSTÈME DE FICHIERS** (`ota-all`) : l'écran de connexion branche sur la clé `virgin`, neuve. La liste à rejouer :
  1. jeton pris pendant la fenêtre ⇒ **ne fonctionne plus** après le changement de mot de passe ;
  2. `admin`/`admin` ⇒ **403**, y compris après un `complete-wizard` déclenché de l'extérieur ;
  3. `GET /get-config` sans jeton ⇒ **401** ;
  4. **redémarrer**, puis rejouer (1) — ⭐ c'est le seul essai qui attrape un jeton mal persisté ;
  5. **page de secours OTA** en état vierge **et** en « assistant interrompu » ⇒ les deux doivent aboutir à l'écran d'envoi ;
  6. ⭐ **`login.html` en « assistant interrompu »** ⇒ doit afficher le formulaire de **connexion**, ⛔ pas rediriger vers l'assistant (c'est le défaut G1, corrigé) ;
  7. parcours d'assistant complet après un appui long sur le bouton.

### Relevé du 2026-11-17 — appareil réel 192.168.1.48, 2.93.0

| Essai | Mesuré | Verdict |
|---|---|---|
| 3. `GET /get-config` sans jeton | **401** (avec jeton : **200**) | ✅ le trou de bug-184 est fermé |
| 2bis. `POST /auth/complete-wizard` sans jeton | **401** | ✅ le fait 5 est fermé : un tiers ne referme plus la fenêtre |
| 4. jeton pris **avant** redémarrage, rejoué **après** | **200** sur `/get-config` et `/get-system-info` | ✅ `apiToken` **et** `auth_epoch` persistent : la rotation d'époque ne se rejoue pas à chaque démarrage |
| A9. rotation unique à la montée de version | ⭐ **prouvée par accident** : l'envoi du système de fichiers a été **refusé** (« Jeton refusé par l'appareil ») juste après la montée du firmware, parce que celui-ci venait de renouveler le jeton. Un second jeton a permis l'envoi. | ✅ observé en vrai |

**Journal** : 0 erreur, `Authentification activée (API Token X-Auth-Token uniquement)`.

⚠️ **Le message « SÉCURITÉ : jeton d'API renouvelé » n'a PAS pu être relu** : la fenêtre du journal tient une minute, et deux redémarrages ont eu lieu depuis. Le refus d'envoi ci-dessus en est la preuve directe.

#### ⛔ Les 5 essais NON faits, et pourquoi

Les essais **1, 2 (branche 403), 5, 6 et 7** exigent tous un appareil **en état vierge ou « assistant interrompu »**. Sur l'appareil de production, cet état ne s'obtient qu'en **effaçant la mémoire de configuration** — étalonnages, consignes, programmation de filtration compris. ⛔ Non fait : décision de l'utilisateur, pas d'un agent.

⚠️ **Nuance mesurée sur l'essai 2** : `admin`/`admin` rend **401**, pas 403 — et c'est correct. Le mot de passe de cet appareil **n'est pas** celui d'usine, donc le refus tombe sur les identifiants (`kDenyCredentials`) avant d'atteindre la branche « mot de passe d'usine » (`kDenyMustChangePassword`, 403). ⛔ **La branche 403 n'est donc PAS prouvée sur appareil** — seulement en natif (`test_login_factory_never_allows`).

⇒ Ces 5 points demandent soit un **second appareil** sacrifiable, soit une remise à zéro acceptée de celui-ci.

#### ⭐ DÉCISION UTILISATEUR DU 2026-11-17 — essai REPOUSSÉ, pas abandonné

L'utilisateur fera ces 5 essais **avec un appareil réinitialisé, dans quelques mois** (horizon
début 2027) : d'autres chantiers passent avant. ⛔ **Le lot n'attend pas** — il est déployé et les
4 points mesurables l'ont été.

⚠️ **Ce qui reste donc NON PROUVÉ sur matériel jusque-là** — à relire avant toute livraison
client, ⛔ ne pas présenter ces points comme acquis :
1. un jeton pris pendant la fenêtre d'installation cesse de fonctionner après le choix du mot de passe ;
2. la branche **403** « mot de passe encore celui d'usine » ;
3. la page de secours OTA en état vierge **et** en « assistant interrompu » ;
4. `login.html` en « assistant interrompu » (c'est le défaut **G1**, corrigé mais non revu en vrai) ;
5. le parcours d'assistant complet après un appui long sur le bouton.

⇒ Suivi ouvert dans **`bug-186`** pour que l'échéance ne se perde pas dans une fiche close.

- [x] C1 — `docs/API.md`, `docs/subsystems/auth.md`, `CHANGELOG.md` (2.93.0), **ADR-0131**, `docs/UPDATE_GUIDE.md`, `docs/features/page-settings.md`, `docs/features/page-wizard.md`, `docs/user/src/guide-utilisation.html`

## Critères d'acceptation
- [ ] ⭐ **`admin`/`admin` ne rend JAMAIS un jeton**, quel que soit l'ordre des appels — y compris après un `complete-wizard` déclenché par un tiers
- [ ] `GET /get-config` rend **401** sans jeton — **mesuré sur l'appareil**
- [ ] Un jeton pris pendant le premier démarrage est **invalide** après la clôture de l'assistant
- [ ] Changer le mot de passe **invalide** les jetons existants
- [ ] Un appareil neuf reste configurable **sans impasse**
- [ ] `pio run` sans nouveau warning ; gardes CI vertes

## Notes pour agent
- ⚠️ **Les quatre faits ont été VÉRIFIÉS dans le code** le 2026-11-17. ⛔ Ne pas les re-chercher ; vérifier qu'ils sont toujours vrais, oui (l'application évolue).
- ⚠️ `regenerateApiToken()` **existe déjà** (`src/auth.h:56`) : ce lot **applique** une capacité présente, il n'en invente pas.
- ⛔ **Ne pas absorber `bug-183`** (comparaisons en temps variable, journalisation des échecs) : fiche distincte, en attente.

## Hors périmètre
- Le retrait de HTTP Basic → [`bug-184`](../done/bug-184-authentification-basic-accepte-un-mot-de-passe-qui-n-est-pas-le-bon.md).
- Les comparaisons de mot de passe et le journal des échecs → [`bug-183`](../todo/bug-183-auth-login-echecs-muets-et-comparaison-non-constante.md).
- Le chiffrement des échanges → ⛔ tranché le 2026-11-17 : on reste en clair.
