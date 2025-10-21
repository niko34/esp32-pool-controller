# Guide de Simulation - Contrôleur de Piscine ESP32

## Vue d'ensemble

Le mode simulation a été complètement refait pour modéliser de manière réaliste le comportement d'une piscine lors de l'injection de produits chimiques (pH- et chlore).

## Modèle Physique Réaliste

### 1. Inertie et Temps de Mélange

Contrairement à l'ancienne version qui appliquait instantanément l'effet des produits, le nouveau modèle simule l'**inertie** du système :

- Le produit injecté ne se mélange **pas instantanément** dans toute la piscine
- Il faut attendre que l'eau circule à travers le système de filtration
- Le mélange se fait de manière **progressive** selon une courbe exponentielle

#### Temps de Cycle de Filtration

```
Temps de cycle (heures) = Volume piscine (m³) / Débit filtration (m³/h)
```

**Exemple** : Piscine de 50m³ avec filtration de 16m³/h
```
Temps de cycle = 50 / 16 = 3.125 heures
```

Il faut donc **3h08** pour que toute l'eau passe une fois à travers la filtration.

### 2. Réservoir Tampon Virtuel

Le modèle utilise un **réservoir tampon virtuel** :

1. Quand on injecte du produit → il va dans le "tampon"
2. Le produit se mélange progressivement du tampon vers la piscine
3. La vitesse de mélange dépend de la constante `mixingTimeConstant`

#### Constante de Temps de Mélange

- **0.5 cycle** → 63% du produit est mélangé après 0.5 cycle de filtration
- **1.0 cycle** → 63% du produit est mélangé après 1 cycle complet
- Plus la valeur est petite, plus le mélange est rapide

**Formule du transfert** :
```
Taux de transfert = 1 - exp(-cycles_écoulés / mixingTimeConstant)
```

### 3. Effet des Produits Chimiques

#### pH Moins (Acide)

Paramètre : `phMinusEffectPerLiter`
- **Valeur par défaut** : `-0.2` 
- **Signification** : 1 litre de pH- diminue le pH de 0.2 pour une piscine de 10m³
- **Ajustement automatique** au volume réel de votre piscine

**Exemple** :
- Piscine de 50m³
- Injection de 100ml de pH-
- Effet = (0.1L × -0.2) × (10m³ / 50m³) = **-0.004 unités pH**

#### Chlore (ORP)

Paramètre : `chlorineEffectPerLiter`
- **Valeur par défaut** : `100` mV
- **Signification** : 1 litre de chlore augmente l'ORP de 100mV pour une piscine de 10m³

### 4. Dérive Naturelle

La piscine évolue naturellement au fil du temps :

#### Dérive du pH
- Paramètre : `phDriftPerHour` = `+0.02` par heure
- Le pH a tendance à **augmenter** (évaporation, UV, utilisation)

#### Dérive de l'ORP
- Paramètre : `orpDriftPerHour` = `-5.0` mV par heure
- L'ORP a tendance à **diminuer** (consommation du chlore par UV, matière organique)

### 5. Accélération du Temps

Le système permet d'**accélérer le temps** pour observer rapidement le comportement :

- Paramètre : `timeAcceleration` = `360.0`
- **360x** signifie que 1 heure réelle passe en **10 secondes**
- **Toutes les dynamiques** suivent cette accélération :
  - Injection de produits
  - Mélange progressif
  - Dérive naturelle
  - Horloge système (si `overrideClock = true`)

## Configuration

### Paramètres dans `SimulationConfig`

```cpp
struct SimulationConfig {
  bool enabled = true;

  // Paramètres physiques de la piscine
  float poolVolumeM3 = 50.0f;                    // Volume de la piscine en m³
  float filtrationFlowM3PerHour = 16.0f;         // Débit de filtration en m³/h

  // Paramètres pH- (acide)
  float phPumpRateMlPerMin = 30.0f;              // Débit pompe pH- (ml/min)
  float phMinusEffectPerLiter = -0.2f;           // Effet de 1L de pH- sur le pH pour 10m³
  float phMixingTimeConstant = 0.5f;             // Constante de temps mélange (en cycles)

  // Paramètres Chlore (ORP)
  float orpPumpRateMlPerMin = 30.0f;             // Débit pompe chlore (ml/min)
  float chlorineEffectPerLiter = 100.0f;         // Effet de 1L de chlore sur ORP (mV) pour 10m³
  float orpMixingTimeConstant = 0.5f;            // Constante de temps mélange (en cycles)

  // Dérive naturelle
  float phDriftPerHour = 0.02f;                  // Dérive du pH par heure
  float orpDriftPerHour = -5.0f;                 // Dérive de l'ORP par heure

  // Valeurs initiales
  float initialPh = 7.8f;
  float initialOrp = 600.0f;
  float initialTemp = 24.0f;

  // Accélération temporelle
  float timeAcceleration = 360.0f;               // 360x = 1h en 10s
  bool overrideClock = true;                     // Accélère l'horloge système
};
```

## Scénarios d'Utilisation

### Scénario 1 : Test Rapide (360x)

**Configuration** :
```cpp
timeAcceleration = 360.0f;  // 1h → 10 secondes
```

**Résultat** :
- Une journée complète (24h) passe en **4 minutes**
- Idéal pour tester rapidement le système
- Les graphiques montrent l'évolution sur 24h en quelques minutes

### Scénario 2 : Observation Détaillée (60x)

