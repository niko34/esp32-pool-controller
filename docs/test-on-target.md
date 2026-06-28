# Validation on-target — check-list manuelle

Ce document complète les **tests unitaires natifs** (`pio test -e native`, 118 tests, 100 % lignes sur la logique pure) en couvrant ce qu'ils ne peuvent **pas** atteindre : l'**intégration matériel + réseau** (I²C, PWM, RTC, LittleFS, WiFi, WebSocket, MQTT) et les **scénarios complets**.

> **Quand l'exécuter** : après chaque `./deploy.sh all` (ou `firmware`/`fs`) qui touche la régulation, la filtration, les capteurs, la persistance ou les contrats web/MQTT. Pas besoin de tout rejouer à chaque fois — cocher la section concernée par le changement + la section **Dosage** (sécurité) en cas de doute.

## Où observer
- **Journal système in-app** : interface web → Paramètres → carte **Logs** (bouton *Actualiser*). Active « Logs des sondes » / « Logs DEBUG » pour le détail.
- **Moniteur série** : `pio device monitor -b 115200`.
- **UI temps réel** : tableau de bord (badges, chips d'état) — alimenté par WebSocket.
- **MQTT** : un client MQTT (ou Home Assistant) abonné à `pool/#`.

Convention : `[ ]` à cocher, **gras** = message de log attendu (au mot près).

---

## 1. Démarrage & capteurs
- [ ] Boot sans crash ; log de version firmware = version attendue (`src/version.h`).
- [ ] Watchdog actif (pas de reset cyclique).
- [ ] pH / ORP / température affichés sur le tableau de bord, valeurs **plausibles** (pH ~6–8, ORP ~600–800 mV, temp cohérente).
- [ ] Chip d'état filtre = « Mesure stable » après le warmup (≈ 5 mesures).
- [ ] 2 sondes DS18B20 identifiées (eau + circuit) si applicable.
- [ ] Débrancher brièvement une sonde EZO → chip « EZO indisponible » ; rebrancher → retour normal.

## 2. Filtration (horaire)
- [ ] **Plage simple** (ex. 08:00–18:00) : filtration ON dans la plage, OFF hors plage.
- [ ] **Plage à cheval sur minuit** (ex. 22:00–06:00) : ON la nuit, OFF le jour. *(cas le plus piégeux — vérifie `isMinutesInRange` start>end)*
- [ ] **forceOn** pendant une plage OFF → filtration ON ; **forceOff** pendant une plage ON → OFF.
- [ ] Timeout des forçages : laisser un forçage actif > 4 h → log **« ForceOn expiré (timeout 4h) »** / retour mode normal.
- [ ] **Mode auto** : changer la température de référence → la plage se recalcule (durée ≈ temp/2, bornée 1 h–24 h), log **« Planning auto: …°C → HH:MM-HH:MM »**.
- [ ] Désactiver la filtration → relais OFF, carte dashboard filtration masquée.

## 3. Dosage pH / ORP automatique (sécurité — prioritaire)
> Cible pH > pH mesuré (pour pH+) ou < (pour pH-) ; filtration active.
- [ ] **Cycle complet** : log **« Démarrage dosage pH (auto): pH=… cible=… erreur=… »** PUIS, après ≥ 30 s, **« Arrêt dosage pH (auto): durée=≥30s vol≈…mL »**. *(valide minInjectionTimeMs + feature-037)*
- [ ] **Pause mélange** : juste après l'arrêt, log **« [Dosage pH] Refus : pause mélange en cours »** pendant 15 min (20 min pour ORP). *(valide le fix v2.2.5 : pause armée à l'arrêt, pas au démarrage)*
- [ ] **Proportionnalité** : grosse erreur (cible loin) → débit élevé (proche du max) ; petite erreur → débit moindre. *(feature-037)*
- [ ] **Garde présence d'eau** : couper la filtration en plein dosage → injection stoppée, log **« Refus : filtration arrêtée »** (sauf mode continu).
- [ ] **Garde calibration** : sonde non calibrée (pH < 2 points) → **« Refus : calibration insuffisante »**, pas d'injection.
- [ ] **Garde filtre** : capteur instable / filtre non prêt → **« Refus : filtre capteur non prêt »** ou **« capteur instable »**.
- [ ] **Anti-rafale** : provoquer plusieurs cycles rapprochés → au-delà de 6/min ou 20/15 min, refus anti-rafale. *(feature-039)*
- [ ] **Limite horaire** : atteindre la limite (déf. 5 min/h pH) → injection coupée, **« limite horaire atteinte »**.
- [ ] **Limite journalière** : vérifier le plafond (déf. 300 mL/j pH, 500 mL/j chlore) — quotas **non dépassés**.
- [ ] **pH+ vs pH-** : selon le type configuré, le sens de correction est correct (erreur = cible−pH en pH+, pH−cible en pH-).

