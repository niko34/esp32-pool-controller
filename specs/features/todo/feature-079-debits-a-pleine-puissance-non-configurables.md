# feature-079 — Les débits à pleine puissance sont des constantes de flotte non configurables

## Métadonnées

| Champ | Valeur |
|---|---|
| Statut | todo |
| Origine | Gate `pool-chemistry` du 28/08/2026, en marge de l'arbitrage sur le débit d'usine — signalé comme **prioritaire sur le sujet examiné** |
| Priorité | Haute — sécurité chimique, faille ouverte sur toute la flotte |
| Version cible | à définir |

## Domaines touchés

- `src/constants.h` (~l.946-947) — `kPump1Flow255MlPerMin`, `kPump2Flow255MlPerMin`
- `src/pump_controller.cpp` — crédit du kick, majorant du mode manuel
- Configuration / NVS — les rendre mesurables comme le débit à l'amplitude
- `docs/subsystems/pump-controller.md`, ADR-0043/0047

## Sécurité chimique

**Oui — `pool-chemistry` obligatoire.** Ces constantes servent de **majorant** du
volume crédité en injection manuelle et au crédit du kick de démarrage. Les
sous-estimer fait sous-compter : l'appareil injecte plus que ses limites ne le
croient.

## Problème

```cpp
constexpr float kPump1Flow255MlPerMin = 61.0f;  // Pompe 1 (pH)  — pesé à 255
constexpr float kPump2Flow255MlPerMin = 67.0f;  // Pompe 2 (ORP) — pesé à 255
```

Ces deux valeurs sont :

- **issues d'UN seul exemplaire** chacune (l'unité de référence, banc du
  23/08/2026) ;
- **non configurables** — aucun chemin NVS, aucune route, aucune mesure possible
  par l'utilisateur, contrairement au débit à l'amplitude
  (`pumpN_pulse_flow_ml_per_min`, mesurable depuis la v2.24.0) ;
- **utilisées comme majorant de sécurité** : crédit du volume en injection
  manuelle, et crédit des 0,4 s de kick de chaque impulsion.

Or les deux exemplaires mesurés diffèrent déjà de **10 %** entre eux (61 et 67).
Un appareil neuf dont la pompe 1 débiterait 67 mL/min à pleine puissance
**sous-compterait de 10 %** en injection manuelle : l'utilisateur croirait avoir
injecté 100 mL alors que 110 mL sont sortis, et les limites journalières
seraient dépassées d'autant.

## Ce qui rend le défaut asymétrique

Le projet a soigné le débit à **l'amplitude** : valeur d'usine conservatrice
(`kPumpTypePulseFlowMlPerMin`, arbitrée deux fois par le gate), drapeau
`flow_weighed`, assistant de mesure, `static_assert` de non-régression.

Le débit à **pleine puissance** n'a rien de tout cela, alors qu'il porte le
majorant du seul mode où l'utilisateur commande directement la pompe.

## Piste

### ⛔ Fausse piste à ne pas reprendre — mesurer la purge

Première idée écartée par l'utilisateur le 28/08, **à raison** : « ce n'est pas
possible de mesurer la purge, elle injecte de l'acide ou du chlore. Je ne veux
pas que l'utilisateur manipule ces produits pour les mesurer. »

L'ordre des étapes de l'assistant est délibéré : la purge tourne **refoulement
encore branché**, justement parce qu'elle chasse le produit resté dans le tuyau ;
c'est ce qui permet à la mesure suivante de se faire à l'eau claire, après
débranchement. Faire lire le volume purgé imposerait de débrancher AVANT, donc de
recueillir de l'acide ou du chlore dans une éprouvette. **Aucune procédure de
mesure ne doit exposer l'utilisateur à un produit concentré.**

### 1. Ajouter une phase de mesure à pleine puissance, APRÈS le débranchement

À ce stade de l'assistant, le tuyau est déjà purgé et le refoulement est en
éprouvette : ce qui sort est de **l'eau claire**. Il suffit d'une phase minutée
supplémentaire à duty 255, en plus de la mesure existante à duty 127.

Durée : ~30 s suffisent (≈ 30 mL à 61 mL/min), lisibles sans ambiguïté sur une
éprouvette graduée au millilitre. Coût pour l'utilisateur : une lecture de plus,
**zéro manipulation de produit supplémentaire**, aucun changement de montage.

C'est l'option à retenir : elle ferme la faille, réutilise le montage déjà en
place, et respecte la contrainte de sécurité.

### 2. Repli si la phase supplémentaire est jugée trop lourde

Adopter une valeur d'usine **unique et majorante** pour les deux pompes (max
observé + dispersion, soit nettement au-dessus de 67), verrouillée par
`static_assert`, au lieu de deux valeurs calées chacune sur un exemplaire unique.
Cela **borne** le risque sans le fermer : un appareil neuf sous-comptera moins,
mais restera non mesurable.

### ⛔ Ne pas déduire le débit 255 du débit 127

Les mesures du 28/08 l'interdisent : les deux pompes diffèrent de **10 % à
duty 255** mais de **2 % à duty 127**. Le rapport n'est pas transposable d'un
point de fonctionnement à l'autre — c'est l'argument même par lequel le gate a
refusé de dériver la valeur d'usine des mesures.

## Critères de clôture

- Les débits à pleine puissance sont mesurables par l'utilisateur, avec drapeau
  « mesuré » distinct de la valeur d'usine.
- Valeur d'usine majorante et verrouillée par `static_assert`, sur le modèle de
  `kPumpTypeFlowMeasuredMaxMlPerMin`.
- Le crédit du mode manuel et celui du kick utilisent la valeur mesurée quand
  elle existe.
- Visa `pool-chemistry`.

## Contexte

Décidé le 28/08/2026 de **conserver `kPumpTypePulseFlowMlPerMin = 28,0`**
(gate NO GO sur 26,0 : max de n=2 du même lot, marge nulle), avec justification
réécrite sur la base des mesures réelles 26,0 / 25,5 et ajout du `static_assert`.
C'est en instruisant ce point que le gate a signalé la faille ci-dessus comme
**plus grave que celle examinée**.
