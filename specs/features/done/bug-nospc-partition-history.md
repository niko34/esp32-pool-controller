# Bug — Saturation de la partition history (NOSPC) → div/0 littlefs récurrent

- **Type** : bug + durcissement
- **Statut** : done
- **Version cible** : 2.11.0
- **Créée** : 2026-07-27
- **Domaine** : persistance (history + logger + disjoncteur feature-059) — aucun chemin de dosage
- **Specs liées** : investigation-corruption-recurrente-history (done — mécanisme identifié), feature-059 (disjoncteur, confinement)

## Cause racine (investigation du 2026-07-27)

Pire cas d'occupation de la partition 64 Ko (16 blocs) : réécriture copy-on-write de `/history.json` (17-22 Ko, toutes les 5 min, ancien + nouveau coexistent = 10-12 blocs) + `/system.log` ≤ 16 Ko (4-5 blocs) + `.tmp` de rotation 12 Ko (3 blocs) + 2 blocs de métadonnées = **19-22 blocs demandés > 16 disponibles**. NOSPC structurel après ~5-7 jours d'accumulation. Le div/0 dans `lfs_alloc` sur FS plein est un bug littlefs amont répertorié (issue #1063), version embarquée figée par la plateforme (espressif32@6.9.0 / core 2.0.17). Chemins d'erreur aveugles : `saveToFile` ne détecte pas l'écriture partielle, son message d'erreur déclenche un flush immédiat sur le FS plein toutes les 5 min. Hypothèse résiduelle H2 (corruption RAM `lfs_t` persistée, CRC valides) à discriminer à la prochaine occurrence.

## Livrables

- [ ] A1 — **Préservation de preuve dans le disjoncteur** (`history.cpp`, avant `esp_partition_erase_range`) : dump hex des blocs 0-1 de la partition (8 Ko) sur Serial + empreinte courte (CRC/first-bytes) en NVS `hist_meta`. Discriminant H1 (chaîne métadonnées pleine) vs H2 (superblock CRC-valide insensé) vs H3 (paire déchirée).
- [ ] A2 — **Garde d'espace dans `saveToFile`** : avant écriture, `remove()` de l'ancien `/history.json` (divise le pic COW par 2) puis vérification d'espace libre (`totalBytes - usedBytes`) contre la taille mesurée du JSON + marge ; espace insuffisant → save sauté + log throttlé + compteur NVS. Trade-off à acter (ADR) : un crash pendant la fenêtre remove→write perd l'historique — borné par le disjoncteur et la fréquence 5 min.
- [ ] A3 — **Détection d'écriture partielle** : comparer les octets écrits (`serializeJson` retour) à la taille mesurée (`measureJson`) + vérifier l'état d'erreur du File ; échec → remove du fichier partiel + log + compteur.
- [ ] A4 — **Réduction d'empreinte log** : `kMaxLogFileBytes` 16384 → 8192, `kRotateKeepBytes` 12288 → 6144 (pire cas log : 4-5 blocs → 2-3 blocs).
- [ ] A5 — **Instrumentation** : log périodique throttlé de l'occupation FS (`usedBytes/totalBytes`, 1/h max ou sur franchissement de seuil), epoch du dernier save réussi + compteur NOSPC en NVS.
- [ ] A6 — Anti-flush-storm : le chemin d'échec de `saveToFile` ne doit pas déclencher de flush disque immédiat (log en WARNING, pas ERROR, ou flush conditionné).
- [ ] D1 — Logique pure testable si l'architect en identifie (ex. décision de save selon espace/taille/marge) — pattern ADR-0017.
- [ ] D2 — `pio run` 0 warning nouveau, suites natives vertes.
- [ ] C1 — ADR-0033 (budget d'espace partition history : garde + remove-avant-write, trade-off perte ponctuelle vs saturation destructrice ; bug amont documenté).
- [ ] C2 — `docs/subsystems/system-health.md` (préservation de preuve dans le disjoncteur), doc history/logger, `CHANGELOG.md` [2.11.0].

## Critères d'acceptation

- [ ] Le pire cas d'occupation simultanée ne peut structurellement plus dépasser la capacité (budget documenté dans l'ADR avec les nouvelles constantes).
- [ ] FS plein malgré tout → saves sautés proprement avec trace, jamais d'écriture partielle silencieuse, jamais de flush-storm.
- [ ] Prochaine occurrence du div/0 (si H2) → dump de preuve disponible sur Serial + empreinte NVS avant effacement.
- [ ] Aucun chemin de dosage touché ; `pio run` + tests verts.

## Notes d'implémentation (v2.11.0, 2026-07-27)

- **Cycle** : investigation (spec dédiée en done/) → architect → embedded-developer → code-reviewer (**Approved**, 2 mineures non bloquantes assumées : linkage externe des helpers — pattern du fichier ; race bénigne préexistante importData/loopTask sur les statics) → doc-writer (interrompu 2× par erreurs API, terminé par l'agent principal pour l'entrée CHANGELOG).
- **Livrables** : A1 dump de preuve `[HISTDUMP]` (8 Ko hex Serial + CRC32×2 + head 16 o en NVS, lecture seule, WDT reset ×4) inséré avant l'erase du disjoncteur ; A2/A3 `saveToFile` restructuré (mesure → `historySaveAdmit` (module pur history_logic, anti-overflow par soustractions gardées) → remove → write → vérif partielle avec suppression du partiel) — l'ancien code tronquait l'historique dès l'open("w"), avant toute vérification ; A4 plafonds log 8192/6144 (logger.h) ; A5 instrumentation NVS `nospc_cnt`/`last_ok` (écrite sur échec uniquement) + INFO occupation throttlé 1 h ; A6 chemins d'échec en WARNING (jamais de flush immédiat).
- **Budget acté (ADR-0033)** : pire cas 22 blocs > 16 → **13 blocs ≤ 16, marge 3 blocs**.
- **Tests** : +10 cas `historySaveAdmit` dans test_native_history (frontières exactes ±1, SIZE_MAX des deux côtés) — **363/363 verts**, `pio run` 0 warning nouveau.
- **Reste on-target** (après déploiement) : vérifier au fil des jours l'INFO d'occupation FS ; à ~J+7 (ancien seuil fatal), confirmer l'absence de récidive ; si `[HISTDUMP]` apparaît un jour → hypothèse H2, analyser le dump.

## Critères d'acceptation — validation

- [x] Pire cas structurellement ≤ capacité (13/16 blocs, ADR-0033).
- [x] FS plein → saves sautés proprement, jamais d'écriture partielle silencieuse, jamais de flush-storm.
- [x] Prochaine occurrence → preuve préservée (Serial + NVS) avant effacement.
- [x] Aucun chemin de dosage ; pio run + 363/363.
