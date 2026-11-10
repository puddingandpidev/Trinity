// ============================================================= { START OF FILE } =============================================================== //
using System.Diagnostics;
using System.Text.Json;
using TeamServer.Interface;
using Trinity.Shared.DTOs.Listener.Http;
using Trinity.Shared.DTOs.Payload;
using Trinity.Shared.Errors;
using Trinity.Shared.Models;
using Trinity.Shared.Results;

/// <summary>
/// CMakePayloadBuilder is responsible for building payloads using CMake. It implements the IPayloadBuilder interface and provides methods to configure the build environment, compile the source code, and copy the resulting executable to the output directory.
/// https://github.com/HavocFramework/Havoc/blob/main/payloads/Demon/CMakeLists.txt
/// </summary>
namespace TeamServer.Services
{
    public class CMakePayloadBuilder : IPayloadBuilder
    {
        private readonly IWebHostEnvironment _webHostEnvironment;
        private readonly IConfiguration _configuration;

        public CMakePayloadBuilder(
            IWebHostEnvironment webHostEnvironment,
            IConfiguration configuration
        )
        {
            _webHostEnvironment = webHostEnvironment;
            _configuration = configuration;
        }

        /// <summary>
        /// Builds a payload using CMake based on the provided PayloadCreationDTO. It configures the build environment, compiles the source code, and copies the resulting executable to the output directory.
        /// </summary>
        /// <param name="payloadCreationDTO"></param>
        /// <returns></returns>
        public async Task<Result<string>> BuildAsync(
            PayloadCreationDTO payloadCreationDTO,
            string payloadUUID,
            Listener listener
        )
        {
            try
            {
                string sourceName = GetSourceName(payloadCreationDTO.Architecture);

                if (string.IsNullOrWhiteSpace(sourceName))
                {
                    return PayloadError.GenerationFailed();
                }

                string agentPath = Path.GetFullPath(
                    Path.Combine(_webHostEnvironment.ContentRootPath, "..", sourceName)
                );

                if (!Directory.Exists(agentPath))
                {
                    return PayloadError.GenerationFailed();
                }

                string buildRoot = Path.Combine(
                    _webHostEnvironment.ContentRootPath,
                    "PayloadBuilds",
                    Guid.NewGuid().ToString()
                );

                Directory.CreateDirectory(buildRoot);

                string outputPath = Path.Combine(_webHostEnvironment.ContentRootPath, "Payloads");

                Directory.CreateDirectory(outputPath);

                string compiler = GetCompiler(payloadCreationDTO.Architecture);
                string toolchainName = GetToolchainName(payloadCreationDTO.Architecture);

                if (string.IsNullOrWhiteSpace(compiler) || string.IsNullOrWhiteSpace(toolchainName))
                {
                    return PayloadError.GenerationFailed();
                }

                var httpConfig = JsonSerializer
                    .Deserialize<HttpConfig>(listener.Config);

                if (httpConfig is null || httpConfig.Hosts is null || httpConfig.Hosts.Count == 0)
                {
                    return PayloadError.GenerationFailed();
                }

                // CMakeLists.txt consumes INPUT_HOSTS (semicolon-separated), INPUT_PAYLOAD_UUID,
                // PORT, INPUT_USER_AGENT and INPUT_REQUEST_HEADER as cache variables.
                string hostsJoined = string.Join(";", httpConfig.Hosts);

                // Fall back to the CMakeLists.txt defaults when the listener config is empty.
                string userAgent =
                    string.IsNullOrWhiteSpace(httpConfig.UserAgent)
                        ? "Trinity Agent Client/1.0"
                        : httpConfig.UserAgent;
                string requestHeader =
                    string.IsNullOrWhiteSpace(httpConfig.Header)
                        ? "Content-Type: application/json"
                        : httpConfig.Header;

                Console.WriteLine(
                    $"payload build: source={sourceName} arch={payloadCreationDTO.Architecture} "
                        + $"toolchain={toolchainName} build={buildRoot}"
                );

                // Configure the build environment and generate build files using CMake
                Result configureResult = await RunProcessAsync(
                    "cmake",
                    $"-S \"{agentPath}\" -B \"{buildRoot}\" -G Ninja -DCMAKE_BUILD_TYPE=Release "
                        + $"-DINPUT_HOSTS=\"{hostsJoined}\" -DINPUT_PAYLOAD_UUID=\"{payloadUUID}\" "
                        + $"-DPORT={httpConfig.C2Port} "
                        + $"-DINPUT_USER_AGENT=\"{userAgent}\" "
                        + $"-DINPUT_REQUEST_HEADER=\"{requestHeader}\" "
                        + $"-DCMAKE_TOOLCHAIN_FILE=\"{agentPath}/cmake/toolchains/{toolchainName}\""
                );

                if (!configureResult.IsSuccess)
                {
                    return PayloadError.GenerationFailed();
                }

                Result buildResult = await RunProcessAsync(
                    "cmake",
                    $"--build \"{buildRoot}\" --config Release"
                );

                if (!buildResult.IsSuccess)
                {
                    return PayloadError.GenerationFailed();
                }

                string executablePath = FindExecutable(buildRoot);

                if (string.IsNullOrWhiteSpace(executablePath))
                {
                    return PayloadError.GenerationFailed();
                }

                string outputFileName =
                    Path.GetFileNameWithoutExtension(payloadCreationDTO.Name) + ".exe";

                string finalOutputPath = Path.Combine(outputPath, outputFileName);

                File.Copy(executablePath, finalOutputPath, true);

                return finalOutputPath;
            }
            catch (Exception e)
            {
                Console.WriteLine(e);
                return PayloadError.GenerationFailed();
            }
        }

