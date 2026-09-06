---
name: parametres-bassin-utilisateur
description: "Chimie de l'eau du bassin — volume, produits, TAC, stabilisant, consignes, et les seuils de référence à ne pas se tromper"
metadata: 
  node_type: memory
  type: project
  originSessionId: 82048c5b-7e8f-4cec-8242-ad9b5ed01f91
  modified: 2026-09-06T00:00:00.000Z
---

## Bassin

30-50 m³ (**volume exact NON confirmé** — une erreur de 25 % invalide tout calcul
de dosage ; à faire confirmer avant tout arbitrage chimique).

## Analyse d'eau du 28/08/2026 (mesurée)

| Paramètre | Valeur | Statut |
|---|---|---|
| TAC | **112 mg/L** | normal (80-120) |
| Stabilisant (acide cyanurique) | **35 mg/L** | ⭐ **DANS la plage idéale** |
| Température | 26,2 °C | — |

## Stabilisant : 35 mg/L est CORRECT

**Les seuils de référence ne sont pas ici** — ce n'est pas une préférence
utilisateur mais de la connaissance métier, donc ils vivent dans
`.claude/agents/pool-chemistry.md` (section « Seuils de référence du domaine »),
**versionnée dans le dépôt** et qui suit donc le projet, y compris chez les tiers.
Décision de l'utilisateur le 28/08/2026.

Ce qui est propre à CE bassin : **35 mg/L mesuré le 28/08, dans la plage idéale**.
Les **15 mg/L de juillet 2026 étaient trop bas**, ce qui explique l'épisode de
surconsommation de chlore de cet été. Passer de 15 à 35 est une **correction**,
pas une dérive : aucune source parasite à traquer, aucune dilution à envisager.

## Produits

- **pH−** : *pH Moins Ultra* (Cash Piscines), **12 % H₂SO₄ + 13 % HCl ≈ 6,9 éq/L**.
  Dosage constructeur : 0,2 L / 10 m³ pour 0,1-0,2 unité de pH.
- **Chlore** : hypochlorite de sodium **36 degrés chlorométriques** (≈ 114 g/L Cl₂,
  ~1,6 mol/L NaOCl). **Basique** : neutralise ~22 % de l'acide injecté.

## Consignes (mise à jour 31/08/2026)

pH **7,30** (plage autorisée 7,20-7,50 depuis v2.25.0) ; offset **+0,50** ;
ORP **530 mV** — ⭐ **calée sur bandelette** : à CYA 35, le chlore mesuré est
« visiblement idéal » à 530-550 (test utilisateur du 31/08). La méthode fait
foi : la consigne ORP de CE bassin se règle sur une mesure directe du chlore,
pas sur une valeur théorique (l'ORP est déprimé par le CYA).

## Réglages de régulation (état 31/08/2026, après restauration volontaire)

L'utilisateur a cliqué « Restaurer les valeurs par défaut » (v2.31.0, 1er jour)
puis remis l'ORP à sa main : limites **pH 10 min/h** (avant : 25 — capacité
~3 380 mL/j sur 13 h de filtration, suffisant pour ~1 800 de besoin), **ORP
10 min/h** (avant : 6, abaissé après l'épisode de juillet — signalé, choix
utilisateur de le laisser ou non), volumes programmés 0, taille de bassin
`medium`. Historique : la limite pH avait été montée 5 → 25 par l'utilisateur
entre le 24 et le 30/08 (cf. [[piege-config-usine-vs-config-vivante]]).

## Relevés de bidon — concordance avec le compteur de l'appareil

### 31/08/2026 — baseline PROPRE posée (chlore)

Bidon chlore **mesuré à 8 750 mL**, compteur `orp_total_injected_ml` **remis à 0**
au même instant. Le point antérieur (même jour, avant remise à zéro) n'était pas
exploitable : le remplissage initial du bidon « 20 L » n'était pas contrôlé.

### 06/09/2026 — première concordance exploitable : l'appareil SUR-compte de +4,76 %

| Grandeur | Valeur |
|---|---:|
| Compteur appareil `orp_total_injected_ml` | **5 290,44 mL** |
| Restant **estimé** (8 750 − 5 290,44) | 3 459,56 mL |
| Restant **mesuré** | **3 700 mL** |
| Injecté **réel** | **5 050 mL** |
| **Biais** | **+240,44 mL = +4,76 %** |

**Méthode** : récipient doseur de 3 L rempli **à ras**, puis complément de 0,7 L.
Incertitude ~±30-50 mL ⇒ biais réel entre **+3,8 % et +5,8 %** — distinguable de
zéro. Fenêtre : **6 jours**, amplitude `duty = 127`.

**Trois choses à ne pas oublier de ce relevé :**

1. **Le sens est CONSERVATEUR** — sur-compter **resserre** les quotas journaliers
   et **avance** l'alerte de bidon vide. Aucun risque chimique, aucune urgence.
2. **+4,76 % est un PLANCHER, pas une valeur.** Une purge expulse du liquide sans
   le compter et **masque** donc une partie du sur-comptage.
   `measures_purge_count` valait 0 au relevé, mais l'appareil avait redémarré
   20 min plus tôt — ce compteur ne couvre pas la fenêtre.
3. **Le biais était DÉJÀ documenté** (`src/constants.h:2083-2089`, dette assumée
   de l'ADR-0051). Ce relevé ne le découvre pas : il le **chiffre**, pour la
   première fois, et dans le sens annoncé.

**Ce que ce relevé NE dit PAS** : la **nature** du biais. Avec le volume seul,
« biais multiplicatif » et « perte fixe par impulsion » (~0,21 mL sur ~1 160
impulsions) sont indiscernables. ⛔ **NO GO `pool-chemistry` du 06/09 sur tout
facteur de comptabilité de flotte** tant que la nature n'est pas établie.
Suites : `feature-110` (compteur d'impulsions — le préalable) puis `feature-078`.

**Pour le prochain relevé** : noter, au même instant, le **volume restant mesuré**,
le **compteur de volume** et — dès que `feature-110` sera livrée — le **compteur
d'impulsions**. Deux inconnues, deux équations.

## Charge basique quotidienne (28/08/2026)

**CO₂ (dégazage) ≈ 80 %, chlore ≈ 20 %.** Besoin estimé : **~1,8 L d'acide/jour
pour simplement TENIR le pH**, +0,9 L pour descendre de 0,2 unité. Or la
régulation n'en demandait que **1,0 L** — la loi P (`flow = kp × erreur`, kp = 8)
sous-dimensionne, et **aucun plafond n'était atteint** (limite à 2000 mL).

Voir [[offset-ph-nature-et-non-suspicion]] : l'offset corrige un décalage de
mesure, le pH affiché est juste.
