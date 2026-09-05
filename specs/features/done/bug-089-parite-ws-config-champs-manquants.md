# bug-089 — La trame WS `config` omet des champs servis par /get-config

## Métadonnées

| Champ | Valeur |
|---|---|
| Type | bug (parité — 4e occurrence du motif documenté) |
| Statut | **done — livré non déployé** (v2.39.0, 2026-09-05) |
| Version cible | 2.39.0 |
| Priorité | basse (contourné côté client pour la 088) |
| Origine | Relevé par web-ui-developer puis confirmé en revue, cycle feature-088 (31/08/2026) |
| Dernière mise à jour | 2026-09-05 |

## Le problème

`_buildConfigJson` (`src/ws_manager.cpp:719-812`) n'inclut pas tous les champs
que `/get-config` (`src/web_routes_config.cpp:102-308`) sert. Conséquence :
après une modification de config (ex. restauration des défauts, feature-088),
les AUTRES clients WS connectés ne voient pas ces champs rafraîchis avant leur
prochain `/get-config`.

> **Inventaire refait à neuf le 2026-09-05** (après 2.36.0/2.37.0/2.38.0).
> L'énumération d'origine de cette spec (« `stabilization_delay_min`,
> `regulation_speed`, `regulation_defaults` ») est **périmée** :
>
> - `stabilization_delay_min` a été **SUPPRIMÉ** par la feature-091 (v2.34.0) :
>   il n'est plus servi par `/get-config` (`regulation_defaults` est passé de 8
>   à 7 réglages), la clé POST est acceptée sans effet et le champ NVS subsiste
>   inerte. **Il n'y a plus rien à synchroniser pour ce champ.**
> - L'écart réel n'est pas de 3 clés mais de **23** : `/get-config` sert
>   **89** clés, `_buildConfigJson` **66**. Aucune clé n'est WS-seule (le sens
>   dangereux du « clignotement » est donc indemne ici).
>
> Manquantes (diff réel) : `max_cycles_per_day`, `regulation_speed`,
> `pool_size`, `ph_target_min/max/step`, `ph_offset_min/max/step`,
> `ph_auto_manual_epoch`, `ph_auto_manual_reason`, `ph_tracking_enabled`,
> `ph_container_ml`, `ph_total_injected_ml`, `ph_remaining_ml`,
> `ph_alert_threshold_ml`, `orp_tracking_enabled`, `orp_container_ml`,
> `orp_total_injected_ml`, `orp_remaining_ml`, `orp_alert_threshold_ml`,
> `regulation_defaults`, `regulation_defaults_by_size`.

C'est la **4e occurrence** du motif de parité documenté (3 récidives
`/data` ↔ WS déjà en mémoire du projet) — ici sur la trame `config`, pas la
trame d'état.

## Atténuation en place (feature-088)

`syncRegulationDefaults` (data/app.js) met en cache `regulation_defaults` et le
réinjecte dans les trames partielles — le bouton de restauration ne clignote
pas. `stabilization_delay_min` n'existe plus (feature-091) ; c'est donc
`regulation_speed` qui reste non rafraîchi par WS chez les clients tiers, avec
les 22 autres clés de l'inventaire ci-dessus.

## Ce qui est demandé

1. Inventorier par diff systématique les clés de `/get-config` absentes de
   `_buildConfigJson` (les trois citées + toute autre découverte).
2. Les ajouter à la trame `config` (attention au budget de trame et au
   commentaire de chiffrage d'octets — patron des cycles précédents).
3. Prouver la parité par le même grep bilatéral que pour la trame d'état.
4. **ENJEU PRINCIPAL (pas un bonus)** : une **sentinelle** qui casse quand une
   clé est ajoutée d'un seul côté. Le motif en est à sa 4e occurrence ; corriger
   des champs sans garde structurelle garantit une 5e.

## Budget de trame (contrainte dure)

- `_buildConfigJson` : `StaticJson<2560>`, frame **distincte** de `sensor_data`.
- Plafond DUR `kWsFrameBufBytes = 3584`, garde **par trame** (non cumulative) ;
  elle **saute le push**, elle ne tronque jamais. Dépasser = la trame `config`
  cesse silencieusement d'être livrée à TOUS les clients — régression pire que
  le bug corrigé.
- Coût estimé d'une parité **intégrale** : ~**1365 o** (662 o pour les 21 clés
  plates, 178 o `regulation_defaults`, 525 o `regulation_defaults_by_size`).
