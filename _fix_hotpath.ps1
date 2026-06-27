$path = "C:\Users\Administrator\Desktop\ProjectBunny\src\DirectX12\DX12State.cpp"
$content = [System.IO.File]::ReadAllText($path, [System.Text.Encoding]::UTF8)

$old = "void DX12HotPathUpdate()`n{`n	const bool needsHeavyTracking =`n		DX12FrameAnalysisIsActive() ||`n		DX12FrameAnalysisIsCapturing() ||`n		DX12FrameAnalysisIsCaptureRequested() ||`n		DX12ShaderDumpIsCapturingFrame() ||`n		DX12ShaderDumpIsCaptureRequested() ||`n		DX12ShaderDumpIsBusy();`n`n	const bool needsRecordWork =`n		needsHeavyTracking ||`n		DX12HuntIsEnabled() ||`n		DX12ModHasActiveShaderOverrides() ||`n		DX12ModNeedsPresentReplacement() ||`n		DX12ModNeedsPreSkinningUavProbe();"

$new = "void DX12HotPathUpdate()`n{`n	// Fast path: if already skipping all and no activity is present, avoid InterlockedExchange storm`n	if (gDX12HotPathSkipAll &&`n		!DX12FrameAnalysisIsActive() &&`n		!DX12FrameAnalysisIsCapturing() &&`n		!DX12FrameAnalysisIsCaptureRequested() &&`n		!DX12ShaderDumpIsCapturingFrame() &&`n		!DX12ShaderDumpIsCaptureRequested() &&`n		!DX12ShaderDumpIsBusy() &&`n		!DX12HuntIsEnabled() &&`n		!DX12ModHasActiveShaderOverrides() &&`n		!DX12ModNeedsPresentReplacement() &&`n		!DX12ModNeedsPreSkinningUavProbe())`n		return;`n`n	const bool needsHeavyTracking =`n		DX12FrameAnalysisIsActive() ||`n		DX12FrameAnalysisIsCapturing() ||`n		DX12FrameAnalysisIsCaptureRequested() ||`n		DX12ShaderDumpIsCapturingFrame() ||`n		DX12ShaderDumpIsCaptureRequested() ||`n		DX12ShaderDumpIsBusy();`n`n	const bool needsRecordWork =`n		needsHeavyTracking ||`n		DX12HuntIsEnabled() ||`n		DX12ModHasActiveShaderOverrides() ||`n		DX12ModNeedsPresentReplacement() ||`n		DX12ModNeedsPreSkinningUavProbe();"

if ($content.Contains($old)) {
    $content = $content.Replace($old, $new)
    [System.IO.File]::WriteAllText($path, $content, [System.Text.Encoding]::UTF8)
    Write-Host "SUCCESS: Replacement applied"
} else {
    Write-Host "FAIL: Pattern not found"
    # Debug: show what's around the function
    $idx = $content.IndexOf("void DX12HotPathUpdate")
    if ($idx -ge 0) {
        Write-Host "Found at index $idx"
        Write-Host "---BEGIN---"
        Write-Host $content.Substring($idx, 300)
        Write-Host "---END---"
    }
}
