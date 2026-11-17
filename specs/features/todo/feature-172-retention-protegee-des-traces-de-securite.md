## Métadonnées
- **ID** : feature-172
- **Slug** : retention-protegee-des-traces-de-securite
- **Type** : feature (sécurité / observabilité)
- **Statut** : todo
- **Priorité** : **haute** — c'est ce qui ferme réellement [`bug-112`](../done/bug-112-journal-non-borne-sur-route-protegee.md), clos sur un résidu assumé
- **Version cible** : à définir
- **Dernière mise à jour** : 2026-11-16
- **Spec(s) liée(s)** : [`bug-112`](../done/bug-112-journal-non-borne-sur-route-protegee.md) · [`bug-183`](../todo/bug-183-auth-login-echecs-muets-et-comparaison-non-constante.md) · [`feature-151`](../doing/feature-151-signature-de-l-image-ota.md) (la trace des autorisations OTA) · `bug-080` (le critère « 7 jours sans redémarrage ») · [ADR-0125](../../../docs/adr/0125-l-expiration-de-l-interrupteur-de-signature-se-mesure-sur-un-horodatage-nvs.md) · [ADR-0127](../../../docs/adr/0127-le-bornage-du-journal-d-authentification-est-global-par-emetteur.md)

## Domaines touchés
- [ ] Capteurs — [ ] Régulation — [ ] Filtration / Éclairage — [ ] UART / écran
- [x] Web — routes HTTP (lecture de la rétention)
- [ ] MQTT / Home Assistant
- [x] OTA (la trace des autorisations temporaires doit survivre)
- [x] Authentification
- [x] Historique / persistance
- [ ] Frontend (`data/`) — à confirmer : une surface de lecture peut être nécessaire

## Agents requis
- [x] spec-reader · [x] architect · [x] embedded-developer · [x] test-engineer · [x] code-reviewer · [x] doc-writer
- [x] **`security-reviewer`** — obligatoire, **P1 sur le design avant code**. C'est la place et la taille de la rétention qui portent le risque, et elles ne se lisent pas dans un diff.
- [ ] `pool-chemistry` — sans objet.
- [ ] `ux-ui-designer` — ⚠️ à cocher **si** une surface de lecture est ajoutée dans `data/`.

## Description

Donner aux **traces de sécurité et de redémarrage** une rétention **séparée** du journal courant, hors d'atteinte des lignes qu'un client **non authentifié** peut provoquer.

## Contexte

`bug-112` a borné neuf émetteurs : un client sans identifiant ne purge plus le journal en **13 secondes**. Mais le lot a été clos sur un **résidu assumé et chiffré** : au pire mesuré, **38 lignes / 24 h et par site**, **342 cumulées**, soit ~79 Ko contre un journal de **8 Ko** (6 144 o conservés). ⇒ **Le journal reste purgeable à vitesse lente.**

⭐ **Et la revue de sécurité a conclu que l'énumération ne peut pas converger** (quatrième passe, verdict GO) :

> *« Restent les émetteurs indirects — alertes de mémoire ou de réseau provoquées par la charge d'un client anonyme — qu'aucun inventaire de routes ne voit. Seule la rétention séparée ferme ce sujet. »*

**C'est l'argument central de cette spec.** Trois passes de revue ont chacune trouvé des émetteurs que la précédente avait manqués : le compte est passé de 2 à 5, puis 8, puis 9. Borner un émetteur de plus ne change pas la nature du problème — **tant que toutes les lignes partagent un fichier de 8 Ko, chaque ligne nouvelle est une porte**.

**Ce qui doit survivre, nommément :**
1. La ligne « Cinétique heap avant reset » d'un redémarrage inopiné — c'est la preuve du critère « 7 jours sans redémarrage » de `bug-080`, et une capture automatique quotidienne en dépend.
2. ⭐ **La trace d'une autorisation temporaire de signature OTA** (`feature-151`). ADR-0125 établit que le témoin MQTT **n'est pas une preuve** : un firmware hostile lit les identifiants en NVS et republie `OFF`, un courtier sans persistance perd le `retain`. **Le journal persistant est l'autre témoin** — et il est effaçable.
3. Les refus d'authentification, une fois `bug-183` livré.

## Comportement attendu

