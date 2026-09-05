# ADR-0068 — La flash de 8 Mo est utilisée en entier (layout v5)

- **Statut** : Accepté
- **Date** : 2026-09-05
- **Décideurs** : Nicolas Philippe
- **Spec(s) liée(s)** : aucune spec dédiée — chantier d'infrastructure ouvert au constat de saturation pendant `bug-080` (lots B-2 / B-3). Dissout `bug-099`, dégonfle `bug-098`.
- **Supersède** : [ADR-0024](0024-partitions-layout-v4.md) (layout v4, app 1792 KB / spiffs 320 KB, table close à 0x400000)

## Contexte

### Le fait qui change tout : la puce est une 8 Mo, la table s'arrêtait à 4 Mo

`platformio.ini` déclarait `board_build.flash_size = 4MB` et la table de partitions
se terminait à `0x400000`. **Cette déclaration ne décrivait pas le matériel.**
Vérification `esptool flash_id` sur l'appareil, **avant toute modification** :

```
Chip type          : ESP32-D0WD-V3 (revision v3.1)
MAC                : 8c:94:df:9c:8d:f8
Manufacturer       : 20
Device             : 4017
Detected flash size: 8MB
```

L'octet de capacité du `Device ID` vaut `0x17`, soit 2²³ = **8 388 608 octets**.
La moitié haute de la flash — **4 Mo** — n'était référencée par aucune partition :
ni lue, ni écrite, ni comptée. Elle n'était pas « réservée », elle était **perdue**.

### Ce que cette erreur de déclaration coûtait

L'occupation applicative était de **1 825 717 o sur un slot de 1 835 008 o, soit
99,5 %** — **9 291 octets de marge**. Concrètement : moins de 10 Ko, c'est-à-dire
moins que le coût typique d'une fonctionnalité moyenne de ce projet.

Cette saturation n'était pas un inconfort théorique, elle **dictait les décisions
techniques** depuis plusieurs versions. Les notes des lots B-2 ([ADR-0066](0066-sonde-du-tas-au-moment-du-refus.md))
et B-3 ([ADR-0067](0067-garde-503-avant-fopen-handler-prioritaire.md)) portaient
explicitement la phrase *« le prochain lot devra arriver avec son propre poste de
récupération flash »* : chaque nouveau correctif devait **financer sa propre place**
en supprimant du code ailleurs. C'est une contrainte qui pousse à arbitrer sur le
poids plutôt que sur la justesse.

