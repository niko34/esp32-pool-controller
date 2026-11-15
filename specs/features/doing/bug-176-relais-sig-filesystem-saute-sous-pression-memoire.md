## Métadonnées
- **ID** : bug-176
- **Slug** : relais-sig-filesystem-saute-sous-pression-memoire
- **Type** : bug
- **Statut** : doing
- **Priorité** : haute
- **Version cible** : 2.88.0
- **Spec(s) liée(s)** : `feature-151` (signature de l'image OTA, lot D3-a livré en 2.87.0 —
  cette spec est **bloquante pour D3-c**), `bug-169` (même motif : la voie `littlefs.bin` meurt en silence
  pendant que le firmware fonctionne — six semaines sans que personne ne le voie)

## Domaines touchés
- [ ] Capteurs (pH / ORP / température)
- [ ] Régulation (pump_controller, PID)
- [ ] Filtration / Éclairage
- [x] Web — routes HTTP / WebSocket
- [ ] MQTT / Home Assistant
- [ ] UART / écran LVGL
- [x] OTA
- [ ] Authentification
- [ ] Historique / persistance
- [x] Frontend (`data/`)

## Agents requis
- [x] spec-reader
- [x] architect
- [ ] pool-chemistry
- [x] ux-ui-designer
- [x] embedded-developer
- [x] web-ui-developer
- [x] test-engineer
- [x] code-reviewer
- [x] doc-writer

⚠️ `security-reviewer` : **obligatoire** (modification de `web_routes_ota.cpp`, chaîne OTA).

## Description
Sur un `/check-update` réel, le relais du fichier `.sig` du **filesystem** a été **sauté**
par la garde mémoire TLS, laissant les champs vides. La page a donc installé l'image du
système de fichiers **sans signature**, alors que celle du firmware a bien été vérifiée.

## Contexte

**Relevé de terrain du 2026-11-15**, premier essai réel de la mise à jour en ligne signée
(appareil 2.87.0 → 2.87.1, release v2.87.1 signée).

```
12:19:37 INFO /check-update : relais .sig firmware   — validé (612 o)
12:19:37 WARN /check-update : relais .sig filesystem — garde TLS fermée (mémoire)
                                                       — relais sauté, champs vides
```

Conséquence mesurée à l'installation, dans le journal persistant :

```
12:32:16 WARN [feature-151 D1] /download-update — authenticité OTA : ABSENTE
                                                   — D3 refuserait : OUI
12:32:25 INFO [feature-151 D1] /download-update — authenticité OTA : valide
                                                   — D3 refuserait : non
12:32:55 INFO [feature-151 D1 B1] /download-update — liaison manifeste/image : LIÉE
```

⇒ **Le firmware a été authentifié, le filesystem non.** La ligne le dit explicitement :
en D3-c, cette installation de filesystem **aurait été refusée**.

### Pourquoi la garde s'est fermée — les chiffres

Seuils (`src/constants.h:1304`, `:1322`, `:1335`) confrontés aux mesures du moment :

| Mesure | Seuil | Valeur | Verdict |
|---|---|---|---|
| Heap interne 8 bits **libre** | 32 768 o | **31 024 o** | ❌ sous le seuil de **1 744 o** |
| Plus gros bloc interne 8 bits | 24 576 o | 28 660 o | ✅ |
| Plus gros bloc PSRAM | 40 960 o | 4 128 756 o | ✅ |

**Un seul critère a cédé, et de 5 %.**

Cause structurelle : `/check-update` ouvre **trois sessions TLS sortantes successives dans
une seule requête HTTP** — l'API GitHub, puis le `.sig` du firmware, puis celui du
filesystem (`web_routes_ota.cpp:1775-1786`). Le heap interne ne remonte pas à son niveau
d'entrée entre les sessions : relevé 47 388 o avant la requête, 31 024 o au moment du
troisième appel. **Le dernier relais paie la facture des deux premiers.**

⚠️ **Le défaut est INTERMITTENT.** Un `/check-update` lancé en `curl` quelques minutes plus
tôt le même jour avait ramené les **deux** signatures. C'est le pire cas de figure : livré
tel quel en D3-c, il produirait un **refus aléatoire** des mises à jour du filesystem.

### Deux défauts révélés au passage

1. **Le WARN ne journalise aucune mesure** (`web_routes_ota.cpp:1296-1298`) : ni heap, ni
   seuil, ni motif discriminé. Le diagnostic n'a été possible que parce que la sonde
   « Chute maxBloc8 » s'est déclenchée à la même seconde, par coïncidence. Sans elle,
   le défaut était **indiagnosticable à distance**.
2. **La condition C2 de feature-151 est démontrée** : « release non signée » et « relais
   échoué » produisent tous deux des champs vides, et la page ne peut pas les distinguer.
   En D3-c elle refuserait sans pouvoir dire pourquoi — ⛔ la phrase qu'on ne peut pas
   donner à un client dont l'appareil est bloqué.

## Comportement attendu

1. **Un `/check-update` sur une release signée ramène les DEUX signatures**, de façon
   reproductible, dans les conditions mémoire nominales de l'appareil.
2. Quand un relais ne peut malgré tout pas aboutir, **la réponse dit lequel et pourquoi**,
   avec un code machine distinguant au minimum :
   - `absent` — la release ne porte pas de `.sig` (release antérieure à feature-151) ;
   - `indisponible` — le relais a échoué ou a été sauté (mémoire, réseau, HTTP, parse).
   ⇒ Ces deux états ⛔ ne doivent plus être confondus dans un champ vide.
3. **Le WARN de saut porte ses mesures** : les trois valeurs et les trois seuils, comme le
   fait déjà `refuseIfTlsGateClosed` (`web_routes_ota.cpp:1194-1202`).
