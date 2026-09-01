# ADR-0057 — Taille du bassin : présélection de réglages, jamais entrée de calcul

- **Statut** : Accepté
- **Date** : 2026-08-31
- **Décideurs** : utilisateur (principe fondateur), pool-chemistry (table + visa et contre-visa du 31/08/2026), architect
- **Spec(s) liée(s)** : feature-090 (s'appuie sur feature-087 — valeurs d'usine recalibrées — et feature-088 — source unique `regulation_defaults.h`, dialogue avant/après)

## Contexte

Le paramétrage de la régulation (limites d'injection, vitesse PID, délai de stabilisation) demande des connaissances que l'utilisateur final n'a pas. La seule grandeur qu'il connaît vraiment est la **taille de son bassin**. La tentation classique est d'en faire une **entrée de calcul** (dosage volumétrique : mL = ΔpH × volume × facteur) — c'est ce que l'ADR-0055 avait déjà écarté pour le feedforward. Par ailleurs, les valeurs d'usine v2.30.0 (feature-087) ne sont mesurées **que** sur le bassin de référence (30-50 m³) : un petit bassin subirait une excursion de mélange aveugle double (≈ 0,20 pH à 20 m³ pour 10 min/h), un grand bassin manquerait de capacité.

## Décision

Trois décisions solidaires :

### (a) INVARIANT ABSOLU — la taille ne sert qu'à PRÉSÉLECTIONNER

La taille du bassin (`PoolSize` : `small` / `medium` / `large`) **présélectionne des réglages modifiables**. Elle n'entre **JAMAIS** dans un calcul de dosage, de débit, de quota ou de correction. La régulation reste pilotée par les réglages (modifiables) et par l'**apprentissage de la consommation réelle** (anneau `ph_load7`, feedforward ADR-0055). Principe fondateur de l'utilisateur : « si je donne le volume de mon bassin et qu'il est pris en compte, l'appareil ne fonctionnera pas avec une autre piscine ».

**Preuve consignée (31/08/2026)** : `grep -rln "poolSize" src/` ne retourne que `config.h`, `config.cpp`, `regulation_defaults.h` et `web_routes_config.cpp` — jamais `pump_controller.*` ni aucun chemin de dosage. L'invariant est en outre prouvé **par mutation** dans `test/test_native_regulation_defaults/` (l'application d'une colonne ne touche que les 8 champs de tuning, `poolSize` n'est pas un champ restauré).

### (b) Table à trois colonnes, statut des mesures explicite, verrou de migration

`kRegulationDefaultsBySize` ([`src/regulation_defaults.h`](../../src/regulation_defaults.h)) porte les 8 défauts de tuning par taille :

| Réglage | Small (< 20 m³) | **Medium (20-60 m³)** | Large (> 60 m³) |
|---|---|---|---|
| Limites injection pH / ORP | 5 / 5 min/h | **10 / 10 min/h** | 15 / 15 min/h |
| Vitesse de correction | slow | **normal** | fast |
| Délai de stabilisation | 5 min | **5 min** | 10 min |
| Consignes pH / ORP | 7,3 / 600 mV | 7,3 / 600 mV | 7,3 / 600 mV |
| Volumes quotidiens programmés | 0 | 0 | 0 |

- **Statut des colonnes** : Medium = adossée aux **mesures** du bassin de référence (v2.30.0, visa pool-chemistry du 31/08) ; Small et Large = **EXTRAPOLATIONS**, étiquetées comme telles dans le code et la doc. Logique : **excursion de mélange aveugle ~constante** (~0,10-0,13 pH) à travers les tailles, capacité journalière qui suit le besoin, mélange plus lent au-delà de 60 m³ ; petit bassin = gain du procédé élevé → correction douce, grand bassin → l'inverse. Les consignes pH/ORP ne dépendent **pas** de la taille (chimie de l'eau, pas volume).
- **Verrous compile-time** : `static_assert` de bornes chimiques **par colonne**, de **monotonie** (limites et vitesse croissantes avec la taille) et **verrou de migration** — la colonne Medium est comparée aux littéraux v2.30.0 **en dur** : clé NVS `pool_size` absente ⇒ Medium ⇒ **zéro changement de comportement** à la mise à jour ; retoucher Medium exige de retoucher l'assert, en connaissance de cause (nouveau visa requis).

