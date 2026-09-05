# ADR-0065 — La trame WebSocket porte l'état mutable, pas les constantes

- **Statut** : Accepté
- **Date** : 2026-09-05
- **Décideurs** : architect, web-ui-developer, code-reviewer, doc-writer
- **Spec(s) liée(s)** : [bug-089](../../specs/features/done/bug-089-parite-ws-config-champs-manquants.md)

## Contexte

`GET /get-config` sert **89** clés ; la trame WebSocket `config` (`WsManager::_buildConfigJson()`) n'en portait que **66**. Après un changement de configuration, les **autres** clients connectés ne voyaient pas les 23 clés manquantes se rafraîchir avant leur prochain `/get-config` — jauge de bidon gelée, bascule automatique de sécurité pH invisible, vitesse de régulation périmée.

C'est la **quatrième occurrence documentée** du motif de parité dans ce dépôt (pentes de sonde en 2.21.2, caractérisation de pompes en 2.23.0, indicateurs de garde de dosage en 2.25.0, puis ici la trame `config`). À chaque fois, la leçon retenue a été « penser à vérifier des deux côtés » : une **discipline orale**, donc oubliée quatre fois.

**La correction évidente — rétablir l'égalité brute des deux ensembles — aurait été pire que le bug.** Les 23 clés coûtent **+1 365 o** (662 o pour les 21 clés plates, 178 o `regulation_defaults`, 525 o `regulation_defaults_by_size`) sur une trame construite dans le buffer statique `sWsFrameBuf`, dont le plafond `kWsFrameBufBytes = 3584` o est le **seul plafond dur** de ce chemin. Sa garde `measureJson(doc) + 1 > cap` ne **tronque pas** : elle **abandonne le push** et incrémente `_oversizedFrames`. Rétablir la parité intégrale, c'était donc remplacer *quelques champs manquants chez certains clients* par *une trame absente pour tous les clients*.

Il fallait trancher **maintenant** parce que le cycle ajoutait des clés à la trame : sans critère écrit, chaque ajout futur aurait à nouveau été arbitré au jugé, à quelques centaines d'octets d'un plafond dur.

## Décision

**La trame WebSocket `config` porte l'état MUTABLE À L'EXÉCUTION et NON DÉRIVABLE. Tout le reste est HTTP-seul et DÉCLARÉ.**

Application au cycle bug-089 :

- **15 clés ajoutées** à `_buildConfigJson()` : `max_cycles_per_day`, `regulation_speed`, `pool_size`, `ph_auto_manual_epoch`, `ph_auto_manual_reason`, et les 10 clés de suivi produit pH/ORP (`*_tracking_enabled`, `*_container_ml`, `*_total_injected_ml`, `*_remaining_ml`, `*_alert_threshold_ml`).
- **8 clés déclarées en exception** : les 6 bornes de réglage pH (`ph_target_min/max/step`, `ph_offset_min/max/step`) et `regulation_defaults_by_size` sont des **constantes de compilation** — un client les lit une fois, les pousser en boucle dépense le budget de trame pour rien ; `regulation_defaults` est **dérivable** côté client depuis `pool_size` (désormais dans la trame) et le cache des colonnes `by_size`.

**La règle est rendue exécutoire par une sentinelle** — c'est le cœur de cette décision, pas une annexe. Une règle sans garde a déjà échoué quatre fois. `test/test_native_ws_config_parity/` lit le **texte source** des deux fichiers entre des **marqueurs commentés** (`>>> PARITE-CONFIG:DEBUT` / `<<< PARITE-CONFIG:FIN`), et non des bornes de fonction qui casseraient au premier refactor. Quatre assertions :

1. **clé HTTP non couverte → échec** (ni dans la trame, ni déclarée) ;
2. **clé WS-seule → échec** — sens **dangereux** du motif : un champ WS-seul ne disparaît pas, il **clignote**, la charge `/get-config` l'écrasant périodiquement ;
3. **marqueur absent → échec explicite**, avec le message « restaurer le marqueur, **ne pas désactiver le test** » (plus un plancher de 60 clés extraites, contrôle négatif contre un extracteur silencieux) ;
4. **exception morte → échec** — une clé exemptée qui n'existe plus côté HTTP. C'est elle qui **empêche la liste de pourrir** : sans elle, la table serait devenue en un an un cimetière de justifications périmées.

Chaque exception porte un **motif écrit d'au moins 20 caractères**, asserté. Coût de la sentinelle : **zéro octet de flash** (test natif, [ADR-0017](0017-logique-metier-pure-humble-object-testabilite.md)).

## Alternatives considérées

