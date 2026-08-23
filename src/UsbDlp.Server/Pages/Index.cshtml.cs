using Microsoft.AspNetCore.Mvc.RazorPages;
using UsbDlp.Server.Data;

namespace UsbDlp.Server.Pages;

public sealed class IndexModel : PageModel
{
    private readonly UsbDlpRepository _repository;

    public IndexModel(UsbDlpRepository repository)
    {
        _repository = repository;
    }

    public DashboardModel Data { get; private set; } = null!;
    public string ActiveTab { get; private set; } = "dashboard";
    public PagedResult<DeviceInventoryRow>? Devices { get; private set; }
    public PagedResult<WorkstationInventoryRow>? Workstations { get; private set; }
    public DeviceDetail? DeviceDetail { get; private set; }
    public WorkstationDetail? WorkstationDetail { get; private set; }
    public string Query { get; private set; } = string.Empty;

    public async Task OnGetAsync(string? tab, string? q, int? page, Guid? deviceId, Guid? workstationId, CancellationToken cancellationToken)
    {
        ActiveTab = tab is "devices" or "policies" or "workstations" or "requests" or "events" or "settings" ? tab : "dashboard";
        Query = q?.Trim() ?? string.Empty;
        Data = await _repository.DashboardAsync(cancellationToken);
        if (ActiveTab == "devices")
        {
            DeviceDetail = deviceId is null ? null : await _repository.DeviceDetailAsync(deviceId.Value, cancellationToken);
            Devices = await _repository.SearchDevicesAsync(Query, Math.Max(page ?? 1, 1), 25, "lastSeen", cancellationToken);
        }
        else if (ActiveTab == "workstations")
        {
            WorkstationDetail = workstationId is null ? null : await _repository.WorkstationDetailAsync(workstationId.Value, cancellationToken);
            Workstations = await _repository.SearchWorkstationsAsync(Query, Math.Max(page ?? 1, 1), 25, "lastSeen", cancellationToken);
        }
    }
}
