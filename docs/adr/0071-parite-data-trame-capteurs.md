# ADR-0071 — Parité `/data` ↔ trame `sensor_data` : marqueurs, sentinelle, exceptions motivées

- **Statut** : Accepté
- **Date** : 2026-09-07
- **Décideurs** : architect, embedded-developer, web-ui-developer, test-engineer, code-reviewer, doc-writer
- **Spec(s) liée(s)** : [feature-092](../../specs/features/done/feature-092-carte-debit-injection-ph-orp.md)
- **Étend** : [ADR-0065](0065-trame-websocket-etat-mutable-pas-constantes.md) (parité de l'axe `config`)

## Contexte

[ADR-0065](0065-trame-websocket-etat-mutable-pas-constantes.md) a posé une règle de parité **et** sa sentinelle — mais **uniquement pour l'axe `config`** (`GET /get-config` ↔ trame WebSocket `config`). Le second axe, `GET /data` ↔ trame `sensor_data`, n'avait **aucune garde**.

Or c'est **cet axe-là** qui concentre les récidives du projet. **Trois** occurrences documentées, en trois versions :

| Version | Champs | Symptôme |
|---|---|---|
| 2.21.2 | 4 pentes de sonde pH (`phSlope*`) | servies en WS seulement — un client en polling HTTP ne pouvait jamais afficher la santé de sonde |
| 2.23.0 | caractérisation des pompes | même famille |
| 2.25.0 | indicateurs de garde de dosage | même famille |

À chaque fois, la leçon retenue fut « penser à vérifier des deux côtés » : une **discipline orale**, donc oubliée trois fois. Et c'est le piège n° 1 inscrit dans la mémoire de projet — *un champ servi d'un seul côté ne disparaît pas, il **CLIGNOTE***, parce que `latestSensorData` est **remplacé en entier** par chaque charge `/data` (aucune fusion).

**Le critère de partage d'ADR-0065 ne peut pas être transposé tel quel.** « La trame porte l'état mutable à l'exécution et non dérivable » discrimine efficacement sur l'axe config, où la moitié des clés sont des constantes de compilation. Sur l'axe capteurs, il ne discrimine **rien** : la trame porte de la **mesure**, et tout y est mutable par nature. Il fallait donc un critère et un mécanisme **différents**, pas une simple extension de portée.

Deuxième asymétrie, découverte pendant le cycle : sur l'axe config, le danger n'a **qu'un sens** (une clé WS-seule est écrasée par la charge `/get-config`). Sur l'axe capteurs, il en a **deux** — le push WebSocket efface aussi une clé `/data`-seule. Une règle « aucune clé WS-seule » aurait laissé passer la moitié des cas.

Il fallait trancher **maintenant** parce que feature-092 ajoutait 8 clés à cet axe précis, et corrigeait au passage 4 clés WS-seules préexistantes (la pause mélange) : sans garde écrite, ces corrections auraient été refaites au jugé, et la 4ᵉ récidive n'était qu'une question de temps.

## Décision

**L'axe `GET /data` ↔ trame `sensor_data` est régi par un bloc à marqueurs textuels, gardé par une sentinelle native, avec une table d'exceptions à motif obligatoire.**

Quatre éléments, indissociables :

1. **Marqueurs commentés** `>>> PARITE-DATA:DEBUT` / `<<< PARITE-DATA:FIN` dans `handleGetData` ([`src/web_routes_data.cpp`](../../src/web_routes_data.cpp)) et `_buildSensorJson` ([`src/ws_manager.cpp`](../../src/ws_manager.cpp)). Entre ces marqueurs, **toute** clé s'écrit avec un **receveur unique** (`doc[` côté HTTP, `d[` côté WS) et les deux côtés portent **exactement le même jeu de clés**, asserté **dans les deux sens**. Les bornes de **portée** sont des signatures de fonction (`static void handleGetData(` … `static void handleGetKpi(`), pas des numéros de ligne. Receveurs **imbriqués exclus** (`faObj[`, objet `flow_assist`) : les compter polluerait le diff de premier niveau.

2. **Sentinelle native** [`test/test_native_ws_data_parity/`](../../test/test_native_ws_data_parity/) — **9 cas**, sur le patron de `test_native_ws_config_parity`. Lecture **textuelle** du source, coût **zéro octet de flash** ([ADR-0017](0017-logique-metier-pure-humble-object-testabilite.md)). Marqueur absent ⇒ échec **explicite**, message « restaurer le marqueur, **ne pas désactiver le test** ».

3. **Table d'exceptions à motif obligatoire**, en **trois** catégories — parce que la nature des exceptions diffère et qu'un seul bac les rendrait toutes également excusables :

   | Catégorie | Compte | Nature |
   |---|---:|---|
   | `kWsOnlyLive` | **18** | état vivant servi par **aucune** route HTTP — **dette inventoriée**, antérieure au cycle |
   | `kWsPlusOtherRoute` | **8** | clés **dual-sourcées** : absentes de `/data`, servies par une **autre** route HTTP dont le **nom est obligatoire** et asserté non vide (7 par `GET /get-config`, 1 par `GET /get-logs`). Catégorie qui **n'existe pas** sur l'axe config |
   | `kDataOnly` | **3** | clés servies par `/data` seul — le **sens inverse**, même effet |

   Chaque entrée porte un motif écrit de **≥ 20 caractères**, asserté, sans doublon.

4. **Échec sur exception MORTE ou CONTREDITE**, et **cliquet** sur la catégorie « état vivant » : `kWsOnlyLiveCeiling = 18` — le compte ne peut **jamais augmenter**. Une clé neuve va dans `/data`, jamais dans cette table. Le cliquet transforme une dette en **plafond décroissant**, au lieu d'un permis de croître.

> **Le fait qui justifie cet ADR mieux que tout argument : la sentinelle a détecté un défaut RÉEL pendant ce cycle même, avant la revue.**
>
> `ph_pump_flow_uncharacterized` / `orp_pump_flow_uncharacterized` étaient `/data`-**seuls**. Le pied de carte « pompe non caractérisée » les lisait depuis `latestSensorData` : la clé valait donc `undefined` après **chaque** push WS (~5 s), le pied restait masqué (`!== true`), et ne réapparaissait qu'après une charge `/data` (~30 s). Le pied **clignotait**.
>
> C'est le **piège n° 1 du projet dans son sens INVERSE** — d'ordinaire c'est la charge `/data` qui écrase un champ WS-seul ; ici c'est le push WS qui effaçait un champ `/data`-seul. L'axe config n'avait **pas** à traiter ce sens : c'est exactement ce que le second axe ajoute. Et le défaut avait été **introduit par une demande de l'agent principal** du cycle, dans une revue qui l'avait laissé passer.
>
> Le correctif est la **parité** (migration des deux booléens dans le bloc `PARITE-DATA`), pas une mémorisation côté client. La sentinelle est alors **repassée au rouge une seconde fois**, sur les deux exceptions devenues **mortes** : le mécanisme prévu pour empêcher la table de pourrir a fonctionné **du premier coup, sur un vrai changement** (`kDataOnly` : 5 → 3 entrées).

## Alternatives considérées

- **Étendre la règle d'ADR-0065 sans nouveau mécanisme** (rejetée) — le critère « mutable à l'exécution et non dérivable » ne discrimine rien sur une trame de **mesure** : il aurait déclaré tout légitime des deux côtés, sans jamais rien signaler. Une règle qui n'écarte aucun cas n'est pas une règle.
- **Règle « aucune clé WS-seule », symétrique de l'axe config** (rejetée) — elle aurait signalé à tort les **8** clés dual-sourcées (bidon, éclairage, boost, mode d'installation, état MQTT, `uptime_ms`), toutes légitimement absentes de `/data` parce qu'une autre route les sert. Et, surtout, elle n'aurait **rien vu** du défaut réel du cycle, qui était dans l'autre sens.
- **Une seule table d'exceptions** (rejetée) — mélanger une **dette** (18 clés WS-seules, dont plusieurs dégradent réellement l'affichage après une charge `/data`) avec des **partages légitimes** (8 clés dual-sourcées) et le **sens inverse** (3 clés `/data`-seules) rendrait le cliquet impossible à poser : on ne peut borner que ce qu'on a isolé. Trois catégories, trois régimes.
- **Parité intégrale sans exception** (rejetée) — les 18 clés WS-seules d'état vivant (compteurs de fenêtre horaire, comptes à rebours à la seconde, échos de commande) n'ont **aucun consommateur HTTP** ; les servir sur `/data` coûterait du budget pour rien, et le budget de la trame capteurs est précisément à **137 o** du plafond dur. La dette est **inventoriée et bornée**, pas effacée.
- **Sentinelle par analyse syntaxique C++** (rejetée) — même arbitrage qu'ADR-0065 : coût et fragilité sans commune mesure avec le gain. L'angle mort est **connu et écrit** (une clé commentée entre les marqueurs reste comptée) : la sentinelle vise **la distraction, pas la malveillance**.
- **Mémorisation côté client (fusion des charges au lieu du remplacement)** (rejetée) — c'était le correctif « facile » du défaut découvert. Il aurait masqué la classe entière de défauts au lieu de la supprimer, et rendu indétectable la prochaine clé mono-sourcée. On corrige la **source**, pas le symptôme.
- **Élaguer la trame pour faire de la place** (rejetée dans ce cycle) — retirer `ph_integral` et `ph_integral_term` de la seule trame WS rendrait **49 o** (marge 137 → 186 o) et ces deux clés n'ont plus **aucun consommateur client**. Refusé parce que cela les rendrait `/data`-seules, soit **deux exceptions de plus dans la sentinelle, sur l'axe qui vient de faillir, le jour même où l'on installe la garde**. L'autre voie — relever `kWsFrameBufBytes` — coûterait **+512 o de `.bss`** pendant la famine mémoire **active** documentée par bug-080. Le chiffre est consigné pour la prochaine feature qui manquera de place.

