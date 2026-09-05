## Métadonnées
- **ID** : feature-008
- **Slug** : journal-par-injection
- **Type** : feature
- **Statut** : **clôturée — écartée** (2026-07-04, non implémentée, rapport valeur/coût défavorable)
- **Priorité** : normale (à la rédaction — caduque à la clôture)
- **Version cible** : n/a
- **Dernière mise à jour** : 2026-07-04
- **Doc(s) liée(s)** : [docs/subsystems/history.md](../../docs/subsystems/history.md), [docs/subsystems/pump-controller.md](../../docs/subsystems/pump-controller.md)

## Note de clôture (2026-07-04)

Spec rédigée en avril 2026, **écartée** (et non « supersédée » : le besoin n'est pas couvert à l'identique — il est jugé insuffisant au regard du coût). Audit contre le code v2.6.0 :

**Le besoin est largement servi par l'existant :**
- Audit « quoi/quand/combien » : chaque arrêt de dosage est loggé avec durée, volume et cumul journalier (`Arrêt dosage pH (auto): durée=Xs vol≈Y mL total jour=Z/300 mL`, `pump_controller.cpp` l.838/989), et ces logs sont **persistés** sur LittleFS (`/system.log`, partition history, rotation 16 KB — plusieurs jours de rétention à 2-5 injections/jour).
- Diagnostic short-cycling : mieux servi par les **causes de refus** (feature-036 `DoseRefusal`, champ WS `phDoseBlockedReason` + chips UI) et par les compteurs par cause prévus en feature-022 — ce sont les refus qui expliquent un short-cycling, pas les injections réussies.
- Dépassement de quota : impossible par construction depuis 036/006 (gardes prédictives testées en natif, cumul UI, drapeau `limit_reached` MQTT/UI) — l'audit a posteriori vise un scénario qui ne peut plus se produire.

**La prémisse technique centrale était fausse :** la spec budgétait une rotation à 100 KB sur une partition `history` « de 128 KB, déjà isolée ». La partition réelle fait **64 KB** (`partitions.csv`, layout v3 ADR-0019) et héberge déjà `/history.json` ET `/system.log`. Le design aurait dû être entièrement repensé (ADR stockage), alourdissant encore l'une des specs les plus chères du backlog (module persistance + 2 endpoints + 2 cartes UI paginées + export CSV + hook pump_controller avec passage pool-chemistry obligatoire).

**Porte de sortie si un vrai besoin d'audit émerge à l'usage :** variante allégée à ~20 % du coût — ring RAM des ~50 dernières injections exposé via un GET simple + petite table UI, sans persistance, sans rotation, sans export. Mentionnée dans `feature-022-observabilite-dosage.md`.

Aucun code modifié au titre de cette spec. Le contenu ci-dessous est conservé en l'état (avec ses erreurs de prémisse) à titre de traçabilité.

## Domaines touchés
- [ ] Capteurs (pH / ORP / température)
- [ ] Régulation (pump_controller, PID)
- [ ] Filtration / Éclairage
- [x] Web — routes HTTP / WebSocket
- [ ] MQTT / Home Assistant
- [ ] UART / écran LVGL
- [ ] OTA
- [ ] Authentification
- [x] Historique / persistance
- [x] Frontend (`data/`)

## Agents requis
- [x] spec-reader
- [x] architect
- [ ] pool-chemistry — non (pas de logique chimique)
- [ ] ux-ui-designer — non (table standard, pas de refonte visuelle ; à reconsidérer si la pagination ou le filtre demandent un design dédié)
- [x] embedded-developer
- [x] web-ui-developer
- [x] test-engineer
- [x] code-reviewer
- [x] doc-writer

## Description
Ajouter un journal circulaire « par-injection » : chaque injection pH ou ORP (auto, programmée, manuelle) génère une entrée avec timestamp, produit, volume, durée, origine, raison d'arrêt. Consultable via un nouvel endpoint `GET /api/history/injection` et exposé dans les pages /ph et /orp.

## Contexte
À ce jour, seul l'historique 5 min agrégé existe (`/get-history`, voir [docs/subsystems/history.md](../../docs/subsystems/history.md)). Impossible de répondre à « à 14h02, j'ai injecté 25 mL de pH- parce que la cible était à 7.5 ».

