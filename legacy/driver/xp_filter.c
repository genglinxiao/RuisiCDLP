#include <ntddk.h>
#include <wdmsec.h>

#include "usb_dlp/xp_filter_protocol.h"

typedef struct _FILTER_EXTENSION {
  PDEVICE_OBJECT lower;
  PDEVICE_OBJECT pdo;
  LIST_ENTRY link;
  volatile LONG allowed;
  WCHAR physical_name[USB_DLP_FILTER_NAME_CAPACITY];
} FILTER_EXTENSION, *PFILTER_EXTENSION;

static PDEVICE_OBJECT g_control_device;
static UNICODE_STRING g_symbolic_link;
static LIST_ENTRY g_devices;
static KSPIN_LOCK g_devices_lock;

static NTSTATUS complete_irp(PIRP irp, NTSTATUS status, ULONG_PTR information) {
  irp->IoStatus.Status = status;
  irp->IoStatus.Information = information;
  IoCompleteRequest(irp, IO_NO_INCREMENT);
  return status;
}

static NTSTATUS forward_irp(PDEVICE_OBJECT device, PIRP irp) {
  PFILTER_EXTENSION extension = (PFILTER_EXTENSION)device->DeviceExtension;
  IoSkipCurrentIrpStackLocation(irp);
  return IoCallDriver(extension->lower, irp);
}

static BOOLEAN is_usb_storage(PDEVICE_OBJECT pdo) {
  WCHAR name[64];
  ULONG needed = 0;
  UNICODE_STRING actual;
  UNICODE_STRING expected;
  NTSTATUS status = IoGetDeviceProperty(
      pdo, DevicePropertyEnumeratorName, sizeof(name), name, &needed);
  if (!NT_SUCCESS(status) || needed < sizeof(WCHAR) ||
      needed % sizeof(WCHAR) != 0 ||
      needed > sizeof(name)) return FALSE;
  name[(sizeof(name) / sizeof(name[0])) - 1] = L'\0';
  RtlInitUnicodeString(&actual, name);
  RtlInitUnicodeString(&expected, L"USBSTOR");
  return RtlEqualUnicodeString(&actual, &expected, TRUE);
}

static NTSTATUS add_device(PDRIVER_OBJECT driver, PDEVICE_OBJECT pdo) {
  PDEVICE_OBJECT device = NULL;
  PFILTER_EXTENSION extension;
  ULONG needed = 0;
  KIRQL irql;
  NTSTATUS status;

  if (!is_usb_storage(pdo)) return STATUS_SUCCESS;
  status = IoCreateDevice(driver, sizeof(FILTER_EXTENSION), NULL,
                          FILE_DEVICE_DISK, FILE_DEVICE_SECURE_OPEN,
                          FALSE, &device);
  if (!NT_SUCCESS(status)) return status;
  extension = (PFILTER_EXTENSION)device->DeviceExtension;
  RtlZeroMemory(extension, sizeof(*extension));
  extension->pdo = pdo;
  status = IoGetDeviceProperty(
      pdo, DevicePropertyPhysicalDeviceObjectName,
      sizeof(extension->physical_name), extension->physical_name, &needed);
  if (!NT_SUCCESS(status) || needed < sizeof(WCHAR) ||
      needed % sizeof(WCHAR) != 0 ||
      needed > sizeof(extension->physical_name) ||
      extension->physical_name[(needed / sizeof(WCHAR)) - 1] != L'\0') {
    IoDeleteDevice(device);
    return NT_SUCCESS(status) ? STATUS_INVALID_DEVICE_STATE : status;
  }
  extension->lower = IoAttachDeviceToDeviceStack(device, pdo);
  if (extension->lower == NULL) {
    IoDeleteDevice(device);
    return STATUS_NO_SUCH_DEVICE;
  }
  device->Flags |= extension->lower->Flags & (DO_BUFFERED_IO | DO_DIRECT_IO |
                                             DO_POWER_PAGABLE | DO_POWER_INRUSH);
  device->Characteristics |= extension->lower->Characteristics &
      (FILE_REMOVABLE_MEDIA | FILE_READ_ONLY_DEVICE | FILE_FLOPPY_DISKETTE);
  KeAcquireSpinLock(&g_devices_lock, &irql);
  InsertTailList(&g_devices, &extension->link);
  KeReleaseSpinLock(&g_devices_lock, irql);
  device->Flags &= ~DO_DEVICE_INITIALIZING;
  return STATUS_SUCCESS;
}

