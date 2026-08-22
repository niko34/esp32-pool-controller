# ADR-0040 — On ne calibre pas des seuils sur une fragmentation pathologique (régime bi-métrique assumé)

- **Statut** : Accepté
- **Date** : 2026-08-15
- **Décideurs** : `architect`, `pool-chemistry` (**gate — NO GO sur la bascule des gardes restantes**), `code-reviewer`, utilisateur
- **Spec(s) liée(s)** : `specs/features/done/bug-famine-8bits-parachute-mort-et-gardes-aveugles.md` (v2.20.2), suite : `specs/features/todo/bug-bascule-gardes-8bits-recalibrage.md`

## Contexte

[ADR-0038](0038-metrique-memoire-allouable-8-bits.md) (v2.19.0) a identifié la cause racine de trois crashs : `ESP.getFreeHeap()` / `ESP.getMaxAllocHeap()` mesurent `MALLOC_CAP_INTERNAL`, **IRAM 32 bits comprise**, donc de la mémoire qu'aucun `malloc`/`new` ne peut utiliser. Il a explicitement **découpé** le chantier : v2.19.0 = **mesurer** (risque nul), v2.19.1 = **basculer les gardes et recalibrer**, après ≥ 24 h de relevés on-target, selon la règle `kRestartMaxAllocHeapBytes = clamp(0,40 × min24h ; 8192 ; 16384)` — règle validée par le gate chimique **en supposant un `min24h` de l'ordre de plusieurs dizaines de kilo-octets**.

Les relevés sont arrivés (firmware 2.20.0 core 3, **5 h** d'uptime — l'utilisateur a demandé le cycle malgré cette réserve, qui est assumée) :

```
libre8 = 46 480      maxBloc8 = 4 596      minMaxBloc8_boot = 3 060
interne: libre = 74 472   maxBloc = 27 636   ← FIGÉ (reliquat IRAM 32 bits)
```

Sur 1 h de traces : `maxBloc8 ∈ [4 084 ; 9 204]`, `minMaxBloc8_boot = 3 060` constant.

Deux faits changent la donne :

1. **Ce n'est pas un épuisement, c'est une fragmentation à 10:1.** `libre8` est stable et sain (40-50 Ko), mais **aucun bloc contigu ne dépasse 4,6 Ko**. Le `min24h` attendu « en dizaines de Ko » vaut en réalité **3 060 octets**.
2. **Le signal de mesure était lui-même fabriqué par la garde.** Le tick de réarmement lisait la métrique brute figée à 27 636, concluait donc toujours `27 636 ≥ 24 576` et retentait un `malloc(12288)` **voué à l'échec toutes les 10 s**. Sur les **1 025** échecs d'allocation comptés, **~100 %** étaient ce `malloc`-là.

Appliquer la règle d'ADR-0038 telle quelle : `0,40 × 3 060 = 1 224` → **écrêté au plancher 8 192**. Or `8 192 > 3 060` : le seuil de redémarrage d'auto-guérison serait **au-dessus du minimum observé en fonctionnement normal**. L'appareil redémarrerait en boucle, puis basculerait en **mode maintenance — c'est-à-dire DOSAGE SUSPENDU** — au bout de 3 redémarrages, en quelques minutes. Le mode maintenance est un cul-de-sac connu ([ADR-0029](0029-auto-guerison-heap.md)) : on n'en sort que par crash ou reboot manuel.

Il faut donc trancher **maintenant**, avant tout codage : faut-il calibrer des seuils sur une fragmentation pathologique, ou traiter d'abord la fragmentation ?

## Décision

**On ne calibre pas des seuils de sécurité sur une fragmentation pathologique.** Deux conséquences opérationnelles, actées en v2.20.2 :

1. **La règle de recalibrage publiée dans ADR-0038 est déclarée INAPPLICABLE en l'état.** `clamp(0,40 × min24h ; 8192 ; 16384)` reposait sur une hypothèse de `min24h` qui s'est révélée fausse d'un ordre de grandeur. Elle ne sera pas appliquée, et les relevés du 15/08 sont **invalidés** comme base de calibrage (≈ 100 % du signal d'échec était auto-infligé).

