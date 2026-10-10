# ETS2DynamicHeadMovement 1.0
> *Dynamic In-Cabin Camera Movement for Euro Truck Simulator 2*

---

## Overview
Built using the logic from **ETS2HeadTracker 2.0**, **ETS2DynamicHeadMovement 1.0** was developed as an alternative to ETS2HeadTracker. It was created specifically to users who want dynamic, realistic head movement without operating external tracking hardware or software, as well as iOS users unable to run the Android client for **ETS2HeadTracker**. 

This mod ran directly into the game's native Telemetry SDK to extract camera movement data and apply realistic head-motion simulation.

---

## Features
* Simulates micro-movements and gentle cockpit shaking from the perspective of a real driver seated behind the wheel.
* Automatically glances at the left mirror, right mirror, and instrument cluster at randomized intervals (more like once every x seconds).
* Rotates camera view left and right based on the steering wheel input.
* Automatically checks the corresponding side mirror for a few seconds whenever blinkers are on.

---

## Installation
1. Place `ETS2DynamicHeadMovement.dll` into the `plugins` folder inside your ETS2 game installation directory (`bin/win_x64/plugins`).
2. Launch ETS2 and load into your profile.
3. **IMPORTANT:** In game options, **DO NOT ENABLE** *Steering Camera Rotation* and *Blinker Camera Rotation* (disable them to prevent input conflicts).
4. Interior head movement will be overridden by the mod's algorithmic logic.

---

## Requirements
* Requires **OpenTrack** installed on the user PC.
* Navigate to OpenTrack **Output** settings:
  * Select **facetrack 2.0 Enhanced** as the Output.
  * Set the dropdown mode next to Output settings to **Both**.