static NTSTATUS remove_completion(PDEVICE_OBJECT device, PIRP irp, PVOID context) {
  UNREFERENCED_PARAMETER(device);
  UNREFERENCED_PARAMETER(irp);
  KeSetEvent((PKEVENT)context, IO_NO_INCREMENT, FALSE);
  return STATUS_MORE_PROCESSING_REQUIRED;
}

static NTSTATUS dispatch_pnp(PDEVICE_OBJECT device, PIRP irp) {
  PFILTER_EXTENSION extension = (PFILTER_EXTENSION)device->DeviceExtension;
  PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(irp);
  if (stack->MinorFunction == IRP_MN_REMOVE_DEVICE) {
    KIRQL irql;
    NTSTATUS status;
    KEVENT completion;
    KeAcquireSpinLock(&g_devices_lock, &irql);
    RemoveEntryList(&extension->link);
    KeReleaseSpinLock(&g_devices_lock, irql);
    KeInitializeEvent(&completion, NotificationEvent, FALSE);
    IoCopyCurrentIrpStackLocationToNext(irp);
    IoSetCompletionRoutine(irp, remove_completion, &completion, TRUE, TRUE, TRUE);
    status = IoCallDriver(extension->lower, irp);
    if (status == STATUS_PENDING) {
      KeWaitForSingleObject(&completion, Executive, KernelMode, FALSE, NULL);
    }
    status = irp->IoStatus.Status;
    IoDetachDevice(extension->lower);
    IoDeleteDevice(device);
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return status;
  }
  return forward_irp(device, irp);
}

static NTSTATUS dispatch_power(PDEVICE_OBJECT device, PIRP irp) {
  PFILTER_EXTENSION extension = (PFILTER_EXTENSION)device->DeviceExtension;
  PoStartNextPowerIrp(irp);
  IoSkipCurrentIrpStackLocation(irp);
  return PoCallDriver(extension->lower, irp);
}

static NTSTATUS dispatch_control(PDEVICE_OBJECT device, PIRP irp) {
  PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(irp);
  USB_DLP_FILTER_REQUEST* request;
  USB_DLP_FILTER_REPLY* reply;
  PLIST_ENTRY entry;
  KIRQL irql;
  ULONG found = 0;
  ULONG allowed = 0;
  ULONG count = 0;

  if (device != g_control_device) {
    PFILTER_EXTENSION extension = (PFILTER_EXTENSION)device->DeviceExtension;
    if (InterlockedCompareExchange(&extension->allowed, 0, 0) == 0)
      return complete_irp(irp, STATUS_ACCESS_DENIED, 0);
    return forward_irp(device, irp);
  }
  if (stack->Parameters.DeviceIoControl.IoControlCode !=
          IOCTL_USB_DLP_FILTER_SET_ALLOWED &&
      stack->Parameters.DeviceIoControl.IoControlCode !=
          IOCTL_USB_DLP_FILTER_QUERY)
    return complete_irp(irp, STATUS_INVALID_DEVICE_REQUEST, 0);
  if (stack->Parameters.DeviceIoControl.InputBufferLength <
          sizeof(USB_DLP_FILTER_REQUEST) ||
      stack->Parameters.DeviceIoControl.OutputBufferLength <
          sizeof(USB_DLP_FILTER_REPLY))
    return complete_irp(irp, STATUS_BUFFER_TOO_SMALL, 0);
  request = (USB_DLP_FILTER_REQUEST*)irp->AssociatedIrp.SystemBuffer;
  if (request->version != USB_DLP_FILTER_PROTOCOL_VERSION ||
      request->physical_device_name[USB_DLP_FILTER_NAME_CAPACITY - 1] != L'\0' ||
      request->physical_device_name[0] == L'\0' || request->allowed > 1)
    return complete_irp(irp, STATUS_INVALID_PARAMETER, 0);

  KeAcquireSpinLock(&g_devices_lock, &irql);
  for (entry = g_devices.Flink; entry != &g_devices; entry = entry->Flink) {
    PFILTER_EXTENSION extension = CONTAINING_RECORD(entry, FILTER_EXTENSION, link);
    ++count;
    if (RtlEqualMemory(extension->physical_name,
                       request->physical_device_name,
                       sizeof(extension->physical_name))) {
      found = 1;
      if (stack->Parameters.DeviceIoControl.IoControlCode ==
          IOCTL_USB_DLP_FILTER_SET_ALLOWED)
        InterlockedExchange(&extension->allowed, request->allowed);
      allowed = (ULONG)InterlockedCompareExchange(&extension->allowed, 0, 0);
    }
  }
  KeReleaseSpinLock(&g_devices_lock, irql);
  reply = (USB_DLP_FILTER_REPLY*)irp->AssociatedIrp.SystemBuffer;
  reply->version = USB_DLP_FILTER_PROTOCOL_VERSION;
  reply->found = found;
  reply->allowed = allowed;
  reply->attached_usb_disks = count;
  return complete_irp(irp, STATUS_SUCCESS, sizeof(*reply));
}