- **Parité intégrale des 23 clés** (rejetée) — **+1 365 o**. La trame pire cas passerait à ≈ 3 447 o pour un plafond dur de 3 584 : **marge ~100 o**. Le premier topic MQTT un peu long saisi par l'utilisateur (`server`, `topic`, `username`, `wifi_ssid` ne sont bornés **nulle part** par `/save-config`) ferait franchir le plafond, et la trame `config` cesserait **silencieusement** d'être livrée à tous les clients. On aurait acheté la parité au prix du canal.
- **Sortir `fillRegulationDefaults` de `web_routes_config.cpp` pour alimenter aussi la trame** (rejetée) — **+703 o** de trame permanents pour une information que le client **sait reconstruire** (`pool_size` + cache `by_size`, `syncRegulationDefaults()`). Payer un budget contraint pour une valeur dérivable est le contraire du critère retenu.
- **Relever `kWsFrameBufBytes`** (rejetée) — `sWsFrameBuf` est un tableau **statique en `.bss`** : +512 o de **RAM permanents**, sur un appareil dont les incidents récurrents sont des famines de tas 8 bits ([ADR-0058](0058-gardes-heap-metrique-8-bits.md), [ADR-0061](0061-bascule-psram-au-dela-de-4095-octets-reserve-parachute-en-interne.md)). Dépenser de la RAM permanente **pour transporter des constantes** est un mauvais échange ; la constante reste disponible pour de l'état réellement nouveau.
- **Trame `config` différentielle** (rejetée) — le client devrait alors distinguer « champ inchangé » de « champ absent », exactement l'ambiguïté que le projet combat depuis 2.21.2 (clé absente ≠ `false`, clé absente ≠ `null`, [ADR-0048](0048-seuils-ph-fixes-derives-de-la-plage-de-consigne.md)). Un snapshot complet est idempotent : un push raté n'a aucune conséquence.
- **Sentinelle par analyse syntaxique C++** (rejetée) — coût et fragilité sans commune mesure avec le gain. L'extraction textuelle a un angle mort **connu et écrit** (une clé commentée entre les marqueurs reste comptée) : elle vise **la distraction, pas la malveillance**, ce qui est le bon périmètre.
- **Rien de plus qu'une consigne écrite dans `CLAUDE.md`** (rejetée) — c'est précisément ce qui existait, et le motif en est à sa quatrième occurrence.

## Conséquences

### Positives
- Les clients connectés voient la jauge de bidon, la vitesse de régulation, la taille de bassin, la garde anti-cycling et la **bascule automatique de sécurité pH** se rafraîchir sans rechargement.
- Le budget de trame reste **franc** : pire cas ≈ **2 948 o** pour 3 584, **marge 636 o**. Le gabarit `StaticJson<>` passe de 2560 à 2944 — **documentaire** en ArduinoJson 7, donc **0 octet de RAM**.
- Toute divergence future casse **au build des tests natifs**, avec un message qui dit **quoi faire**. La 5ᵉ occurrence du motif est structurellement empêchée.
- La liste d'exceptions ne peut plus pourrir : une exception morte ou contredite est un échec.

### Négatives / dette assumée
- **Trois** choses doivent rester synchronisées pour toute nouvelle clé : le bloc HTTP, le bloc WS, et la table d'exceptions. La sentinelle rend l'oubli bruyant, elle ne le supprime pas.
- Les **marqueurs commentés** deviennent un élément de contrat : les déplacer ou les supprimer rend la sentinelle aveugle (échec explicite, mais il faut savoir le lire).
- **Angle mort textuel** : une clé mise en commentaire entre les marqueurs reste comptée comme présente.
- **Réserve de dimensionnement** : la marge de 636 o est prouvée sous l'hypothèse d'environ 64 o pour les champs texte libres non bornés (`server`, `topic`, `username`, `wifi_ssid`). Surveiller `oversizedFrameCount()`.
- Le pire cas de longueur (`ph_auto_manual_reason`, **427 o**) **coïncide avec l'escalade de sécurité pH** — le moment où perdre la trame coûterait le plus cher.

### Ce que ça verrouille
- **Le contrat public de la trame `config`** : elle n'est **pas** un miroir de `/get-config` et ne le redeviendra pas. Un intégrateur qui lit la trame doit continuer d'appeler `/get-config` pour les constantes.
- **Le critère d'arbitrage de tout ajout futur** : « mutable à l'exécution et non dérivable ? » est désormais la question à laquelle il faut répondre par écrit, dans la trame ou dans la table d'exceptions.
- **Le sens interdit** : aucune clé ne peut être WS-seule. Une exception ne peut aller que dans le sens HTTP-seul.
- **La convention de nommage du receveur** (`doc[` côté HTTP, `d[` côté WS) et l'emplacement des blocs entre marqueurs.
- **Le chiffrage par littéral, jamais par estimation** : la longueur d'une chaîne de trame se reconstruit depuis son unique écrivain (voir la rectification d'un facteur 2,7 sur `ph_auto_manual_reason`).

## Références

- Code : [`src/ws_manager.cpp`](../../src/ws_manager.cpp) (`_buildConfigJson()`, marqueurs + bloc de dimensionnement), [`src/web_routes_config.cpp`](../../src/web_routes_config.cpp) (`handleGetConfig()`, marqueurs), [`src/constants.h`](../../src/constants.h) (`kWsFrameBufBytes`), [`data/app.js`](../../data/app.js) (`syncRegulationDefaults()`)
- Test : [`test/test_native_ws_config_parity/test_ws_config_parity.cpp`](../../test/test_native_ws_config_parity/test_ws_config_parity.cpp)
- Spec : [`specs/features/done/bug-089-parite-ws-config-champs-manquants.md`](../../specs/features/done/bug-089-parite-ws-config-champs-manquants.md)
- Doc : [`docs/subsystems/ws-manager.md`](../subsystems/ws-manager.md#champs-config-ajoutés-en-bug-089--parité-de-la-trame-v2390), [`docs/API.md`](../API.md#websocket-temps-réel)
- ADR liés : [ADR-0005](0005-websocket-push-sans-polling.md) (push sans polling), [ADR-0017](0017-logique-metier-pure-humble-object-testabilite.md) (tests natifs), [ADR-0032](0032-reponses-http-bornees.md) (réponses bornées, extension WS), [ADR-0048](0048-seuils-ph-fixes-derives-de-la-plage-de-consigne.md) (parité des canaux, clé absente ≠ valeur)