Utile pour :
- Auditer un dépassement de quota journalier a posteriori
- Diagnostiquer un short-cycling (anti-cycling refuse → l'utilisateur se demande pourquoi)
- Afficher la timeline dans l'UI (futur)

## Comportement attendu

### Côté firmware
- Nouveau fichier `/injection.log` sur la partition `history` dédiée ([ADR-0007](../../docs/adr/0007-table-partitions-custom.md)) — 128 KB, déjà isolée.
- Format : CSV binaire compact ou JSON lines (à trancher par architect). Recommandation : JSON lines pour rester compat avec `/get-history`.
- Une entrée par injection, générée **à l'arrêt** de l'injection (pas au démarrage) pour disposer de la durée et de la raison :
  - `ts` (ISO 8601 local, YYYY-MM-DDTHH:MM:SS)
  - `chem` (`ph` | `orp`)
  - `mode` (`automatic` | `scheduled` | `manual`)
  - `volume_ml` (float, 1 décimale)
  - `duration_ms` (uint32)
  - `reason_start` (`pid_error_above_threshold` | `scheduled_slot` | `manual`)
  - `reason_stop` (`target_reached` | `hourly_limit` | `daily_limit` | `manual_stop` | `max_cycles` | `min_time_expired` | `filtration_off` | `ota` | `power_off`)
  - `ph_at_start`, `ph_at_stop` (ou `orp_*`, float)
- Journal circulaire : si la taille dépasse 100 KB (marge sur 128 KB), supprimer les N plus anciennes entrées. Pas de rotation temporelle forcée.
- Flush différé : écriture batch toutes les 60 s OU immédiatement en fin d'injection.
- Endpoint `GET /api/history/injection` (auth READ) :
  - Query : `?chem=ph|orp`, `?from=ISO`, `?to=ISO`, `?limit=N` (défaut 100, max 1000)
  - Retour : JSON array, plus récent en premier
- Endpoint `DELETE /api/history/injection` (auth CRITICAL) pour purge manuelle.

### Côté UI
- Dans `/ph` et `/orp`, nouvelle carte repliable « Historique des injections » sous la carte Historique :
  - Table : date, durée, volume, raison arrêt
  - 20 entrées par page, bouton « Charger plus »
- Lien de téléchargement « Exporter CSV » (filtre courant appliqué).

### Cas limites
- Boot : si `/injection.log` existe et est corrompu, l'ignorer (backup silencieux) et repartir vierge + log `warning`.
- Injection interrompue par reboot : pas d'entrée journal (la fin n'a jamais été appelée). Acceptable — ne compromet pas la sécurité (les cumuls journaliers restent en NVS).
- Horloge non synchronisée au moment d'une injection : utiliser `uptime_ms` en fallback et marquer `ts: "boot+NNNs"`.
- Disque plein : la rotation circulaire garantit qu'on ne dépasse pas 100 KB ; vérifier dans les tests.

## Comportement actuel
Aucun journal par-injection. Seul `/get-history` fournit les bins 5 min agrégés.

## Contraintes techniques
- ESP32, offline first
- Partition `history` (128 KB, LittleFS dédié) — voir `partitions.csv`
- Pas de bloquage I/O dans AsyncWebServer handler (< 50 ms)
- Écritures batchées, flush fin d'injection
- ArduinoJson 7 pour serialization
- Cohabitation avec `/get-history` et `/history/import` existants (ne pas mélanger les fichiers)

## Fichiers potentiellement concernés
- `src/history.cpp` / `src/history.h` — ajouter un module `InjectionLog` ou classe dédiée
- `src/pump_controller.cpp` — appeler `InjectionLog.record(...)` en fin d'injection (dans `stopDosing()` ou équivalent)
- `src/web_routes_data.cpp` — nouveaux endpoints `/api/history/injection` (GET, DELETE)
- `src/constants.h` — `constexpr size_t kInjectionLogMaxBytes = 100 * 1024;`
- `data/index.html` — cartes historique injections /ph, /orp
- `data/app.js` — consommation API, pagination, export CSV
- `docs/API.md` — documenter les nouveaux endpoints
- `docs/features/page-ph.md`, `page-orp.md` — mentionner la nouvelle carte
- `docs/subsystems/pump-controller.md` — mentionner l'appel à `InjectionLog.record`

## Livrables — check-list
- [ ] A1 — Module `injection_log.h/.cpp` [nouveau] : `InjectionEntry`, `record()`, `read()`, `purge()`
- [ ] A2 — Format JSON Lines persisté sur la partition `history` (LittleFS dédié)
- [ ] A3 — Rotation circulaire à 100 KB (max sur 128 KB partition)
- [ ] A4 — Hook `pump_controller` : appel `InjectionLog::record()` à la fin de chaque injection
- [ ] A5 — `kInjectionLogMaxBytes = 100 * 1024` dans `constants.h`
- [ ] A6 — `GET /api/history/injection?chem=&from=&to=&limit=` dans `web_routes_data.cpp` (auth READ)
- [ ] A7 — `DELETE /api/history/injection` (auth CRITICAL)
- [ ] B1 — Carte « Historique des injections » repliable dans `/ph` et `/orp`
- [ ] B2 — Pagination 20 entrées + bouton « Charger plus »
- [ ] B3 — Bouton « Exporter CSV » (filtre courant)
- [ ] C1 — `docs/API.md` : nouvelle section « Historique par-injection »
- [ ] C2 — `docs/features/page-ph.md`, `page-orp.md` : nouvelle carte
- [ ] C3 — `docs/subsystems/pump-controller.md` : hook record
- [ ] C4 — `docs/subsystems/injection-log.md` [nouveau] : module + format
- [ ] C5 — `docs/adr/NNNN-format-injection-log.md` : ADR sur format JSON Lines + rotation
- [ ] C6 — `CHANGELOG.md` : entrée
- [ ] D1 — `pio run` sans nouveau warning, `./build_fs.sh` passe

