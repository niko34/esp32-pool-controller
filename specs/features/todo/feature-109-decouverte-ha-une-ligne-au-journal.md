## Métadonnées
- **ID** : feature-109
- **Slug** : decouverte-ha-une-ligne-au-journal
- **Type** : feature (observabilité)
- **Statut** : todo
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
- [ ] A1 — une ligne de synthèse par rafale, avec compte publié / compte total / échecs / durée
- [ ] A2 — les échecs restent **nommément** identifiables
- [ ] A3 — distinguer une rafale complète d'une rafale interrompue
- [ ] A4 — découverte MQTT inchangée : topics, charges utiles, `retain`, déclenchement
- [ ] A5 — vérifier si un niveau `DEBUG` non persisté existe ; l'employer pour le détail, ou dire pourquoi ce n'est pas possible
- [ ] D1 — test natif de la logique de synthèse (module pur ADR-0017), **validé par mutation**
- [ ] D2 — non-régression des tests existants
- [ ] C1 — `docs/subsystems/mqtt-manager.md`, `docs/subsystems/logger.md`
- [ ] C2 — `CHANGELOG.md` : le journal devient exploitable, c'est un fait utilisateur
- [ ] C3 — mettre à jour `bug-080` : l'obstacle à son critère « 7 jours » est levé

## Critères d'acceptation
- [ ] Une rafale complète produit **exactement une ligne** au journal persistant (plus une
      par échec réel, borné)
- [ ] **Fenêtre forensique mesurée sur l'appareil** avant / après : le journal doit couvrir
      des **heures**, pas des minutes. C'est le critère qui compte — les autres sont des moyens
- [ ] Un échec de publication reste identifiable par son topic
- [ ] Aucune modification des topics, charges utiles ou cadence de la découverte
- [ ] `pio run` sans nouveau warning ; `pio test -e native` sans régression

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
