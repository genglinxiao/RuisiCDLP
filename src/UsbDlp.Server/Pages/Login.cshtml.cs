using System.Security.Claims;
using System.Security.Cryptography;
using System.Text;
using Microsoft.AspNetCore.Authentication;
using Microsoft.AspNetCore.Authentication.Cookies;
using Microsoft.AspNetCore.Mvc;
using Microsoft.AspNetCore.Mvc.RazorPages;

namespace UsbDlp.Server.Pages;

public sealed class LoginModel(IConfiguration configuration) : PageModel
{
    [BindProperty] public string Username { get; set; } = string.Empty;
    [BindProperty] public string Password { get; set; } = string.Empty;
    public string? Error { get; private set; }

    public async Task<IActionResult> OnPostAsync()
    {
        string expectedUser = configuration["UsbDlp:BootstrapAdminUsername"] ?? string.Empty;
        string expectedPassword = configuration["UsbDlp:BootstrapAdminPassword"] ?? string.Empty;
        if (expectedPassword.Length < 14 || !FixedEquals(Username, expectedUser) || !FixedEquals(Password, expectedPassword))
        {
            Error = "Invalid credentials or bootstrap administrator is not configured.";
            return Page();
        }
        ClaimsPrincipal principal = new(new ClaimsIdentity([new Claim(ClaimTypes.Name, expectedUser), new Claim(ClaimTypes.Role, "admin")], CookieAuthenticationDefaults.AuthenticationScheme));
        await HttpContext.SignInAsync(CookieAuthenticationDefaults.AuthenticationScheme, principal);
        return RedirectToPage("/Index");
    }

    private static bool FixedEquals(string actual, string expected) => CryptographicOperations.FixedTimeEquals(Encoding.UTF8.GetBytes(actual), Encoding.UTF8.GetBytes(expected));
}
