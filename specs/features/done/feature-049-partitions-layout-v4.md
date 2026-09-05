# Feature-049 — Table de partitions layout v4 : +128 KB par slot app

- **Type** : feature (infrastructure, format minimal — précédent direct : feature-044 / layout v3)
- **Statut** : done — reste on-target : flash USB complet (`./deploy.sh all`) + vérifications post-flash
- **Version cible** : 2.13.0
- **Créée** : 2026-07-06 (suite feature-048 : le payload FS gzippé à 155 KB libère la place)
- **Domaines** : partitions.csv, platformio.ini, build_fs.sh, deploy.sh, docs (ADR successeur de 0019) — aucun code métier

## Motivation

Le gzip (feature-048) a fait tomber le payload FS de 443 à 155 KB : la partition `spiffs` de 576 KB (layout v3) est surdimensionnée. Reprendre 256 KB pour les slots applicatifs porte la marge firmware de 264 à 392 KB par slot (1440/1792 = 80,4 %).

## Layout v4 (cible)

| Partition | Offset | Taille | vs v3 |
|---|---|---|---|
| nvs | 0x9000 | 0x5000 | = |
| otadata | 0xE000 | 0x2000 | = |
| app0 | 0x10000 | **0x1C0000** (1792 KB) | +128 KB |
| app1 | **0x1D0000** | **0x1C0000** (1792 KB) | +128 KB |
| spiffs | **0x390000** | **0x50000** (320 KB) | −256 KB |
| history | 0x3E0000 | 0x10000 | = (données préservées) |
| coredump | 0x3F0000 | 0x10000 | = |

Somme = 0x400000 (4 MB) ; offsets app alignés 64 KB ; spiffs à 48 % avec le payload actuel (marge 165 KB).

## Modifications

1. `partitions.csv` : table v4 + commentaire d'en-tête mis à jour (ADR référencé, avertissement flash USB).
2. `platformio.ini` : `board_build.filesystem_size = 327680`.
3. `build_fs.sh` : `-s 327680` + message.
4. `deploy.sh` : `LITTLEFS_OFFSET=0x390000`, contrôle de taille 327680.
5. `CLAUDE.md` : ligne Filesystem (spiffs 320 KB = 0x50000, layout v4).
6. Docs : nouvel ADR `0024-partitions-layout-v4.md` (successeur 0019), `UPDATE_GUIDE.md` (migration v4 : **flash USB obligatoire**, ne PAS pousser le FS v4 en OTA sur une table v3), `BUILD.md`, `CHANGELOG.md`.

## Contraintes / risques

- **Flash USB obligatoire une fois** (bootloader + table) — l'OTA ne peut pas appliquer un changement de table. Séquence sûre : `./deploy.sh all` par USB.
- **Piège de transition** : une image FS construite à 320 KB ne doit PAS être flashée en OTA sur un appareil encore en table v3 (montage LittleFS avec métadonnées discordantes). L'UPDATE_GUIDE doit l'interdire explicitement ; la release qui embarque le v4 se flashe par USB.
- `history`/`coredump` inchangés (mêmes offsets) : données préservées, la détection de redimensionnement de HistoryManager ne se déclenche pas.
- L'UI (Infos système, feature-047) lit tailles/occupation à l'exécution via les API — aucun changement de code nécessaire.

## Critères d'acceptation

- [x] Somme des partitions = 4 MB exactement, offsets app alignés 0x10000, `pio run` SUCCESS (le linker valide la taille du slot).
- [x] `./build_fs.sh` produit une image de 327680 octets exactement.
- [x] Le quatuor partitions.csv / platformio.ini / build_fs.sh / deploy.sh est cohérent (mêmes valeurs).
- [x] ADR-0024 créé, UPDATE_GUIDE avec procédure de migration v4 et interdiction du FS v4 en OTA sur table v3.
- [ ] On-target (utilisateur) : flash USB complet, boot OK, FS monté (Infos système : ~320 KB), historique préservé, OTA suivant fonctionnel.

## Notes d'implémentation (2026-07-06, v2.13.0)

- **Livré** : layout v4 conforme au tableau cible — app0/app1 0x1C0000 (1792 KB, occupation firmware ≈78,5 %, marge 264 → ~392 KB par slot), spiffs 0x390000/0x50000 (320 KB, payload gzippé 155 KB = 48 %), history/coredump inchangés (offsets identiques, données préservées).
- **Revue** : code-reviewer **Approved** — calculs de la table refaits indépendamment (alignements, jointures d'offsets, somme = 0x400000). Quatuor partitions.csv / platformio.ini / build_fs.sh / deploy.sh cohérent (327680) ; `deploy.sh` reconstruit automatiquement une image FS de mauvaise taille. `CLAUDE.md` (ligne Filesystem) à jour.
- **Remarque mineure du reviewer** : le `build_src_filter` `ota_integrity_logic` présent dans `platformio.ini` relève de la feature-026, pas de cette feature — simple note de commit, aucune action.
- **Docs** : ADR-0024 créé (ADR-0019 → Superseded), `UPDATE_GUIDE.md` (migration v3 → v4, interdiction OTA v4-sur-v3), `BUILD.md`, `CHANGELOG.md` [2.13.0].
- **Reste on-target** (dernier critère non coché) : flash USB complet `./deploy.sh all`, puis vérifications post-flash (boot OK, Infos système FS ~320 KB, historique présent, OTA suivant fonctionnel).
