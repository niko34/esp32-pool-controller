# Récidive de fragmentation — v2.50.0, 10/09/2026

Note factuelle **préalable à tout codage**, conforme à la phase 1 demandée.
Séparation stricte : certain / fortement probable / à mesurer.

## Chronologie reconstituée

L'`uptime` du post-mortem est stocké **modulo 65 536 s**. Le journal donne
`uptime=38404 s mod 65536` au reset.

| Instant | Événement | Source |
|---|---|---|
| **09/09 ~08:14** | Démarrage v2.50.0 (`38 404 + 65 536 = 103 940 s` avant le reset) | calcul sur l'uptime modulo |
| 09/09 ~08:16 | **OOM à uptime = 116 s**, 1 612 o `caps=0x080C`, `maxBloc=1 536` | post-mortem NVS |
| 09/09 08:23 | Test de charge volontaire (rate-limit à 122 req/min) | journal du 09/09 |
| 10/09 07:54 → 12:55 | **Plateau : `maxBloc8 = 9 716` CONSTANT, `free8 ≈ 48 Ko`** | 11 relevés |
| 10/09 09:58:47 | Sauvegarde d'historique **sautée**, `maxBloc8 = 6 644` | journal |
| 10/09 11:58:56 | Sauvegarde d'historique **sautée**, `maxBloc8 = 7 668` | journal |
| 10/09 ~13:12 | **`maxBloc8 = 7 872` pendant ≥ 78 s**, `free8 ≈ 48 Ko`, `ws=0` | cinétique pré-reset, 32 échantillons |
| **10/09 13:13:47** | **SW_RESET** (`reset_reason=3`) — redémarrage manuel par cURL | journal |

Durée de vie du firmware avant récidive : **28,9 h**.

## ✅ CERTAIN — établi par le code ET le journal

1. **Le message est bien le 503 de la garde mémoire.** `src/web_server.cpp:143`
   renvoie littéralement `Memoire insuffisante, reessayez`, avec `Retry-After: 5`.
2. **La bande morte 4 096 – 8 191 existe.** `kStaticFileMinAllocBytes = 8192`
   (`constants.h:350`) refuse les fichiers statiques ; `kRestartMaxAllocHeapBytes
   = 4096` (`constants.h:200`) déclenche l'auto-guérison. Entre les deux :
   régulation active, interface web refusée, **aucun redémarrage automatique**.
   L'appareil y a séjourné à **7 872 o** pendant au moins 78 s.
3. **Ce n'est pas un épuisement, c'est une fragmentation.** `free8 ≈ 48 Ko`
   stable pendant tout l'épisode, PSRAM à 4,18 Mo libres. Le total libre ne
   diminue pas : **aucune fuite**.
4. **Le reset est manuel.** `SW_RESET, reset_reason=3` — ni panic, ni
   auto-guérison (qui n'aurait pu se déclencher qu'à moins de 4 096).
5. **Aucun client WebSocket au moment de l'incident** : `ws=0` sur les 32
   échantillons de la cinétique.
6. **Le post-mortem n'est PAS celui du 13:13.** Il porte `uptime = 116 s`, contre
   ~103 940 s au reset. Son profil de tas (`free8=58880 maxB8=27584`) est celui
   d'un démarrage frais. Il provient d'un boot de la fenêtre de déploiement du
   09/09 ; l'attribuer à l'incident serait faux.
7. **Deux sauvegardes d'historique ont creusé le tas** à 6 644 o et 7 668 o,
   **après** construction du JSON. Vérifié dans `src/history.cpp` :
   `JsonDocument doc` puis `measureJson(doc)` **précèdent** le second
   `fsGuardAdmit()`. Le pic est donc payé avant d'être contrôlé.

### ✅ CERTAIN — le défaut de télémétrie `fs_guard`, pire que soupçonné

`fsGuardAdmit()` pose `sFopenEpisode.active = true` (`fs_guard.cpp:90`) au moment
du refus. `fopenGuardDrain()` n'initialise `episodeStartMs` et `deferralsAtStart`
que sous `if (!st.active)` (`fopen_guard_logic.cpp:44`).

**Conséquence : la branche est structurellement inatteignable.** Donc :