static NTSTATUS dispatch_regular(PDEVICE_OBJECT device, PIRP irp) {
  PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(irp);
  PFILTER_EXTENSION extension;
  if (device == g_control_device) {
    if (stack->MajorFunction == IRP_MJ_CREATE ||
        stack->MajorFunction == IRP_MJ_CLOSE ||
        stack->MajorFunction == IRP_MJ_CLEANUP)
      return complete_irp(irp, STATUS_SUCCESS, 0);
    return complete_irp(irp, STATUS_INVALID_DEVICE_REQUEST, 0);
  }
  extension = (PFILTER_EXTENSION)device->DeviceExtension;
  if (stack->MajorFunction != IRP_MJ_CLOSE &&
      stack->MajorFunction != IRP_MJ_CLEANUP &&
      stack->MajorFunction != IRP_MJ_SYSTEM_CONTROL &&
      InterlockedCompareExchange(&extension->allowed, 0, 0) == 0)
    return complete_irp(irp, STATUS_ACCESS_DENIED, 0);
  return forward_irp(device, irp);
}

static VOID unload_driver(PDRIVER_OBJECT driver) {
  UNREFERENCED_PARAMETER(driver);
  IoDeleteSymbolicLink(&g_symbolic_link);
  if (g_control_device != NULL) IoDeleteDevice(g_control_device);
}

NTSTATUS DriverEntry(PDRIVER_OBJECT driver, PUNICODE_STRING registry_path) {
  UNICODE_STRING device_name;
  UNICODE_STRING sddl;
  GUID class_guid = {0x86f7f521, 0x2440, 0x4c8d,
                     {0x8f, 0xdb, 0x86, 0xec, 0x49, 0x13, 0x28, 0x42}};
  ULONG index;
  NTSTATUS status;
  UNREFERENCED_PARAMETER(registry_path);
  InitializeListHead(&g_devices);
  KeInitializeSpinLock(&g_devices_lock);
  for (index = 0; index <= IRP_MJ_MAXIMUM_FUNCTION; ++index)
    driver->MajorFunction[index] = dispatch_regular;
  driver->MajorFunction[IRP_MJ_PNP] = dispatch_pnp;
  driver->MajorFunction[IRP_MJ_POWER] = dispatch_power;
  driver->MajorFunction[IRP_MJ_DEVICE_CONTROL] = dispatch_control;
  driver->DriverExtension->AddDevice = add_device;
  driver->DriverUnload = unload_driver;
  RtlInitUnicodeString(&device_name, L"\\Device\\RuisiUsbDlpFilter");
  RtlInitUnicodeString(&g_symbolic_link, L"\\DosDevices\\RuisiUsbDlpFilter");
  RtlInitUnicodeString(&sddl, L"D:P(A;;GA;;;SY)(A;;GA;;;BA)");
  status = IoCreateDeviceSecure(driver, 0, &device_name, FILE_DEVICE_UNKNOWN,
                                FILE_DEVICE_SECURE_OPEN, FALSE, &sddl,
                                &class_guid, &g_control_device);
  if (!NT_SUCCESS(status)) return status;
  status = IoCreateSymbolicLink(&g_symbolic_link, &device_name);
  if (!NT_SUCCESS(status)) {
    IoDeleteDevice(g_control_device);
    g_control_device = NULL;
    return status;
  }
  g_control_device->Flags &= ~DO_DEVICE_INITIALIZING;
  return STATUS_SUCCESS;
}
