# ETS2DynamicHeadMovement
> *Dynamic In-Cabin Camera Movement for Euro Truck Simulator 2*

---

## Overview
Built using the logic from [ETS2HeadTracker 2.0](https://github.com/pebbian/ETS2HeadTracker-2.0), **ETS2DynamicHeadMovement** was developed as an alternative to [ETS2HeadTracker](https://github.com/pebbian/ETS2HeadTracker-1.0). It was created specifically to users who want dynamic, realistic head movement without operating external tracking hardware or software, as well as iOS users unable to run the Android client for [ETS2HeadTracker 2.0](https://github.com/pebbian/ETS2HeadTracker-2.0). 

This plugin ran directly into the game's native Telemetry SDK to extract camera movement data and apply realistic head-motion simulation.

---

## Features
* Simulates micro-movements and gentle cockpit shaking from the perspective of a real driver seated behind the wheel.
* Automatically glances at the left mirror, right mirror, and instrument cluster at randomized intervals (more like once every x seconds).
* Rotates camera view left and right based on the steering wheel input.
* Automatically checks the corresponding side mirror for a few seconds whenever blinkers are on.

---

## Installation
1. Place `ETS2DynamicHeadMovement.dll` into the `plugins` folder inside your ETS2 game installation directory (`bin/win_x64/plugins`).
2. Edit controls.sii inside your ETS2 profile folder (located in Documents) and change constant c_ht_roll 0.000000 to constant c_ht_roll 1.000000.
3. Launch ETS2 and enable head tracking.
4. **IMPORTANT:** In game options, **DO NOT ENABLE** *Steering Camera Rotation* and *Blinker Camera Rotation* (disable them to prevent input conflicts).
5. Interior head movement will be overridden by the plugin's logic.
6. Open the ui by pressing **Insert**.

---

## Requirements
* Requires [OpenTrack](https://github.com/opentrack/opentrack) installed on the user PC.
* Navigate to OpenTrack **Output** settings:
  * Select **facetrack 2.0 Enhanced** as the Output.
  * Set the dropdown mode next to Output settings to **Both**.
 
Watch the review and leave a suggestion here: https://www.youtube.com/watch?v=aASSKkr_wAI&t=3s

Enjoying the project? You can support me here: https://ko-fi.com/pebbian. Thanks for having my back!
