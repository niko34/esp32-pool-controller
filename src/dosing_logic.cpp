#include "dosing_logic.h"
#include "constants.h"  // kCyclesPerDayMin/Max (self-contained, natif-safe)

// =============================================================================
// resolveWaterPresent — présence d'eau PURE (feature-056)
// =============================================================================
// Garde de sécurité chimique NON négociable : décide si le dosage est autorisé
// selon le mode d'installation. Fail-closed strict (condition pool-chemistry
// #2 : doute = refus — mode inconnu, signal externe inconnu, signal périmé →
// waterPresent = false). Testable en natif (aucun accès matériel).
WaterPresence resolveWaterPresent(const WaterPresenceInputs& in) {
  switch (in.mode) {
    case InstallMode::ManagedFiltration:
      // Eau présente ssi PoolController commande la filtration ON.
      return { in.filtrationCommandedOn, WaterSource::FiltrationCommanded, false };

    case InstallMode::PoweredByFiltration:
      // PC alimenté par la phase filtration : vivant ⟺ eau circule.
      return { true, WaterSource::PoweredAssumed, false };

    case InstallMode::ExternalFiltration: {
      // On teste la CONNAISSANCE du signal AVANT son âge : un signal jamais reçu
      // (boot) n'a pas d'âge signifiant → refus (fail-safe OFF).
      if (!in.externalSignalKnown) {
        return { false, WaterSource::ExternalSignal, false };
      }
      bool fresh = (in.externalSignalAgeMs <= in.externalStaleMs);
      // Périmé = connu mais trop ancien (signalé `stale` pour l'UI/diagnostic).
      bool stale = !fresh;
      bool present = in.externalSignalOn && fresh;
      return { present, WaterSource::ExternalSignal, stale };
    }
  }
  // Mode inconnu → fail-closed.
  return { false, WaterSource::ExternalSignal, false };
}

// Migration ancien schéma → InstallMode. filtrationEnabled est absorbé : la
// distinction « PC gère la filtration » ne crée jamais à elle seule le mode
// External (mode nouveau, choisi explicitement par l'installateur).
InstallMode migrateInstallMode(const char* regMode, bool filtrationEnabled) {
  (void)filtrationEnabled;
  if (regMode != nullptr && strcmp(regMode, "continu") == 0) {
    return InstallMode::PoweredByFiltration;
  }
  return InstallMode::ManagedFiltration;
}

// =============================================================================
// resolveFilterState — composition PURE de l'état de filtre (v2.25.1)
// =============================================================================
// ORDRE EXACT (identique à la priorité documentée dans dosing_logic.h — toute
// divergence entre les deux est un piège de maintenance) :
//   frozen → gateClosed → stale → warmup
// Pourquoi CET ordre-là :
//   1. `frozen` d'abord : une sonde figée RÉPOND (fenêtre pleine, lecture
//      fraîche) — aucun autre prédicat ne la trahit, donc rien ne doit la masquer.
//   2. `gateClosed` AVANT `stale` : porte fermée, l'âge de la dernière mesure
//      acceptée croît PAR CONCEPTION (le filtre n'ingère plus rien). Annoncer
//      « périmé » ferait de chaque arrêt de circulation une panne de sonde
//      déclarée — puce rouge et alerte MQTT à chaque cycle de filtration.
//   3. `stale` avant `warmup` : une sonde muette a aussi la fenêtre vide ; si le
//      warmup primait, une panne réelle s'afficherait « en stabilisation »
//      indéfiniment — exactement le défaut corrigé en v2.25.1.
// Ne décide RIEN du dosage : sert uniquement à nommer la cause du refus.
DoseFilterState resolveFilterState(bool frozen, bool gateClosed, bool stale,
                                   bool warmingUp) {
  if (frozen) return DoseFilterState::Frozen;
  if (gateClosed) return DoseFilterState::GateClosed;
  if (stale) return DoseFilterState::Stale;
  if (warmingUp) return DoseFilterState::Warmup;
  return DoseFilterState::Ready;
}

