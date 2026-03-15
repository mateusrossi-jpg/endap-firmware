# ENDAP
Edge Native Distributed Automation Platform

ENDAP is a deterministic distributed automation runtime designed for edge devices.

The system implements a PLC-like architecture with deterministic control loops,
industrial communication and distributed automation capabilities.

---

# Kernel

Control loop: **1 ms deterministic cycle**

Scheduler phases:

IO → Fieldbus → Automation → Events → Diagnostics

---

# Performance

Typical runtime measurements:

jitter(avg): ~28 µs  
execution(max): ~188 µs  

Cycle slack ≈ 80%

---

# Architecture

Layers:

Application  
Automation Engine  
Control Kernel  
Fieldbus Layer  
HAL  
Hardware

---

# Hardware

ESP32  
Xtensa LX6  
FreeRTOS  
ESP-IDF

---

# Project Status

ENDAP Kernel v1 — Stable

Next phase:

Cluster Architecture  
ENDAP Cluster Protocol (ECP)

---

# Author

Mateus Rossi