## Nouvelles signatures / API
- `GET /api/history/injection?chem=ph&from=...&to=...&limit=100` → JSON array
- `DELETE /api/history/injection` → 204 (auth CRITICAL)
- Classe `InjectionLog` dans `src/injection_log.h` [nouveau] :
  ```cpp
  struct InjectionEntry { time_t ts; Chemistry chem; Mode mode; float volumeMl; uint32_t durationMs; Reason start, stop; float valueStart, valueStop; };
  void InjectionLog::record(const InjectionEntry&);
  size_t InjectionLog::read(Chemistry, time_t from, time_t to, size_t limit, InjectionEntry* out);
  void InjectionLog::purge();
  ```

## ADR attendu ?
- **Probable** : oui
- **Critère(s) candidat(s)** : 1 (format de persistance partagé), 4 (verrouillage long terme : tout futur consommateur du log devra parser ce format)
- **Sujet pressenti** : `docs/adr/NNNN-format-injection-log.md` — JSON Lines, rotation circulaire 100 KB, partition `history` dédiée. Tracer les alternatives (CSV binaire, base SQLite, exfiltration MQTT) et la décision.

## Éléments visibles (pour doc-writer)

### Endpoints HTTP concernés
- `GET /api/history/injection` (READ)
- `DELETE /api/history/injection` (CRITICAL)

### Champs WebSocket
- Aucun (l'UI fait des GET explicites, pas de push temps réel du journal)

### Topics MQTT
- Aucun

### Documentation à produire / mettre à jour

#### Docs vivantes (1 par surface touchée)
- `docs/features/page-ph.md`, `docs/features/page-orp.md` — carte historique injections
- `docs/subsystems/pump-controller.md` — hook `InjectionLog::record()`
- `docs/subsystems/injection-log.md` [nouveau] — module + format

#### Docs transverses (si contrat externe modifié)
- `docs/API.md` — section nouvelle « Historique par-injection »
- `CHANGELOG.md` — entrée

#### Docs immuables
- `docs/adr/NNNN-format-injection-log.md` — ADR format + rotation

## Critères d'acceptation
- [ ] Après 5 injections pH, `GET /api/history/injection?chem=ph` retourne les 5 entrées, plus récente en premier
- [ ] Chaque entrée contient ts, volume, duration, reason_start, reason_stop, ph_at_start, ph_at_stop
- [ ] Une injection stoppée par limite journalière a bien `reason_stop = "daily_limit"`
- [ ] Une injection manuelle a bien `mode = "manual"`, `reason_start = "manual"`, `reason_stop = "manual_stop"` ou `"min_time_expired"`
- [ ] Rotation circulaire : après dépassement 100 KB, les plus anciennes entrées disparaissent
- [ ] `DELETE /api/history/injection` purge tout, GET suivant retourne `[]`
- [ ] Reboot en pleine injection → pas d'entrée (acceptable)
- [ ] `pio run` compile sans nouveau warning
- [ ] `./build_fs.sh` passe
- [ ] Tous les libellés utilisateur sont en français
- [ ] Comportement nominal testé manuellement selon le plan de test ci-dessous

## Plan de test manuel
1. Forcer 3 injections pH en mode manuel (10, 20, 5 mL), noter les heures. `GET /api/history/injection?chem=ph` retourne 3 entrées cohérentes.
2. Mode automatic, cible 7.0, pH actuel 7.5 → injection démarre, cumul 295/300 → s'arrête à 300. Entrée `reason_stop = "daily_limit"`.
3. Générer assez d'entrées pour dépasser 100 KB (script curl en boucle), vérifier que les plus anciennes sont évincées.
4. `DELETE /api/history/injection` + reboot + GET → vide.
5. Exporter CSV depuis l'UI, ouvrir dans un tableur, vérifier les colonnes.

## Notes pour agent
- **Ne pas mélanger avec le CSV `/get-history`** qui sert à exporter les sensor_data 5 min. Les formats et l'usage sont distincts.
- **Partition dédiée** : le journal va sur la partition `history` LittleFS (si elle est déjà montée séparément). Sinon, vérifier l'ADR-0007.
- **Performance GET** : filtrer côté firmware pour ne pas renvoyer 100 KB à chaque requête. Pagination obligatoire.
- **Intégration UI** : pour l'instant, simple table. Pas de graphe — les bins 5 min couvrent cet usage.

## Hors périmètre
- Consommation temps réel via WS — pas nécessaire, l'UI recharge à l'ouverture de la carte
- Export par injection vers MQTT — non, pollution inutile
- Filtrage multi-critères complexes (e.g. « toutes les injections > 20 mL ») — l'UI peut trier côté client dans la limite de 1000 entrées chargées
