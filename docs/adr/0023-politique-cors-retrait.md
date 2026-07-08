# ADR-0023 — Retrait complet du mécanisme CORS : politique même-origine stricte

- **Statut** : Accepté
- **Date** : 2026-07-06
- **Décideurs** : décision utilisateur (option B) + architect, feature-028
- **Spec(s) liée(s)** : feature-028 (durcissement sécurité web)

## Contexte

Le contrôleur embarquait un mécanisme CORS configurable (`AuthConfig::corsAllowedOrigins`, champ `auth_cors_origins` de `/get-config` / `/save-config`, card dédiée dans Paramètres → Sécurité), mais son implémentation était **partiellement fonctionnelle et malhonnête** :

- les en-têtes étaient posés statiquement via `DefaultHeaders` au démarrage, en ne retenant que le **premier** origin de la liste blanche — le multi-origines configuré était **silencieusement ignoré**, sans validation de l'`Origin` par requête ;
- `setCorsHeaders()` calculait l'origin autorisé mais était du **code mort** (jamais branché sur les réponses) ;
- l'UI laissait croire à un support multi-origines qui n'existait pas.

Or le besoin réel est inexistant : l'UI est **servie par l'ESP32 lui-même** (LittleFS, même origine), le projet est **offline first** (aucune intégration cross-origin cloud), et les clients machine (scripts `curl`, Home Assistant via MQTT) ne sont pas soumis au CORS. Réparer un mécanisme sans cas d'usage revenait à maintenir de la surface d'attaque et de la complexité pour rien.

## Décision

**Retrait complet du mécanisme CORS.** Le contrôleur applique une **politique même-origine stricte** : aucun en-tête `Access-Control-Allow-*` n'est émis, aucun preflight `OPTIONS` n'est traité.

Périmètre du retrait (v2.11.2) :
- bloc `DefaultHeaders` CORS et fonction morte `setCorsHeaders()` ([`src/web_server.cpp`](../../src/web_server.cpp) — un commentaire de politique subsiste dans `setupRoutes()`) ;
- champ `AuthConfig::corsAllowedOrigins` et sa persistance NVS (la valeur NVS existante devient **orpheline**, aucune migration) ;
- champ `auth_cors_origins` de `/get-config`, `/save-config` et du message WebSocket `config` ;
- card CORS de l'UI (Paramètres → Sécurité) ;
- handler preflight `HTTP_OPTIONS`.

Les en-têtes de sécurité **conservés** : `Content-Security-Policy` (`default-src 'self'`…), `X-Content-Type-Options: nosniff`, `X-Frame-Options: SAMEORIGIN`.

## Alternatives considérées

- **Réparer le CORS multi-origines par requête** (rejetée) — validation de l'`Origin` entrant contre la liste blanche avec echo de l'origin autorisé + gestion du preflight `OPTIONS`. ESPAsyncWebServer s'y prête mal (les `DefaultHeaders` sont globaux et statiques ; une validation par requête exige d'instrumenter chaque réponse), et surtout le **besoin est inexistant** : aucune page tierce ne consomme l'API depuis un navigateur.
- **Assumer l'origin unique** (rejetée) — garder le mécanisme actuel en le documentant comme « un seul origin supporté ». Conserverait un champ de config, une persistance NVS et une card UI pour un mécanisme sans besoin réel démontré ; la dette de « prétention de support » resterait.

## Conséquences

### Positives
- Plus aucune fonctionnalité CORS « morte » ou trompeuse : le comportement du firmware correspond exactement à ce qu'il annonce (AC2 de la feature-028).
- Surface de config réduite (un champ, une card UI, un chemin de persistance en moins).

### Négatives / dette assumée
- La clé NVS de `corsAllowedOrigins` reste **orpheline** sur les appareils déjà déployés (valeur inerte, jamais relue — pas de migration, coût nul).
- Une page web tierce hébergée ailleurs ne peut **pas** appeler l'API du contrôleur depuis un navigateur.

### Ce que ça verrouille
- Toute intégration cross-origin future devra passer par un **reverse proxy** (qui pose ses propres en-têtes CORS) ou **rouvrir cette décision** par un nouvel ADR — ne pas ré-ajouter d'en-têtes CORS au cas par cas.
- Les clients non-navigateur (scripts, intégrations) ne sont pas concernés : l'API HTTP reste accessible avec auth, le CORS n'étant qu'une restriction côté navigateur.

## Références

- Code : [`src/web_server.cpp`](../../src/web_server.cpp) (commentaire de politique dans `setupRoutes()`), [`src/config.h`](../../src/config.h) (`AuthConfig` sans `corsAllowedOrigins`), [`src/web_routes_config.cpp`](../../src/web_routes_config.cpp) (`/get-config` / `/save-config` sans `auth_cors_origins`), [`data/index.html`](../../data/index.html) / [`data/app.js`](../../data/app.js) (card CORS retirée)
- Spec : `specs/features/doing/feature-028-durcissement-securite-web.md` (décision utilisateur option B, annotation du 2026-07-06)
- Doc : [docs/API.md](../API.md) (politique d'origine), [docs/subsystems/web-server.md](../subsystems/web-server.md), [docs/features/page-settings.md](../features/page-settings.md)
