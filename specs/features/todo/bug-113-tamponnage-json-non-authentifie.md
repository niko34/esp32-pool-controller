# bug-113 — 16 Ko allouables sans authentification sur trois routes

## Métadonnées
| | |
|---|---|
| Type | bug (sécurité / mémoire) |
| Statut | todo |
| Priorité | Modérée — dégradation propre, mais vecteur réel pendant la famine de `bug-080` |
| Version cible | à définir |
| Relevé par | `code-reviewer`, 2026-09-07, audit de `bug-save-config-applique-avant-authentification` |
| Specs liées | `bug-080` (famine active), `bug-101` (le plafond de 16 Ko sur l'import), `bug-112` |

## Domaines touchés
- [x] Web — routes HTTP (`src/web_routes_data.cpp`, `src/web_routes_config.cpp`, `src/web_routes_sensors.cpp`)
- [ ] Régulation — [ ] Capteurs — [ ] MQTT — [ ] Frontend — [ ] OTA

## Agents requis
- [x] architect · [x] web-ui-developer · [x] code-reviewer · [x] doc-writer
- [ ] `pool-chemistry` — sans objet.

## Description
Trois routes utilisent `AsyncCallbackJsonWebHandler`, qui **tamponne le corps entier**
avant d'appeler le callback : `/history/import`, `/sensors/onewire/identify`,
`/wifi/connect`. Le tamponnage a donc lieu **avant** toute authentification.

**Ce n'est pas** le motif « corps appliqué avant vérification » — les trois vérifient
bien avant toute mutation (audit du 07/09). Mais un client non authentifié peut faire
**allouer de la mémoire** à l'appareil.

## Contexte — les chiffres
- `_maxContentLength` vaut **16384 o** par défaut et n'est **jamais surchargé**.
- `handleBody` refuse `total > 16384` **avant** allocation, puis fait
  `calloc(total + 1)` avec test de nullité et `abort()` propre — **pas** de
  `bad_alloc`, la dégradation est correcte.
- Soit **16 Ko par requête en vol, sur 3 URI, sans authentification**.

Sur un serveur ordinaire ce serait négligeable. Ici, `bug-080` documente une **famine
mémoire active** : le 07/09, **trois chargements de page** ont suffi à produire
**21 échecs d'allocation** de tampons WiFi (1 622 o, `caps=0x080C`), avec un plus gros
bloc effondré à 832 o. 16 Ko réclamables sans identifiants ne sont plus négligeables
dans ce contexte.

## Comportement attendu
- Abaisser `setMaxContentLength()` au strict nécessaire **par route** (l'ordre de
  grandeur suggéré par l'audit est ~4 Ko ; à chiffrer sur les charges réelles —
  ⚠️ `/history/import` a besoin de place, cf. `bug-101` qui traite justement son
  plafond : **coordonner les deux specs, ne pas les traiter en aveugle**).
- Ajouter une garde sur le plus gros bloc disponible avant d'accepter le tamponnage,
  sur le patron de la garde `fopen` (ADR-0070) : refuser proprement en famine plutôt
  que d'aggraver.

## Contraintes techniques
- **Tension explicite avec `bug-101`** : cette spec veut *baisser* le plafond,
  `bug-101` constate qu'il est *trop bas* pour une restauration d'historique. La
  résolution n'est pas un nombre unique mais un plafond **par route**. À trancher
  ensemble.
- La garde mémoire doit utiliser la métrique **8 bits** (`heapLargestFreeBlock8Bit()`),
  jamais `ESP.getMaxAllocHeap()` — cf. la cause racine documentée de `bug-080`.

## Critères d'acceptation
- [ ] Plafond justifié **par route**, chiffré sur les charges réelles
- [ ] Un tamponnage refusé en famine renvoie un 503 explicite, jamais un `abort()`
- [ ] `/history/import` reste fonctionnel pour une restauration réelle (coordonné avec `bug-101`)
- [ ] Le flux de premier démarrage (`/wifi/connect`) reste fonctionnel
- [ ] `pio run` et `pio test -e native` sans régression

## Hors périmètre
- Le journal non borné : c'est `bug-112`.