4. L'interface **ne lance pas une installation de filesystem sans signature en la
   présentant comme normale** quand la release en possède une.

### Cas limites
- Release **sans** `.sig` (antérieure à v2.87.1) : comportement inchangé, installation
  possible, aucune alarme — c'est le cas nominal du parc existant.
- Release avec `.sig` **firmware seulement** (asset manquant) : doit se distinguer d'un
  relais échoué.
- Mémoire réellement basse : ⛔ aucun refus neuf en dehors du périmètre de D3-c — ce lot
  **n'introduit aucun refus**, il rétablit la disponibilité de l'information et sa lisibilité.
- Deux `/check-update` concurrents : la garde et le tampon statique `g_sigFetchBuf`
  (`web_routes_ota.cpp:1282`) sont partagés — vérifier qu'aucun entrelacement n'est possible.

## Comportement actuel
`relayOtaSigFile()` interroge la garde TLS et, si elle est fermée, **retourne sans rien
tenter**, en journalisant un WARN sans chiffres. Les quatre champs de relais partent vides
dans la réponse JSON. Côté page, `sigParams()` (`data/app.js:10779`) envoie **les deux
paramètres ou aucun** — comportement correct en soi — et l'installation se fait donc sans
manifeste ni signature, sans que rien ne le signale à l'utilisateur.

## Contraintes techniques
- ESP32 WROVER-E, PSRAM présente ; heap interne 8 bits = la ressource rare (ADR-0058).
- ⛔ **La métrique qui compte est le heap 8 bits**, pas `ESP.getFreeHeap()` (bug-108).
- AsyncWebServer : aucun handler bloquant > 50 ms — ⚠️ la route déroge déjà (sessions TLS
  synchrones) ; ⛔ ne pas aggraver, et conserver les `esp_task_wdt_reset()` avant chaque GET.
- ⛔ **Borne de blocage** : le `static_assert` de `constants.h:1436` fige l'attente la plus
  longue sans nourrir le watchdog à 19 s < 30 s. ⚠️ Toute session TLS ajoutée ou déplacée
  doit le ré-honorer.
- Allowlist d'hôtes conservée avant toute connexion sortante.
- ⛔ **Aucun refus neuf** : ce lot reste dans la discipline D3-a/D3-b.
- Budget FS `spiffs` 512 KB — vérifier `./build_fs.sh` si `data/` grossit.

## Pistes relevées (⚠️ à arbitrer par `architect`, aucune n'est retenue d'avance)
1. **Relais à la demande** : `/check-update` ne ramène que les URL ; la page demande la
   signature juste avant d'installer, dans une requête HTTP distincte ⇒ **une seule session
   TLS par requête**, heap frais à chaque fois. Coût : un aller-retour de plus, un endpoint
   de plus.
2. **Libérer avant de relayer** : identifier ce qui retient ~16 Ko entre l'entrée de la
   requête et le troisième appel (`JsonDocument doc` et son `BudgetJsonAllocator` sont
   encore vivants pendant les relais, `web_routes_ota.cpp:1703-1786`), et relayer dans le
   creux réel. ⚠️ **À mesurer, pas à supposer.**
3. **Réessai borné** du relais sauté après libération explicite.
4. **Recalibrer le seuil** : ⛔ piste la moins solide — le seuil protège d'un OOM réel
   (bug-108, bug-165) et le baisser à l'aveugle rouvrirait ce défaut.

## Fichiers potentiellement concernés
- `src/web_routes_ota.cpp` — `relayOtaSigFile()` (`:1284`), son site d'appel (`:1775-1786`),
  la construction de la réponse (`:1799-1807`)
- `src/constants.h` — seuils, borne de blocage, éventuelles constantes neuves
- `data/app.js` — `sigParams()` (`:10779`), les deux appels `/download-update`
  (`:10946`, `:10999`), le message affiché à l'utilisateur
- `docs/API.md` — contrat de `/check-update` (champs de relais et leur statut)
- `docs/subsystems/ota-manager.md` — relais `.sig` et garde mémoire
- `specs/features/doing/feature-151-*.md` — lever la condition C2

## Livrables — check-list
- [ ] A1 — Les deux `.sig` d'une release signée sont relayés de façon reproductible
- [x] A2 — Statut de relais par genre dans la réponse, distinguant « absent » de « indisponible »
- [x] A3 — WARN de saut enrichi des trois mesures et des trois seuils
- [x] A4 — Mesure on-target du heap aux trois sessions, consignée dans la spec
- [x] B1 — La page exploite le statut et ne masque pas une installation non signée
- [x] C1 — `docs/API.md` + `docs/subsystems/ota-manager.md` + `CHANGELOG.md`
- [x] C2 — Condition C2 de feature-151 levée et tracée dans sa spec
- [x] D1 — Suite native sur la décision de relais (présent / absent / sauté)
- [ ] D2 — Validation par MUTATION des tests neufs
- [ ] D3 — Essai de terrain : `/check-update` puis installation complète, verdicts relevés

## Nouvelles signatures / API
- Champs additifs probables dans `/check-update` : statut de relais par genre
  (⚠️ **additifs** — un client qui les ignore doit continuer de fonctionner)
- Endpoint éventuel si la piste 1 est retenue (à nommer par `architect`)

## ADR attendu ?
- **Probable** : oui
- **Critère(s) candidat(s)** : 1, 3
- **Sujet pressenti** : où se rapatrie la signature — à la vérification ou à l'installation —
  et pourquoi trois sessions TLS dans une requête ne tiennent pas.

## Éléments visibles (pour doc-writer)

### Endpoints HTTP concernés
- `GET /check-update` — champs de statut de relais
- `POST /download-update` — inchangé dans son contrat
- Endpoint neuf éventuel (piste 1)

### Champs WebSocket
- aucun

### Topics MQTT
- aucun

