# Subsystem — `auth`

- **Fichiers** : [`src/auth.h`](../../src/auth.h), [`src/auth.cpp`](../../src/auth.cpp) (voir aussi [`src/web_routes_auth.cpp`](../../src/) pour les endpoints)
- **Singleton** : `extern AuthManager authManager;`

## Rôle

Gère l'authentification admin + API token + rate limiting pour toutes les routes HTTP (et indirectement WebSocket). Trois niveaux de protection :

```cpp
enum class RouteProtection {
  NONE,      // Route publique (lecture seule, ex: /get-config, /data)
  WRITE,     // Route d'écriture (auth requise — ex: /filtration/on)
  CRITICAL   // Route critique admin (ex: /save-config, /factory-reset, /reboot)
};
```

## Mécanisme supporté : une seule preuve d'identité

**API Token** (`checkTokenAuth`) — header `X-Auth-Token: <token>` uniquement (le query param `?token=...` est refusé, voir Cas limites). Un mot de passe s'échange contre ce jeton par `POST /auth/login`.

⛔ **HTTP Basic a été retiré (bug-184, v2.92.0)** : `checkBasicAuth()` n'existe plus. La bibliothèque serveur comparait le Base64 reçu **sans tenir compte de la casse** (`WebAuthentication.cpp:24`) ; mesuré sur l'appareil, une casse inversée répondait `200`. Voir [ADR-0130](../adr/0130-l-appareil-n-accepte-que-le-jeton-pas-http-basic.md).

⚠️ **Correction d'une phrase fausse de longue date** : ce fichier affirmait « les routes CRITICAL exigent Basic Auth (pas le token), les routes WRITE acceptent l'un ou l'autre ». C'était faux **avant** ce lot : `checkAuth()` n'a jamais distingué `WRITE` de `CRITICAL` dans le choix de la méthode. Le niveau ne change que la politique de quota et de journalisation.

### Décision d'admission : module pur

`checkAuth()` ne décide plus elle-même : elle collecte les faits (auth activée, route publique, quota, jeton valide) et délègue à [`decideAdmission()`](../../src/auth_admission_logic.h) → `AuthVerdict{kAllow, kDenyUnauth, kDenyRateLimited}`. Testé en natif : `test/test_native_auth_admission/` (ADR-0017).

**Plus aucune exemption anonyme** (bug-185, v2.93.0) : l'exemption du premier démarrage sur `/save-config` est **supprimée** (champs `firstBoot` et `isSaveConfigUrl` retirés de `AuthAdmissionInput`). `saveConfigBodyAuthorized()` (partagé avec `/tune-ph-offset`) exige le jeton dans tous les cas. Le seul droit anonyme restant est `/auth/change-password`, route publique par déclaration, à l'état vierge (voir ci-dessous).

## Module pur `auth_bootstrap_logic.h` (bug-185, v2.93.0)

[`src/auth_bootstrap_logic.h`](../../src/auth_bootstrap_logic.h) — sans Arduino, sans allocation, sans journal ; testé en natif (`test/test_native_auth_bootstrap/`, ADR-0017). Décisions : `isFactoryPasswordLiteral`, `computeFirstBoot`, `decideLogin`, `decidePasswordChange`, `planAfterPasswordChange`, `decideWizardClose`. Décision d'architecture : [ADR-0131](../adr/0131-l-etat-de-premier-demarrage-se-deduit-du-mot-de-passe.md).

**Invariant** : *un droit anonyme n'existe que si aucun mot de passe n'a jamais été choisi, et il se limite à en choisir un.*

⛔ **Deux notions distinctes — leur confusion était le défaut** :

| Notion | Sens | Sert à |
|---|---|---|
| `factoryPassword` (`AuthManager::isFactoryPassword()`) | le mot de passe vaut `admin` **ou est vide** | **accorder ou refuser les droits** : droit anonyme de `change-password`, refus `403` de `/auth/login`, `409` de `complete-wizard` |
| `firstBoot` (`isFirstBootDetected()`) | « l'assistant doit encore être montré » = `!wizardCompleted \|\| factoryPassword` | **l'affichage** de l'assistant et la fermeture de la fenêtre — **jamais** un droit |

