# Schéma de Câblage Détaillé - ESP32 Pool Controller

## 🔌 Vue d'Ensemble

```
┌────────────────────────────────────────────────────────────────┐
│                      ALIMENTATION                               │
├────────────────────────────────────────────────────────────────┤
│                                                                 │
│  230V AC ──┬──► Transfo 12V DC (2A) ──┬──► Pompe 1 (via L298N)│
│            │                           └──► Pompe 2 (via L298N)│
│            │                                                    │
│            ├──► Transfo 5V DC (2A) ─────► ESP32 (VIN + GND)   │
│            │                                                    │
│            └──► Relais 230V ─────────────► Pompe Filtration   │
│                                                                 │
└────────────────────────────────────────────────────────────────┘
```

## 📟 ESP32 - Brochage Complet

### Broches Analogiques (ADC)

| GPIO | Fonction | Capteur | Notes |
|------|----------|---------|-------|
| 34 | ADC1_CH6 | ORP (Redox) | 0-3.3V, ajuster avec diviseur si capteur >3.3V |
| 35 | ADC1_CH7 | pH | 0-3.3V, ajuster avec diviseur si capteur >3.3V |

**Important ADC ESP32:**
- ADC1 uniquement (ADC2 conflit avec WiFi)
- Résolution: 12 bits (0-4095)
- Tension max: **3.3V** (ne jamais dépasser !)
- Impédance d'entrée: ~100kΩ

### Broches Numériques (GPIO)

| GPIO | Fonction | Destination | Type |
|------|----------|-------------|------|
| 4 | OneWire | DS18B20 Température | Input avec pull-up 4.7kΩ |
| 27 | Output | Relais Filtration | Output 3.3V → 5V relay module |
| 25 | PWM (CH0) | Pompe 1 - Vitesse | LEDC 1kHz, 8-bit |
| 32 | Output | Pompe 1 - IN1 (Direction) | Output 3.3V |
| 33 | Output | Pompe 1 - IN2 (Direction) | Output 3.3V |
| 26 | PWM (CH1) | Pompe 2 - Vitesse | LEDC 1kHz, 8-bit |
| 18 | Output | Pompe 2 - IN1 (Direction) | Output 3.3V |
| 19 | Output | Pompe 2 - IN2 (Direction) | Output 3.3V |

### Broches Réservées (Ne Pas Utiliser)

| GPIO | Raison |
|------|--------|
| 0 | Boot mode (utilisé au démarrage) |
| 1 (TX0) | UART console série |
| 2 | Boot mode (LED interne sur certains boards) |
| 3 (RX0) | UART console série |
| 6-11 | Flash SPI (CRITICAL - ne jamais toucher!) |
| 12 | Boot mode voltage |
| 15 | Boot mode silence |

## 🔩 Connexions Détaillées

### 1. Capteur pH

**Exemple: Capteur pH analogique E-201-C**

```
Capteur pH E-201-C                ESP32
┌──────────────┐                 ┌─────────┐
│              │                 │         │
│   VCC (red)  ├─────────────────┤ 5V      │
│              │                 │         │
│   GND (blk)  ├─────┬───────────┤ GND     │
│              │     │           │         │
│   OUT (blu)  ├─────┤           │         │
│              │     │           │         │
└──────────────┘     │           │         │
                     │           │         │
                 ┌───▼────┐      │         │
                 │  R1    │      │         │
                 │  10kΩ  │      │         │
                 │        │      │         │
                 └───┬────┘      │         │
                     ├───────────┤ GPIO 35 │
                 ┌───▼────┐      │         │
                 │  R2    │      │         │
                 │  10kΩ  │      │         │
                 │        │      │         │
                 └───┬────┘      │         │
                     │           │         │
                    GND ─────────┤ GND     │
                                 └─────────┘
```

**Diviseur de tension:**
- Si capteur output 0-5V → Utiliser R1=R2=10kΩ
- Output vers ESP32 = Input × (R2 / (R1 + R2)) = 5V × 0.5 = 2.5V max ✓
- Si capteur 0-3.3V → Connexion directe possible

**Condensateur de filtrage (optionnel):**
- 100nF céramique entre OUT et GND (réduire bruit)

### 2. Capteur ORP (Redox)

**Exemple: Capteur ORP E-201-ORP**

```
Capteur ORP                      ESP32
┌──────────────┐                ┌─────────┐
│              │                │         │
│   VCC (red)  ├────────────────┤ 5V      │
│              │                │         │
│   GND (blk)  ├────┬───────────┤ GND     │
│              │    │           │         │
│   OUT (wht)  ├────┤           │         │
│              │    │           │         │
└──────────────┘    │           │         │
                    │           │         │
                ┌───▼────┐      │         │
                │  10kΩ  │      │         │
                └───┬────┘      │         │
                    ├───────────┤ GPIO 34 │
                ┌───▼────┐      │         │
                │  10kΩ  │      │         │
                └───┬────┘      │         │
                    │           │         │
                   GND          │         │
                                └─────────┘
```

