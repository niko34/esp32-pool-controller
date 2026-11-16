# bug-112 — Le refus d'authentification journalise sans limite : la fenêtre forensique est effaçable sans identifiants

## Métadonnées
| | |
|---|---|
| Type | bug (sécurité / observabilité) |
| Statut | livré 2.89.1 — tests verts, revue OK (reste à promouvoir en `done/` avec le commit) |
| Priorité | **Haute** — vise directement le seul critère encore ouvert de `bug-080` |
| Version cible | **2.89.1** (livré) |
| Relevé par | `code-reviewer`, 2026-09-07, pendant l'audit de `bug-save-config-applique-avant-authentification` |
| Specs liées | `bug-080` (le critère menacé), `feature-109` (la fenêtre qu'il détruit), `bug-113` (même chemin non authentifié — **déjà close** : `done/`, 2026-09-07, v2.48.0, lot `bug-101`) |

## Domaines touchés
- [x] Web — routes HTTP / Authentification (`src/auth.cpp`, `sendAuthRequired`)
- [x] Observabilité (`src/logger.cpp`)
- [ ] Régulation — [ ] Capteurs — [ ] MQTT — [ ] Frontend — [ ] OTA

## Agents requis
- [x] architect · [x] embedded-developer · [x] code-reviewer · [x] doc-writer
- [ ] `pool-chemistry` — **sans objet** : aucun chemin de dosage, aucune décision, aucun seuil chimique.

## Description
Toute route protégée journalise son refus d'authentification **sans limite de débit**.
Un client non authentifié sur le réseau local peut donc **purger le journal
persistant à volonté**, et avec lui la fenêtre forensique dont dépend le
diagnostic de `bug-080`.

## Contexte — les chiffres, mesurés
- ⚠️ **Correction (2026-11-16)** : la version initiale citait « 24 Ko sur 32 », « ~105 requêtes », « ~70 s » : tailles **historiques**, périmées. Le journal fait **8 192 o** et en conserve **6 144** à la rotation (`src/logger.h:117-118`).
- Une requête non authentifiée coûte **~230 o persistés** (`sendAuthRequired` ≈ 84 o,
  plus les émetteurs de la route concernée).
- La purge prend donc **~27 requêtes, ~13 s** : le constat était **plus grave** que la spec ne le disait.
- Au-delà du quota, le limiteur est lui-même un **amplificateur** : `src/auth.cpp:171`
  **et** `:244` (la spec disait 243) journalisent → jusqu'à **3 lignes par requête**.

**Pourquoi ça compte précisément** : `feature-109` a livré un cycle entier pour
diviser par 67 le débit du journal et porter la fenêtre forensique à **~2 h 30**.
C'est elle qui doit porter la ligne « Cinétique heap avant reset » d'un redémarrage
inopiné — la preuve du critère « 7 jours sans redémarrage » de `bug-080`, dont une
capture automatique quotidienne dépend (`/root/pool-captures/`). Un émetteur non
authentifié et non borné rend cette preuve effaçable par un tiers.

## Comportement actuel
`checkRateLimit()` est appelé dans le handler de **requête** (`src/auth.cpp:197`),
donc **après** un éventuel handler de corps. Le quota borne les requêtes *traitées*,
**jamais** les lignes déjà écrites.

## Comportement attendu
Le refus d'authentification reste **visible** — c'est une information de sécurité,
la supprimer serait pire — mais **borné** : throttle par route et/ou par IP, avec
compteur d'occultés reversé dans la ligne suivante, sur le patron déjà livré en
v2.46.1 pour les routes de calibration (`kCalibrationAuthWarnThrottleMs`,
`src/web_routes_calibration.cpp`).

**Invariant impératif** : le chemin muet ne doit **allouer aucune mémoire**. C'est le
point non négociable — ce chemin est atteignable sans authentification pendant une
famine mémoire active (`bug-080` : 21 échecs d'allocation reproduits le 07/09 en trois
chargements de page, tampons WiFi de 1 622 o).

## Contraintes techniques
- Le correctif touche un chemin commun à **toutes** les routes protégées : le risque
  de régression est la perte de visibilité d'une attaque réelle, pas un plantage.
- Ne pas confondre avec le throttle déjà en place : v2.46.1 borne les trois routes de
  calibration. Ici il s'agit de l'émetteur **générique**, qui **préexiste**.

## Critères d'acceptation
- [x] 500 requêtes non authentifiées consécutives ne consomment pas plus de N lignes
      de journal (N à fixer par l'architect, chiffré)
- [x] Le chemin muet n'alloue pas (vérifié par lecture, et par l'absence de churn)
- [x] Un refus isolé reste journalisé — la visibilité d'une tentative unique n'est pas perdue
- [x] Le compteur d'occultés est reversé, borné, et immunisé au repliement de `millis()`
- [ ] `pio run -e esp32dev_core3` et `pio test -e native` sans régression

## Plan de test manuel
1. Relever la taille du journal, puis envoyer 200 requêtes non authentifiées.
2. Vérifier que le journal n'a pas été purgé et que les lignes antérieures survivent.
3. Vérifier qu'une tentative isolée, une heure plus tard, est bien journalisée.

## Hors périmètre
- Le tamponnage JSON non authentifié : c'était `bug-113` (déjà close, v2.48.0).
- Le double-`send` préexistant (authentifié au-dessus du quota → 200 puis 429).

## Notes d'implémentation (v2.89.1)
- **Fichiers** : `src/auth_log_throttle_logic.h` (pur, ADR-0017), `src/auth_log_throttle_site.h` (coquille atomique, auparavant écrite 3 fois, testée 0), `auth.*`, `constants.h`, `main.cpp`, `web_routes_{calibration,config,ota}.*`, `test/test_native_auth_log_throttle/`, `tools/check_unauth_log_guard.sh` (+ job CI `unauth-log-guard`), `tools/check_memory_discipline.sh`.
- **Mécanisme** : recul exponentiel (60 s << shift, plafond 6 = 64 min, −1 par heure sans tentative), drain depuis `loop()` après 120 s de silence, borné par la même fenêtre. **Neuf sites** (401, 429, trois calibrations, OTA, `/save-config`, `/tune-ph-offset`, `/ws`) ; clé globale par émetteur ([ADR-0127](../../../docs/adr/0127-le-bornage-du-journal-d-authentification-est-global-par-emetteur.md)).
- **Chiffres, sans arrondi** : pire cas **mesuré** 38 lignes / 24 h par site ; **aucun majorant établi** (un 39 reste possible) ; neuf sites : 9 × 38 = 342 lignes / 24 h, somme de maxima mesurés, sans scénario joint mesuré ; ~79 Ko / 24 h pour un journal de 8 Ko ⇒ purgeable à vitesse lente, assumé.
- **Troisième tour de revue** : deux émetteurs de plus, atteignables sans identifiants. (1) `ws_manager.cpp` « Token rejeté » (un WARN par refus de jeton sur `/ws`, hors `checkRateLimit()`) ⇒ **borné** (`logWsTokenRejected`, drain dans `WsManager::update()`, préfixe `[WS] Jeton rejeté :`). (2) `POST /debug/sensor_trace_clear` (un INFO persisté par requête, sans auth) ⇒ **authentifié, PAS borné** (`REQUIRE_AUTH(WRITE)`, décision A1 de `feature-070`) : ce n'est pas un refus d'authentification (un site de throttle l'aurait annoncé comme tel) et l'action efface une fenêtre de diagnostic observée. Changement observable : un `curl` sans jeton est refusé ; le frontend utilisait déjà `authFetch`. Décompte : **neuf** sites throttlés, pas dix.
- **Garde CI, cinq règles** (chacune validée par mutation) : branche de refus des gardes `…Authorized(` ; rien entre l'en-tête d'un handler et sa garde ; dans tout lambda handler, `systemLogger` seulement après `REQUIRE_AUTH`/`…Authorized(`/`checkAuth(` ; rien dans `ws_manager.cpp::_onData`/`_onClientConnect` ; rien dans les six fonctions d'`auth.cpp`. ⚠️ **Limite** : la règle 3 ne juge que les lambdas ; un lambda sans garde appelant une fonction nommée qui journalise n'est pas vu (48 faux positifs dans `web_routes_ota.cpp` en jugeant aussi les fonctions nommées). **Exemption unique** : `AUTH-LOG-EXEMPT` sur l'INFO de `/auth/complete-wizard` (écrit une fois ; celle de `handleSaveConfig` échappe à la règle 3, hors lambda).
- ⭐ **Leçon (trois NO GO)** : trois revues successives ont chacune trouvé des sites manqués par la précédente (2 → 5 → 8 → 9 + 1 authentifié). L'énumération manuelle ne suffit pas ; d'où la **garde mécanique**, qui protège contre la récidive et contre les sites neufs de la forme couverte, ⛔ pas contre toutes les formes.
- **Leçon initiale** : l'inventaire initial s'arrêtait à `auth.cpp`. **Deux émetteurs de `web_routes_config.cpp` (`/save-config`, `/tune-ph-offset`) ont été trouvés par la revue de sécurité**, pas par la spec. D'où la garde CI mécanique : un inventaire écrit à la main ne suffit pas.
- `checkRateLimit` retourne désormais `true` si `client()` est nul : jugé non exploitable par la revue (`_client` ne devient nul que par `clientRelease()` ou le destructeur ; sans connexion aucune réponse ne revient). L'ancien code déréférençait nul.
- **Suite** : rétention séparée des traces de sécurité (spec à créer).