- `FopenGuardAction::EpisodeStart` n'est **jamais** retourné — **zéro occurrence**
  de « Ouvertures de fichiers refusées » dans le journal terrain ;
- `episodeStartMs` reste à **0** ⇒ la durée rapportée est `millis()` **depuis le
  boot** ;
- `deferralsAtStart` reste à **0** ⇒ le nombre rapporté est le **cumul** depuis le
  boot.

Vérification arithmétique sur le terrain :

| Ligne journalisée | Durée annoncée | Uptime réel à cet instant |
|---|---|---|
| « 1 refus en **92 279** s » (09:59) | 25,6 h | 25,6 h |
| « 2 refus en **99 488** s » (11:59) | 27,6 h | 27,6 h |
| « 3 refus en **102 792** s » (12:54) | 28,6 h | 28,6 h |

Les trois durées **égalent l'uptime**, et les compteurs 1→2→3 sont cumulatifs.
Aucun épisode n'est mesuré.

## 🔶 FORTEMENT PROBABLE — cohérent, non démontré

1. **La dégradation est TEMPORELLE, pas liée à la charge.** À l'incident : `ws=0`,
   `skip=0`, pas de rafale en cours, RSSI −37 dBm. Le plateau s'est installé sur
   ~29 h sans événement déclencheur identifiable.
2. **Le plateau est structurel.** `maxBloc8 = 9 716` est **exactement constant**
   sur 11 relevés couvrant 5 h, puis **exactement 7 872** sur 32 échantillons.
   Des valeurs aussi stables suggèrent une frontière de découpage du tas, pas une
   fluctuation — même phénomène que les 2 932 o reproductibles du 08/09.
3. **Le poste de dépense est ailleurs que dans les rafales HTTP.** Le contexte au
   creux du plancher `m804` bascule de `0x02` (rafale HTTP) à `0x01` (clients WS)
   après la première sauvegarde d'historique ratée.

## ❓ À MESURER — aucune conclusion permise

1. **Ce qui immobilise les blocs pendant 29 h.** Aucun candidat n'est démontré.
   Interdiction expresse de toucher aux constantes ou structures du `logger`
   sans preuve instrumentée.
2. **Pourquoi 9 716 puis 7 872**, et pourquoi ces valeurs sont si stables.
3. **La part respective** de la sauvegarde d'historique et du service HTTP dans
   l'installation du plateau.
4. **L'origine exacte** du post-mortem à 116 s (quel boot de la fenêtre de
   déploiement).

## ⚠️ CE QUI INVALIDE MES CONCLUSIONS DU 09/09

`feature-125` posait **trois conditions de réouverture** pour S2. **Aucune n'est
remplie** par cette récidive :

| Condition écrite le 09/09 | État au 10/09 |
|---|---|
| « plus de connexions simultanées » | `ws = 0`, aucune rafale |
| « un firmware plus bavard » | inchangé depuis le 09/09 |
| « un réseau plus lent » | RSSI −37 dBm, excellent |

**Les conditions étaient donc mal posées** : elles décrivaient une aggravation par
la **charge**, alors que la récidive s'installe par la **durée**. Une spec de
réserve dont les conditions ne se déclenchent pas au moment où le problème
revient ne protège de rien.

**Et la validation de S7 était insuffisante.** Le plancher de 8 692 o mesuré le
09/09 se situait à **500 o** au-dessus de la garde à 8 192 — sur un **tas frais**,
après moins d'une heure de fonctionnement. Ce test ne pouvait pas voir un plateau
qui met 29 h à s'installer.

⇒ **S7 n'est pas invalidé** — il a déplacé les structures lwIP et le facteur 8 est
réel — **mais il ne traite pas la dégradation temporelle**, qui est un phénomène
distinct et non instruit.

## Conséquences documentaires (phase 1, item 4)

- `bug-080` et `feature-124` ne doivent plus présenter le sujet comme résolu.
- `feature-125` doit être **rouverte**, et ses conditions **réécrites** autour de
  la durée, pas de la charge.
- Le protocole de validation doit exiger une mesure **après 24–48 h d'uptime**,
  jamais sur tas frais.