**Configuration** :
```cpp
timeAcceleration = 60.0f;  // 1h → 1 minute
```

**Résultat** :
- Une journée complète (24h) passe en **24 minutes**
- Permet d'observer plus en détail les transitions
- Meilleur pour comprendre la dynamique de mélange

### Scénario 3 : Temps Réel (1x)

**Configuration** :
```cpp
timeAcceleration = 1.0f;  // Temps réel
```

**Résultat** :
- Simulation en temps réel
- Utile pour les tests de longue durée ou validation finale

## Exemple de Comportement

### Injection de pH-

**Contexte** :
- Piscine de 50m³
- pH initial = 7.8
- Cible pH = 7.2
- Débit pompe = 30 ml/min
- Accélération = 360x

**Déroulement** :

1. **t=0s** : pH = 7.8, dosage pH- activé
2. **t=5s** (30min simulées) : Injection de 900ml, effet encore faible car en cours de mélange
3. **t=10s** (1h simulée) : pH commence à baisser visiblement (~7.65)
4. **t=30s** (3h simulées) : Le produit est bien mélangé, pH atteint ~7.3
5. **t=40s** (4h simulées) : Le PID a ajusté, pH stabilisé à 7.2

**Inertie visible** : L'effet n'est pas instantané, on voit la courbe descendre progressivement !

## Avantages du Nouveau Modèle

✅ **Réalisme** : Simule l'inertie réelle d'une piscine  
✅ **Dynamique de mélange** : Modèle exponentiel basé sur le débit de filtration  
✅ **Paramétrable** : Tous les paramètres physiques sont configurables  
✅ **Accélération variable** : Testez à différentes vitesses  
✅ **Dérive naturelle** : Simule l'évolution sans intervention  
✅ **Compatible graphiques** : Les courbes montrent l'inertie du système

## Calibrage pour Votre Piscine

### Étape 1 : Mesures Physiques

Mesurez sur votre installation :
- Volume de la piscine (m³)
- Débit de filtration (m³/h)
- Débit réel des pompes doseuses (ml/min)

### Étape 2 : Test pH-

1. Notez le pH initial
2. Injectez une quantité connue de pH- (ex: 500ml)
3. Attendez le mélange complet (2-3 cycles de filtration)
4. Mesurez le pH final
5. Calculez : `phMinusEffectPerLiter = (pH_final - pH_initial) × (volume_piscine / 10) / litres_injectés`

### Étape 3 : Test Chlore

Même principe pour l'ORP :
1. Notez l'ORP initial
2. Injectez une quantité connue de chlore
3. Attendez le mélange
4. Mesurez l'ORP final
5. Calculez : `chlorineEffectPerLiter = (ORP_final - ORP_initial) × (volume_piscine / 10) / litres_injectés`

### Étape 4 : Constante de Mélange

Observez combien de temps il faut réellement pour voir l'effet stabilisé :
- Si c'est ~1.5 cycles de filtration → `mixingTimeConstant = 0.5`
- Si c'est ~3 cycles de filtration → `mixingTimeConstant = 1.0`

## Dépannage

### Le pH/ORP change trop vite

➜ Augmentez `mixingTimeConstant` (essayez 1.0 ou 1.5)  
➜ Vérifiez que `filtrationFlowM3PerHour` correspond à votre débit réel

### Le pH/ORP change trop lentement

➜ Diminuez `mixingTimeConstant` (essayez 0.3)  
➜ Vérifiez `poolVolumeM3`

### L'effet des produits est trop fort/faible

➜ Ajustez `phMinusEffectPerLiter` ou `chlorineEffectPerLiter`  
➜ Faites un test de calibrage (voir ci-dessus)

### Les graphiques ne suivent pas l'accélération

➜ Vérifiez que `overrideClock = true`  
➜ Le système de graphiques doit utiliser `time()` pour l'axe temporel

## Architecture Technique

### Algorithme Principal

```cpp
void updateSimulation() {
  1. Mettre à jour l'horloge accélérée
  2. Calculer le temps simulé écoulé
  3. Ajouter les injections au réservoir tampon
  4. Transférer progressivement du tampon vers la piscine (mélange)
  5. Appliquer la dérive naturelle
  6. Limiter les valeurs aux plages physiques
  7. Publier si changement significatif
}
```

### Fréquence de Mise à Jour

- **100ms** : Mise à jour de la simulation
- Permet une animation fluide même en temps accéléré
- Avec accélération 360x : chaque 100ms = 6 minutes simulées

## Comparaison Ancien vs Nouveau

| Caractéristique | Ancien Modèle | Nouveau Modèle |
|----------------|---------------|----------------|
| Effet instantané | ✅ Oui | ❌ Non (réaliste) |
| Inertie | ❌ Non | ✅ Oui |
| Mélange progressif | ❌ Non | ✅ Oui |
| Basé sur filtration | ❌ Non | ✅ Oui |
| Dérive naturelle | ⚠️ Basique | ✅ Paramétrable |
| Calibrage physique | ⚠️ Difficile | ✅ Intuitif |

## Conclusion

Le nouveau système de simulation offre une représentation **beaucoup plus réaliste** du comportement d'une piscine. Il permet de :

- Tester le contrôleur PID dans des conditions réalistes
- Visualiser l'inertie du système
- Comprendre pourquoi il faut du temps pour corriger le pH/ORP
- Ajuster les paramètres du PID en fonction de la dynamique réelle

Bon test ! 🏊‍♂️
