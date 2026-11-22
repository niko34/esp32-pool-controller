# Bug — bascule des 5 gardes restantes sur la métrique 8 bits + recalibrage des seuils

- **Type** : bug critique différé (mémoire / disponibilité / **sécurité chimique**)
- **Statut** : ✅ **CLOSE le 2026-11-22 — SANS OBJET** : chantier livré par bug-080 lot A (v2.33.0, commit `c0b33f0`). Voir la clôture en fin de fiche.
- **Priorité** : haute — mais **surtout pas urgente** : agir trop tôt suspend le dosage
- **Version cible** : ≥ 2.21.0
- **Créée** : 2026-08-15, à la clôture de la v2.20.2
- **Origine** : bloc E **différé** de `specs/features/done/bug-famine-8bits-parachute-mort-et-gardes-aveugles.md`
- **Domaine** : Mémoire, OOM guard, auto-guérison, WebSocket, HTTP, OTA, et **Régulation** (mode maintenance)
- **ADR liés** : [ADR-0038](../../../docs/adr/0038-metrique-memoire-allouable-8-bits.md), [ADR-0040](../../../docs/adr/0040-pas-de-calibrage-sur-fragmentation-pathologique.md), [ADR-0029](../../../docs/adr/0029-auto-guerison-heap.md), [ADR-0035](../../../docs/adr/0035-parachute-oom-new-handler.md)

## Verdict du gate `pool-chemistry` (2026-08-15) — NO GO

Le gate a **refusé** la bascule des gardes restantes dans le cycle v2.20.2. Seul le bloc E1 (paliers de réarmement du parachute) a été autorisé. Motifs consignés :

1. **La règle de recalibrage publiée dans ADR-0038 est INAPPLICABLE.** `kRestartMaxAllocHeapBytes = clamp(0,40 × min24h ; 8192 ; 16384)` avait été validée en supposant un `min24h` de plusieurs dizaines de Ko. Avec `min = 3 060`, `0,40 × 3 060 = 1 224` est **écrêté au plancher 8 192**, or `8 192 > 3 060` : seuil de redémarrage **au-dessus du minimum observé en fonctionnement normal** → redémarrages en boucle → **mode maintenance = DOSAGE SUSPENDU** au 3ᵉ redémarrage, en quelques minutes.
2. **Basculer les autres gardes aux valeurs actuelles casse le produit** : marge HTTP 4 096 contre `maxBloc8` 4 596 → **503 quasi permanents** ; `kWsPushMinAllocBytes` 16 384 > 4 596 → **plus aucun push WebSocket** ; `kCheckUpdateMinFreeHeapBytes` 65 536 > `libre8` 46 480 → **mise à jour en ligne définitivement refusée**.
3. **Les mesures sont invalidées** : ~100 % du signal d'échec (1 025 échecs comptés) était **fabriqué par la garde de réarmement elle-même**. On ne calibre pas sur du bruit auto-infligé.
4. **Doctrine retenue** : **on ne calibre pas des seuils sur une fragmentation pathologique.** Voir [ADR-0040](../../../docs/adr/0040-pas-de-calibrage-sur-fragmentation-pathologique.md).

## Périmètre — 5 gardes, 6 constantes restées sur la métrique BRUTE

(la garde TLS sortante en porte deux à elle seule)


| Constante | Valeur | Garde | Risque en cas d'erreur |
|---|---|---|---|
| `kRestartMaxAllocHeapBytes` | 8 192 | Reboot d'auto-guérison | **MAXIMAL — mode maintenance = dosage suspendu** |
| `kHttpHeapMarginBytes` | 4 096 | Marge d'admission des réponses HTTP | Interface web indisponible (503) |
| `kWsPushMinAllocBytes` | 16 384 | Saut des pushes WebSocket | Temps réel muet |
| `kLowHeapWarnBytes` | 20 000 | WARN heap + flush disque one-shot | Diagnostic seulement |
| `kCheckUpdateMinFreeHeapBytes` | 65 536 | Garde TLS sortante — heap libre | Mise à jour en ligne refusée |
| `kCheckUpdateMinMaxAllocBytes` | 20 480 | Garde TLS sortante — plus gros bloc | Idem |

## Mesures on-target disponibles (firmware 2.20.0 core 3, 5 h d'uptime — **À REFAIRE**)

```
libre8 = 46 480      maxBloc8 = 4 596      minMaxBloc8_boot = 3 060
interne: libre = 74 472   maxBloc = 27 636   ← FIGÉ (reliquat IRAM 32 bits)
```