En parallèle, la partition `history` (64 Ko) était **elle aussi** une contrainte
active, et pas une contrainte lointaine : `bug-099` documente une marge de croissance
tombant à **90 octets** dès qu'un `.tmp` de rotation orphelin coexiste avec un tampon
de log plein. Quatre-vingt-dix octets. Le mode de défaillance de ce budget étant le
**silence** (`historySaveAdmit()` refuse la sauvegarde sans rien casser de visible —
voir [history.md](../subsystems/history.md#le-mode-de-défaillance-du-budget-est-le-silence-pas-la-corruption-)),
la marge n'était pas seulement mince : elle était **mince et muette**.

Trancher maintenant plutôt que plus tard suit la logique des migrations v2→v3 et
v3→v4 : un changement de table exige un **flash USB**, donc un accès physique au
boîtier. Autant le faire pendant que l'appareil est accessible et que rien ne brûle.

## Décision

**Étendre la table de partitions aux 8 388 608 octets réellement présents sur la
puce (layout v5), et répartir les 4 Mo récupérés entre les deux slots applicatifs,
`spiffs` et `history`.**

### Table layout v5 ([`partitions.csv`](../../partitions.csv))

| Partition | Offset v5 | Taille v5 | v4 | Δ |
|---|---|---|---|---|
| nvs | 0x9000 | 0x5000 (20 KB) | idem | **inchangé — config préservée** |
| otadata | 0xE000 | 0x2000 (8 KB) | idem | inchangé |
| **app0** | 0x10000 | **0x390000 (3648 KB)** | 0x1C0000 (1792 KB) | **+1856 KB** |
| **app1** | **0x3A0000** | **0x390000 (3648 KB)** | 0x1C0000 (1792 KB) | **+1856 KB**, offset décalé |
| **spiffs** | **0x730000** | **0x80000 (512 KB)** | 0x50000 (320 KB) | **+192 KB**, offset décalé |
| **history** | **0x7B0000** | **0x40000 (256 KB)** | 0x10000 (64 KB) | **+192 KB**, offset décalé |
| coredump | **0x7F0000** | 0x10000 (64 KB) | 0x10000 (64 KB) | taille inchangée, offset décalé |

Vérifications d'arithmétique, refaites poste par poste : `app0` fin = 0x10000 + 0x390000
= 0x3A0000 = début `app1` ✓ ; `app1` à 0x3A0000 **aligné 64 Ko** (0x3A0000 / 0x10000 = 58,
entier — contrainte dure des partitions `app`) ✓ ; `app1` fin = 0x730000 = début `spiffs` ✓ ;
`spiffs` fin = 0x7B0000 = début `history` ✓ ; `history` fin = 0x7F0000 = début `coredump` ✓ ;
`coredump` fin = **0x800000 = 8 388 608 o** ✓. Aucun chevauchement, **zéro octet perdu**.

### Répartition : pourquoi 3648 Ko par slot et non 3840

Le maximum atteignable, en laissant `spiffs`/`history`/`coredump` à leurs tailles v4,
serait de **3840 Ko par slot**. On a délibérément **prélevé 192 Ko pour `spiffs` et
192 Ko pour `history`** (soit 384 Ko, donc 192 Ko de moins par slot) pour trois raisons :

1. `history` à 64 Ko était une **contrainte active et silencieuse** (`bug-099`, marge
   résiduelle 90 o). Une contrainte qui mord aujourd'hui prime sur une marge qui
   servira peut-être dans trois ans.
2. À 3648 Ko, la marge applicative est déjà de **1 909 835 o, soit 51 %**. Les 192 Ko
   supplémentaires n'auraient fait passer la marge de 51 % à 56 % : sans effet
   décisionnel.
3. `spiffs` à 512 Ko rend au frontend une latitude que le layout v4 lui avait retirée
   (« frontend contraint à 320 Ko », conséquence négative assumée de l'ADR-0024).

### Mesures après migration (vérifiées sur l'appareil)

| Grandeur | v4 | v5 |
|---|---|---|
| Occupation applicative (`esp32dev_core3`) | **99,5 %** (1 825 717 / 1 835 008) | **48,9 %** (1 825 717 / 3 735 552) |
| Marge applicative par slot | **9 291 o** | **1 909 835 o** |
| `spiffs` | 320 Ko | **512 Ko** — 221 184 o utilisés, **303 104 o libres** |
| `history` | 64 Ko | **256 Ko** |

Relevé `/get-system-info` post-flash : `ota_partition_size = 3 735 552`,
`fs_total_bytes = 524 288`, `flash_size = 8 388 608`, boot sur `app0`.

## Alternatives considérées

- **Option A (rejetée) — Dégraisser le firmware.**
  C'était la trajectoire implicite du projet : chaque lot finançait sa place. Elle
  demande un travail de suppression à chaque cycle, arbitre les correctifs sur leur
  **poids** plutôt que sur leur justesse, et **ne rend la place qu'une fois**. Elle ne
  résout pas non plus le budget `history`. Surtout : elle consiste à se serrer la
  ceinture au-dessus de 4 Mo de flash inutilisée.

- **Option B (rejetée) — Rogner `spiffs` de 320 à 256 Ko, dans le cadre 4 Mo.**
  C'est le palier qu'on aurait pris sans le `flash_id`. **Chiffrage** : 64 Ko libérés,
  à partager entre `app0` **et** `app1` (les deux slots OTA doivent rester égaux), soit
  **+32 Ko par partition applicative** — contre **+1856 Ko** pour l'option retenue.
  **Un facteur 58.** Et le prix aurait été un frontend ramené à 256 Ko, alors même que
  l'option retenue le porte à 512 Ko. Cette alternative est conservée ici parce qu'elle
  illustre le vrai risque : **optimiser à l'intérieur d'une contrainte fausse**.

- **Option C (rejetée) — Maximum applicatif, 3840 Ko par slot.**
  Laisse `history` à 64 Ko, donc laisse `bug-099` ouvert et son mode de panne muet en
  place, pour une marge applicative passant de 51 % à 56 % — écart sans conséquence
  décisionnelle. Voir « Répartition » ci-dessus.

- **Option D (rejetée) — Changer de module (8/16 Mo, PCB v3).**
  C'était la porte de sortie identifiée par les ADR-0015, 0019 et 0024 (« plus de
  palier disponible sur une flash 4 MB »). Elle est **sans objet** : le module en place
  **est** un 8 Mo. Le coût matériel et le re-spin de PCB étaient un coût imaginaire.

- **Option E (retenue) — Table étendue à 0x800000, app 3648 Ko, spiffs 512 Ko, history 256 Ko.**
  Supprime la contrainte dominante du projet, dissout un bug ouvert, ne coûte qu'un
  flash USB déjà nécessaire pour tout changement de table.

## Conséquences

### Positives

- **La contrainte flash cesse d'être un paramètre de conception.** 1 909 835 o de marge
  par slot, contre 9 291. La phrase *« le prochain lot devra arriver avec son propre
  poste de récupération flash »*, portée par les notes des lots B-2 et B-3, est
  **retirée** : elle n'a plus d'objet.
- **`bug-099` est dissous.** La marge de croissance de la partition `history` n'est plus
  de 90 octets dans le pire cas mais de plusieurs dizaines de Ko : un `.tmp` orphelin
  cumulé à un tampon de log plein ne franchit plus rien. Le nettoyage du `.tmp` reste
  souhaitable **par hygiène**, il n'est plus **structurel**.
- **`bug-098` devient trivial à financer.** L'octet supplémentaire par point (339 o au
  total) était un arbitrage ; ce n'en est plus un.
- **Le frontend n'est plus contraint à 320 Ko** (conséquence négative de l'ADR-0024,
  levée) : 303 104 o libres dans `spiffs`.