### (c) La vitesse de correction dérive de la taille et disparaît de l'UI

`regulation_speed` reste **intact dans l'API, la NVS et MQTT**, mais est **masqué dans l'UI** dès que le firmware expose `pool_size` : il dérive de la taille (slow/normal/fast). Il reste visible dans le **dialogue de changement de taille** (seul endroit) et se ré-affiche face à un ancien firmware.

## Alternatives considérées

- **Volume numérique en entrée de calcul de dosage** (rejetée) — casse l'invariant (a) : l'appareil ne fonctionnerait correctement que sur le bassin déclaré, et un volume erroné fausserait chaque dose. L'apprentissage fait déjà ce travail sans dépendre d'une déclaration (ADR-0055).
- **Suppression du champ `regulation_speed`** (rejetée) — casserait la rétrocompat (API/MQTT/NVS, clients anciens) et priverait les experts d'un réglage fin par API ; le masquage UI obtient la simplification sans rupture de contrat.
- **Entité Home Assistant pour `pool_size`** (rejetée) — une écriture automatisée (automatisation HA) changerait 8 réglages de régulation d'un coup sans dialogue de confirmation, contraire à l'esprit de l'invariant (choix humain, délibéré, rare). `pool_size` n'est exposé que dans `/get-config` / `/save-config`.
- **Table des défauts dupliquée en JS** (rejetée) — leçon 087/088 : source unique firmware. L'UI lit `regulation_defaults_by_size` servi par `/get-config` ; aucune valeur de colonne n'existe côté client.

## Conséquences

### Positives
- Un seul réglage compréhensible pilote le paramétrage initial ; les 8 réglages restent modifiables (le dialogue avant/après protège les personnalisations).
- Migration de flotte **sans effet** garantie à la compilation.
- L'appareil reste correct sur n'importe quelle piscine (l'apprentissage pilote, pas la déclaration).

### Négatives / dette assumée
- Colonnes Small et Large **non mesurées** (extrapolations visées, à confirmer sur le terrain).
- Sur > 60 m³, le quota journalier d'usine `maxPhMlPerDay` = 300 mL/j (inchangé, protection) **plafonne en pratique** bien avant la promesse 15 min/h (≈ 11 min d'injection/jour à 28 mL/min) — à relever manuellement si besoin, l'alerte de sous-dimensionnement (ADR-0056) guide.
- La vitesse masquée ne se règle plus que par API (cas experts) ou via la taille.

### Ce que ça verrouille
- Aucun futur code ne peut lire `poolSize` dans un calcul de dosage/débit/quota sans violer cet ADR (et le grep de preuve).
- La colonne Medium est figée sur les littéraux v2.30.0 tant que le `static_assert` de migration n'est pas retouché avec visa.
- La sérialisation `"small"`/`"medium"`/`"large"` (NVS + JSON) est un contrat stable ; valeur inconnue ⇒ Medium (fail-safe).

## Références

- Code : [`src/regulation_defaults.h`](../../src/regulation_defaults.h) (enum, table, verrous), [`src/web_routes_config.cpp`](../../src/web_routes_config.cpp) (bloc `pool_size` de `/save-config`, `regulation_defaults_by_size`), [`src/config.cpp`](../../src/config.cpp) (NVS `pool_size`)
- Tests : `test/test_native_regulation_defaults/` (colonnes, monotonie, migration, sérialisation — mutations 6/6)
- Spec : `specs/features/done/feature-090-taille-bassin-preselections-regulation.md` (visa et contre-visa pool-chemistry du 31/08/2026)
- ADR liés : [0017](0017-logique-metier-pure-humble-object-testabilite.md) (header pur testable), [0054](0054-seuil-de-vraisemblance-fonction-de-la-configuration.md) (seuils fonction de la config), [0055](0055-feedforward-appris-additif-plafonne.md) (dosage volumétrique déjà écarté), [0056](0056-sous-dimensionnement-actionneur-3-jours-80-pct.md) (alerte guidant le relèvement des limites)
- Doc : [`docs/API.md`](../API.md), [`docs/features/page-settings.md`](../features/page-settings.md), [`docs/features/page-wizard.md`](../features/page-wizard.md), [`docs/subsystems/pump-controller.md`](../subsystems/pump-controller.md)
