' Ruisi USB DLP - XP storage surface inventory (read only, no changes)
'
' Lists two things per machine:
'   1. every device node that is currently DISABLED in Device Manager
'      (ConfigManagerErrorCode 22 = disabled), with instance ID, name, service,
'      class GUID - this is the "what did we turn off" list that the agent
'      itself never prints
'   2. every storage-capable controller present, so a fleet can be sized
'      before enabling controller level enforcement:
'        PCI\CC_0805 = SD host controller, PCI\CC_0607 = CardBus/PCIe bridge,
'        PCI\CC_0C00 = IEEE 1394, USBSTOR = USB mass storage, SD\ = SD bus child
'
' Run with:  cscript //nologo list-storage-surface.vbs
' Output is pipe separated so it can be redirected to a csv and opened in Excel.

Option Explicit

Dim wmi, devices, item, rendered
Dim disabledCount, controllerCount, storageCount, totalCount

Set wmi = GetObject("winmgmts:\\.\root\cimv2")
Set devices = wmi.ExecQuery("SELECT Name,DeviceID,Service,ClassGuid,ConfigManagerErrorCode " & _
                            "FROM Win32_PnPEntity")

WScript.Echo "section|instance_id|name|service|class_guid|problem_code"

disabledCount = 0
controllerCount = 0
storageCount = 0
totalCount = 0

For Each item In devices
  Dim id, name, service, guid, problem
  id = SafeText(item.DeviceID)
  name = SafeText(item.Name)
  service = SafeText(item.Service)
  guid = SafeText(item.ClassGuid)
  problem = item.ConfigManagerErrorCode
  totalCount = totalCount + 1

  If problem = 22 Then
    WScript.Echo "disabled|" & id & "|" & name & "|" & service & "|" & guid & "|" & problem
    disabledCount = disabledCount + 1
  End If

  Dim upper
  upper = UCase(id)

  If InStr(upper, "CC_0805") > 0 Then
    WScript.Echo "controller_sd|" & id & "|" & name & "|" & service & "|" & guid & "|" & problem
    controllerCount = controllerCount + 1
  ElseIf InStr(upper, "CC_0607") > 0 Then
    WScript.Echo "controller_cardbus|" & id & "|" & name & "|" & service & "|" & guid & "|" & problem
    controllerCount = controllerCount + 1
  ElseIf InStr(upper, "CC_0C00") > 0 Then
    WScript.Echo "controller_1394|" & id & "|" & name & "|" & service & "|" & guid & "|" & problem
    controllerCount = controllerCount + 1
  ElseIf InStr(upper, "USBSTOR\") > 0 Then
    WScript.Echo "usb_storage|" & id & "|" & name & "|" & service & "|" & guid & "|" & problem
    storageCount = storageCount + 1
  ElseIf InStr(upper, "SD\") = 1 Then
    WScript.Echo "sd_child|" & id & "|" & name & "|" & service & "|" & guid & "|" & problem
    storageCount = storageCount + 1
  End If
Next

WScript.Echo "summary|total_pnp=" & totalCount & "|total_disabled=" & disabledCount & _
             "|controllers=" & controllerCount & "|storage_devices=" & storageCount
WScript.Echo "disabled_total|" & disabledCount
WScript.Echo "hint|controllers=0 means this machine has no SD/CardBus/1394 reader to disable"

Function SafeText(value)
  If IsNull(value) Or IsEmpty(value) Then
    SafeText = ""
  Else
    SafeText = Trim(CStr(value))
  End If
End Function