- **La NVS est préservée.** Elle reste à `0x9000`, **hors de la zone déplacée** :
  consigne pH 7,3, consigne ORP 530, offset pH 0,5, taille de bassin, SSID, calibrations,
  compteurs journaliers — rien à reconfigurer après le flash. Vérifié sur l'appareil.

### Négatives / dette assumée

- ⚠️ **Flash USB obligatoire. L'OTA ne peut PAS appliquer ce changement** — il n'écrit
  que l'application, jamais le bootloader ni la table. Accès physique au connecteur
  requis. Voir [UPDATE_GUIDE.md](../UPDATE_GUIDE.md#migration-layout-v4--v5-flash-8-mo--depuis-2026-09-05).
- ⚠️ **`spiffs`, `history` et `coredump` changent d'offset : leur contenu est EFFACÉ.**
  Pour `history`, l'effacement est de surcroît **explicite** : `HistoryManager::begin()`
  compare la taille de partition à la clé NVS `hist_meta/part_sz` et efface sur écart
  (protection anti-`lfs_alloc` div/0). **Sauvegarder l'historique AVANT le flash, appareil
  encore en ligne** — c'est une étape de la procédure, pas une précaution optionnelle.
- ⚠️ **Toute la sauvegarde n'est pas réimportable.** `POST /history/import` refuse les
  corps de plus de **16 384 octets** (`AsyncCallbackJsonWebHandler::_maxContentLength`,
  valeur par défaut de la lib, jamais surchargée) : **145 points compactés passent, 146
  non**. Sur 245 points sauvegardés le 05/09, **100 n'ont pas pu être réimportés** (23 au
  31 août — les plus anciens) ; ils restent dans le fichier de sauvegarde. Limite
  documentée dans [API.md](../API.md#post-historyimport--write).
- **Les offsets de récupération manuelle changent.** `esptool erase_region 0x3E0000 0x10000`
  (partition `history`, cité par [ADR-0031](0031-disjoncteur-bootloop-history.md) et
  [system-health.md](../subsystems/system-health.md)) devient
  **`erase_region 0x7B0000 0x40000`**. L'ancienne commande n'efface plus la partition
  `history` mais une zone au milieu de `app1` : **à ne surtout pas lancer telle quelle**.
- **Granularité 64 Ko** des slots `app` (alignement `0x10000` obligatoire) : les futurs
  ajustements se font par paliers de 64 Ko minimum. Inchangé depuis l'ADR-0024.
- **La déclaration `flash_size` n'est plus vérifiable à l'œil.** Elle affirme désormais
  une propriété du matériel ; sur un module 4 Mo, ce firmware **ne démarrerait pas**.
  C'est le prix de l'exactitude, et c'est le bon sens de l'erreur (échec franc, pas
  silencieux) — mais la flotte est **mono-appareil** et doit le rester sur ce point.

### Ce que ça verrouille

- **Le sujet « place en flash » est clos pour des années.** À rythme de consommation
  historique, 1,9 Mo de marge par slot ne se consomme pas sur un horizon prévisible.
  Corollaire : **aucun arbitrage de conception ne doit plus invoquer la place flash**
  sans remesurer d'abord.
- **Il n'y a plus de palier au-delà.** C'était vrai en v4 pour une mauvaise raison (table
  fausse) ; c'est vrai en v5 pour la bonne : la table occupe **la totalité** de la puce.
  Un dépassement futur exigerait réellement un module 16 Mo et un re-spin de PCB.
- **Le quatuor de références reste la contrainte d'intégrité du layout**, avec un membre
  critique : `partitions.csv`, `platformio.ini` (`flash_size = 8MB`,
  `filesystem_size = 524288`, **dans les deux environnements**), `build_fs.sh` (`-s 524288`)
  et `deploy.sh` (**`LITTLEFS_OFFSET = 0x730000`**, contrôle de taille 524288). ⚠️ Oublier
  `LITTLEFS_OFFSET` écrit le filesystem **au mauvais endroit** — au milieu de `app1` — sans
  message d'erreur.
- [ADR-0024](0024-partitions-layout-v4.md) passe en `Superseded by ADR-0068`.

## Migration

Procédure suivie le 2026-09-05, à reproduire telle quelle si elle se represente :

1. **Sauvegarder tout ce qui vit sur les partitions DATA, appareil encore en ligne** →
   `builds/sauvegarde-avant-flash-8mo/` : `history.json` (245 points du 23/08 au 05/09),
   `history.csv`, `system-log.json`, `config.json`, `data.json`, `system-info.json`,
   `kpi.json`. **Cette étape est irréversible si on l'oublie.**
2. **`esptool flash_id`** — confirmer la taille **physique** avant de toucher à quoi que
   ce soit. C'est ce qui distingue ce changement d'un pari.
3. Modifier les **4** fichiers du quatuor, puis `pio run` et `./build_fs.sh`.
4. **`./deploy.sh all` par USB** : bootloader + table + application + filesystem.
   **L'OTA ne peut pas appliquer ce changement.** Ne pas utiliser `./deploy.sh factory`
   (efface la NVS).
5. `deploy.sh` efface `otadata` (`0xE000`) pour forcer le démarrage sur `app0` —
   **indispensable** après un déplacement de partitions : l'`otadata` héritée pointerait
   vers un slot qui n'existe plus au même endroit.
6. **Réimporter l'historique** — par lots de **≤ 145 points** (voir la limite 16 384 o
   ci-dessus).