// -----------------------------------------------------------------------------
// Qualification de la cause de la garde 3 (v2.25.1) — OBSERVABILITÉ SEULE
// -----------------------------------------------------------------------------
// Ce helper ne prend AUCUNE décision : il est appelé DANS la branche déjà
// prise par la garde et ne fait que choisir le libellé/code rendu. Le verdict
// (`allowed = false`) et l'ordre des gardes sont intouchés.
// `Unknown` et `Ready` (état incohérent avec le refus) → cause GÉNÉRIQUE : on
// n'invente jamais une cause bénigne.
static DoseRefusal qualifyReadingNaN(DoseFilterState st) {
  switch (st) {
    case DoseFilterState::Frozen:     return DoseRefusal::ReadingNaNFrozen;
    case DoseFilterState::Stale:      return DoseRefusal::ReadingNaNStale;
    case DoseFilterState::GateClosed: return DoseRefusal::ReadingNaNGateClosed;
    case DoseFilterState::Warmup:     return DoseRefusal::ReadingNaNWarmup;
    case DoseFilterState::Ready:
    case DoseFilterState::Unknown:
      break;
  }
  return DoseRefusal::ReadingNaN;
}

// feature-152 : qualifyFilterNotReady() a disparu — la garde 3b qu'elle
// qualifiait (« filtre non prêt ») n'existe plus. La VALEUR OFFICIELLE de
// sensor_filter est désormais NaN dans tous les cas qu'elle couvrait (warmup,
// figé, périmé, porte fermée) : qualifyReadingNaN() ci-dessus suffit seule.

// =============================================================================
// evaluateDose — décision PURE, ordre des gardes identique à canDose().
// =============================================================================
// IMPORTANT (pool-chemistry, feature-036) : NE PAS réordonner, NE PAS fusionner,
// NE PAS court-circuiter les gardes. L'ordre et les seuils comparatifs doivent
// rester strictement identiques au canDose() d'origine (characterization refactor).
DoseDecision evaluateDose(const DoseInputs& in) {
  // 1. Watchdog actif (le plus critique).
  if (!in.watchdogActive) {
    return { false, DoseRefusal::WatchdogInactive };
  }

  // 2. Présence d'eau (feature-056) : source UNIQUE resolveWaterPresent(), qui
  // encapsule les 3 modes d'installation (Managed=commandé, Powered=présumé,
  // External=signal frais). Doute = refus (fail-closed strict).
  if (!in.waterPresent) {
    return { false, DoseRefusal::FiltrationOff };
  }

  // 3. Valeur officielle non NaN (fail-closed). feature-152 : FUSION des
  // anciennes gardes 3/3b/3c — la valeur officielle de sensor_filter est déjà
  // NaN dans tous les cas qui bloquaient séparément auparavant (fenêtre
  // incomplète, sonde figée, mesure périmée, porte fermée) ; la médiane
  // écarte déjà les aberrations par construction, il n'y a donc plus de
  // drapeau d'instabilité distinct à tester. Seule la cause rendue est
  // qualifiée par l'état de filtre reçu en ENTRÉE.
  if (isnan(in.reading)) {
    return { false, qualifyReadingNaN(in.filterState) };
  }

  // 4. Calibration suffisante (pH ≥ 2 points, ORP ≥ 1 ; -1 si injoignable → bloqué).
  if (in.calPoints < in.requiredPoints) {
    return { false, DoseRefusal::CalibrationInsufficient };
  }

  // 5. Pas de stabilisation post-calibration en cours.
  if (in.stabilizationActive) {
    return { false, DoseRefusal::StabilizationActive };
  }

  // 5b. Pas de pause mélange hydraulique active (gate indépendante).
  if (in.mixingActive) {
    return { false, DoseRefusal::MixingActive };
  }

  // 6. Mode régulation = automatic.
  if (!in.modeAutomatic) {
    return { false, DoseRefusal::ModeNotAutomatic };
  }

  // 7. Limite journalière non atteinte.
  if (in.dailyInjectedMl >= in.maxDailyMl) {
    return { false, DoseRefusal::DailyLimit };
  }

  // 8. Limite horaire non atteinte (0 = pas de limite).
  if (in.hourlyLimitMs > 0 && in.usedMs >= in.hourlyLimitMs) {
    return { false, DoseRefusal::HourlyLimit };
  }

  // 9. Anti-cycling : cyclesToday < maxCyclesPerDay.
  if (in.cyclesToday >= in.maxCyclesPerDay) {
    return { false, DoseRefusal::CyclesPerDay };
  }

  // 10. Anti-rafale court terme (fenêtre 1 min).
  if (in.cyclesLastMin >= in.maxCyclesPerMin) {
    return { false, DoseRefusal::BurstPerMinute };
  }

  // 10b. Anti-rafale court terme (fenêtre 15 min).
  if (in.cyclesLast15Min >= in.maxCyclesPer15Min) {
    return { false, DoseRefusal::BurstPer15Min };
  }

  // Toutes les gardes sont passées.
  return { true, DoseRefusal::None };
}

