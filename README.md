# ValheimFsr4AA

FSR 4 Native AA for Valheim through OptiScaler. The game renders with Vulkan; OptiScaler runs FSR on a separate DX12 device. Input and output are 2560×1440, with frame generation disabled.

[Download 0.2.1 for Windows x64](https://github.com/Ainquisition/ValheimFsr4AA/releases/download/v0.2.1/ValheimFsr4AA-0.2.1-win-x64.zip) · [Install and build](BUILD.md)

Requires BepInEx. Launch with `Launch-adapter.cmd`, press F9 to toggle AA, and select FSR through OptiScaler. Installation scripts preserve backups and refuse unknown DLLs.

Tested on Windows 10 with an RX 9070 XT, Valheim 1.0.20 and BepInEx 5.4.23.5. GPU tests confirm FSR4 4.1.1 Native AA at 1.00×. The owner also tested camera movement, teleporting and alt-tab.

Experimental: fixed 1440p, synchronous GPU waits and a known output-alpha issue. See [verification notes](BUILD.md#verify). No FPS gain is promised.

Adapter code: MIT. Dependency licenses and exact OptiScaler source revisions: [DEPENDENCY-SOURCE.md](DEPENDENCY-SOURCE.md).
