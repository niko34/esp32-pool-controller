## Métadonnées
- **ID** : feature-123
- **Slug** : experience-de-reserve-dma-au-demarrage
- **Type** : feature — **expérience active** sur le chemin de démarrage (lot C-1b)
- **Statut** : todo
- **Priorité** : normale — **bloquée** jusqu'au relevé de `feature-122` sur cible
- **Version cible** : 2.50.0
- **Dernière mise à jour** : 2026-09-08
- **Spec(s) liée(s)** : `feature-122` (lot C-1a, **prérequis strict** : elle fournit les planchers et le dimensionnement), `bug-080` lot C (la cause racine)

## Domaines touchés
- [ ] Capteurs (pH / ORP / température)
- [ ] Régulation (pump_controller, PID) → **le gate `pool-chemistry` est requis pour un AUTRE motif, voir ci-dessous**
- [ ] Filtration / Éclairage
- [x] Web — routes HTTP (dépôt de la taille d'essai, lecture du verdict)
- [ ] MQTT / Home Assistant
- [ ] UART / écran LVGL
- [ ] OTA
- [ ] Authentification
- [ ] Historique / persistance
- [ ] Frontend (`data/`)

## Agents requis
- [x] spec-reader
- [x] architect
- [x] **pool-chemistry — OBLIGATOIRE**, non pour un chemin de dosage mais parce que ce lot retire **irréversiblement** de la RAM interne **au démarrage**, sur le tas de ~57 Ko dont dépendent les six gardes de sécurité chimique
- [ ] ux-ui-designer — aucune surface UI
- [x] embedded-developer
- [x] web-ui-developer — dépôt de la taille d'essai par route authentifiée
- [x] test-engineer
- [x] code-reviewer
- [x] doc-writer

## Description

Installer une **réserve DMA d'essai** au démarrage, mesurer son effet sur les
quatre masques de capacités, et confronter le résultat à la table dérivée. Le
lot ne pose **aucune réserve définitive** : il valide le mécanisme et son
dimensionnement pour que le correctif de fond (lot C-3) puisse être écrit sur des
chiffres relevés.

## Contexte

Détaché de `feature-122` le **2026-09-08**, sur décision de l'utilisateur prise
avant le début du cycle. Le périmètre d'origine mélangeait une mesure sans risque
et cette expérience, qui touche le démarrage de façon irréversible.

### Pourquoi ce lot est dangereux, et pourquoi il est isolé

1. `esp_psram_extram_reserve_dma_pool` prend le **plus gros bloc contigu
   d'abord** — exactement la grandeur que mesurent toutes les gardes du projet.
2. **Aucune API ne retire une région** enregistrée. Un mauvais dimensionnement
   est **irréversible jusqu'au redémarrage**.
3. Sur `ESP_ERR_NO_MEM`, la fonction **rend la main en laissant les régions déjà
   enregistrées** : le code de retour ne dit **pas** ce qui a été pris.
4. **L'OTA est lui-même gardé par des seuils mémoire**
   (`kCheckUpdateMinMaxAllocBytes`, `oomTlsGateReason`). Une réserve mal calibrée
   peut fermer la porte par laquelle on viendrait la corriger — et il n'existe
   **aucun repli série** depuis le conteneur de développement.
5. Le tas 8 bits ne vaut qu'**environ 57 Ko au démarrage** et retombe à **26 Ko**
   en exploitation. Les 32 Ko du défaut IDF sont **inabordables**.

### Prérequis strict

**Ne pas ouvrir ce lot avant le relevé de `feature-122` sur cible.** C'est elle
qui fournit le chiffre manquant : la **demande concurrente réelle en pointe** en
mémoire `0x080C`. Dimensionner la réserve sans lui serait deviner — et deviner
ici coûte potentiellement l'accès à l'appareil.

Il est même possible que ce lot devienne **inutile** : si les planchers de
`feature-122` montrent que la demande en pointe est couverte par une simple
réduction de demande (`bug-121`, double ouverture `exists()`+`open()`), le
correctif de fond n'aura pas besoin de réserve du tout.

### Ce qui est déjà établi — à ne pas redémontrer

- Mécanisme confirmé au désassemblage de `esp_psram.c.obj` :
  `heap_caps_get_largest_free_block(0x808)` → `heap_caps_malloc(n, 0x808)` →
  `heap_caps_add_region_with_caps(caps[3], ptr, ptr+n-1)`.
- Tableau de capacités **confirmé depuis la source IDF** :
  `caps[] = {0, MALLOC_CAP_DMA|MALLOC_CAP_INTERNAL, MALLOC_CAP_8BIT|MALLOC_CAP_32BIT}`
  → `all_caps = 0x080E`. Donc `0x0804` (métrique actuelle des gardes) **voit** la
  réserve, et `0x1804` ne la voit pas. **La mesure de ce lot est une
  confirmation, pas une exploration** — mais elle reste nécessaire : les
  bibliothèques sont précompilées, la réserve peut être faite de plusieurs
  morceaux non contigus, et l'ADR-0058 est né d'une dérivation crue sur parole.
- **NVS est lisible en tête de `setup()`** : `nvs_flash_init()` est appelé par
  `initArduino()` avant `setup()`, donc `Preferences` est utilisable avant
  `Serial.begin()`.

### ⚠️ Transmis par `feature-122` (lot C-1a livré en v2.49.0) — À LIRE AVANT DE DIMENSIONNER

Trois points relevés en revue de `feature-122`, qui conditionnent la lecture des
chiffres que ce lot va utiliser.

1. **Le plancher `0x080C` n'a qu'une résolution de 4 s.** C'est le chiffre qui
   dimensionnera la réserve, et il est échantillonné **en tour de rôle** (un seul
   masque O(blocs) supplémentaire par échantillon de 2 s), là où `0x0804` est
   mesuré **à chaque requête** (filtre de la garde 503 + garde d'ouverture de
   fichiers). **Ne pas lire `hf.dma.mb` comme une pointe** : une pointe de
   quelques millisecondes lui échappe.
2. **Mais dimensionner sur `0x0804` reste CONSERVATEUR.** Comme
   `heaps(0x080C) ⊆ heaps(0x0804)`, le plancher fin de `0x0804` (`minBoot` de la
   ligne `Heap 8bits:`) **borne supérieurement** celui de `0x080C`. Le chiffre
   `0x080C` sert à confirmer l'**ordre de grandeur** et à identifier le
   **contexte** du creux (`hf.dma.cb`), pas à capturer la pointe.
3. **Ce que le témoin A7 ne dit pas.** Il tranche où tombe un `malloc(4096)`
   **au démarrage, PSRAM quasi vide** — **pas** où tombe le tampon stdio en
   pleine rafale, sous pression PSRAM, quand la préférence `extmem` peut
   retomber silencieusement en interne (c'est une **préférence**, pas une
   contrainte). Un `w96 = true` ne garantit donc pas que les tampons partent en
   PSRAM **pendant** un chargement d'interface.

Cadre hérité : [ADR-0077](../../../docs/adr/0077-metrique-de-garde-exclut-les-pools-inatteignables.md)
— la pose de la réserve **et** la bascule de métrique vers `0x1804` appartiennent
au **même lot**. Protocole de relevé et signification de chaque champ :
[system-health.md](../../../docs/subsystems/system-health.md#protocole-de-relevé-sur-cible).

## Comportement attendu

- Le firmware livré **ne réserve rien** : la taille d'essai par défaut vaut
  **zéro**.
- Une taille d'essai peut être déposée par une **route authentifiée**, et
  s'applique **au démarrage suivant** — jamais à chaud.
- Un dimensionnement mortel est **inexprimable**, pas seulement déconseillé :
  plafond absolu **et** reste minimal obligatoire après réserve, vérifiés par une
  **fonction pure**.
- **Essai one-shot auto-désarmé** : un drapeau NVS « essai en vol » n'est effacé
  qu'après confirmation de bonne santé en `loop()`. Un démarrage qui n'atteint
  pas la confirmation laisse le drapeau posé, et le démarrage **suivant saute la
  réserve** en le journalisant. Un redémarrage suffit donc **toujours** à revenir
  à un état connu.
- La **taille réellement obtenue** est dérivée du **delta des sondes**, jamais du
  code de retour ni de la taille demandée.
- Verdict par masque : la table dérivée est rapportée **CONFIRMÉE** ou
  **DÉMENTIE**.

### Cas limites
- `ESP_ERR_NO_MEM` → démarrage normal, sans réserve, et le dire. Jamais
  d'`abort()`.
- `psramFound()` faux → saut silencieux (binaire unique WROOM/WROVER-E).
- Réserve obtenue en **plusieurs morceaux** non contigus → la somme compte, pas
  le nombre de blocs.
- Drapeau d'essai laissé par une **coupure secteur** : le drapeau vit en **NVS**
  et non en RAM RTC, précisément parce que la RAM RTC ne survit pas à une coupure
  et qu'un essai « oublié » serait rejoué à l'aveugle.
- Verdict **DÉMENTIE** : c'est un **résultat**, pas un échec de cycle. Interdiction
  de « réparer » la table pour la faire coller.

## Comportement actuel

Aucune réserve n'existe. `CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL 0` dans le
`sdkconfig.h` réellement compilé, contre un défaut IDF de **32768**.

## Contraintes techniques
- **Chemin de démarrage** : exécution avant `systemLogger.begin()` → aucun log
  sur place, mémoriser puis journaliser (idiome `main.cpp:127-131`)
- **Aucun `abort()`**, jamais, sur ce chemin
- Watchdog actif ; `kWatchdogTimeoutSec = 30`
- ADR-0033 : une ligne agrégée, jamais une par échantillon
- ADR-0058 : la référence est le pool 8 bits
- ADR-0070 : aucune ouverture de fichier hors du helper unique
- Aucun champ WebSocket (marge de budget 43 o)
- Surface non authentifiée non élargie (`bug-113` : −80,73 %)

## Fichiers potentiellement concernés
- `src/main.cpp` — l'appel, à côté de `heap_caps_malloc_extmem_enable`
- `src/constants.h` — défaut à zéro, plafond, reste minimal, délai de confirmation
- un module **pur** neuf — plafonnement et refus de la taille d'essai
- `src/web_routes_config.cpp` — dépôt de la taille, lecture du verdict
- `docs/subsystems/system-health.md`, `docs/API.md`, ADR probable

## Livrables — check-list
- [ ] A1 — Constantes : défaut **zéro**, plafond absolu, reste minimal obligatoire,
      délai de confirmation
- [ ] A2 — Fonction **pure** de plafonnement/refus (rend la taille retenue **et**
      un motif de refus), rendant le dimensionnement mortel **inexprimable**
- [ ] A3 — Lecture NVS de la taille en tête de `setup()`, garde `psramFound()`,
      appel **uniquement** si taille > 0
- [ ] A4 — Essai one-shot : drapeau NVS, confirmation en `loop()`, saut au
      démarrage suivant si non confirmé, journalisé
- [ ] A5 — Taille **réellement obtenue** dérivée du **delta des sondes**
- [ ] A6 — Les quatre masques relevés avant/après, une ligne agrégée après
      `systemLogger.begin()`
- [ ] A7 — Verdict **CONFIRMÉE / DÉMENTIE** par masque, calculé par la fonction
      pure de classification livrée en `feature-122`
- [ ] B1 — Dépôt de la taille d'essai par route **authentifiée** ; lecture du
      verdict dans `/get-system-info`
- [ ] C1 — `docs/subsystems/system-health.md` : le mécanisme d'essai et son
      désarmement ; `docs/API.md` : les champs neufs
- [ ] C2 — ADR si le mécanisme d'essai auto-désarmé est jugé structurant
- [ ] C3 — `CHANGELOG.md`
- [ ] T1 — Plafonnement : une taille mortelle est refusée avec son motif
- [ ] T2 — Taille obtenue ≠ demandée ; réserve refusée → démarrage normal ;
      essai non confirmé → saut
- [ ] T3 — **Mutation** : plafond retiré ; taille **demandée** journalisée au lieu
      de la taille **obtenue** ; drapeau d'essai non posé → rejeu à l'aveugle

## Nouvelles signatures / API
- Endpoints HTTP nouveaux : **aucun** — dépôt et lecture sur des routes existantes
- Champs WebSocket : **aucun**
- Topics MQTT : aucun
- Constantes : défaut à zéro, plafond, reste minimal, délai de confirmation

## Sécurité chimique
- Dosage impliqué : **non** — aucun chemin de dosage modifié
- **Mais le gate `pool-chemistry` est OBLIGATOIRE** : ce lot retire
  irréversiblement de la RAM interne au démarrage, sur le tas où se mesurent les
  **six gardes de sécurité chimique** documentées dans `system-health.md` et les
  seuils d'auto-guérison (`kRestartMaxAllocHeapBytes`). Un boot-loop suspendrait
  le dosage après trois redémarrages.
- Conditions attendues du gate : **plafond de taille** et **reste minimal** après
  réserve, tous deux chiffrés et vérifiables.

## ADR attendu ?
- **Probable** : oui
- **Critère(s) candidat(s)** : 1, 2, 4
- **Sujet pressenti** : « une expérience sur le chemin de démarrage doit se
  désarmer d'elle-même » — la contrainte vaut pour toute expérience future sur ce
  chemin, et le motif (l'OTA gardé par des seuils mémoire, sans repli série) est
  invisible dans le code.

## Notes

Ce lot ne doit **jamais** être livré en même temps que le correctif de fond
(lot C-3, la réserve définitive plus la bascule de métrique sur `0x1804`).
Livrés ensemble, on ne saurait plus dire lequel des deux a produit l'effet
observé — et si la métrique n'était pas basculée, la garde 503 serait aveuglée :
elle afficherait le gros bloc libre de la réserve pendant que le pool général
resterait affamé, et l'appareil planterait exactement là où elle protège
aujourd'hui. Troisième occurrence de ce mode de défaillance dans ce dépôt, après
le parachute anti-OOM logé en PSRAM et les quatre gardes rendues mortes par une
métrique IRAM-inclusive (ADR-0058).