### Le principe

Une **rétention dédiée**, de taille fixe, où n'écrivent que des événements **que le client ne choisit pas** — et dont le débit est donc borné par la **réalité**, pas par un limiteur.

⚠️ **C'est le critère de conception qui décide tout** : si un client anonyme peut provoquer l'écriture d'une ligne dans cette rétention, elle ne vaut pas mieux que le journal actuel. Le filtre n'est pas « c'est important », c'est **« qui déclenche l'écriture »**.

### Questions à trancher par l'`architect`, chiffrées

1. **Où.** Une zone dédiée de la partition `history` (256 Ko à l'offset 0x7B0000, layout v5, ADR-0068) ? Un fichier séparé ? La NVS ? ⚠️ L'espace flash est **contraint** et la partition `history` porte déjà `/history.json` et `/system.log` : toute réservation se prend sur de l'existant. **Chiffrer ce qu'on retire.**
2. **Combien.** Quelle taille pour quelle durée de rétention ? Partir du **débit réel** des événements retenus, pas d'un nombre rond.
3. **Quels événements, exactement.** Liste **fermée**, nommée, avec pour chacun : qui le déclenche, et pourquoi un anonyme ne le peut pas. ⛔ Une catégorie ouverte (« les lignes critiques ») redeviendra une porte.
4. **Que fait-on quand c'est plein.** Rotation, ou refus d'écrire ? ⚠️ Une rotation permet d'évincer les traces anciennes en provoquant des traces neuves — à n'autoriser que si le point 3 rend ce déclenchement impossible à un anonyme.
5. **Comment on la lit.** Nouvelle route authentifiée ? Section de `/download-logs` ? ⛔ Une rétention illisible ne sert à rien.
6. **Migration.** Un appareil en service doit-il perdre son historique ? ⚠️ Ce projet a **déjà** connu une boot-loop sur une partition d'historique corrompue (07/2026, disjoncteur d'effacement automatique en v2.10.0, `main.cpp:500`). Tout changement de disposition se conçoit **avec** le chemin de reprise.

### Cas limites

- **Famine mémoire** : l'écriture doit être possible **sans allocation** — c'est précisément quand la mémoire manque qu'on veut la trace (`bug-080` : 21 échecs d'allocation en trois chargements de page).
- **Écriture pendant un OTA** : `setOtaInProgress` suspend des chemins d'écriture ; ⚠️ la trace d'une autorisation OTA ne doit pas être perdue **parce qu'un OTA est en cours** — c'est exactement le moment qui compte.
- **Horloge invalide** : une trace sans date reste utile ; ⛔ ne pas refuser d'écrire faute d'heure.
- **Partition corrompue** : le disjoncteur existant ne doit pas effacer la rétention protégée en même temps que l'historique, ou alors il doit le **dire**.

## Comportement actuel

Toutes les lignes — diagnostic, régulation, sécurité, redémarrages — partagent **un seul** `/system.log` de 8 192 o, dont 6 144 survivent à la rotation (`src/logger.h:117-118`). Aucune ligne n'est protégée des autres. Un client non authentifié, bien que borné depuis `bug-112`, fait encore tourner ce fichier.

## Contraintes techniques

- ESP32, flash 8 Mo, layout v5 (ADR-0068) — ⚠️ `partitions.csv` touché ⇒ **ADR obligatoire** et chemin de migration.
- ⛔ **Le chemin d'écriture de la rétention n'alloue pas** (ADR-0082, `tools/check_memory_discipline.sh`).
- ⛔ Aucune ouverture de fichier hors du helper unique (ADR-0070, `tools/check_fopen_guard.sh`).
- Watchdog : aucune écriture flash synchrone sur un chemin périodique.
- ⚠️ La métrique qui compte pour juger l'impact mémoire est **`max_alloc_8bit`**, pas la mémoire libre (cause racine du 15/08/2026).

## Livrables — check-list