**Notes:**
- Même principe diviseur que pH
- Ajouter condensateur 100nF si mesures instables

### 3. Sonde Température DS18B20

**Sonde étanche OneWire:**

```
DS18B20 (étanche)                ESP32
┌──────────────┐                ┌─────────┐
│              │                │         │
│  Red (VCC)   ├────────────────┤ 3.3V    │
│              │                │         │
│  Black (GND) ├────────────────┤ GND     │
│              │        ┌───────┤         │
│  Yellow (Data)├───┬───┤       │ GPIO 4  │
│              │   │   │        │         │
└──────────────┘   │   │        │         │
                   │   │        │         │
               ┌───▼───▼──┐     │         │
               │ Pull-up  │     │         │
               │  4.7kΩ   │     │         │
               │          │     │         │
               └────┬─────┘     │         │
                    │           │         │
                   3.3V ────────┤ 3.3V    │
                                └─────────┘
```

**Résistance pull-up obligatoire:**
- Valeur: 4.7kΩ (peut aller de 2.2kΩ à 10kΩ)
- Entre DATA et VCC (3.3V)
- Sans pull-up: capteur non détecté (-127°C)

**Longueur câble:**
- <10m: 4.7kΩ OK
- 10-50m: Utiliser 2.2kΩ
- >50m: Prévoir amplification

### 4. Module Relais Filtration

**Relais 5V avec optocoupleur:**

```
ESP32                   Module Relais               Pompe Filtration
┌─────────┐            ┌──────────────┐            ┌─────────────┐
│         │            │              │            │             │
│ GPIO 27 ├────────────┤ IN           │            │             │
│         │            │              │            │             │
│   GND   ├────────────┤ GND      COM ├────────────┤ Phase (L)   │
│         │            │              │            │             │
│ (opt)   │     ┌──────┤ VCC      NO  ├────────────┤230V Contact │
│   5V    ├─────┘      │              │            │             │
│         │            │          NC  │ (non utilisé)            │
└─────────┘            └──────────────┘            └─────────────┘
                             │
                             │ 230V Neutre (N) direct → Pompe
                             └─────────────────────────────────────►
```

**Notes importantes:**
- VCC relais: Certains modules ont VCC isolé → connecter au 5V ESP32
- Signal GPIO 27: 3.3V → OK pour la plupart des modules 5V (seuil ~2.5V)
- Si relais ne commute pas: Ajouter transistor NPN (2N2222) entre GPIO et IN
- **DANGER 230V**: Travail sur installation électrique = électricien qualifié !

### 5. Pompes Doseuses + Drivers L298N

**Configuration pour 2 pompes:**

```
              ESP32                   L298N Driver #1              Pompe pH- 12V
         ┌─────────┐                ┌──────────────┐             ┌──────────┐
         │         │                │              │             │          │
         │ GPIO 25 ├────────────────┤ ENA (PWM)    │             │          │
         │ GPIO 32 ├────────────────┤ IN1      OUT1├─────────────┤ +        │
         │ GPIO 33 ├────────────────┤ IN2      OUT2├─────────────┤ -        │
         │         │                │              │             │          │
         │   GND   ├────┬───────────┤ GND          │             └──────────┘
         │         │    │           │              │
         └─────────┘    │           │ 12V      (VCC├──────► 12V Alim
                        │           └──────────────┘
                        │
                        │           ┌──────────────┐             ┌──────────┐
                        │           │              │             │          │
         ┌─────────┐    │           │              │             │          │
         │         │    │           │ ENA (PWM)    │             │          │
         │ GPIO 26 ├────┼───────────┤ IN1      OUT1├─────────────┤ +        │
         │ GPIO 18 ├────┼───────────┤ IN2      OUT2├─────────────┤ -        │
         │ GPIO 19 ├────┼───────────┤              │             │          │
         │         │    │           │ GND          │             └──────────┘
         │   GND   ├────┴───────────┤              │          Pompe Chlore 12V
         │         │                │ 12V      (VCC├──────► 12V Alim
         └─────────┘                └──────────────┘
                                     L298N Driver #2
```

**Réglage vitesse mini (jumper ENA):**
1. Retirer jumper ENA si présent
2. PWM ESP32 contrôle vitesse via ENA
3. IN1=HIGH, IN2=LOW → Rotation sens horaire
4. IN1=LOW, IN2=HIGH → Rotation anti-horaire
5. IN1=IN2 → Stop (frein)

**Pompes péristaltiques recommandées:**
- Tension: 12V DC
- Débit: 0.5 à 3 ml/min (réglable par PWM)
- Tubing: Silicone alimentaire Ø intérieur 4-6mm
- Exemple: "12V Peristaltic Pump" sur AliExpress/Amazon

## ⚡ Alimentation - Schéma Complet

