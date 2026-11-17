# ADR-0127 — Le bornage du journal d'authentification est global par émetteur, pas par route ni par IP

- **Statut** : Accepté
- **Date** : 2026-11-16
- **Décideurs** : Nicolas + Claude (`architect`, `security-reviewer`, `code-reviewer`)
- **Spec(s) liée(s)** : [`bug-112`](../../specs/features/done/bug-112-journal-non-borne-sur-route-protegee.md), [`bug-080`](../../specs/features/), [`feature-151`](../../specs/features/doing/feature-151-signature-de-l-image-ota.md) (D3-c)

## Contexte

Un client **non authentifié** du réseau local pouvait purger le journal persistant en **~27 requêtes (~13 s)** : fichier `kMaxLogFileBytes` = 8 192 o, 6 144 o conservés à la rotation ([`logger.h`](../../src/logger.h)), ~230 o persistés par requête refusée. La fenêtre détruite est celle dont dépendent `bug-080` (cinétique heap avant reset) et la trace des autorisations temporaires de signature OTA ([ADR-0125](0125-l-expiration-de-l-interrupteur-de-signature-se-mesure-sur-un-horodatage-nvs.md)).

Le refus d'authentification doit rester **visible** (information de sécurité) mais **borné**, sur un chemin atteignable sans identifiants pendant une famine mémoire : **aucune allocation** sur le chemin muet.

## Décision

1. **Clé globale par émetteur.** **Neuf** sites indépendants (401, 429, trois routes de calibration, upload OTA, `/save-config`, `/tune-ph-offset`, `/ws` « Jeton rejeté »). Le 401 est **un seul état pour les 86 routes protégées**, pas une table par route, et pas une clé par IP.
2. **Recul exponentiel** : fenêtre `kUnauthWarnThrottleMs << shift` ; `shift` +1 par ligne émise (sauf la première), plafond `kUnauthWarnMaxShift` (6, soit 64 min) ; −1 par `kUnauthWarnDecayMs` (1 h) **sans tentative**, jamais au drain. Lignes à t = 0, 1, 3, 7, 15, 31, 63 min.
3. **Drain** depuis `loop()` après `kUnauthWarnDrainQuietMs` (120 s) de silence, **borné par la même fenêtre** ; un drain émis compte comme une ligne. Le drain n'écrit pas l'état : il publie, `async_tcp` l'applique (`authLogThrottleFoldDrain`).
4. **Décision pure** dans `auth_log_throttle_logic.h` (ADR-0017) ; coquille atomique factorisée et testable en natif dans `auth_log_throttle_site.h`. Garde CI `tools/check_unauth_log_guard.sh` (job `unauth-log-guard`), **cinq règles**, chacune validée par mutation : (1) aucun `systemLogger` dans la branche de refus d'une garde `if (!…Authorized(` de `web_routes_*.cpp` ; (2) aucun entre l'en-tête d'un handler et sa garde ; (3) dans tout **lambda handler** de `web_routes_*.cpp`, un `systemLogger` ne vient qu'après un `REQUIRE_AUTH`, un `…Authorized(` ou un `checkAuth(` ; (4) aucun dans `ws_manager.cpp::_onData` ni `_onClientConnect` ; (5) aucun dans les six fonctions d'`auth.cpp` (`checkAuth`, `checkTokenAuth`, `checkBasicAuth`, `checkRateLimit`, `sendAuthRequired`, `sendRateLimitExceeded`). **Exemption unique** : `AUTH-LOG-EXEMPT(raison)` sur l'INFO de `/auth/complete-wizard` (borné par nature : `clearFirstBootFlag()` rend `isFirstBootDetected()` faux, il n'écrit qu'une fois).
5. **Seul le journal est borné**, jamais la réponse HTTP (401/429 partent toujours).
6. **Borner et authentifier sont deux réponses distinctes.** `POST /debug/sensor_trace_clear` (un INFO persisté par requête, sans authentification, donc sans quota) n'est **pas** un neuvième site : il a été **authentifié** (`REQUIRE_AUTH(WRITE)`, décision A1 de `feature-070`). Ce n'est pas un refus d'authentification — un site de throttle l'aurait annoncé comme tel — et l'action efface une fenêtre de diagnostic que l'opérateur observe. Le frontend l'appelait déjà via `authFetch`.