### Documentation à produire / mettre à jour
#### Docs vivantes
- `docs/features/page-settings.md` — si un message utilisateur change
- `docs/subsystems/ota-manager.md` — relais `.sig`, garde mémoire, statuts

#### Docs transverses
- `docs/API.md` — champs de `/check-update`
- `docs/UPDATE_GUIDE.md` — ce que voit l'opérateur si une signature manque
- `CHANGELOG.md` — entrée 2.88.0

#### Docs immuables
- `docs/adr/NNNN-*.md` — si l'architecture du rapatriement change

## Critères d'acceptation
- [ ] Sur release signée, **dix** `/check-update` consécutifs ramènent les deux signatures
- [x] Le journal d'un saut permet le diagnostic **sans** dépendre d'une autre sonde
- [x] « Release non signée » et « relais indisponible » sont distinguables par la page
- [x] Aucun refus neuf introduit par ce lot
- [ ] Une release **sans** `.sig` s'installe toujours, firmware et filesystem (non vérifié on-target)
- [x] Tous les libellés utilisateur sont en français
- [x] `pio run` compile sans nouveau warning
- [x] `./build_fs.sh` passe sans dépasser la partition
- [x] Suite native verte, zéro régression

## Plan de test manuel
1. **Nominal** — `/check-update` sur v2.87.1 : les deux relais validés, chiffres relevés.
2. **Répétition** — dix vérifications d'affilée, aucun saut.
3. **Pression mémoire** — provoquer un heap dégradé et vérifier que le message reste
   diagnostique et qu'aucun refus neuf n'apparaît.
4. **Release non signée** — vérifier l'installation des deux genres, sans alarme.
5. **Terrain** — installation complète depuis la page, verdicts d'authenticité relevés au
   journal persistant pour les **deux** genres.

## Notes pour agent
- ⚠️ **Le journal volatil de l'appareil ne tient qu'une minute** (8 Ko) : lire
  `/download-logs` (persistant, partition `history`, survit au remplacement du filesystem),
  pas `/get-logs`, pour tout verdict pris avant un redémarrage.
- ⛔ **« L'envoi a été accepté » ne prouve rien.** Le verdict se lit au journal.
- ⚠️ L'appareil régule un bassin réel et **doit continuer à doser** quoi qu'il arrive.
- ⚠️ Compilation = ressource exclusive GLOBALE : jamais deux agents qui compilent.

## Hors périmètre
- Le **refus** sur signature invalide ou absente : c'est D3-c de feature-151.
- Le rechargement aveugle de la page après redémarrage : spec séparée (défaut B du
  2026-11-15, à ouvrir).
- Le durcissement de `#no_update_msg` (`app.js:10841`) : porté par D3-b.

---

## 📏 MESURE ON-TARGET (2026-11-15) — faite SANS instrumentation, livrable A4

⭐ **Obtenue sans déployer le lot 0** : l'événement « saut » est déjà observable depuis l'extérieur
(`filesystem_sig_url` non vide + `filesystem_manifest` vide dans la réponse `/check-update`), et le
tas est lisible via `GET /get-system-info?hf=1` (`free_heap_8bit`, `max_alloc_8bit`, `psram_free`).

### Protocole
Appareil réel en 2.87.1. Trois campagnes successives, en lecture seule, aucun flash :
1. 10 vérifications à vide puis 10 sous charge ⛔ **campagne INVALIDE**, voir « Erreurs de méthode ».
2. 6 paires **alternées** calme/chargé, charge = **1** sondeur `/data` à 1 Hz (60 req/min, sous le
   quota appareil de 120, `constants.h:219`).
3. Test de récupération : **180 s de silence**, puis une vérification isolée, puis une enchaînée.

### Résultats

| Contexte | Vérifications exécutées | Relais firmware | Relais filesystem |
|---|---|---|---|
| Appareil **au calme** | **5** | relayé 5/5 | **relayé 5/5** |
| **Trafic concurrent** | **9** | relayé 7/9 | **sauté 9/9** |

Durées : **10-11 s** quand les deux relais aboutissent, **7-8 s** quand le filesystem saute,
**3 s** quand les deux sautent. ⇒ La durée confirme le nombre de sessions TLS réellement ouvertes.

### Tas mesuré (campagne 3)

```
Après 180 s de silence :  free8 = 50 656   maxAlloc8 = 42 996   psram_free = 4 174 860
  → vérification isolée    11 s — firmware relayé, filesystem relayé
Après :                    free8 = 50 376   maxAlloc8 = 42 996
  → seconde enchaînée      11 s — firmware relayé, filesystem relayé
Après :                    free8 = 50 132   maxAlloc8 = 28 660
```

### Conclusions

1. ✅ **Aucune fuite, aucune dégradation durable.** 524 o consommés en deux vérifications complètes,
   et `free8` revient à ~50 Ko. L'état dégradé observé en campagne 2 ne survit pas à 180 s de calme.
2. ✅ **H1 CONFIRMÉE** (hypothèse `architect` passe 1) : le saut est corrélé au **trafic concurrent**,
   pas au nombre de vérifications. L'alternance de la campagne 2 exclut le biais d'ordre.