bool shouldStartDosingPure(float error, float startThreshold,
                           unsigned int cyclesToday, unsigned int maxCyclesPerDay) {
  // 1. Nombre de cycles par jour (le warning éventuel reste dans la coquille).
  if (cyclesToday >= maxCyclesPerDay) {
    return false;
  }
  // 2. Seuil de démarrage (hystérésis).
  return error > startThreshold;
}

bool shouldContinueDosingPure(float error, float stopThreshold,
                              unsigned long runTimeMs, unsigned long minInjectionTimeMs) {
  // 1. Forcer le temps minimum d'injection.
  if (runTimeMs < minInjectionTimeMs) {
    return true;
  }
  // 2. Continuer si au-dessus du seuil d'arrêt (hystérésis).
  return error > stopThreshold;
}

CycleStartAccounting evaluateCycleStartAccounting(bool wasActive, bool startDecided,
                                                  bool flowActive) {
  CycleStartAccounting out;
  // Un cycle n'est comptabilisé qu'à la transition inactive → actif EFFECTIF :
  // même prédicat que le latch, donc jamais deux comptages pour un démarrage.
  out.countCycleStart = !wasActive && startDecided && flowActive;
  return out;
}

// feature-110 — jumeau du précédent, un cran plus bas : on compte les
// DÉMARRAGES PHYSIQUES de pompe, seul ensemble d'événements homogène au volume
// crédité (voir le bloc de doc de dosing_logic.h). Aucune garde de dosage ici :
// cette fonction n'autorise rien, elle observe.
PumpStartAccounting evaluatePumpStartAccounting(bool prevOn, bool onNow,
                                                uint32_t creditedDeltaMs,
                                                uint32_t anomalyDeltaMs) {
  PumpStartAccounting out;
  // Front strict OFF→ON : une session `scheduled` en marche continue ne produit
  // donc qu'UN démarrage, et un start/stop manuel logé entre deux ticks
  // n'en produit AUCUN — mais il ne crédite aucun volume non plus, la paire
  // (N, V) reste cohérente.
  out.countStart = (!prevOn && onNow);
  // Seuil 0 = indicateur désactivé (aucune anomalie possible). Frontière >
  // STRICTE : un delta pile au seuil reste normal.
  out.countAnomaly = (anomalyDeltaMs != 0u) && (creditedDeltaMs > anomalyDeltaMs);
  return out;
}

uint32_t saturatingInc(uint32_t v) {
  // Plafond DUR : jamais de repli à 0 (cf. dosing_logic.h).
  return (v == UINT32_MAX) ? v : (uint32_t)(v + 1u);
}

