# ADR-0123 — Une seule session TLS sortante par requête HTTP : la signature se rapatrie à la demande

- **Statut** : Accepté
- **Date** : 2026-11-15
- **Décideurs** : `architect` (arbitrage chiffré), `security-reviewer` (P1 + P1-bis, GO sous 7 conditions S1–S7), `ux-ui-designer` (principe « seul l'état anormal parle »), utilisateur (refus du second clic)
- **Spec(s) liée(s)** : [`bug-176`](../../specs/features/done/bug-176-relais-sig-filesystem-saute-sous-pression-memoire.md), [`feature-151`](../../specs/features/done/feature-151-signature-de-l-image-ota.md) (lot D3-a livré en 2.87.0, D3-c bloqué par cette décision)

## Contexte

Depuis la v2.87.0 (feature-151 D3-a, [ADR-0122](0122-socle-decision-ota-genre-empreinte-relais-sig.md)), `GET /check-update` ouvrait **trois sessions TLS sortantes enchaînées dans une seule requête** : l'API GitHub, puis `firmware.bin.sig`, puis `littlefs.bin.sig` (≈ 11 s de blocage de l'unique tâche `async_tcp`).

Le 2026-11-15, premier essai réel : le relais du `.sig` du **filesystem** a été **sauté** par la garde TLS mémoire (`otaTlsGateClosed`, [ADR-0058](0058-gardes-heap-metrique-8-bits.md)) — heap interne 8 bits libre 31 024 o contre un seuil de 32 768 o, soit 1 744 o (5 %) de manque. Le firmware a été authentifié, le filesystem non ; en D3-c, ce filesystem aurait été **refusé au hasard**.

Mesure on-target du même jour (sans instrumentation, lecture de `/get-system-info?hf=1`), après avoir écarté deux campagnes invalides (quota de 120 req/min dépassé par le générateur de charge ; sockets TIME_WAIT du générateur) :

| Contexte | Vérifications | Relais firmware | Relais filesystem |
|---|---|---|---|
| Appareil au calme | 5 | relayé 5/5 | **relayé 5/5** |
| Trafic concurrent (1 requête `/data` par seconde) | 9 | relayé 7/9 | **sauté 9/9** |

Au repos `free8` ≈ 50 000 o ; la garde se ferme sous 32 768 o ⇒ la chute pendant la requête est d'**au moins 17 700 o**. La route elle-même n'alloue qu'environ **2 700 o** (dont ≈ 1 500 o de nœuds de chaînes ArduinoJson) : **le déficit n'est pas de la consommation, c'est de la RÉTENTION**. Pendant que `async_tcp` est bloquée, les trames reçues restent en RAM interne (≈ 1,6 Ko par tampon RX du pilote WiFi, DMA-capable). ⚠️ **La page ouverte de l'utilisateur est le trafic qui affame l'appareil** — ce qui explique l'intermittence et pourquoi un `curl` isolé ne reproduit jamais le défaut. Aucune fuite : `free8` revient à ≈ 50 Ko après 180 s de calme.

## Décision

**Un handler HTTP ouvre au plus UNE session TLS sortante.** `/check-update` ne relaie plus rien : il **mémorise les URL** des `.sig` côté appareil (`g_sigSource`, deux emplacements) et rend un statut par genre. La page demande chaque signature **juste avant de l'installer**, via `GET /ota-sig?kind=firmware|filesystem` (une session, ≈ 4,5 s). Appels **séquentiels** côté page, jamais en parallèle.

Garde-fous retenus (revue de sécurité) :

- **S1** `setRedirectLimit(1)` (`kOtaMaxRedirectsPerGet`) sur **tous** les clients sortants ; la borne de blocage est un `static_assert` (`constants.h`) : 2×(3 000 + 5 000) + 3 000 + 5 000 = **24 000 ms < 30 000 ms** de TWDT.
- **S2** `absent` n'est émis que si la réponse GitHub a été **analysée avec succès** sans asset de ce nom. Tout autre doute (jamais vérifié, URL trop longue, hôte refusé, mémoire, horloge, HTTP, délai, grammaire, anti-déni) ⇒ `unavailable` + motif d'une énumération **fermée**.
- **S3** emplacements effacés à l'entrée de `/check-update`, écrits seulement sur succès ; divergence de `tag` ⇒ « non récupérée », jamais « non signée ».
- **S4** intervalle minimal `kOtaSigMinIntervalMs` = 10 s **par genre**, vérifié après l'authentification et avant TLS ⇒ `unavailable/throttled`.
- **S5** garde TLS fermée et horloge non synchronisée sont rendues en **200** `unavailable`, jamais 503.
- **S7** l'URL ne vient jamais du client et n'est jamais journalisée.
- La garde TLS et le **WARN de saut enrichi** (3 mesures + 3 seuils) sont **conservés**.

## Alternatives considérées

Fenêtre de blocage et déficit estimés au débit mesuré sur le terrain (2 045 o/s ; ≈ 1 610 o/s en laboratoire, la page rafraîchissant le journal toutes les 5 s) :

