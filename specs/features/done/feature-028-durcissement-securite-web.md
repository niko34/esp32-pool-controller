## Métadonnées
- **ID** : feature-028
- **Slug** : durcissement-securite-web
- **Type** : feature
- **Statut** : done
- **Priorité** : haute
- **Version cible** : **2.11.2**
- **Dernière mise à jour** : 2026-07-06 (livré en v2.11.2 — tests verts, revue Approved)
- **Spec(s) liée(s)** : aucune — issue de la revue de code projet (2026-06-20)

> **Annotations d'audit (2026-07-06)** :
> 1. Point 1 (comparaison `==` non constant-time, auth.cpp:108) et point 3 (code mort `masked*`, auth.cpp:103-106) : **toujours valides**.
> 2. Point 2 (CORS) : **partiellement périmé** — contrairement à la revue de juin, le code actuel pose réellement les en-têtes (`DefaultHeaders`, web_server.cpp:90-108), mais en mode dégradé malhonnête : premier origin de la liste seulement, statique, sans validation par requête ; le multi-origines configuré est silencieusement ignoré.
> 3. **DÉCISION UTILISATEUR (option b) : RETRAIT COMPLET du CORS** — usage même-origine strict (UI servie par l'ESP32, offline first). Périmètre du retrait : bloc `DefaultHeaders` CORS de web_server.cpp, champ `AuthConfig::corsAllowedOrigins` (config.h + persistance), champs `auth_cors_origins` de /get-config et /save-config, champ UI `#auth_cors_origins` + bouton `#cors-save-btn` (Paramètres → Sécurité, index.html + app.js). La valeur NVS existante devient simplement orpheline (pas de migration nécessaire). ADR court (politique : même origine stricte, pas de CORS).
> 4. Comparaison constant-time : longueur vérifiée d'abord (la longueur du token n'est pas secrète — format fixe), puis comparaison octet à octet sans court-circuit (accumulation OR, pas de return anticipé).
> 5. Version cible réalignée : 2.11.2.

## Domaines touchés
- [ ] Capteurs (pH / ORP / température)
- [ ] Régulation (pump_controller, PID)
- [ ] Filtration / Éclairage
- [x] Web — routes HTTP / WebSocket
- [ ] MQTT / Home Assistant
- [ ] UART / écran LVGL
- [ ] OTA
- [x] Authentification
- [ ] Historique / persistance
- [ ] Frontend (`data/`)

## Agents requis
- [x] spec-reader
- [x] architect
- [ ] pool-chemistry
- [ ] ux-ui-designer
- [x] embedded-developer
- [x] web-ui-developer
- [x] test-engineer
- [x] code-reviewer
- [x] doc-writer

## Description
Durcir la couche web/authentification : comparaison de token à **temps constant**, **réparation ou retrait** du mécanisme CORS aujourd'hui non fonctionnel, et **suppression du code mort de debug** dans l'authentification.

## Contexte
La revue de code (2026-06-20) a relevé trois écarts dans la couche web :
1. `auth.cpp:108` — comparaison du token via `==` sur `String` : **non constant-time**, sensible (théoriquement) à une attaque temporelle ; c'est pourtant le contrôle d'accès principal.
2. `web_server.cpp:93-104` — le CORS est annoncé mais cassé : `setCorsHeaders` calcule `allowed` mais **ne pose aucun header** (code mort), et le fallback ne retient que le **premier** origin de la liste blanche → multi-origines silencieusement non supporté. Comportement trompeur.
3. `auth.cpp:104-106` — `maskedReceived`/`maskedExpected` (vestiges de debug) calculés mais jamais utilisés : code mort.

Ces points sont individuellement mineurs en LAN, mais touchent le contrôle de sécurité central et la lisibilité ; regroupés ici car ils forment un même chantier « hygiène sécurité web ».

## Comportement attendu
- **Token** : comparaison à temps constant (longueur d'abord neutralisée, comparaison octet à octet sans court-circuit). Aucun token loggué en clair (état actuel `auth.cpp:15,81` à préserver).
- **CORS** :
  - soit **réparer** : poser réellement les en-têtes `Access-Control-Allow-Origin` en validant l'`Origin` entrant contre la liste blanche (echo de l'origin autorisé, gestion `OPTIONS` preflight), supprimer la prétention de support si non tenue ;
  - soit **retirer proprement** le mécanisme si le CORS n'est pas un besoin réel (offline first, même origine) — décision à prendre par l'architect.
- **Code mort** : suppression des variables `masked*` et de toute logique de debug résiduelle dans `auth.cpp`.

### Cas limites
- Token de longueur différente de l'attendu → rejet sans fuite de timing.
- Requête sans `Origin` (appel direct, même origine) → comportement inchangé.
- `Origin` non autorisé → pas d'en-tête CORS permissif (ou rejet selon décision).
- Preflight `OPTIONS` → réponse correcte si CORS conservé.

## Comportement actuel
- Auth par token comparé avec `==` (`auth.cpp:108`), refus du token en query param déjà en place (`auth.cpp:93-99`, à conserver).
- CORS partiellement implémenté et non fonctionnel (`web_server.cpp:93-104`).
- Code mort de debug dans `auth.cpp`.

## Contraintes techniques
- ESP32, offline first — le CORS n'est pas indispensable si l'UI est servie depuis la même origine ; trancher explicitement.
- Ne pas régresser les protections existantes (refus token en query param, non-logging du token).
- AsyncWebServer — gestion correcte des handlers `OPTIONS` si CORS conservé.
- Français pour tout message utilisateur.