3. 🔴 **Le déficit est d'au moins 17 700 o.** Au repos `free8` ≈ 50 000 ; la garde se ferme sous
   32 768 ⇒ la chute pendant la requête dépasse **17,7 Ko**. ⚠️ C'est **plus de six fois** ce que la route
   alloue elle-même (≈ 2,7 Ko mesurés par l'architect, dont ≈ 1,5 Ko de nœuds de chaînes ArduinoJson).
4. ⛔ **La piste 2 (« libérer avant de relayer ») est ÉLIMINÉE PAR LE CALCUL** : elle récupérerait
   ~1,5 à 2,2 Ko contre un déficit de 17,7 Ko. ⚠️ C'était la piste la plus tentante — elle « couvrait »
   les 1 744 o du relevé initial, qui n'étaient que la **marge résiduelle**, pas le déficit.
5. ⇒ La cause est **la fenêtre de blocage de `async_tcp`** (10-11 s), pendant laquelle les paquets
   entrants sont retenus en RAM interne. **La page ouverte de l'utilisateur est le trafic qui affame
   l'appareil** — ce qui explique l'intermittence et pourquoi un `curl` isolé n'a jamais reproduit le défaut.

### ⚠️ Erreurs de méthode à ne pas reproduire
- 🔴 **Campagne 1 invalide** : 3 sondeurs à 1 Hz = **180 req/min > quota de 120** (`constants.h:219`).
  L'appareil a opposé son propre limiteur, et ces refus ont été comptés comme des résultats. ⚠️ Le
  corps des réponses n'était pas capturé — l'erreur est restée invisible jusqu'à la lecture du journal.
- 🔴 **Campagne 2, « dégradation cumulative » APPARENTE** : à partir de la 4ᵉ paire, le relais sautait
  même à vide. Cause réelle : **les sockets TIME_WAIT du générateur de charge** (MSL 60 s,
  `CONFIG_LWIP_TCP_MSL`), que les pauses de 20 s ne drainaient pas. ⇒ **L'instrument mesurait
  lui-même.** La campagne 3 (180 s de silence) l'a réfutée.
- ⇒ **Toute campagne future espace ses essais d'au moins 90 s** et capture le **corps** de chaque réponse.

### Conséquence sur le périmètre du lot
⭐ **Le lot 0 d'instrumentation (A3b, sondes heap autour des trois sessions) n'est plus nécessaire
pour ARBITRER** : la mesure externe a suffi. Il reste utile pour le **diagnostic en exploitation**
(A3, le WARN enrichi), qui garde sa valeur propre — un client ne peut pas lancer cette campagne.
⇒ Un déploiement et un redémarrage de l'appareil sont **épargnés**.

---

## 🔒 ARBITRAGE ET REVUES DE SÉCURITÉ (2026-11-15)

### Piste retenue : **relais à la demande** (piste 1), par les chiffres

`/check-update` cesse de relayer et **mémorise les URL côté appareil** ; la page demande chaque
signature juste avant usage, via `GET /ota-sig?kind=firmware|filesystem`. ⇒ **Une seule session
TLS sortante par requête HTTP.**

| Variante | Fenêtre de blocage | Déficit | Marge (débit terrain 2 045 o/s) | |
|---|---|---|---|---|
| **Actuel** (3 sessions / 1 requête) | 11 s | 22 500 o | **−7 870 o** | ❌ saute |
| **Piste 1** (1 session / requête) | **4,5 s** | **9 200 o** | **+5 420 o** | ✅ |
| Piste 2 (libérer le `doc`) | 11 s | 20 500 o | −5 870 o | ❌ |
| Piste 3 (réessai) | 14 s | 28 600 o | −13 980 o | ❌❌ **aggrave** |
| Piste 4 (baisser le seuil) | 11 s | 22 500 o | « passe » | ⛔ handshake à 25 Ko libre |
| Piste 5b (tâche dédiée) | ~0 s | ~0 o | +14 620 o | ❌ 8 Ko de pile interne permanents |

⭐ **Mécanisme nommé** : le coût ajusté d'un paquet retenu vaut **≈ 1,6 Ko** — la taille d'un
tampon RX dynamique du pilote WiFi, en RAM interne DMA-capable. ⇒ H1 n'est plus une corrélation,
elle a un mécanisme dimensionné. Validation croisée : terrain 2 045 o/s, laboratoire 1 610 o/s
(la page auto-rafraîchit le journal toutes les 5 s, `app.js:11999`).

⚠️ **Honnêteté du résultat** : le facteur de sécurité n'est que de **1,6×** au débit du terrain
(2,4× en laboratoire). C'est un **passage**, pas un confort ⇒ la garde ET le WARN enrichi A3 sont
**conservés** : si le contexte se dégrade (deux téléphones ouverts), le journal le dira.

### 🔴 G1 — DÉFAUT PRÉEXISTANT TROUVÉ : la borne de blocage n'est pas prouvée

`setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS)` est posé (`web_routes_ota.cpp:1311` et
`:2039`) mais **`setRedirectLimit` n'existe nulle part dans `src/`** ⇒ le défaut de la
bibliothèque s'applique : **`_redirectLimit = 10`** (`HTTPClient.h:315`, vérifié).

⚠️ Les dix sauts se font **à l'intérieur d'un seul `GET()`**, donc **sans nourrir le watchdog**.
Pire cas ≈ **10 × 8 s ≈ 90 s** contre **30 s** de TWDT ⇒ **redémarrage par watchdog**, sur un
appareil qui dose. La tâche `async_tcp` est bien abonnée au TWDT (`web_routes_ota.cpp:1643-1648`).

⛔ **Le `static_assert` de `constants.h:1436` repose donc sur une prémisse FAUSSE** : il suppose
2 connexions par `GET` (une redirection). Le correctif C1 de feature-151 (livré en 2.87.0) a
annoncé une borne de 19 s qui **n'est pas prouvée**. ⚠️ Défaut **déjà en production** (2.87.0,
2.87.1), touchant aussi `/download-update`. En pratique GitHub ne fait qu'une redirection ⇒ risque
faible, mais **« faible » n'est pas « borné »**, et c'était l'objet même du correctif.
⇒ **Replié dans ce lot** : `setRedirectLimit(1)`, et le `static_assert` neuf le cite.

### Conditions de la revue de sécurité — P1 + P1-bis (verdicts GO SOUS CONDITIONS)