2. **Le firmware entre dans un régime BI-MÉTRIQUE explicitement assumé.** Seuls **les deux paliers de réarmement du parachute OOM** basculent sur la mesure 8 bits (`heapLargestFreeBlock8Bit()`), avec leurs valeurs recalibrées sous invariant vérifié. **Toutes les autres gardes restent sur la métrique brute** — `kRestartMaxAllocHeapBytes`, `kHttpHeapMarginBytes`, `kWsPushMinAllocBytes`, `kLowHeapWarnBytes`, `kCheckUpdateMinFreeHeapBytes`, `kCheckUpdateMinMaxAllocBytes` — **par NO GO explicite du gate `pool-chemistry`**.

Le réarmement est le **seul** seuil éligible parce qu'il est le seul dont l'échec est **non destructif** : ne pas réarmer coûte une réserve d'urgence absente ; se tromper sur l'auto-guérison coûte l'**arrêt du dosage**. Ce qui rendait sa bascule **urgente**, c'est qu'en régime brute il fabriquait 3 faux échecs par fenêtre de 30 s, saturait le journal et faussait le verdict de famine.

Valeurs recalibrées sous **invariant C2** du gate, vérifié dans [`constants.h`](../../src/constants.h) :

| Constante | Avant | Après | Contrainte vérifiée |
|---|---|---|---|
| `kOomFallbackRearmMinMaxAllocBytes` | 12 288 | **2 048** | `2 048 < 3 060` (pire creux depuis le boot) et `2 048 ≤ 4 084` (min du régime établi) |
| `kOomFallbackReserveBytes` | 2 048 | **1 024** | `1 024 ≤ 2 048 / 2` — allouer la réserve ne peut pas à soi seul faire retomber le tas sous son propre seuil |
| `kOomRearmMinMaxAllocBytes` | 24 576 | **24 576** (inchangé) | Délibérément **inatteignable** en régime fragmenté → plus aucun `malloc(12288)` répété ; le réarmement nominal repart seul si la fragmentation disparaît |

**Condition d'entrée de la bascule différée** : ≥ **24 h de relevés propres sous 2.20.2** (signal débruité des échecs auto-infligés) **et** identification du fragmenteur via la capture de `function_name`.

## Alternatives considérées