## 4. Rollover journalier
- [ ] Au passage de **minuit (heure locale NTP)** : log **« Reset journalier (minuit local) — pH=…/… mL, ORP=…/… mL »** ; compteurs journaliers remis à 0. *(feature-039)*
- [ ] Sans NTP (RTC seul) : reset par fallback 24 h → **« Réinitialisation compteurs journaliers (fallback 24h) »**.

## 5. Calibration sondes
- [ ] Écran de calibration guidé accessible **dans tous les modes** (auto, programmé, manuel). *(feature-034)*
- [ ] Calibration pH (points 7.0 / 4.0) et ORP : détection de fin **robuste**, y compris en **recalibrant un point déjà calibré**.
- [ ] Après calibration réussie : **stabilisation post-cal** (dosage suspendu le temps prévu), chip de calibration mis à jour.
- [ ] Pendant une injection en cours, le bouton « Calibrer » est **désactivé**.

## 6. Éclairage
- [ ] Plage horaire (ex. 20:00–23:00) : ON le soir, OFF sinon.
- [ ] **Manuel ON/OFF** prioritaire sur l'horaire.
- [ ] Cas `start == end` configuré → éclairage **allumé toute la journée** (divergence voulue vs filtration). *(feature-040)*

## 7. Température (écran dédié)
- [ ] Valeur courante + chip de calibration affichés ; bouton « Calibrer la sonde ».
- [ ] Calibration par offset (température de référence connue → offset) ; chip passe à « Calibré · … ». *(feature-035)*
- [ ] Désactiver la sonde → carte dashboard masquée.

## 8. WebSocket / UI temps réel
- [ ] Tableau de bord se met à jour en direct (pH/ORP/temp, badges filtration/dosage) sans recharger.
- [ ] Badge « injection en cours » s'allume pendant le cycle pompe, s'éteint après.
- [ ] Chip d'état filtre exact (Stabilisation / Mesure stable / Pics rejetés / Capteur instable / EZO indisponible).
- [ ] `phDoseBlockedReason` / `orpDoseBlockedReason` reflètent la vraie cause de refus (modal détail du chip).
- [ ] Couper le WiFi puis le rétablir → la WS se reconnecte, données reprennent.

## 9. MQTT / Home Assistant
- [ ] Topics `pool/...` publiés (mesures, états).
- [ ] **Auto-discovery HA** : entités créées automatiquement (capteurs, switches).
- [ ] Une commande depuis HA (ex. forcer filtration) est prise en compte.
- [ ] Reconnexion MQTT après coupure réseau.

## 10. Persistance (survie au reboot)
- [ ] Modifier une config (cible pH, plage filtration, mode) → **rebooter** → la config est **conservée**.
- [ ] Compteurs journaliers de dosage **persistés** (NVS) : après reboot en cours de journée, le cumul n'est pas remis à 0 à tort.
- [ ] Historique : après quelques heures, les **moyennes horaires** apparaissent (consolidation) ; survit au reboot (LittleFS).

## 11. OTA
- [ ] `./deploy.sh ota-firmware` puis `ota-fs` : mise à jour réussie, reboot propre, version mise à jour.
- [ ] Pendant l'OTA, le dosage est suspendu (pas d'injection pendant le flash).

## 12. Robustesse / sécurité
- [ ] Perte NTP/RTC : la filtration **conserve son état** (pas de faux start/stop), dosage gardé par les sécurités.
- [ ] Bouton reset matériel : comportement attendu.
- [ ] Aucun reset watchdog en fonctionnement nominal prolongé.

---

## Lien avec les tests automatiques
| Niveau | Couvre | Statut |
|---|---|---|
| **Unitaire natif** (`pio test -e native`, CI) | logique pure : décision/PID/anti-rafale/rollover dosage, filtrage, horaires, agrégation | ✅ 100 % lignes, automatisé |
| **On-target** (ce document) | intégration matériel + réseau + scénarios complets | 🟡 manuel, à dérouler après deploy |

Les deux sont **complémentaires** : l'unitaire attrape les régressions de logique (cf. bug pause-mélange v2.2.5) ; l'on-target valide que la **coquille** (collecte des globals, I²C, PWM, persistance) câble correctement cette logique au matériel réel.
