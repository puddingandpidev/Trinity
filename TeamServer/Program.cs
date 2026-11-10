using System.Text;
using FluentValidation;
using Microsoft.AspNetCore.Authentication.JwtBearer;
using Microsoft.AspNetCore.Mvc.Controllers;
using Microsoft.EntityFrameworkCore;
using Microsoft.IdentityModel.Tokens;
using TeamServer.Configurations;
using TeamServer.Data;
using TeamServer.ExceptionHandlers;
using TeamServer.Handlers.CheckinHandler;
using TeamServer.Handlers.CheckinHandler.Strategies;
using TeamServer.Interface;
using TeamServer.NotificationDispatchers;
using TeamServer.Repositories;
using TeamServer.Services;
using TeamServer.Services.Factories;
using TeamServer.Validators;
using TeamServer.Validators.Commands;
using TeamServer.Validators.Listeners.Http;
using TeamServer.Validators.Listeners.Smb;
using Trinity.Shared.Configurations;
using Trinity.Shared.DTOs.Command;
using Trinity.Shared.DTOs.Listener.Http;
using Trinity.Shared.DTOs.Listener.Smb;
using Trinity.Shared.Enums.Tasks;
using Trinity.Shared.Interfaces;

namespace TeamServer
{
    public class Program
    {
        public static void Main(string[] args)
        {
            var builder = WebApplication.CreateBuilder(args);

            // Services 
            builder.Services.AddScoped<IListenerService, ListenerService>();
            builder.Services.AddScoped<ListenerService>();
            builder.Services.AddScoped<ICommandService, CommandService>();
            builder.Services.AddScoped<CommandService>();
            builder.Services.AddScoped<IPayloadService, PayloadService>();
            builder.Services.AddScoped<PayloadService>();
            builder.Services.AddScoped<IPayloadBuilder, CMakePayloadBuilder>();
            builder.Services.AddScoped<IAgentService, AgentService>();
            builder.Services.AddScoped<AgentService>();
            builder.Services.AddScoped<TaskService>();
            builder.Services.AddScoped<ITaskService, TaskService>();
            builder.Services.AddScoped<IOperatorService, OperatorService>();
            builder.Services.AddScoped<ICampaignService, CampaignService>();
            builder.Services.AddScoped<CampaignService>();
            builder.Services.AddScoped<IAdminService, AdminService>();
            builder.Services.AddScoped<IFileStorageService, FileStorageService>();
            builder.Services.AddScoped<DatabaseService>();
            builder.Services.AddScoped<IFileTransferService, FileTransferService>();
            builder.Services.AddScoped<IReceivedFileService, ReceivedFileService>();
            builder.Services.AddScoped<IKeyloggerService, KeyloggerService>();
            builder.Services.AddScoped<ToolRepository>();
            builder.Services.AddScoped<ToolService>();
            builder.Services.AddScoped<IAuthService, AuthService>();

            // Handlers 
            builder.Services.AddScoped<ICheckinResponseHandler, CheckinResponseHandler>();
            builder.Services.AddScoped<SuccessStrategy>();
            builder.Services.AddScoped<RunningStrategy>();
            builder.Services.AddScoped<ErrorStrategy>();

            // Validators
            builder.Services.AddValidatorsFromAssemblyContaining<OperatorValidator>();

            /// Singletons
            builder.Services.AddSingleton<ListenerFactory>();
            builder.Services.AddSingleton<HttpModuleFactory>();
            builder.Services.AddSingleton<CommandFactory>();
            builder.Services.AddSingleton<AgentFactory>();
            builder.Services.AddSingleton<TaskResultFactory>();
            builder.Services.AddSingleton<HttpListenerManager>();
            builder.Services.AddSingleton<IServerNetworkSettings, ServerNetworkSettings>();
            builder.Services.AddSingleton<IToolFactory, ToolFactory>();
            builder.Services.AddSingleton<IHostFactory, HostFactory>();
            builder.Services.AddSingleton<ITaskRequestFactory, TaskRequestFactory>();

            // JWT bearer authentication for the operator/admin endpoints used by the Client web app.
            // Ref: https://learn.microsoft.com/en-us/aspnet/core/security/authentication/configure-jwt-bearer-authentication
            builder.Services.Configure<JwtSettings>(builder.Configuration.GetSection(JwtSettings.SectionName));
            builder.Services.Configure<AuthSettings>(builder.Configuration.GetSection(AuthSettings.SectionName));
            var jwtSettings = builder.Configuration.GetSection(JwtSettings.SectionName).Get<JwtSettings>() ?? new JwtSettings();

            builder
                .Services.AddAuthentication(JwtBearerDefaults.AuthenticationScheme)
                .AddJwtBearer(options =>
                {
                    options.TokenValidationParameters = new TokenValidationParameters
                    {
                        ValidateIssuer = true,
                        ValidIssuer = jwtSettings.Issuer,
                        ValidateAudience = true,
                        ValidAudience = jwtSettings.Audience,
                        ValidateIssuerSigningKey = true,
                        IssuerSigningKey = new SymmetricSecurityKey(Encoding.UTF8.GetBytes(jwtSettings.Key)),
                        ValidateLifetime = true,
                        ClockSkew = TimeSpan.FromSeconds(30),
                        NameClaimType = System.Security.Claims.ClaimTypes.Name,
                        RoleClaimType = System.Security.Claims.ClaimTypes.Role,
                    };
                });

            builder.Services.AddAuthorization();

            builder.Services.AddControllers(options =>
            {
                options.Filters.Add<ValidationFilter>();
            });

            builder.Services.AddSwaggerGen(options =>
            {
                options.EnableAnnotations();

                // Name operations after the controller action instead of the path. Without this the
                // document carries no operationId at all, so openapi-generator invents
                // `apiV1CommandsAgentIDSpawnShellPost`-style names that change whenever a route
                // moves. Action names are stable and readable (`QueueShellCommand`); the one
                // duplicate action name in the tree is renamed in KeyloggerController.
                options.CustomOperationIds(apiDesc =>
                    apiDesc.ActionDescriptor is ControllerActionDescriptor descriptor
                        ? descriptor.ActionName
                        : null);
            });

            builder.Services.AddDbContextPool<Context>(opt => opt.UseNpgsql(
                builder.Configuration.GetConnectionString("Postgres"),
                o => o.MapEnum<Status>("status")
                      .MapEnum<TaskType>("taskType")
                      .MapEnum<TaskCode>("taskCode")));

            // Repositories
            builder.Services.AddScoped<ListenerRepository>();
            builder.Services.AddScoped<ProtocolRespository>();
            builder.Services.AddScoped<TaskStatusRepository>();
            builder.Services.AddScoped<CommandRepository>();
            builder.Services.AddScoped<AgentRepository>();
            builder.Services.AddScoped<TaskRepository>();
            builder.Services.AddScoped<TaskResultRepository>();
            builder.Services.AddScoped<PayloadRepository>();
            builder.Services.AddScoped<CampaignRepository>();
            builder.Services.AddScoped<OperatorRepository>();
            builder.Services.AddScoped<CampaignBridgeRepository>();
            builder.Services.AddScoped<AdminRepository>();
            builder.Services.AddScoped<HostRepository>();

            // Validators 
            builder.Services.AddScoped<IValidator<CreateHttpListenerRequest>, CreateHttpListenerRequestValidator>();
            builder.Services.AddScoped<IValidator<CreateSmbRequest>, CreateSmbRequestValidator>();
            builder.Services.AddScoped<IValidator<BofDTO>, BofDTOValidator>();
            builder.Services.AddScoped<IValidator<CancelDownloadDTO>, CancelDownloadDTOValidator>();
            builder.Services.AddScoped<IValidator<DotNetAsmDTO>, DotNetAsmDTOValidator>();
            builder.Services.AddScoped<IValidator<DownloadDTO>, DownloadDTOValidator>();
            builder.Services.AddScoped<IValidator<EscalateDTO>, EscalateDTOValidator>();
            builder.Services.AddScoped<IValidator<KillProcessDTO>, KillProcessDTOValidator>();
            builder.Services.AddScoped<IValidator<PowerShellDTO>, PowerShellDTOValidator>();
            builder.Services.AddScoped<IValidator<RunAsDTO>, RunAsDTOValidator>();
            builder.Services.AddScoped<IValidator<RunDTO>, RunDTOValidator>();
            builder.Services.AddScoped<IValidator<RunAsDTO>, RunAsDTOValidator>();
            builder.Services.AddScoped<IValidator<RunUDTO>, RunUDTOValidator>();
            builder.Services.AddScoped<IValidator<SetSleepDTO>, SetSleepDTOValidator>();
            builder.Services.AddScoped<IValidator<UpdateHostsDTO>, UpdateHostsDTOValidator>();
            builder.Services.AddScoped<IValidator<ShellDTO>, ShellDTOValidator>();
            builder.Services.AddScoped<IValidator<SpawnToDTO>, SpawnToDTOValidator>();
            builder.Services.AddScoped<IValidator<UploadDTO>, UploadDTOValidator>();
            builder.Services.AddScoped<IValidator<WinTaskDTO>, WinTaskDTOValidator>();
            builder.Services.AddScoped<IValidator<KeyloggerDTO>, KeyloggerDTOValidator>();

            // SignalR
            builder.Services.AddSignalR();

            // Server-side sender for the hub's groups. The reference registered the Hub itself,
            // which cannot work (SignalR creates hub instances per connection, so an injected one
            // has no Clients); NotificationDispatcher wraps IHubContext instead.
            builder.Services.AddSingleton<INotificationHub, NotificationDispatcher>();
            // Exception handler
            builder.Services.AddSingleton<GlobalExceptionHandler>();
            builder.Services.AddExceptionHandler<GlobalExceptionHandler>();
            builder.Services.AddProblemDetails();

            // Server configuration 
            ServerNetworkSettings serverNetworkSettings = new ServerNetworkSettings()
                .ResolveIpV4Address()
                .ResolveIpV6Address();
            builder.Services.AddSingleton(serverNetworkSettings);

            var app = builder.Build();

            using (var scope = app.Services.CreateScope())
            {
                var context = scope.ServiceProvider.GetRequiredService<Context>();

                if (builder.Configuration.GetValue<bool>("ApplyMigrations"))
                {
                    context.Database.Migrate();
                }

                StatusCacheService.Initialise(context);
                BootstrapOperator(context, builder.Configuration);

                // Listener modules are in memory; without this a restart leaves every stored
                // listener unbound even though the UI still lists it.
                scope.ServiceProvider
                    .GetRequiredService<IListenerService>()
                    .RestoreListenersAsync()
                    .GetAwaiter()
                    .GetResult();
            }

            // Configure the HTTP request pipeline.
            if (app.Environment.IsDevelopment())
            {
                app.UseSwagger();
                app.UseSwaggerUI(options =>
                {
                    options.SwaggerEndpoint("/swagger/v1/swagger.json", "v1");
                    options.RoutePrefix = string.Empty;
                });
            }

            app.MapHub<NotificationHub>("/register");
            app.UseExceptionHandler();
            if (app.Configuration.GetValue<bool>("EnableHttpsRedirection"))
            {
                app.UseHttpsRedirection();
            }
            app.UseAuthentication();
            app.UseAuthorization();
            app.MapGet("/health", () => Results.Ok(new { status = "ok" }));
            app.MapControllers();
            app.Run();
        }
        //^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^//
        /// <summary>
        /// Seeds the first operator account so the JWT login endpoint is reachable on a fresh
        /// database (operator management itself now requires authentication).
        /// Credentials come from the "Bootstrap" configuration section and can be overridden with
        /// Bootstrap__OperatorUsername / Bootstrap__OperatorPassword environment variables.
        /// </summary>
        private static void BootstrapOperator(Context context, IConfiguration configuration)
        {
            if (context.Operators.Any())
            {
                return;
            }

            var username = configuration.GetValue<string>("Bootstrap:OperatorUsername") ?? "operator";
            var password = configuration.GetValue<string>("Bootstrap:OperatorPassword") ?? "trinity-operator";

            context.Operators.Add(new Trinity.Shared.Models.Operator { Username = username, Password = password });
            context.SaveChanges();
        }

    }
}