- Marge flash au moment de la rédaction : **13 911 o** (99,2 % occupés).

## Piste retenue à instruire : parité par contrat, pas par égalité brute

Plusieurs des 23 clés manquantes sont des **constantes de compilation**
(`ph_target_min/max/step`, `ph_offset_min/max/step`,
`regulation_defaults_by_size`) : elles ne changent JAMAIS à l'exécution et
n'ont donc rien à faire dans une trame poussée en boucle. Cibler l'égalité
brute des deux ensembles dépense le budget de trame pour rien.

→ Instruire une sentinelle à **liste d'exceptions explicite** : chaque clé
HTTP-seule doit être justifiée par écrit dans la liste, et le test casse dès
qu'une clé n'est ni dans la trame WS ni dans la liste.

## Critères d'acceptation

- [x] Inventaire du diff `/get-config` ↔ trame WS `config` refait et à jour
- [x] Les clés **à état variable** manquantes sont poussées par la trame WS
- [x] Les clés volontairement HTTP-seules sont listées AVEC justification écrite
- [x] Budget de trame vérifié et chiffré (gabarit documenté à jour), marge
      restante sous `kWsFrameBufBytes` explicitée
- [x] Sentinelle implémentée (ou écartée avec justification chiffrée) et
      **validée par mutation** : ajouter une clé d'un seul côté doit la tuer
- [x] Coût flash mesuré et compatible avec la marge disponible

## Notes d'implémentation (v2.39.0, 2026-09-05)

### Ce qui a été retenu

**Critère de partage** — c'est le vrai livrable, il contraindra tous les ajouts
futurs :

> La trame porte l'état **mutable à l'exécution** et **non dérivable** ;
> tout le reste est HTTP-seul et **déclaré**.

La parité **intégrale** a été explicitement rejetée : **+1 365 o** sur un plafond
**dur** (`kWsFrameBufBytes = 3584`) dont la garde `measureJson(doc) + 1 > cap`
**saute le push** au lieu de tronquer. Elle aurait remplacé des champs manquants
chez certains clients par **une trame absente pour tous**. Décision formalisée en
[ADR-0065](../../../docs/adr/0065-trame-websocket-etat-mutable-pas-constantes.md).

- **15 clés ajoutées** : `max_cycles_per_day`, `regulation_speed`, `pool_size`,
  `ph_auto_manual_epoch`, `ph_auto_manual_reason`, + les 10 clés de suivi produit
  pH/ORP.