## Fichiers potentiellement concernés
- `src/auth.cpp` — comparaison constant-time, suppression code mort `:104-106` [modif]
- `src/web_server.cpp` — réparation ou retrait CORS `:93-104` [modif]
- `src/constants.h` — éventuelle liste blanche d'origines centralisée [modif]
- `docs/API.md` — politique CORS / auth documentée [doc]

## Nouvelles signatures / API (optionnelle)
- Fonction interne : `bool constantTimeEquals(const String& a, const String& b);` (ou équivalent)

## Sécurité chimique (optionnelle)
- Dosage impliqué : non.

## ADR attendu ?
- **Probable** : oui (faible)
- **Critère(s) candidat(s)** : 2 (décision « réparer vs retirer le CORS » invisible dans le code une fois faite)
- **Sujet pressenti** : politique CORS du contrôleur (support multi-origines vs même origine stricte).

## Éléments visibles (pour doc-writer)

### Endpoints HTTP concernés
- En-têtes CORS sur l'ensemble des routes (si conservé)

### Champs WebSocket
- aucun

### Topics MQTT
- aucun

### Documentation à produire / mettre à jour
#### Docs vivantes
- `docs/subsystems/auth.md` — comparaison constant-time (créer si absent)

#### Docs transverses
- `docs/API.md` — politique CORS et authentification
- `CHANGELOG.md` — entrée sécurité

#### Docs immuables
- `docs/adr/NNNN-politique-cors.md` — si ADR confirmé

## Critères d'acceptation
- [x] AC1 — La comparaison du token est à temps constant (pas de court-circuit dépendant du contenu).
- [x] AC2 — Aucune fonctionnalité CORS « morte » ne subsiste : soit les en-têtes sont réellement posés et corrects (multi-origines testé), soit le mécanisme est retiré et documenté.
- [x] AC3 — Le code mort de debug (`masked*`) est supprimé.
- [x] AC4 — Aucune régression : refus du token en query param, token jamais loggué en clair.
- [x] `pio run` compile sans nouveau warning.

## Plan de test manuel
1. Auth nominale (token correct) → accès OK.
2. Token incorrect / longueur différente → rejet.
3. (Si CORS conservé) requête cross-origin depuis une origine autorisée puis non autorisée → en-têtes corrects / absents.
4. Preflight `OPTIONS` → réponse attendue.

## Notes pour agent
- `architect` doit trancher CORS « réparer vs retirer » avant implémentation (offline first plaide pour un retrait simple si l'UI est même origine).
- Conserver impérativement les protections existantes de `auth.cpp`.

## Hors périmètre
- Passage à une auth par session/JWT — hors scope (reste sur le modèle token actuel).
- Rate limiting (déjà géré ailleurs, cf. commit 6c79cfc).

## Notes d'implémentation (2026-07-06, v2.11.2)

### Fichiers modifiés
- `src/auth.cpp` — `constantTimeEquals` (statique) : longueur vérifiée d'abord (non secrète, format fixe 32 hexa), puis accumulation `diff |= a[i] ^ b[i]` sur tous les octets sans court-circuit ; utilisée par `checkTokenAuth` ; code mort `maskedReceived`/`maskedExpected` supprimé.
- `src/auth.h` — nouvelle API publique `bool AuthManager::secureTokenEquals(const String& candidate) const` (commentaire : à utiliser pour TOUTE vérification de token, jamais `==`/`!=`).
- `src/ws_manager.cpp` — auth WS via `authManager.secureTokenEquals(token)`.
- `src/web_server.cpp` — retrait du bloc `DefaultHeaders` CORS et de `setCorsHeaders` (code mort) ; commentaire de politique conservé dans `setupRoutes()` ; CSP/nosniff/X-Frame-Options intacts ; preflight `HTTP_OPTIONS` retiré.
- `src/config.h` + persistance — champ `AuthConfig::corsAllowedOrigins` supprimé (valeur NVS orpheline, pas de migration).
- `src/web_routes_config.cpp` — champ `auth_cors_origins` retiré de `/get-config`, `/save-config` et du message WebSocket `config`.
- `data/index.html` / `data/app.js` — card CORS retirée (Paramètres → Sécurité : `#auth_cors_origins`, `#cors-save-btn`).
- `src/version.h` — 2.11.2.

### Découverte en revue (code-reviewer, Request changes → Approved)
L'auth **WebSocket** (`ws_manager.cpp`) comparait le même token en `!=` (court-circuitant) — le durcissement HTTP seul aurait laissé le canal temporel ouvert sur le WS. Corrigé en exposant `AuthManager::secureTokenEquals` et en l'utilisant côté WS. Verdict final : Approved.

### Retrait CORS complet (option B — surfaces touchées)
`DefaultHeaders` CORS + `setCorsHeaders` (web_server.cpp), `AuthConfig::corsAllowedOrigins` + NVS (config), `auth_cors_origins` (/get-config, /save-config, message WS `config`), card UI Sécurité, preflight `HTTP_OPTIONS`. Politique même-origine stricte formalisée dans [ADR-0023](../../../docs/adr/0023-politique-cors-retrait.md).

### Vérifications
- `pio run` : 0 warning. Tests natifs : 185/185 verts. `node --check` + `build_fs` OK.

### Reste à valider on-target
- Auth nominale (token correct) → accès OK ; token faux / longueur différente → rejet (HTTP et WS).
- Vérifier que la page Paramètres → Sécurité ne montre plus la card CORS.
