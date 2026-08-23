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

    public async Task OnGetAsync(string? tab, CancellationToken cancellationToken)
    {
        ActiveTab = tab is "policies" or "workstations" or "requests" or "events" or "settings" ? tab : "dashboard";
        Data = await _repository.DashboardAsync(cancellationToken);
    }
}