## Conséquences

### Positives
- La **4ᵉ récidive** du motif sur cet axe est structurellement empêchée : toute divergence casse **au build des tests natifs**, avec un message qui dit **quoi faire**.
- Le **sens inverse** du piège est couvert pour la première fois — et il a été **prouvé utile immédiatement**.
- La table d'exceptions ne peut pas pourrir : une exception **morte** ou **contredite** est un échec.
- Le **cliquet** rend la dette décroissante par construction. Ajouter une clé WS-seule exige de **relever un plafond** — donc de l'écrire et de le défendre.
- Le décompte est **vérifié par mesure**, pas supposé : 26 clés WS-seules constatées = 18 « état vivant » + 8 dual-sourcées.
- Coût **zéro octet de flash**.

### Négatives / dette assumée
- **Quatre** choses doivent rester synchronisées pour toute nouvelle clé : le bloc HTTP, le bloc WS, la table d'exceptions adéquate et le cliquet. La sentinelle rend l'oubli bruyant, elle ne le supprime pas.
- Les **marqueurs commentés** et les **signatures de fonction** de portée (`handleGetData`, `handleGetKpi`, `_buildSensorJson`, `_buildConfigJson`) deviennent des éléments de contrat : les déplacer ou les renommer fait échouer la sentinelle (échec explicite, mais il faut savoir le lire).
- **Angle mort textuel** : une clé mise en commentaire entre les marqueurs reste comptée comme présente.
- **18 clés WS-seules restent une dette réelle**, pas un satisfecit : plusieurs sont lues depuis `latestSensorData` avec un repli silencieux (`|| 0`, `=== true`), donc **dégradent l'affichage** après chaque charge `/data`. Les motifs le disent, clé par clé. Leur résorption relève de specs dédiées.
- La marge de trame est tombée à **137 o** : tout ajout futur sur cet axe exige un retrait compensatoire ou un relèvement de `kWsFrameBufBytes` — décision d'ADR, pas de routine.

