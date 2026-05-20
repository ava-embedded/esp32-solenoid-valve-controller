# ESP32 Pneumatic Solenoid Valve Controller

Custom PCB-based solenoid valve control system developed during my 
internship at Grupo Modelo – Cervecería Yucateca (bottling department).

## Problem
The bottling line had recurring filling failures. The default response 
was replacing the entire pilot valve (~$19,000 MXN), even when the root 
cause was mechanical or just a flow sensor issue.

## Solution
Built a diagnostic and control system that allows technicians to:
- Test individual solenoid valves electrically vs mechanically
- Simulate filling phase combinations via a web interface
- Identify the exact fault before any replacement

**Potential savings: up to $12,660 MXN per fault event.**

## Hardware
- Custom PCB (designed in EasyEDA, manufactured)
- ESP32 microcontroller
- Pneumatic solenoid valves

## Features
- ESP32 hosted in AP mode — no router needed
- Web interface accessible from any device on the local network
- Configurable solenoid valve combinations to simulate filling phases

## Tech Stack
- Firmware: C++
- Web Interface: HTML / JavaScript
- PCB Design: EasyEDA

## Photos
[See /photos folder]

## Author
Adrian Valle Araujo  
Embedded Systems Engineering Student — Universidad Politécnica de Yucatán  
linkedin.com/in/adrianvalle-388920389
