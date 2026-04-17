#ifndef WEB_ROUTES_CONTROL_H
#define WEB_ROUTES_CONTROL_H

#include <ESPAsyncWebServer.h>

// État d'une injection manuelle en cours
struct ManualInjectState {
  bool active = false;
  unsigned long startMs = 0;
  unsigned long durationMs = 0;
};

extern ManualInjectState manualInjectPh;
extern ManualInjectState manualInjectOrp;

// Retourne le temps restant en secondes (0 si inactif ou terminé)
int manualInjectRemainingS(const ManualInjectState& s);

// À appeler depuis la loop principale pour l'auto-stop
void updateManualInject();

// Déclarations des handlers pour les routes de contrôle
void setupControlRoutes(AsyncWebServer* server);

// Handler pour routes dynamiques des pompes (à appeler depuis onNotFound)
bool handleDynamicPumpRoutes(AsyncWebServerRequest* req);

#endif // WEB_ROUTES_CONTROL_H
