# Bug — bascule des 5 gardes restantes sur la métrique 8 bits + recalibrage des seuils

- **Type** : bug critique différé (mémoire / disponibilité / **sécurité chimique**)
- **Statut** : todo — **BLOQUÉE** par une condition d'entrée (voir ci-dessous)
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
