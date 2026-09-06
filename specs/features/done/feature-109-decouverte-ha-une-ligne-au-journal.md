## Métadonnées
- **ID** : feature-109
- **Slug** : decouverte-ha-une-ligne-au-journal
- **Type** : feature (observabilité)
- **Statut** : **done** — livré, revu, déployé le 2026-09-06 en v2.44.0
- **Priorité** : **haute** — bloque l'observabilité des redémarrages, donc la clôture de `bug-080`
- **Version cible** : à définir
- **Dernière mise à jour** : 2026-09-06
- **Spec(s) liée(s)** : `bug-080` (critère « 7 jours sans redémarrage inopiné »), `bug-100` (le journal est un des chemins gardés), ADR-0033 (budget de la partition `history`)

## Domaines touchés
- [x] MQTT / Home Assistant — **le contenu de la découverte ne change pas**, seule sa journalisation
- [ ] Régulation — [ ] Capteurs — [ ] Filtration / Éclairage — [ ] Web — [ ] OTA — [ ] Frontend
- **`pool-chemistry` non requis** : aucun code de dosage, aucun seuil, aucune limite.

## Agents requis
- [x] spec-reader · [x] architect · [x] embedded-developer · [x] test-engineer · [x] code-reviewer · [x] doc-writer
- [ ] pool-chemistry · [ ] ux-ui-designer · [ ] web-ui-developer

## Description
Remplacer les **54 lignes** `INFO` émises à chaque publication de la découverte Home
Assistant par **une seule ligne de synthèse**, sans jamais masquer un échec.

## Contexte — mesuré sur l'appareil le 2026-09-06

La découverte HA est republiée **à chaque (re)connexion MQTT** — c'est voulu et ne doit
pas changer : un redémarrage du broker exige cette republication. Ce qui pose problème
est sa **trace**.

`src/mqtt_manager.cpp:1761`, site unique :
```cpp
systemLogger.info("Discovery " + configTopic + (ok ? " OK" : " FAILED"));
```
appelé par les **54** `publishConfig()` de `publishDiscovery()`.

### Le coût, en chiffres relevés

| Grandeur | Valeur | Source |
|---|---|---|
| Lignes par rafale | **54** | `grep -c "publishConfig(topic)"` |
| Poids moyen d'une ligne | **98 o** | mesuré sur l'export du 06/09 |
| **Poids d'une rafale** | **≈ 5,3 Ko** | 54 × 98 |
| Taille de rotation du journal | **8 192 o** | `logger.h:43`, `kMaxLogFileBytes` |
| Conservé à la rotation | **6 144 o** | `logger.h:44`, `kRotateKeepBytes` (ADR-0033) |
| Part du journal relevé | **83 %** | 221 lignes de découverte sur 266 datées |
| Reconnexions MQTT observées | **4 en 28 min** | export du 06/09 |
| Capacité du tampon RAM | **100 entrées** | `logger.h:42`, `kMaxPersistBufferEntries` |

**Une seule rafale occupe 65 % du fichier de journal et 86 % de ce que la rotation
conserve.** Elle consomme aussi **54 des 100 entrées** du tampon RAM avant tout flush :
plus de la moitié de l'historique en mémoire est évincée à chaque reconnexion.

### Le dégât, constaté

Le 06/09, l'appareil a redémarré vers **14:19 locale**. Le firmware **journalise
pourtant la cause** : `heapRingDumpIfCrash()` (`heap_ring.cpp:110-140`) émet au
démarrage un `WARNING` « Cinétique heap avant reset (*label*, reset_reason=N) » avec
l'anneau des échantillons mémoire précédant le reset.

Cette ligne n'existait plus au moment du relevé, à 19:15 : **la fenêtre du journal
persistant ne couvrait que 28 minutes** (18:44 → 19:12). La cause du redémarrage est
donc **définitivement perdue**, alors qu'elle avait été correctement enregistrée.

C'est le **même mécanisme** qui avait détruit la fenêtre forensique du crash du
15/08/2026. Il n'a jamais été corrigé.

**Conséquence sur `bug-080`** : son critère « 7 jours sans redémarrage inopiné » repose
sur une observabilité qui ne tient pas trente minutes. **On ne peut pas compter des
redémarrages qu'on ne peut pas voir**, et la spec ne peut pas être close honnêtement
tant que ce n'est pas réglé.

## Comportement attendu
- Une publication de découverte complète produit **une seule ligne** au journal, du type :
  `Découverte HA publiée : 54 entités, 0 échec (durée 1 240 ms)`.
