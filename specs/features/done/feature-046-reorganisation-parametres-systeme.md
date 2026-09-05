## Métadonnées
- **ID** : feature-046
- **Slug** : reorganisation-parametres-systeme
- **Type** : feature (réorganisation UI, frontend uniquement)
- **Statut** : done
- **Priorité** : basse
- **Version cible** : 2.5.1
- **Dernière mise à jour** : 2026-07-04

## Description
Paramètres : (1) **supprimer** la carte « Version du firmware » de l'onglet **Système** (redondante : « Infos système » affiche déjà Firmware + Build) ; (2) **déplacer** la carte « Infos système » de l'onglet **Avancé** vers le **haut** de l'onglet **Système**.

## Périmètre exact (inventorié)
- `data/index.html` :
  - Supprimer la carte « Version du firmware » (l.1501-1511, `#sys_current_firmware_version`) du `#panel-system`.
  - Déplacer la carte « Infos système » COMPLÈTE (l.1736-1760 : kv 10 lignes `sys_firmware_version`→`sys_mac_address` + bouton `#refresh_info_btn`) depuis `#panel-dev` vers `#panel-system`, en **première position** (juste après l'ouverture de la section).
  - Les cartes « Mise à jour en ligne (GitHub) » et « Mise à jour manuelle » restent dans Système (après Infos système) ; la carte cachée « Écran LVGL » reste dans Avancé.
- `data/app.js` :
  - Retirer la ligne `$("#sys_current_firmware_version")…` (l.3836) de `loadSystemInfo()` (id supprimé → référence morte sinon).
  - ⚠️ Vérifier le déclencheur de `loadSystemInfo()` l.5749 : s'il est conditionné à l'ouverture de l'onglet **Avancé** (lazy-load), l'adapter pour que les infos se chargent aussi/plutôt à l'ouverture de **Système** — sinon la carte déplacée resterait vide. Le bouton Actualiser (l.5311) reste inchangé.
- Aucun changement backend (`/get-system-info` inchangé).

## Critères d'acceptation
- [x] AC1 — Onglet Système : « Infos système » en 1ʳᵉ carte, peuplée (pas de tirets figés) ; plus de carte « Version du firmware ».
- [x] AC2 — Onglet Avancé : plus de carte « Infos système » ; coredump/logs/écran LVGL intacts.
- [x] AC3 — Bouton « Actualiser » fonctionne ; la version firmware reste visible (ligne « Firmware » d'Infos système).
- [x] AC4 — 0 référence morte (`sys_current_firmware_version` absent de app.js/index.html) ; `node --check` OK ; `./build_fs.sh` OK.
- [x] AC5 — Docs : page-settings.md à jour (les 2 onglets), CHANGELOG 2.5.1.
- [x] AC6 — Validation navigateur utilisateur.

## Hors périmètre
- Cartes Mise à jour (en ligne/manuelle), coredump, logs ; tout backend.

---

## Itération 2 (2026-07-04)
**Déplacer** la carte « Historique des mesures (température, pH, ORP) » (l.1590-1627 : export CSV, import CSV, suppression — ids `history_export_btn`, `history_import_file`, `history_import_btn`, `history_clear_btn`) de l'onglet **Système** (`#panel-system`) vers la **tête** de l'onglet **Avancé** (`#panel-dev`, en 1ʳᵉ position). Aucun changement JS attendu (handlers par id, indépendants de l'emplacement — à vérifier).

### Critères d'acceptation (itération 2)
- [x] AC7 — Onglet Avancé : « Historique des mesures » en 1ʳᵉ carte, export/import/suppression fonctionnels ; onglet Système : la carte n'y est plus (Infos système puis Mises à jour, puis Redémarrage…).
- [x] AC8 — HTML équilibré, 0 référence cassée, `node --check` + `./build_fs.sh` OK ; docs (page-settings.md, CHANGELOG 2.5.1) mises à jour.
