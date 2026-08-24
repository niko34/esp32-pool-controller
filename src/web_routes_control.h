#ifndef WEB_ROUTES_CONTROL_H
#define WEB_ROUTES_CONTROL_H

#include <ESPAsyncWebServer.h>

// État d'une injection manuelle en cours
struct ManualInjectState {
  bool active = false;
  unsigned long startMs = 0;
  unsigned long durationMs = 0;
  float requestedVolumeMl = 0.0f; // Volume demandé (0 si non renseigné)
  // Crédit plancher fin d'injection (bug sous-compte v2.9.1) :
  // startCumulMl = cumul journalier au démarrage accepté ; creditMl = volume
  // effectif promis (0 = pas de crédit dû). Consommés à la fin NATURELLE
  // uniquement (jamais sur stop manuel / interruption filtration).
  float startCumulMl = 0.0f;
  float creditMl = 0.0f;
};

extern ManualInjectState manualInjectPh;
extern ManualInjectState manualInjectOrp;

// feature-075 : instantané LECTURE SEULE de la session « assistant de mesure du
// débit » pour GET /data (resynchro du compte à rebours client — pas de champ WS).
// `phase` pointe sur un littéral statique ("purge"/"measure", "" si aucune marche).
struct FlowAssistStatus {
  bool active = false;   // session ouverte (dosage auto suspendu)
  int pumpNumber = 0;    // pompe PHYSIQUE en assistant (1|2, 0 si aucune session)
  const char* phase = "";// phase de la marche EN COURS ("" si pompe à l'arrêt)
  int remainingS = 0;    // secondes restantes de la marche en cours (0 sinon)
  int purgesLeft = 0;    // purges restantes de la session (max si aucune session)
  // Revue feature-075 : la DERNIÈRE marche a été interrompue par le SERVEUR
  // (perte de filtration). Consommé par l'UI à l'échéance du compte à rebours :
  // une mesure avortée ne doit JAMAIS mener à la saisie du volume (débit
  // sous-estimé → sur-injection systématique). Réarmé à false au start suivant.
  bool aborted = false;
};
FlowAssistStatus getFlowAssistStatus();

// Retourne le temps restant en secondes (0 si inactif ou terminé)
int manualInjectRemainingS(const ManualInjectState& s);

// À appeler depuis la loop principale pour l'auto-stop
void updateManualInject();

// Déclarations des handlers pour les routes de contrôle
void setupControlRoutes(AsyncWebServer* server);

// Handler pour routes dynamiques des pompes (à appeler depuis onNotFound)
bool handleDynamicPumpRoutes(AsyncWebServerRequest* req);

#endif // WEB_ROUTES_CONTROL_H
