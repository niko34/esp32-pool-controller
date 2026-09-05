## Métadonnées
- **ID** : feature-044
- **Slug** : repartitionnement-app-1664k
- **Type** : feature (infrastructure — table de partitions, layout v3)
- **Statut** : done
- **Priorité** : normale
- **Version cible** : 2.4.0 (changement de procédure de déploiement — flash USB obligatoire une fois)
- **Dernière mise à jour** : 2026-07-04
- **Spec(s) liée(s)** : feature-043 (migration uPlot, −148 KB FS — prérequis accompli), ADR-0015 (layout v2, supersédé), ADR-0018

## Domaines touchés
- [x] Build / partitions (partitions.csv, platformio.ini, build_fs.sh, deploy.sh)
- [ ] Régulation / Capteurs / Web / Frontend / Tests natifs — AUCUN code applicatif modifié

## Agents requis
- [x] spec-reader — intégré (spec écrite d'après analyse directe, arithmétique vérifiée)
- [ ] architect — le design EST la table ci-dessous (arithmétique vérifiée par script) ; la revue re-vérifie
- [ ] pool-chemistry — NON applicable
- [x] embedded-developer (application des 4 fichiers de config)
- [x] code-reviewer — **doit re-vérifier l'arithmétique indépendamment** (risque de brick logiciel)
- [x] doc-writer (ADR-0019 succédant à 0015, BUILD.md, UPDATE_GUIDE.md, CHANGELOG)

## Description
Passer au **layout v3** : agrandir `app0`/`app1` de 1536 → **1664 KB** (+128 KB chacune) en réduisant `spiffs` de 832 → **576 KB**, possible depuis la migration uPlot (payload FS 449 KB). Objectif : firmware de 90,8 % → **~84 %** d'occupation (marge ~277 KB).

## Table de partitions — layout v3 (arithmétique vérifiée)
| Partition | Offset | Taille | Δ vs v2 |
|---|---|---|---|
| nvs | 0x9000 | 0x5000 | inchangé (config préservée) |
| otadata | 0xE000 | 0x2000 | inchangé |
| app0 | 0x10000 | **0x1A0000** (1664 KB) | +128 KB |
| app1 | **0x1B0000** | **0x1A0000** | +128 KB, offset décalé |
| spiffs | **0x350000** | **0x90000** (576 KB) | −256 KB, offset décalé |
| history | 0x3E0000 | 0x10000 | **inchangé — historique PRÉSERVÉ** |
| coredump | 0x3F0000 | 0x10000 | inchangé |

Vérifications : app1 offset 0x1B0000 aligné 64 KB ✓ ; spiffs fin = 0x3E0000 = début history ✓ ; total = 0x400000 (4 MB) ✓ ; firmware actuel 1 427 597 o < 1 703 936 o ✓ ; payload FS 449 177 o → ~82 % de 576 KB ✓.

## Fichiers à modifier (les 4 références en dur, inventaire exhaustif par grep)
1. **partitions.csv** — nouvelles valeurs + commentaire layout v3 (ADR-0019).
2. **platformio.ini** l.21-22 — `board_build.filesystem_size = 589824` (0x90000) + commentaire.
3. **build_fs.sh** l.3-4/12/15 — `-s 589824` + commentaires (retirer aussi la mention obsolète 1088KB).
4. **deploy.sh** l.10 — `LITTLEFS_OFFSET="0x350000"` ⚠️ CRITIQUE (sinon le FS serait écrit au milieu d'app1) + l.60 commentaire obsolète « 1088KB ».
Aucun changement `src/` (history.cpp trouve sa partition par label à l'exécution) ni `ota_update.sh` (l'OTA FS passe par Update/U_SPIFFS, partition-aware). `src/version.h` → 2.4.0.

## Procédure de migration (à documenter — UPDATE_GUIDE)
- **Flash USB obligatoire UNE FOIS** : `./deploy.sh all` (pio upload réécrit bootloader + table + boot_app0 + firmware ; puis FS au nouvel offset). **L'OTA ne peut PAS appliquer un changement de table.**
- **Préservés** : config NVS (0x9000 intact), historique (0x3E0000 intact), coredump.
- **Perdus/neutres** : contenu spiffs (re-flashé), ancienne app1 (résidus inertes, réécrits au premier OTA).
- Après migration : OTA firmware/FS fonctionnent normalement (nouvelles bornes).
- ⚠️ NE PAS utiliser `./deploy.sh factory` (efface la NVS → perte config + mot de passe AP régénéré) sauf si problème.

## Critères d'acceptation
- [x] AC1 — `pio run` SUCCESS ; l'affichage Flash utilise la nouvelle taille (1 703 936 o) → ~83,8 % (mesuré : 1 427 753 o).
- [x] AC2 — `./build_fs.sh` produit un `littlefs.bin` de **589 824 o exactement**, payload inclus (~82 %) — payload 449 177 o.
- [x] AC3 — Cohérence totale : partitions.csv, platformio.ini, build_fs.sh, deploy.sh alignés (aucune autre référence en dur oubliée — grep ; inclut le fix du check de taille deploy.sh resté au layout v1 : 1 114 112 → 589 824).
- [x] AC4 — Revue : arithmétique re-vérifiée indépendamment (offsets, alignements, chevauchements, 4 MB).
- [x] AC5 — Docs : ADR-0019 (supersède 0015), BUILD.md, UPDATE_GUIDE.md (procédure USB), CHANGELOG 2.4.0.
- [x] AC6 — Validation on-target par l'utilisateur : flash USB `./deploy.sh all`, boot OK, config conservée, historique conservé, UI OK, puis un OTA de contrôle.

## Hors périmètre
- Toute modification du code applicatif ; changement de la taille history/coredump ; OTA de la table (impossible).

---

## Suivi / clôture (2026-07-04)
- Implémentation + revue Approved (arithmétique re-vérifiée indépendamment) + docs (ADR-0019, 0015 supersédé, BUILD, UPDATE_GUIDE, CHANGELOG, ota-manager.md corrigé du layout v1 résiduel).
- Bonus : fix du check de taille deploy.sh cassé depuis le layout v2 (1114112→589824) ; history/coredump offsets inchangés → historique préservé.
- **AC6 validé par l utilisateur** : flash USB OK, boot OK, FS lu 472/576 KB (82 %, layout v3 actif), config et historique conservés. Firmware 83,8 % (marge ~270 KB). Version 2.4.0.