- [ ] A1 — Liste **fermée** des événements retenus, avec pour chacun son déclencheur et la preuve qu'un anonyme ne peut pas le provoquer
- [ ] A2 — Emplacement et taille, **chiffrés**, avec ce qui est retiré à l'existant
- [ ] A3 — Écriture sans allocation, sans blocage, horloge invalide tolérée
- [ ] A4 — Politique de remplissage (rotation ou refus) et sa justification
- [ ] A5 — Lecture par une route **authentifiée**
- [ ] A6 — Chemin de migration pour un appareil en service, et interaction avec le disjoncteur d'auto-effacement
- [ ] B1 — ⭐ **Étendre `tools/check_unauth_log_guard.sh` d'une liste blanche des routes sans garde** : toute route neuve sans `REQUIRE_AUTH` échoue en CI tant qu'elle n'est pas déclarée. ℹ️ Idée de la revue P2 du 2026-11-16 : cela ferme la limite connue de la règle 3 (elle ne juge que les lambdas, la version jugeant les fonctions nommées produisait **48 faux positifs**) **sans** analyse d'appels.
- [ ] D1 — Tests natifs de la logique de rétention, validés **par mutation**
- [ ] D2 — Essai de terrain : provoquer les événements retenus, saturer le journal courant par des requêtes non authentifiées, **vérifier que la rétention survit**
- [ ] C1 — ADR (disposition flash + critère « qui déclenche l'écriture »), `docs/subsystems/logger.md`, `docs/API.md`, `CHANGELOG.md`

## ADR attendu ?
- **Probable** : **oui**
- **Critère(s)** : 1 (contrainte structurante), 2 (le critère de sélection est invisible dans le code), 3 (alternatives : partition dédiée / fichier / NVS), 4 (une disposition flash verrouille les mises à jour futures)
- **Sujet pressenti** : « Les traces de sécurité ont une rétention séparée, dont le critère d'admission est le déclencheur, pas l'importance »

## Critères d'acceptation

- [ ] Après 10 000 requêtes non authentifiées, les traces de sécurité et de redémarrage sont **toujours là**
- [ ] Aucun événement de la liste fermée ne peut être provoqué par un client anonyme — **démontré ligne par ligne**, pas affirmé
- [ ] Une trace d'autorisation OTA survit à une saturation complète du journal courant
- [ ] L'écriture n'alloue pas, `max_alloc_8bit` stable avant / après
- [ ] Un appareil en service migre sans perdre son historique, ou le perd **en le disant**
- [ ] `pio run` sans nouveau warning ; gardes CI vertes

## Plan de test manuel

1. Provoquer un redémarrage inopiné, vérifier la trace dans la rétention.
2. Armer une autorisation temporaire de signature OTA, puis saturer le journal courant par 10 000 requêtes non authentifiées. La trace de l'armement doit être **intacte**.
3. Remplir la rétention jusqu'à sa limite ; observer la politique choisie en A4.
4. Débrancher l'horloge (heure invalide), provoquer un événement retenu : il doit être écrit.
5. Flasher depuis la version précédente : vérifier la migration et l'absence de boot-loop.

## Notes pour agent

- ⚠️ **Le critère d'admission n'est pas « c'est important », c'est « qui déclenche l'écriture ».** Une rétention qui accueille « les lignes critiques » finira par accueillir une ligne critique qu'un anonyme peut provoquer, et le problème recommencera. C'est la leçon chiffrée de `bug-112`.
- ⚠️ `bug-112` a été clos **sur un résidu assumé** : cette spec n'est pas un confort, c'est la seconde moitié du travail.
- ⚠️ Toucher `partitions.csv` sur une flotte en service n'est pas anodin — le projet a perdu un historique et subi une boot-loop sur ce terrain (07/2026).

## Hors périmètre

- Le bornage des émetteurs : **fait**, `bug-112`.
- Les échecs de `/auth/login` : [`bug-183`](../todo/bug-183-auth-login-echecs-muets-et-comparaison-non-constante.md).
- `rateLimitMap` : `feature-131`. Routes `/debug/*` : [`feature-070`](feature-070-durcissement-routes-debug-et-cooldown-reset.md). `/get-config` : [`bug-181`](../done/bug-181-get-config-expose-la-configuration-sans-authentification.md).
- Le retrait du code mort `ArduinoOTA` (`src/ota_manager.cpp:57-66`, un ERROR d'authentification anonyme avec écriture flash synchrone s'il était rebranché) : **simple suppression**, à faire au prochain passage dans ce fichier — ⛔ pas une spec.
