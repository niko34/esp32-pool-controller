# Guide de Calibration - ESP32 Pool Controller

## 📌 Pourquoi Calibrer ?

Les capteurs pH et ORP nécessitent une calibration régulière pour garantir des mesures précises. Sans calibration, vous risquez un dosage incorrect et des problèmes de qualité d'eau.

**Fréquence recommandée:**
- Calibration initiale: Obligatoire avant première utilisation
- Contrôle mensuel: Vérification avec solutions étalons
- Recalibration complète: Tous les 3 mois

## 🧪 Matériel Nécessaire

### Solutions Étalons pH
- **pH 4.01** (solution acide) - ~5€
- **pH 7.00** (solution neutre) - ~5€
- **pH 10.01** (solution basique) - optionnel

Marques recommandées: Hanna Instruments, Milwaukee, Extech

### Solution Étalon ORP
- **Solution ORP 470 mV** (à 25°C) - ~15€
- Ou **Solution Quinhydrone** (86 mV)

### Accessoires
- Béchers en verre ou plastique propres (3x)
- Eau distillée (rinçage)
- Papier absorbant non pelucheux
- Multimètre (pour vérifier tension ADC)
- Thermomètre (compensation température)

## 🔬 Calibration pH - Méthode Complète

### Étape 1: Préparation

1. **Nettoyer la sonde**
   ```
   - Rincer abondamment à l'eau distillée
   - Essuyer délicatement (ne pas frotter l'électrode)
   - Si stockée à sec, tremper 30 min dans solution de stockage (KCl 3M)
   ```

2. **Chauffer les solutions**
   - Les solutions doivent être à 20-25°C
   - Noter la température exacte (compensation)

3. **Connecter à l'ESP32**
   - Brancher la sonde sur GPIO 35
   - Alimenter l'ESP32
   - Ouvrir le moniteur série (115200 bauds)

### Étape 2: Calibration Point 1 (pH 7.00)

1. **Plonger la sonde** dans solution pH 7.00
   - Attendre stabilisation (30-60 secondes)
   - Agiter légèrement si nécessaire

2. **Relever la valeur brute**
   ```
   Moniteur série affichera quelque chose comme:
   [SENSOR] Raw pH ADC: 2048 | Calculated: 7.12
   ```
   Noter: `raw_pH7 = 2048`

3. **Calculer l'offset**
   ```
   offset = 7.00 - valeur_calculée
   offset = 7.00 - 7.12 = -0.12
   ```

### Étape 3: Calibration Point 2 (pH 4.01)

1. **Rincer la sonde** à l'eau distillée

2. **Plonger dans solution pH 4.01**
   - Attendre stabilisation

3. **Relever la valeur**
   ```
   [SENSOR] Raw pH ADC: 1365 | Calculated: 4.73
   ```
   Noter: `raw_pH4 = 1365`

4. **Calculer le slope**
   ```
   delta_pH = 7.00 - 4.01 = 2.99
   delta_ADC = raw_pH7 - raw_pH4 = 2048 - 1365 = 683
   slope = delta_pH / delta_ADC = 2.99 / 683 = 0.00438
   ```

### Étape 4: Appliquer dans le Code

Éditer [`src/sensors.cpp`](src/sensors.cpp):

```cpp
void SensorManager::readRealSensors() {
  // ... code existant ...

  int rawPh = analogRead(PH_PIN);

  // CALIBRATION - Remplacer les valeurs ci-dessous
  const float pH_OFFSET = -0.12;      // Votre offset calculé
  const float pH_SLOPE = 0.00438;     // Votre slope calculé
  const float pH_REFERENCE = 2048.0;  // Valeur ADC pour pH 7.0

  // Formule calibrée
  phValue = 7.0 + (rawPh - pH_REFERENCE) * pH_SLOPE + pH_OFFSET;

  // Limites de sécurité
  if (phValue < 0.0f) phValue = 0.0f;
  if (phValue > 14.0f) phValue = 14.0f;
}
```

### Étape 5: Vérification

1. **Tester avec pH 7.00**
   - Rincer et replonger dans pH 7.00
   - Valeur doit afficher 7.00 ± 0.05

2. **Tester avec pH 4.01**
   - Rincer et replonger dans pH 4.01
   - Valeur doit afficher 4.01 ± 0.10

3. **Tester dans la piscine**
   - Comparer avec test manuel (gouttes ou bandelettes)
   - Écart acceptable: ± 0.2 pH

## ⚡ Calibration ORP - Méthode Complète

### Étape 1: Préparation

1. **Nettoyer la sonde ORP**
   ```
   - Rincer à l'eau distillée
   - Essuyer délicatement
   - Vérifier que l'électrode de platine brille
   ```

2. **Préparer la solution étalon**
   - Solution ORP 470 mV (standard)
   - Température 20-25°C

### Étape 2: Mesure de Référence

1. **Plonger la sonde**
   - Dans solution 470 mV
   - Attendre 2-3 minutes (ORP lent à stabiliser)

2. **Relever la valeur**
   ```
   [SENSOR] Raw ORP ADC: 1920 | Calculated: 469.6 mV
   ```
   Noter: `raw_ORP_ref = 1920` et `measured_ORP = 469.6`

