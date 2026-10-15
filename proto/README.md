# proto/

`Preset.proto` und `ProductionAutomation.proto` sind die von der Community
aus Cortex Control rekonstruierten Schemas (Paket `cortex_protobuf_v2`,
`BinaryPreset` ohne Paket). Quelle: pyquadcortex `protocol/proto/`
(stokes-audio, MIT, Lizenztext: `LICENSE.pyquadcortex`); byte-identisch mit
pyquadcortex 94e5053 (geprueft 2026-10-15 mit `cmp`), inhaltlich identisch
mit CortexUSB `src/Protos/`.

Die C++-Bindings werden zur Build-Zeit von `protoc` (3.21) erzeugt
(`build/<preset>/*.pb.{h,cc}`) und nicht eingecheckt. Das Protokoll ist
unversioniert: nach einem CorOS-Update zuerst `qcctl --verbose version`
und `qcctl current` pruefen.