// =============================================================================
// Sanitization config (feature-060) — HORS des gardes de dosage
// =============================================================================
// Borne maxCyclesPerDay dans [kCyclesPerDayMin, kCyclesPerDayMax] (verdict
// pool-chemistry 2026-07-29 : clamp serveur + chargement NVS). Ce n'est PAS
// une garde de dosage : l'ordre des gardes 2→15 d'evaluateDose() est intouché.
unsigned int clampCyclesPerDay(long requested, bool* wasClamped) {
  unsigned int clamped;
  if (requested < static_cast<long>(kCyclesPerDayMin)) {
    clamped = kCyclesPerDayMin;
  } else if (requested > static_cast<long>(kCyclesPerDayMax)) {
    clamped = kCyclesPerDayMax;
  } else {
    clamped = static_cast<unsigned int>(requested);
  }
  if (wasClamped != nullptr) {
    *wasClamped = (requested != static_cast<long>(clamped));
  }
  return clamped;
}

// Clamp maison : Arduino::constrain est une macro indisponible en natif.
static inline float clampf(float v, float lo, float hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}

PidResult computePidPure(float kp, float ki, float kd,
                         float error, float prevError, float integral,
                         float dtSec, float integralMax, float deadband,
                         float minFlow, float maxFlow, bool freezeIntegral) {
  // feature-025 (B7) : zone morte = seuil de démarrage (deadband). STRICT < :
  // |erreur| < deadband → sortie 0 ET aucune accumulation intégrale.
  bool inDeadband = (fabsf(error) < deadband);

  if (inDeadband) {
    // Sortie nulle dans la zone morte ; intégrale NON modifiée.
    return { 0.0f, integral, error };
  }

  // feature-025 (B6) : anti-windup strict — geler l'accumulation si la coquille
  // le signale (saturation, filtre non prêt, etc.) ou si on est dans la deadband
  // (déjà traité ci-dessus).
  bool allowIntegration = !freezeIntegral;

  // Calcul PID
  float proportional = kp * error;

  if (allowIntegration) {
    integral += error * dtSec;
    // Anti-windup (borne intégrale : clamp haut ET bas)
    if (integral > integralMax) integral = integralMax;
    if (integral < -integralMax) integral = -integralMax;
  }
  float integralTerm = ki * integral;

  float derivative = kd * (error - prevError) / dtSec;

  float output = proportional + integralTerm + derivative;
  if (output < 0.0f) output = 0.0f;

  // feature-037 (Option Y) : bornage final déplacé ici depuis la coquille.
  float flow = clampf(output, minFlow, maxFlow);

  return { flow, integral, error };
}