| # | Condition | Portée |
|---|---|---|
| S1 | `setRedirectLimit(1)` sur le client de relais ; le `static_assert` neuf le cite | G1 |
| S2 | ⭐ `absent` émis **UNIQUEMENT** si la réponse GitHub a été analysée **avec succès** sans asset de ce nom. Tout autre cas ⇒ `unavailable` + motif (`no_check`, `url_too_long`, `host`, `memory`, `time`, `http`, `timeout`, `grammar`, `throttled`) | ⛔ sinon D3-c refuse au hasard |
| S3 | Emplacements `g_sigSource` **effacés à l'entrée** de `/check-update`, écrits **seulement sur succès**. Divergence de `tag` ⇒ « non récupérée », ⛔ jamais « non signée » | cohérence |
| S4 | Intervalle minimal **par genre** entre deux sessions sortantes, vérifié **après** l'authentification et **avant** TLS ⇒ `unavailable/throttled` sans connexion | déni |
| S5 | Garde TLS fermée et horloge non synchronisée rendues en `unavailable`, ⛔ pas en 503. La page traite tout statut inconnu comme `unavailable` | lisibilité |
| S6 | 🔴 **D3-c ne sort PAS dans le même firmware que ce lot** : il attend une release dont le filesystem porte la page neuve, et **une voie de réparation indépendante de la page est documentée** | ⛔ risque de blocage client |
| S7 | Longueur d'URL vérifiée **avant** `isUrlAllowed` ; `/ota-sig` ne journalise **jamais** l'URL | bug-112 |