| heure | libre8 | maxBloc8 | minMaxBloc8_boot | maxBloc interne |
|---|---|---|---|---|
| 20:40 | 47 240 | 9 204 | 3 060 | 27 636 |
| 20:45 | 46 088 | 4 596 | 3 060 | 27 636 |
| 21:20 | 40 324 | 4 084 | 3 060 | 27 636 |
| 21:25 | 49 780 | 4 596 | 3 060 | 27 636 |
| 21:30 | 44 960 | 4 596 | 3 060 | 27 636 |
| 21:40 | 47 776 | 4 596 | 3 060 | 27 636 |

Lecture : **ce n'est pas un épuisement, c'est une fragmentation à 10:1.** `libre8` est stable et sain (40-50 Ko) ; aucun bloc contigu ne dépasse 4,6 Ko.

> ⚠️ **Réserve assumée, à reporter au prochain gate** : ces relevés couvrent **5 h**, pas les **24 h** exigées par le plan d'ADR-0038. L'utilisateur avait explicitement demandé le cycle v2.20.2 malgré cette réserve. Elle **ne peut plus être assumée** pour la bascule des gardes de sécurité.

## Condition d'entrée (les DEUX doivent être remplies)

- [ ] **≥ 24 h de relevés propres sous firmware 2.20.2**, en régime chargé compris (chargement de l'interface, export d'historique, session OTA, plusieurs clients WebSocket). « Propres » = post-correctif du bloc E1 : les échecs auto-infligés du réarmement sont exclus, la trace « Heap 8bits: » est à 30 min. Relever `minBoot` et `minFen`.
- [ ] **Le fragmenteur est identifié** via la capture de `function_name` livrée en v2.20.2 (WARN `Alloc heap échouée x… appelant=…`). Question ouverte associée : qui demande les allocations de **4 096 o** ?

Tant que ces deux points ne sont pas acquis, **ne pas ouvrir ce chantier** : recalibrer sur la fragmentation actuelle reviendrait à entériner un état pathologique comme nominal.

## Travail attendu, une fois la condition remplie

1. **Re-dériver une règle de calibrage** à partir des relevés propres — la formule d'ADR-0038 ne doit pas être réutilisée sans re-dérivation. Contraintes : aucun seuil ne rend le **mode maintenance** atteignable en régime normal ; débounce du redémarrage maintenu à **3**.
2. **Rétablir l'homogénéité de l'invariant d'ordre** `restart < rearmOom ≤ wsPush < lowHeapWarn < min(maxBloc8 sur 24 h)` — il est aujourd'hui sans signification (membres non homogènes).
3. **Basculer les gardes dans l'ordre du RISQUE CROISSANT** : pushes WS et gardes HTTP/TLS d'abord, **auto-guérison en DERNIER**. Chaque étape est un jalon vérifiable on-target.
4. Réévaluer les paramètres du verdict post-mortem (`kOomCorruptionMaxFails`, `kOomCorruptionBurstSpanS`, `kOomCorruptionSaneBlockRatio`) avec les relevés propres.
5. **Décider du sort de `kOomRearmMinMaxAllocBytes` = 24 576** : laissé délibérément inatteignable en v2.20.2. Si la fragmentation est traitée, il redevient pertinent tel quel ; sinon, il doit être recalibré ou documenté comme définitivement inerte.

## Piste amont — traiter la fragmentation plutôt que la subir

À instruire **avant** ou **en parallèle** du recalibrage : deux gisements ont été relevés par lecture de code, **aucun démontré** — les ~300 `String` persistantes du logger (`kMaxLogEntries = 200` + `_persistBuffer` de 100) et les `new char[len+1]` par trame WebSocket. Autres pistes ouvertes depuis ADR-0038 : `AsyncStaticWebHandler`, `LittleFS.begin(false)` limité à 10 fichiers ouverts, fuite éventuelle de `File`.

⚠️ **Ne PAS toucher** à `kMaxLogEntries` ni à la structure `LogEntry` tant que la capture de `function_name` n'a pas désigné le coupable — c'était déjà une consigne de la spec d'origine.

## Contraintes non négociables (reconduites)