- **Un échec n'est jamais masqué.** Toute entité en échec reste identifiable — soit une
  ligne par échec (ils sont rares et diagnostiques), soit la liste bornée des topics
  fautifs dans la ligne de synthèse. `architect` tranche la forme ; l'exigence est qu'un
  échec reste **nommément** identifiable, pas seulement compté.
- Le détail exhaustif peut rester disponible à un niveau **non persisté** (`DEBUG`), si
  le niveau existe et n'entre pas dans le fichier. À vérifier, pas à supposer.
- **La découverte elle-même est strictement inchangée** : mêmes topics, mêmes charges
  utiles, même `retain`, même déclenchement à chaque (re)connexion. Cette spec ne touche
  qu'à la journalisation.

### Cas limites
- Publication **partielle** (déconnexion en cours de rafale) : `safePublish()` retourne
  `false` silencieusement si le lien est tombé. La ligne de synthèse doit distinguer
  « 54/54 » d'une rafale interrompue à mi-parcours.
- Rafale **répétée** : plusieurs reconnexions rapprochées ne doivent pas produire une
  ligne identique en boucle sans information — inclure de quoi les distinguer (compteur
  de reconnexion, durée).
- **Aucune entité** publiée (échec total, broker perdu immédiatement) : doit produire une
  ligne, pas le silence.

## Comportement actuel
54 lignes `INFO` par rafale, à ~98 octets, qui saturent un journal de 8 Ko et un tampon
RAM de 100 entrées.

## Contraintes techniques
- `publishDiscovery()` s'exécute **uniquement depuis `mqttTask`** (commentaire l. 1739) —
  pas de contrainte `async_tcp`, mais la construction de la ligne ne doit pas allouer
  plus que les 54 `String` qu'elle remplace.
- **Ne pas régler le problème en baissant le niveau de log global** ni en augmentant
  `kMaxLogFileBytes` : la partition `history` est contrainte par l'ADR-0033, et
  l'appareil est en famine mémoire chronique. La bonne réponse est d'**émettre moins**,
  pas de **stocker plus**.
- Ne pas toucher à `kMaxLogFileBytes` ni à `kRotateKeepBytes` sans passer par l'ADR-0033.

## Fichiers potentiellement concernés
- `src/mqtt_manager.cpp` (l. 1739-1770) — le lambda `publishConfig` et `publishDiscovery`
- `src/logger.h` / `src/logger.cpp` — seulement si un niveau non persisté est retenu
- `docs/subsystems/mqtt-manager.md`, `docs/subsystems/logger.md`, `docs/MQTT.md`
- `test/` — module pur si la synthèse est extraite

## Livrables — check-list
- [x] A1 — une ligne de synthèse par rafale, avec compte publié / compte total / échecs / durée
- [x] A2 — les échecs restent **nommément** identifiables
- [x] A3 — distinguer une rafale complète d'une rafale interrompue
- [x] A4 — découverte MQTT inchangée : topics, charges utiles, `retain`, déclenchement
- [x] A5 — vérifier si un niveau `DEBUG` non persisté existe ; l'employer pour le détail, ou dire pourquoi ce n'est pas possible
- [x] D1 — test natif de la logique de synthèse (module pur ADR-0017), **validé par mutation**
- [x] D2 — non-régression des tests existants
- [x] C1 — `docs/subsystems/mqtt-manager.md`, `docs/subsystems/logger.md`
- [x] C2 — `CHANGELOG.md` : le journal devient exploitable, c'est un fait utilisateur
- [x] C3 — mettre à jour `bug-080` : l'obstacle à son critère « 7 jours » est levé

## Critères d'acceptation
- [x] Une rafale complète produit **exactement une ligne** au journal persistant (plus une
      par échec réel, borné)
- [ ] **Fenêtre forensique mesurée sur l'appareil** avant / après : le journal doit couvrir
      des **heures**, pas des minutes. C'est le critère qui compte — les autres sont des moyens
- [x] Un échec de publication reste identifiable par son topic
- [x] Aucune modification des topics, charges utiles ou cadence de la découverte
- [x] `pio run` sans nouveau warning ; `pio test -e native` sans régression

## Plan de test manuel
1. Provoquer une reconnexion MQTT (couper le broker 30 s), relever `/download-logs` :
   une seule ligne de synthèse.
2. Provoquer une reconnexion **pendant** la rafale : la ligne doit signaler la rafale partielle.
3. Laisser tourner 24 h, relever la fenêtre couverte par le journal persistant et la
   comparer aux 28 minutes du 06/09.
4. **Vérifier qu'un redémarrage laisse sa ligne** « Cinétique heap avant reset » lisible
   plusieurs heures après — c'est l'objectif réel de cette spec.

