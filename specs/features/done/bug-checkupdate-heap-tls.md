# Bug — `/check-update` échoue sur heap fatigué : « Erreur HTTP GitHub: -1 » puis « JSON parse error »

- **Type** : bug (format minimal `specs/modele.md`)
- **Statut** : done (2026-08-04 — livré en v2.16.1, revue Approved, builds core 2 + core 3 verts, 428/428 tests natifs)
- **Version cible** : 2.16.1
- **Créée** : 2026-08-04 (constaté sur appareil 2.14.0 après publication de la release v2.16.0)
- **Domaine** : Web (route `/check-update`, `src/web_routes_ota.cpp`) — aucun chemin de dosage touché
- **Spec liée** : ADR-0032 (réponses HTTP bornées — ce handler est le dernier survivant de la famille), feature-026 (empreintes d'intégrité)

## Symptôme observé

Sur appareil en fonctionnement prolongé (2.14.0, heap fragmenté) : `Erreur HTTP GitHub: -1`
systématique (3/3, échec < 1 s) — l'allocation du contexte TLS (~48 Ko) échoue avant
toute communication. Après reboot (heap frais mais boot en cours) : tentative 1 =
réponse mal formée côté navigateur, tentative 2 = `Erreur parsing JSON GitHub`,
tentative 3 = succès. Certificats hors de cause (chaîne Sectigo→USERTrust ECC validée
contre les CA épinglées du firmware ; GitHub répond 200 depuis la même IP publique).

## Cause racine

`handleCheckUpdate()` (`src/web_routes_ota.cpp:285`) viole le patron ADR-0032 :
`https.getString()` charge la réponse GitHub ENTIÈRE en RAM (10 236 octets mesurés
pour la release v2.16.0, croît avec chaque release — corps de release jusqu'à
125 000 caractères côté GitHub), puis `deserializeJson(doc, payload)` construit le
document COMPLET (~2-3× l'entrée). Pic transitoire : TLS ~48 Ko + String 10 Ko +
JsonDocument ~25 Ko ≈ 85-95 Ko, avec exigence de gros blocs contigus → échec sur
heap fragmenté. Aggravant : la troncature des notes (`kOtaReleaseNotesMaxChars`,
`substring(0, 2048)`) peut couper un caractère UTF-8 multi-octets en plein milieu
(notes en français : accents, emojis) → JSON invalide côté navigateur (cohérent avec
la tentative 1 post-reboot).

## Correctif attendu (patron ADR-0032 : parsing en flux borné)

1. **Parsing en flux avec filtre** : `https.useHTTP10(true)` (pas de chunked —
   patron officiel ArduinoJson) + `deserializeJson(doc, https.getStream(),
   DeserializationOption::Filter(filter))` où `filter` ne retient QUE les champs
   consommés : `tag_name`, `name`, `published_at`, `body`, `assets[].name`,
   `assets[].browser_download_url`, `assets[].digest`. Plus jamais les 10 Ko en
   String ni le document complet.
2. **Budget mémoire dur** : allocateur ArduinoJson personnalisé plafonné
   (`kOtaCheckJsonBudgetBytes`, constants.h) — un corps de release anormalement
   gros produit `NoMemory` → 500 propre (« réponse GitHub trop volumineuse »)
   au lieu d'une consommation non bornée. Logique de plafond en header pur
   testable en natif (ADR-0017).
3. **Troncature UTF-8 sûre** : couper les notes à `kOtaReleaseNotesMaxChars` sur
   une frontière de caractère UTF-8 (fonction pure testable en natif).
4. Chemins d'erreur log distincts : budget dépassé vs JSON invalide.

## Critères d'acceptation

- [x] `/check-update` ne stocke plus jamais la réponse GitHub entière en RAM
      (plus de `getString()` sur ce chemin) ; pic ≈ TLS + ≤ budget JSON + réponse
- [x] Corps de release > budget → 500 JSON propre + log explicite, pas de crash
- [x] Notes tronquées = UTF-8 valide quel que soit le point de coupe
- [x] Réponse JSON de la route INCHANGÉE vu du frontend (mêmes champs)
- [x] `pio run` + `pio run -e esp32dev_core3` sans warning ; tests natifs verts
      (+ nouvelle suite pour le plafond d'allocation et la coupe UTF-8)

## Hors périmètre

- Le coût TLS lui-même (~48 Ko, incompressible avec WiFiClientSecure)
- `/download-update` (déjà en flux, vérification SHA-256 fail-closed)
- UI (aucun changement de contrat)