```
                      ┌─────────────────────────────────────┐
                      │  Tableau Électrique Principal       │
                      │                                     │
     230V AC ─────────┤  Disjoncteur 16A + Différentiel 30mA│
                      │                                     │
                      └────────┬────────────────────────────┘
                               │
                    ┌──────────┴──────────┐
                    │                     │
            ┌───────▼────────┐    ┌──────▼────────┐
            │ Transfo 230V→  │    │ Transfo 230V→ │
            │     12V DC 2A  │    │    5V DC 2A   │
            └───────┬────────┘    └──────┬────────┘
                    │                    │
         ┌──────────┴────────┐           │
         │                   │           │
     ┌───▼────┐         ┌───▼────┐      │
     │ L298N  │         │ L298N  │      │
     │ Driver1│         │ Driver2│      │
     └───┬────┘         └───┬────┘      │
         │                   │           │
    ┌────▼─────┐        ┌───▼─────┐    │
    │ Pompe pH-│        │ Pompe Cl│    │
    │   12V    │        │   12V   │    │
    └──────────┘        └─────────┘    │
                                        │
                               ┌────────▼─────────┐
                               │  ESP32 DevKit    │
                               │  VIN + GND       │
                               └──────────────────┘
```

**Recommandations sécurité:**
- Boîtier étanche IP65 minimum (extérieur piscine)
- Fusible sur chaque alim 12V (2A rapide)
- Différentiel 30mA obligatoire
- Mise à la terre correcte
- Distance >2m du bord piscine (norme NF C15-100)

## 🔧 Liste des Composants

### Électronique

| Composant | Quantité | Référence | Prix (~) |
|-----------|----------|-----------|----------|
| ESP32 DevKit | 1 | ESP32-WROOM-32 | 8€ |
| Capteur pH | 1 | E-201-C ou compatible | 25€ |
| Capteur ORP | 1 | E-201-ORP ou compatible | 30€ |
| Sonde DS18B20 | 1 | DS18B20 étanche | 5€ |
| Module Relais 5V | 1 | 1 canal optocouplé | 3€ |
| Driver moteur L298N | 2 | L298N dual H-bridge | 3€×2 |
| Pompe péristaltique | 2 | 12V 0-100ml/min | 15€×2 |
| Résistances 10kΩ | 4 | 1/4W ±5% | 0.10€×4 |
| Résistance 4.7kΩ | 1 | 1/4W ±5% | 0.10€ |
| Condensateurs 100nF | 2 | Céramique | 0.20€×2 |
| Transfo 230V→12V DC | 1 | 2A min | 10€ |
| Transfo 230V→5V DC | 1 | 2A (ou USB) | 8€ |
| Câbles, boîtier, visserie | - | - | 20€ |

**Total estimé: ~150-200€**

### Consommables

- Tubing silicone Ø6mm (pompes)
- pH- liquide (acide chlorhydrique 10-20%)
- Chlore liquide (hypochlorite 12-15%)
- Gaines thermorétractables
- Dominos/bornier électrique

## 🧪 Tests & Vérification

### Avant Mise en Service

1. **Test ESP32 seul**
   ```
   - Alimenter ESP32 via USB
   - Upload firmware
   - Vérifier logs série
   ```

2. **Test capteurs à sec**
   ```
   - Brancher pH, ORP, Température
   - Vérifier valeurs ADC (raw values)
   - Vérifier pas de court-circuit (multimètre)
   ```

3. **Test pompes**
   ```
   - Alimenter drivers 12V
   - Tubing dans eau (pas produits chimiques)
   - Tester via interface web (mode manuel)
   - Vérifier sens rotation correct
   ```

4. **Test relais filtration**
   ```
   - Brancher lampe 230V au relais (TEST!)
   - Activer filtration via interface
   - Vérifier commutation relais (clic audible)
   ```

### Mise en Service Progressive

1. **Phase 1**: Capteurs uniquement (monitoring passif)
2. **Phase 2**: Ajouter filtration automatique
3. **Phase 3**: Activer dosage pH avec limite stricte (test)
4. **Phase 4**: Activer dosage ORP après validation pH

## 📞 Support Technique

**Problème de mesure ?**
- Vérifier diviseur tension (multimètre)
- Tester capteur seul (sans ESP32)
- Vérifier GND commun (masse unique)

**Pompes ne tournent pas ?**
- Vérifier 12V arrive bien aux L298N
- Tester pompe en direct 12V (bypass driver)
- Vérifier câblage IN1/IN2 (inversé?)

**Relais ne commute pas ?**
- Mesurer tension GPIO (doit être 3.3V)
- Vérifier LED relais s'allume
- Tester avec signal 5V externe

---

**⚠️ AVERTISSEMENT ÉLECTRIQUE**

Toute intervention sur installation 230V doit être effectuée par personne qualifiée. Couper l'alimentation générale avant manipulation. Vérifier absence de tension avec testeur.

**En cas de doute, faire appel à un électricien professionnel.**