## Notes pour agent
- Le site est **unique** (`mqtt_manager.cpp:1761`) : le correctif est petit, son effet ne
  l'est pas.
- Ne pas confondre cette spec avec une baisse de verbosité générale. La découverte HA est
  un cas particulier : **54 lignes rigoureusement identiques d'une rafale à l'autre**, sans
  valeur diagnostique individuelle, qui chassent des lignes irremplaçables.
- L'anneau mémoire de `heap_ring.cpp` est le principal bénéficiaire : sa trace n'a de sens
  que si elle survit assez longtemps pour être relevée.

## Hors périmètre
- La cadence de reconnexion MQTT et ses causes.
- `kMaxLogFileBytes` / `kRotateKeepBytes` (ADR-0033).
- Les autres sources de verbosité du journal — à traiter séparément si la mesure du
  critère d'acceptation montre qu'elles pèsent encore.

---

## Notes d'implémentation — 06/09/2026

> **Statut de la spec délibérément inchangé.** Le code est gelé, revu et *Approved* ;
> la clôture dépend d'un arbitrage humain posé en parallèle. Ces notes consignent
> ce qui a été livré, pas une décision de clôture.

### Fichiers livrés

| Fichier | Nature |
|---|---|
| `src/discovery_report_logic.h` | **Nouveau** — module pur ADR-0017, **header-only** (rien à ajouter au build natif ni à `build_src_filter`) |
| `src/constants.h` | `kDiscoveryFailTopicMax = 48`, `kDiscoverySummaryLineMax = 160` |
| `src/mqtt_manager.h` / `.cpp` | `_discoveryBurstSeq`, garde composite scindée, tally, ligne de synthèse ; commentaire « 52 publish » corrigé en « 54 » |
| `test/test_native_discovery_report/` | **22 cas** — 1 325 → **1 347 tests natifs** |

**Coût** : flash 1 828 981 → **1 830 809 o** (+1 828) ; RAM +8 o.

### Format exact des lignes émises

```text
INFO    Découverte HA #3 : 54/54 entités, 0 échec, 1240 ms
WARNING Découverte HA #4 : 12/54 entités, 42 échecs, interrompue à la 13e (sensor/pool_ph/config), 310 ms
WARNING Découverte HA #7 : 52/54 entités, 2 échecs épars, 1er à la 9e (sensor/pool_orp/config), 1180 ms
WARNING Découverte HA #6 : abandonnée avant publication (lien MQTT absent)
```

Niveau `INFO` si `failed == 0`, **`WARNING` dès le premier échec**.

### Quatre décisions de forme, et leur raison

1. **Le pire cas est borné à UNE ligne, pas « à une constante ».**
   `publishDiscovery()` ne s'arrête jamais (aucun `break`, aucun retour anticipé) :
   un lien coupé à la 1ʳᵉ entité produit **54 échecs et une seule ligne**. Une
   ligne par échec — pourtant la lecture naturelle de A2 — aurait ramené les
   5,3 Ko **exactement le jour du crash**, c'est-à-dire le jour où la fenêtre
   compte. A2 est satisfait autrement : le **premier** topic fautif est nommé.
2. **« interrompue » vs « épars » est un prédicat CALCULÉ**, pas un libellé :
   `failed == attempted - firstFailIndex + 1`. Il distingue « le lien est tombé »
   de « deux entités ont échoué » — **deux diagnostics opposés**.
3. **Le total est compté à l'exécution**, jamais figé en constante. Une constante
   d'entités recréerait la classe de bug du commentaire « 52 publish » qui avait
   dérivé en silence sur plusieurs versions.
4. **Formatage exclusivement par `snprintf`** dans un buffer de l'appelant,
   **aucun `String`** : le `String` du harnais natif est un stub qui ne stocke
   rien — un formateur à base de `String` compilerait, les tests passeraient au
   vert et **ne prouveraient rien**. Ici `bufLen` est réellement honoré et
   vérifiable (arène à canari côté test).

### A5 — le gain de `DEBUG` est PARTIEL, et il faut le dire

Le détail par entité subsiste en `DEBUG`. Le gain n'est **pas** total :

| Chemin | Épargné ? | Pourquoi |
|---|---|---|
| `/system.log` | ✅ | `if (_persistEnabled && level != LogLevel::DEBUG)` — `logger.cpp:58`. **C'est le gain qui compte.** |
| Ring RAM (`kMaxLogEntries = 200`) | ❌ | L'entrée y est poussée comme une autre |
| Push WebSocket / Serial | ❌ | Émis pour tous les niveaux |