### Points tranchés par la revue
- **SSRF** : l'allowlist **n'est pas** revalidée après redirection (`HTTPClient.cpp:1444-1471`
  n'exige que le même protocole). Voie jugée **fermée en pratique** : HTTPS obligatoire, chaîne
  limitée aux 5 racines de `github_root_ca.h`, corps ≤ 1 Ko, grammaire stricte, signature vérifiée
  par l'appareil, aucune cible LAN atteignable, et un client authentifié n'influence rien.
  ⇒ Spec de durcissement à ouvrir (suivi manuel des redirections).
- **Déni de service** : trivial **mais pas neuf**. Les handlers étant sérialisés, ~13 appels/min
  suffisent à occuper `async_tcp` en continu, et `/check-update` le permet déjà. Le quota de
  120 req/min n'est jamais la borne. ⇒ S4.
- **Oracle** : aucun signal neuf — `/get-system-info?hf=1` donne déjà les chiffres exacts au même
  porteur de jeton. Acceptable si `reason` est une **énumération fermée**.
- **Gravité réelle de bug-176** : **disponibilité**, pas authenticité. `/download-update` ne refuse
  rien aujourd'hui ; un client authentifié peut déjà omettre les paramètres. Après D3-c, la famine
  ne produirait qu'un **déni de mise à jour**.

### ADR-0123 — justifié
`docs/adr/0123-une-seule-session-tls-sortante-par-requete-http.md` — critères 1 et 3. ⛔ Sans le
tableau d'arbitrage chiffré ci-dessus, cet ADR ne vaut rien : c'est le calcul qui a tué la piste 2,
la plus tentante.

### Rectification documentaire obligatoire
`constants.h:1336-1342` affirme que le parse JSON ne consomme pas la RAM interne. **C'est faux** :
`BudgetJsonAllocator::allocate` (`web_routes_ota.cpp:1100`) appelle `malloc()` nu, et
`kPsramExtmemThresholdBytes` = 1 024 o (`constants.h:1222`) place en **interne** tout bloc ≤ 1 024 o
— or ArduinoJson 7 alloue ses nœuds de chaînes en blocs petits et nombreux.
⚠️ **C'est la DEUXIÈME fois que ce commentaire est « rectifié » pour redevenir faux.** Un
commentaire de dérivation non adossé à un `static_assert` ou à une mesure **pourrit** : la
rectification doit **pointer le `static_assert` de `:1376`** plutôt que ré-expliquer la physique.

### Périmètre : le lot 0 d'instrumentation disparaît
| Livrable | Sort |
|---|---|
| A3b — sondes heap autour des 3 sessions | ❌ **tombe** (il n'existe plus 3 sessions ; la mesure externe a tranché) |
| A3 — WARN de saut enrichi | ✅ **conservé et déplacé** vers `/ota-sig` (valeur en exploitation : un client ne peut pas lancer la campagne du 15/11) |
| A4 — mesure on-target | ✅ **acquis**, consigné ci-dessus |

⇒ **Un seul déploiement pour 2.88.0**, pas deux. ⛔ Signer **en dernier**, après `./build_fs.sh`.

---

## 🔧 LOT FIRMWARE LIVRÉ (2026-11-15) — A0 à A10

`pio run` SUCCESS, **Flash 51,3 %** (1 915 613 o), **RAM 29,8 %** (97 792 o), aucun nouveau
warning. `check_memory_discipline.sh` et `check_fopen_guard.sh` verts. Suite existante
`test_native_ota_sig_relay` 14/14. ⛔ Aucun test natif neuf (travail de `test-engineer`), aucun
essai on-target, rien de déployé. `data/` **non touché**.

Fichiers : `src/ota_sig_relay_logic.{h,cpp}`, `src/web_routes_ota.cpp`, `src/constants.h`,
`src/version.h` (**2.88.0**).

### Décisions notables
- **A7 / S4** : `kOtaSigMinIntervalMs = 10 000` **par genre**, contrôlé après l'authentification
  et **avant** TLS ⇒ `unavailable/throttled` sans ouvrir de connexion. Arithmétique non signée
  sûre au repliement de `millis()`.
- **A6 / S2** : `absent` n'est émis **que** si l'état de l'emplacement vaut `kCheckedNoAsset`.
  ⭐ Tout autre cas — **y compris une valeur d'état hors énumération** — donne `unavailable` + motif.
- **S7** : `isUrlAllowed` gagne un paramètre `logRefusal = true` ; le relais l'appelle avec
  `false` ⇒ `/ota-sig` ne journalise **jamais** l'URL.
- Un hôte hors liste blanche donne `unavailable/host` (⛔ pas `absent`) — cohérent avec S2.
- Le `tag` mémorisé est le `tag_name` **brut**, avec son « v ».

### ⚠️ Écart au brief, DÉCLARÉ par le développeur
Le `static_assert` de `constants.h:1436` devait rester inchangé ; le littéral `2UL` y a été
remplacé par `(kOtaMaxRedirectsPerGet + 1UL)`. **Valeur identique (19 s)**, et le changement
**améliore** l'assert : il est désormais adossé à la constante qui borne réellement les
redirections, au lieu d'un littéral qui redeviendrait faux si la limite changeait. ✅ Accepté.

### 🔴 QUATRIÈME session TLS — trouvée et corrigée dans ce lot
`/download-update` (`web_routes_ota.cpp:2159`) ouvre une session TLS **jamais couverte** par le
correctif C1 de feature-151, qui ne visait que les **trois** sessions de « Vérifier »
(`:1367-1370`, `:1552-1556`). ⚠️ Ni `setHandshakeTimeout`, ni `setConnectTimeout` ⇒ **handshake
au défaut de 120 s contre 30 s de TWDT** : redémarrage par watchdog **pendant le téléchargement
d'un firmware**, sur un appareil qui dose.

⇒ **Corrigé ici** : `setHandshakeTimeout(kOtaTlsHandshakeTimeoutSec)` + `setConnectTimeout(kOtaTlsConnectTimeoutMs)`.
⛔ **Le délai de lecture du socket n'est volontairement PAS posé** : sur une image de ~1,9 Mo, une
échéance mal dimensionnée couperait une liaison lente. C'est l'objet de **`bug-174`**
(échéance d'INACTIVITÉ), déjà ouvert dans `todo/`.

⚠️ **Leçon de méthode** : le `static_assert` ne borne **que ce qu'il nomme**. Il affirmait « le
pire GET bloquant » alors qu'il ne couvrait que trois sessions sur quatre. ⇒ Un invariant de
blocage doit être rattaché à **toutes** les sessions sortantes, ou dire lesquelles il exclut.

---

## 🎨 ÉTUDE ERGONOMIE — RENDUE (2026-11-15), sur captures de l'écran réel

Captures prises à 390 px et 1024 px sur la route `#/settings/system` et **regardées** avant envoi.

### Principe retenu : **seul l'état anormal parle**
- `absent` (release non signée — **cas nominal du parc existant**) ⇒ ⛔ **aucune ligne, aucune alarme**.
- `deferred` ⇒ aucune ligne.
- `unavailable` ⇒ une **ligne neutre**, `#update_sig_note`, placée **juste au-dessus des deux
  boutons** — à côté de l'action, pas en haut de carte.

Composant réemployé : `.field__hint`, **contexte d'emploi nouveau** (« pied de carte de réglage,
près d'un contrôle ») ⇒ à inscrire dans `docs/DESIGN_SYSTEM.md`. ⛔ Ni `.callout` ni
`.callout--warn` (réservés aux sécurités bloquantes), **aucune couleur d'alerte** : le sens est
dans le texte, pas dans la teinte — donc sans écueil de daltonisme.

### Libellés arrêtés (largeur utile ≈ 340 px à 390 px ⇒ ~54 car./ligne)
| Cas | Libellé | Car. |
|---|---|---|
| `relayed` | « Release signée : l'appareil vérifiera l'image. » | 46 |
| `absent`, `deferred` | *(rien)* | 0 |
| `unavailable` passager (`memory`, `time`, `timeout`, `http`, `no_check`) | « Signature non récupérée. Cliquez sur Vérifier pour réessayer. » | 63 |
| `unavailable` / `throttled` | « Appareil trop sollicité. Réessayez dans 1 minute. » | 51 |
| `unavailable` définitif (`host`, `url_too_long`, `grammar`) | « Signature non récupérable pour cette version : rien ne sera vérifié. » | 70 |
| Bouton, si `unavailable` | « Mettre à jour sans vérification » | 31 |

⛔ Le code `*_sig_reason` **brut** va dans l'attribut `title` et la console, **jamais** dans le texte.
Si les deux genres divergent, la ligne ne décrit que **le genre qui sera installé**.
Accessibilité : `role="status"`, `aria-live="polite"`, `hidden` (⛔ pas `style.display`), cibles ≥ 44 px.

### 🔴 DÉCISION PRODUIT — le SECOND CLIC est REFUSÉ
L'étude proposait, si `/ota-sig` échoue au clic alors que le check annonçait `relayed`, de **ne rien
lancer**, de basculer le bouton en « Mettre à jour sans vérification » et d'attendre un **second clic**
— présenté comme « une confirmation, pas un refus », et explicitement soumis à arbitrage.

⛔ **Refusé.** C'est une boîte de confirmation déguisée, et le projet en **retire** (`feature-170`
est ouverte pour supprimer celle de la correction pH). ⇒ **Le libellé du bouton change, et UN SEUL
CLIC installe.** L'information est portée à l'endroit de l'action, ce qui suffit — c'est d'ailleurs
l'argument de l'étude elle-même (« informer là où l'on agit »).

### 🔵 HORS PÉRIMÈTRE — versé dans `feature-169`
L'étude propose ~700 px de gains sur un écran de 2 600 px (fusion de lignes d'Infos système,
repli de « Mise à jour manuelle », carte « Maintenance » unifiée, avertissement de réinitialisation
ramené de 9 à 2 lignes). ⛔ **Sans rapport avec bug-176** ⇒ versé dans `feature-169`
(onglet Système), qui existe déjà. ⚠️ Ne pas l'implémenter ici : « changements minimaux, rester
dans le périmètre de la spec ».

### Points relevés par l'étude, à ne pas perdre
- Elle **n'a pas lu `app.css`** et le déclare ⇒ aucun composant partagé n'évolue, aucun inventaire
  d'emplois n'était dû. ✅ Comportement correct.
- La carte « Mise à jour en ligne » ne doit **pas** récupérer les trois lignes retirées au lot
  précédent : deux boutons + au plus une ligne conditionnelle est le bon équilibre.
- Quand `bug-177` sera traité, la ligne d'erreur de `/check-update` devra distinguer un **429**
  (« Appareil trop sollicité ») d'un appareil **injoignable**.

---

## 🧪 TESTS (2026-11-15) — D1 et D2

**Suite neuve** : `test/test_native_ota_sig_relay_decide/test_ota_sig_relay_decide.cpp`, **16 cas**.
⭐ **S2 balayée sur les 256 valeurs d'état** : `absent` n'est émis que pour `kCheckedNoAsset` ;
tout le reste, **y compris hors énumération**, donne `unavailable` + motif. Seul `kPresent`
autorise une session TLS.

**Non-régression MESURÉE** : **81 suites, 2 418 cas, 2 418 verts**.
`test_native_ota_sig_relay`, `test_native_ota_order_guard` (garde [G6]) et
`test_native_ota_update_fail_closed` passent.

### D2 — mutations
| Mutation | Résultat |
|---|---|
| 1. Inverser `absent` ↔ `unavailable` (3 variantes) | tuée |
| 2. Accepter `"Firmware"` ou un préfixe | tuée |
| 3. Figer le motif à `memory` | tuée |
| 4. Recopier le statut firmware sur filesystem | tuée (via l'indice de genre ; le statut par genre vit dans le handler, hors natif) |
| 5. Neutraliser la garde TLS | tuée (via `kGateClosed` → `kMemory` ; le branchement réel `:1353` n'est couvert que par l'essai on-target) |
| 6. Soustraction signée dans `otaSigThrottled` | ⭐ **a SURVÉCU**, puis tuée |
| Retirer `if (!hasLast)` | tuée |
| `nowMs < last + interval` | tuée |

⭐ **La mutation 6 est le résultat le plus important de ce lot.** Le mutant « soustraction signée
sur 32 bits » se comporte comme l'arithmétique non signée **tant que l'écart est < 24,8 jours** :
la suite initiale était **verte et aveugle** au repliement de `millis()` — ⚠️ **le défaut même que
ce projet a déjà connu** (ADR-0073, feature-129). Un test d'écart > 24,8 jours a été ajouté, le
mutant est tué, et l'arithmétique du développeur est **confirmée correcte**.
⇒ **Sans mutation, personne ne l'aurait su.** C'est l'argument de cette méthode, en une ligne.

⚠️ `pio run` du `test-engineer` tournait **en cache** (5 s) ⇒ son « zéro warning » ne couvrait que
cette passe. **Compilation propre relancée** (voir Mesures de clôture).

---

## 👀 REVUE DE CODE (2026-11-15) — Request changes, puis levée

**Aucun problème de sécurité : les 7 conditions S1–S7 vérifiées dans le diff**, ligne à ligne.

### 🔴 Deux points bloquants — CORRIGÉS
1. ⭐ **`fetchSigParams` échouait EN SILENCE** (`data/app.js`) : tout échec de `/ota-sig` rendait
   `""` sans ligne, sans changement de libellé, sans `console.warn` ⇒ l'installation partait sans
   signature, **sans rien dire**. ⚠️ **Une défaillance silencieuse dans le correctif d'une
   défaillance silencieuse.** ✅ Fermé : ligne écrite, bouton basculé, `console.warn`, **puis
   l'installation continue** (un seul clic, décision inchangée). Exception ou HTTP non-ok ⇒
   `unavailable` passager. ⚠️ La divergence de `tag` reste volontairement **muette** et sans
   paramètres (on retombe sur « non signée »), mais avec un `console.warn`.
2. ⭐ **La ligne « Release signée : l'appareil vérifiera l'image. » était INATTEIGNABLE.**
   L'étude d'ergonomie l'avait conçue quand `/check-update` relayait encore ; **il ne relaie plus**
   et ne peut donc pas savoir si la signature sera récupérable (`otaSigSourceDecide` ne rend jamais
   `kRelayed`). ✅ **Supprimée** — avec `bothRelayed`, devenu code mort, et le booléen de retour
   inutilisé de `renderSigNote`.
   ⇒ **Décision** : supprimer plutôt que rendre atteignable. Cette ligne positive était la seule
   exception au principe « seul l'état anormal parle » ; **l'absence d'avertissement EST le message
   rassurant**. ⛔ Ne pas la faire apparaître pendant l'installation : ce serait rassurer
   l'utilisateur au moment où il ne peut plus rien décider.
   ⇒ `CHANGELOG.md`, `docs/UPDATE_GUIDE.md` et `docs/features/page-settings.md` rectifiés : ils
   promettaient une phrase que le code ne pouvait pas produire.

### ⚖️ Un point mineur ÉCARTÉ, avec sa raison
La revue juge que `constants.h:1336-1347` « ré-explique la physique » et demande deux lignes.
⇒ **Conservé en l'état** : le commentaire **interdit explicitement** de ré-expliquer la physique,
**nomme les deux pourrissements précédents avec leurs dates**, puis désigne le `static_assert`
de `:1376` comme seule autorité. La prose restante est **bornée par un avertissement**, ce qui est
la meilleure protection disponible à moins de tout supprimer. ⚠️ Désaccord **noté**, pas masqué.

### Points tenus, vérifiés par la revue
**Aucun refus neuf** (`/check-update` ne gagne que l'effacement des emplacements, leur écriture et
4 champs additifs ; `/download-update` que deux bornes TLS) · release non signée installée à
l'identique · `esp_task_wdt_reset()` conservé **avant le GET et dans la boucle de lecture** ·
`isUrlAllowed` avant toute sortie, muette côté `/ota-sig` · longueurs bornées 192/24 + NUL ·
heap 8 bits partout · manifeste re-sérialisé · `static_assert` 19 000 ms inchangé avec commentaire
réécrit, et le neuf à 24 000 < 30 000 adossé aux constantes · front sans `Promise.all`, séquentiel,
`textContent` seul, statut inconnu ⇒ `unavailable`.

### ⚠️ Correction d'une FAUSSE référence, à la source
`bug-170` était cité comme « le motif de la voie `littlefs.bin` qui meurt en silence ». **C'est
faux** : `bug-170` traite des routes duty qui démarrent une pompe sans garde chimique. ⇒ La bonne
référence est **`bug-169`** (assets renommés, `littlefs.bin` plus joint, **six semaines** sans que
personne ne le voie). ⚠️ L'erreur venait de **`.claude/commands/release.md:65`**, qui la propageait
à chaque release ⇒ **corrigée à la source**, pas seulement dans cette spec.

### ⚠️ Chiffre corrigé
« ~10× ce que la route alloue » était un arrondi flatteur : 17 700 / 2 700 = **6,6×**.
⇒ Remplacé partout par « plus de 6× ».

---

## ✅ ESSAI DE TERRAIN — 2.88.0 SUR L'APPAREIL (2026-11-15) — LE DÉFAUT EST CORRIGÉ

Déploiement OTA **signé** (firmware + filesystem), phrase de passe saisie par l'utilisateur.
Appareil en **2.88.0**, `free8` = 53 548 o, `maxAlloc8` = 51 188 o au démarrage.

### ⭐ Le critère décisif : 10 cycles, 20 appels `/ota-sig`, **0 saut**

| Contexte | Cycles | Relais firmware | Relais filesystem |
|---|---|---|---|
| **Au calme** | 5 | relayed 5/5 | **relayed 5/5** |
| **Trafic concurrent** (1 sondeur `/data` à 1 Hz) | 5 | relayed 5/5 | **relayed 5/5** |

⇒ **20 signatures sur 20**, 512 caractères chacune, `tag = v2.87.1` cohérent partout.
⚠️ **C'est exactement la condition qui échouait 9/9 avant le lot.**

### Fenêtre de blocage : effondrée
`/check-update` passe de **10-11 s** à **2,0 – 4,1 s** ; chaque `/ota-sig` tient en **2,5 – 3,0 s**.

### ⭐ Le modèle de l'architecte vérifié au centième
Journal de l'appareil, session TLS de `/check-update` :
`free8` **50 556 → 41 644** = **8 912 o** consommés.
⇒ La valeur **prédite** pour cette solution était **9 200 o** (annexe A-4 de la passe 2).
⚠️ Le tas ne descend jamais près du seuil : **49 288 à 53 548 o** mesurés contre **32 768** requis
— là où il tombait à **31 024 o** le matin même.

### Conditions de sécurité vérifiées ON-TARGET (les 5 invérifiables en natif)
| | Vérification | Résultat |
|---|---|---|
| **S2** | `/ota-sig` **avant tout `/check-update`** (emplacement vide après redémarrage) | ✅ `unavailable` / `no_check` — ⛔ **PAS `absent`** |
| **S4** | 2 appels `sig-fw` consécutifs | ✅ le 2ᵉ en **0,0 s** ⇒ `unavailable/throttled`, **sans ouvrir de connexion** |
| **S5** | garde et horloge | ✅ aucun 503 observé, tout en 200 |
| **S7** | URL au journal | ✅ **`grep -c "https://"` = 0** sur le journal persistant |
| `kind` | `?kind=Firmware` (majuscule) | ✅ **400** `{"code":"bad_kind"}` — égalité stricte |

Journal : `/ota-sig : relais .sig filesystem — validé (614 o)` et `... firmware — validé (612 o)`,
à chaque cycle. ⛔ Aucun WARN de saut, aucun CRITICAL.

### ⛔ CE QUI N'EST PAS ENCORE VÉRIFIÉ
- **L'installation complète de bout en bout** : impossible tant qu'aucune release **2.88.0**
  n'existe — la dernière publiée est la 2.87.1, donc **antérieure**. ⇒ À faire après la release.
- **Le contre-test « release non signée »** : toujours pas fait.
- `/download-update` avec `setRedirectLimit(1)` : non exercé (aucun téléchargement).

### 🔴 DÉFAUT PRÉEXISTANT DÉCOUVERT PAR CET ESSAI — `bug-178` ouvert
`/check-update` a répondu **`update_available: TRUE`** alors que l'appareil (2.88.0) est **plus
récent** que la release (2.87.1).
```cpp
web_routes_ota.cpp:1866   bool updateAvailable = (latestVersion != currentVersion);
```
⇒ **Inégalité de chaînes, pas comparaison de versions.** L'appareil propose un **retour en
arrière** présenté comme une mise à jour. ⚠️ L'accepter réinstallerait la 2.87.1 **et
réintroduirait le défaut que ce lot vient de corriger**.
⚠️ **Préexistant**, mais **invisible jusqu'ici** : l'appareil n'avait jamais une version plus
récente que la dernière release. ⇒ **`specs/features/todo/bug-178-mise-a-jour-proposee-vers-une-version-anterieure.md`**.
⛔ **En attendant : ne pas appuyer sur « Mettre à jour » sur l'appareil.**