- **Appliquer la règle d'ADR-0038 telle quelle** (rejetée) — c'était le plan écrit, validé, prêt à coder. Avec `min = 3 060`, il **briquait l'appareil** : reboots en boucle puis dosage suspendu en quelques minutes. Une décision antérieurement validée reste conditionnée aux hypothèses sous lesquelles elle l'a été ; quand la mesure les dément, on ne l'applique pas « parce qu'elle est écrite ».
- **Basculer toutes les gardes sur la mesure 8 bits en laissant les seuils actuels** (rejetée) — arithmétiquement destructeur : marge HTTP 4 096 contre `maxBloc8` courant 4 596 → **503 quasi permanents** sur l'interface web ; `kWsPushMinAllocBytes` 16 384 > 4 596 → **plus aucun push WebSocket** ; `kCheckUpdateMinFreeHeapBytes` 65 536 > `libre8` 46 480 → **mise à jour en ligne définitivement refusée** ; `kRestartMaxAllocHeapBytes` 8 192 > 3 060 → **dosage suspendu**. La bascule sans recalibrage n'est pas une demi-mesure prudente, c'est la panne.
- **Descendre tous les seuils sous 3 060 pour respecter l'invariant d'ordre** (rejetée) — mécaniquement possible, mais cela revient à **entériner la fragmentation comme état nominal** : les gardes deviendraient si basses qu'elles ne protégeraient plus de rien, tout en donnant l'illusion d'être calibrées. On aurait échangé une garde aveugle contre une garde inerte.
- **Ne rien basculer du tout et attendre 24 h de relevés** (rejetée) — c'était l'option la plus conservatrice, mais elle laissait la **boucle de réarmement infinie** en place : or c'est elle qui **empêche d'obtenir des relevés propres** (elle fabrique son propre signal d'alarme) et qui sature le journal. Attendre aurait garanti que la prochaine mesure soit aussi inexploitable que celle-ci.
- **Retenue : bascule du seul réarmement + gel documenté des autres gardes + condition d'entrée explicite pour la suite** — seule option qui débruite l'instrument sans mettre le dosage en jeu, et qui rend la dette **visible et bornée** au lieu de silencieuse.

## Conséquences

### Positives

- **Le signal de mesure est débruité** : les échecs auto-infligés disparaissent du compteur et du post-mortem, la prochaine campagne de relevés porte sur des échecs **réels**.
- **La boucle infinie de réarmement est fermée** : plus de `malloc(12288)` toutes les 10 s, plus de 3 faux WARN par fenêtre de 30 s.
- **Le parachute reste armé en régime fragmenté** : le palier de repli est désormais atteignable (`2 048 < 3 060`), là où l'ancien seuil de 12 288 le rendait inaccessible.
- **Aucun risque nouveau sur le dosage** : aucune garde décisionnelle ne change de grandeur comparée, donc **aucun comportement de sécurité ne bouge** dans cette version.
- La dette est **nommée, chiffrée et datée** dans les deux sous-systèmes concernés et dans une spec de suivi, au lieu de rester une incohérence apparente.

### Négatives / dette assumée

- **Le firmware est dans un état bi-métrique**, plus difficile à lire : deux seuils comparent une grandeur, cinq en comparent une autre. L'**invariant d'ordre** d'ADR-0038 (`restart < rearmOom ≤ wsPush < lowHeapWarn < min24h`) est **momentanément sans signification**, puisque ses membres ne sont plus homogènes.
- **Les gardes restantes demeurent aveugles** : l'auto-guérison ne peut toujours pas se déclencher (8 192 sur une métrique figée à 27 636), la garde TLS refuse sur des chiffres qui ne décrivent pas le tas réel.
- **La cause de la fragmentation reste inconnue.** Cet ADR débruite l'instrument ; il n'identifie pas le fragmenteur (c'est l'objet de la capture de `function_name`).
- La réserve de repli passe de 2 Ko à **1 Ko** : elle ne couvre plus que ~4× le duo fautif du 13/08 au lieu de ~8×. Contrepartie acceptée d'un palier réellement armable.
- Le calibrage repose sur **5 h** de relevés, pas 24 h. Réserve explicitement portée au gate et **assumée** par l'utilisateur.

### Ce que ça verrouille

- **Aucun seuil de garde ne sera calibré sur des mesures prises pendant que la garde elle-même fabrique le signal.** Tout recalibrage exige au préalable la démonstration que le signal mesuré est **exogène**.
- **Toute bascule de garde vers la métrique 8 bits doit passer par le gate `pool-chemistry`**, avec l'arithmétique des seuils explicitée avant codage, et suit l'ordre du **risque croissant** : pushes WS et gardes HTTP/TLS d'abord, **auto-guérison en dernier**.
- **Le régime bi-métrique doit être signalé partout où un seuil est cité** : toute table de seuils du projet porte désormais la colonne « métrique lue ». Un seuil sans métrique déclarée est un défaut de documentation.
- **La règle `clamp(0,40 × min24h ; 8192 ; 16384)` ne doit plus être citée comme applicable** sans re-dérivation sur des relevés propres ; ADR-0038 porte une note additive datée à ce sujet.
- **Le mode maintenance reste un état à ne jamais rendre atteignable en régime normal** : tout seuil qui le permettrait est rejeté d'office, quelle que soit l'élégance de la formule qui le produit.

## Références

- Code : [`src/oom_guard.cpp`](../../src/oom_guard.cpp) (tick : ré-inscription, réarmement 8 bits, exclusion des échecs auto-infligés, backoff), [`src/oom_guard_logic.h`](../../src/oom_guard_logic.h) (`oomShouldRegisterHandler`, `oomFailWarnNextDelayMs`, `oomShouldCountFailure`), [`src/constants.h`](../../src/constants.h) (échelle des seuils annotée par métrique), [`src/main.cpp`](../../src/main.cpp) (`checkHeapHealth` — cadence de la trace)
- Spec : `specs/features/done/bug-famine-8bits-parachute-mort-et-gardes-aveugles.md` ; suite : `specs/features/todo/bug-bascule-gardes-8bits-recalibrage.md`
- Doc : [subsystems/oom-guard.md](../subsystems/oom-guard.md), [subsystems/system-health.md](../subsystems/system-health.md), [subsystems/logger.md](../subsystems/logger.md), [API.md](../API.md#get-get-system-info--write)
- Tests : `test/test_native_oom_guard/` (82 tests, +16)
- ADR liés : [ADR-0038](0038-metrique-memoire-allouable-8-bits.md) (métrique 8 bits — règle de recalibrage remise en cause ici), [ADR-0035](0035-parachute-oom-new-handler.md) (parachute OOM — invariant de ré-inscription), [ADR-0029](0029-auto-guerison-heap.md) (auto-guérison et mode maintenance), [ADR-0037](0037-postmortem-oom-et-integrite-tas.md) (post-mortem et verdict), [ADR-0032](0032-reponses-http-bornees.md) (gardes HTTP à seuil fixe)
