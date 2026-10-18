# Offline asset audit: resolve every reference the game makes, against the romfs you have,
# BEFORE spending a device round. This is the script that closed the "missing assets"
# question: it reproduces the host's three-tier resolution (exact path -> strip "data/" ->
# bare-name lookup through a basename index) over the whole tree, so a path problem shows up
# here as a count instead of as a green screen two hours later.
#
#   asset_audit.ps1 -Root <romfs dir> [<more dirs> ...]
#
# All output is ASCII: Add-Type reads script files as ANSI, so non-ASCII literals in the C#
# body below break compilation in a way that looks like a syntax error.
param(
  [Parameter(Mandatory = $true)][string[]]$Root
)
$ErrorActionPreference = 'Stop'

$set = New-Object 'System.Collections.Generic.HashSet[string]' ([System.StringComparer]::OrdinalIgnoreCase)
$byName = @{}
$zero = New-Object 'System.Collections.Generic.List[string]'
$nfiles = 0
foreach ($root in $Root) {
  Get-ChildItem -LiteralPath $root -Recurse -File -ErrorAction SilentlyContinue | ForEach-Object {
    $rel = $_.FullName.Substring($root.Length + 1).Replace('\', '/')
    $nfiles++
    [void]$set.Add($rel)
    $n = [System.IO.Path]::GetFileName($rel)
    if (-not $byName.ContainsKey($n)) { $byName[$n] = New-Object 'System.Collections.Generic.List[string]' }
    $byName[$n].Add($rel)
    if ($_.Length -eq 0) { $zero.Add("$root :: $rel") }
  }
}
Write-Output "files=$nfiles zeroByte=$($zero.Count)"
$zero | Select-Object -First 40 | ForEach-Object { "  ZERO  $_" }

# Collect references out of the data files. Two shapes: an attribute that names a file, and
# any quoted string that ends in an asset extension.
$refs = @{}
$scanned = 0; $luaReq = 0
foreach ($root in $Root) {
  Get-ChildItem -LiteralPath $root -Recurse -File -Include '*.xml', '*.json', '*.lua' -ErrorAction SilentlyContinue | ForEach-Object {
    $scanned++
    $txt = ''
    try { $txt = [System.IO.File]::ReadAllText($_.FullName) } catch { return }
    if ($_.Extension -ieq '.lua') {
      foreach ($m in [regex]::Matches($txt, 'require\s*\(?\s*"')) { $luaReq++ }
      return
    }
    $dir = $_.FullName.Substring($root.Length + 1).Replace('\', '/')
    $dir = if ($dir.Contains('/')) { $dir.Substring(0, $dir.LastIndexOf('/')) } else { '' }
    $glob = @{}
    foreach ($m in [regex]::Matches($txt, '(?:filename|path|texture|file|src|asset)\s*=\s*"([^"]+)"')) {
      $r = $m.Groups[1].Value
      if (-not ($dir -and $set.Contains("$dir/$r"))) { if (-not $glob.ContainsKey($r)) { $glob[$r] = 0 }; $glob[$r]++ }
    }
    foreach ($m in [regex]::Matches($txt, '["'']([A-Za-z0-9_\-./]+\.(?:tex|xml|ksh|json|zip|fev|fsb|po|sjson))["'']')) {
      $r = $m.Groups[1].Value
      if (-not ($dir -and $set.Contains("$dir/$r"))) { if (-not $glob.ContainsKey($r)) { $glob[$r] = 0 }; $glob[$r]++ }
    }
    foreach ($k in $glob.Keys) { if (-not $refs.ContainsKey($k)) { $refs[$k] = 0 }; $refs[$k] += $glob[$k] }
  }
}
Write-Output "scanned=$scanned refs=$($refs.Count) luaRequireCalls=$luaReq"

# Now resolve them the way the host does.
$ok = 0; $fixData = 0; $byNameHit = 0; $missSum = 0
$miss = @{}; $elsewhere = @{}
foreach ($r in $refs.Keys) {
  $p = $r
  if ($p.StartsWith('rom://')) { $p = $p.Substring(6) }
  if ($p.StartsWith('./')) { $p = $p.Substring(2) }
  if ($p.StartsWith('/')) { $p = $p.Substring(1) }
  if ($set.Contains($p)) { $ok++; continue }
  if ($p.StartsWith('data/')) { $p2 = $p.Substring(5); if ($set.Contains($p2)) { $fixData++; continue } }
  $n = [System.IO.Path]::GetFileName($p)
  if ($byName.ContainsKey($n)) {
    $key = "$r  =>  " + (($byName[$n] | Select-Object -First 3) -join ' | ')
    $elsewhere[$key] = $refs[$r]; $byNameHit += $refs[$r]
    continue
  }
  $miss[$r] = $refs[$r]; $missSum += $refs[$r]
}
Write-Output "== ok=$ok dataPrefixFix=$fixData nameElsewhere=$byNameHit missing=$missSum"
Write-Output "---- needs the host to strip a data/ prefix ----"
$refs.Keys | Where-Object { $_.StartsWith('data/') -and $set.Contains($_.Substring(5)) } | Sort-Object | Select-Object -First 80 | ForEach-Object { "  $_" }
Write-Output "---- resolvable by bare name only (host must fall back to the index) ----"
$elsewhere.GetEnumerator() | Sort-Object Value -Descending | Select-Object -First 40 | ForEach-Object { "{0,5}  {1}" -f $_.Value, $_.Key }
Write-Output "---- genuinely missing ----"
$miss.GetEnumerator() | Sort-Object Value -Descending | Select-Object -First 120 | ForEach-Object { "{0,5}  {1}" -f $_.Value, $_.Key }
