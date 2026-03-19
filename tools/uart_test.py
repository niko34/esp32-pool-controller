#!/usr/bin/env python3
"""
Script de test interactif pour le protocole UART du pool controller.

Usage:
    python3 uart_test.py [PORT] [BAUD]

Exemples:
    python3 uart_test.py /dev/ttyUSB0
    python3 uart_test.py COM3 115200

Dépendances:
    pip install pyserial

En mode interactif :
    - Tapez une commande JSON et appuyez sur Entrée pour l'envoyer
    - Les réponses et événements asynchrones s'affichent automatiquement
    - Commandes prédéfinies disponibles (tapez leur numéro) :
        1  ping
        2  get_info
        3  get_status
        4  get_config
        5  get_alarms
        6  get_network_status
        7  set_config ph_target 7.3
        8  run_action filtration_mode auto
        q  quitter
"""

import sys
import json
import threading
import time

try:
    import serial
except ImportError:
    print("Erreur: pyserial non installé. Lancez: pip install pyserial")
    sys.exit(1)

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------

DEFAULT_PORT = "/dev/tty.usbserial-0001"
DEFAULT_BAUD = 115200

PRESETS = {
    "1": {"cmd": "ping"},
    "2": {"cmd": "get_info"},
    "3": {"cmd": "get_status"},
    "4": {"cmd": "get_config"},
    "5": {"cmd": "get_alarms"},
    "6": {"cmd": "get_network_status"},
    "7": {"cmd": "set_config", "data": {"ph_target": 7.3}},
    "8": {"cmd": "run_action", "data": {"action": "filtration_mode", "mode": "auto"}},
}

# ---------------------------------------------------------------------------
# Réception asynchrone
# ---------------------------------------------------------------------------

def reader_thread(ser, stop_event):
    buf = ""
    while not stop_event.is_set():
        try:
            data = ser.read(ser.in_waiting or 1)
            if data:
                buf += data.decode("utf-8", errors="replace")
                while "\n" in buf:
                    line, buf = buf.split("\n", 1)
                    line = line.strip()
                    if line:
                        try:
                            obj = json.loads(line)
                            pretty = json.dumps(obj, ensure_ascii=False, indent=2)
                            msg_type = obj.get("type", "?")
                            color = {
                                "pong": "\033[92m",
                                "ack": "\033[92m",
                                "error": "\033[91m",
                                "alarm": "\033[93m",
                                "event": "\033[96m",
                            }.get(msg_type, "\033[0m")
                            print(f"\n{color}← {pretty}\033[0m\n> ", end="", flush=True)
                        except json.JSONDecodeError:
                            print(f"\n\033[90m← (raw) {line}\033[0m\n> ", end="", flush=True)
        except Exception:
            if not stop_event.is_set():
                time.sleep(0.01)

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def send(ser, obj):
    line = json.dumps(obj, separators=(",", ":")) + "\n"
    print(f"\033[94m→ {line.strip()}\033[0m")
    ser.write(line.encode("utf-8"))


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_PORT
    baud = int(sys.argv[2]) if len(sys.argv) > 2 else DEFAULT_BAUD

    print(f"Connexion UART: {port} @ {baud} bps")
    try:
        ser = serial.Serial(port, baud, timeout=0.1)
    except serial.SerialException as e:
        print(f"Erreur: {e}")
        sys.exit(1)

    print("Connecté. Tapez un numéro de commande prédéfinie, du JSON brut, ou 'q' pour quitter.")
    print("Commandes: 1=ping 2=get_info 3=get_status 4=get_config 5=get_alarms 6=get_network_status")
    print("           7=set_config(ph) 8=filtration_mode(auto)\n")

    stop_event = threading.Event()
    t = threading.Thread(target=reader_thread, args=(ser, stop_event), daemon=True)
    t.start()

    try:
        while True:
            try:
                user_input = input("> ").strip()
            except EOFError:
                break

            if not user_input:
                continue

            if user_input.lower() in ("q", "quit", "exit"):
                break

            if user_input in PRESETS:
                send(ser, PRESETS[user_input])
                continue

            # Essayer de parser comme JSON
            try:
                obj = json.loads(user_input)
                send(ser, obj)
            except json.JSONDecodeError:
                print(f"JSON invalide: {user_input}")

    except KeyboardInterrupt:
        pass

    stop_event.set()
    ser.close()
    print("\nDéconnecté.")


if __name__ == "__main__":
    main()