| Variante | Fenêtre | Déficit | Marge | Verdict |
|---|---|---|---|---|
| **Actuel** (3 sessions / 1 requête) | 11 s | 22 500 o | −7 870 o | ❌ saute |
| **Relais à la demande** (1 session / requête) | **4,5 s** | **9 200 o** | **+5 420 o** | ✅ **retenu** |
| Libérer le `JsonDocument` avant de relayer | 11 s | 20 500 o | −5 870 o | ❌ |
| Réessayer le relais sauté | 14 s | 28 600 o | −13 980 o | ❌❌ **aggrave** |
| Baisser le seuil de la garde | 11 s | 22 500 o | « passe » | ⛔ handshake à ≈ 25 Ko libres |
| Tâche dédiée (relais hors `async_tcp`) | ≈ 0 s | ≈ 0 o | +14 620 o | ❌ 8 Ko de pile interne **permanents** |

- **Libérer avant de relayer** (rejetée) : c'était la piste la plus tentante, car elle « couvrait » les 1 744 o du relevé initial — qui n'étaient que la **marge résiduelle**, pas le déficit. Elle récupère ≈ 1,5 à 2 Ko contre 17,7 Ko manquants : tuée par le calcul. (À titre de repère, 17 700 o contre ≈ 2 700 o alloués par la route : le déficit est plusieurs fois ce que la route consomme, donc ce n'est pas de la consommation.)
- **Réessayer** (rejetée) : attendre **nourrit la file** de paquets retenus, donc aggrave le déficit au lieu de le résorber.
- **Baisser le seuil** (rejetée) : il protège d'un OOM réel ([bug-108](../../specs/features/done/bug-108-garde-tls-ota-sur-metrique-ira-inclusive.md), [bug-165](../../specs/features/done/bug-165-recherche-de-mise-a-jour-en-ligne-echoue.md)) ; rouvrirait ce défaut en faisant un handshake à 25 Ko libres.
- **Tâche dédiée** (rejetée) : résout le blocage mais immobilise 8 Ko de RAM interne en permanence sur un appareil dont c'est la ressource rare.

## Conséquences

### Positives
- Une seule session par requête : fenêtre de blocage ≈ 4,5 s au lieu de ≈ 11 s ; `/check-update` ne dépend plus de la mémoire pour relayer.
- `absent` (release non signée, cas **nominal** du parc) et `unavailable` (anormal) deviennent distinguables : **la condition C2 de feature-151 est levée**.
- Le WARN de saut porte ses mesures : un saut est diagnosticable à distance.
- Au passage : `setRedirectLimit(1)` ferme un défaut préexistant (10 redirections possibles à l'intérieur d'un seul `GET()`, donc sans nourrir le TWDT — pire cas ≈ 90 s contre 30 s), et `/download-update` reçoit enfin `setHandshakeTimeout` / `setConnectTimeout` (handshake à 120 s par défaut).

### Négatives / dette assumée
- **+1 aller-retour par genre** installé, un endpoint de plus, et un **état inter-requêtes** (`g_sigSource`, 2 emplacements) qui n'existait pas.
- **+432 o de `.bss`**.
- ⚠️ **Marge honnête : facteur 1,6 seulement** au débit du terrain (2,4× en laboratoire). C'est un passage, pas un confort : avec deux téléphones ouverts, un saut reste possible — d'où le maintien de la garde et du WARN.
- ⚠️ **`/download-update` garde une fenêtre de blocage bien plus large** (le téléchargement lui-même, plusieurs dizaines de secondes). **Non traité ici, assumé explicitement.**
- Contrat `/check-update` : les 4 champs `*_manifest` / `*_signature` subsistent mais sont **toujours vides** (changement de comportement pour un client existant).

### Ce que ça verrouille
- Tout futur appel sortant TLS dans un handler suit la règle « une session par requête » ; le `static_assert` de `constants.h` en porte la borne.
- 🔴 **Contrainte d'ordre : le lot D3-c de `feature-151` ne doit PAS sortir dans le même firmware que cette décision.** Une page ancienne (filesystem non encore mis à jour) enverrait des paramètres `manifest`/`signature` vides et serait refusée — **y compris pour installer le filesystem qui la corrigerait**. D3-c attend une release dont le filesystem porte la page neuve, et une voie de réparation indépendante de la page doit être documentée.

## Références

- Code : `src/web_routes_ota.cpp` (`handleOtaSig`, `storeSigSource`, `relayOtaSigFile`), `src/ota_sig_relay_logic.{h,cpp}`, `src/constants.h` (`kOtaMaxTlsSessionsPerRequest`, `kOtaMaxRedirectsPerGet`, `kOtaSigMinIntervalMs`), `data/app.js` (`fetchSigParams`, `renderSigNote`)
- Spec : [`bug-176`](../../specs/features/done/bug-176-relais-sig-filesystem-saute-sous-pression-memoire.md), [`feature-151`](../../specs/features/done/feature-151-signature-de-l-image-ota.md)
- ADR liés : [ADR-0058](0058-gardes-heap-metrique-8-bits.md), [ADR-0098](0098-toute-constante-de-mesure-declare-sa-source.md), [ADR-0121](0121-fichier-sig-conteneur-de-transport-deux-lignes-jamais-reconstruit.md), [ADR-0122](0122-socle-decision-ota-genre-empreinte-relais-sig.md)
- Autres specs : bug-108, bug-165 (gardes TLS) ; bug-170 (cité par bug-176 comme « même motif » — la voie `littlefs.bin` meurt en silence)