- Aucune journalisation, aucune allocation, aucun `String` depuis `oomFailedAllocHook` ni depuis le `new_handler` (verrou du tas détenu → mesurer = deadlock). Documenté INTERDIT dans `src/heap_metrics.h`.
- Le report d'un redémarrage pendant une injection reste **REFUSÉ** (PWM matériel : une boucle figée laisserait la pompe énergisée — différer transformerait une dose tronquée, bénigne, en surdosage).
- Le mode maintenance est un cul-de-sac connu (sortie seulement par crash ou reboot manuel) : **tout seuil qui le rend atteignable en régime normal est inacceptable**.
- La ré-inscription du `new_handler` et son invariant d'ordre (miroir **avant** inscription, indépendance vis-à-vis du miroir) sont **contractuels** — voir la note additive d'ADR-0035.

## Critères d'acceptation

- [ ] ≥ 24 h de relevés propres consignés dans cette spec (tableau `minBoot` / `minFen` / régime chargé)
- [ ] Le fragmenteur est nommé, avec la trace `appelant=` qui l'établit
- [ ] Une règle de calibrage **re-dérivée** est écrite et validée par `pool-chemistry` **avant** tout codage
- [ ] Les 6 constantes lisent la métrique 8 bits, avec l'invariant d'ordre rétabli et homogène
- [ ] `pool-chemistry` GO explicite : aucun seuil ne rend le mode maintenance atteignable en régime normal
- [ ] Vérification on-target après chaque jalon de bascule (WS, puis HTTP/TLS, puis auto-guérison)
- [ ] `pio run` bi-core verts, `pio test -e native` sans régression
- [ ] `docs/subsystems/oom-guard.md`, `docs/subsystems/system-health.md` et ADR-0040 mis à jour (le régime bi-métrique prend fin → nouvel ADR ou note additive datée)

---

# ✅ CLÔTURE — 2026-11-22 : **le chantier a été livré par un AUTRE lot, il y a onze versions**

⛔ **Aucun code n'a été écrit pour cette fiche, et il n'y en avait pas à écrire.** Instruction
demandée par l'utilisateur ; première action : **mesurer**, ⛔ pas coder.

## 1. Ce que la fiche affirme, et qui est FAUX depuis la v2.33.0
> *« 5 gardes, 6 constantes **restées sur la métrique BRUTE** »*

🔴 **Les six lisent la métrique 8 bits.** Vérifié ligne à ligne le 2026-11-22 (v3.0.0) :
| Constante | Site d'appel | Métrique réellement lue |
|---|---|---|
| `kRestartMaxAllocHeapBytes` · `kLowHeapWarnBytes` | `src/main.cpp:1541`, `:1555` | `heapLargestFreeBlock8Bit()` (`:1314`) |
| `kWsPushMinAllocBytes` | `src/ws_manager.cpp:263` | `heapLargestFreeBlock8Bit()` |
| `kHttpHeapMarginBytes` | `src/web_routes_heap.cpp:280`, `:398` | `heapLargestFreeBlock8Bit()` |
| `kCheckUpdateMinFreeHeapBytes` · `kCheckUpdateMinMaxAllocBytes` | `src/web_routes_ota.cpp:1482` | `heapFree8Bit()` (`:1480`) |

**Le lot responsable est identifié** : commit **`c0b33f0`** — *« fix: les gardes mémoire lisent
enfin la vraie mémoire (**v2.33.0, bug-080 lot A**) »*, livré le **2026-09-02**.
⇒ `specs/features/done/bug-080-redemarrage-panic-et-fragmentation-heap.md` le consigne.

## 2. Les valeurs ont été recalibrées, elles aussi
| Constante | Fiche (v2.20.x, août) | **Aujourd'hui (v3.0.0)** |
|---|---|---|
| `kRestartMaxAllocHeapBytes` | 8 192 | **4 096** |
| `kHttpHeapMarginBytes` | 4 096 | 4 096 *(inchangée)* |
| `kWsPushMinAllocBytes` | 16 384 | **6 144** |
| `kLowHeapWarnBytes` | 20 000 | **12 288** |
| `kCheckUpdateMinFreeHeapBytes` | 65 536 | **32 768** |
| `kCheckUpdateMinMaxAllocBytes` | 20 480 | **24 576** |

## 3. 🔴 Et la FRAGMENTATION PATHOLOGIQUE, qui justifiait le NO GO, A DISPARU
Relevé sur l'appareil le **2026-11-22**, v3.0.0, 34 min d'uptime :
```
libre8 = 50 324    maxBloc8 = 28 660    minBoot = 25 588    minFen = 25 588
```
| Grandeur | Fiche (15/08, v2.20.0) | **2026-11-22 (v3.0.0)** | Facteur |
|---|---|---|---|
| `maxBloc8` | 4 596 | **28 660** | **× 6,2** |
| `minMaxBloc8_boot` | 3 060 | **25 588** | **× 8,4** |
| Ratio `libre8 / maxBloc8` | **10 : 1** (pathologique) | **1,75 : 1** | — |