- **8 exceptions déclarées avec motif écrit** : les 6 bornes de réglage pH et
  `regulation_defaults_by_size` (**constantes de compilation**),
  `regulation_defaults` (**dérivable** côté client depuis `pool_size` + cache
  `by_size` ; l'extraire de `fillRegulationDefaults` coûterait **+703 o**).

### Chiffres

| Grandeur | Valeur |
|---|---|
| Ajout de trame | **+866 o** au pire cas |
| dont `ph_auto_manual_reason` | **452 o** (427 o de valeur) |
| Trame `config` nominale | **2 053 o** |
| Trame `config` pire cas | **≈ 2 948 o** |
| Marge sous `kWsFrameBufBytes` (3 584) | **636 o** |
| Gabarit `StaticJson<>` | 2560 → **2944** (documentaire ArduinoJson 7, **0 o de RAM**) |
| Flash | **1 821 813 / 1 835 008 o**, marge **13 195 o** |
| Tests | **32 suites / 1 232 cas**, **4 mutations tuées** |

⚠️ **Chiffre rectifié en revue — facteur 2,7.** Le bloc de dimensionnement
annonçait 167 o pour `ph_auto_manual_reason` (« ~140 caractères »). Les chaînes
réelles, reconstruites depuis leur **unique écrivain** `setPhAutoManualTrace()`,
font **411 caractères / 427 octets** (branche diagnostic de sonde) et 325 / 340
(branche hors norme). **Aucun dépassement, rien à corriger dans le code** — mais
c'est ce chiffre que lira le prochain contributeur, et le pire cas de longueur
**coïncide avec l'escalade de sécurité pH**, donc le moment où perdre la trame
coûterait le plus cher.

**Réserve connue** : `server`, `topic`, `username`, `wifi_ssid` ne sont bornés
nulle part par `/save-config` ; la marge est prouvée sous l'hypothèse d'environ
64 o par champ. Surveiller `oversizedFrameCount()`.

### La sentinelle — l'enjeu réel du lot

`test/test_native_ws_config_parity/test_ws_config_parity.cpp`. Le motif en est à
sa **4ᵉ occurrence** ; la leçon retenue avait toujours été « penser à vérifier des
deux côtés », une discipline **orale**, donc oubliée quatre fois. La sentinelle la
remplace par une garde qui ne dépend de la vigilance de personne.

Lecture du **texte source** entre des **marqueurs commentés**
(`>>> PARITE-CONFIG:DEBUT` / `<<< PARITE-CONFIG:FIN`) — et non des bornes de
fonction, qui casseraient au premier refactor. Quatre assertions :

1. clé HTTP non couverte → échec ;
2. **clé WS-seule** → échec (sens **dangereux** : un champ WS-seul ne disparaît
   pas, il **clignote**, la charge `/get-config` l'écrasant périodiquement) ;
3. **marqueur absent** → échec explicite : « restaurer le marqueur, **ne pas
   désactiver le test** » (+ plancher de 60 clés, contrôle négatif) ;
4. **exception morte** → échec — c'est elle qui **empêche la liste de pourrir**.

Motif écrit **≥ 20 caractères asserté** par exception, doublons interdits,
décompte d'inventaire vérifié. **Coût : zéro octet de flash** (test natif).

⚠️ **Angle mort assumé et documenté** : extraction **textuelle** — une clé mise en
commentaire entre les marqueurs reste comptée. La sentinelle vise **la
distraction, pas la malveillance** ; parser du C++ ici serait un remède pire que
le mal.

**4 mutations tuées** : clé ajoutée côté HTTP seul · clé ajoutée côté WS seul ·
marqueur supprimé · exception laissée dans la table après retrait de la clé HTTP.

### Fichiers modifiés

- `src/ws_manager.cpp` — `_buildConfigJson()` : marqueurs, 15 clés, gabarit
  2560 → 2944, bloc de dimensionnement rectifié, liste des exclusions.
- `src/web_routes_config.cpp` — `handleGetConfig()` : marqueurs + convention
  normative (receveur `doc[`).
- `data/app.js` — `syncRegulationDefaults()` : reconstruction de la colonne
  courante depuis `pool_size` + cache `by_size` (au lieu d'un cache périmé).
- `src/version.h` — 2.38.0 → 2.39.0.
- `test/test_native_ws_config_parity/test_ws_config_parity.cpp` — **nouveau**.
- Docs : `docs/adr/0065-*.md` (**nouveau**), `docs/adr/README.md`,
  `docs/README.md`, `docs/subsystems/ws-manager.md`, `docs/API.md`,
  `docs/features/page-settings.md`, `CHANGELOG.md`.

### Hors périmètre / non fait

- **Aucun topic MQTT ajouté.**
- **Aucun guide utilisateur ni PDF régénéré, aucune capture d'écran à refaire** :
  aucun écran, libellé ou réglage ne change — seul le rafraîchissement
  multi-clients s'améliore, ce qui ne se photographie pas.

### Statut de déploiement

**Livré non déployé** — compilé, tests verts, revue OK ; pas encore flashé sur
l'appareil.
