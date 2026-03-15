# ENDAP — Edge Native Distributed Automation Platform

ENDAP is a deterministic distributed automation platform designed for edge devices.

## Features

- Deterministic 1 ms control loop
- Phase-based scheduler
- RS485 deterministic fieldbus
- Runtime determinism monitoring
- Watchdog supervision
- Static memory allocation (no heap in control loop)

## Architecture

Control Kernel phases:

IO → Fieldbus → Automation → Events → Diagnostics

## Hardware

ESP32  
Xtensa LX6  
FreeRTOS  
ESP-IDF

## Status

ENDAP Kernel v1 — Stable
