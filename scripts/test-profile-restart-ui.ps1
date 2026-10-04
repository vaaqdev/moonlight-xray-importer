param([int]$AppPid, [string]$ProfilePath, [ValidateSet('ImportRestart','ImportLater','RestartPending','ImportInvalid')][string]$Action='ImportRestart')
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes
Add-Type -TypeDefinition @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class ImportNativeDialog {
  public delegate bool Callback(IntPtr window, IntPtr data);
  [DllImport("user32.dll")] static extern bool EnumChildWindows(IntPtr parent, Callback callback, IntPtr data);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern int GetClassName(IntPtr window, StringBuilder name, int size);
  [DllImport("user32.dll")] static extern IntPtr GetParent(IntPtr window);
  [DllImport("user32.dll")] static extern int GetDlgCtrlID(IntPtr window);
  [DllImport("user32.dll")] static extern IntPtr GetDlgItem(IntPtr window, int id);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern IntPtr SendMessage(IntPtr window, uint message, IntPtr wparam, string text);
  [DllImport("user32.dll", EntryPoint="SendMessageW")] static extern IntPtr SendClick(IntPtr window, uint message, IntPtr wparam, IntPtr lparam);
  public static bool Choose(IntPtr dialog, string path) {
    IntPtr edit=IntPtr.Zero;
    EnumChildWindows(dialog, (window,data)=> {
      var name=new StringBuilder(256); GetClassName(window,name,256);
      if(name.ToString()=="Edit") {
        for(var parent=GetParent(window); parent!=IntPtr.Zero && parent!=dialog; parent=GetParent(parent)) {
          if(GetDlgCtrlID(parent)==1148) { edit=window; break; }
        }
      }
      return true;
    },IntPtr.Zero);
    var open=GetDlgItem(dialog,1);
    if(edit==IntPtr.Zero || open==IntPtr.Zero) return false;
    SendMessage(edit,0x000C,IntPtr.Zero,path);
    SendClick(open,0x00F5,IntPtr.Zero,IntPtr.Zero);
    return true;
  }
}
'@
$app = Get-Process -Id $AppPid
$window = [System.Windows.Automation.AutomationElement]::FromHandle($app.MainWindowHandle)
$tree = [System.Windows.Automation.TreeScope]::Descendants
function Find-Element([string]$name) {
    $condition = New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::NameProperty, $name)
    return $window.FindFirst($tree, $condition)
}
function Invoke-Element([string]$name) {
    $element = Find-Element $name
    if (-not $element) { throw "Missing UI element: $name" }
    $element.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke()
}
if (-not (Find-Element 'Import JSON profile...')) {
    $buttonType = New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::ControlTypeProperty, [System.Windows.Automation.ControlType]::Button)
    $settings = $null
    foreach ($button in $window.FindAll($tree, $buttonType)) {
        if ($button.Current.Name -like 'Settings*') { $settings=$button; break }
    }
    if (-not $settings) { throw 'Settings button missing' }
    $settings.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke()
    Start-Sleep -Seconds 2
}
if ($Action -eq 'RestartPending') {
    Invoke-Element 'Restart Moonlight'
} else {
    Invoke-Element 'Import JSON profile...'
    Start-Sleep -Seconds 2
    $desktop = [System.Windows.Automation.AutomationElement]::RootElement
    $pidCondition = New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::ProcessIdProperty, $AppPid)
    $dialog=$null
    foreach ($element in $desktop.FindAll($tree, $pidCondition)) {
        if ($element.Current.ControlType -eq [System.Windows.Automation.ControlType]::Window -and $element.Current.Name -eq 'Import Xray / v2rayNG JSON profile') { $dialog=$element; break }
    }
    if (-not $dialog -or -not [ImportNativeDialog]::Choose([IntPtr]$dialog.Current.NativeWindowHandle, $ProfilePath)) {
        throw 'Cannot select JSON in native file dialog'
    }
}
Start-Sleep -Seconds 3
if ($Action -eq 'ImportInvalid') {
    if (Find-Element 'Restart now') { throw 'Invalid profile offered restart' }
    if (-not (Find-Element 'Profile import failed')) { throw 'Missing profile validation error' }
    'PASS: invalid JSON displays an error without offering restart'
    exit 0
}
$restart = Find-Element 'Restart now'
if (-not $restart -or $restart.Current.IsOffscreen) { throw 'Restart prompt is not visible' }
if (-not (Find-Element 'Your JSON profile was validated and saved. You must restart Moonlight to activate it. Restart now?')) {
    throw 'Missing explicit restart-required message'
}
'PASS: visible restart-required prompt after profile import'
if ($Action -eq 'ImportLater') {
    Invoke-Element 'Later'
    Start-Sleep -Milliseconds 500
    if (-not (Find-Element 'Restart Moonlight')) { throw 'Missing persistent restart action' }
    'PASS: Later keeps the app open and exposes Restart Moonlight in Settings'
} else {
    Invoke-Element 'Restart now'
    'PASS: Restart now invoked through the real UI'
}
