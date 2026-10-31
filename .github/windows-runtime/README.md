Pinned Windows D3D compiler redistributable
=========================================

`d3dcompiler_47.dll` is the x64 Microsoft D3D compiler already shipped in YG Dolphin 0.3.96 and 0.3.97. Version: 10.0.26100.7705 (WinBuild.160101.0800). Its exact bytes are required by the existing pc2 shader-cache packaging flow. This is an unchanged third-party runtime, never an Authenticode signing target.

SHA256: `a05f99734f7c4822fefc12b367af21fd0976ed6608752fb1e1e80b6ece7ecbbb`

Provenance: extracted without modification from the reviewed 0.3.96 Windows shipping ZIP, SHA256 `0e02c9b0a009c53a5d5d149c304afc63ae794428f6ee9d7b3070ed35bd9ee35f`, member `YG Dolphin/d3dcompiler_47.dll`. The Windows workflow downloads this one file from this repository at its exact workflow commit, checks the SHA256 before building, and includes it in the per-file artifact manifest. No previous engine executable or game content is reused.
