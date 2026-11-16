#ifndef NATIVE_FAKE_ESP_TASK_WDT_H
#define NATIVE_FAKE_ESP_TASK_WDT_H
// Faux esp_task_wdt (D3-b2, C5) : no-op.
inline int esp_task_wdt_reset() { return 0; }
#endif