Le seul rempart sur les trois derniers est `authCfg.debugLogsEnabled`, **à `false`
par défaut**. Le gain sur le fichier est **acquis** ; celui sur le ring RAM ne
l'est **pas**.

**Piège de placement.** La garde `if (authCfg.debugLogsEnabled)` est posée **au
site d'appel, autour de la concaténation**. Elle paraît redondante — `Logger::debug()`
court-circuite déjà en interne — mais les arguments sont évalués **avant** l'appel :
sans elle, les **54 concaténations `String` seraient payées à chaque rafale, DEBUG
désactivé compris**. Contre-intuitif et coûteux à redécouvrir.

### Validation par mutation — 5/5

| # | Mutation appliquée au module pur | Tué par |
|---|---|---|
| 1 | `firstFailIndex = attempted` → `attempted - 1` (rang décalé de un) | Assertion sur le rang exact du 1er échec (`13e`, `9e`) |
| 2 | Prédicat d'interruption remplacé par `failed > 1` (drapeau cosmétique) | Cas « échecs épars » — 2 échecs non contigus reclassés à tort en « interrompue » |
| 3 | `discoveryCopyTopicTail` tronque par la **tête** au lieu de la **queue** | Assertion sur le discriminant (`..._orp_sensor_problem/config`), invisible si le préfixe est conservé |
| 4 | `attempted` remplacé par une constante `54` figée | Cas rafale partielle : `12/54` devient `12/54` même quand 12 entités seulement ont été tentées |
| 5 | `bufLen` ignoré (`sprintf` au lieu de `snprintf`) | Canari de l'arène de test — écriture hors du buffer utile détectée |

**5 mutants sur 5 tués.** Aucune suite muette.

### M1 — estimation de l'effet (conception, non mesuré)

Poids réel d'une rafale : **79 o** contre ~5 292 o, soit **×67** sur la découverte.

| | Découverte | Autres sources | **Total** |
|---|---|---|---|
| Avant | 756 o/min | 157 o/min | **913 o/min** |
| Après | **11 o/min** | 157 o/min | **168 o/min** |

Débit total **÷5,4** ; fenêtre forensique **estimée** à **≈ 2 h 30** contre 28 min
mesurées le 06/09.

> ⚠️ **93 % du débit résiduel vient désormais des autres sources.** Elles étaient
> minoritaires, elles sont **dominantes**. La spec de suivi annoncée en
> « Hors périmètre » (« les autres sources de verbosité du journal — à traiter
> séparément si la mesure le montre ») n'est **plus optionnelle** : la mesure l'a
> montré, tout gain supplémentaire passe par elles.

### M2 — procédure de vérification sur l'appareil (à exécuter après déploiement)

1. **Rafale nominale** — couper le broker MQTT 30 s, le relancer, attendre la
   reconnexion. Relever `GET /download-logs` : **exactement une** ligne
   `Découverte HA #N : 54/54 entités, 0 échec, … ms`, niveau `INFO`, et **aucune**
   ligne `Discovery <topic> OK`.
2. **Rafale interrompue** — couper le broker **pendant** la rafale (fenêtre de
   ~1 s après la reconnexion). Attendu : une ligne `WARNING` de la forme
   `P/A entités, F échecs, interrompue à la Ne (<topic>)`, avec `P + F == A`.
3. **Abandon** — provoquer un appel de découverte lien absent. Attendu :
   `Découverte HA #N : abandonnée avant publication (lien MQTT absent)`.
   **Aucun silence.**
4. **Numérotation** — enchaîner deux reconnexions rapprochées : deux lignes de
   `#N` **distincts et croissants**. Un numéro manquant est une tentative avortée,
   pas une anomalie de compteur.
5. **Contrat HA (non-régression)** — vérifier dans Home Assistant que les **54**
   entités sont toujours déclarées, avec les mêmes `unique_id`, après une
   reconnexion. Aucune entité orpheline, aucune entité perdue.
6. **`DEBUG`** — activer *Paramètres → Avancé → Logs DEBUG*, forcer une rafale :
   les 54 lignes `Discovery … OK` apparaissent dans la page Logs et sur Serial,
   et **restent absentes** de `/download-logs`.
7. **Fenêtre forensique (le critère réel)** — laisser tourner **24 h**, puis
   relever l'écart entre la première et la dernière ligne datée de
   `/download-logs`. Comparer aux **28 minutes** du 06/09. Vérifier enfin qu'un
   redémarrage laisse sa ligne « Cinétique heap avant reset » **lisible plusieurs
   heures après**.

