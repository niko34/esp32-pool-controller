# feature-087 — Recalibrage des valeurs d'usine de régulation

## Métadonnées

| Champ | Valeur |
|---|---|
| Type | ajustement de valeurs d'usine (format minimal `specs/modele.md`) |
| Statut | done — livré en v2.30.0 (31/08/2026) |
| Priorité | normale |
| Version cible | 2.30.0 |
| Origine | Demande utilisateur du 31/08/2026, sur la base des mesures récentes |
| Sécurité chimique | **Oui — visa `pool-chemistry` obligatoire** (défauts de régulation) |

## Le problème

Les valeurs d'usine de `config.h` ont été invalidées ou questionnées par les
mesures du bassin de référence (30-50 m³, TAC 112, stabilisant 35 mg/L —
un bassin médian, rien d'extrême) :

1. **`phInjectionLimitMinutes = 5`** : prouvé impuissant. Besoin estival
   ~1 800 mL/j, maximum délivrable à 5 min/h ≈ 911 mL/j (mesuré le 23/08 :
   765-802 mL en saturation permanente). Des semaines de pH bloqué au-dessus
   de la consigne — aucune amélioration de régulation ne pouvait compenser.
2. **`orpTarget = 650`** : quasi inatteignable sur un bassin stabilisé
   (l'ORP est déprimé par le CYA ; à 35 mg/L la consigne tenable mesurée est
   550). Une consigne inatteignable = dosage de chlore permanent borné
   seulement par les limites — mécanisme de l'épisode de surconsommation
   de 07/2026. Le cas commun (bassin extérieur stabilisé 30-50 mg/L) est
   celui que le défaut doit servir.
3. **`phTarget = 7.2`** : bas de la plage idéale. Consomme de l'acide sans
   bénéfice net, alors qu'à pH plus haut c'est le chlore qui surconsomme
   (fraction HOCl plus faible à ORP égale). 7,3 = valeur éprouvée sur le
   bassin de référence, centre de plage.

## Changements demandés (config.h uniquement)

| Défaut | Avant | Après | Justification-mesure |
|---|---|---|---|
| `phInjectionLimitMinutes` | 5 | **10** | 5 min/h ⇒ ~911 mL/j < besoin médian 1 800 ; 10 min/h ⇒ ~2 000 mL/j sur 7 h, volume aveugle ≤ 0,10 pH (40 m³) / ~0,20 (20 m³) |
| `orpTarget` | 650.0f | **600.0f** | 650 inatteignable à CYA 30-50 ⇒ dosage permanent ; 600 tenable et relevable |
| `phTarget` | 7.2f | **7.3f** | valeur éprouvée ; à pH plus bas, acide sans bénéfice ; à plus haut, chlore surconsommé |

## Garde-fous / contexte

- **Aucun effet sur les appareils existants** : la NVS prime, ces défauts ne
  servent qu'aux appareils neufs ou réinitialisés.
- Le défaut 10 min/h reste modéré **parce que** la v2.29.0 guide désormais
  l'utilisateur : l'alerte de sous-dimensionnement (ADR-0056) lui dit
  explicitement quand et pourquoi relever le réglage. Défaut prudent +
  guidage explicite > défaut agressif.
- `kPhLoadSustainableFallbackMlPerMin = 2,17` (repli du filet de
  vraisemblance) est dérivé de l'ANCIEN défaut (5/60 × 26). À traiter : soit
  conserver comme plancher prudent en corrigeant son commentaire (sens
  fail-closed), soit recalculer — à faire trancher par le visa.
- Vérifier les `static_assert` de constants.h qui référencent les cibles
  (7,3 ∈ [7,20 ; 7,50] ✓ à confirmer à la compilation).

## Critères d'acceptation

- [x] Visa `pool-chemistry` GO sur les trois valeurs (31/08/2026 — assorti de deux conditions, reportées dans `docs/UPDATE_GUIDE.md`)
- [x] `pio run` SUCCESS 0 warning, `pio test -e native` 994/994
- [x] Arbitrage du repli 2,17 documenté (conservé comme **plancher délibéré**, commentaire de `constants.h` corrigé — voir note technique)
- [x] CHANGELOG + doc mises à jour (UPDATE_GUIDE si impact flotte)
- [x] Revue de code avant commit

## Note technique de clôture (31/08/2026)

**Livré en v2.30.0.** Fichiers modifiés : `src/config.h` (les trois défauts), `src/constants.h` (commentaire du repli, aucune valeur changée), `src/version.h` (2.30.0). Builds verts : `pio run` 0 warning, 994/994 tests natifs — aucun test nouveau requis (changement de valeurs d'usine, la logique est inchangée ; les `static_assert` existants vérifient 7,3 ∈ [`kPhTargetMin` ; `kPhTargetMax`] à la compilation).

**Arbitrage du repli 2,17** (question posée par la spec, tranchée par le visa) : `kPhLoadSustainableFallbackMlPerMin` **reste à 2,17** mais change de statut — ce n'est plus une dérivation du défaut d'usine (5/60 × 26), c'est un **plancher prudent délibéré**. Un repli sert quand la configuration est indisponible ou aberrante : il doit être fail-closed (filtre de vraisemblance plus strict, jours écartés, pas de restauration). L'aligner sur le nouveau défaut (10/60 × 28 ≈ 4,67) l'affaiblirait précisément dans les cas où il sert. Le commentaire de `constants.h:1200` porte l'arbitrage et interdit tout « alignement » futur sur les défauts.

**Conditions du visa** (documentées dans `docs/UPDATE_GUIDE.md`, note v2.30.0) : (a) défaut 10 min/h réservé aux bassins ≥ ~20 m³ — volume de mélange aveugle ~280 mL/h ≈ 0,20 pH à 20 m³, 0,40 à 10 m³, contre une marge de 0,50 ; en dessous, redescendre à 5 min/h ; (b) bassin intérieur non stabilisé : relever la consigne ORP vers 650.

**Pas d'ADR** : valeurs paramétrables, aucune contrainte structurelle ; l'arbitrage vit ici et dans le visa, et l'ADR-0054 couvre déjà le principe « seuil fonction de la configuration ».

**Aucune observation terrain requise** : les défauts d'usine ne concernent pas l'appareil de référence (NVS provisionnée — limite déjà à 25 min/h, consignes 7,30 / 550 réglées à la main). Le tableau du volume aveugle de `docs/subsystems/pump-controller.md` date l'ancien défaut (≤ v2.29.x) et ajoute la ligne 10 min/h (260 mL, 0,092 pH à 40 m³).