        /// <summary>
        /// Determines the appropriate compiler based on the specified architecture. It retrieves the compiler path from the configuration settings and returns it. If the architecture is not recognized, an empty string is returned.
        /// </summary>
        /// <param name="architecture"></param>
        /// <returns></returns>
        private string GetCompiler(string architecture)
        {
            return architecture.ToLowerInvariant() switch
            {
                "x64" => _configuration["PayloadBuilder:Compilers:x64"] ?? "x86_64-w64-mingw32-g++",
                "x86" => _configuration["PayloadBuilder:Compilers:x86"] ?? "i686-w64-mingw32-g++",
                "x32" => _configuration["PayloadBuilder:Compilers:x86"] ?? "i686-w64-mingw32-g++",
                _ => string.Empty,
            };
        }

        /// <summary>
        /// Resolves which agent tree a payload is built from. Defaults to <c>Agent</c> (the port);
        /// <c>PayloadBuilder:Sources:&lt;arch&gt;</c> selects another source tree that sits beside it,
        /// currently <c>Agent.Legacy</c> — the upstream agent at <c>89b6440</c>, which ships its own
        /// <c>cmake/toolchains/mingw-w64-clang.cmake</c> and cross-compiles in the same image.
        /// Only a single path segment is accepted: the value names a directory, never a path.
        /// </summary>
        private string GetSourceName(string architecture)
        {
            string? configured = architecture.ToLowerInvariant() switch
            {
                "x64" => _configuration["PayloadBuilder:Sources:x64"],
                "x86" or "x32" => _configuration["PayloadBuilder:Sources:x86"],
                _ => null,
            };

            if (string.IsNullOrWhiteSpace(configured))
            {
                return "Agent";
            }

            return configured.IndexOfAny(new[] { '/', '\\', ':' }) >= 0 ? string.Empty : configured;
        }

        /// <summary>
        /// Resolves the CMake toolchain file for a CPU architecture. The toolchain (not the
        /// compiler name) decides the actual compiler+linker pair, so it is configurable per
        /// architecture through <c>PayloadBuilder:Toolchains:&lt;arch&gt;</c>. Clang/LLD is the
        /// default: it emits a real PDB for payload debugging and a structurally different PE.
        /// Set the value to <c>mingw-w64-x86_64.cmake</c> to build with GCC + GNU ld instead.
        /// </summary>
        private string GetToolchainName(string architecture)
        {
            string? configured = architecture.ToLowerInvariant() switch
            {
                "x64" => _configuration["PayloadBuilder:Toolchains:x64"],
                "x86" or "x32" => _configuration["PayloadBuilder:Toolchains:x86"],
                _ => null,
            };

            if (!string.IsNullOrWhiteSpace(configured))
            {
                return configured;
            }

            return architecture.ToLowerInvariant() switch
            {
                "x64" => "mingw-w64-clang.cmake",
                "x86" or "x32" => "mingw-w64-i686.cmake", // TODO: Make toolchain
                _ => string.Empty,
            };
        }

        /// <summary>
        /// Runs a process asynchronously with the specified file name and arguments. It captures the standard output and error streams, waits for the process to exit, and returns a Result indicating success or failure based on the exit code.
        /// </summary>
        /// <param name="fileName"></param>
        /// <param name="arguments"></param>
        /// <returns></returns>
        private async Task<Result> RunProcessAsync(string fileName, string arguments)
        {
            var processStartInfo = new ProcessStartInfo
            {
                FileName = fileName,
                Arguments = arguments,
                WorkingDirectory = _webHostEnvironment.ContentRootPath,
                RedirectStandardOutput = true,
                RedirectStandardError = true,
                UseShellExecute = false,
                CreateNoWindow = true,
            };

            using (var process = new Process { StartInfo = processStartInfo })
            {
                process.Start();

                var stdout = await process.StandardOutput.ReadToEndAsync();
                var stderr = await process.StandardError.ReadToEndAsync();

                await process.WaitForExitAsync();

                return process.ExitCode == 0 ? Result.Success() : PayloadError.GenerationFailed();
            }
        }

        /// <summary>
        /// Resolves the built payload executable. The Agent target always produces
        /// <c>&lt;buildDirectory&gt;/Agent.exe</c>, which is asked for by name first: a debugging build may add
        /// helper executables (AgentSupervisor.exe, -DAGENT_SUPERVISOR=ON), and a directory scan would
        /// otherwise be free to ship one of those as the payload.
        /// </summary>
        /// <param name="buildDirectory"></param>
        /// <returns></returns>
        private string FindExecutable(string buildDirectory)
        {
            string expected = Path.Combine(buildDirectory, "Agent.exe");
            if (File.Exists(expected))
            {
                return expected;
            }

            string[] executables = Directory.GetFiles(
                buildDirectory,
                "*.exe",
                SearchOption.AllDirectories
            );

            return executables.FirstOrDefault() ?? string.Empty;
        }
    }
}
// ============================================================= { END OF FILE } =============================================================== //