// =============================================================================
// feature-092 — Composition du débit demandé (observabilité)
// =============================================================================
// MIROIR LITTÉRAL du tick pH : l'ordre des six étapes ci-dessous n'est PAS
// réordonnable (cf. l'en-tête de dosing_logic.h). En particulier, la borne basse
// à zéro porte sur le COUPLE (p + i) et jamais sur la somme des trois termes.
DoseComposition doseComposition(float kp, float error, float kiIntegral,
                                float deadband, float ffServed, float ampFlow,
                                DoseFlowRegime regime) {
  DoseComposition c;

  // --- 1. FAIL-CLOSED : sans erreur exploitable, il n'y a pas de composition --
  // On rend NAN (→ « — » côté interface) plutôt qu'un 0 qui se lirait « pH à la
  // consigne » sur une mesure absente.
  if (!isfinite(error)) {
    c.p = c.i = c.piServed = c.ff = c.demand = NAN;
    c.inDeadband = false;
    return c;
  }

  // --- 2. BANDE MORTE, comparaison STRICTE (comme computePidPure) ------------
  c.inDeadband = (fabsf(error) < deadband);

  // --- 3. TERMES p ET i : BRUTS, SIGNÉS, JAMAIS BORNÉS -----------------------
  // Ce sont les grandeurs pédagogiques de la carte (« écart courant »,
  // « rattrapage accumulé »). Dans la bande morte, computePidPure rend 0 et
  // n'accumule rien : les deux termes valent donc 0, exactement.
  c.p = c.inDeadband ? 0.0f : (kp * error);
  c.i = c.inDeadband ? 0.0f : kiIntegral;

  // --- 4. RÉGIME : ce que le FF apporte RÉELLEMENT ce tour --------------------
  // `Zero` (bassin sous la consigne pendant une prolongation de session) coupe
  // le FF ; `CappedToFeedforward` et `Full` le servent. p et i restent BRUTS
  // dans tous les régimes — le régime borne ce qui est SERVI, pas ce qui est
  // mesuré.
  c.ff = (regime == DoseFlowRegime::Zero) ? 0.0f : ffServed;

  // --- 5. PI SERVI : borné dans [0 ; amplitude restante après le FF] ---------
  // C'EST L'INVARIANT À NE PAS DÉPLACER (INV-C2). Le tick calcule
  // `phPiMaxFlow = ampFlow − ff` (borné ≥ 0) puis borne le PI dans
  // [0 ; phPiMaxFlow] AVANT d'ajouter le FF.
  float piRoom = ampFlow - c.ff;
  if (!(piRoom > 0.0f)) piRoom = 0.0f;  // NAN ou négatif ⇒ aucune marge
  c.piServed = clampf(c.p + c.i, 0.0f, piRoom);
  // Les deux régimes de prolongation annulent le PI : `CappedToFeedforward`
  // interdit que PI + FF dépasse le FF SEUL, `Zero` coupe tout.
  if (regime != DoseFlowRegime::Full) c.piServed = 0.0f;

  // --- 6. DEMANDE : somme, clampée à l'amplitude (défense en profondeur) -----
  c.demand = clampf(c.piServed + c.ff, 0.0f, ampFlow);
  return c;
}

// =============================================================================
// Anti-rafale + rollover journalier PURS (feature-039)
// =============================================================================

int countCyclesInWindow(const uint32_t* history, size_t size, uint32_t now, uint32_t windowMs) {
  int count = 0;
  for (size_t i = 0; i < size; ++i) {
    uint32_t ts = history[i];
    // ts == 0 = slot vide (jamais utilisé). On ignore.
    // Cas wrap millis() (49.7 jours) : (now - ts) déborde correctement en
    // arithmétique non-signée → la fenêtre reste cohérente au passage 0xFFFFFFFF.
    // frontière <= inclusive volontaire.
    if (ts != 0 && (now - ts) <= windowMs) count++;
  }
  return count;
}

size_t recordCycleTimestamp(uint32_t* history, size_t idx, size_t size, uint32_t now) {
  history[idx] = now;
  return (idx + 1) % size;
}

bool shouldRolloverByDate(const char* currentDayDate, const char* todayStr) {
  return strlen(currentDayDate) > 0 && strcmp(currentDayDate, todayStr) != 0;
}

bool shouldRolloverByMillis(uint32_t dayStartMs, uint32_t now) {
  // frontière >= inclusive volontaire ; wrap millis uint32 (arithmétique non signée).
  return (now - dayStartMs) >= 86400000UL;
}

