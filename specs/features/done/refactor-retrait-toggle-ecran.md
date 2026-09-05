# Refactor — Retrait du toggle « Écran LVGL » et du champ `screen_enabled` (suivi feature-065)

- **Type** : refactor ponctuel (format minimal `specs/modele.md`)
- **Statut** : done (2026-08-08 — revue Approved, builds bi-core + 428 tests + build_fs verts)
- **Version cible** : 2.17.1
- **Créée** : 2026-08-08
- **Domaine** : Web (UI Settings + /get-config//set-config + NVS) — aucun chemin de dosage
- **Spec liée** : feature-065 (le support écran est retiré ; le toggle était conservé inerte)

## Contexte
feature-065 a retiré tout le support écran mais conservé la surface
`screen_enabled` (hors périmètre). Demande utilisateur : la retirer aussi.

## Périmètre exact (inventaire grep 2026-08-08)
- `data/index.html` : carte « Écran LVGL » (déjà en display:none) → supprimée
- `data/app.js` : 3 usages (collectConfig l.1574, peuplement l.1771-1773,
  autosave l.6136) → supprimés
- `src/web_routes_config.cpp` : champ `screen_enabled` de /get-config (l.155)
  et bloc set-config (l.497-500) → supprimés (contrat API : champ retiré)
- `src/config.cpp` : putBool/getBool NVS (l.183, 373) → supprimés — la clé NVS
  `screen_enabled` devient orpheline sur les appareils existants (inoffensif,
  quelques octets ; purge non justifiée)
- `src/config.h` : champ `authCfg.screenEnabled` → supprimé
- `src/version.h` → 2.17.1

## Critères d'acceptation
- [x] `grep -ri "screen_enabled\|screenEnabled" src/ data/` → vide HORS 2 commentaires
  explicatifs volontaires (web_routes_config.cpp : compat client legacy ;
  config.cpp : clé NVS orpheline documentée) — critère amendé en revue
- [x] `pio run` + `esp32dev_core3` verts ; `pio test -e native` 428/428
- [x] `./build_fs.sh` OK (littlefs.bin 320 Ko)
- [x] Aucun autre champ de config touché (vérifié en revue) ; revue Approved