### Étape 3: Calcul du Facteur

```
expected_ORP = 470.0 mV
factor = expected_ORP / measured_ORP
factor = 470.0 / 469.6 = 1.0009
```

### Étape 4: Appliquer dans le Code

Éditer [`src/sensors.cpp`](src/sensors.cpp):

```cpp
void SensorManager::readRealSensors() {
  // ... code existant ...

  int rawOrp = analogRead(ORP_PIN);

  // CALIBRATION - Remplacer les valeurs
  const float ORP_FACTOR = 1.0009;  // Votre facteur calculé
  const float ORP_OFFSET = 0.0;     // Ajuster si nécessaire

  // Formule calibrée
  orpValue = ((rawOrp / 4095.0f) * 1000.0f * ORP_FACTOR) + ORP_OFFSET;

  // Limites
  if (orpValue < 0.0f) orpValue = 0.0f;
  if (orpValue > 2000.0f) orpValue = 2000.0f;
}
```

### Étape 5: Vérification

1. **Re-tester avec solution 470 mV**
   - Doit afficher 470 ± 10 mV

2. **Test en conditions réelles**
   - Comparer avec testeur ORP portatif
   - Piscine normale: 600-750 mV

## 🌡️ Compensation de Température

Les sondes pH et ORP sont sensibles à la température. Pour une précision optimale:

### Compensation pH

```cpp
// Coefficient de température pH (typique: 0.03 pH/°C)
const float TEMP_COEFF_PH = 0.03;
const float TEMP_REF = 25.0; // °C de calibration

float tempCompensatedPh = phValue + (tempValue - TEMP_REF) * TEMP_COEFF_PH;
```

### Compensation ORP

```cpp
// Coefficient de température ORP (typique: -1 mV/°C)
const float TEMP_COEFF_ORP = -1.0;
const float TEMP_REF_ORP = 25.0;

float tempCompensatedOrp = orpValue + (tempValue - TEMP_REF_ORP) * TEMP_COEFF_ORP;
```

## 🛠️ Problèmes Courants

### pH: Lecture Instable

**Symptômes**: Valeur saute constamment
**Causes possibles**:
- Sonde usée (électrode de verre fissurée)
- Bulles d'air sur électrode
- Mauvais contact électrique

**Solutions**:
```
1. Retirer et nettoyer la sonde
2. Tapoter légèrement pour éliminer bulles
3. Vérifier câblage (contacts oxydés?)
4. Tester avec multimètre: tension doit être stable
5. Si >1 an d'utilisation: remplacer sonde
```

### ORP: Réponse Lente

**Symptômes**: Plusieurs minutes pour stabiliser
**Causes**: Normal pour ORP (électrochimie lente)

**Solutions**:
```
1. Nettoyer électrode de platine (solution HCl 10%)
2. Polir légèrement avec papier très fin (600 grit)
3. Attendre 5 minutes avant lecture
4. Vérifier que électrode de référence (Ag/AgCl) est immergée
```

### Valeurs Hors Limites

**pH toujours 7.0**: Sonde non connectée ou HS
**ORP toujours 0**: Électrode de référence sèche ou cassée

## 📋 Checklist de Calibration

Avant de déclarer la calibration terminée:

- [ ] Solutions étalons non périmées (date <6 mois)
- [ ] Température solutions mesurée et notée
- [ ] Sonde rincée entre chaque solution
- [ ] Valeurs stables avant relevé (variation <0.01 pH ou <1 mV)
- [ ] Test de vérification avec solution non utilisée
- [ ] Code modifié et compilé sans erreur
- [ ] Upload sur ESP32 réussi
- [ ] Test dans eau piscine vs mesure manuelle
- [ ] Écart acceptable (<0.2 pH, <20 mV ORP)
- [ ] Date de calibration notée (prochain contrôle dans 3 mois)

## 📝 Journal de Calibration

Tenir un journal pour suivre les dérives:

```
Date: 2024-01-15
Température: 22°C

pH:
  Solution 7.00 → Mesuré: 7.02 (avant calib)
  Solution 4.01 → Mesuré: 4.15 (avant calib)
  Offset calculé: -0.02
  Slope calculé: 0.00442
  Vérif 7.00 → 7.00 ✓
  Vérif 4.01 → 4.02 ✓

ORP:
  Solution 470 mV → Mesuré: 468 mV
  Factor: 1.0043
  Vérif → 470 mV ✓

Test piscine:
  pH sonde: 7.3 | Test gouttes: 7.4 → OK
  ORP sonde: 680 mV | Testeur: 675 mV → OK

Prochain contrôle: 2024-04-15
```

## 🔗 Ressources

- [Théorie pH](https://en.wikipedia.org/wiki/PH_meter)
- [Maintenance sondes](https://www.hannainst.com/blog/electrode-maintenance)
- [Troubleshooting ORP](https://www.sensorex.com/orp-sensor-troubleshooting/)

---

**Conseils de pro:**
- Toujours calibrer dans ordre croissant pH (4→7→10)
- Ne jamais toucher l'électrode de verre (pH) avec les doigts
- Stocker sonde pH dans solution KCl 3M (jamais eau distillée!)
- Remplacer électrolyte interne si sonde rechargeable