// bug-161 (gate C1) : verrou de complétude/numérotation de ManualInjectRefusal
// — même motif que les static_assert DoseRefusal de pump_controller.cpp.
// Cet énum n'est pas sérialisé en brut sur le fil (traduit par un switch →
// code JSON string dans web_routes_control.cpp), mais le RANG D'ÉVALUATION
// dans evaluateManualInject() reste un contrat gelé pool-chemistry : ne
// jamais renuméroter, ne jamais insérer au milieu, toujours ajouter à la fin.
static_assert(static_cast<int>(ManualInjectRefusal::None) == 0, "ManualInjectRefusal renuméroté");
static_assert(static_cast<int>(ManualInjectRefusal::WatchdogInactive) == 1, "ManualInjectRefusal renuméroté");
static_assert(static_cast<int>(ManualInjectRefusal::FiltrationOff) == 2, "ManualInjectRefusal renuméroté");
static_assert(static_cast<int>(ManualInjectRefusal::StabilizationActive) == 3, "ManualInjectRefusal renuméroté");
static_assert(static_cast<int>(ManualInjectRefusal::AlreadyInjecting) == 4, "ManualInjectRefusal renuméroté");
static_assert(static_cast<int>(ManualInjectRefusal::DailyLimit) == 5, "ManualInjectRefusal renuméroté");
static_assert(static_cast<int>(ManualInjectRefusal::HourlyLimit) == 6, "ManualInjectRefusal renuméroté");
static_assert(static_cast<int>(ManualInjectRefusal::MaxCyclesPerDay) == 7, "ManualInjectRefusal renuméroté");
static_assert(static_cast<int>(ManualInjectRefusal::BurstPerMinute) == 8, "ManualInjectRefusal renuméroté");
static_assert(static_cast<int>(ManualInjectRefusal::BurstPer15Min) == 9, "ManualInjectRefusal renuméroté");
static_assert(static_cast<int>(ManualInjectRefusal::AutomaticInjectInProgress) == 10, "ManualInjectRefusal renuméroté");

// =============================================================================
// evaluateManualInject — gardes d'injection manuelle PURES (feature-006)
// =============================================================================
// ORDRE EXACT validé pool-chemistry (GO sous conditions, condition #1 :
// watchdog EN PREMIER). NE PAS réordonner, NE PAS fusionner les gardes.
ManualInjectDecision evaluateManualInject(const ManualInjectInputs& in) {
  // Reliquat journalier informatif : plancher 0 (jamais négatif).
  float remaining = in.maxDailyMl - in.dailyInjectedMl;
  if (remaining < 0.0f) remaining = 0.0f;

  // 1. Watchdog actif (règle absolue : aucun dosage sans watchdog).
  if (!in.watchdogActive) {
    return { false, ManualInjectRefusal::WatchdogInactive, 0.0f };
  }

  // 2. Présence d'eau (feature-056) : source UNIQUE resolveWaterPresent(),
  // résolue côté collecte selon le mode d'installation. Doute = refus.
  if (!in.waterPresent) {
    return { false, ManualInjectRefusal::FiltrationOff, 0.0f };
  }

  // 3. Pas de stabilisation post-calibration en cours (par pompe).
  if (in.stabilizationActive) {
    return { false, ManualInjectRefusal::StabilizationActive, 0.0f };
  }

  // (ex-rang 4, CalibrationHold) — RETIRÉ bug-160 (volet 2, REFONTE
  // 2026-10-13) : le mécanisme de bail qui le justifiait est ABANDONNÉ. Une
  // calibration en cours bascule désormais la régulation en "manual" (mqttCfg,
  // posé par web_routes_calibration.cpp), et interrompt toute injection
  // manuelle ALREADY ACTIVE directement à l'entrée (gate 5, C6) — ce n'est plus
  // une garde évaluée ici à chaque nouvelle demande.

  // 4. bug-161 (gate pool-chemistry C1) : pas d'injection AUTOMATIQUE en cours
  // sur cette pompe. Le bouton d'injection manuelle était masqué hors du mode
  // `manual` (app.js, updatePhModeControls/updateOrpModeControls) — ce trou
  // était donc INATTEIGNABLE. Ce lot lève ce masquage (l'injection manuelle
  // n'est plus conditionnée au mode) : SANS cette garde, on pourrait injecter
  // à la main PAR-DESSUS un dosage automatique en cours. ⚠️ ORDRE LOAD-BEARING :
  // reste DERRIÈRE Watchdog (1) et FiltrationOff (2) — une cause bénigne ne
  // doit jamais masquer une cause absolue — et DEVANT les quotas (5-10,
  // ADR-0095 : la cause annoncée doit être celle qui mord). Place AVANT
  // AlreadyInjecting (double start MANUEL, question distincte).
  if (in.automaticInjectInProgress) {
    return { false, ManualInjectRefusal::AutomaticInjectInProgress, 0.0f };
  }

  // 5. Pas de double démarrage (une injection manuelle est déjà en cours).
  if (in.alreadyInjecting) {
    return { false, ManualInjectRefusal::AlreadyInjecting, 0.0f };
  }

  // 6. Limite journalière PRÉDICTIVE : refuser si le cumul + volume demandé
  // DÉPASSERAIT la limite. Frontière `cumul + volume == max` ACCEPTÉE
  // (STRICT >) — frontière figée pool-chemistry feature-006. maxDailyMl ≤ 0
  // = illimité (pas de garde journalière).
  if (in.maxDailyMl > 0.0f && (in.dailyInjectedMl + in.requestedMl > in.maxDailyMl)) {
    return { false, ManualInjectRefusal::DailyLimit, remaining };
  }

  // 7. Limite horaire PRÉDICTIVE : mêmes conventions que l'auto (0 = illimité) ;
  // frontière `usedMs + durée == limite` ACCEPTÉE (STRICT >) — figée
  // pool-chemistry feature-006. Budget usedMs PARTAGÉ avec l'auto.
  if (in.hourlyLimitMs > 0 && (in.usedMs + in.requestedDurationMs > in.hourlyLimitMs)) {
    return { false, ManualInjectRefusal::HourlyLimit, 0.0f };
  }

  // 8. Anti-cycling journalier : frontière >= (à la limite → refus), identique
  // à la garde 9 d'evaluateDose (compteur partagé auto + manuel).
  if (in.cyclesToday >= in.maxCyclesPerDay) {
    return { false, ManualInjectRefusal::MaxCyclesPerDay, 0.0f };
  }

  // 9. Anti-rafale fenêtre 1 min : frontière >= (identique à l'auto, ring partagé).
  if (in.cyclesLastMin >= in.maxCyclesPerMin) {
    return { false, ManualInjectRefusal::BurstPerMinute, 0.0f };
  }

  // 10. Anti-rafale fenêtre 15 min : frontière >= (identique à l'auto, ring partagé).
  if (in.cyclesLast15Min >= in.maxCyclesPer15Min) {
    return { false, ManualInjectRefusal::BurstPer15Min, 0.0f };
  }

  // Toutes les gardes sont passées : injection autorisée, reliquat renseigné.
  return { true, ManualInjectRefusal::None, remaining };
}

