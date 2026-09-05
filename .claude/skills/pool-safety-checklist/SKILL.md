---
name: pool-safety-checklist
description: Check-list de sécurité chimique pour toute modification touchant la régulation pH/ORP, le dosage, les capteurs chimiques ou les pompes doseuses. À charger systématiquement par l'agent pool-chemistry et avant toute modif de pump_controller.* / sensors.* (pH/ORP) / régulation PID.
---

# Check-list sécurité chimie — ESP32 Pool Controller

## Pourquoi cette skill existe

Le contrôleur pilote des **pompes doseuses de produits chimiques** : **pH- acide** (via `maxPhMinusMlPerDay`) et **chlore liquide** (via régulation ORP / `orpTrackingEnabled`, `orpContainerVolumeMl`). Le **pH+ basique n'est pas géré** dans la version actuelle de `config.h`. Un bug peut conduire à :
- **Brûlures** / irritation (baignade impossible voire dangereuse)
- **Corrosion** des équipements
- **Chloration excessive** (vapeurs toxiques si chloramines)
- **Perte financière** (surconsommation, équipement HS)

Toute régression ici est inacceptable. Cette skill liste les invariants à vérifier systématiquement.

## Invariants

### I1 — Présence d'eau obligatoire
- ❌ Pas de dosage si la filtration n'est pas ON depuis au moins X secondes (circulation établie)
- Vérification : `filtration.isRunning() && filtration.runningSinceMs() > kMinCirculationBeforeDosingMs`
- Côté code : point d'entrée unique dans `pump_controller` qui vérifie ça en premier

### I2 — Capteur valide
- ❌ Pas de dosage si la dernière mesure est > `kSensorFreshnessMaxMs` (ex. 60 s)
- ❌ Pas de dosage si la valeur est hors plage physique (pH ∉ [0, 14], ORP ∉ [-2000, +2000] mV)
- ❌ Pas de dosage si le capteur est marqué non calibré dans la config
- À chaque refus : `systemLogger.critical()` + alerte MQTT

### I3 — Limites de dosage
- **Par injection** : durée max configurable, bornée par `constants.h`
- **Par heure** : cumul injection ≤ `config.productHourlyLimit`
- **Par jour** : cumul injection ≤ `config.productDailyLimit`
- Compteurs persistants en LittleFS, atomiques (mutex)
- Reset quotidien à minuit local (pas UTC) — vérifier RTC
- Les commandes **manuelles** sont AUSSI soumises à ces limites

### I4 — Non-concurrence
- Une seule injection par produit à la fois (pH- OU chlore, pas les deux simultanément sur la même zone d'aspiration si l'installation le partage)
- Si un jour une pompe pH+ est ajoutée : pH- et pH+ antagonistes ne doivent JAMAIS tourner en parallèle
- Protection par flag + mutex

### I5 — Watchdog
- Le dosage n'est autorisé qu'avec `esp_task_wdt_status() == ESP_OK`
- Toute boucle de régulation : `esp_task_wdt_reset()` à chaque cycle
- En mode debug/test, si watchdog désactivé : **dosage auto désactivé**

### I6 — PID sain
- Anti-windup sur le terme intégral (clamp)
- Sortie clampée à `[0, kMaxPidOutput]`
- Deadband autour du setpoint (évite le pompage incessant)
- Période d'échantillonnage constante — pas de `Ts` variable
- Reset du PID en cas de redémarrage de la régulation (évite I/D incohérents)

### I7 — Ordre d'arrêt
- Au shutdown / restart / perte filtration : **couper le dosage immédiatement**
- En cas d'erreur critique capteur : **couper la régulation auto** (pas juste logger)

### I8 — Traçabilité
- Chaque injection logguée : `who` (auto/manuel + user), `when`, `product`, `duration_ms`, `reason`
- Exposé via MQTT et API `/api/pool/dosing/history`

## Checklist à appliquer à chaque diff

Pour tout code dans `src/pump_controller.*`, `src/sensors.*` (pH/ORP), régulation PID, calibration :

- [ ] Chaque point d'entrée vers l'injection vérifie I1 (filtration)
- [ ] Chaque point d'entrée vérifie I2 (capteur)
- [ ] Chaque point d'entrée vérifie I3 (limites) — y compris manuel
- [ ] I4 garanti (non-concurrence pompes, cf. supra)
- [ ] Watchdog vérifié (I5)
- [ ] Pas de régression sur le PID existant (I6)
- [ ] Chemin d'arrêt testé (I7)
- [ ] Logs + MQTT conformes (I8)
- [ ] Tests unitaires pour les cas bloquants (filtration OFF, capteur stale, limite atteinte)
- [ ] Rapport `pool-chemistry` avec verdict GO présent dans la spec

## Exemples de violations à bloquer

```cpp
// ❌ Dosage sans check filtration
void injectPhMinus(uint32_t durationMs) {
  pumpPhMinus.on();
  delay(durationMs);   // pire : bloque la loop
  pumpPhMinus.off();
}
```

```cpp
// ❌ PID sans anti-windup
integral += error * dt;     // diverge en cas de setpoint inatteignable
output = Kp*error + Ki*integral + Kd*derivative;
```

```cpp
// ❌ Compteur non persistant
static uint32_t dailyInjectionMs = 0;  // perdu au reboot
```

## Versions à jour

Cette skill doit être relue dès qu'une évolution touche :
- `src/constants.h` (section dosage)
- `src/pump_controller.*`
- `src/sensors.*` (partie pH/ORP)
- Nouvelle fonctionnalité de régulation

Mettre à jour la skill si les invariants évoluent ou si de nouveaux capteurs/produits sont ajoutés.