⭐ **Les deux sont PUBLIÉES par `GET /auth/status`** (`firstBoot` et, depuis la v2.93.0, **`virgin`**
= `isFactoryPassword()`). ⛔ **Toute page qui propose un formulaire ANONYME doit brancher sur
`virgin`**, jamais sur `firstBoot` : sur un appareil dont l'assistant a été interrompu **après** le
choix du mot de passe, `firstBoot` est vrai mais le formulaire anonyme est **refusé** — c'est ce
qui transformait la page de secours OTA en **impasse**.
⇒ Deux points de branchement, et deux seulement : `src/ota_recovery_page.html.in` (⛔ **sans**
repli : elle est embarquée dans le firmware qui sert la clé) et `data/login.html` (**avec** repli
sur `firstBoot` : le système de fichiers se déploie séparément et peut tourner sur un firmware
antérieur).

Pourquoi : `wizardCompleted` est le bit qu'un inconnu pouvait poser (`complete-wizard` anonyme). `firstBoot` reste vrai sur un appareil **déjà possédé** dont l'assistant a été interrompu : lui accorder un droit laisserait un inconnu écraser le mot de passe du propriétaire. Et `decideLogin` ne reçoit **pas** `firstBoot`, volontairement : il enfermerait dehors le propriétaire d'un appareil interrompu après le choix du mot de passe (l'OTA est le seul canal de déploiement, pas de port série).

**Migration auto-réparatrice** : au démarrage, `computeFirstBoot` rouvre l'assistant d'un appareil dont un tiers avait refermé la fenêtre (`wizardCompleted` vrai **et** mot de passe `admin`).

**Ordre d'effets de `/auth/change-password`** (figé, non testable en natif) : mot de passe → `forceWifiConfig` → jeton régénéré **en RAM et dans `authCfg`** (sinon l'ancien ressuscite au redémarrage) → **une seule** écriture NVS → révocation des WebSockets → réponse avec le **nouveau** jeton.

### Époque d'authentification `kAuthEpoch`

`kAuthEpoch` ([`constants.h`](../../src/constants.h), valeur **1** = bug-185) est comparée au champ persisté `authEpoch` (NVS `auth_epoch`, défaut 0). Si l'appareil est en retard, `AuthManager::begin()` **régénère le jeton une fois**, l'enregistre, puis écrit la nouvelle époque. **Motif** : corriger le code ne retire pas les jetons **déjà pris** ; sans ce passage, le correctif ne protégerait que les appareils neufs. ⚠️ Effet visible : **tout le monde se reconnecte une fois** ; un `POOL_TOKEN` figé dans un script tiers devient périmé. MQTT et Home Assistant ne sont pas touchés. ⛔ N'incrémenter que pour une raison de sécurité nommée. Journal : `warning` « jeton d'API renouvelé par la mise à jour » (pas sur un appareil neuf).

⚠️ **Ce que cela ne ferme pas** : entre la mise sous tension d'un appareil neuf et le choix du mot de passe, le premier arrivé gagne (décision de l'utilisateur, aucun geste physique). Attaque **visible** et **réversible** (bouton physique). Le serveur reste en HTTP clair : la rotation protège de l'émission, pas de l'écoute.

## Comparaison de token à temps constant (v2.11.2, feature-028)

⚠️ **Règle absolue : jamais de `==` / `!=` direct sur le token API.** Une comparaison `String` classique court-circuite au premier octet différent, rendant le temps de réponse dépendant du préfixe correct — canal temporel sur le contrôle d'accès central.

Toute vérification passe par la comparaison à temps constant :

```cpp
// auth.cpp — interne
static bool constantTimeEquals(const String& a, const String& b);

// auth.h — API publique pour les autres modules (WebSocket notamment)
bool AuthManager::secureTokenEquals(const String& candidate) const;
```