⇒ ⭐ **Les quatre motifs du NO GO de `pool-chemistry` reposaient tous sur cet état.** Aucun ne
tient : avec `minBoot = 25 588`, le seuil de redémarrage (4 096) est **six fois plus bas** que le
minimum observé — ⛔ aucune boucle de redémarrage possible, donc ⛔ aucun chemin vers le mode
maintenance par cette voie.
⚠️ Cohérent avec le repère du projet : *maxBloc8 ~25 Ko = **NOMINAL**, pas une alerte.*
ⓘ L'observabilité livrée depuis fonctionne : le journal porte
`Chute maxBloc8: 51188 -> 28660 (-22528)` et une trace `Heap 8bits:` toutes les 30 min.

## 4. ⚠️ LE SEUL POINT QUI RESTE — et il est d'aujourd'hui, pas d'août
`kCheckUpdateMinMaxAllocBytes = **24 576**` contre un `minBoot` mesuré à **25 588**.
⇒ **1 012 octets de marge.** Si le plus gros bloc descend d'un kilooctet de plus, la **mise à jour
en ligne est refusée** — ⛔ silencieusement, du point de vue de l'utilisateur.
⚠️ **Un seul démarrage de 34 min ne suffit pas à en juger** : c'est exactement la campagne de 24 h
que cette fiche exigeait, et elle n'a jamais été faite.

### ⏱️ CAMPAGNE OUVERTE le 2026-11-22 — à la demande de l'utilisateur
**Surveillance installée**, cadence **30 min**, ⛔ lecture seule (l'appareil dose pendant la mesure) :
- script : `/root/pool-captures/f210-marge-ota/mesure-marge-ota.sh`
- relevés : `/root/pool-captures/f210-marge-ota/mesures.csv`
- alertes : `/root/pool-captures/f210-marge-ota/alertes.log`
- cron : `*/30 * * * *` — ⚠️ **marqué TEMPORAIRE, à retirer une fois la décision prise**

**Ce qu'elle relève** (`hf.i8`) : plus gros bloc courant, **plancher depuis le boot** (`mb`, *le*
chiffre qui décide), plancher de la fenêtre de 30 min (`wb`, qui se réarme et capte les creux),
heap libre et son plancher, contexte au creux.
**Deux seuils d'alerte** : plancher **sous** `kCheckUpdateMinMaxAllocBytes` (24 576) ou sous
`kCheckUpdateMinFreeHeapBytes` (32 768) ⇒ *« la mise à jour en ligne serait REFUSÉE »* ; marge
**< 2 Ko** ⇒ *« marge faible »*.

⚠️ **Cadence choisie, ⛔ pas prise au hasard** : 30 min s'aligne sur le réarmement des planchers de
fenêtre (`heapFloorsRearmWindow`) et reste **très au-dessus** du seuil d'effet d'observateur —
⭐ *chaque lecture de `/get-system-info` est elle-même un GET qui traverse le filtre de la garde et
pousse un échantillon* (condition X4 de feature-132 : jamais sous 60 s).

**Premier relevé, 2026-11-22T09:09Z** (uptime 2 431 s) :
`bloc courant 28 660 · plancher boot 25 588 · libre 53 136 · plancher libre 47 476` ⇒ **marge
1 012 o**, l'alerte « marge faible » s'est déclenchée **dès le premier passage**.

⇒ **Décision reportée à ≥ 24 h de relevés.** ⛔ Pas tranchée ici.

## 5. ⭐ La leçon, et elle vaut au-delà de cette fiche
**Une fiche BLOQUÉE que personne ne relit devient un DOUBLON.** Celle-ci décrivait un chantier
« critique différé » pendant **trois mois** alors qu'un autre lot l'avait livré **dix-sept jours
après sa création**. ⇒ Ses conditions d'entrée (24 h de relevés, fragmenteur identifié) n'ont
**jamais** été remplies — et n'avaient plus à l'être.
⚠️ C'est la raison d'être de la consigne du projet : *étudier chaque fiche **avant** de la traiter,
parce que l'application a évolué.* Ici, elle a évité d'ouvrir un chantier **entièrement vide**.

**Statut : ✅ CLOSE — sans objet, travail livré par bug-080 lot A (v2.33.0).**
