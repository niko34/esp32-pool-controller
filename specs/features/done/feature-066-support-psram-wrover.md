## Métadonnées
- **ID** : feature-066
- **Slug** : support-psram-wrover
- **Type** : feature
- **Statut** : done côté CODE (2026-08-08 — lancée avant réception sur décision
  utilisateur ; revue Approved, builds bi-core + 428 tests + FS verts).
  **Validation WROVER en attente du module** (critère 2 — procédure de swap
  ci-dessous à dérouler à réception).
- **Priorité** : haute (résout bug-checkupdate-tls-core3, restaure le TLS sortant sous core 3)
- **Version cible** : 2.18.0
- **Dernière mise à jour** : 2026-08-08
- **Spec(s) liée(s)** : bug-checkupdate-tls-core3, feature-065 (GPIO 16/17 libérés), ADR-0034

## Domaines touchés
- [ ] Capteurs / Régulation / Filtration / MQTT / UART / OTA / Auth / Historique
- [x] Build (platformio.ini) + main.cpp (log boot) + Infos système (web_routes_config + UI)

## Agents requis
- [x] spec-reader, [x] architect, [x] embedded-developer, [x] web-ui-developer (Infos système),
- [x] test-engineer, [x] code-reviewer, [x] doc-writer
- [ ] pool-chemistry — à confirmer par spec-reader : aucun chemin de dosage modifié,
  mais le swap matériel justifie un verre doseur de contrôle dans le plan de test manuel

## Description
Activer l'exploitation de la PSRAM du module WROVER-E : flag de build, visibilité
(log de boot + Infos système), et procédure de swap documentée. Même binaire
compatible WROOM (sans PSRAM, dégradation gracieuse) et WROVER (avec).

## Comportement attendu
- Build : `-DBOARD_HAS_PSRAM` sur les DEUX envs (esp32dev, esp32dev_core3).
  Vérifier la dégradation gracieuse sur module SANS PSRAM (l'init échoue
  proprement, le firmware continue — à valider sur les deux cores).
- Boot : log INFO « PSRAM : X Mo détectés » ou « PSRAM absente » (systemLogger).
- Infos système : taille/libre PSRAM (`ESP.getPsramSize()/getFreePsram()`),
  affiché « — » si absente.
- Aucune autre logique modifiée : l'allocation en PSRAM des blocs > 4 Ko est
  automatique (SPIRAM_USE_MALLOC précompilé, vérifié le 2026-08-08).

## Procédure de swap (plan de test manuel — à réception du module)
1. Sur ÉTABLI (jamais sur le câblage piscine) : flash USB complet du module
   neuf en core 2 (`./deploy.sh all`) — bootloader IDF 4.4, mêmes propriétés
   de retour arrière que la production. NVS vierge → noter AVANT le swap toute
   la config de production (WiFi, MQTT, consignes pH/ORP 580, limites,
   calibrations pompes/sondes, mode d'installation).
2. OTA core 3 v2.18.0 sur l'établi ; vérifier : log PSRAM détectée, Infos
   système, **« Vérifier les mises à jour » fonctionne** (test décisif du
   levier), heap/maxAlloc dans le log de check-update.
3. Swap physique sur le PCB (mêmes 38 broches), re-saisie de la config,
   vérifications capteurs (I2C EZO, OneWire, RTC), verre doseur de contrôle
   (mêmes GPIO 25/33, même puce — contrôle rapide, pas une revalidation).
4. Conserver l'ancien module WROOM flashé tel quel = retour arrière matériel.

## Critères d'acceptation
- [x] Binaire unique CÔTÉ CODE : dégradation gracieuse vérifiée en revue dans
  les sources du core (psramInit avant setup, échec propre, getPsramSize()=0,
  getFreeHeap()=RAM interne seule → gardes heap WS non diluées) ; à confirmer
  au boot réel sur le WROOM de production
- [ ] Sur WROVER core 3 : check-update GitHub fonctionne (bug-checkupdate-tls-core3 clos)
- [x] Infos système affiche la PSRAM (« absente » sur WROOM) ; builds bi-core
  + 428 tests verts. Coût flash des -mfix (requis, libs précompilées avec
  cache workaround MEMW des DEUX côtés) : core 2 +39,8 Ko (82,9 %),
  core 3 → 1 693 776 o (92,3 % — le gain feature-065 est reconsommé, marge
  ~138 Ko à surveiller)

## Hors périmètre
- Placement explicite d'allocations applicatives en PSRAM (heap_caps) — l'automatique suffit
- Retrait des défenses heap existantes (elles restent, coût nul)