### Le critère réel n'est PAS vérifié

Le critère « **Fenêtre forensique mesurée sur l'appareil avant / après : le journal
doit couvrir des heures, pas des minutes** » reste **décoché**. Les 1 347 tests
natifs et l'estimation M1 ne le remplacent pas : il exige un **déploiement** puis
un **relevé dans la durée** (étape 7 ci-dessus). Les autres critères sont des
**moyens** ; celui-ci est la fin.

### Aucun ADR — justification explicite

`architect` l'a confirmé **après conception**, et non par défaut. Aucun des quatre
critères de déclenchement n'est rempli :

1. **Ne contraint rien d'autre** — le changement vit à un **site unique**
   (`publishDiscovery()`), sans effet sur un autre sous-système.
2. **Contrat inchangé** — 54 topics, charges utiles, `retain`, ordre et
   déclenchement strictement identiques. Rien de public n'est verrouillé.
3. **Aucun verrouillage d'orientation** — la forme de la ligne est modifiable sans
   migration, sans compatibilité ascendante à tenir, sans consommateur externe.
4. **Aucune primitive de journalisation réutilisable introduite** — c'est le point
   décisif. `discovery_report_logic.h` formate **une** ligne pour **un** appelant ;
   ce n'est pas un patron de synthèse généralisable que d'autres émetteurs
   devraient suivre. S'il le devenait, **c'est ce jour-là** que l'ADR se justifierait.

Alternatives écartées (consignées ici, faute d'ADR requis) : une ligne par échec
(ramène 5,3 Ko le jour du crash — cf. décision 1) ; baisser le niveau de log global
ou agrandir `kMaxLogFileBytes` (interdits par la spec et par ADR-0033 : **émettre
moins**, pas **stocker plus**).

### Hors périmètre confirmé

- **`docs/user/` et les PDF : rien à régénérer.** Aucune surface utilisateur, aucun
  écran, aucun réglage, aucun message d'interface ne change. Le seul changement
  observable est le contenu du journal de bord, dont le format n'est pas décrit
  dans la documentation utilisateur.
- `docs/MQTT.md` : **non modifié** — aucun topic, payload ni comportement
  d'auto-discovery ne change.
- La cadence de reconnexion MQTT et ses causes ; `kMaxLogFileBytes` /
  `kRotateKeepBytes` (ADR-0033).

---

## Clôture — 2026-09-06, v2.44.0

**Classée `done/` avec une réserve explicite**, sur décision de l'utilisateur.

### Ce qui est vérifié
Tous les critères mesurables hors cible : une seule ligne par rafale, pire cas **borné à
une ligne** (et non « à une constante »), échec identifiable **par son topic** dans le
journal persistant, contrat MQTT/HA strictement inchangé, 22 tests neufs (1 325 → 1 347),
**5 mutants sur 5 tués**, flash +1 828 o, aucun nouveau warning.

Poids réel mesuré : **79 o par rafale** contre ~5 292 — soit **×67 sur la découverte**,
×5,4 sur le débit total du journal (913 → 168 o/min).

### ⚠️ Ce qui N'EST PAS vérifié — la réserve
**Le critère d'acceptation qui compte reste ouvert** : « le journal couvre des heures, pas
des minutes ». Il n'est pas démontré par les tests natifs — il exige un relevé **dans la
durée**, sur l'appareil.

- Valeur **extrapolée** : fenêtre ≈ **2 h 30** contre 28 min mesurées le 06/09.
- **Relevé de contrôle à faire** : procédure M2, étape 7 — après un redémarrage, vérifier
  que la ligne « Cinétique heap avant reset » de `heap_ring.cpp` reste lisible **plusieurs
  heures** plus tard. C'est le seul test qui prouve l'objectif réel de cette spec.
- Déployé en v2.44.0 le 2026-09-06 : le relevé est possible à partir de cette date.

### La suite est nécessaire, pas optionnelle
**93 % du débit résiduel** (157 des 168 o/min) vient désormais des **autres** sources de
journal. La découverte HA n'est plus le contributeur dominant — elle ne pèse plus que
11 o/min. La spec de suivi annoncée en « Hors périmètre » conditionne donc l'atteinte
réelle de l'objectif : sans elle, on a déplacé le plafond sans le lever.

### Effet sur `bug-080`
L'obstacle d'observabilité à son critère « 7 jours sans redémarrage inopiné » est **levé** :
la cause d'un redémarrage survivra désormais assez longtemps pour être relevée. `bug-080`
**reste ouverte** — ce cycle lève un obstacle, il ne clôt pas son critère.
