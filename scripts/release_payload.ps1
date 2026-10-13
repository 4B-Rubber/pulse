# Shared production executables for portable, signing and symbol packaging.
$PulseReleaseExecutables = @('pulse.exe', 'Pulse.Index.exe', 'Pulse.Document.exe',
    'Pulse.Preview.exe', 'pulse_shell.exe', 'pulse_integration.exe', 'pulse_elevated.exe')
$PulseReleaseRuntimeDlls = @('lumatext.dll', 'pdfium.dll')
$PulseReleasePayload = $PulseReleaseExecutables + $PulseReleaseRuntimeDlls