// =============================================================================
// evaluateScheduledDose — répartition scheduled PURE (feature-011)
// =============================================================================
// Conditions pool-chemistry n°2/3/4/5 documentées dans dosing_logic.h.
// NE PAS réordonner les gardes ni relâcher les clamps.
ScheduledDoseDecision evaluateScheduledDose(const ScheduledDoseInputs& in) {
  // Fail-closed (condition n°4 watchdog ; condition n°2 horizon) : watchdog
  // inactif, hors plage / horizon nul, débit non configuré, ou fenêtre invalide
  // (garde défensive anti division par zéro) → aucune injection.
  // windowIndex=-1 force le recalcul du volume au retour en plage.
  if (!in.watchdogActive || in.horizonMinutes <= 0 ||
      in.effectiveFlowMlPerMin <= 0.0f || in.windowMinutes <= 0) {
    return { false, -1, 0.0f, NAN };
  }

  // Cible journalière effective : plafonnée par la limite de sécurité (si > 0).
  float effective = in.dailyTargetMl;
  if (in.maxDailyMl > 0.0f && effective > in.maxDailyMl) {
    effective = in.maxDailyMl;
  }

  // Volume restant à répartir sur la journée (clamp >= 0 — condition n°3).
  float remaining = effective - in.dailyInjectedMl;
  if (remaining < 0.0f) remaining = 0.0f;

  // Index de fenêtre absolu dans la journée (0..95 pour des fenêtres de 15 min).
  int windowIndex = in.nowMin / in.windowMinutes;

  float stopTargetMl;
  if (windowIndex != in.prevWindowIndex) {
    // Nouvelle fenêtre : recalcul du volume de fenêtre depuis l'état courant.
    // floor volontaire ; plancher nWin >= 1 SÛR : horizonMinutes > 0 est garanti
    // par la garde d'entrée (condition n°2 — jamais nWin=1 via max(1,0)).
    int nWin = in.horizonMinutes / in.windowMinutes;
    if (nWin < 1) nWin = 1;
    float v = remaining / static_cast<float>(nWin);

    // Borne budget horaire (ADR-0020, budget PARTAGÉ auto+manuel+test+UART) :
    // le volume de fenêtre est converti en temps via le débit effectif ;
    // l'excédent se reporte mécaniquement aux fenêtres suivantes.
    if (in.hourlyLimitMs > 0) {
      float budgetMl = (in.usedMs >= in.hourlyLimitMs)
          ? 0.0f
          : (static_cast<float>(in.hourlyLimitMs - in.usedMs) / 60000.0f) *
                in.effectiveFlowMlPerMin;
      if (v > budgetMl) v = budgetMl;
    }
    if (v < 0.0f) v = 0.0f;  // clamp >= 0 (condition n°3)

    // Report anti short-cycling : si la durée d'injection correspondante est
    // < minInjectionTimeMs, rien dans CETTE fenêtre — le volume se reporte
    // (remaining inchangé, moins de fenêtres restantes → doses plus longues).
    if (v > 0.0f &&
        (v / in.effectiveFlowMlPerMin) * 60000.0f <
            static_cast<float>(in.minInjectionTimeMs)) {
      v = 0.0f;
    }
    stopTargetMl = in.dailyInjectedMl + v;
  } else {
    // Même fenêtre : la cible d'arrêt cumulée reste celle calculée à l'entrée
    // de la fenêtre (le cumul progresse vers elle pendant l'injection).
    stopTargetMl = in.prevStopTargetMl;
  }

  // Débit moyen planifié restant (diagnostic WS) : NAN quand plus rien à
  // injecter (quota atteint). horizonMinutes > 0 garanti par la garde d'entrée.
  float plannedFlow = (remaining > 0.0f)
      ? remaining / static_cast<float>(in.horizonMinutes)
      : NAN;

  // Condition n°3 : doseNow réévalué à CHAQUE tick contre
  // min(stopTargetMl, effective) — une baisse de cible ou de plafond en cours
  // de fenêtre arrête l'injection immédiatement, sans attendre la fenêtre
  // suivante.
  float stopCap = (stopTargetMl < effective) ? stopTargetMl : effective;
  bool doseNow = in.dailyInjectedMl < stopCap;

  return { doseNow, windowIndex, stopTargetMl, plannedFlow };
}

// =============================================================================
// Mode Boost — valeurs effectives PURES (feature-053)
// =============================================================================
// Effet STRICTEMENT réservé au mode ORP "automatic" (condition pool-chemistry #1).
float effectiveOrpTargetPure(float orpTarget, bool boostActive, bool orpModeAutomatic,
                             float deltaMv, float ceilingMv) {
  if (!boostActive || !orpModeAutomatic) {
    return orpTarget;
  }
  // fminf plafonne la cible relevée ; le max() garantit que la cible n'est
  // JAMAIS abaissée si elle dépasse déjà le plafond (condition #5).
  float raised = fminf(orpTarget + deltaMv, ceilingMv);
  return orpTarget > raised ? orpTarget : raised;
}

float effectiveMaxChlorinePure(float maxChlorine, bool boostActive, bool orpModeAutomatic,
                               float factor, float hardCapMl) {
  if (!boostActive || !orpModeAutomatic) {
    return maxChlorine;
  }
  // Limite journalière relevée × facteur, bornée en dur (condition #1).
  return fminf(maxChlorine * factor, hardCapMl);
}
