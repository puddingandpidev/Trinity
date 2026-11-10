using System.IO;
using Client.Components;
using Client.Services;
using Microsoft.AspNetCore.DataProtection;
using MudBlazor.Services;
using Trinity.Shared.Interfaces;

namespace Client
{
    public class Program
    {
        /// <summary>
        /// Where the data protection key ring lives. The container sets <c>DataProtection:KeysPath</c>
        /// to /app/keys (a volume); a host-side run defaults to a directory under the content root, so
        /// <c>dotnet run</c> works outside Docker instead of dying on a path that only exists there.
        /// </summary>
        private static string KeysDirectory(IConfiguration configuration) =>
            configuration["DataProtection:KeysPath"]
            ?? Path.Combine(AppContext.BaseDirectory, "keys");

        public static void Main(string[] args)
        {
            var builder = WebApplication.CreateBuilder(args);

            // Add services to the container.
            builder.Services.AddRazorComponents()
                .AddInteractiveServerComponents();

            // Registers MudBlazor Components
            builder.Services.AddMudServices();

            // Runtime-configured TeamServer connection + typed API clients (DTO-based).
            // Every call carries the JWT issued at login (AuthorizationMessageHandler).
            builder.Services.AddScoped<TeamServerConnection>();
            builder.Services.AddScoped<AuthTokenStore>();
            // Protected session storage (sign-in and bearer token) is encrypted with the data
            // protection key ring. Persist it, otherwise every container restart invalidates the
            // browser's stored values and the operator is silently signed out.
            builder.Services.AddDataProtection()
                .PersistKeysToFileSystem(new DirectoryInfo(KeysDirectory(builder.Configuration)))
                .SetApplicationName("Trinity.Client");
            // Every authenticated call goes through AuthorizedHttp: an IHttpClientFactory message
            // handler cannot see the circuit's scoped AuthTokenStore (handlers come from the root
            // provider), which made every page load after sign-in answer 401.
            builder.Services.AddScoped<AuthorizedHttp>();

            builder.Services.AddScoped<AgentApiClient>();
            builder.Services.AddScoped<TaskApiClient>();
            builder.Services.AddScoped<AuthApiClient>();
            builder.Services.AddScoped<ListenerApiClient>();
            builder.Services.AddScoped<CommandApiClient>();
            builder.Services.AddScoped<FilesApiClient>();
            builder.Services.AddScoped<CommandRunner>();
            builder.Services.AddScoped<ServerApiClient>();

            // Named TeamServer HttpClient (fixed BaseUrl from appsettings).
            builder
                .Services.AddHttpClient("TeamServer", client =>
                {
                    client.BaseAddress = new Uri(builder.Configuration["TeamServer:BaseUrl"]!);
                });

            // App Services
            builder.Services.AddScoped<CampaignService>();
            builder.Services.AddScoped<OperatorService>();
            builder.Services.AddScoped<AdminService>();
            builder.Services.AddScoped<PayloadService>();


            var app = builder.Build(); // <- Build happens here, after all services are registered

            if (!app.Environment.IsDevelopment())
            {
                app.UseExceptionHandler("/Error");
                app.UseHsts();
            }

            app.UseStatusCodePagesWithReExecute("/not-found", createScopeForStatusCodePages: true);
            app.UseHttpsRedirection();
            app.UseAntiforgery();
            app.MapStaticAssets();
            app.MapRazorComponents<App>()
                .AddInteractiveServerRenderMode();

            app.Run();
        }
    }
}