Journal de démarrage attendu : `LittleFS monté avec succès`, `Partition historique
dédiée montée`, `Historique chargé (N points)`.

## Références

- Code : [`partitions.csv`](../../partitions.csv), [`platformio.ini`](../../platformio.ini),
  [`build_fs.sh`](../../build_fs.sh), [`deploy.sh`](../../deploy.sh)
- Limite d'import : [`src/web_routes_data.cpp:960-1025`](../../src/web_routes_data.cpp),
  `AsyncJson.cpp` (`_maxContentLength(16384)`, `handleRequest` → `send(413)`)
- ADR antérieurs : [ADR-0024](0024-partitions-layout-v4.md) (layout v4, supersédé),
  [ADR-0019](0019-partition-app-1664k.md), [ADR-0015](0015-partition-app-1.5mb.md),
  [ADR-0009](0009-partition-coredump.md) (partition `coredump`),
  [ADR-0031](0031-disjoncteur-bootloop-history.md) (offset de récupération — **périmé par
  le présent ADR**), [ADR-0033](0033-budget-espace-partition-history.md) (budget `history`)
- Contexte d'ouverture : [ADR-0066](0066-sonde-du-tas-au-moment-du-refus.md),
  [ADR-0067](0067-garde-503-avant-fopen-handler-prioritaire.md) — lots B-2 et B-3 de `bug-080`
- Specs à relire au nouveau budget : `specs/features/todo/bug-098-drapeau-dosage-orp-perdu-a-l-ecriture.md`,
  `specs/features/todo/bug-099-tmp-de-rotation-jamais-nettoye-au-boot.md`
- Doc : [BUILD.md](../BUILD.md), [UPDATE_GUIDE.md](../UPDATE_GUIDE.md),
  [subsystems/history.md](../subsystems/history.md), [API.md](../API.md)