### Ce que ça verrouille
- **Les deux axes de parité du projet sont désormais gardés**, chacun par sa sentinelle et son critère propre. Il n'y a pas de troisième axe non gardé : `/get-kpi` et `/get-logs` n'ont pas de pendant WebSocket.
- **Le sens interdit devient bidirectionnel** sur l'axe capteurs : ni WS-seule non déclarée, ni `/data`-seule non déclarée.
- **Le plafond `kWsOnlyLiveCeiling = 18`** : le franchir est un acte explicite, pas un effet de bord.
- **L'obligation de motif écrit (≥ 20 caractères) et de route nommée** pour toute exception. Une exception sans motif est indiscernable d'un vrai trou de parité — c'est le sens de l'assertion.
- **La méthode de chiffrage du budget de trame** : pire cas **structurel**, jamais une mesure vivante (voir la rectification de +115 o sur les deux `*DoseBlockedReason`, [ws-manager.md](../subsystems/ws-manager.md#deux-trames-deux-gabarits--un-seul-plafond-dur-à-lire-avant-tout-chiffrage)).

## Références

- Code : [`src/web_routes_data.cpp`](../../src/web_routes_data.cpp) (`handleGetData`, marqueurs `PARITE-DATA`), [`src/ws_manager.cpp`](../../src/ws_manager.cpp) (`_buildSensorJson`, marqueurs + bloc de dimensionnement), [`src/constants.h`](../../src/constants.h) (`kWsFrameBufBytes`, `kDoseBlockedReasonMaxLen`), [`data/app.js`](../../data/app.js) (`_renderDoseUncharacterizedFoot`)
- Test : [`test/test_native_ws_data_parity/test_ws_data_parity.cpp`](../../test/test_native_ws_data_parity/test_ws_data_parity.cpp)
- Spec : [`specs/features/done/feature-092-carte-debit-injection-ph-orp.md`](../../specs/features/done/feature-092-carte-debit-injection-ph-orp.md)
- Doc : [`docs/subsystems/ws-manager.md`](../subsystems/ws-manager.md#le-second-axe-de-parité--data--sensor_data-parite-data), [`docs/API.md`](../API.md#champs-de-composition-du-débit-dinjection-feature-092-v2460)
- ADR liés : [ADR-0065](0065-trame-websocket-etat-mutable-pas-constantes.md) (axe `config`, décision étendue ici), [ADR-0005](0005-websocket-push-sans-polling.md) (push sans polling), [ADR-0017](0017-logique-metier-pure-humble-object-testabilite.md) (logique pure, tests natifs), [ADR-0048](0048-seuils-ph-fixes-derives-de-la-plage-de-consigne.md) (clé absente ≠ `null` ≠ `0`)