## Alternatives considérées

- **Clé par IP** (rejetée) — l'IP est usurpable trivialement sur un LAN, et la table croît (cf. `rateLimitMap`, feature-131).
- **Table fixe par route** (rejetée) — 86 sites ⇒ hachage à collisions, donc une garantie qui n'en est pas une.
- **`millis()` nu** (rejetée) — repliement à 49,7 j ; soustractions non signées et booléen « jamais journalisé » à la place.
- **Fenêtre fixe 60 s** (insuffisante) — 1 ligne/min suffit à purger le journal en une heure.

## Conséquences

### Positives
- Mémoire constante, chemin muet sans allocation ; décision testée en natif et par mutation.
- Une tentative isolée reste journalisée.

### Négatives / dette assumée
- ⚠️ **Risque assumé de la clé globale : un attaquant peut MASQUER une attaque sur une route en saturant une autre.** Seule l'URL de la requête bavarde est nommée dans la ligne. Atténué par deux drapeaux, `otherRoutes` et `otherIps`, reversés dans la ligne suivante.
- ⚠️ **IPv6** : le cast de l'adresse donne 0 ⇒ le drapeau « autres IP » n'est **jamais levé entre clients IPv6** (faux négatif).
- ⚠️ **Résidu chiffré, aucun majorant établi.** Pire cas **mesuré** : **38 lignes / 24 h par site** (émissions et drains cumulés), atteint par des groupes de 5 tentatives espacées de 60 s répétés toutes les 3 841 s (le drain n'est pas une tentative : la décroissance continue de courir pendant qu'il émet). C'est le pire **trouvé** sur l'espace exploré (~9 millions de tirages guidés) ; **aucune recherche exacte** : un 39 reste possible. **Neuf sites cumulés : 9 × 38 = 342 lignes / 24 h** — somme de maxima mesurés, aucun scénario joint mesuré, aucun majorant établi. À ~230 o la ligne : **~79 Ko / 24 h** contre un journal de 8 Ko. ⇒ **le journal reste purgeable à vitesse lente. C'est assumé.**

- ⚠️ **Limite de la garde (règle 3).** Elle ne juge que les **lambdas** : un lambda sans garde qui appelle une **fonction nommée** contenant un `systemLogger` n'est pas vu. L'essai qui jugeait aussi les fonctions nommées produisait **48 faux positifs** dans `web_routes_ota.cpp` (helpers appelés depuis des routes authentifiées). La limite est assumée, non masquée. (L'exemption de `handleSaveConfig`, hors lambda, échappe aussi à la règle 3.)
- ⭐ **Leçon du lot : l'énumération manuelle a raté des sites à CHAQUE passe** — 2, puis 5, puis 8, puis 9 + 1 authentifié : trois NO GO successifs. La garde mécanique protège contre la récidive **et** contre les sites neufs **de la forme couverte** ; ⛔ pas contre toutes les formes.

### Ce que ça verrouille
- Tout nouveau handler protégé doit passer par la garde `check_unauth_log_guard.sh` : un `systemLogger` avant l'authentification ou dans une branche de refus échoue en CI.
- La vraie réponse (rétention séparée des traces de sécurité) reste à faire : spec de suivi.

## Note datée (2026-11-17)

`checkBasicAuth` a disparu avec bug-184 ([ADR-0130](0130-l-appareil-n-accepte-que-le-jeton-pas-http-basic.md)) : la règle (5) de la garde CI porte désormais sur **cinq** fonctions (`checkAuth`, `checkTokenAuth`, `checkRateLimit`, `sendAuthRequired`, `sendRateLimitExceeded`). La décision ci-dessus est inchangée.

## Références

- [`src/auth_log_throttle_logic.h`](../../src/auth_log_throttle_logic.h), [`src/auth_log_throttle_site.h`](../../src/auth_log_throttle_site.h), [`src/auth.cpp`](../../src/auth.cpp), [`src/constants.h`](../../src/constants.h) (`kUnauthWarn*`)
- [`docs/subsystems/auth.md`](../subsystems/auth.md), [`docs/subsystems/logger.md`](../subsystems/logger.md)