- **Longueur vérifiée d'abord** (retour anticipé acceptable : la longueur du token n'est pas secrète — format fixe, 32 caractères hexadécimaux).
- Puis comparaison de **tous** les octets sans court-circuit : accumulation des différences par `diff |= a[i] ^ b[i]`, verdict sur `diff == 0`.

Deux consommateurs :
1. **Auth HTTP** — `checkTokenAuth()` ([`auth.cpp:126`](../../src/auth.cpp)) sur le header `X-Auth-Token`.
2. **Auth WebSocket** — `WsManager::_onData()` ([`ws_manager.cpp`](../../src/ws_manager.cpp)) via `authManager.secureTokenEquals(token)` sur le message `{"type":"auth","token":"..."}`. Historiquement le WS comparait le token en `!=` (court-circuit) — corrigé en v2.11.2 via l'API publique.

> ⚠️ Référence `ws_manager.cpp:114` **retirée** (v2.57.0, bug-081) : le fichier a été
> largement remanié (registre `_authRegistry` + envoi ciblé) et ce numéro de ligne ne pointe
> plus sur `_onData()`. Au-delà de la comparaison à temps constant ci-dessus, bug-081
> corrige un défaut **adjacent** : un client WS authentifié n'était **ciblé nulle part** —
> `textAll()` diffusait à tous les clients connectés dès qu'au moins un était authentifié.
> Deux conséquences côté `auth` :
> - **Ciblage** — les trois broadcasts WS passent désormais par
>   `AsyncWebSocket::text(uint32_t id, AsyncWebSocketSharedBuffer)` sur les ids de
>   `_authRegistry`, et plus par `textAll()`. Détail : [ws-manager.md](ws-manager.md#bug-081--diffusion-ciblée-sur-registre-au-lieu-de-textall).
> - **Révocation** — `WsManager::revokeAllAuthenticated()` est appelée depuis **ce** module
>   (`web_routes_auth.cpp`, `POST /auth/regenerate-token`) et depuis `web_routes_config.cpp`
>   (bascule `auth_enabled` faux→vrai) : une rotation de token ou une activation de
>   l'authentification révoque immédiatement tout client WS déjà authentifié avec
>   l'ancien état, qui doit ré-authentifier. Sans elle, le keepalive WS (30 s) aurait
>   maintenu indéfiniment une session authentifiée avec un token révoqué.

Le code mort de debug (`maskedReceived` / `maskedExpected`) a été supprimé au passage ; seul le masquage volontaire des 8 premiers caractères pour les logs de génération subsiste (le token n'est **jamais** loggué en clair).

### Mot de passe : `passwordMatches()` (bug-183)

`AuthManager::getPassword()` est **supprimée**. `AuthManager::passwordMatches(const String& candidate) const` hache les deux côtés en SHA-256 (`OtaStreamHasher`, contexte sur la pile), compare les 32 octets avec `fixedWidthEquals` ([`secret_compare.h`](../../src/secret_compare.h), OU des XOR, sans sortie anticipée), puis efface les deux empreintes par `mbedtls_platform_zeroize`. Pourquoi deux primitives (jeton / mot de passe), la limite du zeroize (le mot de passe reste en clair) et la mesure du désassemblage : [ADR-0132](../adr/0132-le-mot-de-passe-se-compare-par-empreintes-en-temps-fixe.md). Garde CI : `tools/check_secret_compare_guard.sh` (job `secret-compare-guard`).

## Rate limiting

```cpp
MAX_REQUESTS_PER_MINUTE = kMaxRequestsPerMinute = 120    // constants.h
RATE_LIMIT_WINDOW_MS    = kRateLimitWindowMs    = 60000  // constants.h
```

⚠️ **Corrigé le 2026-11-18 (bug-189)** : ce bloc annonçait **30** et renvoyait à `constants.h:37`.
La valeur réelle est **120** et la constante vit bien plus bas dans le fichier. ⛔ Le défaut est
devenu visible parce que la section bug-189 ci-dessous cite **120** : le fichier se contredisait
lui-même. ⇒ ⛔ **Ne pas recopier un numéro de ligne dans cette documentation** ([ADR-0073](../adr/0073-chiffre-consigne-porte-sa-methode.md)) :
il pourrit à la première insertion. La constante se cherche par son nom.

`rateLimitMap` : `std::map<String, RateLimitEntry>` indexé par IP cliente. Entrées expirées nettoyées périodiquement par `cleanupRateLimitMap()`.

Au-delà de **120 requêtes / minute par IP** → `sendRateLimitExceeded()` (HTTP 429).

⚠️ Ce quota protège l'appareil de la **charge**, ⛔ **pas** le mot de passe : il plafonne le
débit sans jamais sanctionner un échec. C'est précisément ce que bug-189 ajoute, séparément
et sans y toucher (voir plus bas). ⚠️ Les deux 429 se distinguent par le champ `retryAfter`,
présent seulement sur celui de bug-189.

## Bornage du journal des refus (`bug-112`, v2.89.1)

Les refus d'authentification journalisent en `WARNING`, mais **de façon bornée** : sans cela un client anonyme purgeait le journal persistant en ~27 requêtes (~13 s). Décision et alternatives : [ADR-0127](../adr/0127-le-bornage-du-journal-d-authentification-est-global-par-emetteur.md).

- **Dix sites bornés** (neuf à l'origine ; `gAuthFailSite` ajouté par bug-183, voir ci-dessous) : 401 (`sendAuthRequired`), 429 (`checkRateLimit`), les trois routes de calibration, l'upload OTA, `/save-config`, `/tune-ph-offset`, `/ws` (« Jeton rejeté », [ws-manager.md](ws-manager.md)). États **indépendants**.
- ⭐ **Borner ≠ authentifier.** Un dixième émetteur, `POST /debug/sensor_trace_clear` (un INFO par requête, sans auth), n'est **pas borné** : il a été **retiré du chemin non authentifié** (`REQUIRE_AUTH(WRITE)`). Ce n'est pas un refus d'authentification (un site de throttle l'aurait annoncé comme tel) et l'action efface une fenêtre de diagnostic (décision A1 de `feature-070`).
- **Clé globale par émetteur** — ni par route, ni par IP : le 401 est **un seul état** pour toutes les routes protégées. Conséquence : seule l'URL de la requête qui a pu parler est nommée ; les autres refus sont comptés dans « et N refus non journalisés », avec les drapeaux `(autres routes)` et `(autres IP)`. ⚠️ Entre clients **IPv6** le drapeau « autres IP » n'est jamais levé.
- **Recul exponentiel** : fenêtre `kUnauthWarnThrottleMs << shift` (60 s de base), `shift` +1 par ligne émise sauf la première, plafond `kUnauthWarnMaxShift` (6 = 64 min), −1 par `kUnauthWarnDecayMs` (1 h) sans tentative. Lignes à t = 0, 1, 3, 7, 15, 31, 63 min.
- **Drain** : `AuthManager::drainUnauthWarnLogs()` (appelé par `loop()`, ainsi que les drains calibration, OTA et config) annonce les occultés après `kUnauthWarnDrainQuietMs` (120 s) de silence, borné par la même fenêtre. Préfixes : `Accès non autorisé :`, `Rate limit dépassé :`, `[OTA] POST /update :`, `[Calibration] /calibrate_x :`, `[Config] /save-config :`, `[pH] /tune-ph-offset :`. Le drain ne remet pas à zéro la date de dernière tentative.
- **Une tentative isolée reste journalisée.** Le 401/429 HTTP part toujours : seul le journal est borné.
- **Contrat** : `sendRateLimitExceeded` ne journalise plus ; l'appelant (`checkRateLimit`) l'a déjà fait. `checkRateLimit` retourne `true` si `client()` est nul (non exploitable : sans connexion aucune réponse ne revient ; l'ancien code déréférençait nul).
- **Chiffres** : pire cas **mesuré** 38 lignes/24 h par site, **380 (10 × 38)** pour dix sites — somme de maxima mesurés, aucun scénario joint mesuré, aucun majorant établi (38 est le pire **trouvé**, un 39 reste possible) ⇒ le journal reste purgeable à vitesse lente, assumé.
- **`checkTokenAuth` comme prédicat de masquage** (2.93.1, `GET /wifi/status`) : employé seul, sans refus, pour décider si un champ (`ssid`) est rendu. ⚠️ Piège : `check_unauth_log_guard.sh` ne le reconnaît **pas** comme une garde (règle 3 : seuls `REQUIRE_AUTH`, `…Authorized(`, `checkAuth(`). Toute journalisation dans un handler qui l'emploie seul fait **tomber la CI** ; un tel handler doit rester muet (aussi : ni `checkAuth`, ni `sendAuthRequired`, ni `checkRateLimit`, qui journalisent).
- **Garde CI** : `tools/check_unauth_log_guard.sh` (job `unauth-log-guard`), **cinq règles**, chacune validée par mutation : (1) aucun `systemLogger` dans la branche de refus d'une garde `if (!…Authorized(` de `web_routes_*.cpp` ; (2) aucun `systemLogger` entre l'en-tête d'un handler et sa garde ; (3) dans tout **lambda handler** de `web_routes_*.cpp`, un `systemLogger` ne vient qu'**après** un `REQUIRE_AUTH`, un `…Authorized(` ou un `checkAuth(` ; (4) aucun `systemLogger` dans `ws_manager.cpp::_onData` ni `_onClientConnect` ; (5) aucun `systemLogger` dans `checkAuth`, `checkTokenAuth`, `checkRateLimit`, `sendAuthRequired`, `sendRateLimitExceeded`. ⚠️ **Limite** : la règle 3 ne juge que les lambdas — un lambda sans garde qui appelle une **fonction nommée** contenant un `systemLogger` n'est pas vu (juger aussi les fonctions nommées donnait 48 faux positifs dans `web_routes_ota.cpp`). Exemption unique : `AUTH-LOG-EXEMPT` sur l'INFO de `/auth/complete-wizard` (écrit une seule fois : `clearFirstBootFlag()` rend `isFirstBootDetected()` faux). Code : `auth_log_throttle_logic.h` (pur), `auth_log_throttle_site.h` (coquille atomique).

## Échecs d'authentification journalisés (`bug-183`, v2.93.3)

Famille de lignes `[Auth] …`, bornée par **un seul** site (`gAuthFailSite`) partagé par `/auth/login` et `/auth/change-password` (deux sites doubleraient le budget de journal de qui choisit sa route).

- WARNING `[Auth] <route> : <nature> depuis <IP>` — natures : nom d'utilisateur inconnu, mot de passe incorrect, jeton invalide. ⛔ Jamais le mot de passe, ni sa longueur, ni le nom d'utilisateur essayé.
- Complément d'occultation : ` ; et N autres échecs non journalisés (route et nature non précisées)`, ou `(route, nature et IP non précisées)` quand les occultés viennent d'autres adresses.
- INFO `[Auth] Connexion réussie depuis <IP>`, suivi de ` (N échecs d'authentification sur /auth/* depuis le démarrage ou la dernière connexion réussie)` s'il y en a eu.
- ⚠️ **Trois limites du compteur** : (1) **en RAM**, remis à zéro au redémarrage et à chaque connexion réussie ; (2) il **agrège les deux routes** `/auth/login` et `/auth/change-password` ; (3) il **ne compte pas** les 401 par jeton des autres routes.
- La réponse HTTP est **inchangée** : 401 identique que le nom ou le mot de passe soit faux ; le classement ne sert qu'au texte du journal.
- `drainAuthRoutesWarnLogs()` est exporté et appelé depuis `main.cpp`.

## Ralentissement des échecs de mot de passe (`bug-189`, v2.94.0)

Avant : le quota (`kMaxRequestsPerMinute`, 120/min/IP) plafonnait le débit sans sanctionner l'échec, soit **172 800 essais/jour/adresse** (un dictionnaire d'un million de mots en ~6 jours). Désormais les essais ratés ralentissent progressivement. Décision, alternatives chiffrées : [ADR-0133](../adr/0133-le-ralentissement-des-echecs-de-mot-de-passe-est-global-avec-exemption.md).

**Module pur** `src/auth_fail_backoff_logic.h` (ADR-0017 : natif, sans allocation ni journal). État `gAuthBackoff` dans `web_routes_auth.cpp`, **en RAM**, capacité fixe. Fonctions : `authFailCheck()` (lecture seule), `authFailRecordFailure()`, `authFailRecordPasswordProof()`, `authFailClearExemptions()` (via `authFailBackoffClearExemptions()`, appelée par `setPassword` et `resetPasswordToDefault`).

| Porte | Constantes (`constants.h`) | Valeurs |
|---|---|---|
| Par IP | `kAuthFailIpSlots` / `…Franchise` / `…BaseMs` / `…MaxShift` / `…CapMs` / `…DecayMs` | 8 créneaux · **3** essais gratuits · 1 s · shift 6 · plafond **60 s** · pardon **300 s** |
| Globale | `kAuthFailGlobalFranchise` / `…BaseMs` / `…MaxShift` / `…CapMs` / `…DecayMs` | **10** essais · 1 s · shift 4 · plafond **10 s** · pardon **60 s** |
| Exemption | `kAuthFailExemptSlots` | **4** adresses ayant prouvé le mot de passe |

- **Verdict** : refus si la porte IP refuse, ou si l'adresse n'est pas exemptée et que la globale refuse ; `Retry-After` = le plus grand des délais. Une adresse exemptée n'a que **sa propre** porte par IP (indépendante des créneaux) ; ses échecs n'alimentent pas la globale. Adresse inconnue (`ip == 0`) : jamais exemptée.
- **Temps** : pas d'échéance absolue ; verdict sur l'écart **non signé** `now - lastFailMs`, décalage borné avant le `<<` ⇒ attente ≤ plafond par construction, y compris au repliement de `millis()`. Pardon **incrémental** (un échec par période). Un refus ne modifie aucun état.
- **Refus immédiat** (jamais un `delay()`) : HTTP 429, voir [API.md](../API.md#post-authlogin--public). Nom inconnu et mot de passe faux sont comptés à l'identique (le délai n'est pas un oracle d'énumération). Un refus est journalisé (`… essai refusé (ralentissement)`) mais ne gonfle pas le compteur d'échecs de `bug-183`.
- **Où** : `/auth/login` après `checkRateLimit`, avant `deserializeJson` ; `/auth/change-password` après l'analyse, avant `passwordMatches`/`checkTokenAuth`, **seulement** pour l'essai par `currentPassword` et hors état vierge. Tout `kAllow` de cette route vide la table puis réinscrit l'auteur (sauf IP inconnue).

### ⭐ Un jeton valide passe toujours

La garde est **hors** de `checkAuth`/`decideAdmission`/`checkRateLimit` : `auth.cpp:338` n'examine le jeton que si le quota passe, donc une garde placée là désarmerait le jeton et **enfermerait le propriétaire dehors**. L'OTA étant le seul canal de reprise, ce serait perdre l'appareil. Aucun état n'exige d'action humaine : tout s'efface par écoulement du temps, au plus 60 s d'attente.

### Relevé sur l'appareil (2026-11-18, v2.94.0 — livrable D3 de `bug-189`)

⚠️ Chiffres **mesurés**, méthode comprise ([ADR-0073](../adr/0073-chiffre-consigne-porte-sa-methode.md)) : 133 tentatives anonymes à une par seconde pendant 150 s, depuis une seule adresse, sur l'appareil de production.

| Grandeur | Relevé |
|---|---|
| Mots de passe réellement comparés | **11** sur 133 |
| Attente entre deux essais acceptés | 1 · 2 · 4 · 8 · 16 · 32 · puis **60 s** (plafond) |
| Régime établi | **1 essai/minute** contre 120 auparavant — facteur **120** |
| Temps de traitement (réseau compris) | **16 à 25 ms**, 401 comme 429 |
| Lignes de journal pour 133 essais | **4** (2 émises + 56 + 75 occultés = 133) |

Portes fermées (429 obtenu juste avant), avec un jeton émis **avant** le ralentissement : page protégée **200**, changement de mot de passe **200**. Avec un `currentPassword` volontairement faux : **401** « mot de passe actuel incorrect » — ⛔ pas un 429, donc la demande est bien **instruite** et le porteur du jeton épargné.

⛔ **Ce que ce relevé ne couvre pas** : l'adresse qui attaquait était **exemptée** (connexion réussie préalable), donc ses échecs ont alimenté sa propre porte. Aucune autre adresse n'a été gênée — effet voulu — mais **la porte globale et les 8 créneaux n'ont pas été éprouvés sur matériel** : ils restent couverts par les cas natifs. La politique mesurée est celle **par IP** ; une adresse non exemptée subit `max(créneau, globale)`, donc au moins cette sévérité.

### ⛔ Ce que ce mécanisme ne couvre PAS

1. **8 640 essais/jour** en attaque distribuée (simulé : 8 642 max globale, 1 441 par adresse, facteur 20×) ; jusqu'à **14 400** si l'attaquant tient les quatre adresses exemptées.
2. Un **mot de passe faible** : 10 000 mots tombent en **1,2 jour**.
3. L'écoute du mot de passe **en clair** sur le Wi-Fi (décision produit).
4. ⚠️ **Famine du propriétaire hors table pendant une attaque active** : après un redémarrage (table en RAM), depuis un nouveau client, ou depuis la page de secours OTA.
5. **NAT partagé** avec l'attaquant : sa porte monte à 60 s.
6. ⚠️ **`/save-config` vide la table sans réinscrire l'auteur** (`web_routes_config.cpp:1645` → `setPassword`) ; aucune page n'emploie ce chemin et le jeton n'y est pas renouvelé, la session survit.
7. Le modèle de test n'est pas le handler réel (non compilable en natif) ; un redémarrage rend en outre la franchise (3 essais gratuits).

⚠️ La robustesse du mot de passe choisi n'est traitée par aucun livrable de ce lot.

### Garde CI `tools/check_auth_fail_guard.sh` (job `auth-fail-guard`)

Règles : `authFailCheck(` n'est appelé que dans `web_routes_auth.cpp`, exactement 2 fois ; le 2ᵉ est précédé de `!currentPassword.isEmpty() && !virginState` ; `authFailRecordFailure(` exactement 2 fois ; `auth.cpp` et `auth_admission_logic.h` n'appellent aucune `authFail*` hors `authFailBackoffClearExemptions`. ⛔ **Ce qu'elle ne prouve pas** : elle lit le **source** (commentaires retirés), donc la forme de l'appel et non son effet ; un appel indirect (pointeur, macro) lui échapperait.

### Écran de connexion et page de secours

Pas de page `docs/features/` dédiée (un seul formulaire) : le comportement est ici, le visuel dans [DESIGN_SYSTEM §5.22](../DESIGN_SYSTEM.md). Sur 429, `login.html` et la page de secours OTA lisent `Retry-After` (repli : champ `retryAfter`, puis 30 s ; borné à 1–3600 s), affichent « Trop d'essais. Patientez un instant. » et désactivent le bouton avec le décompte « Réessayer dans 0:42 ». Rien n'est mémorisé : recharger est sans danger. Le champ « Nom d'utilisateur » visible est supprimé (champ masqué conservé pour les gestionnaires de mots de passe).

## Premier démarrage

`isFirstBoot = !wizardCompleted || isFactoryPassword()` (v2.93.0 ; avant : `!wizardCompleted` seul). L'UI affiche alors l'assistant, qui force le choix du mot de passe. Voir la section « Module pur » ci-dessus pour la différence avec `factoryPassword`.

- `clearFirstBootFlag()` — appelé après le premier changement de mot de passe réussi (et par `complete-wizard` si le mot de passe n'est plus celui d'usine).
- ⚠️ `resetPasswordToDefault()` est du **code mort** (aucun appelant). Le bouton physique (maintien `kFactoryResetButtonHoldMs`) efface la NVS entière puis redémarre ; l'appareil revient donc à l'état vierge.

## AP fallback password

`getApPassword()` retourne le mot de passe du point d'accès `PoolControllerAP`. ⚠️ Il n'est **pas** dérivé du chipID : il est **tiré au hasard** (8 caractères) au premier démarrage, stocké en NVS, **journalisé en clair à dessein** (« Notez-le sur une étiquette ») et **survit** à la remise à zéro.

## Endpoints d'auth

| Endpoint | Rôle |
|----------|------|
| `POST /auth/login` | Vérifie les creds, retourne un token API ; `403` tant que le mot de passe vaut `admin` |
| `GET /auth/status` | État courant (enabled, firstBoot) |
| `GET /auth/token` | Obtient le token API (CRITICAL : exige déjà un jeton valide ; sert à relire un jeton connu, pas à en récupérer un perdu) |
| `POST /auth/change-password` | Change le mot de passe admin ; anonyme **seulement** à l'état vierge ; régénère le jeton et rend le nouveau |
| `POST /auth/regenerate-token` | Génère un nouveau token (invalide l'ancien) |
| `POST /auth/complete-wizard` | Filet idempotent de fin d'onboarding ; `WRITE` (jeton), `409` si mot de passe d'usine |
| `POST /auth/ap-password` | Change le mot de passe AP (mode hors-ligne) |

## Macro helper

```cpp
#define REQUIRE_AUTH(req, level) \
  if (!authManager.checkAuth(req, level)) { return; }
```

Utilisé dans tous les handlers de `web_routes_*.cpp` pour garder le code concis :
```cpp
server->on("/save-config", HTTP_POST, [](AsyncWebServerRequest* req) {
  REQUIRE_AUTH(req, RouteProtection::CRITICAL);
  // ...
});
```

## Stockage du mot de passe

⚠️ Le mot de passe est stocké **en clair** en NVS : `setPassword()` ([`auth.cpp:147`](../../src/auth.cpp:147)) fait `adminPassword = pwd;` sans aucune transformation, et `config.cpp` l'écrit tel quel (`prefs.putString("auth_password", …)`). Il n'est **jamais** renvoyé par l'API (`"******"`). (Une version antérieure de ce fichier affirmait à tort qu'il était hashé.)

## Cas limites

- **Auth désactivée** (`authEnabled = false`) : toutes les routes sont accessibles sans creds — déconseillé, mais possible pour debug local.
- **Token perdu** : `POST /auth/regenerate-token` est `CRITICAL` ([`web_routes_auth.cpp:165`](../../src/web_routes_auth.cpp:165)) : **elle exige le jeton qu'on vient de perdre**, donc elle ne sert pas à le récupérer. ⚠️ Et il ne se retrouve **pas** dans l'interface non plus : **aucun écran de `data/` n'affiche le jeton** (vérifié le 2026-11-17). ⇒ Le **seul** chemin est `POST /auth/login` avec le mot de passe administrateur — donc hors état vierge, où cette route rend `403` (mot de passe d'usine). ⚠️ Sur un appareil vierge, le seul fournisseur de jeton est `POST /auth/change-password` (bug-185, [ADR-0131](../adr/0131-l-etat-de-premier-demarrage-se-deduit-du-mot-de-passe.md)).
- **Rate limit atteint** : réponse 429, pas de décompte de la requête elle-même.
- **Token en query param** (`?token=...`) : **refusé** — seul le header `X-Auth-Token` est accepté (fuite via logs, historique navigateur, Referer).
- **CORS** : mécanisme **retiré** en v2.11.2 — politique même-origine stricte, voir [ADR-0023](../adr/0023-politique-cors-retrait.md).

## Fichiers liés

- [`src/auth.h`](../../src/auth.h), [`src/auth.cpp`](../../src/auth.cpp)
- [`src/web_routes_auth.cpp`](../../src/)
- [`src/auth_fail_backoff_logic.h`](../../src/auth_fail_backoff_logic.h) — ralentissement des échecs (bug-189)
- [`src/constants.h:37`](../../src/constants.h:37) — rate limits
- [docs/API.md](../API.md) — matrice complète des niveaux d'auth par route
